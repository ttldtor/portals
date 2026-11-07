// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_encoding.hpp>
#include <portals/document_source.hpp>
#include <portals/scroll_controls.hpp>

namespace portals {
class ManagedScene;
struct ManagedError;

namespace detail {
struct DocumentViewState;
struct DocumentViewAccess;
} // namespace detail

/** @brief Bounded text projection; these limits never restrict logical line length. */
struct DocumentViewConfig {
  DocumentDecoderOptions decoder{.hint = DocumentEncoding::UTF8}; ///< UTF-8 hint; explicit encoding takes precedence.
  std::size_t maxFrameCells{16384};     ///< Maximum actual viewport area; larger views report PAGE_TOO_LARGE.
  std::size_t maxFrameBytes{64 * 1024}; ///< Rendered payload cap, including escaped controls and cluster continuations.
  std::size_t workBytes{256 * 1024};    ///< Maximum source bytes admitted per worker turn.
  bool wrap{true};                      ///< Wrap complete ordinary graphemes at the viewport edge.
  bool lineNumbers{};                   ///< Show proven logical line numbers in a fixed gutter.
};

/** @brief Owning scalar observation; no source I/O or preparation is performed by status commands. */
struct DocumentViewStatus {
  DocumentRevision revision;         ///< Last worker-observed exact source revision.
  std::uint64_t byteOffset{};        ///< Requested original byte anchor, preserved during reflow.
  std::uint64_t visibleByteOffset{}; ///< Proven first rendered row/fragment byte, independent of the requested anchor.
  std::optional<std::uint64_t> byteExtent;  ///< Last observed exact extent, or unknown.
  std::optional<std::uint64_t> logicalLine; ///< One-based proven line containing the anchor.
  std::optional<std::uint64_t> totalLines;  ///< Proven final logical count, or unknown.
  std::optional<DocumentError> error;       ///< Latest preparation failure, if any.
  bool anchorBeyondExtent{}; ///< Requested anchor exceeds the last known extent; rendering projects available content.
  bool backpressured{}; ///< Shared capacity is busy; source-budget capacity notification permits a bounded retry.
  bool preparing{true}; ///< Bounded work remains for the latest intent.
  bool waiting{};       ///< Provider returned no bytes without definitive EOF; explicit refresh resumes it.
  bool finalEnd{};      ///< Latest worker reached definitive EOF.
  bool fragmentedCluster{}; ///< Grapheme continuation used when a cluster or bounded context cannot be proven locally.
};

/** @brief Read-only retained document viewport; source I/O, decoding and layout run on application workers.
 * Construct with ui::viewer for automatic scene-owned preparation. Paint only consumes immutable frames.
 * Raw tree emplacement provides an empty viewport until a managed source is attached.
 */
class DocumentView : public ScrollArea {
public:
  /** @param width Immutable terminal width policy. @param config Bounded preparation settings.
   * @param style Manual appearances beneath theme rules. @param frame Owning standard frame options.
   * @param scroll Optional native scroll input/chrome overrides; absent selects viewer defaults.
   * @throws std::bad_alloc On construction allocation before mounting.
   */
  explicit DocumentView(WidthPolicy width = {}, DocumentViewConfig config = {}, ControlStyle style = {},
                        FrameOptions frame = {}, std::optional<ScrollAreaOptions> scroll = {});
  ~DocumentView() override; ///< Release bounded UI ownership without joining background work.
  /** @param children Must be empty. @return Prepared intrinsic extent or INVALID_ARGUMENT. */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @param context Borrowed renderer. @return Existing frame/sink error; never reads or parses a source. */
  std::expected<void, TreeError> paint(PaintContext &context) override;

  /** @return True for the read-only style selector. */
  bool styleReadOnly() const noexcept override {
    return true;
  }

private:
  friend struct detail::DocumentViewAccess;
  std::shared_ptr<detail::DocumentViewState> document_;
};

/** @param tree UI owner outside frames. @param node Live managed viewer. @param offset Original byte anchor.
 * @return Deferred seek admission; no source I/O or decoder work is performed.
 */
std::expected<void, SourceError> seekDocumentByte(WidgetTree &tree, Node<DocumentView> node, std::uint64_t offset);
/** @param tree UI owner outside frames. @param node Live managed viewer.
 * @return Deferred metadata/read refresh admission, preserving the byte anchor.
 */
std::expected<void, SourceError> refreshDocument(WidgetTree &tree, Node<DocumentView> node);
/** @param tree UI owner outside frames. @param node Live managed viewer.
 * @return Owning status or exact tree/affinity error; never reports errors or invalidates.
 */
std::expected<DocumentViewStatus, TreeError> documentViewStatus(WidgetTree &tree, Node<DocumentView> node);
/// @cond INTERNAL
std::expected<void, ManagedError> setDocumentLineNumbers(ManagedScene &, Node<DocumentView>, bool);
std::expected<void, ManagedError> setDocumentWrap(ManagedScene &, Node<DocumentView>, bool);
std::expected<void, ManagedError> setDocumentEncoding(ManagedScene &, Node<DocumentView>, DocumentEncoding);
/// @endcond
} // namespace portals
