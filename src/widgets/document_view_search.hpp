// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_view.hpp>
#include <vector>

namespace portals::detail {
// Original source bytes; callers provide a proven whole-grapheme range.
struct DocumentSearchSelection {
  DocumentRevision revision;
  std::uint64_t begin{};
  std::uint64_t end{};

  friend bool operator==(const DocumentSearchSelection &, const DocumentSearchSelection &) = default;
};

struct DocumentSearchPresentation {
  std::optional<DocumentSearchSelection> selection;
  bool reflow{};
};

struct DocumentSearchMap {
  std::uint64_t rowOffset{};
  std::size_t begin{};
  std::size_t end{};
};

struct DocumentSearchScan {
  DocumentSearchSelection selection;
  std::vector<DocumentSearchMap> maps = std::vector<DocumentSearchMap>(0);
};

struct DocumentSearchRun {
  std::size_t row{};
  std::int32_t column{};
  std::size_t first{};
  std::size_t count{};
};

struct DocumentSearchFrame {
  SourceCredit credit; // Release prepared storage before its independent reservation.
  DocumentSearchSelection selection;
  DocumentDecoderOptions decoderOptions;
  std::vector<DocumentSearchRun> runs = std::vector<DocumentSearchRun>(0);
};

// Shared-owner and checked allocator bookkeeping remain charged with the payload.
inline constexpr std::size_t DOCUMENT_SEARCH_OWNER_BYTES = 128;
inline constexpr std::size_t DOCUMENT_SEARCH_FRAME_BYTES = sizeof(DocumentSearchFrame) + DOCUMENT_SEARCH_OWNER_BYTES;

struct DocumentScan;
struct DocumentPreparedFrame;
struct DocumentPrepareIntent;

// Worker-only original-byte mapping. Capacity belongs to the staged scan credit.
void stageDocumentSearch(DocumentScan &scan, const DocumentPrepareIntent &intent);
void trimDocumentSearch(DocumentScan &scan, std::uint64_t firstRetained);
void mapDocumentSearch(DocumentScan &scan, std::uint64_t rowOffset, std::size_t renderedBegin, std::size_t renderedEnd,
                       std::uint64_t sourceBegin, std::uint64_t sourceEnd);
std::expected<void, DocumentError> projectDocumentSearch(DocumentPreparedFrame &frame, const DocumentScan &scan);

// Managed UI operation. Preparation and any source access remain on workers.
std::expected<void, ManagedError> setDocumentSearchSelection(ManagedScene &scene, Node<DocumentView> node,
                                                             std::optional<DocumentSearchSelection> selection,
                                                             bool reveal);
} // namespace portals::detail
