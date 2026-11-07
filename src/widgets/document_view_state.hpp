// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "scroll_controls_state.hpp"
#include <array>
#include <limits>
#include <portals/document_view.hpp>
#include <portals/style.hpp>

namespace portals::ui::detail {
struct SceneActivity;
}

namespace portals::detail {
inline constexpr std::int32_t DOCUMENT_GUTTER_CELLS = 9;
inline constexpr std::size_t DOCUMENT_PREPARATION_ENVELOPE = 64 * 1024;
inline constexpr std::size_t DOCUMENT_PREPARED_CELL_BYTES = 128;

// One checked allowance is shared by job/frame admission and peak overlap.
// Separate gutter text has its own vector/object/cluster capacity even when
// only a narrow clipped prefix is visible in the actual viewport.
inline std::expected<std::size_t, SourceError> documentPreparationBytes(CellSize viewport,
                                                                        const DocumentViewConfig &config) noexcept {
  constexpr auto MAX = std::numeric_limits<std::size_t>::max();
  if (viewport.width <= 0 || viewport.height <= 0) {
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }
  const auto width = static_cast<std::size_t>(viewport.width);
  const auto height = static_cast<std::size_t>(viewport.height);
  if (width > MAX / height || width * height > config.maxFrameCells) {
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }
  auto bytes = DOCUMENT_PREPARATION_ENVELOPE;
  const auto include = [&](std::size_t count, std::size_t unit) {
    if (count > (MAX - bytes) / unit) {
      return false;
    }
    bytes += count * unit;
    return true;
  };
  constexpr auto GUTTER_ROW_BYTES = sizeof(GraphemeText) + DOCUMENT_GUTTER_CELLS * DOCUMENT_PREPARED_CELL_BYTES;
  if (!include(config.maxFrameBytes, 4) || !include(width * height, DOCUMENT_PREPARED_CELL_BYTES) ||
      (config.lineNumbers && !include(height, GUTTER_ROW_BYTES))) {
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }
  return bytes;
}

inline constexpr std::size_t DOCUMENT_CHECKPOINTS = 256;
inline constexpr std::size_t DOCUMENT_CLUSTER_BYTES = 4096;
inline constexpr std::size_t DOCUMENT_CLUSTER_SCALARS = 512;

struct DocumentLayoutCheckpoint {
  DocumentRevision revision;
  std::optional<DocumentDecoderCheckpoint> decoder;
  std::uint64_t offset{};
  std::uint64_t line{1};
  bool afterCR{};
  bool continuedCluster{};
  bool lineKnown{true};
  bool regionalContextUnknown{};
};

struct DocumentPreparedRow {
  std::uint64_t offset{};
  std::uint64_t line{1};
  std::string bytes = std::string(0, '\0');
};

// Only a worker mutates a new copy. Published snapshots are never modified again.
struct DocumentScan {
  std::shared_ptr<const SourceCredit> reservation; // Outlives every copied/published scanner allocation.
  DocumentMetadata metadata;
  DocumentDecoderOptions decoderOptions;
  std::int32_t layoutWidth{};
  std::uint64_t requestedAnchor{};
  std::uint64_t horizontal{};
  bool wrap{};
  DocumentDecoder decoder;
  // The real original-prefix proof is revision/configuration qualified by this scan.
  // It survives eviction of layout checkpoints; fixed probes never retain source pages.
  std::optional<DocumentDecoderCheckpoint> origin;
  std::array<std::byte, DocumentDecoder::LOCAL_WINDOW_BYTES> localProbe{};
  std::size_t localProbeSize{};
  std::uint64_t localProbeOffset{};
  bool localProbeFinal{};
  bool localPending{};
  bool localStart{};
  bool regionalContextUnknown{};
  bool lineKnown{true};
  std::uint64_t offset{};
  std::uint64_t line{1};
  std::uint64_t column{};
  std::uint64_t rowOffset{};
  std::uint64_t clusterOffset{};
  std::uint64_t clusterEnd{};
  DocumentLayoutCheckpoint clusterCheckpoint;
  DocumentLayoutCheckpoint scalarCheckpoint;
  std::array<DocumentLayoutCheckpoint, DOCUMENT_CHECKPOINTS> checkpoints{};
  std::size_t checkpointCount{};
  std::size_t checkpointCursor{};
  std::array<char, DOCUMENT_CLUSTER_BYTES> cluster{};
  std::size_t clusterBytes{};
  std::size_t clusterScalars{};
  std::int32_t previousScalar{};
  std::int32_t graphemeState{};
  bool havePrevious{};
  bool resumeContinuation{};
  bool afterCR{};
  bool rowTouched{};
  std::uint64_t skipRows{};
  std::optional<std::uint64_t> anchorLine;
  std::optional<std::uint64_t> totalLines;
  bool reached{};
  bool done{};
  bool waiting{};
  bool fragmented{};
  std::size_t payload{};
  std::string row = std::string(0, '\0');
  std::vector<DocumentPreparedRow> rows = std::vector<DocumentPreparedRow>(0);
};

struct DocumentPreparedFrame {
  SourceCredit credit; // Declared first: payload is destroyed before returning credit.
  DocumentViewStatus status;
  std::vector<GraphemeText> rows = std::vector<GraphemeText>(std::initializer_list<GraphemeText>{});
  std::optional<std::vector<GraphemeText>> gutters;
  std::int32_t gutterCells{};
  std::vector<std::uint64_t> anchors = std::vector<std::uint64_t>(0);
  std::unique_ptr<GraphemeText> footer;
  CellSize viewport;
};

struct DocumentViewState {
  WidthPolicy width;
  DocumentViewConfig config;
  std::shared_ptr<const DocumentPreparedFrame> frame;
  std::shared_ptr<ui::detail::SceneActivity> activity;
  std::shared_ptr<ByteDocumentSource> source;
  StyleRoleRegistration roles;
  DocumentViewStatus status;
  std::uint64_t anchor{};
  std::uint64_t horizontal{};
  std::uint64_t serial{};
  std::int64_t rowDelta{};
  bool tail{};
  bool refresh{true};
};

struct DocumentViewAccess {
  static DocumentViewState &state(DocumentView &widget) noexcept {
    return *widget.document_;
  }

  static const DocumentViewState &state(const DocumentView &widget) noexcept {
    return *widget.document_;
  }

  static std::shared_ptr<DocumentViewState> shared(DocumentView &widget) noexcept {
    return widget.document_;
  }
};

struct DocumentPrepareIntent {
  DocumentViewConfig config;
  WidthPolicy width;
  CellSize viewport;
  std::uint64_t anchor{};
  std::uint64_t horizontal{};
  std::int64_t rowDelta{};
  bool tail{};
  bool resume{};
  std::optional<DocumentLayoutCheckpoint> seed;
};

struct DocumentPreparation {
  SourceCredit credit;
  std::shared_ptr<const DocumentScan> scan;
  std::shared_ptr<const DocumentPreparedFrame> frame;
  std::optional<DocumentError> error;
  std::uint64_t generation{};
  bool continueWork{};
  bool providerWaiting{};
};

ScrollAreaOptions documentScrollOptions();

// Run only on a worker. All capacity is precharged by the job before calling.
std::shared_ptr<const DocumentPreparation> prepareDocument(std::shared_ptr<ByteDocumentSource> source,
                                                           const DocumentPrepareIntent &intent,
                                                           std::shared_ptr<const DocumentScan> previous,
                                                           std::uint64_t generation, SourceCredit credit,
                                                           std::stop_token stop);
} // namespace portals::detail
