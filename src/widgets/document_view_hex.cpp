// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_state.hpp"
#include <algorithm>
#include <cstdio>

namespace portals::detail {
namespace {
struct HexFailure {
  DocumentError error;
};

[[noreturn]] void failHex(SourceError code) {
  throw HexFailure{DocumentError{code, {}, {}}};
}

HexRowOptions rowOptions(const DocumentHexOptions &options, std::size_t bytes) noexcept {
  return {bytes, options.groupSize, options.uppercase, options.showAscii};
}

std::size_t fittedBytes(const DocumentPrepareIntent &intent) {
  const auto options = documentHexRowOptions(intent.config.hex, static_cast<std::size_t>(intent.viewport.width));

  if (!options) {
    failHex(SourceError::INVALID_ARGUMENT);
  }
  return options->bytesPerRow;
}

std::uint64_t lastPage(std::uint64_t extent, std::size_t bytes, std::int32_t height) noexcept {
  const auto rows = extent / bytes + (extent % bytes != 0);
  const auto page = static_cast<std::uint64_t>(height);
  return rows > page ? (rows - page) * bytes : 0;
}

std::uint64_t movedRow(std::uint64_t row, std::int64_t delta) noexcept {
  if (delta < 0) {
    const auto count = static_cast<std::uint64_t>(-(delta + 1)) + 1;
    return row > count ? row - count : 0;
  }
  const auto count = static_cast<std::uint64_t>(delta);
  return row > std::numeric_limits<std::uint64_t>::max() - count ? std::numeric_limits<std::uint64_t>::max()
                                                                 : row + count;
}

DocumentHexRole byteRole(HexByteClass value) noexcept {
  switch (value) {
  case HexByteClass::ZERO:
    return DocumentHexRole::ZERO;
  case HexByteClass::SPACE:
  case HexByteClass::PRINTABLE:
    return DocumentHexRole::PRINTABLE;
  case HexByteClass::CONTROL:
    return DocumentHexRole::CONTROL;
  case HexByteClass::HIGH_BYTE:
    return DocumentHexRole::HIGH_BYTE;
  }
  return DocumentHexRole::BODY;
}

void appendHexRow(DocumentScan &scan, const DocumentPrepareIntent &intent, std::span<const std::byte> bytes,
                  std::uint64_t offset) {
  auto &hex = *scan.hex;
  const auto options = rowOptions(hex.options, hex.bytesPerRow);
  const auto columns = *hexRowWidth(options);
  std::string formatted(columns, ' ');
  std::vector<HexByteCell> cells(bytes.size());
  const auto output = formatHexRow(bytes, offset, options, formatted, cells);
  if (!output) {
    failHex(output.error() == HexError::OFFSET_OVERFLOW ? SourceError::POSITION_EXHAUSTED
                                                        : SourceError::INVALID_ARGUMENT);
  }
  const auto first = static_cast<std::size_t>(std::min<std::uint64_t>(intent.horizontal, columns));
  const auto count = std::min(static_cast<std::size_t>(intent.viewport.width), columns - first);
  std::vector<DocumentHexRole> roles(count, DocumentHexRole::BODY);
  const auto mark = [&](std::size_t column, std::size_t length, DocumentHexRole role) {
    for (auto part = column; part < column + length; ++part) {
      if (part >= first && part - first < count) {
        roles[part - first] = role;
      }
    }
  };
  mark(0, 16, DocumentHexRole::GUTTER);
  for (const auto &cell : cells) {
    mark(cell.hexColumn, 2, byteRole(cell.byteClass));
    if (cell.asciiColumn) {
      mark(*cell.asciiColumn, 1, byteRole(cell.byteClass));
    }
  }
  if (scan.rows.size() == static_cast<std::size_t>(intent.viewport.height)) {
    scan.payload -= scan.rows.front().bytes.size();
    scan.rows.erase(scan.rows.begin());
    hex.roles.erase(hex.roles.begin());
  }
  if (count > intent.config.maxFrameBytes - std::min(scan.payload, intent.config.maxFrameBytes)) {
    failHex(SourceError::PAGE_TOO_LARGE);
  }
  scan.payload += count;
  DocumentPreparedRow row;

  row.offset = offset;
  row.line = 0;
  row.bytes.assign(formatted, first, count);
  row.end = offset + bytes.size();
  scan.rows.push_back(row);
  hex.roles.push_back(DocumentHexRoleRow{roles});
}

std::shared_ptr<const DocumentPreparedFrame> hexFrame(ByteDocumentSource &source, const DocumentPrepareIntent &intent,
                                                      const DocumentScan &scan) {
  const auto amount = documentPreparationBytes(intent.viewport, intent.config, scan.hex->rawRows.size());
  if (!amount) {
    failHex(amount.error());
  }
  auto credit = source.budget()->reserve({*amount, 0, 0, 0});
  if (!credit) {
    DocumentError error{credit.error(), {}, {}};
    error.budgetDemand = SourceBudgetUsage{*amount, 0, 0, 0};
    throw HexFailure{error};
  }
  auto frame = std::make_shared<DocumentPreparedFrame>();
  frame->credit = std::move(*credit);
  frame->viewport = intent.viewport;
  frame->status.mode = DocumentViewMode::HEX;
  frame->status.revision = scan.metadata.revision;
  frame->status.byteOffset = intent.anchor;
  frame->status.byteExtent = scan.metadata.byteExtent;
  frame->status.anchorBeyondExtent = scan.metadata.byteExtent && intent.anchor > *scan.metadata.byteExtent;
  frame->status.visibleByteOffset = scan.rows.empty() ? scan.hex->first : scan.rows.front().offset;
  frame->status.preparing = false;
  frame->status.waiting = scan.waiting;
  frame->status.finalEnd = scan.done;
  if (intent.tail || intent.rowDelta) {
    frame->status.byteOffset = frame->status.visibleByteOffset;
  }
  frame->finalEndVisible = false;
  frame->hex = std::make_unique<DocumentHexFrame>();
  frame->hex->columns = *hexRowWidth(rowOptions(scan.hex->options, scan.hex->bytesPerRow));
  frame->rows.reserve(scan.rows.size());
  frame->anchors.reserve(scan.rows.size());
  frame->hex->rows.reserve(scan.rows.size());
  for (std::size_t index = 0; index < scan.rows.size(); ++index) {
    const auto &row = scan.rows[index];
    auto text = GraphemeText::from(row.bytes, intent.width);
    if (!text) {
      failHex(SourceError::PAGE_TOO_LARGE);
    }
    frame->rows.push_back(std::move(*text));
    frame->anchors.push_back(row.offset);
    frame->visibleByteEnd = row.end;
    frame->hex->rows.emplace_back(std::initializer_list<DocumentHexRun>{});
    auto &runs = frame->hex->rows.back();
    const auto &roles = scan.hex->roles[index].columns;

    // One reserved run slot per retained column bounds capacity without a temporary row copy.
    runs.reserve(roles.size());
    for (std::size_t first = 0; first < roles.size();) {
      const auto role = roles[first];
      auto end = first + 1;
      while (end < roles.size() && roles[end] == role) {
        ++end;
      }
      if (role != DocumentHexRole::BODY) {
        auto part = GraphemeText::from(std::string_view(row.bytes).substr(first, end - first), intent.width);
        if (!part) {
          failHex(SourceError::PAGE_TOO_LARGE);
        }
        runs.push_back({static_cast<std::int32_t>(first), role, std::move(*part)});
      }
      first = end;
    }
  }
  frame->finalEndVisible = scan.done && !scan.waiting && frame->visibleByteEnd == scan.offset;
  std::array<char, 96> caption{};
  const auto progress = documentProgress(frame->status, frame->finalEndVisible, frame->visibleByteEnd);
  if (progress) {
    std::snprintf(caption.data(), caption.size(), "%u.%u%%%s", static_cast<unsigned>(*progress / 10),
                  static_cast<unsigned>(*progress % 10), scan.waiting ? " | waiting" : "");
  } else {
    std::snprintf(caption.data(), caption.size(), "progress unknown%s", scan.waiting ? " | waiting" : "");
  }
  auto footer = GraphemeText::from(caption.data(), intent.width);
  if (!footer) {
    failHex(SourceError::PAGE_TOO_LARGE);
  }
  frame->footer = std::make_unique<GraphemeText>(std::move(*footer));
  return frame;
}
} // namespace

std::shared_ptr<const DocumentPreparation> prepareHexDocument(std::shared_ptr<ByteDocumentSource> source,
                                                              const DocumentPrepareIntent &intent,
                                                              std::shared_ptr<const DocumentScan> previous,
                                                              std::uint64_t generation, SourceCredit credit,
                                                              std::stop_token stop) {
  auto result = std::make_shared<DocumentPreparation>();
  result->credit = std::move(credit);
  result->generation = generation;
  std::shared_ptr<DocumentScan> scan;
  try {
    if (stop.stop_requested()) {
      failHex(SourceError::CANCELLED);
    }
    const auto metadata = source->inspect(stop);
    if (!metadata) {
      throw HexFailure{metadata.error()};
    }
    const auto bytes = fittedBytes(intent);
    const bool resume = intent.resume && previous && previous->hex &&
                        previous->metadata.revision == metadata->revision &&
                        previous->requestedAnchor == intent.anchor && previous->layoutWidth == intent.viewport.width &&
                        previous->horizontal == intent.horizontal && previous->hex->options == intent.config.hex &&
                        previous->hex->bytesPerRow == bytes && previous->hex->height == intent.viewport.height;
    constexpr std::size_t RESULT_ENVELOPE = 16 * 1024;
    auto reserved = result->credit.split({result->credit.usage().bytes - RESULT_ENVELOPE, 0, 0, 0});
    if (!reserved) {
      failHex(reserved.error());
    }
    auto reservation = std::make_shared<SourceCredit>(std::move(*reserved));
    if (resume) {
      scan = std::make_shared<DocumentScan>(*previous);
      scan->hex = std::make_shared<DocumentHexScan>(*previous->hex);
      // Rebuild bounded capacity by copying: relocating an owning string through
      // an implicit noexcept move can terminate under MSVC debug allocation failure.
      std::vector<DocumentPreparedRow> rows(0);

      rows.reserve(static_cast<std::size_t>(intent.viewport.height));
      rows.insert(rows.end(), scan->rows.begin(), scan->rows.end());
      scan->rows.swap(rows);
      scan->waiting = false;
    } else {
      scan = std::make_shared<DocumentScan>();
      scan->metadata = *metadata;
      scan->layoutWidth = intent.viewport.width;
      scan->horizontal = intent.horizontal;
      scan->requestedAnchor = intent.anchor;
      scan->hex = std::make_shared<DocumentHexScan>();
      scan->hex->options = intent.config.hex;
      scan->hex->bytesPerRow = bytes;
      scan->hex->height = intent.viewport.height;
      auto first = intent.anchor / bytes * bytes;
      if (intent.rowDelta) {
        const auto row = movedRow(intent.anchor / bytes, intent.rowDelta);
        first = row > std::numeric_limits<std::uint64_t>::max() / bytes
                  ? std::numeric_limits<std::uint64_t>::max() / bytes * bytes
                  : row * bytes;
      }
      if (metadata->byteExtent) {
        const auto last = lastPage(*metadata->byteExtent, bytes, intent.viewport.height);
        if (intent.tail || intent.rowDelta || intent.anchor >= *metadata->byteExtent) {
          first = std::min(first, last);
          if (intent.tail) {
            first = last;
          }
        }
      } else if (intent.tail) {
        first = 0;
        scan->hex->tailUnknown = true;
      }
      if (!metadata->capabilities.seekable) {
        if (previous && (!previous->hex || previous->metadata.revision != metadata->revision ||
                         previous->hex->bytesPerRow != bytes ||
                         first < (previous->hex->rawRows.empty() ? previous->hex->first
                                                                 : previous->hex->rawRows.front()->offset))) {
          failHex(SourceError::UNSUPPORTED);
        }
        if (first && (!previous || first > previous->offset)) {
          failHex(SourceError::UNSUPPORTED);
        }
      }
      scan->hex->first = first;
      scan->offset = first;
      scan->hex->pending.reserve(bytes);
      scan->rows.reserve(static_cast<std::size_t>(intent.viewport.height));
      scan->hex->roles.reserve(static_cast<std::size_t>(intent.viewport.height));
      if (!metadata->capabilities.seekable) {
        scan->hex->rawRows.reserve(static_cast<std::size_t>(intent.viewport.height) + intent.retainedHexRows);
        if (previous && previous->hex) {
          // Until the UI accepts a relative batch, reflow may still request its old anchor.
          // Carry that bounded overlap plus resident successors; the next accepted anchor
          // evicts the old prefix. Every row owns only its subcredit, never a scan chain.
          const auto keepFrom = intent.rowDelta ? std::min(first, intent.anchor / bytes * bytes) : first;
          for (const auto &row : previous->hex->rawRows) {
            if (row->offset < keepFrom) {
              continue;
            }
            scan->hex->rawRows.push_back(row);
            if (row->offset >= first && scan->rows.size() < static_cast<std::size_t>(intent.viewport.height)) {
              appendHexRow(*scan, intent, row->bytes, row->offset);
            }
          }
          scan->offset = previous->offset;
          scan->done = previous->done;
          scan->hex->pending = previous->hex->pending;
        }
      }
    }
    scan->reservation = reservation;
    previous.reset();
    std::size_t work{};
    while (!scan->done && !scan->waiting && work < intent.config.workBytes &&
           (scan->hex->tailUnknown || scan->rows.size() < static_cast<std::size_t>(intent.viewport.height))) {
      if (stop.stop_requested()) {
        failHex(SourceError::CANCELLED);
      }
      if (scan->metadata.byteExtent && scan->offset >= *scan->metadata.byteExtent) {
        scan->done = true;
        break;
      }
      const auto capacity = std::min(bytes - scan->hex->pending.size(), intent.config.workBytes - work);
      const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
        std::min(capacity, source->maxPageBytes()), std::numeric_limits<std::uint64_t>::max() - scan->offset));
      if (!requested) {
        failHex(SourceError::POSITION_EXHAUSTED);
      }
      auto page = source->read({scan->metadata.revision, scan->offset, requested, generation}, stop);
      if (!page) {
        if (page.error().code == SourceError::NO_PROGRESS ||
            (page.error().code == SourceError::WOULD_BLOCK && !page.error().budgetDemand)) {
          scan->waiting = true;
          break;
        }
        throw HexFailure{page.error()};
      }
      const auto input = page->bytes();
      scan->hex->pending.insert(scan->hex->pending.end(), input.begin(), input.end());
      scan->offset += input.size();
      work += input.size();
      scan->done = page->finalEnd() || (scan->metadata.byteExtent && scan->offset == *scan->metadata.byteExtent);
      if (scan->hex->pending.size() == bytes || (scan->done && !scan->hex->pending.empty())) {
        const auto offset = scan->offset - scan->hex->pending.size();
        if (!scan->metadata.capabilities.seekable) {
          auto rawCredit = reservation->split({DOCUMENT_HEX_RAW_ROW_BYTES + scan->hex->pending.size(), 0, 0, 0});
          if (!rawCredit) {
            failHex(rawCredit.error());
          }
          auto row = std::make_shared<DocumentHexRawRow>();

          row->credit = std::move(*rawCredit);
          row->offset = offset;
          row->bytes.assign(scan->hex->pending.begin(), scan->hex->pending.end());
          scan->hex->rawRows.push_back(std::move(row));
        }
        appendHexRow(*scan, intent, scan->hex->pending, offset);
        scan->hex->pending.clear();
      }
      if (input.empty() && !scan->done) {
        scan->waiting = true;
      }
    }
    if (scan->done && !scan->hex->pending.empty()) {
      const auto offset = scan->offset - scan->hex->pending.size();
      if (!scan->metadata.capabilities.seekable) {
        auto rawCredit = reservation->split({DOCUMENT_HEX_RAW_ROW_BYTES + scan->hex->pending.size(), 0, 0, 0});
        if (!rawCredit) {
          failHex(rawCredit.error());
        }
        auto row = std::make_shared<DocumentHexRawRow>();

        row->credit = std::move(*rawCredit);
        row->offset = offset;
        row->bytes.assign(scan->hex->pending.begin(), scan->hex->pending.end());
        scan->hex->rawRows.push_back(std::move(row));
      }
      appendHexRow(*scan, intent, scan->hex->pending, offset);
      scan->hex->pending.clear();
    }
    // An empty final reply at a distant unknown position proves no exact extent.
    // Origin zero or actual bytes ending at the final cursor provide that proof.
    if (scan->done && !scan->metadata.byteExtent && (!scan->offset || !scan->rows.empty())) {
      scan->metadata.byteExtent = scan->offset;
    }
    result->scan = scan;
    result->continueWork =
      !scan->done && !scan->waiting &&
      (scan->hex->tailUnknown || scan->rows.size() < static_cast<std::size_t>(intent.viewport.height));
    if (!result->continueWork) {
      result->frame = hexFrame(*source, intent, *scan);
    }
  } catch (const HexFailure &failure) {
    if (failure.error.code == SourceError::WOULD_BLOCK && !failure.error.budgetDemand) {
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
