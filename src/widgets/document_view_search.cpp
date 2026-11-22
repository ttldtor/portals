// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_state.hpp"
#include <algorithm>

namespace portals::detail {
void stageDocumentSearch(DocumentScan &scan, const DocumentPrepareIntent &intent) {
  if (!intent.searchSelection || intent.searchSelection->revision != scan.metadata.revision) {
    scan.search.reset();

    return;
  }

  auto staged = std::make_shared<DocumentSearchScan>();

  staged->selection = *intent.searchSelection;
  staged->maps.reserve(static_cast<std::size_t>(intent.viewport.width) *
                       (static_cast<std::size_t>(intent.viewport.height) + 1));
  if (scan.search && scan.search->selection == *intent.searchSelection) {
    staged->maps.insert(staged->maps.end(), scan.search->maps.begin(), scan.search->maps.end());
  }
  scan.search = std::move(staged);
}

void trimDocumentSearch(DocumentScan &scan, std::uint64_t firstRetained) {
  if (!scan.search) {
    return;
  }

  std::erase_if(scan.search->maps, [firstRetained](const DocumentSearchMap &map) {
    return map.rowOffset < firstRetained;
  });
}

void mapDocumentSearch(DocumentScan &scan, std::uint64_t rowOffset, std::size_t renderedBegin, std::size_t renderedEnd,
                       std::uint64_t sourceBegin, std::uint64_t sourceEnd) {
  if (!scan.search || renderedBegin == renderedEnd) {
    return;
  }

  const auto &selection = scan.search->selection;

  if (sourceBegin < selection.begin || sourceEnd > selection.end || sourceBegin == sourceEnd) {
    return;
  }

  auto &maps = scan.search->maps;

  if (!maps.empty() && maps.back().rowOffset == rowOffset && maps.back().end == renderedBegin) {
    maps.back().end = renderedEnd;

    return;
  }

  // Visible maps fit the reserved screen plus the one pending layout row.
  // Never let vector growth silently exceed the source-budget admission.
  if (maps.size() == maps.capacity()) {
    throw std::bad_alloc();
  }

  maps.push_back({rowOffset, renderedBegin, renderedEnd});
}

std::expected<void, DocumentError> projectDocumentSearch(DocumentPreparedFrame &frame, const DocumentScan &scan) {
  if (!scan.search || scan.search->selection.revision != frame.status.revision) {
    return {};
  }

  std::size_t capacity{};

  for (const auto anchor : frame.anchors) {
    capacity += static_cast<std::size_t>(
      std::count_if(scan.search->maps.begin(), scan.search->maps.end(), [anchor](const DocumentSearchMap &map) {
        return map.rowOffset == anchor;
      }));
  }

  const auto amount = DOCUMENT_SEARCH_FRAME_BYTES + capacity * sizeof(DocumentSearchRun);
  auto credit = frame.credit.split({amount, 0, 0, 0});

  if (!credit) {
    return std::unexpected(DocumentError{credit.error(), {}, {}});
  }

  auto projected = std::make_shared<DocumentSearchFrame>();

  projected->credit = std::move(*credit);
  projected->selection = scan.search->selection;
  projected->decoderOptions = scan.decoderOptions;
  projected->runs.reserve(capacity);

  for (std::size_t row = 0; row < frame.rows.size(); ++row) {
    const auto graphemes = frame.rows[row].graphemes();

    for (const auto &map : scan.search->maps) {
      if (map.rowOffset != frame.anchors[row]) {
        continue;
      }

      std::int32_t column{};
      std::size_t first{};

      while (first < graphemes.size() && graphemes[first].byte_end <= map.begin) {
        column += graphemes[first].cells;
        ++first;
      }

      auto end = first;

      while (end < graphemes.size() && graphemes[end].byte_begin < map.end) {
        ++end;
      }

      if (end != first) {
        projected->runs.push_back({row, column, first, end - first});
      }
    }
  }

  frame.search = std::move(projected);

  return {};
}
} // namespace portals::detail
