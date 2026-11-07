// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_state.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utf8proc.h>

namespace portals::detail {
namespace {
constexpr std::uint64_t LOCAL_PRECONTEXT_BYTES = DOCUMENT_CLUSTER_BYTES;
constexpr std::uint64_t LOCAL_LOOKBEHIND_BYTES = 3;
constexpr std::size_t CHECKPOINT_DISTANCE = 64 * 1024;

struct PreparationFailure {
  DocumentError error;
};

[[noreturn]] void fail(SourceError error) {
  throw PreparationFailure{DocumentError{error, {}, {}}};
}

void appendScalar(std::string &text, char32_t scalar) {
  std::array<utf8proc_uint8_t, 4> encoded{};
  const auto count = utf8proc_encode_char(static_cast<utf8proc_int32_t>(scalar), encoded.data());
  text.append(reinterpret_cast<const char *>(encoded.data()), static_cast<std::size_t>(count));
}

std::int32_t contentWidth(const DocumentPrepareIntent &intent) {
  return std::max(1, intent.viewport.width - (intent.config.lineNumbers ? DOCUMENT_GUTTER_CELLS : 0));
}

void saveCheckpoint(DocumentScan &scan, const DocumentLayoutCheckpoint &point) {
  if (!point.decoder && point.offset != 0) {
    return;
  }

  std::uint64_t latest{};
  for (std::size_t index = 0; index < scan.checkpointCount; ++index) {
    latest = std::max(latest, scan.checkpoints[index].offset);
  }

  if (point.offset <= latest || point.offset - latest < CHECKPOINT_DISTANCE) {
    return;
  }

  scan.checkpoints[scan.checkpointCursor] = point;
  scan.checkpointCursor = (scan.checkpointCursor + 1) % DOCUMENT_CHECKPOINTS;
  scan.checkpointCount = std::min(scan.checkpointCount + 1, DOCUMENT_CHECKPOINTS);
}

void discardFirst(DocumentScan &scan) {
  scan.payload -= scan.rows.front().bytes.size();
  scan.rows.erase(scan.rows.begin());
}

bool enoughRows(const DocumentScan &scan, const DocumentPrepareIntent &intent) {
  return !intent.tail && scan.reached && !scan.precedingRows &&
         scan.rows.size() >= static_cast<std::size_t>(intent.viewport.height);
}

bool needsCrLfLookahead(const DocumentScan &scan, const DocumentPrepareIntent &intent) noexcept {
  // Resolve only the CR on the last displayed row. A subsequent CR creates
  // a hidden row and must not extend this lookahead again.
  return scan.afterCR && scan.rows.size() == static_cast<std::size_t>(intent.viewport.height);
}

void checkPayload(const DocumentScan &scan, const DocumentPrepareIntent &intent, std::size_t current = 0,
                  std::size_t incoming = 0) {
  // A full screen can leave one flushed and one pending grapheme in the
  // decoder's final drain. Charge their combined scratch once; never enlarge
  // the rendered frame limit. The fixed preparation envelope covers this.
  constexpr std::size_t LOOKAHEAD_BYTES = DOCUMENT_CLUSTER_BYTES * 2;
  auto remaining = intent.config.maxFrameBytes;
  auto scratch = !scan.precedingRows && scan.rows.size() >= static_cast<std::size_t>(intent.viewport.height)
                   ? LOOKAHEAD_BYTES
                   : std::size_t{};
  for (auto bytes : {scan.payload, current, incoming}) {
    const auto covered = std::min(bytes, remaining);
    remaining -= covered;
    bytes -= covered;
    if (bytes > scratch) {
      fail(SourceError::PAGE_TOO_LARGE);
    }
    scratch -= bytes;
  }
}

void finishRow(DocumentScan &scan, const DocumentPrepareIntent &intent, std::uint64_t end) {
  const auto height = static_cast<std::size_t>(intent.viewport.height);
  const bool forward = intent.rowDelta > 0;
  const auto preceding = forward ? height
                         : intent.rowDelta < 0
                           ? std::min<std::uint64_t>(static_cast<std::uint64_t>(-(intent.rowDelta + 1)) + 1, height)
                           : 0;
  const bool contains = scan.rowOffset <= intent.anchor && (intent.anchor < end || scan.rowOffset == intent.anchor);

  if (!scan.reached && contains && !intent.tail) {
    scan.reached = true;
    scan.anchorLine = scan.lineKnown ? std::optional(scan.line) : std::nullopt;
    while (scan.rows.size() > preceding) {
      discardFirst(scan);
    }
    scan.precedingRows = forward ? scan.rows.size() : 0;
  }

  const bool skipped = scan.reached && scan.skipRows;
  if (skipped) {
    --scan.skipRows;
  }
  if (!skipped || forward) {
    DocumentPreparedRow row;
    row.offset = scan.rowOffset;
    row.line = scan.line;
    const auto horizontal = intent.config.wrap ? 0 : intent.horizontal;
    const bool rightEdgeVisible = intent.config.wrap || scan.column <= horizontal ||
                                  scan.column - horizontal <= static_cast<std::uint64_t>(contentWidth(intent));
    row.end = rightEdgeVisible ? end : scan.rowEnd;
    row.bytes.swap(scan.row);
    scan.payload += row.bytes.size();
    scan.rows.push_back(std::move(row));
    if (skipped) {
      ++scan.precedingRows;
    }
  }

  // A forward request retains at most one screen, including predecessors.
  // At EOF this fills the last screen instead of scrolling content into air.
  if (forward && scan.reached && scan.precedingRows) {
    while (scan.rows.size() > height) {
      discardFirst(scan);
      if (scan.precedingRows) {
        --scan.precedingRows;
      }
    }
  }

  if (!scan.reached || intent.tail) {
    const auto retained = intent.tail ? height : std::max<std::size_t>(preceding, 1);
    while (scan.rows.size() > retained) {
      discardFirst(scan);
    }
  }

  checkPayload(scan, intent);

  scan.row.clear();
  scan.rowTouched = false;
  scan.column = 0;
  scan.rowOffset = end;
  scan.rowEnd = end;
}

void flushCluster(DocumentScan &scan, const DocumentPrepareIntent &intent) {
  if (!scan.clusterBytes) {
    return;
  }

  const std::string_view bytes(scan.cluster.data(), scan.clusterBytes);
  auto measured = GraphemeText::from(
    bytes, intent.width,
    {DOCUMENT_CLUSTER_BYTES, DOCUMENT_CLUSTER_BYTES, DOCUMENT_CLUSTER_BYTES, DOCUMENT_CLUSTER_BYTES * 2});
  if (!measured) {
    fail(SourceError::INVALID_REPLY);
  }

  const bool atKnownEnd = scan.metadata.byteExtent && intent.anchor >= *scan.metadata.byteExtent;

  // EOF has no containing scalar. Keep the ordinary last available row from
  // bounded precontext, including an empty final row after a separator. The
  // requested byte anchor stays unchanged, even when it lies beyond the extent.
  if (scan.localStart && !scan.reached && !atKnownEnd) {
    if (scan.clusterEnd <= intent.anchor) {
      scan.clusterBytes = 0;
      scan.clusterScalars = 0;

      return;
    }

    // Precontext establishes the ordinary containing cluster. Its preceding
    // partial line is not a displayed row, nor evidence of a global line number.
    scan.rows.clear();
    scan.payload = 0;
    scan.row.clear();
    scan.rowOffset = scan.clusterOffset;
    scan.column = 0;
    scan.rowTouched = false;
    scan.reached = true;
    scan.fragmented = scan.fragmented || scan.clusterCheckpoint.continuedCluster;
  }

  const auto cells = static_cast<std::uint64_t>(measured->measure());
  const auto width = static_cast<std::uint64_t>(contentWidth(intent));

  if (intent.config.wrap && scan.rowTouched && cells > width - std::min(scan.column, width)) {
    finishRow(scan, intent, scan.clusterOffset);
    saveCheckpoint(scan, scan.clusterCheckpoint);
  }

  // The newly touched row replaces a predecessor before consuming its
  // payload allowance, including when a grapheme started a wrapped row.
  if (intent.rowDelta > 0 && !scan.rowTouched && scan.precedingRows &&
      scan.rows.size() >= static_cast<std::size_t>(intent.viewport.height)) {
    discardFirst(scan);
    --scan.precedingRows;
  }

  const auto horizontal = intent.config.wrap ? 0 : intent.horizontal;
  const auto endColumn = scan.column + cells;
  if (endColumn < scan.column) {
    fail(SourceError::POSITION_EXHAUSTED);
  }

  if (scan.column >= horizontal && (endColumn <= horizontal + width || (intent.config.wrap && !scan.rowTouched))) {
    checkPayload(scan, intent, scan.row.size(), bytes.size());
    scan.row.append(bytes);
    scan.rowEnd = scan.clusterEnd;
  } else if (scan.column < horizontal && endColumn > horizontal) {
    // Preserve alignment when a horizontal window starts inside a wide cluster.
    scan.row.append(static_cast<std::size_t>(endColumn - horizontal), ' ');
    scan.rowEnd = scan.clusterEnd;
  }

  scan.column = endColumn;
  scan.rowTouched = true;
  scan.clusterBytes = 0;
  scan.clusterScalars = 0;
}

void addScalar(DocumentScan &scan, const DocumentPrepareIntent &intent, const DecodedScalar &decoded,
               const DocumentLayoutCheckpoint &before) {
  const auto scalar = decoded.scalar;
  constexpr char32_t FIRST_REGIONAL_INDICATOR = 0x1f1e6;
  constexpr char32_t LAST_REGIONAL_INDICATOR = 0x1f1ff;

  // A leading RI run can hide arbitrary parity beyond our bounded precontext,
  // even when each flag is short. Only leaving that run resolves this context.
  scan.regionalContextUnknown =
    scan.regionalContextUnknown && scalar >= FIRST_REGIONAL_INDICATOR && scalar <= LAST_REGIONAL_INDICATOR;

  if (scalar == U'\r' || scalar == U'\n') {
    flushCluster(scan, intent);
    const auto end = decoded.byteOffset + decoded.byteLength;

    if (!(scalar == U'\n' && scan.afterCR)) {
      finishRow(scan, intent, end);
      if (scan.line == std::numeric_limits<std::uint64_t>::max()) {
        fail(SourceError::POSITION_EXHAUSTED);
      }
      ++scan.line;
    } else {
      scan.rowOffset = end;
      scan.rowEnd = end;
      if (!scan.rows.empty() && scan.rows.back().end == decoded.byteOffset) {
        scan.rows.back().end = end;
      }
    }

    scan.afterCR = scalar == U'\r';
    scan.havePrevious = false;
    scan.graphemeState = 0;
    return;
  }

  scan.afterCR = false;
  if (!scan.localStart && !scan.reached && !intent.tail && !intent.rowDelta &&
      decoded.byteOffset + decoded.byteLength > intent.anchor) {
    scan.reached = true;
    scan.anchorLine = scan.lineKnown ? std::optional(scan.line) : std::nullopt;
    scan.rows.clear();
    scan.payload = 0;
  }
  std::string safe(0, '\0');
  if (scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f)) {
    std::array<char, 12> escaped{};
    std::snprintf(escaped.data(), escaped.size(), "\\u%04X", static_cast<unsigned>(scalar));
    safe.assign(escaped.data());
  } else {
    appendScalar(safe, scalar);
  }

  const bool boundary =
    scan.havePrevious &&
    utf8proc_grapheme_break_stateful(scan.previousScalar, static_cast<std::int32_t>(scalar), &scan.graphemeState);
  const bool forced =
    !boundary && scan.clusterBytes &&
    (scan.clusterScalars >= DOCUMENT_CLUSTER_SCALARS || safe.size() > DOCUMENT_CLUSTER_BYTES - scan.clusterBytes);

  if (boundary || forced) {
    flushCluster(scan, intent);
  }

  if (!scan.clusterBytes) {
    scan.clusterOffset = decoded.byteOffset;
    scan.clusterCheckpoint = before;
    scan.clusterCheckpoint.line = scan.line;
    scan.clusterCheckpoint.afterCR = scan.afterCR;
    scan.clusterCheckpoint.regionalContextUnknown = scan.regionalContextUnknown;
  }

  if (forced || scan.resumeContinuation || (!scan.clusterBytes && scan.regionalContextUnknown)) {
    // A visible continuation marks a bounded EGC or unproven leading RI context.
    // Original bytes and decoder positions remain untouched.
    constexpr std::string_view CONTINUATION = "\xE2\x97\x8C";
    std::copy(CONTINUATION.begin(), CONTINUATION.end(), scan.cluster.begin());
    scan.clusterBytes = CONTINUATION.size();
    scan.fragmented =
      scan.fragmented || !scan.localStart || scan.reached || decoded.byteOffset + decoded.byteLength > intent.anchor;
    scan.clusterCheckpoint.continuedCluster = true;
    if (forced || scan.resumeContinuation) {
      scan.graphemeState = 0;
    }
    scan.resumeContinuation = false;
  }

  std::copy(safe.begin(), safe.end(), scan.cluster.begin() + static_cast<std::ptrdiff_t>(scan.clusterBytes));
  scan.clusterBytes += safe.size();
  scan.clusterEnd = decoded.byteOffset + decoded.byteLength;
  ++scan.clusterScalars;
  scan.previousScalar = static_cast<std::int32_t>(scalar);
  scan.havePrevious = true;
}

std::shared_ptr<DocumentScan> initialScan(const DocumentPrepareIntent &intent, DocumentMetadata metadata,
                                          const std::shared_ptr<const DocumentScan> &previous) {
  // The caller qualifies the complete navigation intent before requesting resume.
  // Matching only anchor/horizontal would retain a different rowDelta's skip state.
  const bool resume = previous && intent.resume;
  auto scan = resume ? std::make_shared<DocumentScan>(*previous) : std::make_shared<DocumentScan>();
  if (resume) {
    scan->waiting = false;
    return scan;
  }

  scan->metadata = metadata;
  scan->decoderOptions = intent.config.decoder;
  scan->layoutWidth = contentWidth(intent);
  scan->requestedAnchor = intent.anchor;
  scan->horizontal = intent.horizontal;
  scan->wrap = intent.config.wrap;
  scan->decoder = DocumentDecoder(intent.config.decoder);
  scan->scalarCheckpoint.revision = metadata.revision;
  scan->clusterCheckpoint.revision = metadata.revision;
  scan->skipRows = intent.rowDelta > 0 ? static_cast<std::uint64_t>(intent.rowDelta) : 0;
  if (!previous && intent.seed && intent.seed->revision == metadata.revision && intent.seed->decoder &&
      intent.seed->offset <= intent.anchor) {
    scan->decoder = DocumentDecoder::restore(*intent.seed->decoder);
    scan->offset = intent.seed->offset;
    scan->rowOffset = intent.seed->offset;
    scan->line = intent.seed->line;
    scan->lineKnown = intent.seed->lineKnown;
    scan->regionalContextUnknown = intent.seed->regionalContextUnknown;
    scan->afterCR = intent.seed->afterCR;
    scan->scalarCheckpoint = *intent.seed;
    scan->resumeContinuation = intent.seed->continuedCluster;
  }
  if (previous) {
    scan->origin = previous->origin;
    scan->checkpoints = previous->checkpoints;
    scan->checkpointCount = previous->checkpointCount;
    scan->checkpointCursor = previous->checkpointCursor;
    const auto target = intent.tail ? metadata.byteExtent.value_or(intent.anchor) : intent.anchor;
    const DocumentLayoutCheckpoint *selected{};
    for (std::size_t index = 0; index < scan->checkpointCount; ++index) {
      const auto &candidate = scan->checkpoints[index];
      if (candidate.revision == metadata.revision && candidate.offset < target && candidate.decoder &&
          (!selected || candidate.offset > selected->offset)) {
        selected = &candidate;
      }
    }
    if (selected) {
      scan->decoder = DocumentDecoder::restore(*selected->decoder);
      scan->offset = selected->offset;
      scan->rowOffset = selected->offset;
      scan->line = selected->line;
      scan->lineKnown = selected->lineKnown;
      scan->regionalContextUnknown = selected->regionalContextUnknown;
      scan->afterCR = selected->afterCR;
      scan->scalarCheckpoint = *selected;
      scan->resumeContinuation = selected->continuedCluster;
    }
  }
  const auto target = std::min(intent.anchor, metadata.byteExtent.value_or(intent.anchor));
  scan->localPending = metadata.capabilities.seekable && !intent.tail && !intent.rowDelta && target > scan->offset &&
                       target - scan->offset > LOCAL_PRECONTEXT_BYTES;

  return scan;
}

bool prepareLocalStart(DocumentScan &scan, ByteDocumentSource &source, const DocumentPrepareIntent &intent,
                       std::uint64_t generation, std::size_t &work, std::stop_token stop) {
  if (!scan.localPending) {
    return true;
  }

  const auto target = std::min(intent.anchor, scan.metadata.byteExtent.value_or(intent.anchor));
  const auto restartTarget = target - LOCAL_PRECONTEXT_BYTES;
  while (scan.localPending) {
    if (stop.stop_requested()) {
      fail(SourceError::CANCELLED);
    }

    const auto base =
      scan.origin && restartTarget > LOCAL_LOOKBEHIND_BYTES ? restartTarget - LOCAL_LOOKBEHIND_BYTES : 0;
    if (scan.localProbeOffset != base) {
      scan.localProbeOffset = base;
      scan.localProbeSize = 0;
      scan.localProbeFinal = false;
    }

    if (scan.localProbeSize < scan.localProbe.size() && !scan.localProbeFinal) {
      const auto count =
        std::min({source.maxPageBytes(), scan.localProbe.size() - scan.localProbeSize, intent.config.workBytes - work});
      if (!count) {
        return false;
      }

      auto page = source.read({scan.metadata.revision, base + scan.localProbeSize, count, generation}, stop);
      if (!page) {
        if (page.error().code == SourceError::NO_PROGRESS) {
          scan.waiting = true;

          return false;
        }

        throw PreparationFailure{page.error()};
      }

      std::ranges::copy(page->bytes(), scan.localProbe.begin() + static_cast<std::ptrdiff_t>(scan.localProbeSize));
      scan.localProbeSize += page->bytes().size();
      work += page->bytes().size();
      scan.localProbeFinal = page->finalEnd();

      if (scan.localProbeSize < scan.localProbe.size() && !scan.localProbeFinal) {
        continue;
      }
    }

    const auto bytes = std::span(scan.localProbe).first(scan.localProbeSize);
    if (!scan.origin) {
      DocumentDecoder decoder(intent.config.decoder);
      std::size_t consumed{};
      do {
        std::array<DecodedScalar, 1> output{};
        // No EOF: capture a real resolved boundary before the decoder is finished.
        const auto decoded = decoder.feed(bytes.subspan(consumed), consumed, false, output);
        consumed += decoded.inputConsumed;
        scan.origin = decoder.checkpoint();
        if (decoded.status == DocumentDecodeStatus::MALFORMED ||
            decoded.status == DocumentDecodeStatus::ENCODING_UNRESOLVED ||
            decoded.status == DocumentDecodeStatus::UNSUPPORTED_POLICY) {
          fail(SourceError::INVALID_REPLY);
        }

        if (!decoded.inputConsumed && !decoded.outputProduced) {
          break;
        }
      } while (!scan.origin && consumed < bytes.size());

      if (!scan.origin) {
        // A short BOM-only source has no capturable origin. The ordinary EOF
        // path handles it without inventing a continuation checkpoint.
        scan.localPending = false;

        return true;
      }

      continue;
    }

    const auto proof = DocumentDecoder::localCheckpoint(*scan.origin, bytes, base, restartTarget, scan.localProbeFinal);
    if (proof.status != DocumentDecoderLocalStatus::READY || !proof.checkpoint) {
      fail(SourceError::INVALID_REPLY);
    }

    scan.decoder = DocumentDecoder::restore(*proof.checkpoint);
    scan.offset = proof.restartByteOffset;
    scan.rowOffset = scan.offset;
    scan.lineKnown = false;
    scan.line = 1;
    scan.afterCR = false;
    scan.scalarCheckpoint = {scan.metadata.revision, proof.checkpoint, scan.offset, 1, false, false, false};
    scan.localStart = true;
    scan.regionalContextUnknown = true;
    scan.localPending = false;
    // If the bounded precontext itself lies inside one pathological cluster,
    // retain the existing explicit continuation policy instead of hiding a split.
    scan.resumeContinuation = true;
  }

  return true;
}

std::shared_ptr<const DocumentPreparedFrame> makeFrame(std::shared_ptr<ByteDocumentSource> source,
                                                       const DocumentPrepareIntent &intent, const DocumentScan &scan) {
  const auto amount = documentPreparationBytes(intent.viewport, intent.config);
  if (!amount) {
    fail(amount.error());
  }
  const auto reserved = *amount;
  auto credit = source->budget()->reserve({reserved, 0, 0, 0});
  if (!credit) {
    DocumentError error{credit.error(), {}, {}};
    error.budgetDemand = SourceBudgetUsage{reserved, 0, 0, 0};
    throw PreparationFailure{error};
  }

  auto frame = std::make_shared<DocumentPreparedFrame>();
  frame->credit = std::move(*credit);
  frame->viewport = intent.viewport;
  frame->status.revision = scan.metadata.revision;
  frame->status.byteOffset = intent.anchor;
  frame->status.anchorBeyondExtent = scan.metadata.byteExtent && intent.anchor > *scan.metadata.byteExtent;
  frame->status.byteExtent = scan.metadata.byteExtent;
  frame->status.totalLines = scan.totalLines;
  frame->status.preparing = false;
  frame->status.waiting = scan.waiting;
  frame->status.finalEnd = scan.done;
  frame->status.fragmentedCluster = scan.fragmented;
  frame->rows.reserve(scan.rows.size() + 1);
  if (intent.config.lineNumbers) {
    frame->gutters.emplace(std::initializer_list<GraphemeText>{});
    frame->gutters->reserve(scan.rows.size() + 1);
    frame->gutterCells = DOCUMENT_GUTTER_CELLS;
  }
  frame->anchors.reserve(scan.rows.size() + 1);

  auto rows = scan.rows;
  if (!scan.done && scan.precedingRows) {
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(scan.precedingRows));
  }
  if (scan.reached && !scan.skipRows && scan.rowTouched &&
      rows.size() < static_cast<std::size_t>(intent.viewport.height)) {
    rows.push_back({scan.rowOffset, scan.line, scan.row, scan.rowEnd});
  }
  // Derive the published position from displayed rows, after excluding
  // predecessors that are still hidden while a provider is waiting.
  frame->status.visibleByteOffset = rows.empty() ? scan.rowOffset : rows.front().offset;
  frame->status.logicalLine = scan.anchorLine;
  if (scan.lineKnown && !rows.empty() && (intent.tail || intent.rowDelta || !scan.anchorLine)) {
    frame->status.logicalLine = rows.front().line;
  }
  if (intent.tail || intent.rowDelta) {
    frame->status.byteOffset = frame->status.visibleByteOffset;
  }

  // DONE has flushed decoder/cluster state; every retained text row must fit.
  // An empty logical row after the final separator adds no unseen content.
  frame->finalEndVisible =
    scan.done && !scan.waiting && !rows.empty() && rows.size() <= static_cast<std::size_t>(intent.viewport.height);

  std::size_t renderedBytes{};
  for (const auto &row :
       std::span(rows).first(std::min(rows.size(), static_cast<std::size_t>(intent.viewport.height)))) {
    std::string rendered(0, '\0');
    if (intent.config.lineNumbers) {
      std::array<char, 32> number{};
      if (!scan.lineKnown) {
        std::snprintf(number.data(), number.size(), "      ?  ");
      } else if (row.line <= 9999999) {
        std::snprintf(number.data(), number.size(), "%7llu  ", static_cast<unsigned long long>(row.line));
      } else {
        std::snprintf(number.data(), number.size(), "      +  ");
      }
      rendered.assign(number.data());
    }
    renderedBytes += rendered.size() + row.bytes.size();
    if (renderedBytes > intent.config.maxFrameBytes) {
      fail(SourceError::PAGE_TOO_LARGE);
    }
    if (frame->gutters) {
      auto gutter = GraphemeText::from(
        rendered, intent.width,
        {DOCUMENT_GUTTER_CELLS, DOCUMENT_GUTTER_CELLS, DOCUMENT_GUTTER_CELLS, DOCUMENT_GUTTER_CELLS});
      if (!gutter) {
        fail(SourceError::PAGE_TOO_LARGE);
      }
      frame->gutters->push_back(std::move(*gutter));
    }
    auto text =
      GraphemeText::from(row.bytes, intent.width,
                         {intent.config.maxFrameBytes + 32, DOCUMENT_CLUSTER_BYTES, intent.config.maxFrameBytes + 32,
                          static_cast<std::size_t>(intent.viewport.width) + 32});
    if (!text) {
      fail(SourceError::PAGE_TOO_LARGE);
    }
    frame->rows.push_back(std::move(*text));
    frame->anchors.push_back(row.offset);
    frame->visibleByteEnd = std::max(row.offset, row.end);
  }

  std::array<char, 192> footer{};
  std::array<char, 32> progress{};

  if (const auto value = documentProgress(frame->status, frame->finalEndVisible && frame->visibleByteEnd == scan.offset,
                                          frame->visibleByteEnd)) {
    std::snprintf(progress.data(), progress.size(), "%u.%u%%", static_cast<unsigned>(*value / 10),
                  static_cast<unsigned>(*value % 10));
  } else {
    std::snprintf(progress.data(), progress.size(), "progress unknown");
  }

  std::snprintf(footer.data(), footer.size(), "%s%s%s", progress.data(), scan.waiting ? " | waiting" : "",
                scan.fragmented ? " | cluster continuation" : "");
  auto caption = GraphemeText::from(footer.data(), intent.width);
  if (!caption) {
    fail(SourceError::INVALID_ARGUMENT);
  }
  frame->footer = std::make_unique<GraphemeText>(std::move(*caption));
  return frame;
}
} // namespace

std::shared_ptr<const DocumentPreparation> prepareDocument(std::shared_ptr<ByteDocumentSource> source,
                                                           const DocumentPrepareIntent &intent,
                                                           std::shared_ptr<const DocumentScan> previous,
                                                           std::uint64_t generation, SourceCredit credit,
                                                           std::stop_token stop) {
  auto result = std::make_shared<DocumentPreparation>();
  result->credit = std::move(credit);
  result->generation = generation;
  std::shared_ptr<DocumentScan> scan;

  try {
    auto metadata = source->inspect(stop);
    if (!metadata) {
      throw PreparationFailure{metadata.error()};
    }
    if (previous && (previous->metadata.revision != metadata->revision ||
                     previous->layoutWidth != contentWidth(intent) || previous->wrap != intent.config.wrap ||
                     previous->decoderOptions.explicitEncoding != intent.config.decoder.explicitEncoding ||
                     previous->decoderOptions.hint != intent.config.decoder.hint ||
                     previous->decoderOptions.policy != intent.config.decoder.policy ||
                     previous->decoderOptions.workLimit != intent.config.decoder.workLimit)) {
      previous.reset();
    }
    constexpr std::size_t RESULT_ENVELOPE = 16 * 1024;
    auto reserved = result->credit.split({result->credit.usage().bytes - RESULT_ENVELOPE, 0, 0, 0});
    if (!reserved) {
      fail(reserved.error());
    }
    auto reservation = std::make_shared<SourceCredit>(std::move(*reserved));
    scan = initialScan(intent, *metadata, previous);
    scan->reservation = std::move(reservation);
    previous.reset();
    std::size_t work{};
    bool drain{};
    const bool localReady = prepareLocalStart(*scan, *source, intent, generation, work, stop);

    while (localReady && !scan->done && !scan->waiting &&
           (!enoughRows(*scan, intent) || needsCrLfLookahead(*scan, intent)) && work < intent.config.workBytes) {
      if (stop.stop_requested()) {
        fail(SourceError::CANCELLED);
      }
      const auto remaining = std::numeric_limits<std::uint64_t>::max() - scan->offset;
      const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min(source->maxPageBytes(), intent.config.workBytes - work), remaining));
      if (!requested) {
        fail(SourceError::POSITION_EXHAUSTED);
      }
      auto page = source->read({scan->metadata.revision, scan->offset, requested, generation}, stop);
      if (!page) {
        if (page.error().code == SourceError::NO_PROGRESS) {
          // Source222 classifies an empty non-final read as NO_PROGRESS.
          // The viewport waits for explicit refresh instead of treating it as EOF.
          scan->waiting = true;
          break;
        }
        throw PreparationFailure{page.error()};
      }
      auto input = page->bytes();
      work += input.size();
      if (input.empty() && !page->finalEnd()) {
        scan->waiting = true;
        break;
      }

      do {
        const auto before = scan->scalarCheckpoint;
        std::array<DecodedScalar, 1> output{};
        const auto decoded = scan->decoder.feed(input, scan->offset, page->finalEnd(), output);
        scan->offset += decoded.inputConsumed;
        input = input.subspan(decoded.inputConsumed);
        drain = decoded.status == DocumentDecodeStatus::OUTPUT_FULL;

        if (decoded.status == DocumentDecodeStatus::MALFORMED ||
            decoded.status == DocumentDecodeStatus::ENCODING_UNRESOLVED ||
            decoded.status == DocumentDecodeStatus::INVALID_OFFSET ||
            decoded.status == DocumentDecodeStatus::UNSUPPORTED_POLICY) {
          fail(SourceError::INVALID_REPLY);
        }
        if (decoded.outputProduced) {
          addScalar(*scan, intent, output[0], before);
          if (auto checkpoint = scan->decoder.checkpoint()) {
            if (!scan->origin) {
              scan->origin = checkpoint;
            }
            scan->scalarCheckpoint = {
              scan->metadata.revision, checkpoint, scan->offset,    scan->line,
              scan->afterCR,           false,      scan->lineKnown, scan->regionalContextUnknown};
            if (output[0].scalar == U'\n' || output[0].scalar == U'\r') {
              saveCheckpoint(*scan, scan->scalarCheckpoint);
            }
          }
        }
        if (decoded.status == DocumentDecodeStatus::DONE) {
          flushCluster(*scan, intent);
          scan->totalLines = scan->lineKnown ? std::optional(scan->line) : std::nullopt;
          // A terminal separator ends the last row; it does not create a
          // new display row in a read-only viewer. Keep the empty document.
          if (scan->rowTouched || (scan->rows.empty() && scan->line == 1)) {
            finishRow(*scan, intent, scan->offset);
          }
          scan->done = true;
          break;
        }
        // A final page may leave only the decoder's bounded empty-input drain.
        // Prove DONE now instead of requiring another scroll past a final LF.
        if (enoughRows(*scan, intent) && !needsCrLfLookahead(*scan, intent) &&
            !(input.empty() && page->finalEnd() && drain)) {
          break;
        }
        if (!decoded.inputConsumed && !decoded.outputProduced && !drain) {
          break;
        }
      } while (!input.empty() || drain);
    }

    result->scan = scan;
    // Once a horizontal window is available, a very long unwrapped line need
    // not be scanned to its end merely to open the first screen. Navigation
    // that asks to skip that line continues bounded turns until it finds LF/EOF.
    const bool horizontalReady = !intent.config.wrap && scan->reached && !scan->skipRows &&
                                 scan->column >= intent.horizontal + static_cast<std::uint64_t>(contentWidth(intent));
    result->continueWork = !scan->done && !scan->waiting &&
                           (!enoughRows(*scan, intent) || needsCrLfLookahead(*scan, intent)) && !horizontalReady;
    if (!result->continueWork) {
      result->frame = makeFrame(source, intent, *scan);
    }
  } catch (const PreparationFailure &failure) {
    if (failure.error.code == SourceError::WOULD_BLOCK && !failure.error.budgetDemand) {
      if (scan) {
        scan->waiting = true;
      }
      result->scan = scan ? scan : previous;
      result->providerWaiting = true;
    } else {
      result->error = failure.error;
    }
  } catch (const std::bad_alloc &) {
    result->error = DocumentError{SourceError::NO_MEMORY, {}, {}};
  } catch (...) {
    result->error = DocumentError{SourceError::CALLBACK_FAILED, {}, {}};
  }
  return result;
}
} // namespace portals::detail
