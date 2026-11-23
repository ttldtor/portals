// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_highlight.hpp>
#include <portals/document_source.hpp>
#include <portals/render.hpp>

#include <array>
#include <memory>
#include <vector>

namespace portals::detail {
struct DocumentPreparedFrame;
struct DocumentScan;
struct DocumentPrepareIntent;
struct DocumentPreparation;

// A mapping is retained only for a displayed cluster. Rendered offsets are UTF-8;
// source offsets refer to the original encoding and never derive from UTF-8 lengths.
struct DocumentTextMap {
  std::uint64_t sourceOffset{};
  std::size_t renderedOffset{};
  std::uint64_t rowOffset{};
  SemanticRole role{SemanticRole::NORMAL};
  bool classified{};
};

struct DocumentTextRun {
  std::int32_t column{};
  std::size_t first{};
  std::size_t count{};
  SemanticRole role{SemanticRole::NORMAL};
};

struct DocumentTextFrame {
  std::vector<std::vector<DocumentTextRun>> rows = std::vector<std::vector<DocumentTextRun>>(0);
};

inline constexpr std::size_t DOCUMENT_LEXICAL_CHECKPOINTS = 32;
inline constexpr std::uint64_t DOCUMENT_LEXICAL_CHECKPOINT_BYTES = 256 * 1024;

struct DocumentLexicalCheckpoint {
  DocumentDecoderCheckpoint decoder;
  DocumentHighlightCheckpoint lexer;
  std::uint64_t offset{};
};

// A worker stages a private copy under the containing DocumentScan reservation.
// Published copies are immutable. No UI callback or application owner is retained.
inline constexpr std::uint8_t DOCUMENT_UNCLASSIFIED_ROLE = 255;

// A bounded raw replay window for an explicitly forward-only provider. A page is
// copied once before its owner is released; accepted bytes never depend on a UI frame.
struct DocumentSourceReplay {
  explicit DocumentSourceReplay(std::size_t capacity) : bytes(capacity), roles(capacity, DOCUMENT_UNCLASSIFIED_ROLE) {
  }

  std::vector<std::byte> bytes;
  std::vector<std::uint8_t> roles;
  std::uint64_t begin{};
  std::uint64_t end{};
  std::size_t head{};
  bool final{};
};

// Synthetic fragment offsets never enter the qualified source lexer or replay.
struct DocumentHighlightPreview {
  DocumentHighlighter lexer;
  std::vector<DocumentTextMap> maps = std::vector<DocumentTextMap>(0);
  std::optional<TokenRun> lastRun;
  std::uint64_t fragmentBase{};
  std::uint64_t nextOffset{};
  std::uint64_t clusterOffset{};
  SemanticRole clusterRole{SemanticRole::NORMAL};
  bool clusterClassified{};
  bool started{};
  bool final{};
  bool failed{};
};

struct DocumentHighlightAttempt {
  CellSize viewport;
  WidthPolicy width;
  std::uint64_t anchor{};
  std::uint64_t horizontal{};
  std::int64_t rowDelta{};
  DocumentSyntaxPolicy policy{DocumentSyntaxPolicy::AUTO};
  std::size_t remaining{};
  std::size_t budget{};
  bool wrap{};
  bool lineNumbers{};
  bool tail{};
  bool exhausted{};
};

struct DocumentHighlightScan {
  DocumentLanguage language{DocumentLanguage::PLAIN};
  DocumentRevision revision;
  DocumentDecoderOptions decoderOptions;
  DocumentDecoder decoder;
  DocumentHighlighter lexer;
  std::array<std::optional<DocumentLexicalCheckpoint>, DOCUMENT_LEXICAL_CHECKPOINTS> checkpoints{};
  std::size_t checkpointCount{};
  std::size_t checkpointCursor{};
  std::uint64_t lastCheckpoint{};
  std::uint64_t offset{};
  std::uint64_t classifiedEnd{};
  std::uint64_t retainBegin{};
  std::uint64_t retainEnd{};
  std::uint64_t previewOriginalByteCutoff{};
  std::vector<DocumentTextMap> maps = std::vector<DocumentTextMap>(0);
  std::vector<DocumentTextMap> cachedMaps = std::vector<DocumentTextMap>(0);
  std::size_t maxMaps{};
  std::uint64_t clusterOffset{};
  SemanticRole clusterRole{SemanticRole::NORMAL};
  bool clusterClassified{};
  bool started{};
  bool beganAtOrigin{true};
  std::shared_ptr<const DocumentPreparedFrame> plain;
  std::optional<DocumentSourceReplay> replay;
  std::shared_ptr<DocumentHighlightPreview> preview;
  std::optional<DocumentHighlightAttempt> attempt;
  bool previewLayout{};
  std::optional<DocumentHighlightResult> failure;
  bool sequential{};
  bool final{};
  bool waiting{};
  bool ready{};
  bool pending{};
  DocumentSyntaxState status{DocumentSyntaxState::PENDING};
};

// A cached span is borrowed from the current staged replay owner until its next
// append. A provider span pins its owning page for the complete decoder call.
class DocumentInputPage {
public:
  explicit DocumentInputPage(DocumentBytePage page)
      : page_(std::move(page)), bytes_(page_->bytes()), final_(page_->finalEnd()) {
  }

  DocumentInputPage(std::span<const std::byte> bytes, bool final) : bytes_(bytes), final_(final) {
  }

  std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }

  bool finalEnd() const noexcept {
    return final_;
  }

private:
  std::optional<DocumentBytePage> page_;
  std::span<const std::byte> bytes_;
  bool final_{};
};

std::expected<DocumentInputPage, DocumentError>
readDocumentInput(ByteDocumentSource &source, DocumentReadRequest request, DocumentHighlightScan *highlight,
                  std::stop_token stop, bool protectWindow = true, DocumentReadBatch *batch = nullptr);

// Worker-only helpers. The current layout scan owns all mutable state and its
// precharged reservation; prepared ranges borrow only immutable frame text.
bool sameDocumentDecoderOptions(const DocumentDecoderOptions &left, const DocumentDecoderOptions &right) noexcept;
std::shared_ptr<DocumentHighlightScan> stageDocumentHighlight(const DocumentScan &scan,
                                                              const DocumentPrepareIntent &intent);
void feedDocumentHighlight(DocumentHighlightScan &scan, std::span<const DecodedScalar> input, bool final = false);
void feedDocumentPreview(DocumentHighlightScan &scan, std::span<const DecodedScalar> input, bool final = false);
void mapDocumentCluster(DocumentHighlightScan &scan, std::uint64_t rowOffset, std::size_t renderedOffset,
                        std::uint64_t sourceOffset);
void trimDocumentHighlightMaps(DocumentHighlightScan &scan, std::uint64_t firstRetained);
std::unique_ptr<const DocumentTextFrame> projectDocumentHighlight(const DocumentPreparedFrame &frame,
                                                                  const DocumentHighlightScan &scan, bool &ready,
                                                                  bool preview = false);
} // namespace portals::detail
