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

bool sameProjection(const DocumentHighlightAttempt &attempt, const DocumentPrepareIntent &intent) noexcept {
  return attempt.viewport.width == intent.viewport.width && attempt.viewport.height == intent.viewport.height &&
         attempt.width == intent.width && attempt.anchor == intent.anchor && attempt.horizontal == intent.horizontal &&
         attempt.rowDelta == intent.rowDelta && attempt.wrap == intent.config.wrap &&
         attempt.lineNumbers == intent.config.lineNumbers && attempt.tail == intent.tail;
}

std::uint64_t tailPreviewOriginalByteCutoff(const DocumentScan &scan, const DocumentPrepareIntent &intent) noexcept {
  if (!intent.tail || !intent.config.wrap || !scan.metadata.capabilities.seekable || !scan.metadata.byteExtent ||
      intent.viewport.height <= 0) {
    return 0;
  }

  constexpr auto MAX = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t MAX_SCALAR_SOURCE_BYTES = 4;
  constexpr auto MAX_SEPARATOR_SOURCE_BYTES = 2 * MAX_SCALAR_SOURCE_BYTES;
  auto bytes = MAX_SCALAR_SOURCE_BYTES * DOCUMENT_CLUSTER_BYTES;
  const auto include = [&](std::uint64_t count, std::uint64_t unit) {
    bytes = count > (MAX - bytes) / unit ? MAX : bytes + count * unit;
  };

  // Decoder units, including malformed replacements, consume at most four
  // source bytes. Every non-separator scalar contributes at least one UTF-8
  // payload byte; escapes and continuation marks only increase that payload.
  include(intent.config.maxFrameBytes, MAX_SCALAR_SOURCE_BYTES);
  // CRLF consumes at most eight source bytes and is the only payload-free row
  // seam. A spare row and cluster cover the fragment's initial boundary.
  include(static_cast<std::uint64_t>(intent.viewport.height) + 1, MAX_SEPARATOR_SOURCE_BYTES);
  const auto extent = *scan.metadata.byteExtent;

  return extent > bytes ? extent - bytes : 0;
}

void stageAttempt(DocumentHighlightScan &scan, const DocumentPrepareIntent &intent) {
  const bool sameWindow = scan.attempt && sameProjection(*scan.attempt, intent);
  const bool sameAttempt = sameWindow && scan.attempt->policy == intent.config.syntax &&
                           scan.attempt->budget == intent.config.maxAutoHighlightBytes;

  if (!sameAttempt) {
    scan.attempt = DocumentHighlightAttempt{intent.viewport,
                                            intent.width,
                                            intent.anchor,
                                            intent.horizontal,
                                            intent.rowDelta,
                                            intent.config.syntax,
                                            intent.config.maxAutoHighlightBytes,
                                            intent.config.maxAutoHighlightBytes,
                                            intent.config.wrap,
                                            intent.config.lineNumbers,
                                            intent.tail};
  }

  if (intent.suppressPreview) {
    scan.preview.reset();
    scan.previewLayout = false;
    return;
  }

  scan.previewLayout = !scan.sequential && intent.config.syntax != DocumentSyntaxPolicy::EXACT;
  if (scan.previewLayout) {
    try {
      scan.preview = scan.preview ? std::make_shared<DocumentHighlightPreview>(*scan.preview)
                                  : std::make_shared<DocumentHighlightPreview>();
      if (!intent.resume) {
        scan.preview->maps.clear();
        scan.preview->started = false;
        scan.preview->failed = false;
        scan.preview->clusterClassified = false;
        scan.preview->lastRun.reset();
      }
      scan.preview->maps.reserve(scan.maxMaps);
    } catch (const std::bad_alloc &) {
      scan.preview.reset();
      scan.previewLayout = false;
    }
  }
}

void classifyPreview(DocumentHighlightScan &scan, TokenRun run) {
  auto &preview = *scan.preview;
  constexpr auto MAX = std::numeric_limits<std::uint64_t>::max();
  if (run.byteOffset > MAX - preview.fragmentBase || run.byteLength > MAX - run.byteOffset - preview.fragmentBase) {
    preview.failed = true;
    return;
  }

  run.byteOffset += preview.fragmentBase;
  const auto end = run.byteOffset + run.byteLength;
  auto point = std::lower_bound(preview.maps.begin(), preview.maps.end(), run.byteOffset,
                                [](const DocumentTextMap &map, std::uint64_t offset) {
                                  return map.sourceOffset < offset;
                                });

  for (; point != preview.maps.end() && point->sourceOffset < end; ++point) {
    point->role = run.role;
    point->classified = true;
  }
  if (scan.clusterOffset >= run.byteOffset && scan.clusterOffset < end) {
    preview.clusterOffset = scan.clusterOffset;
    preview.clusterRole = run.role;
    preview.clusterClassified = true;
  }
  preview.lastRun = run;
}
} // namespace

std::expected<DocumentInputPage, DocumentError>
readDocumentInput(ByteDocumentSource &source, DocumentReadRequest request, DocumentHighlightScan *highlight,
                  std::stop_token stop, bool protectWindow, DocumentReadBatch *batch) {
  const auto failure = [](SourceError error) {
    return std::unexpected(DocumentError{error, {}, {}});
  };
  if (stop.stop_requested()) {
    return failure(SourceError::CANCELLED);
  }
  if (!highlight || !highlight->sequential || !highlight->replay) {
    auto page = batch ? batch->read(request, stop) : source.read(request, stop);
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
      // Keep the previous bounded viewport's source-qualified roles during
      // navigation, for seekable sources as well as forward-only replay.
      staged->cachedMaps.swap(staged->maps);
      staged->maps.clear();
      staged->plain.reset();
      staged->pending = false;
    }
    staged->maxMaps = std::max(intent.retainedTextMaps, static_cast<std::size_t>(intent.viewport.width) *
                                                          (static_cast<std::size_t>(intent.viewport.height) + 1));
    staged->maps.reserve(staged->maxMaps);
    if (intent.searchReflow) {
      // Coloring a selected range must not replay the lexer. Exact cached maps
      // and immutable preview maps still describe this unchanged visible window.
      staged->previewLayout = false;
    } else {
      stageAttempt(*staged, intent);
    }
    staged->previewOriginalByteCutoff =
      staged->previewLayout && staged->preview ? tailPreviewOriginalByteCutoff(scan, intent) : 0;
    if (!intent.searchReflow && !staged->failure && staged->status != DocumentSyntaxState::REPLAY_UNAVAILABLE) {
      staged->status =
        language == DocumentLanguage::PLAIN ? DocumentSyntaxState::DISABLED : DocumentSyntaxState::PENDING;
    }
    if (!staged->preview && !staged->sequential && intent.config.syntax == DocumentSyntaxPolicy::PREVIEW &&
        language != DocumentLanguage::PLAIN) {
      staged->status = DocumentSyntaxState::RESOURCE_LIMIT;
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
  stageAttempt(*staged, intent);
  staged->previewOriginalByteCutoff =
    staged->previewLayout && staged->preview ? tailPreviewOriginalByteCutoff(scan, intent) : 0;
  if (!staged->preview && !staged->sequential && intent.config.syntax == DocumentSyntaxPolicy::PREVIEW &&
      language != DocumentLanguage::PLAIN) {
    staged->status = DocumentSyntaxState::RESOURCE_LIMIT;
  }
  return staged;
}

void feedDocumentPreview(DocumentHighlightScan &scan, std::span<const DecodedScalar> input, bool final) {
  if (!scan.previewLayout || !scan.preview || scan.preview->failed) {
    return;
  }
  auto &preview = *scan.preview;

  for (const auto &scalar : input) {
    if (!preview.started || preview.final || scalar.byteOffset != preview.nextOffset) {
      // Layout shortcuts can discard a span. Close the old synthetic fragment
      // and start a new local fragment, never a qualified real-source origin.
      if (preview.started && !preview.final) {
        feedDocumentPreview(scan, {}, true);
      }
      if (preview.failed) {
        return;
      }
      preview.lexer = DocumentHighlighter({.language = scan.language});
      preview.fragmentBase = scalar.byteOffset;
      preview.started = true;
      preview.final = false;
      preview.lastRun.reset();
    }

    auto local = scalar;
    local.byteOffset -= preview.fragmentBase;
    auto pending = std::span(&local, 1);
    std::array<TokenRun, 32> output{};

    do {
      const auto result = preview.lexer.feed(pending, output);
      pending = pending.subspan(result.inputConsumed);
      for (const auto &run : std::span(output).first(result.outputProduced)) {
        classifyPreview(scan, run);
      }
      if (result.status == DocumentHighlightStatus::NEED_INPUT) {
        break;
      }
      if (result.status != DocumentHighlightStatus::OUTPUT_FULL &&
          result.status != DocumentHighlightStatus::WORK_LIMIT) {
        preview.failed = true;
        return;
      }
    } while (true);
    preview.nextOffset = scalar.byteOffset + scalar.byteLength;
  }

  if (final && preview.started && !preview.final && !preview.failed) {
    std::array<TokenRun, 32> output{};
    do {
      const auto result = preview.lexer.feed({}, output, true);
      for (const auto &run : std::span(output).first(result.outputProduced)) {
        classifyPreview(scan, run);
      }
      if (result.status == DocumentHighlightStatus::DONE) {
        preview.final = true;
        return;
      }
      if (result.status != DocumentHighlightStatus::OUTPUT_FULL &&
          result.status != DocumentHighlightStatus::WORK_LIMIT) {
        preview.failed = true;
        return;
      }
    } while (true);
  }
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
  if (sourceOffset < scan.previewOriginalByteCutoff) {
    return;
  }
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
  if (scan.previewLayout && scan.preview && scan.preview->maps.size() < scan.maxMaps) {
    auto provisional = point;
    provisional.role = SemanticRole::NORMAL;
    provisional.classified = false;
    if (scan.preview->clusterClassified && scan.preview->clusterOffset == sourceOffset) {
      provisional.role = scan.preview->clusterRole;
      provisional.classified = true;
    } else if (scan.preview->lastRun) {
      const auto &run = *scan.preview->lastRun;
      if (sourceOffset >= run.byteOffset && sourceOffset - run.byteOffset < run.byteLength) {
        provisional.role = run.role;
        provisional.classified = true;
      }
    }
    scan.preview->maps.push_back(provisional);
  }
}

void trimDocumentHighlightMaps(DocumentHighlightScan &scan, std::uint64_t firstRetained) {
  const auto first = std::lower_bound(scan.maps.begin(), scan.maps.end(), firstRetained,
                                      [](const DocumentTextMap &value, std::uint64_t offset) {
                                        return value.sourceOffset < offset;
                                      });
  scan.maps.erase(scan.maps.begin(), first);
  if (scan.previewLayout && scan.preview) {
    auto &maps = scan.preview->maps;
    const auto retained =
      std::lower_bound(maps.begin(), maps.end(), firstRetained, [](const DocumentTextMap &value, std::uint64_t offset) {
        return value.sourceOffset < offset;
      });
    maps.erase(maps.begin(), retained);
  }
}

std::unique_ptr<const DocumentTextFrame> projectDocumentHighlight(const DocumentPreparedFrame &frame,
                                                                  const DocumentHighlightScan &scan, bool &ready,
                                                                  bool preview) {
  const auto &maps = preview && scan.preview ? scan.preview->maps : scan.maps;
  auto result = std::make_unique<DocumentTextFrame>();
  result->rows.reserve(frame.rows.size());
  ready = preview ? scan.preview && !scan.preview->failed
                  : !scan.failure && scan.status != DocumentSyntaxState::REPLAY_UNAVAILABLE;
  for (std::size_t row = 0; row < frame.rows.size(); ++row) {
    // Construct the nested vector in place through its throwing count constructor.
    // MSVC Debug container-proxy allocation must stay inside the typed failure path.
    result->rows.emplace_back(0);
    auto &runs = result->rows.back();
    const auto clusters = frame.rows[row].graphemes();
    auto point = std::lower_bound(maps.begin(), maps.end(), frame.anchors[row],
                                  [](const DocumentTextMap &value, std::uint64_t offset) {
                                    return value.rowOffset < offset;
                                  });
    auto sourcePoint = std::lower_bound(scan.maps.begin(), scan.maps.end(), frame.anchors[row],
                                        [](const DocumentTextMap &value, std::uint64_t offset) {
                                          return value.rowOffset < offset;
                                        });
    const auto rowEnd =
      std::upper_bound(point, maps.end(), frame.anchors[row], [](std::uint64_t offset, const DocumentTextMap &value) {
        return offset < value.rowOffset;
      });
    // One source cluster can expand to several escaped glyphs with one role.
    // Reserve by source mappings, not by the expanded display byte count.
    runs.reserve(static_cast<std::size_t>(rowEnd - point));
    std::int32_t column{};
    const auto advance = [&](auto &selected, const auto &points, std::size_t renderedOffset) {
      while (selected != points.end() && selected->rowOffset == frame.anchors[row] &&
             std::next(selected) != points.end() && std::next(selected)->rowOffset == selected->rowOffset &&
             std::next(selected)->renderedOffset <= renderedOffset) {
        ++selected;
      }
    };

    for (std::size_t index = 0; index < clusters.size(); ++index) {
      advance(point, maps, clusters[index].byte_begin);
      if (preview) {
        advance(sourcePoint, scan.maps, clusters[index].byte_begin);
      }

      // A retained preview can outlive navigation or a policy reflow. Matching
      // display positions alone does not prove that they describe these bytes.
      const bool sameSource =
        !preview ||
        (point != maps.end() && sourcePoint != scan.maps.end() && point->sourceOffset == sourcePoint->sourceOffset &&
         point->rowOffset == sourcePoint->rowOffset && point->renderedOffset == sourcePoint->renderedOffset);
      const bool exact = preview && sourcePoint != scan.maps.end() && sourcePoint->rowOffset == frame.anchors[row] &&
                         sourcePoint->renderedOffset <= clusters[index].byte_begin && sourcePoint->classified;
      const bool known =
        exact || (point != maps.end() && point->rowOffset == frame.anchors[row] &&
                  point->renderedOffset <= clusters[index].byte_begin && point->classified && sameSource);
      ready = ready && known;
      const auto role = exact ? sourcePoint->role : known ? point->role : SemanticRole::NORMAL;
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
  const bool keepProgress = highlight.started && !highlight.final && !highlight.failure && highlight.offset <= target &&
                            highlight.lexer.nextByteOffset() == highlight.offset && highlight.decoder.checkpoint() &&
                            (!selected || highlight.offset > selected->offset);

  if (!keepProgress) {
    highlight.decoder = selected ? DocumentDecoder::restore(selected->decoder) : DocumentDecoder(intent.config.decoder);
    highlight.lexer =
      selected ? DocumentHighlighter(selected->lexer) : DocumentHighlighter({.language = intent.config.language});
    highlight.offset = selected ? selected->offset : 0;
    highlight.started = selected != nullptr;
  }
  highlight.lastCheckpoint = highlight.offset;
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
  frame->search = plain.search;
  frame->anchors = plain.anchors;
  if (plain.footer) {
    auto caption = plain.footer->bytes();
    if (caption.ends_with(" ~")) {
      caption.remove_suffix(2);
    }
    auto footer = GraphemeText::from(caption, intent.width);
    if (!footer) {
      throw DocumentError{SourceError::INVALID_REPLY, {}, {}};
    }
    frame->footer = std::make_unique<GraphemeText>(std::move(*footer));
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
  frame->status.syntax = intent.config.syntax;
  frame->status.syntaxAccuracy = DocumentSyntaxAccuracy::EXACT;
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
  if (result->baseFrame) {
    result->syntaxAccuracy = result->baseFrame->status.syntaxAccuracy;
  }
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
    stageDocumentSearch(*scan, intent);
    scan->highlight = stageDocumentHighlight(*scan, intent);
    scan->reservation = std::move(reservation);
    auto &highlight = *scan->highlight;
    result->baseFrame = highlight.plain;
    if (!highlight.pending) {
      beginHighlight(highlight, intent);
    }
    previous.reset();
    result->scan = scan;
    std::size_t work{};
    bool ready = frameClassified(*highlight.plain, highlight);
    const bool automatic = intent.config.syntax == DocumentSyntaxPolicy::AUTO;
    auto &attempt = *highlight.attempt;
    const auto required = highlight.maps.empty() ? highlight.offset : highlight.maps.back().sourceOffset;
    if (!ready && (intent.config.syntax == DocumentSyntaxPolicy::PREVIEW ||
                   (automatic && (attempt.exhausted || !attempt.remaining ||
                                  (required > highlight.offset && required - highlight.offset > attempt.remaining))))) {
      attempt.exhausted = true;
    }

    while (!ready && !attempt.exhausted && !highlight.failure && !highlight.final && !highlight.waiting &&
           work < intent.config.workBytes) {
      if (stop.stop_requested()) {
        throw DocumentError{SourceError::CANCELLED, {}, {}};
      }
      const auto remaining = std::numeric_limits<std::uint64_t>::max() - highlight.offset;
      auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min(source->maxPageBytes(), intent.config.workBytes - work), remaining));
      if (automatic) {
        requested = std::min(requested, attempt.remaining);
        if (!requested) {
          attempt.exhausted = true;
          break;
        }
      }
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
      if (automatic) {
        // Charge the admitted page, including bytes not needed once the frame
        // becomes exact. Cancellation or a short turn never refunds admission.
        attempt.remaining -= input.size();
      }
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
      if (automatic && !attempt.remaining && !ready) {
        attempt.exhausted = true;
      }
    }
    if (ready) {
      result->frame = coloredFrame(*source, intent, *highlight.plain, highlight);
      result->syntax = result->frame->status.highlighting;
      result->syntaxAccuracy = result->frame->status.syntaxAccuracy;
      highlight.ready = true;
      highlight.status = result->syntax;
      highlight.plain.reset();
      highlight.preview.reset();
    } else if (highlight.failure) {
      result->syntax = highlight.status;
      result->syntaxErrorOffset = highlight.failure->errorOffset;
      highlight.plain.reset();
    } else if (attempt.exhausted) {
      highlight.status = DocumentSyntaxState::PREVIEW;
      highlight.pending = false;
      result->syntax = highlight.status;
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
