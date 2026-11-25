// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/virtual_rows_tree.hpp>
#include <portals/layout.hpp>
#include <portals/managed_navigation.hpp>
#include <portals/model.hpp>
#include <portals/model_edit.hpp>

namespace portals {
namespace ui {
template <class> class Model;
template <class> struct WindowRowsModel;
} // namespace ui

namespace detail {
/// @cond INTERNAL
template <class T> struct VirtualRowsModelBinding {
  virtual ~VirtualRowsModelBinding() = default;
  virtual std::expected<void, TreeError> replaceModel(ui::Model<T>, ModelRebindOptions) = 0;

  virtual std::expected<void, TreeError> replaceWindowModel(const ui::WindowRowsModel<T> &, ModelRebindOptions) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
};

/// @endcond
} // namespace detail

/** @brief Typed opt-in for custom editor state owned by a delegated row.
 * Custom controls call RowContext::admitEdit before changing an unfinished draft. Opaque
 * widget caches are not cloned or reset by the host. All operations run on the UI thread.
 */
class VirtualRowEditParticipant {
public:
  virtual ~VirtualRowEditParticipant() = default; ///< Release only the participant's own state.
  /** @return Whether draft, validation or admitted work still requires this exact row assignment. */
  virtual bool unfinished() const noexcept = 0;
  /** @return Admission to hide the row, or a typed refusal before visibility/focus changes.
   * This nonblocking preflight must not mutate the tree/model or commit/cancel input.
   */
  virtual std::expected<void, TreeError> beforeLeave() = 0;

  /** @brief Retire the participant after its row identity has closed; never commit/cancel backend work. */
  virtual void retire() noexcept {
  }
};

/** @brief Fixed-height bounded real-child row host settings. */
struct VirtualRowsOptions {
  ModelCheckpointOptions checkpoints{}; ///< Existing bounded source-position history budget.
  EditOptions editing{};                ///< Existing common field editing mode and retained field/byte limits.
  std::int32_t rowHeight{1};            ///< Positive terminal cell rows per domain row.
  std::size_t maxRows{256};          ///< Positive maximum visible assignments; excessive viewport demand is rejected.
  std::size_t overscanRows{2};       ///< Additional native delivered rows for smooth bounded row-step overlap.
  std::int64_t maxPendingRows{4096}; ///< Positive cap on accepted native relative row intents awaiting delivery.
};

namespace ui::detail {
/// @cond INTERNAL
struct RowPlacement {
  Node<Widget> node;
  std::int32_t y{};
};

struct VirtualRowsState {
  virtual ~VirtualRowsState() = default;
  VirtualRowsOptions options;
  std::vector<RowPlacement> placements = std::vector<RowPlacement>(0);
  std::optional<TreeError> failure;
  std::optional<SourceError> sourceFailure;
  std::optional<WindowStatus> sourceStatus;
  std::optional<SourceCursor> sourceRecovery;
  std::optional<SourceCapabilities> sourceCapabilities;
  std::optional<std::uint64_t> exactCount;
  std::uint64_t first{};
  virtual std::expected<void, TreeError> requestFirst(std::uint64_t) = 0;
  virtual std::expected<void, TreeError> requestMove(std::int64_t) = 0;
  virtual std::expected<void, TreeError> requestPage(bool) = 0;

  virtual std::expected<void, TreeError> requestAnchor(SourceAnchor, SourceDirection) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  virtual std::expected<void, TreeError> notifyReady(std::uint64_t) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
};

/// @endcond
} // namespace ui::detail

/** @brief Bounded real widget subtrees, independent of the text-only VirtualList.
 * @tparam T Immutable domain row type supplied to the declarative row factory.
 * Layout only places committed assignments; factories and managed mutation run between frames.
 */
template <class T> class VirtualRows final : public SizedWidget, public Layout {
public:
  /// @cond INTERNAL
  explicit VirtualRows(std::shared_ptr<ui::detail::VirtualRowsState> state, SizePolicy policy = DEFAULT_VIEWPORT_POLICY)
      : SizedWidget(policy), state_(std::move(state)) {
  }

  const std::shared_ptr<ui::detail::VirtualRowsState> &state() const noexcept {
    return state_;
  }

  /// @endcond
  /** @return Last committed first finite ordinal; unknown-total hosts use cursor navigation instead. */
  std::uint64_t firstRow() const noexcept {
    return state_->first;
  }

  /** @return Last checked asynchronous preparation failure, or empty after success. */
  std::optional<TreeError> lastError() const noexcept {
    return state_->failure;
  }

  /** @return Exact finite extent, or empty for native cursor sources. */
  std::optional<std::uint64_t> rowCount() const noexcept {
    return state_->exactCount;
  }

  /** @return Native source protocol outcome; finite models return empty. */
  std::optional<WindowStatus> windowStatus() const noexcept {
    return state_->sourceStatus;
  }

  /** @return Native source's exact retained protocol failure, independent of tree errors. */
  std::optional<SourceError> sourceError() const noexcept {
    return state_->sourceFailure;
  }

  /** @return Explicit native history-recovery cursor; never followed implicitly. */
  std::optional<SourceCursor> recoveryCursor() const noexcept {
    return state_->sourceRecovery;
  }

  /** @return Captured native traversal capabilities, or empty for finite sources. */
  std::optional<SourceCapabilities> capabilities() const noexcept {
    return state_->sourceCapabilities;
  }

  /** @return Number of currently displayed real row subtrees. */
  std::size_t mountedRows() const noexcept {
    return state_->placements.size();
  }

  /** @param available Offered viewport cells. @return Fixed row-height child bounds. */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override {
    return CellSize{available.width, state_->options.rowHeight};
  }

  /** @brief Ignore child measurements. @return Empty intrinsic extent; size policy owns viewport sizing. */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild>) const override {
    return CellSize{};
  }

  /** @param children Visible committed children. @param available Allocated viewport.
   * @return Bounded row rectangles; never reads a model or calls a factory.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override try {
    std::vector<CellRect> result(0);
    result.reserve(children.size());
    for (const auto &child : children) {
      if (child.desired.height > state_->options.rowHeight) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      const auto found = std::ranges::find(state_->placements, child.node, [](const auto &entry) {
        return entry.node.id();
      });
      if (found == state_->placements.end()) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      auto rect = CellRect::from({0, found->y}, {available.width, state_->options.rowHeight});
      if (!rect) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      result.push_back(*rect);
    }
    // Copy into the elided expected result: MSVC checked vector moves can allocate in noexcept.
    return std::expected<std::vector<CellRect>, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }

private:
  std::shared_ptr<ui::detail::VirtualRowsState> state_;
};

/** @brief Request a finite ordinal; mounting occurs at the existing managed service boundary.
 * @tparam T Domain row type. @param tree UI owner. @param node Live row host.
 * @param first Requested finite row ordinal, clamped at source end.
 * @return Admission or checked lifetime, capability, editor-participation or allocation failure.
 */
template <class T>
std::expected<void, TreeError> setVirtualRowsFirst(WidgetTree &tree, Node<VirtualRows<T>> node, std::uint64_t first) {
  std::expected<void, TreeError> result;
  const auto changed = tree.edit(node, [&](auto &host) {
    result = host.state()->requestFirst(first);
  });
  return changed ? result : changed;
}

/** @brief Accumulate a relative row intent without replacing it with a stale committed position.
 * @tparam T Domain row. @param tree UI owner. @param node Live host. @param rows Signed logical row steps.
 * @return Admission or typed capacity, capability, lifetime or editor-participation failure.
 */
template <class T>
std::expected<void, TreeError> scrollVirtualRows(WidgetTree &tree, Node<VirtualRows<T>> node, std::int64_t rows) {
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](auto &host) {
    result = host.state()->requestMove(rows);
  });
  return edited ? result : edited;
}

/** @brief Navigate one bounded delivered page, preserving unknown source extent.
 * @tparam T Domain row type. @param tree UI owner. @param node Live host. @param backwards Request backward traversal.
 * @return Admission or an explicit unsupported/lifetime/participation failure.
 */
template <class T>
std::expected<void, TreeError> pageVirtualRows(WidgetTree &tree, Node<VirtualRows<T>> node, bool backwards = false) {
  std::expected<void, TreeError> result;
  auto changed = tree.edit(node, [&](auto &host) {
    result = host.state()->requestPage(backwards);
  });
  return changed ? result : changed;
}

/** @brief Request a capability-gated native row/tail/cursor operation.
 * @tparam T Domain row. @param tree UI owner. @param node Live host. @param anchor Exact source operation.
 * @param direction Existing provider traversal direction. @return Admission or explicit unsupported/participation
 * failure.
 */
template <class T>
std::expected<void, TreeError> navigateVirtualRows(WidgetTree &tree, Node<VirtualRows<T>> node, SourceAnchor anchor,
                                                   SourceDirection direction = SourceDirection::FORWARD) {
  std::expected<void, TreeError> result;
  auto changed = tree.edit(node, [&](auto &host) {
    result = host.state()->requestAnchor(anchor, direction);
  });
  return changed ? result : changed;
}

/** @brief Publish a native provider readiness revision through the existing source notifier.
 * @tparam T Domain row. @param tree UI owner. @param node Live host. @param revision Positive strictly newer revision.
 * @return Queue admission or checked protocol/lifetime failure; sourceError retains the exact protocol error.
 */
template <class T>
std::expected<void, TreeError> notifyVirtualRowsReady(WidgetTree &tree, Node<VirtualRows<T>> node,
                                                      std::uint64_t revision) {
  std::expected<void, TreeError> result;
  auto changed = tree.edit(node, [&](auto &host) {
    result = host.state()->notifyReady(revision);
  });
  return changed ? result : changed;
}
} // namespace portals
