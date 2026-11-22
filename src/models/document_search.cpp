// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_search.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <vector>

namespace portals {
namespace {
constexpr std::size_t SESSION_METADATA_BYTES = 4096;
constexpr std::size_t STRING_CAPACITY_FACTOR = 2;
constexpr std::size_t DECODE_SCALARS = 64;

DocumentSearchFailure failure(SourceError error) {
  return {{error, {}, {}}, {}};
}

void cancelled(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw failure(SourceError::CANCELLED);
  }
}

bool decodeFailure(DocumentDecodeStatus status) noexcept {
  return status == DocumentDecodeStatus::MALFORMED || status == DocumentDecodeStatus::ENCODING_UNRESOLVED ||
         status == DocumentDecodeStatus::INVALID_OFFSET || status == DocumentDecodeStatus::UNSUPPORTED_POLICY;
}

struct ScalarPoint {
  std::uint64_t first{};
  std::uint64_t last{};
  std::array<char, 4> utf8{};
  std::uint8_t size{};
  bool boundary{};
};

struct Candidate {
  std::uint64_t first{};
  std::uint64_t last{};
  std::pmr::string snippet;
  std::size_t matchOffset{};
  bool truncated{};
  bool valid{};

  explicit Candidate(std::pmr::memory_resource *memory) : snippet(0, '\0', memory) {
  }

  void assign(const Candidate &other) {
    first = other.first;
    last = other.last;
    snippet.assign(other.snippet);
    matchOffset = other.matchOffset;
    truncated = other.truncated;
    valid = other.valid;
  }
};
} // namespace

struct DocumentSearchSession::State {
  SourceCredit credit;                // Native state and all payload owners die before this reservation.
  std::shared_ptr<SourceArena> arena; // Outlives every PMR container below.
  std::shared_ptr<ByteDocumentSource> source;
  DocumentDecoderOptions decoderOptions;
  DocumentSearchOptions options;
  std::mutex mutex;
  std::pmr::string query;
  std::pmr::vector<char32_t> pattern;
  std::pmr::vector<std::size_t> prefixes;
  std::pmr::vector<ScalarPoint> points;
  Candidate pending;
  Candidate previous;
  DocumentDecoder decoder;
  std::optional<DocumentMetadata> metadata;
  std::optional<DocumentBytePage> page;
  std::array<DecodedScalar, DECODE_SCALARS> batch{};
  std::size_t batchSize{};
  std::size_t batchIndex{};
  std::size_t pageIndex{};
  std::size_t pageBytes{};
  std::size_t pointHead{};
  std::size_t pointSize{};
  std::size_t matched{};
  std::uint64_t offset{};
  std::uint64_t anchor{};
  DocumentSearchDirection direction{DocumentSearchDirection::NEXT};
  utf8proc_int32_t previousScalar{};
  utf8proc_int32_t graphemeState{};
  std::optional<DecodedScalar> queued;
  bool queuedBoundary{};
  bool haveScalar{};
  bool finalInput{};
  bool decoderDone{};
  bool ready{};
  bool finished{};

  State(SourceCredit reserved, std::shared_ptr<SourceArena> storage, std::shared_ptr<ByteDocumentSource> input,
        std::string_view literal, std::size_t scalars, DocumentDecoderOptions decoding, DocumentSearchOptions selected)
      : credit(std::move(reserved)), arena(std::move(storage)), source(std::move(input)), decoderOptions(decoding),
        options(selected), query(literal.data(), literal.size(), arena.get()), pattern(scalars, arena.get()),
        prefixes(scalars, arena.get()), points(scalars + selected.maxSnippetBytes + 1, arena.get()),
        pending(arena.get()), previous(arena.get()), decoder(decoding) {
    pending.snippet.reserve(options.maxSnippetBytes);
    previous.snippet.reserve(options.maxSnippetBytes);
    std::size_t index{};
    for (std::size_t byte = 0; byte < literal.size();) {
      utf8proc_int32_t scalar{};
      const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(literal.data() + byte),
                                          static_cast<utf8proc_ssize_t>(literal.size() - byte), &scalar);
      pattern[index++] = static_cast<char32_t>(scalar);
      byte += static_cast<std::size_t>(count);
    }
    for (std::size_t point = 1; point < pattern.size(); ++point) {
      auto prefix = prefixes[point - 1];
      while (prefix && pattern[prefix] != pattern[point]) {
        prefix = prefixes[prefix - 1];
      }
      prefixes[point] = prefix + static_cast<std::size_t>(pattern[prefix] == pattern[point]);
    }
  }

  std::size_t retainedBytes() const noexcept {
    return credit.usage().bytes + arena->capacity() + SourceArena::METADATA_BYTES;
  }

  DocumentSearchFailure qualify(DocumentSearchFailure cause) const noexcept {
    auto &error = cause.error;
    if (!error.budgetDemand && !error.budgetPeak) {
      return cause;
    }
    auto peak = error.budgetPeak ? *error.budgetPeak : *error.budgetDemand;
    constexpr auto MAX = std::numeric_limits<std::size_t>::max();
    const auto owned = retainedBytes() + pageBytes;
    if (peak.bytes > MAX - owned || (page && peak.pages == MAX)) {
      error.code = SourceError::PAGE_TOO_LARGE;
      return cause;
    }
    peak.bytes += owned;
    peak.pages += page.has_value();
    error.budgetPeak = peak;
    const auto sourcePins = source->pinnedBudgetBytes();
    const auto sourceBytes = source->envelopeBytes() + SourceBudget::ROOT_METADATA_BYTES;
    const auto cap = source->budget()->options();
    if (peak.bytes > cap.maxBytes || sourcePins > cap.maxBytes - peak.bytes ||
        sourceBytes > cap.maxBytes - peak.bytes - sourcePins || peak.pages > cap.maxPages || peak.work > cap.maxWork ||
        peak.consumers >= cap.maxConsumers) {
      error.code = SourceError::PAGE_TOO_LARGE;
    }
    return cause;
  }

  void inspect(std::stop_token stop) {
    cancelled(stop);
    auto info = source->inspect(stop);
    cancelled(stop);
    if (!info) {
      throw DocumentSearchFailure{info.error(), {}};
    }
    if (metadata && metadata->revision != info->revision) {
      throw failure(SourceError::REVISION_CHANGED);
    }
    metadata = *info;
  }

  void reset(DocumentSearchDirection selected, std::uint64_t first) noexcept {
    page.reset();
    pageBytes = pageIndex = batchIndex = batchSize = pointHead = pointSize = matched = 0;
    pending.valid = previous.valid = false;
    pending.snippet.clear();
    previous.snippet.clear();
    metadata.reset();
    decoder = DocumentDecoder(decoderOptions);
    queued.reset();
    offset = 0;
    anchor = first;
    direction = selected;
    previousScalar = graphemeState = 0;
    haveScalar = finalInput = decoderDone = ready = false;
    finished = selected == DocumentSearchDirection::PREVIOUS && first == 0;
  }

  const ScalarPoint &point(std::size_t index) const noexcept {
    return points[(pointHead + index) % points.size()];
  }

  void push(const DecodedScalar &scalar, bool boundary) {
    if (pointSize == points.size()) {
      pointHead = (pointHead + 1) % points.size();
      --pointSize;
    }
    auto &value = points[(pointHead + pointSize++) % points.size()];
    value.first = scalar.byteOffset;
    value.last = scalar.byteOffset + scalar.byteLength;
    value.boundary = boundary;
    value.size = static_cast<std::uint8_t>(utf8proc_encode_char(
      static_cast<utf8proc_int32_t>(scalar.scalar), reinterpret_cast<utf8proc_uint8_t *>(value.utf8.data())));
  }

  void capture() {
    const auto first = pointSize - pattern.size();
    pending.first = point(first).first;
    pending.last = point(pointSize - 1).last;
    pending.valid = true;
    pending.snippet.clear();
    pending.matchOffset = 0;
    pending.truncated = query.size() > options.maxSnippetBytes;
    if (pending.truncated) {
      return;
    }
    auto begin = first;
    auto bytes = query.size();
    for (auto index = first; index > 0;) {
      const auto &value = point(--index);
      if (value.size > options.maxSnippetBytes - bytes) {
        break;
      }
      bytes += value.size;
      if (value.boundary) {
        begin = index;
      }
    }
    for (auto index = begin; index < first; ++index) {
      const auto &value = point(index);
      pending.snippet.append(value.utf8.data(), value.size);
    }
    pending.matchOffset = pending.snippet.size();
    pending.snippet.append(query);
  }

  void confirm(bool boundary) {
    if (!pending.valid) {
      return;
    }
    if (!boundary) {
      pending.valid = false;
      return;
    }
    if (direction == DocumentSearchDirection::NEXT) {
      ready = true;
    } else {
      previous.assign(pending);
      pending.valid = false;
    }
  }

  void queue(const DecodedScalar &scalar) {
    const bool boundary =
      !haveScalar ||
      utf8proc_grapheme_break_stateful(previousScalar, static_cast<utf8proc_int32_t>(scalar.scalar), &graphemeState);
    queued = scalar;
    queuedBoundary = boundary;
    previousScalar = static_cast<utf8proc_int32_t>(scalar.scalar);
    haveScalar = true;
    confirm(boundary);
  }

  void processQueued() {
    const auto scalar = *queued;
    push(scalar, queuedBoundary);
    queued.reset();
    while (matched && pattern[matched] != scalar.scalar) {
      matched = prefixes[matched - 1];
    }
    if (pattern[matched] == scalar.scalar) {
      ++matched;
    }
    if (matched == pattern.size()) {
      const auto &first = point(pointSize - pattern.size());
      const bool eligible = direction == DocumentSearchDirection::NEXT ? first.first >= anchor : first.first < anchor;
      if (first.boundary && eligible) {
        capture();
      }
      matched = prefixes[matched - 1];
    }
    if (direction == DocumentSearchDirection::PREVIOUS && pointSize >= pattern.size() &&
        point(pointSize - pattern.size()).first >= anchor) {
      finishPrevious();
    }
  }

  void finishPrevious() {
    finished = true;
    if (previous.valid) {
      pending.assign(previous);
      ready = true;
    }
  }

  void finish() {
    confirm(true);
    finished = true;
    if (direction == DocumentSearchDirection::PREVIOUS) {
      finishPrevious();
    }
  }

  bool decode(std::size_t &admitted, std::size_t &processed, std::stop_token stop) {
    if (batchIndex < batchSize) {
      queue(batch[batchIndex++]);
      return true;
    }
    if (decoderDone) {
      finish();
      return true;
    }
    if (processed == options.workBytes) {
      return false;
    }
    if (!page && !finalInput) {
      if (admitted == options.workBytes) {
        return false;
      }
      const auto request =
        static_cast<std::size_t>(std::min<std::uint64_t>(std::min(source->maxPageBytes(), options.workBytes - admitted),
                                                         std::numeric_limits<std::uint64_t>::max() - offset));
      if (!request) {
        throw failure(SourceError::POSITION_EXHAUSTED);
      }
      cancelled(stop);
      auto read = source->read({metadata->revision, offset, request, 1}, stop);
      cancelled(stop);
      if (!read) {
        throw DocumentSearchFailure{read.error(), {}};
      }
      page = std::move(*read);
      pageBytes = request + SourceArena::METADATA_BYTES;
      pageIndex = 0;
      admitted += page->bytes().size();
      inspect(stop);
    }
    const auto input = page ? page->bytes().subspan(pageIndex) : std::span<const std::byte>{};
    const auto final = finalInput || (page && page->finalEnd());
    const auto limit = std::min(input.size(), options.workBytes - processed);
    // Strict decoding must not reject a valid earlier hit just because a
    // speculative batch contains a later malformed scalar beyond its boundary.
    const auto output =
      decoderOptions.policy == DocumentDecodePolicy::STRICT_MODE ? std::span(batch).first(1) : std::span(batch);
    const auto result = decoder.feed(input.first(limit), offset, final && limit == input.size(), output);
    if (decodeFailure(result.status)) {
      auto cause = failure(result.status == DocumentDecodeStatus::UNSUPPORTED_POLICY ? SourceError::UNSUPPORTED
                                                                                     : SourceError::INVALID_REPLY);
      cause.decode = result;
      throw cause;
    }
    offset += result.inputConsumed;
    processed += result.inputConsumed;
    pageIndex += result.inputConsumed;
    batchIndex = 0;
    batchSize = result.outputProduced;
    decoderDone = result.status == DocumentDecodeStatus::DONE;
    if (page && pageIndex == page->bytes().size()) {
      finalInput = page->finalEnd();
      page.reset();
      pageIndex = pageBytes = 0;
    }
    if (!result.inputConsumed && !result.outputProduced && !decoderDone) {
      throw failure(SourceError::NO_PROGRESS);
    }
    return true;
  }

  std::shared_ptr<const DocumentSearchMatch> snapshot(std::stop_token stop) {
    const auto amount = SESSION_METADATA_BYTES + sizeof(DocumentSearchMatch) +
                        STRING_CAPACITY_FACTOR * (query.size() + pending.snippet.size() + 2);
    auto reserved = source->budget()->reserve({amount, 0, 0, 0});
    if (!reserved) {
      auto cause = failure(reserved.error());
      cause.error.budgetDemand = SourceBudgetUsage{amount, 0, 0, 0};
      throw cause;
    }
    auto result = std::make_shared<DocumentSearchMatch>();
    result->credit_ = std::move(*reserved);
    result->revision = metadata->revision;
    result->byteOffset = pending.first;
    result->byteLength = pending.last - pending.first;
    result->text.assign(query);
    result->snippet.assign(pending.snippet);
    result->snippetMatchOffset = pending.matchOffset;
    result->snippetMatchLength = pending.truncated ? 0 : query.size();
    result->snippetTruncated = pending.truncated;
    try {
      inspect(stop);
    } catch (const DocumentSearchFailure &failure) {
      auto cause = failure;
      auto &error = cause.error;

      if (error.budgetDemand || error.budgetPeak) {
        auto peak = error.budgetPeak ? *error.budgetPeak : *error.budgetDemand;

        if (amount > std::numeric_limits<std::size_t>::max() - peak.bytes) {
          error.code = SourceError::PAGE_TOO_LARGE;
        } else {
          peak.bytes += amount;
          error.budgetPeak = peak;
        }
      }

      throw cause;
    }
    return result;
  }

  DocumentSearchStep advance(std::stop_token stop) {
    inspect(stop);
    std::size_t admitted{}, processed{};
    for (;;) {
      cancelled(stop);
      if (ready) {
        auto match = snapshot(stop);
        ready = pending.valid = false;
        return {DocumentSearchState::FOUND, metadata->revision, offset, metadata->byteExtent, std::move(match)};
      }
      if (finished) {
        inspect(stop);
        return {DocumentSearchState::END, metadata->revision, offset, metadata->byteExtent, {}};
      }
      if (queued) {
        processQueued();
      } else if (!decode(admitted, processed, stop)) {
        inspect(stop);
        return {DocumentSearchState::PENDING, metadata->revision, offset, metadata->byteExtent, {}};
      }
    }
  }
};

DocumentSearchSession::DocumentSearchSession(std::unique_ptr<State> state) : state_(std::move(state)) {
}

DocumentSearchSession::~DocumentSearchSession() = default;

SourceBudgetUsage DocumentSearchSession::retainedUsage() const noexcept {
  return {state_->retainedBytes() + state_->pageBytes, state_->page.has_value() ? 1u : 0u, 0, 0};
}

std::expected<std::shared_ptr<DocumentSearchSession>, DocumentSearchFailure>
DocumentSearchSession::create(std::shared_ptr<ByteDocumentSource> source, std::string_view query,
                              DocumentDecoderOptions decoderOptions, DocumentSearchOptions options) {
  constexpr auto SAFE_LIMIT = std::numeric_limits<std::size_t>::max() / 1024;
  if (!source || query.empty() || !options.maxQueryBytes || options.maxQueryBytes > SAFE_LIMIT || !options.workBytes ||
      options.workBytes > SAFE_LIMIT || !options.maxSnippetBytes || options.maxSnippetBytes > SAFE_LIMIT ||
      !decoderOptions.workLimit ||
      (decoderOptions.policy != DocumentDecodePolicy::REPLACE &&
       decoderOptions.policy != DocumentDecodePolicy::STRICT_MODE) ||
      decoderOptions.explicitEncoding < DocumentEncoding::UNRESOLVED ||
      decoderOptions.explicitEncoding > DocumentEncoding::WINDOWS1252 ||
      decoderOptions.hint < DocumentEncoding::UNRESOLVED || decoderOptions.hint > DocumentEncoding::WINDOWS1252) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }
  if (!source->capabilities().seekable) {
    return std::unexpected(failure(SourceError::UNSUPPORTED));
  }
  if (query.size() > options.maxQueryBytes) {
    return std::unexpected(failure(SourceError::PAGE_TOO_LARGE));
  }
  std::size_t scalars{};
  for (std::size_t byte = 0; byte < query.size();) {
    utf8proc_int32_t scalar{};
    const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(query.data() + byte),
                                        static_cast<utf8proc_ssize_t>(query.size() - byte), &scalar);
    if (count <= 0) {
      return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
    }
    ++scalars;
    byte += static_cast<std::size_t>(count);
  }
  const auto nativeBytes = SESSION_METADATA_BYTES + sizeof(State) + sizeof(DocumentSearchSession);
  const auto arenaBytes = SourceArena::METADATA_BYTES + SESSION_METADATA_BYTES +
                          STRING_CAPACITY_FACTOR * (query.size() + 2 * options.maxSnippetBytes + 3) +
                          scalars * (sizeof(char32_t) + sizeof(std::size_t)) +
                          (scalars + options.maxSnippetBytes + 1) * sizeof(ScalarPoint);
  auto credit = source->budget()->reserve({nativeBytes + arenaBytes, 0, 0, 0});
  if (!credit) {
    auto cause = failure(credit.error());
    cause.error.budgetDemand = SourceBudgetUsage{nativeBytes + arenaBytes, 0, 0, 0};
    cause.error.budgetPeak = cause.error.budgetDemand;
    const auto cap = source->budget()->options().maxBytes;
    const auto pinned = source->pinnedBudgetBytes() + source->envelopeBytes() + SourceBudget::ROOT_METADATA_BYTES;
    if (pinned > cap || nativeBytes + arenaBytes > cap - pinned) {
      cause.error.code = SourceError::PAGE_TOO_LARGE;
    }
    return std::unexpected(cause);
  }
  try {
    auto split = credit->split({arenaBytes, 0, 0, 0});
    if (!split) {
      return std::unexpected(failure(split.error()));
    }
    auto arena = SourceArena::create(std::move(*split));
    if (!arena) {
      return std::unexpected(failure(arena.error()));
    }
    auto state = std::make_unique<State>(std::move(*credit), std::move(*arena), std::move(source), query, scalars,
                                         decoderOptions, options);
    return std::shared_ptr<DocumentSearchSession>(new DocumentSearchSession(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}

std::expected<void, DocumentSearchFailure> DocumentSearchSession::start(DocumentSearchDirection direction,
                                                                        std::uint64_t anchorByte) {
  if (direction != DocumentSearchDirection::NEXT && direction != DocumentSearchDirection::PREVIOUS) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }
  std::unique_lock lock(state_->mutex);
  state_->reset(direction, anchorByte);
  return {};
}

std::expected<DocumentSearchStep, DocumentSearchFailure> DocumentSearchSession::advance(std::stop_token stop) {
  try {
    cancelled(stop);
    std::unique_lock lock(state_->mutex);
    try {
      return state_->advance(stop);
    } catch (const DocumentSearchFailure &cause) {
      return std::unexpected(state_->qualify(cause));
    }
  } catch (const DocumentSearchFailure &cause) {
    return std::unexpected(cause);
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}
} // namespace portals
