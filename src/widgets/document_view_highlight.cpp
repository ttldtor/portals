// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_highlight.hpp"
#include "document_view_state.hpp"

#include <algorithm>
#include <limits>

namespace portals::detail {
bool sameDocumentDecoderOptions(const DocumentDecoderOptions &left, const DocumentDecoderOptions &right) noexcept {
  return left.explicitEncoding == right.explicitEncoding && left.hint == right.hint && left.policy == right.policy &&
         left.workLimit == right.workLimit;
}

namespace {
std::optional<SemanticRole> replayRole(const DocumentHighlightScan &scan, std::uint64_t offset) noexcept {
  if (!scan.replay || offset < scan.replay->begin || offset >= scan.replay->end) {
    return std::nullopt;
  }
  const auto &replay = *scan.replay;
  const auto index = (replay.head + static_cast<std::size_t>(offset - replay.begin)) % replay.roles.size();
  const auto role = replay.roles[index];

  return role == DOCUMENT_UNCLASSIFIED_ROLE ? std::nullopt : std::optional(static_cast<SemanticRole>(role));
}

void classify(DocumentHighlightScan &scan, const TokenRun &run) {
  const auto end = run.byteOffset + run.byteLength;
  scan.classifiedEnd = std::max(scan.classifiedEnd, end);
  auto map = std::lower_bound(scan.maps.begin(), scan.maps.end(), run.byteOffset,
                              [](const DocumentTextMap &value, std::uint64_t offset) {
                                return value.sourceOffset < offset;
                              });
  for (; map != scan.maps.end() && map->sourceOffset < end; ++map) {
    map->role = run.role;
    map->classified = true;
  }
  if (scan.clusterOffset >= run.byteOffset && scan.clusterOffset < end) {
    scan.clusterRole = run.role;
    scan.clusterClassified = true;
  }
  if (scan.replay) {
    auto &replay = *scan.replay;
    auto first = std::max(run.byteOffset, replay.begin);
    const auto last = std::min(end, replay.end);
    while (first < last) {
      const auto index = (replay.head + static_cast<std::size_t>(first - replay.begin)) % replay.roles.size();
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(last - first, replay.roles.size() - index));
      std::fill_n(replay.roles.begin() + static_cast<std::ptrdiff_t>(index), count,
                  static_cast<std::uint8_t>(run.role));
      first += count;
    }
  }
}

bool lexicalFailure(DocumentHighlightScan &scan, const DocumentHighlightResult &result) {
  switch (result.status) {
  case DocumentHighlightStatus::INVALID_CONFIGURATION:
  case DocumentHighlightStatus::INVALID_INPUT:
  case DocumentHighlightStatus::UNSUPPORTED_PROFILE:
  case DocumentHighlightStatus::RESOURCE_LIMIT:
    scan.failure = result;
    scan.status = result.status == DocumentHighlightStatus::RESOURCE_LIMIT ? DocumentSyntaxState::RESOURCE_LIMIT
                                                                           : DocumentSyntaxState::UNSUPPORTED_PROFILE;
    return true;
  default:
    return false;
  }
}
} // namespace

std::expected<DocumentInputPage, DocumentError> readDocumentInput(ByteDocumentSource &source,
                                                                  DocumentReadRequest request,
                                                                  DocumentHighlightScan *highlight,
                                                                  std::stop_token stop, bool protectWindow) {
  const auto failure = [](SourceError error) {
    return std::unexpected(DocumentError{error, {}, {}});
  };
  if (stop.stop_requested()) {
    return failure(SourceError::CANCELLED);
  }
  if (!highlight || !highlight->sequential || !highlight->replay) {
    auto page = source.read(request, stop);
    if (!page) {
      return std::unexpected(page.error());
    }
    return DocumentInputPage(std::move(*page));
  }
  if (request.revision != highlight->revision) {
    return failure(SourceError::REVISION_CHANGED);
  }
  auto &replay = *highlight->replay;
  const auto capacity = replay.bytes.size();
  if (!capacity || request.offset < replay.begin || request.offset > replay.end) {
    return failure(SourceError::UNSUPPORTED);
  }
  if (request.offset == replay.end && !replay.final) {
    // A bounded history must not silently evict bytes still needed by the
    // projected window. Reject before advancing an irreversible provider.
    // Explicit forward navigation may discard the old source window while its
    // already prepared text/roles remain available for EOF clamping. Those old
    // maps must not pin a skipped, arbitrarily long logical line forever.
    const auto first = std::lower_bound(highlight->maps.begin(), highlight->maps.end(), replay.begin,
                                        [](const DocumentTextMap &point, std::uint64_t offset) {
                                          return point.sourceOffset < offset;
                                        });
    auto protectedOffset = first == highlight->maps.end() ? highlight->clusterOffset : first->sourceOffset;
    protectedOffset = protectWindow ? std::clamp(protectedOffset, replay.begin, replay.end) : replay.end;
    const auto protectedBytes = static_cast<std::size_t>(replay.end - protectedOffset);
    if (protectedBytes >= capacity) {
      return failure(SourceError::PAGE_TOO_LARGE);
    }
    request.maxBytes = std::min(request.maxBytes, capacity - protectedBytes);
    auto page = source.read(request, stop);
    if (!page) {
      return std::unexpected(page.error());
    }
    const auto input = page->bytes();
    const auto retained = static_cast<std::size_t>(replay.end - replay.begin);
    const auto dropped = input.size() > capacity - retained ? input.size() - (capacity - retained) : 0;
    replay.head = (replay.head + dropped) % capacity;
    replay.begin += dropped;
    auto offset = replay.end;
    std::size_t copied{};
    while (copied < input.size()) {
      const auto index = (replay.head + static_cast<std::size_t>(offset - replay.begin)) % capacity;
      const auto count = std::min(input.size() - copied, capacity - index);
      std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(copied), count,
                  replay.bytes.begin() + static_cast<std::ptrdiff_t>(index));
      std::fill_n(replay.roles.begin() + static_cast<std::ptrdiff_t>(index), count, DOCUMENT_UNCLASSIFIED_ROLE);
      offset += count;
      copied += count;
    }
    replay.end = offset;
    replay.final = page->finalEnd();
  }
  if (request.offset == replay.end) {
    return DocumentInputPage({}, replay.final);
  }
  const auto index = (replay.head + static_cast<std::size_t>(request.offset - replay.begin)) % capacity;
  const auto count = static_cast<std::size_t>(
    std::min<std::uint64_t>(std::min(request.maxBytes, capacity - index), replay.end - request.offset));
  return DocumentInputPage(std::span<const std::byte>(replay.bytes).subspan(index, count),
                           replay.final && request.offset + count == replay.end);
}

std::shared_ptr<DocumentHighlightScan> stageDocumentHighlight(const DocumentScan &scan,
                                                              const DocumentPrepareIntent &intent) {
  const auto language = intent.config.language;
  if (scan.highlight &&
      (scan.highlight->language == language || (language == DocumentLanguage::PLAIN && scan.highlight->sequential)) &&
      scan.highlight->revision == scan.metadata.revision &&
      sameDocumentDecoderOptions(scan.highlight->decoderOptions, intent.config.decoder)) {
    auto staged = std::make_shared<DocumentHighlightScan>(*scan.highlight);
    staged->language = language;
    if (!intent.resume) {
      if (staged->sequential) {
        staged->cachedMaps.swap(staged->maps);
      } else {
        staged->maps.clear();
      }
      staged->maps.clear();
      staged->plain.reset();
      staged->pending = false;
    }
    staged->maxMaps = std::max(intent.retainedTextMaps, static_cast<std::size_t>(intent.viewport.width) *
                                                          (static_cast<std::size_t>(intent.viewport.height) + 1));
    staged->maps.reserve(staged->maxMaps);
    if (!staged->failure && staged->status != DocumentSyntaxState::REPLAY_UNAVAILABLE) {
      staged->status =
        language == DocumentLanguage::PLAIN ? DocumentSyntaxState::DISABLED : DocumentSyntaxState::PENDING;
    }
    return staged;
  }
  auto staged = std::make_shared<DocumentHighlightScan>();
  staged->language = language;
  staged->revision = scan.metadata.revision;
  staged->decoderOptions = intent.config.decoder;
  staged->decoder = DocumentDecoder(intent.config.decoder);
  staged->sequential = !scan.metadata.capabilities.seekable;
  staged->beganAtOrigin = !staged->sequential || scan.offset == 0;
  staged->maxMaps = std::max(intent.retainedTextMaps, static_cast<std::size_t>(intent.viewport.width) *
                                                        (static_cast<std::size_t>(intent.viewport.height) + 1));
  staged->maps.reserve(staged->maxMaps);
  if (staged->sequential && intent.replayBytes) {
    staged->replay.emplace(intent.replayBytes);
  }
  if (!staged->beganAtOrigin) {
    staged->status = DocumentSyntaxState::REPLAY_UNAVAILABLE;
  }
  return staged;
}

void feedDocumentHighlight(DocumentHighlightScan &scan, std::span<const DecodedScalar> input, bool final) {
  if (scan.language == DocumentLanguage::PLAIN || scan.failure || scan.final ||
      scan.status == DocumentSyntaxState::REPLAY_UNAVAILABLE) {
    return;
  }
  if (!scan.started) {
    if (input.empty() && !final) {
      return;
    }
    const auto initial = input.empty() ? scan.offset : input.front().byteOffset;
    scan.lexer = DocumentHighlighter({.language = scan.language}, initial);
    scan.started = true;
  }
  while (!input.empty() && input.front().byteOffset < scan.lexer.nextByteOffset()) {
    // Reflow may replay retained bytes, but admitted stream scalars are never fed twice.
    input = input.subspan(1);
  }
  if (!input.empty() && input.front().byteOffset != scan.lexer.nextByteOffset()) {
    scan.status = DocumentSyntaxState::REPLAY_UNAVAILABLE;
    return;
  }
  std::array<TokenRun, 32> output{};
  do {
    const auto result = scan.lexer.feed(input, output, final);
    input = input.subspan(result.inputConsumed);
    for (const auto &run : std::span(output).first(result.outputProduced)) {
      classify(scan, run);
    }
    if (lexicalFailure(scan, result)) {
      return;
    }
    if (result.status == DocumentHighlightStatus::DONE) {
      scan.final = true;
      return;
    }
    if (result.status == DocumentHighlightStatus::NEED_INPUT) {
      return;
    }
    if (!result.inputConsumed && !result.outputProduced) {
      scan.failure =
        DocumentHighlightResult{0, 0, DocumentHighlightStatus::RESOURCE_LIMIT, scan.lexer.nextByteOffset()};
      scan.status = DocumentSyntaxState::RESOURCE_LIMIT;
      return;
    }
  } while (true);
}

void mapDocumentCluster(DocumentHighlightScan &scan, std::uint64_t rowOffset, std::size_t renderedOffset,
                        std::uint64_t sourceOffset) {
  if (scan.maps.size() == scan.maxMaps) {
    scan.failure = DocumentHighlightResult{0, 0, DocumentHighlightStatus::RESOURCE_LIMIT, sourceOffset};
    scan.status = DocumentSyntaxState::RESOURCE_LIMIT;
    return;
  }
  DocumentTextMap point;
  point.sourceOffset = sourceOffset;
  point.renderedOffset = renderedOffset;
  point.rowOffset = rowOffset;
  if (scan.clusterClassified && scan.clusterOffset == sourceOffset) {
    point.role = scan.clusterRole;
    point.classified = true;
  } else if (const auto role = replayRole(scan, sourceOffset)) {
    point.role = *role;
    point.classified = true;
  }
  if (!point.classified && !scan.cachedMaps.empty()) {
    const auto previous = std::lower_bound(scan.cachedMaps.begin(), scan.cachedMaps.end(), sourceOffset,
                                           [](const DocumentTextMap &value, std::uint64_t offset) {
                                             return value.sourceOffset < offset;
                                           });
    if (previous != scan.cachedMaps.end() && previous->sourceOffset == sourceOffset && previous->classified) {
      point.role = previous->role;
      point.classified = true;
    }
  }
  scan.maps.push_back(point);
}

void trimDocumentHighlightMaps(DocumentHighlightScan &scan, std::uint64_t firstRetained) {
  const auto first = std::lower_bound(scan.maps.begin(), scan.maps.end(), firstRetained,
                                      [](const DocumentTextMap &value, std::uint64_t offset) {
                                        return value.sourceOffset < offset;
                                      });
  scan.maps.erase(scan.maps.begin(), first);
}

std::unique_ptr<const DocumentTextFrame> projectDocumentHighlight(const DocumentPreparedFrame &frame,
                                                                  const DocumentHighlightScan &scan, bool &ready) {
  auto result = std::make_unique<DocumentTextFrame>();
  result->rows.reserve(frame.rows.size());
  ready = !scan.failure && scan.status != DocumentSyntaxState::REPLAY_UNAVAILABLE;
  for (std::size_t row = 0; row < frame.rows.size(); ++row) {
    // Construct the nested vector in place through its throwing count constructor.
    // MSVC Debug container-proxy allocation must stay inside the typed failure path.
    result->rows.emplace_back(0);
    auto &runs = result->rows.back();
    const auto clusters = frame.rows[row].graphemes();
    auto point = std::lower_bound(scan.maps.begin(), scan.maps.end(), frame.anchors[row],
                                  [](const DocumentTextMap &value, std::uint64_t offset) {
                                    return value.rowOffset < offset;
                                  });
    const auto rowEnd = std::upper_bound(point, scan.maps.end(), frame.anchors[row],
                                         [](std::uint64_t offset, const DocumentTextMap &value) {
                                           return offset < value.rowOffset;
                                         });
    // One source cluster can expand to several escaped glyphs with one role.
    // Reserve by source mappings, not by the expanded display byte count.
    runs.reserve(static_cast<std::size_t>(rowEnd - point));
    std::int32_t column{};
    for (std::size_t index = 0; index < clusters.size(); ++index) {
      while (point != scan.maps.end() && point->rowOffset == frame.anchors[row] &&
             std::next(point) != scan.maps.end() && std::next(point)->rowOffset == point->rowOffset &&
             std::next(point)->renderedOffset <= clusters[index].byte_begin) {
        ++point;
      }
      const bool known = point != scan.maps.end() && point->rowOffset == frame.anchors[row] &&
                         point->renderedOffset <= clusters[index].byte_begin && point->classified;
      ready = ready && known;
      const auto role = known ? point->role : SemanticRole::NORMAL;
      if (role != SemanticRole::NORMAL) {
        if (!runs.empty() && runs.back().role == role && runs.back().first + runs.back().count == index) {
          ++runs.back().count;
        } else {
          runs.push_back({column, index, 1, role});
        }
      }
      column += clusters[index].cells;
    }
  }
  return result;
}

namespace {
void beginHighlight(DocumentHighlightScan &highlight, const DocumentPrepareIntent &intent) {
  const auto target = highlight.maps.empty() ? 0 : highlight.maps.front().sourceOffset;
  const DocumentLexicalCheckpoint *selected{};
  for (const auto &point : highlight.checkpoints) {
    if (point && point->offset <= target && (!selected || point->offset > selected->offset)) {
      selected = &*point;
    }
  }
  highlight.decoder = selected ? DocumentDecoder::restore(selected->decoder) : DocumentDecoder(intent.config.decoder);
  highlight.lexer =
    selected ? DocumentHighlighter(selected->lexer) : DocumentHighlighter({.language = intent.config.language});
  highlight.offset = selected ? selected->offset : 0;
  highlight.lastCheckpoint = highlight.offset;
  highlight.started = selected != nullptr;
  highlight.final = false;
  highlight.waiting = false;
  highlight.failure.reset();
  highlight.classifiedEnd = 0;
  highlight.status = DocumentSyntaxState::PENDING;
  highlight.pending = true;
}

void lexicalCheckpoint(DocumentHighlightScan &highlight) {
  if (!highlight.started || highlight.offset <= highlight.lastCheckpoint ||
      highlight.offset - highlight.lastCheckpoint < DOCUMENT_LEXICAL_CHECKPOINT_BYTES ||
      highlight.lexer.nextByteOffset() != highlight.offset) {
    return;
  }
  const auto decoder = highlight.decoder.checkpoint();
  if (!decoder) {
    return;
  }
  highlight.checkpoints[highlight.checkpointCursor].emplace(
    DocumentLexicalCheckpoint{*decoder, highlight.lexer.checkpoint(), highlight.offset});
  highlight.checkpointCursor = (highlight.checkpointCursor + 1) % DOCUMENT_LEXICAL_CHECKPOINTS;
  highlight.checkpointCount = std::min(highlight.checkpointCount + 1, DOCUMENT_LEXICAL_CHECKPOINTS);
  highlight.lastCheckpoint = highlight.offset;
}

bool frameClassified(const DocumentPreparedFrame &frame, const DocumentHighlightScan &highlight) {
  if (highlight.failure || highlight.status == DocumentSyntaxState::REPLAY_UNAVAILABLE) {
    return false;
  }
  for (std::size_t row = 0; row < frame.rows.size(); ++row) {
    if (frame.rows[row].graphemes().empty()) {
      continue;
    }
    const auto anchor = frame.anchors[row];
    auto first = std::lower_bound(highlight.maps.begin(), highlight.maps.end(), anchor,
                                  [](const DocumentTextMap &point, std::uint64_t offset) {
                                    return point.rowOffset < offset;
                                  });
    if (first == highlight.maps.end() || first->rowOffset != anchor || first->renderedOffset != 0) {
      return false;
    }
    for (; first != highlight.maps.end() && first->rowOffset == anchor; ++first) {
      if (!first->classified) {
        return false;
      }
    }
  }
  return true;
}

std::shared_ptr<const DocumentPreparedFrame> coloredFrame(ByteDocumentSource &source,
                                                          const DocumentPrepareIntent &intent,
                                                          const DocumentPreparedFrame &plain,
                                                          const DocumentHighlightScan &highlight) {
  const auto amount = documentPreparationBytes(intent.viewport, intent.config, {.frame = true});
  if (!amount) {
    throw DocumentError{amount.error(), {}, {}};
  }
  auto reserved = source.budget()->reserve({*amount, 0, 0, 0});
  if (!reserved) {
    DocumentError failure{reserved.error(), {}, {}};
    failure.budgetDemand = SourceBudgetUsage{*amount, 0, 0, 0};
    throw failure;
  }
  auto frame = std::make_shared<DocumentPreparedFrame>();
  frame->credit = std::move(*reserved);
  frame->status = plain.status;
  frame->rows = plain.rows;
  frame->gutters = plain.gutters;
  frame->gutterCells = plain.gutterCells;
  frame->anchors = plain.anchors;
  if (plain.footer) {
    frame->footer = std::make_unique<GraphemeText>(*plain.footer);
  }
  frame->viewport = plain.viewport;
  frame->finalEndVisible = plain.finalEndVisible;
  frame->visibleByteEnd = plain.visibleByteEnd;
  bool ready{};
  frame->text = projectDocumentHighlight(*frame, highlight, ready);
  if (!ready) {
    throw DocumentError{SourceError::INVALID_REPLY, {}, {}};
  }
  frame->status.language = intent.config.language;
  frame->status.highlighting = DocumentSyntaxState::READY;
  frame->status.highlightErrorOffset.reset();
  return frame;
}
} // namespace

std::shared_ptr<const DocumentPreparation> prepareDocumentHighlight(std::shared_ptr<ByteDocumentSource> source,
                                                                    const DocumentPrepareIntent &intent,
                                                                    std::shared_ptr<const DocumentScan> previous,
                                                                    std::uint64_t generation, SourceCredit credit,
                                                                    std::stop_token stop) {
  auto result = std::make_shared<DocumentPreparation>();
  result->credit = std::move(credit);
  result->generation = generation;
  result->kind = DocumentPreparationKind::HIGHLIGHT;
  result->syntax = DocumentSyntaxState::PENDING;
  result->baseFrame = previous && previous->highlight ? previous->highlight->plain : nullptr;
  try {
    if (!previous || !previous->highlight || !previous->highlight->plain ||
        intent.config.mode != DocumentViewMode::TEXT || intent.config.language == DocumentLanguage::PLAIN) {
      throw DocumentError{SourceError::INVALID_ARGUMENT, {}, {}};
    }
    auto metadata = source->inspect(stop);
    if (!metadata) {
      throw metadata.error();
    }
    if (metadata->revision != previous->metadata.revision) {
      throw DocumentError{SourceError::REVISION_CHANGED, {}, {}};
    }
    constexpr std::size_t RESULT_ENVELOPE = 16 * 1024;
    auto reserved = result->credit.split({result->credit.usage().bytes - RESULT_ENVELOPE, 0, 0, 0});
    if (!reserved) {
      throw DocumentError{reserved.error(), {}, {}};
    }
    auto reservation = std::make_shared<SourceCredit>(std::move(*reserved));
    auto scan = std::make_shared<DocumentScan>(*previous);
    scan->highlight = stageDocumentHighlight(*scan, intent);
    scan->reservation = std::move(reservation);
    auto &highlight = *scan->highlight;
    result->baseFrame = highlight.plain;
    if (!highlight.pending) {
      beginHighlight(highlight, intent);
    }
    previous.reset();
    std::size_t work{};
    bool ready = frameClassified(*highlight.plain, highlight);
    while (!ready && !highlight.failure && !highlight.final && !highlight.waiting && work < intent.config.workBytes) {
      if (stop.stop_requested()) {
        throw DocumentError{SourceError::CANCELLED, {}, {}};
      }
      const auto remaining = std::numeric_limits<std::uint64_t>::max() - highlight.offset;
      const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min(source->maxPageBytes(), intent.config.workBytes - work), remaining));
      if (!requested) {
        throw DocumentError{SourceError::POSITION_EXHAUSTED, {}, {}};
      }
      auto page =
        readDocumentInput(*source, {metadata->revision, highlight.offset, requested, generation}, &highlight, stop);
      if (!page) {
        if (page.error().code == SourceError::NO_PROGRESS ||
            (page.error().code == SourceError::WOULD_BLOCK && !page.error().budgetDemand)) {
          highlight.waiting = true;
          break;
        }
        throw page.error();
      }
      auto input = page->bytes();
      work += input.size();
      bool drain{};
      do {
        if (stop.stop_requested()) {
          throw DocumentError{SourceError::CANCELLED, {}, {}};
        }
        std::array<DecodedScalar, 64> output{};
        const auto decoded = highlight.decoder.feed(input, highlight.offset, page->finalEnd(), output);
        highlight.offset += decoded.inputConsumed;
        input = input.subspan(decoded.inputConsumed);
        drain = decoded.status == DocumentDecodeStatus::OUTPUT_FULL;
        if (decoded.status == DocumentDecodeStatus::MALFORMED ||
            decoded.status == DocumentDecodeStatus::ENCODING_UNRESOLVED ||
            decoded.status == DocumentDecodeStatus::INVALID_OFFSET ||
            decoded.status == DocumentDecodeStatus::UNSUPPORTED_POLICY) {
          throw DocumentError{SourceError::INVALID_REPLY, {}, {}};
        }
        feedDocumentHighlight(highlight, std::span(output).first(decoded.outputProduced),
                              decoded.status == DocumentDecodeStatus::DONE);
        lexicalCheckpoint(highlight);
        ready = frameClassified(*highlight.plain, highlight);
        if (ready || highlight.failure || highlight.final ||
            (!decoded.inputConsumed && !decoded.outputProduced && !drain)) {
          break;
        }
      } while (!input.empty() || drain);
    }
    if (ready) {
      result->frame = coloredFrame(*source, intent, *highlight.plain, highlight);
      result->syntax = result->frame->status.highlighting;
      highlight.ready = true;
      highlight.status = result->syntax;
      highlight.plain.reset();
    } else if (highlight.failure) {
      result->syntax = highlight.status;
      result->syntaxErrorOffset = highlight.failure->errorOffset;
      highlight.plain.reset();
    } else if (highlight.final) {
      throw DocumentError{SourceError::INVALID_REPLY, {}, {}};
    } else {
      result->continueWork = !highlight.waiting;
    }
    result->scan = std::move(scan);
  } catch (const DocumentError &error) {
    result->error = error;
  } catch (const std::bad_alloc &) {
    result->error = DocumentError{SourceError::NO_MEMORY, {}, {}};
  } catch (...) {
    result->error = DocumentError{SourceError::CALLBACK_FAILED, {}, {}};
  }
  return result;
}
} // namespace portals::detail
