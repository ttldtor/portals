// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "scroll_controls_state.hpp"
#include <array>
#include <portals/document_view.hpp>

namespace portals::ui::detail {
struct SceneActivity;
}

namespace portals::detail {
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
