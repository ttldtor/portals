// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/action.hpp>
#include <portals/layout.hpp>
#include <portals/model.hpp>

namespace portals {
/** @brief Owning fixed-row presentation settings; invalid heights are rejected at preparation. */
struct VirtualListOptions {
  std::int32_t row_height{1};   ///< Positive cell rows per logical row.
  std::size_t overscan_rows{2}; ///< Extra requested rows on each side, never formatted while invisible.
  WidthPolicy width_policy;     ///< Required policy for delegate and placeholder text.
  CellStyle normal;             ///< Unselected row fill and text.
  CellStyle selected;           ///< Selected row fill and text.
};

/// @cond INTERNAL
namespace detail {
struct ListWindow {
  RowRange visible;
  RowRange requested;
};

std::expected<ListWindow, TreeError> listWindow(std::uint64_t count, std::uint64_t first, CellSize viewport,
                                                VirtualListOptions options);

struct ListRow {
  std::optional<RowId> id;
  GraphemeText text;
};

std::expected<void, TreeError> paintList(PaintContext &context, CellSize viewport, VirtualListOptions options,
                                         std::span<const ListRow> rows, std::optional<RowId> selected);
std::expected<bool, TreeError> listShown(WidgetTree &tree, Node<Widget> node);
} // namespace detail

/// @endcond

template <class T> class VirtualList;
/** @brief Prepare a local final viewport between frames, using the same pin as every sibling view.
 * Returns a new request exactly once for caller dispatch. No worker is started. A rejected completion
 * can be retried on that token; cancel it if dispatch fails, then prepare again for a new receipt.
 * Hidden ancestors cancel work and release prepared rows. Failure leaves the previous presentation intact.
 * Tree affinity/frame errors, invalid dimensions/ranges, NO_MEMORY and CALLBACK_FAILED are explicit.
 * @tparam T Immutable domain row type satisfying VirtualList's provider/delegate contract.
 * @param tree UI owner; borrowed only for this call, no calls during its frame.
 * @param node Checked observer of the list to prepare.
 * @param frame One shared application pin; the view retains an owning copy on success.
 * @param viewport Final local size from the application's layout constraints, in nonnegative cells.
 * @return Empty optional when no new dispatch is needed, an owning request receipt, or TreeError.
 * @note A changed presentation invalidates paint; an unchanged preparation is a no-op. Exact viewport mismatch at paint
 * returns INVALID_GEOMETRY. Snapshot/delegate failures leave old frame, rows and IDs together; retry may cancel prior
 * work.
 */
template <class T>
std::expected<std::optional<ModelRequest<T>>, TreeError>
prepareVirtualList(WidgetTree &tree, Node<VirtualList<T>> node, const ModelFrame<T> &frame, CellSize viewport);
/** @brief Change the integral first row on UI and invalidate paint; prepare before painting again.
 * @tparam T Immutable domain row type, as defined by VirtualList.
 * @param tree Borrowed UI owner; mutation inside a frame is rejected.
 * @param node Checked list observer; does not convey ownership.
 * @param first Requested position; preparation clamps it to the last row, or zero for an empty model.
 * @return Success or the tree's affinity, lifetime, frame or invalidation error.
 * @note Cancels old work and navigation intent, but preserves the selected RowId.
 */
template <class T>
std::expected<void, TreeError> setVirtualListFirst(WidgetTree &tree, Node<VirtualList<T>> node, std::uint64_t first);
/** @brief Set stable selection on UI, clear pending navigation, and invalidate paint.
 * @tparam T Immutable domain row type, as defined by VirtualList.
 * @param tree Borrowed UI owner; mutation inside a frame is rejected.
 * @param node Checked list observer.
 * @param selected Owning stable ID, or nullopt to clear; an ID need not be currently visible.
 * @return Success, INVALID_ARGUMENT for a zero ID component, or a tree error.
 * @note No model lookup or loading occurs; removed IDs remain selected until explicitly cleared.
 */
template <class T>
std::expected<void, TreeError> setVirtualListSelection(WidgetTree &tree, Node<VirtualList<T>> node,
                                                       std::optional<RowId> selected);
/** @brief Validate settings on UI, invalidate layout and require preparation before the next paint.
 * @tparam T Immutable domain row type, as defined by VirtualList.
 * @param tree Borrowed UI owner; mutation inside a frame is rejected.
 * @param node Checked list observer.
 * @param options Owning replacement settings; row_height must be positive.
 * @return Success, INVALID_ARGUMENT for a nonpositive height, or a tree error.
 * @note The model's range budget is checked later by preparation; overscan is never silently capped.
 */
template <class T>
std::expected<void, TreeError> setVirtualListOptions(WidgetTree &tree, Node<VirtualList<T>> node,
                                                     VirtualListOptions options);
/** @brief Explicitly retry a provider error/pending range at the next preparation; no worker is started.
 * @tparam T Immutable domain row type, as defined by VirtualList.
 * @param tree Borrowed UI owner; mutation inside a frame is rejected.
 * @param node Checked list observer.
 * @return Success or a tree affinity, lifetime, frame or invalidation error.
 * @note Cancels the previous request and invalidates paint; preparation returns a fresh receipt.
 */
template <class T> std::expected<void, TreeError> retryVirtualList(WidgetTree &tree, Node<VirtualList<T>> node);
/** @brief Bind arrows/Home/End, left press and integral wheel scrolling to the existing router.
 * Caller retains the scoped binding. Tree/router outlive it; no widget pointer escapes dispatch.
 * @tparam T Immutable domain row type, as defined by VirtualList.
 * @param router Existing UI router; no additional queue or focus system is created.
 * @param tree The same borrowed tree used by router; it must outlive the registration.
 * @param node Checked list observer to register as focusable.
 * @return Owning disconnect token, or the router's registration/affinity/lifetime error.
 * @note Handled actions invalidate paint. Pending selection is resolved only by preparation.
 */
template <class T>
std::expected<ActionBinding, TreeError> bindVirtualList(EventRouter &router, WidgetTree &tree,
                                                        Node<VirtualList<T>> node);

/** @brief One UI-owned view with bounded owning row text and one independent model consumer.
 * Construct with a consumer of the frame's model. Delegate owns its captures and returns owning text;
 * it runs only during preparation and must not perform I/O, mutate models/tree, or retain row borrows.
 * Preparation is transactional on allocation/delegate failure. The retained frame and text survive
 * through render inside WidgetTree::frame. No row widgets, worker, hidden pin or terminal I/O exist.
 * Use tree-checked free mutations; read getters through tree.read. Destroy on the UI owner.
 * @tparam T Domain value exposed by ModelRow<T>. It need not be copyable/default-constructible;
 * this view only borrows const rows during its delegate. The provider must publish immutable
 * transitively owned values and stable nonzero RowId pairs. The copyable delegate owns captures,
 * returns GraphemeText with options.width_policy, and may throw; preparation reports those failures.
 */
template <class T> class VirtualList : public SizedWidget {
public:
  /** @brief Own consumer, delegate and options; validation happens on preparation.
   * @param consumer Independent move-only range slot, normally obtained from Model<T>::openRange().
   * @param delegate Owning copyable formatter; called only for visible ready rows, with no I/O/mutation.
   * @param options Owning height, overscan, width-policy and styles.
   * @param policy Owning layout constraints inherited by SizedWidget.
   * @note Construct through WidgetTree::emplace on UI; allocation/capture failures follow its errors.
   */
  VirtualList(ModelConsumer<T> consumer, std::function<GraphemeText(const ModelRow<T> &)> delegate,
              VirtualListOptions options = {}, SizePolicy policy = {})
      : SizedWidget(policy), consumer_(std::move(consumer)), delegate_(std::move(delegate)), options_(options) {
  }

  ~VirtualList() override {
    request_.cancel();
  } ///< Cancel work; worker ownership may outlive this view.

  /** @return Integral intended scroll position, clamped at the last successful preparation. */
  std::uint64_t firstRow() const noexcept {
    return first_;
  } ///< Integral scroll position, clamped at preparation.

  /** @return Owning stable ID, including an offscreen/removed ID, or nullopt when cleared. */
  std::optional<RowId> selection() const noexcept {
    return selected_;
  } ///< Stable identity, retained across model revisions and hidden views.

  /** @return Owning interval of the last prepared visible rows, or an empty interval before preparation. */
  RowRange visibleRange() const noexcept {
    return prepared_ ? prepared_->window.visible : RowRange{};
  } ///< Last successfully prepared visible rows.

  /** @return Owning viewport-plus-overscan interval; hidden/unprepared lists return an empty interval. */
  RowRange preparedRange() const noexcept {
    return prepared_ ? prepared_->window.requested : RowRange{};
  } ///< Last successfully prepared viewport plus overscan.

  /** @return Last prepared ready/pending/error state; an unprepared/hidden list reports pending. */
  RangeState rangeState() const noexcept {
    return prepared_ ? prepared_->range.state() : RangeState::PENDING;
  } ///< Last prepared model readiness; error text is owned by its retained model range.

  /** @param available Borrowed-by-value cell bounds; negative extents normalize to zero.
   * @return Normalized available extent without model access, formatting, allocation or invalidation.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override {
    return CellSize{std::max(0, available.width), std::max(0, available.height)};
  } ///< Viewport-sized intrinsic extent, independent of logical row count.

  /** @param local Owning parent-local rectangle supplied by layoutTree inside the frame.
   * @return Success after storing the local size; does not prepare rows or invalidate the tree.
   */
  std::expected<void, TreeError> arrange(CellRect local) override {
    arranged_ = local.size();

    return {};
  } ///< Remember local bounds without reading/loading/formatting rows.

  /** @param context Synchronous borrowed sink; rendered text must not outlive the enclosing tree.frame.
   * @return Success, WOULD_BLOCK for dirty/unprepared content, INVALID_GEOMETRY after resize,
   * or the sink error. Caller must discard partial commands on error.
   * @note UI frame only; no I/O, model reads, delegate calls or invalidation.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override {
    if (!prepared_ || dirty_) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    if (arranged_ != prepared_->viewport) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    return detail::paintList(context, arranged_, options_, prepared_->rows, selected_);
  } ///< Emit widget-local commands; caller applies surface translation and ancestor clip.

private:
  /// @cond INTERNAL
  template <class U>
  friend std::expected<std::optional<ModelRequest<U>>, TreeError> prepareVirtualList(WidgetTree &, Node<VirtualList<U>>,
                                                                                     const ModelFrame<U> &, CellSize);
  template <class U>
  friend std::expected<void, TreeError> setVirtualListFirst(WidgetTree &, Node<VirtualList<U>>, std::uint64_t);
  template <class U>
  friend std::expected<void, TreeError> setVirtualListSelection(WidgetTree &, Node<VirtualList<U>>,
                                                                std::optional<RowId>);
  template <class U>
  friend std::expected<void, TreeError> setVirtualListOptions(WidgetTree &, Node<VirtualList<U>>, VirtualListOptions);
  template <class U> friend std::expected<void, TreeError> retryVirtualList(WidgetTree &, Node<VirtualList<U>>);
  template <class U>
  friend std::expected<ActionBinding, TreeError> bindVirtualList(EventRouter &, WidgetTree &, Node<VirtualList<U>>);

  /// @endcond

  struct Prepared {
    Prepared(const ModelFrame<T> &owner, const ModelRange<T> &data, CellSize size, detail::ListWindow interval)
        : frame(owner), range(data), viewport(size), window(interval) {
    }

    ModelFrame<T> frame;
    ModelRange<T> range;
    CellSize viewport;
    detail::ListWindow window;
    std::vector<detail::ListRow> rows = std::vector<detail::ListRow>(std::initializer_list<detail::ListRow>{});
  };

  std::expected<std::optional<ModelRequest<T>>, TreeError> prepare(const ModelFrame<T> &frame, CellSize viewport,
                                                                   bool &changed) {
    auto window = detail::listWindow(frame.rowCount(), first_, viewport, options_);

    if (!window || !delegate_) {
      return std::unexpected(window ? TreeError::INVALID_ARGUMENT : window.error());
    }

    auto consumer_range = frame.range(consumer_.id());

    if (!consumer_range) {
      return std::unexpected(consumer_range.error());
    }

    if (prepared_ && !dirty_ && !retry_ && prepared_->frame.revision() == frame.revision() &&
        prepared_->viewport == viewport && prepared_->window.visible == window->visible &&
        (window->requested.count == 0 || prepared_->range.state() != RangeState::PENDING ||
         !request_.stopRequested())) {
      return std::optional<ModelRequest<T>>{};
    }

    try {
      auto range = std::move(consumer_range);

      if (window->requested.count == 0) {
        range = ModelRange<T>{};
      } else if (retry_ || range->range() != window->requested || range->state() == RangeState::PENDING) {
        range = frame.read(window->requested);
      }

      if (!range) {
        return std::unexpected(range.error());
      }

      // Stage all owning data before touching the visible state or issuing provider work.
      // A delegate may fail halfway through: frame, row identities and text still commit together.
      // Construct in place: MSVC Debug vector move is noexcept but may allocate an iterator proxy.
      auto next = std::make_unique<Prepared>(frame, *range, viewport, *window);
      auto selection = selected_;
      const bool apply_intent = intent_ && intent_revision_ == frame.dataRevision();

      next->rows.reserve(window->visible.count);

      for (std::size_t i = 0; i < window->visible.count; ++i) {
        const auto index = window->visible.first + i;

        if (range->state() == RangeState::READY) {
          const auto &row = range->rows()[static_cast<std::size_t>(index - window->requested.first)];
          auto text = delegate_(row);

          if (text.policy() != options_.width_policy || row.id.value == 0 || row.id.generation == 0) {
            return std::unexpected(TreeError::INVALID_ARGUMENT);
          }

          next->rows.push_back({row.id, std::move(text)});

          if (apply_intent && index == *intent_) {
            selection = row.id;
          }
        } else {
          auto text = GraphemeText::from(range->state() == RangeState::FAILED ? "!" : "...", options_.width_policy);

          if (!text) {
            return std::unexpected(TreeError::INVALID_ARGUMENT);
          }

          next->rows.push_back({{}, std::move(*text)});
        }
      }

      std::optional<ModelRequest<T>> dispatch;
      const bool needs_request = window->requested.count != 0 && (retry_ || range->state() == RangeState::PENDING) &&
                                 (retry_ || request_.stopRequested() || request_.range() != window->requested ||
                                  request_.dataRevision() != frame.dataRevision());

      // Work is requested only after every fallible formatting/allocation step succeeded.
      // Drop our cancelled lease before requesting, so a one-slot budget can recover after caller release.
      if (needs_request) {
        request_.cancel();
        request_ = {};

        auto request = consumer_.request(window->requested);

        if (!request) {
          return std::unexpected(request.error());
        }

        dispatch.emplace(*request);
        request_ = std::move(*request);
      } else if (range->state() != RangeState::PENDING || window->requested.count == 0) {
        request_.cancel();
        request_ = {};
      }

      // From this point the presentation commit is nonallocating. Old owners survive until all fields agree.
      prepared_.swap(next);
      changed = true;
      selected_ = selection;
      first_ = window->visible.first;
      dirty_ = false;
      retry_ = false;

      if (intent_ && (!apply_intent || range->state() == RangeState::READY)) {
        intent_.reset();
      }

      return dispatch;
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  void choose(std::uint64_t position) {
    // Intent is positional only within one data revision. Publication/reorder never rewrites a stable ID.
    const auto count = prepared_->window.visible.count;

    intent_ = position;
    intent_revision_ = prepared_->frame.dataRevision();

    if (position < first_) {
      first_ = position;
    } else if (count != 0 && position - first_ >= count) {
      first_ = position - count + 1;
    }

    dirty_ = true;
  }

  EventReply action(Action action) {
    if (!prepared_ || prepared_->frame.rowCount() == 0) {
      return EventReply::IGNORED;
    }

    auto position = intent_.value_or(first_);
    bool located = intent_.has_value();

    // Locate identities only in bounded visible delegates; global index lookup belongs to the provider.
    if (!located && selected_) {
      for (std::size_t i = 0; i < prepared_->rows.size(); ++i) {
        if (prepared_->rows[i].id == selected_) {
          position = prepared_->window.visible.first + i;
          located = true;

          break;
        }
      }
    }

    const auto last = prepared_->frame.rowCount() - 1;

    position = std::min(position, last);

    switch (action) {
    case Action::MOVE_UP:
      position -= located && position > 0 ? 1 : 0;

      break;
    case Action::MOVE_DOWN:
      position += located && position < last ? 1 : 0;

      break;
    case Action::MOVE_HOME:
      position = 0;

      break;
    case Action::MOVE_END:
      position = last;

      break;
    default:
      return EventReply::IGNORED;
    }

    choose(position);

    return EventReply::HANDLED;
  }

  ModelConsumer<T> consumer_;
  std::function<GraphemeText(const ModelRow<T> &)> delegate_;
  VirtualListOptions options_;
  std::unique_ptr<Prepared> prepared_;
  ModelRequest<T> request_;
  std::optional<RowId> selected_;
  std::optional<std::uint64_t> intent_;
  std::uint64_t intent_revision_{};
  std::uint64_t first_{};
  CellSize arranged_;
  bool dirty_{true};
  bool retry_{};
};

template <class T>
std::expected<std::optional<ModelRequest<T>>, TreeError>
prepareVirtualList(WidgetTree &tree, Node<VirtualList<T>> node, const ModelFrame<T> &frame, CellSize viewport) {
  std::expected<std::optional<ModelRequest<T>>, TreeError> result;
  bool changed{};
  auto shown = detail::listShown(tree, node);

  if (!shown) {
    return std::unexpected(shown.error());
  }

  auto edited = tree.edit(node, [&](VirtualList<T> &view) {
    if (!*shown) {
      view.request_.cancel();
      view.request_ = {};
      view.prepared_.reset();
      view.dirty_ = true;
      view.intent_.reset();

      return;
    }

    result = view.prepare(frame, viewport, changed);
  });

  if (!edited) {
    return std::unexpected(edited.error());
  }

  if (result && changed) {
    auto invalidated = tree.invalidate(node, Invalidation::PAINT);

    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }
  }

  return result;
}

template <class T>
std::expected<void, TreeError> setVirtualListFirst(WidgetTree &tree, Node<VirtualList<T>> node, std::uint64_t first) {
  auto edited = tree.edit(node, [&](VirtualList<T> &view) {
    if (view.first_ != first) {
      view.first_ = first;
      view.dirty_ = true;
      view.intent_.reset();
      view.request_.cancel();
    }
  });

  return edited ? tree.invalidate(node, Invalidation::PAINT) : edited;
}

template <class T>
std::expected<void, TreeError> setVirtualListSelection(WidgetTree &tree, Node<VirtualList<T>> node,
                                                       std::optional<RowId> selected) {
  if (selected && (selected->value == 0 || selected->generation == 0)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto edited = tree.edit(node, [&](VirtualList<T> &view) {
    view.selected_ = selected;
    view.intent_.reset();
  });

  return edited ? tree.invalidate(node, Invalidation::PAINT) : edited;
}

template <class T>
std::expected<void, TreeError> setVirtualListOptions(WidgetTree &tree, Node<VirtualList<T>> node,
                                                     VirtualListOptions options) {
  if (options.row_height <= 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto edited = tree.edit(node, [&](VirtualList<T> &view) {
    view.options_ = options;
    view.dirty_ = true;
    view.request_.cancel();
  });

  return edited ? tree.invalidate(node, Invalidation::LAYOUT) : edited;
}

template <class T> std::expected<void, TreeError> retryVirtualList(WidgetTree &tree, Node<VirtualList<T>> node) {
  auto edited = tree.edit(node, [&](VirtualList<T> &view) {
    view.retry_ = true;
    view.request_.cancel();
  });

  return edited ? tree.invalidate(node, Invalidation::PAINT) : edited;
}

template <class T>
std::expected<ActionBinding, TreeError> bindVirtualList(EventRouter &router, WidgetTree &tree,
                                                        Node<VirtualList<T>> node) {
  return router.bind(node, [&tree, node](VirtualList<T> &view, const RoutedEvent &event) {
    EventReply reply{EventReply::IGNORED};

    if (const auto *action = std::get_if<ActionEvent>(&event)) {
      reply = view.action(action->action);
    } else if (const auto *mouse = std::get_if<MouseInputEvent>(&event); mouse && view.prepared_) {
      auto snapshot = tree.inspect(node);

      if (!snapshot || !snapshot->geometry.valid || !snapshot->geometry.clip.contains({mouse->column, mouse->row})) {
        return EventReply::IGNORED;
      }

      if (mouse->action == MouseAction::PRESS && mouse->button == MouseButton::LEFT && !view.dirty_) {
        // Router coordinates are surface cells; translate using the current arranged and clipped geometry.
        const auto y = static_cast<std::int64_t>(mouse->row) - snapshot->geometry.surface.origin().y;
        const auto offset = static_cast<std::uint64_t>(y / view.options_.row_height);

        if (offset < view.prepared_->window.visible.count) {
          view.choose(view.prepared_->window.visible.first + offset);
          reply = EventReply::HANDLED;
        }
      } else if (mouse->action == MouseAction::SCROLL &&
                 (mouse->button == MouseButton::WHEEL_UP || mouse->button == MouseButton::WHEEL_DOWN)) {
        const auto count = view.prepared_->frame.rowCount();

        if (mouse->button == MouseButton::WHEEL_UP && view.first_ > 0) {
          --view.first_;
        } else if (mouse->button == MouseButton::WHEEL_DOWN && count > 0 && view.first_ < count - 1) {
          ++view.first_;
        }

        view.dirty_ = true;
        view.intent_.reset();
        view.request_.cancel();
        reply = EventReply::HANDLED;
      }
    }

    if (reply == EventReply::HANDLED) {
      (void)tree.invalidate(node, Invalidation::PAINT);
    }

    return reply;
  });
}
} // namespace portals
