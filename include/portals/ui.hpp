// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/line_edit_text.hpp>
#include <portals/detail/move_only_function.hpp>
#include <portals/grid.hpp>
#include <portals/managed.hpp>
#include <portals/ui_ref.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>
#include <ranges>
#include <tuple>
#include <typeinfo>

/// @brief Owning construction descriptions mounted through the existing ManagedScene lifecycle.
namespace portals::ui {
/** @brief Single-use owning construction recipe; no mounted widget ownership.
 * @tparam T Public Widget subtype constructed by the recipe, including final types.
 */
template <class T> class Description;

namespace detail {
/** @brief Recognize an owning recipe after cv/ref normalization.
 * @tparam T Type tested for an exact Description
 * specialization.
 */
template <class T> struct IsDescription : std::false_type {};

/** @brief Recognize the owning recipe specialization.
 * @tparam T Widget type described by this recipe.
 */
template <class T> struct IsDescription<Description<T>> : std::true_type {};

/** @brief Admit only consuming recipe arguments, rejecting implicit moves from lvalues.
 * @tparam T
 * Forwarding-reference argument type before normalization.
 */
template <class T>
concept DescriptionInput = IsDescription<std::remove_cvref_t<T>>::value && !std::is_lvalue_reference_v<T>;

/** @brief Admit immediately consumable recipes without draining borrowed lvalue elements.
 * @tparam R Input range producing Description values, explicit rvalue references, or an rvalue owning container.
 */
template <class R>
concept DescriptionRange =
  std::ranges::input_range<R> && IsDescription<std::ranges::range_value_t<R>>::value &&
  ((DescriptionInput<std::ranges::range_reference_t<R>> &&
    std::constructible_from<std::ranges::range_value_t<R>, std::ranges::range_reference_t<R>>) ||
   (!std::is_lvalue_reference_v<R> && !std::ranges::view<std::remove_cvref_t<R>> &&
    !std::ranges::enable_borrowed_range<std::remove_cvref_t<R>> &&
    std::same_as<std::ranges::range_reference_t<R>, std::ranges::range_value_t<R> &>));

/** @brief Preserve a construction failure without formatting its diagnostic.
 * @param node Observing target identity,
 * possibly empty before allocation.
 * @param cause Original tree error from the failed construction step.
 * @return
 * Mount-phase error with independent diagnostic fields initially empty.
 */
inline ManagedError error(Node<Widget> node, TreeError cause) {
  return {ManagedPhase::MOUNT, node.id(), cause, {}, {}};
}

/** @brief Preserve the standard text-control error mapping and diagnostic payload.
 * @param node Observing parent before text-control allocation.
 * @param cause Original semantic or resource-limit text error.
 * @return MOUNT error: NODE_LIMIT for text resource caps, otherwise INVALID_ARGUMENT, with unchanged text payload.
 */
inline ManagedError textError(Node<Widget> node, TextError cause) {
  const auto kind = cause.kind;
  const bool limited = kind == TextErrorKind::INPUT_LIMIT || kind == TextErrorKind::CLUSTER_LIMIT ||
                       kind == TextErrorKind::GRAPHEME_LIMIT || kind == TextErrorKind::CELL_LIMIT;
  auto failure = error(node, limited ? TreeError::NODE_LIMIT : TreeError::INVALID_ARGUMENT);

  failure.text = cause;

  return failure;
}

/** @param options Editor options under the receiving scene width policy; owned placeholder bytes are consumed.
 * @param parent Receiving parent identity retained on preparation failure.
 * @return Prepared presentation or exact checked mount error, before internal state publication.
 */
inline std::expected<portals::detail::LineEditPlaceholder, ManagedError> lineEditPlaceholder(LineEditOptions &options,
                                                                                             Node<Widget> parent) {
  if (!portals::detail::validLineEditDisplayMode(options.displayMode) ||
      (options.placeholder && portals::detail::hasLineSeparator(*options.placeholder))) {
    return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
  }
  auto prepared = portals::detail::prepareLineEditPlaceholder(options);
  if (!prepared) {
    return std::unexpected(textError(parent, prepared.error()));
  }
  return std::move(*prepared);
}

// Checked MSVC strings allocate iterator proxies in otherwise noexcept moves.
#if defined(_MSVC_STL_VERSION) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
inline constexpr bool CHECKED_STRING_MOVES = true; ///< Use stationary string captures only in the affected STL mode.
#else
inline constexpr bool CHECKED_STRING_MOVES = false; ///< Ordinary string moves retain their original construction cost.
#endif

/** @param value Text copied during capture; no borrowed view survives.
 * @return Stationary shared bytes only for checked MSVC, otherwise the ordinary owning string.
 */
inline auto captureText(std::string_view value) {
  if constexpr (CHECKED_STRING_MOVES) {
    return std::make_shared<const std::string>(value);
  } else {
    return std::string(value);
  }
}

/** @tparam T Exact owning string or shared string storage returned by captureText.
 * @param value Storage borrowed only during one synchronous recipe operation.
 * @return Bytes borrowed until the capture is replaced or destroyed.
 */
template <class T> std::string_view captionView(const T &value) {
  if constexpr (std::same_as<T, std::string>) {
    return value;
  } else {
    return *value;
  }
}

/** @brief One existing size-policy update, retained until checked mount. */
enum class SizeDimension {
  WIDTH,
  HEIGHT,
  FLEX_X,
  FLEX_Y,
  FLEX
};

/** @brief Owning compact size operation; validation remains in setSizePolicy. */
struct SizeChange {
  SizeDimension dimension; ///< Axis or axes receiving the change.
  std::int32_t extent{};   ///< Fixed cells for WIDTH/HEIGHT.
  std::uint32_t weight{1}; ///< Positive flex weight for FLEX operations.
};

/** @brief Stack-owned transaction bookkeeping, discarded after one description mount. */
struct MountContext {
  Node<Widget> root; ///< First created node; identifies the subtree to retire on failure.
  std::vector<Node<Widget>> nodes =
    std::vector<Node<Widget>>(0); ///< Created observers checked for liveness before publication.
  std::vector<portals::detail::MoveOnlyFunction<void()>> publish =
    std::vector<portals::detail::MoveOnlyFunction<void()>>(0); ///< Nonthrowing assignments publishing reserved refs.
  std::vector<portals::detail::MoveOnlyFunction<void()>> release =
    std::vector<portals::detail::MoveOnlyFunction<void()>>(0); ///< Nonthrowing releases of temporary ref reservations.

  /** @brief Same-attempt Ref reservation used by cold graph finalization before publication. */
  struct PendingRef {
    const RefTarget *identity{}; ///< Original unpublished slot identity; never dereferenced from another thread.
    RefTarget target;            ///< Checked private identity used only by cold finalization.
  };

  std::vector<PendingRef> pending = std::vector<PendingRef>(0); ///< Private same-attempt reservations.
  std::vector<portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>()>> finalize =
    std::vector<portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>()>>(
      0); ///< Pure cold graph completion.
  std::vector<portals::detail::MoveOnlyFunction<void()>> rollback =
    std::vector<portals::detail::MoveOnlyFunction<void()>>(
      0);           ///< Explicit attempt-owned resources, never numeric checkpoints.
  bool committed{}; ///< True only after every finalizer and node check succeeds.

  /** @brief Release attempt-owned resources and ref reservations after publication or rollback. */
  ~MountContext() {
    if (!committed) {
      for (auto &retire : rollback) {
        retire();
      }
    }
    for (auto &reservation : release) {
      reservation();
    }
  }
};

/** @brief Internal bridge to existing managed validation and private consuming recipes. */
struct Access {
  /** @param scene Owner of the nested mount transaction entered on this thread. */
  static void beginMount(ManagedScene &scene) noexcept {
    if (scene.owner_ == std::this_thread::get_id()) {
      ++scene.mountDepth_;
    }
  }

  /** @param scene Owner whose complete transaction, including Ref publication, has ended. */
  static void endMount(ManagedScene &scene) noexcept {
    if (scene.owner_ == std::this_thread::get_id()) {
      --scene.mountDepth_;
    }
  }

  /** @param scene Borrowed managed owner; observed only during UI-affine publication.
   * @return Weak token that expires before scene destruction releases registrations.
   */
  static std::weak_ptr<const void> lifetime(ManagedScene &scene) {
    return scene.lifetime_;
  }

  /** @brief Mount a root or child inside the same transaction.
   * @tparam T Public Widget subtype constructed by the
   * recipe.
   * @param description Owning recipe consumed by this call, including on failure.
   * @param scene
   * Existing UI-affine owner of all mounted nodes.
   * @param parent Live managed parent receiving the new child.
   *
   * @param context Borrowed transaction retaining observations and deferred ref publication.
   * @return Typed
   * observing node or the unchanged creation/setup/child error.
   */
  template <class T>
  static std::expected<Node<T>, ManagedError> mount(Description<T> &&description, ManagedScene &scene,
                                                    Node<Widget> parent, MountContext &context);

  /** @brief Wrap a checked managed factory without exposing recipe construction publicly.
   * @tparam T Widget subtype returned by the factory.
   * @tparam Factory Owning callable returning expected<Node<T>, ManagedError> for scene and parent.
   * @param factory Capture retained until the consuming mount; construction may throw.
   * @return Single-use description with the existing transaction semantics.
   */
  template <class T, class Factory> static Description<T> describe(Factory factory);

  /// @cond INTERNAL
  template <class T, class Factory> static Description<T> cold(Factory factory);

  static void beginCold(ManagedScene &scene) noexcept {
    ++scene.coldMountPending_;
  }

  struct ColdGraph {
    ManagedScene *scene{};
    std::shared_ptr<ManagedScene::LiveTextRecord> live;
    std::shared_ptr<void> output;
    std::shared_ptr<ManagedScene::ObserverRecord> observer;
    std::shared_ptr<portals::detail::ManagedModelSource> model;
    bool pending{};

    void retire() noexcept {
      // The complete logical mark precedes disconnect/destructor reentry.
      if (pending) {
        --scene->coldMountPending_;
        pending = false;
      }
      if (observer) {
        observer->retired = true;
      }
      if (observer) {
        observer->subscription = {};
      }
      if (model) {
        std::erase(scene->models_, model);
      }
      if (observer) {
        std::erase(scene->observers_, observer);
      }
      if (output) {
        std::erase(scene->properties_, output);
        std::erase_if(scene->originProperties_, [&](const auto &record) {
          return record.property.lock() == output;
        });
      }
      if (live) {
        live->origin->retired = true;
        scene->pruneLiveText();
      }
    }
  };

  static std::expected<void, ManagedError> completeTextList(ManagedScene &scene, Node<VirtualList<TextSegment>> node,
                                                            const TextSource &source, bool splitWords,
                                                            TextSplitOptions options, MountContext &context,
                                                            const std::shared_ptr<ColdGraph> &graph,
                                                            ManagedViewHandler<TextSegment> prepared) {
    auto target = source.target_;
    for (const auto &pending : context.pending) {
      if (pending.identity == target.get()) {
        target = std::make_shared<RefTarget>(pending.target);
        break;
      }
    }
    if (target && target->owner != std::this_thread::get_id()) {
      return std::unexpected(error({}, TreeError::WRONG_THREAD));
    }
    if (!target) {
      return std::unexpected(error({}, TreeError::CLOSED));
    }
    const bool existed = std::ranges::any_of(scene.liveText_, [&](const auto &record) {
      return record->origin->node == target->node && record->kind == static_cast<int>(source.kind_);
    });
    auto resolved = TextSource(target, source.kind_);
    auto live = scene.attachLiveText(resolved, PropertyOptions{}, std::source_location::current(), false);
    if (!live) {
      return std::unexpected(live.error());
    }
    const auto existing = std::ranges::find_if(scene.liveText_, [&](const auto &record) {
      return record->origin == live->state_.origin_;
    });
    // Ownership is assigned by the finalizer that created the bridge, not by every shared consumer.
    if (!existed && existing != scene.liveText_.end()) {
      graph->live = *existing;
    }
    auto initialText = live->snapshot();
    if (!initialText) {
      return std::unexpected(error(target->node, initialText.error()));
    }
    if ((*initialText)->value.policy() != scene.widthPolicy()) {
      return std::unexpected(error(target->node, TreeError::INVALID_ARGUMENT));
    }
    auto admitted = scene.observerAdmission(node);
    if (!admitted) {
      return std::unexpected(admitted.error());
    }
    auto textFailure = std::make_shared<std::optional<TextError>>();
    auto derived = scene.derive(*live, [splitWords, options, textFailure](const GraphemeText &text) {
      *textFailure = portals::detail::textSplitLimit(text, options.limits);
      return splitWords ? portals::words(text, options) : portals::graphemes(text, options);
    });
    if (!derived) {
      auto failure = derived.error();
      failure.node = target->node.id();
      failure.text = *textFailure;
      return std::unexpected(failure);
    }
    graph->output = *derived->state_.lock();
    graph->observer = scene.observers_.back();
    graph->observer->textFailure = std::move(textFailure);
    graph->observer->operation = splitWords ? "text.words" : "text.graphemes";
    graph->observer->node = node;
    scene.originProperties_.push_back({live->state_.origin_, graph->output, node});
    // Private cold edges share origin retirement; ordinary public derived outputs remain independent.
    derived->state_.origin_ = live->state_.origin_;
    auto model =
      scene.collectionModel<TextSegment>(derived->state_, CollectionModelOptions{}, portals::detail::segmentRows);
    if (!model) {
      return std::unexpected(model.error());
    }
    auto owned = model->lock();
    if (!owned) {
      return std::unexpected(error(node, owned.error()));
    }
    graph->model = *owned;
    graph->model->receiver = node;
    auto range = (*owned)->model->openRange();
    if (!range) {
      return std::unexpected(error(node, range.error()));
    }
    auto installed = scene.app_.tree().edit(node, [&](VirtualList<TextSegment> &view) {
      view.consumer_ = std::move(*range);
    });
    if (!installed) {
      return std::unexpected(error(node, installed.error()));
    }
    auto attached = scene.attachView(
      node, *owned,
      [&scene, node](const ModelFrame<TextSegment> &frame, const LayoutSnapshot &layout) {
        return prepareVirtualList(scene.app_.tree(), node, frame, layout);
      },
      std::move(prepared));
    if (attached && graph->pending) {
      --scene.coldMountPending_;
      graph->pending = false;
    }
    return attached ? std::expected<void, ManagedError>{} : std::unexpected(attached.error());
  }

  /// @endcond

  /** @brief Build the shared checked data-presentation recipe for public overloads.
   * @tparam Content Retained body widget type.
   * @tparam Initial Owning presentation value or same-scene managed source.
   * @param initial Initial value or source retained by the consuming recipe.
   * @param content Single-use body recipe mounted in the same transaction.
   * @param config Owning construction-only labels, styles and retry action.
   * @param automatic Read the virtual body's prepared status when true.
   * @return Single-use composition preserving mount rollback and deferred reference publication.
   */
  template <class Content, class Initial>
  static Description<DataStateView> dataState(Initial initial, Description<Content> content, DataStateConfig config,
                                              bool automatic);

  /** @tparam Factory Owning checked callable accepting scene, parent and the shared mount context.
   * @param factory Internal adapter performing a complete existing recipe transaction.
   * @return Single-use erased recipe; no extra widget or lifecycle owner is created.
   */
  template <class Factory> static Description<Widget> transaction(Factory factory);

  /** @brief Erase only the observer type while preserving one mount transaction.
   * @tparam T Concrete public widget type produced by the existing recipe.
   * @param description Owning typed recipe consumed once.
   * @return Single-use recipe sharing rollback and deferred reference publication.
   */
  template <class T> static Description<Widget> erase(Description<T> description);

  /** @param scene Existing managed owner receiving a checked policy update.
   * @param node Live sized target created by this mount transaction.
   * @param change Owning operation; other axes and unrelated bounds are preserved.
   * @return Original managed boundary/tree/validation error, or success.
   */
  static std::expected<void, ManagedError> resize(ManagedScene &scene, Node<SizedWidget> node, SizeChange change) {
    const auto allowed = scene.boundary(ManagedPhase::MOUNT, node);

    if (!allowed) {
      return allowed;
    }

    SizePolicy policy;
    auto &tree = scene.app_.tree();
    const auto read = tree.read(node, [&](const SizedWidget &widget) {
      policy = widget.sizePolicy();
    });

    if (!read) {
      return std::unexpected(error(node, read.error()));
    }

    if (change.dimension == SizeDimension::WIDTH) {
      policy.width = {change.extent, change.extent, change.extent, SizeMode::FIXED, 1};
    } else if (change.dimension == SizeDimension::HEIGHT) {
      policy.height = {change.extent, change.extent, change.extent, SizeMode::FIXED, 1};
    } else {
      if (change.dimension != SizeDimension::FLEX_Y) {
        policy.width.mode = SizeMode::FLEX;
        policy.width.preferred = 0;
        policy.width.weight = change.weight;
      }

      if (change.dimension != SizeDimension::FLEX_X) {
        policy.height.mode = SizeMode::FLEX;
        policy.height.preferred = 0;
        policy.height.weight = change.weight;
      }
    }

    const auto changed = setSizePolicy(tree, node, policy);

    return changed ? std::expected<void, ManagedError>{} : std::unexpected(error(node, changed.error()));
  }

  /** @brief Verify a created node remains accessible before publishing references.
   * @param scene Existing managed
   * owner, borrowed for this check.
   * @param node Observing identity whose tree scope is validated.
   * @return
   * Success or the original managed boundary/liveness error.
   */
  static std::expected<void, ManagedError> check(ManagedScene &scene, Node<Widget> node) {
    return scene.boundary(ManagedPhase::MOUNT, node);
  }
};
} // namespace detail

/** @tparam Child Consuming Description type, including layouts and final custom widgets.
 * @param child Owning single child transferred into this same mount transaction.
 * @param options Owning glyph selection and finite allocated-perimeter paint cap.
 * @param style Owning manual appearances beneath theme declarations.
 * @return Owning frame recipe; errors roll back the entire subtree without partial ref publication.
 */
template <detail::DescriptionInput Child>
Description<Border> border(Child &&child, BorderOptions options = {}, ControlStyle style = {});

/** @tparam Child Consuming ordinary content recipe.
 * @param child Single content root, mounted in the same rollback transaction.
 * @param options Owning placement/input options.
 * @param controller Shared values; empty creates default values automatically at mount.
 * @param style Manual appearance defaults beneath themes.
 * @return Automatically initialized, bound and prepared ordinary scroll owner.
 */
template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Child &&child, ScrollAreaOptions options = {},
                                   std::shared_ptr<ScrollController> controller = {}, ControlStyle style = {});

/** @brief Shared observing slot published only after a complete successful description mount.
 * @tparam T Observed public Widget subtype; no widget or scene ownership is retained.
 * Read/write the slot only on the UI thread. Copies share the slot. A published identity is
 * never automatically reset; checked tree access detects later retirement. Use a new slot
 * for another description. Construction can throw std::bad_alloc.
 */
template <class T> class NodeRef {
public:
  /** @brief Allocate an empty observing slot, with no widget ownership. */
  NodeRef() : slot_(std::make_shared<Slot>()) {
  }

  /** @return Empty before successful mount, otherwise the observing identity, possibly stale. */
  Node<T> node() const noexcept {
    return slot_ ? slot_->node : Node<T>{};
  }

private:
  friend struct detail::NamedAccess;
  friend class Description<T>;
  template <class U> friend class Ref;

  /** @brief Shared identity storage; never owns a widget or its scene. */
  struct Slot {
    Node<T> node;             ///< Published identity, empty until the complete mount succeeds.
    bool pending{};           ///< Reserved by an in-progress mount, including across reentrant calls.
    detail::RefTarget target; ///< Weak scene context for the additive fluent reference.
    bool fluent{};            ///< Only fluent references enforce their immutable construction-thread identity.
  };

  std::shared_ptr<Slot> slot_;
};

/** @brief Additive typed observation with fluent caption/draft access, sharing NodeRef publication.
 * @tparam T Public Widget subtype; text() is available for Control subtypes and exactly LineEdit.
 * Construct on the receiving UI thread. Publish with Description::reference only after successful mount.
 * Copies share the observation and never own the scene/widget. Copy/destruction may occur on other
 * threads after publication, but operations reject them before reading mutable scene/slot state.
 * Synchronize transfer; publication must not race foreign-thread use. The low-level node() remains UI-only.
 * Construction allocates and can throw; moved-from references produce an empty text observation.
 */
template <class T> class Ref : public NodeRef<T> {
public:
  /** @brief Allocate an empty observing slot on the UI thread; may throw std::bad_alloc. */
  Ref() {
    this->slot_->fluent = true;
  }

  /** @param axis Selected logical axis.
   * @param offset Exact logical offset, clamped to the current range.
   * @param origin Command site retained by the scene collector.
   * @return Checked programmatic command or reported affinity/liveness/lease error.
   */
  std::expected<void, ManagedError> setOffset(ScrollAxis axis, ScrollOffset offset,
                                              std::source_location origin = std::source_location::current()) const
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return detail::scrollOffset(target(), axis, offset, origin);
  }

  /** @param metrics Exact standalone extent and viewport.
   * @param origin Command site retained by the scene collector.
   * @return Checked metrics command; an area projection is not a metrics writer.
   */
  std::expected<void, ManagedError> setMetrics(ScrollAxisMetrics metrics,
                                               std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, ScrollBar>
  {
    const auto observation = target();
    auto owner = detail::eventScene(observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    auto result = (*owner)->setScrollMetrics(this->node(), metrics);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result;
  }

  /** @param origin Request site retained by the scene collector.
   * @return Checked one-shot reveal admission; final rendering remains asynchronous.
   */
  std::expected<void, ManagedError> ensureVisible(std::source_location origin = std::source_location::current()) const {
    return detail::ensureVisible(target(), origin);
  }

  /** @return Owning final local/surface/clip geometry, or checked affinity/liveness/reentry failure.
   * Before final layout, while hidden, or after invalidation/rearrangement returns INVALID_GEOMETRY.
   * Does not perform layout or report errors; copies never borrow widgets or the scene.
   */
  std::expected<NodeGeometry, ManagedError> geometry() const {
    return detail::geometry(target());
  }

  /** @return Owning final local size or the exact geometry() error; never performs layout. */
  std::expected<CellSize, ManagedError> size() const {
    return geometry().transform([](const NodeGeometry &value) {
      return value.local.size();
    });
  }

  /** @tparam Callback Owning compact action receiving const NodeGeometry& or no arguments.
   * @param callback Void, checked TreeError or EditorCommandResult action retained by the scene.
   * @param origin Registration site retained in diagnostics.
   * @return Scene-owned registration or checked admission/budget/allocation error.
   * Initial valid and changed local/surface/clip bounds arrive on a subsequent UI dispatch.
   * Removal/close retires captures; an executing capture survives self-removal.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, NodeGeometry>
  std::expected<void, ManagedError> onLayout(Callback callback,
                                             std::source_location origin = std::source_location::current()) const {
    auto observation = target();
    auto owner = detail::eventScene(observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        return (*owner)->onLayout(this->node(), std::move(callback), origin);
      } catch (const std::bad_alloc &) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::NO_MEMORY, {}, {}});
      } catch (...) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::CALLBACK_FAILED, {}, {}});
      }
    }();
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result;
  }

  /** @tparam Callback Owning compact click action, optionally taking const ClickEvent&.
   * @param action Void, expected<Value, TreeError> or exact EditorCommandResult callback; success values are ignored.
   * @param origin Registration call site retained in diagnostics.
   * @return Checked registration on a mounted live button; CLOSED before publication/destruction,
   * WRONG_THREAD, STALE_NODE, duplicate INVALID_ARGUMENT or allocation error. Scene owns captures/token.
   * This adds one binding; it never replaces an existing one. Admitted calls survive self-removal.
   * Registration failures are also recorded in a live UI collector, without stopping the scene.
   * Internal callback transfers are checked; constructing the by-value argument before entry may throw.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::same_as<T, Button> && portals::detail::ManagedAction<Callback, ClickEvent>)
  std::expected<void, ManagedError> onClick(Callback action,
                                            std::source_location origin = std::source_location::current()) const {
    detail::commandOrigin(action, origin);
    auto observation = target();
    auto scene = detail::eventScene(observation);

    if (!scene) {
      return std::unexpected(scene.error());
    }

    // The forwarding move into the checked scene API is still part of this boundary.
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        return (*scene)->onClick(this->node(), std::move(action));
      } catch (const std::bad_alloc &) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::NO_MEMORY, {}, {}});
      } catch (...) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::CALLBACK_FAILED, {}, {}});
      }
    }();

    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }

    return result;
  }

  /** @param value Owning explicit presentation for an own-value composite; older generations are ignored.
   * @param origin Public call site for the existing short diagnostic collector.
   * @return Original checked TreeError; bound/automatic composites reject writes with INVALID_ARGUMENT.
   */
  std::expected<void, TreeError> display(DataPresentation value,
                                         std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, DataStateView>
  {
    return detail::displayDataState(target(), value, origin);
  }

  /** @param origin Focus request call site retained in managed diagnostics.
   * @return Checked focus/entry result; CLOSED before publication/after scene destruction,
   * WRONG_THREAD, STALE_NODE, typed eligibility or callback failure. No lifetime is extended.
   * Programmatic focus preserves the same editor blur and validation policies as keyboard focus.
   */
  std::expected<void, ManagedError> focus(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::focus, origin);
  }

  /** @param origin Focus-entry call site retained in managed diagnostics.
   * @return Checked focus-only scope entry, saving a return record; no input trap is installed.
   * Published callback errors retain entry and can be followed by leaveFocusScope().
   */
  std::expected<void, ManagedError>
  enterFocusScope(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::enterFocusScope, origin);
  }

  /** @param origin Focus-return call site retained in managed diagnostics.
   * @return Checked top-scope return; stale/unpublished/foreign-thread errors follow focus().
   * Owned failures are reported once, allowing use in ordinary void button callbacks.
   */
  std::expected<void, ManagedError>
  leaveFocusScope(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::leaveFocusScope, origin);
  }

  /** @param resolver Optional owning synchronous adapter used only for i18n expressions.
   * @param origin Property acquisition call site used by assignment/+= diagnostics.
   * @return Owning observing proxy, valid after Ref destruction; never extends scene/widget lifetime.
   * Resolver capture/storage allocation may throw before a mutation starts.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const
    requires(std::derived_from<T, Control> || std::same_as<T, LineEdit>)
  {
    auto target = this->slot_ ? std::shared_ptr<detail::RefTarget>(this->slot_, &this->slot_->target) : nullptr;
    constexpr std::string_view TYPE = std::same_as<T, LineEdit>   ? "LineEdit"
                                      : std::same_as<T, Label>    ? "Label"
                                      : std::same_as<T, Button>   ? "Button"
                                      : std::same_as<T, CheckBox> ? "CheckBox"
                                                                  : "Control";

    return TextProperty(std::move(target), std::move(resolver), TYPE, origin);
  }

  /** @param change Owning policy override for every mutation of this proxy; does not change editor options.
   * @param resolver Optional owning synchronous resource adapter.
   * @param origin Property acquisition call site retained in diagnostics.
   * @return Observing editor proxy; RESET_BASELINE also clears equal-text history without redraw.
   */
  TextProperty text(ProgrammaticChange change, TextResolver resolver = {},
                    std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return TextProperty(target(), std::move(resolver), "LineEdit", origin, change);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked Undo result, with exact reporting provenance for managed button adapters.
   */
  EditorCommandResult undo(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, false, origin);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked Redo result; empty history is a no-op without callbacks or redraw.
   */
  EditorCommandResult redo(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), true, false, origin);
  }

  /** @brief Clear the visible draft as one user operation, applying the configured EditMode.
   * @param origin Command call site retained in diagnostics.
   * @return Checked technical result with reporting provenance; semantic validation/conflict uses LineEditResult.
   * Empty writable drafts are no-ops. Read-only/disabled editors return WOULD_BLOCK. Return the complete
   * EditorCommandResult from managed callbacks to preserve report-once behavior.
   */
  EditorCommandResult clear(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, false, origin, true);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked grouping boundary; does not change text or presentation.
   */
  EditorCommandResult stopGrouping(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, true, origin);
  }

  /** @return Read-only visible caption source; acquisition never reads or subscribes. */
  TextSource captionText() const
    requires std::derived_from<T, Control>
  {
    return TextSource(target(), TextSource::Kind::CAPTION);
  }

  /** @return Deferred physical body-row source; acquisition never reads or subscribes.
   * Retains the original observing slot, including when acquired before mount, without owning the scene.
   */
  auto visibleModel() const
    requires requires { typename detail::TableRowType<T>::Type; }
  {
    using Row = typename detail::TableRowType<T>::Type;
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;

    return VisibleModelSource<Row>(target(), std::move(node));
  }

  /** @return Read-only visible draft source, including uncommitted or semantically rejected text. */
  TextSource draftText() const
    requires std::same_as<T, LineEdit>
  {
    return TextSource(target(), TextSource::Kind::DRAFT);
  }

  /** @return Read-only latest accepted model source, independent of the editor's visible draft. */
  TextSource committedText() const
    requires std::same_as<T, LineEdit>
  {
    return TextSource(target(), TextSource::Kind::COMMITTED);
  }

  /** @return Effective checked availability without adoption, invalidation or error reporting. */
  std::expected<HistoryAvailability, TreeError> historyAvailability() const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorAvailability(target());
  }

  /** @return Effective Undo availability; false on checked errors without reporting. */
  bool canUndo() const
    requires std::same_as<T, LineEdit>
  {
    const auto value = historyAvailability();

    return value && value->canUndo;
  }

  /** @return Effective Redo availability; false on checked errors without reporting. */
  bool canRedo() const
    requires std::same_as<T, LineEdit>
  {
    const auto value = historyAvailability();

    return value && value->canRedo;
  }

private:
  friend struct detail::ClickAccess;
  friend struct detail::LayoutAccess;
  friend struct detail::NamedAccess;
  friend class DynamicRef;

  std::expected<void, ManagedError>
  focusOperation(std::expected<void, ManagedError> (ManagedScene::*operation)(Node<Widget>),
                 std::source_location origin) const {
    auto observation = target();
    auto owner = detail::eventScene(observation);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    auto result = ((*owner)->*operation)(this->node());

    if (!result) {
      detail::reportFocusError(observation, result.error(), origin);
    }

    return result;
  }

  std::shared_ptr<detail::RefTarget> target() const {
    return this->slot_ ? std::shared_ptr<detail::RefTarget>(this->slot_, &this->slot_->target) : nullptr;
  }
};

namespace detail {
/** @brief Internal checked named publication bridge; no widget or scene ownership escapes. */
struct NamedAccess {
  /** @param stage Existing owning style payload.
   * @param target Weak exact-node observation.
   * @return Independent scoped stage using the same existing style implementation.
   */
  static StyleStage scopeStyle(const StyleStage &stage, std::shared_ptr<RefTarget> target);

  /** @param reference Dynamic observation.
   * @return Shared weak target with no scene ownership.
   */
  static std::shared_ptr<RefTarget> target(const DynamicRef &reference);

  /** @param scene Receiving managed owner.
   * @param target Shared observation, checked before reading mutable fields.
   * @return Exact node scope or typed thread/owner/liveness failure.
   */
  static std::expected<NodeId, ManagedError> scope(ManagedScene &scene, const std::shared_ptr<RefTarget> &target) {
    auto owner = eventScene(target);

    if (!owner) {
      auto failure = owner.error();
      failure.phase = ManagedPhase::STYLE;

      return std::unexpected(failure);
    }

    if (*owner != &scene) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, target->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }

    auto live = scene.boundary(ManagedPhase::STYLE, target->node);

    return live ? std::expected<NodeId, ManagedError>(target->node.id()) : std::unexpected(live.error());
  }

  /** @param scene Receiving managed owner.
   * @param source Original dynamic observation, never resolved again by name.
   * @return Original live identity, or exact prior lookup/type/liveness/owner error.
   */
  static std::expected<Node<Widget>, ManagedError> observe(ManagedScene &scene,
                                                           const std::shared_ptr<RefTarget> &source) {
    auto owner = eventScene(source);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, source->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }

    auto allowed = scene.boundary(ManagedPhase::BIND, source->node);

    return allowed ? std::expected<Node<Widget>, ManagedError>(source->node) : std::unexpected(allowed.error());
  }

  /** @param scene Borrowed live managed owner.
   * @param name Case-sensitive ID; empty, absent and duplicate names reject.
   * @return Unique live managed identity, or exact boundary/name error.
   */
  static std::expected<Node<Widget>, ManagedError> find(ManagedScene &scene, std::string_view name) {
    auto allowed = scene.boundary(ManagedPhase::BIND, scene.root_);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    Node<Widget> found;

    for (const auto &entry : scene.entries_) {
      if (entry->removed || !entry->target || entry->target->id != name || name.empty()) {
        continue;
      }

      auto live = scene.app_.tree().read(entry->node, [](const Widget &) {
      });

      if (!live && (live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED)) {
        continue;
      }

      if (!live) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, entry->node.id(), live.error(), {}, {}});
      }

      if (found != Node<Widget>{}) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, entry->node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }

      found = entry->node;
    }

    if (found == Node<Widget>{}) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    }

    return found;
  }

  /** @tparam T Public Widget subtype checked with RTTI before constructing its observer.
   * @param reference Empty shared observing slot to publish.
   * @param scene Existing owner, borrowed only during checked validation.
   * @param node Unique name result; no unchecked downcast is exposed.
   * @return Success after one publication or INVALID_ARGUMENT for a wrong type.
   */
  template <class T>
  static std::expected<void, ManagedError> publish(const Ref<T> &reference, ManagedScene &scene, Node<Widget> node) {
    auto &slot = *reference.slot_;
    bool compatible{};
    Node<Control> caption;
    Node<LineEdit> draft;
    auto checked = scene.app_.tree().read(node, [&](const Widget &widget) {
      if constexpr (std::same_as<T, Widget>) {
        compatible = true;
      } else {
        compatible = dynamic_cast<const T *>(&widget) != nullptr;
      }

      if (dynamic_cast<const Control *>(&widget)) {
        caption = Node<Control>(node.id());
      }

      if (dynamic_cast<const LineEdit *>(&widget)) {
        draft = Node<LineEdit>(node.id());
      }
    });

    if (!checked || !compatible) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, node.id(), checked ? TreeError::INVALID_ARGUMENT : checked.error(), {}, {}});
    }

    slot.node = Node<T>(node.id());
    slot.target.scene = &scene;
    slot.target.lifetime = scene.lifetime_;
    slot.target.node = node;
    slot.target.caption = caption;
    slot.target.draft = draft;

    return {};
  }

  /** @tparam T Observed public widget type.
   * @param reference Shared slot retaining this request's original failure.
   * @param failure Exact failure copied without widget access.
   */
  template <class T> static void reject(const Ref<T> &reference, const ManagedError &failure) {
    reference.slot_->target.failure = std::make_shared<const ManagedError>(failure);
  }

  /** @param scene Existing collector.
   * @param failure Exact cause stamped with short-error provenance.
   * @param source Owned name and static expected-type diagnostic.
   */
  static void report(ManagedScene &scene, ManagedError &failure, const LookupSource &source) {
    scene.reportShortError(failure, "scene.get", source.name, source.expectedType, source.origin);
  }

  /** @tparam T Observed widget subtype.
   * @param reference Existing typed observation.
   * @return Shared weak target, never scene or widget ownership.
   */
  template <class T> static std::shared_ptr<RefTarget> target(const Ref<T> &reference) {
    return reference.target();
  }

  /** @tparam T Public Widget subtype required by this request.
   * @param request Name and weak facade identity, copied through fallible owning transfers.
   * @return Typed slot with deferred publication or its retained admission failure.
   */
  template <class T> static Ref<T> request(const LookupSource &request) {
    Ref<T> reference;
    LookupSource source(request);

    source.expectedType = typeid(T).name();

    auto result =
      requestLookup(source, [reference](ManagedScene &scene, std::expected<Node<Widget>, ManagedError> node) {
        auto published =
          node ? publish(reference, scene, *node) : std::expected<void, ManagedError>(std::unexpected(node.error()));

        if (!published) {
          reject(reference, published.error());
        }

        return published;
      });

    if (!result) {
      reject(reference, result.error());
    }

    return reference;
  }
};
} // namespace detail

/** @brief Dynamic named observation with checked caption/editor and Button capabilities.
 * Copies own the request name and observing slot; they never retain a scene/widget.
 * Converting to Ref<T> checks the same published NodeId, or waits for this slot before mount.
 */
class DynamicRef {
public:
  /** @tparam T Public Widget subtype tested during deferred/immediate name resolution.
   * @return Typed weak slot; wrong type records INVALID_ARGUMENT before startup or in the live collector.
   * Conversion construction may allocate. A successful slot never retargets on later ID reuse.
   */
  template <class T>
    requires std::derived_from<T, Widget>
  operator Ref<T>() const {
    auto source = source_;

    source.observation = detail::NamedAccess::target(reference_);

    return detail::NamedAccess::request<T>(source);
  }

  /** @param resolver Optional owning synchronous localization adapter.
   * @param origin Property acquisition call site retained for short mutations.
   * @return Existing checked text proxy; non-text widgets return INVALID_ARGUMENT.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const {
    return TextProperty(reference_.target(), std::move(resolver), "Widget", origin);
  }

  /** @tparam Callback Owning compact Button action accepted by Ref<Button>::onClick.
   * @param callback Action transferred into the existing managed binding.
   * @param origin Registration call site.
   * @return Checked Button registration result; use a pipe stage for registration before preparation.
   */
  template <class Callback = detail::QuitCommand>
    requires portals::detail::ManagedAction<Callback, ClickEvent>
  std::expected<void, ManagedError> onClick(Callback callback,
                                            std::source_location origin = std::source_location::current()) const {
    Ref<Button> typed = *this;

    return typed.onClick(std::move(callback), origin);
  }

  /** @return Observing published identity, empty before mount; UI-thread only. */
  Node<Widget> node() const noexcept {
    return reference_.node();
  }

private:
  friend class Scene;
  friend struct detail::NamedAccess;

  explicit DynamicRef(const detail::LookupSource &source)
      : source_(source), reference_(detail::NamedAccess::request<Widget>(source_)) {
  }

  detail::LookupSource source_;
  Ref<Widget> reference_;
};

inline std::shared_ptr<detail::RefTarget> detail::NamedAccess::target(const DynamicRef &reference) {
  return reference.reference_.target();
}

// The public contract is documented on Scene::get in ui_scene.hpp.
template <class T>
  requires std::derived_from<T, Widget>
Ref<T> Scene::get(std::string_view name, std::source_location origin) {
  return detail::NamedAccess::request<T>({ref().anchor_, owner_, std::string(name), origin});
}

namespace detail {
/** @brief Checked adapter to managed-owned layout observations; never owns a widget or scene. */
struct LayoutAccess {
  /** @tparam T Observed widget type.
   * @tparam Callback Owning compact geometry action.
   * @param scene Existing receiving managed owner.
   * @param reference Immutable-affinity checked observing reference.
   * @param callback Action transferred after scene identity admission.
   * @param origin Original registration site.
   * @return Success or exact checked admission/registration failure.
   */
  template <class T, class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<T> &reference, Callback callback,
                                                  std::source_location origin) {
    auto owner = eventScene(reference.target());
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }
    return scene.onLayout(reference.node(), std::move(callback), origin);
  }
};

/** @brief Checked adapter to the managed-owned click chain; never owns a widget or scene. */
struct ClickAccess {
  /** @tparam Callback Owning no-argument or ClickParams callback with a managed action result.
   * @param scene Existing owner in which the reference must have been published.
   * @param reference Observing button identity, read only after immutable affinity admission.
   * @param callback Owning callable transferred into the bounded managed registry.
   * @param origin Original registration site retained for callback failures.
   * @return Success or exact affinity, identity, registration or allocation error.
   */
  template <class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<Button> &reference, Callback callback,
                                                  std::source_location origin) {
    commandOrigin(callback, origin);
    auto owner = eventScene(reference.target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    try {
      return scene.composeButtonClick(reference.node(), scene.clickAdapter(std::move(callback)), origin);
    } catch (const std::bad_alloc &) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::NO_MEMORY, {}, {}});
    } catch (...) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::CALLBACK_FAILED, {}, {}});
    }
  }
};

/** @brief Admit callbacks borrowing the mutable chain context, or ordinary no-argument callbacks.
 * @tparam Callback Copyable callable whose result is void, checked TreeError or an editor receipt.
 */
template <class Callback>
concept ClickCallback = std::copy_constructible<Callback> && (requires(Callback &callback, ClickParams &params) {
                          { callback(params) } -> portals::detail::ManagedActionResult;
                        } || (!std::invocable<Callback &, ClickParams &> && requires(Callback &callback) {
                                                                { callback() } -> portals::detail::ManagedActionResult;
                                                              }));
} // namespace detail

/** @brief Owning setup action invoked once after construction, before children.
 * @tparam T Concrete Widget type observed by the action.
 * Arguments are borrowed for the invocation. External effects and separately created scene
 * state are not rolled back; the attempted description subtree is removed on failure.
 */
template <class T>
using Setup = portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<T>)>;

/** @brief Owning optional editor customization and confirmation policy. */
struct LineEditConfig {
  std::function<std::expected<void, TreeError>(LineEditResult)> result{}; ///< Optional callback after standard editing.
  LineEditOptions options{}; ///< Editor limits and appearances; scene width policy overrides options.widthPolicy.
  SizePolicy policy{};       ///< Owning layout constraints validated by the existing layout engine.
  std::shared_ptr<Clipboard> clipboard{};           ///< Optional UI-owned clipboard adapter retained by the binding.
  std::function<bool(std::string_view)> validate{}; ///< Optional publication validator; false retains visible draft.
};

/** @brief Move-only recipe whose strings, arguments, children and callbacks survive their sources.
 * @tparam T Public Widget subtype. Factories enforce constructor compatibility when instantiated.
 * Factory/modifier allocations and capture construction may throw before mount begins. Mount
 * translates internal exceptions, consumes the entire recipe even on failure, and transfers
 * registered callbacks to the existing ManagedScene. Destruction never changes mounted UI.
 */
template <class T> class Description {
public:
  /** @brief Construct an empty recipe; mounting it returns INVALID_ARGUMENT. */
  Description() = default;

  /** @brief Transfer the unnamed recipe; its source becomes empty. */
  Description(Description &&) noexcept = default;

  /** @brief Replace this recipe with the unnamed source, which becomes empty.
   * @return This recipe. Destroying a replaced recipe has no mounted UI effect.
   */
  Description &operator=(Description &&) noexcept = default;

  Description(const Description &) = delete;

  Description &operator=(const Description &) = delete;

  /** @param reference Empty observing slot; duplicate slots in one mount are rejected.
   * @return This consuming recipe for chaining. The slot remains empty on mount failure.
   */
  Description &&reference(NodeRef<T> reference) && {
    return std::move(*this).storeReference(std::move(reference));
  }

  /** @param target Owning style identity, staged through ManagedScene::style.
   * @return This consuming recipe for chaining.
   */
  Description &&style(StyleTarget target) && {
    if (state_ && state_->id) {
      if (target.id.empty()) {
        target.id = *state_->id;
      } else if (!target.id.empty()) {
        state_->id = target.id;
      }
    }

    return std::move(*this).setup([target = std::move(target)](ManagedScene &scene, Node<T> node) mutable {
      return scene.style(node, std::move(target));
    });
  }

  /** @param value Case-sensitive style id copied into this recipe; type/classes are preserved.
   * @return Consuming recipe; id is assigned at mount in modifier order, before later styles.
   * A later style with an empty id preserves it; an explicit nonempty style id replaces it.
   */
  Description &&id(std::string_view value) && {
    if (state_) {
      state_->id.emplace(value);
    }

    return std::move(*this).setup([value = detail::captureText(value)](ManagedScene &scene, Node<T> node) {
      return scene.id(node, detail::captionView(value));
    });
  }

  /** @tparam Callback Compact owning click callback, optionally receiving ClickEvent.
   * @param callback Void, expected<Value, TreeError> or EditorCommandResult action; success values are ignored.
   * @return Consuming button recipe using one existing managed binding; duplicates fail at mount.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::same_as<T, Button> && portals::detail::ManagedAction<Callback, ClickEvent>)
  Description &&onClick(Callback callback) && {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onClick(node, std::move(callback));
    });
  }

  /** @tparam Value Owning equality-comparable managed state value.
   * @tparam Callback Copyable presentation callback taking const Value&, returning void or expected<void, TreeError>.
   * @param model State owned by the receiving scene.
   * @param callback Owning action, first delivered after the entire lazy mount and reference publication.
   * @return Consuming recipe; the scene owns the subscription after successful mount.
   */
  template <std::equality_comparable Value, class Callback>
    requires std::copy_constructible<Callback> &&
             (std::same_as<std::invoke_result_t<Callback &, const Value &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const Value &>, std::expected<void, TreeError>>)
  Description &&observe(ManagedState<Value> model, Callback callback) && {
    return std::move(*this).setup([model, callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.observe(node, model, std::move(callback));
    });
  }

  /** @tparam Value Owning derived source value.
   * @tparam Callback Copyable presentation callback taking const Value& and returning void or expected<void,
   * TreeError>.
   * @param source Read-only source owned by the receiving scene.
   * @param callback Owning action first delivered after complete mount and Ref publication.
   * @return Consuming recipe with the existing scene-owned subscription and checked failures.
   */
  template <std::equality_comparable Value, class Callback>
    requires std::copy_constructible<Callback> &&
             (std::same_as<std::invoke_result_t<Callback &, const Value &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const Value &>, std::expected<void, TreeError>>)
  Description &&observe(ManagedSource<Value> source, Callback callback) && {
    return std::move(*this).setup([source, callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.observe(node, source, std::move(callback));
    });
  }

  /** @param callback Existing managed custom event adapter; standard bindings reject duplicates.
   * @return This consuming recipe for chaining; the scene owns the callback after mounting.
   */
  Description &&onEvent(ManagedEventHandler<T> callback) && {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onEvent(node, std::move(callback));
    });
  }

  /** @param callback Existing managed dirty-frame adapter; no new polling or runtime is introduced.
   * @return This consuming recipe for chaining.
   */
  Description &&onPrepare(ManagedPrepareHandler<T> callback) && {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onPrepare(node, std::move(callback));
    });
  }

  /** @tparam Callback Owning copyable callback returning expected<Value, TreeError> with non-void Value.
   * @param callback Existing dirty-frame work; its success value is ignored inside the entered scene scope.
   * @return This consuming recipe; errors are unchanged and no additional invalidation is requested.
   */
  template <class Callback>
    requires std::copy_constructible<Callback> &&
             portals::detail::ManagedTreeResult<std::invoke_result_t<Callback &, T &, Node<Widget>>> &&
             (!std::same_as<std::invoke_result_t<Callback &, T &, Node<Widget>>, std::expected<void, TreeError>>)
  Description &&onPrepare(Callback callback) && {
    ManagedPrepareHandler<T> normalized;
    bool present = true;

    if constexpr (std::is_pointer_v<Callback> || requires { callback.operator bool(); }) {
      present = static_cast<bool>(callback);
    }

    if (present) {
      normalized = [callback = std::move(callback)](T &widget, Node<Widget> focused) mutable {
        return portals::detail::invokeIgnoringValue(callback, widget, focused);
      };
    }

    return std::move(*this).onPrepare(std::move(normalized));
  }

  /** @param cells Nonnegative fixed width; replaces only the horizontal bounds/mode.
   * @return Consuming recipe; invalid cells fail at mount without publishing refs.
   */
  Description &&width(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::WIDTH, cells});
  }

  /** @param cells Nonnegative fixed height; replaces only the vertical bounds/mode.
   * @return Consuming recipe; invalid cells fail at mount without publishing refs.
   */
  Description &&height(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::HEIGHT, cells});
  }

  /** @param weight Positive horizontal flex weight; zero preferred basis, existing bounds and vertical axis are
   * preserved.
   * @return Consuming recipe; zero weight fails at checked mount.
   */
  Description &&flexX(std::uint32_t weight = 1) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::FLEX_X, 0, weight});
  }

  /** @param weight Positive vertical flex weight; zero preferred basis, existing bounds and horizontal axis are
   * preserved.
   * @return Consuming recipe; zero weight fails at checked mount.
   */
  Description &&flexY(std::uint32_t weight = 1) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::FLEX_Y, 0, weight});
  }

  /** @param weight Positive flex weight for both axes, with zero preferred bases and existing bounds preserved.
   * @return Consuming recipe; zero weight fails at checked mount. A prior fixed bound still caps flex growth.
   */
  Description &&flex(std::uint32_t weight = 1) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::FLEX, 0, weight});
  }

  /** @param callback Owning customization action, run in modifier order before children.
   * Capture the existing Application explicitly when a low-level tree operation is needed.
   * @return This consuming recipe for chaining. Empty callbacks fail at mount.
   */
  Description &&setup(Setup<T> callback) && {
    if (state_) {
      state_->setup.push_back(std::move(callback));
    }

    return std::move(*this);
  }

  /** @brief Declare this subtree as a nested focus scope, with scene-owned lifetime.
   * @return This consuming recipe; mount registers before child defaults and rolls back on failure.
   */
  Description &&focusScope() && {
    if (state_) {
      state_->focusScope = true;
    }

    return std::move(*this);
  }

  /** @brief Prefer this node when entering its nearest scope without eligible history.
   * @return This consuming recipe; mount fails with INVALID_ARGUMENT without an enclosing scope.
   * With multiple defaults in one scope, the last declared preorder default wins.
   */
  Description &&focusDefault() && {
    if (state_) {
      state_->focusDefault = true;
    }

    return std::move(*this);
  }

private:
  friend struct detail::Access;
  template <detail::DescriptionInput Child> friend Description<Border> border(Child &&, BorderOptions, ControlStyle);
  template <detail::DescriptionInput Child>
  friend Description<ScrollArea> scrollArea(Child &&, ScrollAreaOptions, std::shared_ptr<ScrollController>,
                                            ControlStyle);

  template <class U, class... Args> friend Description<U> custom(Args &&...args);
  template <class U> friend Description<U> control(std::string_view, ControlStyle, SizePolicy);
  template <class U, detail::DescriptionInput... Children> friend Description<U> linear(LinearOptions, Children &&...);
  template <class U, detail::DescriptionRange R> friend Description<U> linear(LinearOptions, R &&);
  template <detail::DescriptionInput... Children> friend Description<Grid> grid(Children &&...);
  friend Description<LineEdit> lineEdit(ManagedState<std::string>, LineEditConfig);

  /** @brief Owning constructor adapter using the existing checked managed mount. */
  using Factory = portals::detail::MoveOnlyFunction<std::expected<Node<T>, ManagedError>(ManagedScene &, Node<Widget>,
                                                                                         detail::MountContext &)>;
  /** @brief Owning child recipe adapter sharing the parent's transaction bookkeeping. */
  using Child = portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<Widget>,
                                                                                    detail::MountContext &)>;

  /** @brief Unmounted recipe storage, consumed and destroyed during mount. */
  struct State {
    Factory create;                                                  ///< Constructs exactly one managed node.
    std::vector<Setup<T>> setup = std::vector<Setup<T>>(0);          ///< Customization actions in modifier order.
    std::vector<Child> children = std::vector<Child>(0);             ///< Consuming child adapters in insertion order.
    std::vector<NodeRef<T>> references = std::vector<NodeRef<T>>(0); ///< Observing slots reserved for this node.
    bool focusScope{};             ///< Register before setup/children; scene owns its token.
    bool focusDefault{};           ///< Set the nearest registered scope's default before children.
    std::optional<std::string> id; ///< Compact recipe identity preserved by later style modifiers.
  };

  /** @brief Allocate recipe storage and take ownership of its constructor adapter.
   * @tparam Create Owning callable returning a checked typed node for ManagedScene and parent.
   * @param create Owning checked
   * constructor callback, invoked only at mount.
   */
  // Reject unrelated by-value conversions before checked-result constructor probes recurse.
  template <class Create>
    requires std::invocable<Create &, ManagedScene &, Node<Widget> &>
  explicit Description(Create create) : state_(std::make_unique<State>()) {
    state_->create = [create = std::move(create)](ManagedScene &scene, Node<Widget> parent,
                                                  detail::MountContext &) mutable {
      return create(scene, parent);
    };
  }

  /** @param change Owning policy operation applied in setup/modifier order.
   * @return This consuming recipe; existing checked policy errors propagate at mount.
   */
  Description &&resize(detail::SizeChange change) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).setup([change](ManagedScene &scene, Node<T> node) {
      return detail::Access::resize(scene, node, change);
    });
  }

  /** @brief Retain a shared observing slot without publishing it yet.
   * @param reference Slot to reserve when this
   * recipe is mounted.
   * @return This consuming recipe for modifier chaining.
   */
  Description &&storeReference(NodeRef<T> reference) && {
    if (state_) {
      state_->references.push_back(std::move(reference));
    }

    return std::move(*this);
  }

  /** @brief Consume this recipe into one existing managed transaction.
   * @param scene Borrowed UI owner receiving
   * the new subtree.
   * @param parent Live managed parent receiving this recipe's root.
   * @param context Borrowed
   * observations and ref reservations for the complete mount.
   * @return Created observer or unchanged error; the
   * outer mount owns subtree rollback.
   */
  std::expected<Node<T>, ManagedError> mountInto(ManagedScene &scene, Node<Widget> parent,
                                                 detail::MountContext &context) && {
    auto state = std::move(state_);

    if (!state || !state->create) {
      return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
    }

    auto node = state->create(scene, parent, context);

    if (!node) {
      return node;
    }

    if (context.root == Node<Widget>{}) {
      context.root = *node;
    }

    context.nodes.push_back(*node);

    for (auto &reference : state->references) {
      auto slot = reference.slot_;

      if (slot && slot->fluent && slot->target.owner != std::this_thread::get_id()) {
        return std::unexpected(detail::error(*node, TreeError::WRONG_THREAD));
      }

      if (!slot || slot->node != Node<T>{} || slot->pending) {
        return std::unexpected(detail::error(*node, TreeError::INVALID_ARGUMENT));
      }

      // Reserve across reentrant mount calls too, but never publish a partial identity.
      // Allocate both callbacks before setting pending so exceptions cannot strand the slot.
      context.release.push_back([slot] {
        slot->pending = false;
      });
      context.publish.push_back([slot, value = *node, owner = &scene, lifetime = detail::Access::lifetime(scene)] {
        slot->node = value;
        slot->target.node = value;
        slot->target.scene = owner;
        slot->target.lifetime = lifetime;

        if constexpr (std::derived_from<T, Control>) {
          slot->target.caption = value;
        } else if constexpr (std::same_as<T, LineEdit>) {
          slot->target.draft = value;
        }
      });
      detail::RefTarget pending;
      pending.node = *node;
      pending.scene = &scene;
      pending.lifetime = detail::Access::lifetime(scene);
      if constexpr (std::derived_from<T, Control>) {
        pending.caption = *node;
      } else if constexpr (std::same_as<T, LineEdit>) {
        pending.draft = *node;
      }
      context.pending.push_back({&slot->target, std::move(pending)});
      slot->pending = true;
    }

    if (state->focusScope) {
      auto registered = scene.focusScope(*node);

      if (!registered) {
        return std::unexpected(registered.error());
      }
    }

    if (state->focusDefault) {
      auto configured = scene.focusDefault(*node);

      if (!configured) {
        return std::unexpected(configured.error());
      }
    }

    for (auto &setup : state->setup) {
      if (!setup) {
        return std::unexpected(detail::error(*node, TreeError::INVALID_ARGUMENT));
      }

      auto configured = setup(scene, *node);

      if (!configured) {
        return std::unexpected(configured.error());
      }
    }

    for (auto &child : state->children) {
      auto mounted = child(scene, *node, context);

      if (!mounted) {
        return std::unexpected(mounted.error());
      }
    }

    return node;
  }

  std::unique_ptr<State> state_;
};

namespace detail {
template <class T, class Factory> Description<T> Access::describe(Factory factory) {
  return Description<T>(std::move(factory));
}

/// @cond INTERNAL
template <class T, class Factory> Description<T> Access::cold(Factory factory) {
  Description<T> description;
  description.state_ = std::make_unique<typename Description<T>::State>();
  description.state_->create = std::move(factory);
  return description;
}

/// @endcond

template <class Factory> Description<Widget> Access::transaction(Factory factory) {
  Description<Widget> description;

  description.state_ = std::make_unique<Description<Widget>::State>();
  description.state_->create = std::move(factory);

  return description;
}

template <class T> Description<Widget> Access::erase(Description<T> description) {
  Description<Widget> erased;

  erased.state_ = std::make_unique<Description<Widget>::State>();
  erased.state_->create =
    [description = std::move(description)](ManagedScene &scene, Node<Widget> parent,
                                           MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
    auto result = mount(std::move(description), scene, parent, context);

    if (!result) {
      return std::unexpected(result.error());
    }

    return Node<Widget>(*result);
  };

  return erased;
}

template <class T>
std::expected<Node<T>, ManagedError> Access::mount(Description<T> &&description, ManagedScene &scene,
                                                   Node<Widget> parent, MountContext &context) {
  return std::move(description).mountInto(scene, parent, context);
}

/** @brief Materialize string-like inputs and otherwise retain a decayed argument value.
 * @tparam T Forwarded
 * constructor argument type; strings must convert to std::string_view.
 * @param value Source copied or moved into
 * recipe storage; nonstring borrows remain explicit.
 * @return Owning std::string for string-like inputs, otherwise a
 * decayed argument value.
 */
template <class T> auto own(T &&value) {
  if constexpr (std::convertible_to<T, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return std::decay_t<T>(std::forward<T>(value));
  }
}

/** @brief Borrow string bytes only while constructing the final owning argument tuple.
 * @tparam T Forwarded constructor argument; string-like values convert to string_view.
 * @param value Input living through the immediate tuple constructor.
 * @return A temporary string view, or the unchanged nonstring forwarding reference.
 */
template <class T> decltype(auto) constructionArgument(T &&value) {
  if constexpr (std::convertible_to<T, std::string_view>) {
    return std::string_view(value);
  } else {
    return std::forward<T>(value);
  }
}
} // namespace detail

/** @brief Capture constructor arguments without requiring inheritance from a DSL widget.
 * @tparam T Public Widget subtype, including final types, constructible from the owned arguments.
 * @tparam Args Decayed owning argument types; string-like inputs are materialized as std::string.
 * @param args Values copied/moved now and moved into T at mount. Other pointers/reference_wrapper
 * remain explicit borrows: their referents must outlive the widget. No deep copy is inferred.
 * @return A single-use recipe. T must own any string_view it retains beyond its constructor.
 */
template <class T, class... Args> Description<T> custom(Args &&...args) {
  using OwnedArguments = std::tuple<decltype(detail::own(std::forward<Args>(args)))...>;
  constexpr bool HAS_TEXT_ARGUMENT = (std::convertible_to<Args, std::string_view> || ...);

  // Construct strings directly in stationary tuple storage. Moving a string or tuple<string>
  // can allocate a checked MSVC proxy despite noexcept; a string_view constructor stays fallible.
  // Explicit element types also preserve tuple/pair constructor arguments without CTAD flattening.
  auto owned = [&] {
    if constexpr (HAS_TEXT_ARGUMENT && detail::CHECKED_STRING_MOVES) {
      return std::make_unique<OwnedArguments>(detail::constructionArgument(std::forward<Args>(args))...);
    } else {
      return OwnedArguments(detail::own(std::forward<Args>(args))...);
    }
  }();

  return Description<T>([owned = std::move(owned)](ManagedScene &scene, Node<Widget> parent) mutable {
    auto &arguments = [&]() -> auto & {
      if constexpr (HAS_TEXT_ARGUMENT && detail::CHECKED_STRING_MOVES) {
        return *owned;
      } else {
        return owned;
      }
    }();

    return std::apply(
      [&](auto &...values) {
        return scene.emplace<T>(parent, std::move(values)...);
      },
      arguments);
  });
}

template <class Content, class Initial>
Description<DataStateView> detail::Access::dataState(Initial initial, Description<Content> content,
                                                     DataStateConfig config, bool automatic) {
  auto result = describe<DataStateView>(
    [initial, config = std::move(config), automatic](ManagedScene &scene, Node<Widget> parent) mutable {
      if constexpr (std::same_as<Initial, DataPresentation>) {
        return scene.beginDataState(parent, initial, std::move(config), automatic);
      } else {
        return scene.beginBoundDataState(parent, initial, std::move(config));
      }
    });
  result.state_->children.push_back(
    [content = std::move(content), automatic](ManagedScene &scene, Node<Widget> parent,
                                              MountContext &context) mutable -> std::expected<void, ManagedError> {
      const auto finalizers = context.finalize.size();

      auto child = mount(std::move(content), scene, parent, context);

      if (!child) {
        return std::unexpected(child.error());
      }

      if (automatic && context.finalize.size() != finalizers) {
        // Cold virtual bodies attach their model during the existing mount finalization.
        // Publish the relationship afterward, before any reference or preparation escapes.
        context.finalize.push_back([&scene, parent, body = *child] {
          return scene.finishDataState(parent, body, true);
        });

        return {};
      }

      return scene.finishDataState(parent, *child, automatic);
    });
  return result;
}

/** @tparam Content Retained widget type mounted as the one body subtree.
 * @param initial Owning explicit state, with nonzero producer generation.
 * @param content Consuming body recipe; independent work is required when REPLACE hides it.
 * @param config Construction-only labels, fixed strip, styles and optional retry.
 * @return Single-use composition; validation/allocation errors preserve ordinary mount rollback.
 */
template <class Content>
Description<DataStateView> dataStateView(DataPresentation initial, Description<Content> content,
                                         DataStateConfig config = {}) {
  return detail::Access::dataState(initial, std::move(content), std::move(config), false);
}

/** @tparam Source Exactly ManagedState<DataPresentation> or readonly ManagedSource<DataPresentation>.
 * @tparam Content Retained body widget type.
 * @param source Weak same-scene one-way observation; no writable alias escapes.
 * @param content Consuming body recipe. @param config Owning immutable presentation settings.
 * @return Single-use bound composition; initial snapshot is validated before publication.
 */
template <class Source, class Content>
  requires(std::same_as<Source, ManagedState<DataPresentation>> ||
           std::same_as<Source, ManagedSource<DataPresentation>>)
Description<DataStateView> dataStateView(Source source, Description<Content> content, DataStateConfig config = {}) {
  return detail::Access::dataState(source, std::move(content), std::move(config), false);
}

/** @tparam View VirtualList/VirtualTable type exposing existing prepared owning facts.
 * @param content Consuming managed virtual view; its provider callback retains ordinary work ownership.
 * @param config Stable strip and optional explicit retry; REPLACE is rejected.
 * @return Automatic composition using the body's successful Prepared without repinning or I/O.
 */
template <class View>
  requires requires(const View &view) {
    { view.dataStatus() } -> std::same_as<DataRangeStatus>;
  }
Description<DataStateView> dataStateView(Description<View> content, DataStateConfig config = {}) {
  return detail::Access::dataState(DataPresentation{DataPhase::LOADING, 1}, std::move(content), std::move(config),
                                   true);
}

/** @brief Construct a standard text-control recipe with the scene's width policy.
 * @tparam T Label, Button or CheckBox-compatible text constructor.
 * @param text UTF-8 bytes copied now; invalid text returns INVALID_ARGUMENT at mount.
 * @param style Owning manual control appearances.
 * @param policy Owning layout constraints.
 * @return Single-use owning recipe; allocation while copying may throw.
 */
template <class T> Description<T> control(std::string_view text, ControlStyle style, SizePolicy policy) {
  return Description<T>([text = detail::captureText(text), style, policy](ManagedScene &scene, Node<Widget> parent) {
    auto value = GraphemeText::from(detail::captionView(text), scene.widthPolicy());

    if (!value) {
      auto failure = detail::textError(parent, value.error());

      return std::expected<Node<T>, ManagedError>(std::unexpected(failure));
    }

    return scene.emplace<T>(parent, std::move(*value), style, policy);
  });
}

/** @param text UTF-8 copied immediately, validated at mount under the scene's width policy.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use label recipe.
 */
inline Description<Label> label(std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Label>(text, style, policy);
}

namespace detail {
/** @brief Resolve an owning caption expression once inside the existing mount transaction.
 * @tparam T Existing text control constructed with text, style and size policy.
 * @param text Owning literal, format result or localization key.
 * @param style Manual appearances copied into the recipe.
 * @param policy Owning layout constraints.
 * @param resolver Owning explicit localization adapter; no locale subscription is created.
 * @return Single-use caption recipe with original text/resource errors.
 */
template <class T>
Description<T> textControl(TextValue text, ControlStyle style, SizePolicy policy, TextResolver resolver) {
  return Access::describe<T>([text = std::move(text), style, policy, resolver = std::move(resolver)](
                               ManagedScene &scene, Node<Widget> parent) -> std::expected<Node<T>, ManagedError> {
    auto value = resolver ? text.resolve(scene.widthPolicy(), resolver) : text.resolve(scene.widthPolicy());

    if (!value) {
      if (const auto *failure = std::get_if<TextError>(&value.error())) {
        return std::unexpected(textError(parent, *failure));
      }

      auto failure = error(parent, TreeError::INVALID_ARGUMENT);

      failure.resource = std::get<TextResourceError>(value.error());

      return std::unexpected(failure);
    }

    return scene.emplace<T>(parent, std::move(*value), style, policy);
  });
}
} // namespace detail

/** @param text Owning literal, format result or localization key, resolved once at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param resolver Explicit owning localization adapter; borrowed services must outlive mount.
 * @return Independent label recipe; expression/resource failures retain their typed payload.
 */
inline Description<Label> label(TextValue text, ControlStyle style = {}, SizePolicy policy = {},
                                TextResolver resolver = {}) {
  return detail::textControl<Label>(std::move(text), style, policy, std::move(resolver));
}

/** @tparam State Exact ManagedState<string> or read-only ManagedSource<string>; bare braces retain literal overload.
 * @param model Scene-owned accepted text state; must belong to the receiving scene.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return One-way caption recipe; changed revisions synchronize before dirty frames without caller tokens.
 */
template <class State>
  requires(std::same_as<std::remove_cvref_t<State>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<State>, ManagedSource<std::string>>)
inline Description<Label> label(State &&model, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Label>("", style, policy).setup([model](ManagedScene &scene, Node<Label> node) {
    return scene.bindText(node, model);
  });
}

/** @param text Owning literal, format result or localization key, resolved once at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param resolver Explicit owning localization adapter; borrowed services must outlive mount.
 * @return Unbound button recipe with an independent caption; onClick can be composed separately.
 */
inline Description<Button> button(TextValue text, ControlStyle style = {}, SizePolicy policy = {},
                                  TextResolver resolver = {}) {
  return detail::textControl<Button>(std::move(text), style, policy, std::move(resolver));
}

/** @tparam State Exact ManagedState<string> or read-only ManagedSource<string>; bare braces retain literal overload.
 * @param model Scene-owned accepted text state; must belong to the receiving scene.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return One-way caption recipe with independent action attachment and existing dirty-frame recovery.
 */
template <class State>
  requires(std::same_as<std::remove_cvref_t<State>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<State>, ManagedSource<std::string>>)
inline Description<Button> button(State &&model, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Button>("", style, policy).setup([model](ManagedScene &scene, Node<Button> node) {
    return scene.bindText(node, model);
  });
}

/** @tparam Caption Owning TextValue, ManagedState<string> or read-only ManagedSource<string> observation.
 * @tparam Callback Owning compact managed action, optionally borrowing ClickEvent.
 * @param text Caption expression or explicit accepted text state.
 * @param action Independent activation callback with existing checked result handling.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Button recipe with scene-owned caption/action lifetimes and transactional mount cleanup.
 */
template <class Caption, class Callback>
  requires((std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>>) &&
           portals::detail::ManagedAction<Callback, ClickEvent>)
inline Description<Button> button(Caption &&text, Callback action, ControlStyle style = {}, SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), style, policy).onClick(std::move(action));
}

/** @param text UTF-8 caption copied immediately.
 * @param action Standard managed activation callback; must be nonempty.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use button recipe retaining its callback until mount.
 */
inline Description<Button> button(std::string_view text, std::function<std::expected<void, TreeError>()> action,
                                  ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Button>(text, style, policy)
    .setup([action = std::move(action)](ManagedScene &scene, Node<Button> node) mutable {
      return scene.onClick(node, std::move(action));
    });
}

/** @tparam Callback Owning compact action returning void, expected<void, TreeError> or EditorCommandResult.
 * @param text UTF-8 caption copied immediately.
 * @param action Action optionally borrowing ClickEvent; return field.undo/redo directly to preserve its receipt.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use button recipe with scene-owned callback and exact command provenance reporting.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
inline Description<Button> button(std::string_view text, Callback action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return control<Button>(text, style, policy)
    .setup([action = std::move(action)](ManagedScene &scene, Node<Button> node) mutable {
      return scene.onClick(node, std::move(action));
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param model Observing managed state from the scene receiving this recipe.
 * @param changed Optional callback after the standard commit; cannot undo the commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use bound checkbox recipe.
 */
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<bool> model,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return control<CheckBox>(text, style, policy)
    .setup([model, changed = std::move(changed)](ManagedScene &scene, Node<CheckBox> node) mutable {
      return scene.bind(node, model, std::move(changed));
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param initial Initial checked value, owned by the receiving scene after mount.
 * @param changed Optional callback after the standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Checkbox recipe with internal managed state; no binding token escapes.
 */
inline Description<CheckBox> checkbox(std::string_view text, bool initial = false,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return control<CheckBox>(text, style, policy)
    .setup([initial, changed = std::move(changed)](ManagedScene &scene, Node<CheckBox> node) mutable {
      return scene.state(initial).and_then([&scene, node, &changed](ManagedState<bool> model) {
        return scene.bind(node, model, std::move(changed));
      });
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param model Observing managed state from the scene receiving this recipe.
 * @param changed Optional callback after the standard commit; cannot undo the commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @param options Typed user activation cycle.
 * @return Single-use bound checkbox recipe.
 */
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<CheckState> model,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return control<CheckBox>(text, style, policy)
    .setup([model, changed = std::move(changed), options](ManagedScene &scene, Node<CheckBox> node) mutable {
      return scene.bind(node, model, std::move(changed), options);
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param initial Initial checked value, owned by the receiving scene after mount.
 * @param changed Optional callback after the standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @param options Typed user activation cycle.
 * @return Checkbox recipe with internal managed state; no binding token escapes.
 */
inline Description<CheckBox> checkbox(std::string_view text, CheckState initial,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return control<CheckBox>(text, style, policy)
    .setup([initial, changed = std::move(changed), options](ManagedScene &scene, Node<CheckBox> node) mutable {
      return scene.state(initial).and_then([&scene, node, &changed, options](ManagedState<CheckState> model) {
        return scene.bind(node, model, std::move(changed), options);
      });
    });
}

/** @brief Configure an explicit checkbox value without positional callback/style placeholders.
 * @tparam Options Exactly CheckBoxOptions after cv/ref removal; bare braces do not deduce this overload.
 * @param text UTF-8 caption copied immediately.
 * @param initial Initial state owned by the receiving scene after mount.
 * @param options Owning activation policy copied now; invalid values fail through the existing bind boundary.
 * @param changed Optional exact commit receipt callback; cannot undo a committed value.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return The existing checkbox recipe, preserving its ownership, errors and synchronization behavior.
 */
template <class Options>
  requires std::same_as<std::remove_cvref_t<Options>, CheckBoxOptions>
inline Description<CheckBox> checkbox(std::string_view text, CheckState initial, Options &&options,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return checkbox(text, initial, std::move(changed), style, policy, std::forward<Options>(options));
}

/** @brief Configure shared checkbox state without positional callback/style placeholders.
 * @tparam Options Exactly CheckBoxOptions after cv/ref removal; implicit conversions are not admitted.
 * @param text UTF-8 caption copied immediately.
 * @param model Observing managed explicit state from the receiving scene.
 * @param options Owning activation policy copied now, including from a temporary or const lvalue.
 * @param changed Optional exact commit receipt callback.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return The existing shared-state checkbox recipe; captures and cleanup follow the original factory.
 */
template <class Options>
  requires std::same_as<std::remove_cvref_t<Options>, CheckBoxOptions>
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<CheckState> model, Options &&options,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return checkbox(text, model, std::move(changed), style, policy, std::forward<Options>(options));
}

/** @param model Observing scene-owned text; existing draft/commit/conflict semantics are retained.
 * @param config Owning optional callbacks, limits, appearances, layout and clipboard service.
 * @return Single-use bound editor recipe; width policy always follows the scene.
 */
inline Description<LineEdit> lineEdit(ManagedState<std::string> model, LineEditConfig config = {}) {
  auto description = detail::Access::describe<LineEdit>(
    [options = config.options, policy = config.policy](
      ManagedScene &scene, Node<Widget> parent) mutable -> std::expected<Node<LineEdit>, ManagedError> {
      options.widthPolicy = scene.widthPolicy();
      auto placeholder = detail::lineEditPlaceholder(options, parent);
      if (!placeholder) {
        return std::unexpected(placeholder.error());
      }

      return scene.emplace<LineEdit>(parent, std::move(options), policy, std::move(*placeholder));
    });

  return std::move(description)
    .setup([model, result = std::move(config.result), clipboard = std::move(config.clipboard),
            validate = std::move(config.validate)](ManagedScene &scene, Node<LineEdit> node) mutable {
      return scene.bind(node, model, std::move(result), std::move(clipboard), std::move(validate));
    });
}

/** @param initial Owning initial text expression, resolved under the receiving scene's width policy at mount.
 * @param config Existing editor settings, including confirmation mode, history, validation and clipboard.
 * @param resolver Optional owning synchronous resource adapter; borrowed services must outlive mount.
 * @return Single-use editor recipe with an internal scene-owned text state; construction may throw.
 */
inline Description<LineEdit> lineEdit(TextValue initial, LineEditConfig config = {}, TextResolver resolver = {}) {
  auto model = std::make_shared<ManagedState<std::string>>();
  auto description = detail::Access::describe<LineEdit>(
    [initial = std::move(initial), options = config.options, policy = config.policy, resolver = std::move(resolver),
     model](ManagedScene &scene, Node<Widget> parent) mutable -> std::expected<Node<LineEdit>, ManagedError> {
      auto resolved = resolver ? initial.resolve(scene.widthPolicy(), resolver) : initial.resolve(scene.widthPolicy());

      if (!resolved) {
        auto failure = detail::error(parent, TreeError::INVALID_ARGUMENT);

        if (const auto *text = std::get_if<TextError>(&resolved.error())) {
          failure.text = *text;
        } else {
          failure.resource = std::get<TextResourceError>(resolved.error());
        }

        return std::unexpected(failure);
      }

      options.widthPolicy = scene.widthPolicy();
      auto placeholder = detail::lineEditPlaceholder(options, parent);
      if (!placeholder) {
        return std::unexpected(placeholder.error());
      }

      auto state = scene.state(std::string(resolved->bytes()));

      if (!state) {
        return std::unexpected(state.error());
      }

      *model = *state;

      return scene.emplace<LineEdit>(parent, std::move(options), policy, std::move(*placeholder));
    });

  return std::move(description)
    .setup([model, result = std::move(config.result), clipboard = std::move(config.clipboard),
            validate = std::move(config.validate)](ManagedScene &scene, Node<LineEdit> node) mutable {
      return scene.bind(node, *model, std::move(result), std::move(clipboard), std::move(validate));
    });
}

/** @param initial UTF-8 bytes copied immediately; validated at mount under the scene width policy.
 * @param config Existing editor settings and owned callbacks.
 * @return Single-use editor recipe with internal managed state; no caller state boilerplate is needed.
 */
inline Description<LineEdit> lineEdit(std::string_view initial, LineEditConfig config = {}) {
  return lineEdit(TextValue(initial), std::move(config));
}

/** @param reference Empty typed observation published after the complete form mounts.
 * @param initial Owning initial expression.
 * @param config Existing editor settings.
 * @param resolver Optional synchronous resource adapter.
 * @return Existing internal-state editor recipe with additive Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, TextValue initial, LineEditConfig config = {},
                                      TextResolver resolver = {}) {
  return lineEdit(std::move(initial), std::move(config), std::move(resolver)).reference(std::move(reference));
}

/** @param reference Empty typed observation.
 * @param initial UTF-8 bytes copied immediately.
 * @param config Existing editor settings.
 * @return Internal-state editor recipe with Ref publication after complete mount.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, std::string_view initial, LineEditConfig config = {}) {
  return lineEdit(initial, std::move(config)).reference(std::move(reference));
}

/// @brief Describe a managed virtual table without opening a range until mount.
/// @tparam T Immutable model row accepted by the column formatters; deduced from model.
/// @param model Weak UI-affine handle belonging to the scene receiving this recipe.
/// @param columns Descriptions copied now, including owning headers and formatter objects.
/// @param options Existing table settings, validated by managed preparation.
/// @param policy Existing layout constraints for the table.
/// @param prepared Owning provider callback after preparation, including retries.
/// Empty is valid for ready snapshots; pending work without it returns INVALID_ARGUMENT.
/// @return Single-use recipe with standard input, styles and automatic preparation.
/// Construction may throw while copying columns or callbacks. Mount preserves typed errors
/// and subtree rollback. The scene owns the model; reference captures remain borrowed.
template <class T>
Description<VirtualTable<T>> virtualTable(ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          VirtualTableOptions options = {}, SizePolicy policy = {},
                                          std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  // A recipe may outlive the caller's array or vector. Copy before retaining a mount callback.
  auto ownedColumns = std::vector<TableColumn<T>>(columns.begin(), columns.end());

  return detail::Access::describe<VirtualTable<T>>(
    [model, columns = std::move(ownedColumns), options, policy,
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent) mutable {
      return scene.virtualTable(parent, model, columns, options, policy, std::move(prepared));
    });
}

/// @brief Describe a managed virtual list without opening a range until mount.
/// @tparam T Immutable model row accepted by the formatter; deduced from model.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param delegate Owning formatter invoked for visible rows during preparation.
/// @param options Existing list settings, validated by managed preparation.
/// @param policy Existing layout constraints for the list.
/// @param prepared Owning provider callback after preparation. Empty rejects pending work
/// with INVALID_ARGUMENT. Captured references must remain valid until retirement.
/// @return Single-use recipe retaining callbacks and standard managed input/style behavior.
/// Capture construction may throw; mount preserves typed errors and subtree rollback.
template <class T>
Description<VirtualList<T>> virtualList(ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        VirtualListOptions options = {}, SizePolicy policy = {},
                                        std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return detail::Access::describe<VirtualList<T>>(
    [model, delegate = std::move(delegate), options, policy,
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent) mutable {
      return scene.virtualList(parent, model, std::move(delegate), options, policy, std::move(prepared));
    });
}

/// @brief Describe a managed chart using the existing frame preparation order.
/// @tparam T Immutable model row projected into optional OHLC values; deduced from model.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param accessor Owning projection; nullopt represents a gap, as in the managed chart API.
/// @param options Existing chart mode, width and glyph configuration.
/// @param first Owning origin callback, invoked in composition registration order during
/// preparation. Empty means row zero. NodeRef observations are already published then.
/// @param prepared Owning provider callback after preparation, including retries. Empty
/// rejects pending work with INVALID_ARGUMENT. Explicit reference captures remain borrowed.
/// @return Single-use recipe with automatic style/preparation and no input binding.
/// Capture construction may throw; mount preserves typed errors and subtree rollback.
template <class T>
Description<Chart<T>> chart(ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                            ChartOptions options = {}, ManagedChartOrigin first = {},
                            std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return detail::Access::describe<Chart<T>>(
    [model, accessor = std::move(accessor), options, first = std::move(first),
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent) mutable {
      return scene.chart(parent, model, std::move(accessor), options, std::move(first), std::move(prepared));
    });
}

/// @brief Named owning settings for a table recipe; defaults match the positional factory.
/// @tparam T Immutable model row type, matching the receiving ManagedModel.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct VirtualTableConfig {
  VirtualTableOptions options{};    ///< Existing dimensions, width policy, budgets and styles.
  SizePolicy policy{};              ///< Layout constraints in terminal cells.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/// @brief Named owning settings for a list recipe; defaults match the positional factory.
/// @tparam T Immutable model row type, matching the receiving ManagedModel.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct VirtualListConfig {
  VirtualListOptions options{};     ///< Existing row height, overscan, width policy and styles.
  SizePolicy policy{};              ///< Layout constraints in terminal cells.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/// @brief Named owning settings for a chart recipe; defaults match the positional factory.
/// @tparam T Immutable model row type projected by the chart accessor.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct ChartConfig {
  ChartOptions options{};           ///< Existing plot mode, width, glyphs and styles.
  ManagedChartOrigin first{};       ///< Origin evaluated during preparation; empty starts at row zero.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/** @brief Owning cold text collection recipe; attaching happens only during a complete mount. */
struct TextCollectionRecipe {
  TextSource source;        ///< Weak direct caption/draft/committed text identity, including same-attempt pending Ref.
  TextSplitOptions options; ///< Positive token and whole-input limits.
  bool splitWords{};        ///< True for whitespace-delimited tokens; false for extended graphemes.
};

/** @param source Read-only text identity; copying does not read or attach.
 * @param options Whole-input limits and positive token cap.
 * @return Cold whitespace-token recipe; punctuation is preserved, not UAX #29 word-break.
 */
inline TextCollectionRecipe words(TextSource source, TextSplitOptions options = {}) {
  return {std::move(source), options, true};
}

/** @param source Read-only text identity, including a direct Ref reserved in this mount.
 * @param options Whole-input limits and positive token cap.
 * @return Cold recipe with one token per existing extended grapheme, including whitespace.
 */
inline TextCollectionRecipe graphemes(TextSource source, TextSplitOptions options = {}) {
  return {std::move(source), options, false};
}

/** @param recipe Cold text transform retaining owning segments with reset-on-change row IDs.
 * @param config Existing list options and sizing; the default formatter displays each complete token.
 * @return Single-use list recipe; pure initial finalization precedes all public Ref publication.
 * The temporary internal shell cannot prepare or paint before its real model and consumer are installed.
 */
inline Description<VirtualList<TextSegment>> virtualList(TextCollectionRecipe recipe,
                                                         VirtualListConfig<TextSegment> config = {}) {
  return detail::Access::cold<VirtualList<TextSegment>>(
    [recipe = std::move(recipe), config = std::move(config)](
      ManagedScene &scene, Node<Widget> parent,
      detail::MountContext &context) mutable -> std::expected<Node<VirtualList<TextSegment>>, ManagedError> {
      auto graph = std::make_shared<detail::Access::ColdGraph>();
      graph->scene = &scene;
      context.rollback.push_back([graph] {
        graph->retire();
      });
      graph->pending = true;
      detail::Access::beginCold(scene);
      config.options.width_policy = scene.widthPolicy();
      auto node = scene.emplace<VirtualList<TextSegment>>(
        parent, ModelConsumer<TextSegment>{}, ManagedListFormatter<TextSegment>([](const ModelRow<TextSegment> &row) {
          return row.value.text();
        }),
        config.options, config.policy);
      if (!node) {
        return node;
      }
      context.finalize.push_back([&scene, &context, graph, node = *node, recipe = std::move(recipe),
                                  prepared = std::move(config.prepared)]() mutable {
        return detail::Access::completeTextList(scene, node, recipe.source, recipe.splitWords, recipe.options, context,
                                                graph, std::move(prepared));
      });
      return node;
    });
}

/// @brief Describe a table using named settings and the same owning mount transaction.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly VirtualTableConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param columns Definitions copied now, including headers and formatter captures.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe; mount and preparation preserve the positional factory's typed errors.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy table options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualTableConfig<T>>
Description<VirtualTable<T>>
virtualTable(ManagedModel<T> model, std::type_identity_t<std::span<const TableColumn<T>>> columns, Config &&config) {
  return virtualTable(model, columns, config.options, config.policy, std::forward<Config>(config).prepared);
}

/// @brief Describe a list using named settings without changing its managed lifecycle.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly VirtualListConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param delegate Owning visible-row formatter; reference captures remain borrowed.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe preserving positional mount, preparation and error semantics.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy list options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualListConfig<T>>
Description<VirtualList<T>> virtualList(ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        Config &&config) {
  return virtualList(model, std::move(delegate), config.options, config.policy, std::forward<Config>(config).prepared);
}

/// @brief Describe a chart using named settings without changing preparation order.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly ChartConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param accessor Owning projection to optional OHLC; nullopt represents a gap.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe preserving positional mount, preparation and error semantics.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy chart options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, ChartConfig<T>>
Description<Chart<T>> chart(ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                            Config &&config) {
  return chart(model, std::move(accessor), config.options, std::forward<Config>(config).first,
               std::forward<Config>(config).prepared);
}

/** @brief Compose a retained linear layout with consuming child recipes.
 * @tparam T Existing Row or Column subtype
 * accepting LinearOptions.
 * @tparam Children Description types, passed as rvalues.
 * @param options Owning layout settings, validated by the existing layout engine.
 * @param children Owning recipes, mounted in argument order after the parent setup.
 * @return Single-use parent recipe; no second runtime or retained description tree is created.
 */
template <class T, detail::DescriptionInput... Children>
Description<T> linear(LinearOptions options, Children &&...children) {
  auto description = custom<T>(options);

  (description.state_->children.push_back(
     [child =
        std::forward<Children>(children)](ManagedScene &scene, Node<Widget> parent,
                                          detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
       auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

       return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
     }),
   ...);

  return description;
}

/** @brief Immediately consume a range into the existing linear-layout mount transaction.
 * @tparam T Existing Row or Column subtype accepting LinearOptions.
 * @tparam R Input range of one Description type; elements must be rvalues, or R an rvalue non-view, non-borrowed
 * container.
 * @param options Owning layout settings, validated by the existing layout engine.
 * @param children Recipes consumed once, in iteration order, before this function returns.
 * @return Single-use parent owning every recipe; an empty range creates an empty layout.
 * Use views::as_rvalue for explicit transfer from lvalue containers. Borrowed lvalue elements,
 * including those exposed by temporary views, are rejected. No iterator or view is retained.
 * Custom non-view, non-borrowed ranges must own their elements; that ownership cannot be inferred.
 * Construction/iteration may throw; already transferred source elements are not restored.
 * Mount preserves the original child error, subtree rollback and deferred reference publication.
 */
template <class T, detail::DescriptionRange R> Description<T> linear(LinearOptions options, R &&children) {
  auto description = custom<T>(options);

  for (auto &&child : children) {
    description.state_->children.push_back(
      [child = std::move(child)](ManagedScene &scene, Node<Widget> parent,
                                 detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
        auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      });
  }

  return description;
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param options Owning horizontal layout settings.
 * @param children Recipes transferred now, in left-to-right order; lvalue elements require explicit transfer.
 * @return Owning row recipe with transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Row> row(LinearOptions options, R &&children) {
  return linear<Row>(options, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param children Recipes transferred now, in left-to-right order; lvalue elements require explicit transfer.
 * @return Owning row recipe with default settings and transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Row> row(R &&children) {
  return linear<Row>({}, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param options Owning vertical layout settings.
 * @param children Recipes transferred now, in top-to-bottom order; lvalue elements require explicit transfer.
 * @return Owning column recipe with transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Column> column(LinearOptions options, R &&children) {
  return linear<Column>(options, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param children Recipes transferred now, in top-to-bottom order; lvalue elements require explicit transfer.
 * @return Owning column recipe with default settings and transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Column> column(R &&children) {
  return linear<Column>({}, std::forward<R>(children));
}

/** @tparam Children Rvalue Description types.
 * @param options Owning horizontal layout settings.
 * @param children Owning child recipes in left-to-right order.
 * @return Single-use row recipe.
 */
template <detail::DescriptionInput... Children> Description<Row> row(LinearOptions options, Children &&...children) {
  return linear<Row>(options, std::forward<Children>(children)...);
}

/** @brief Compose an explicit grid using the existing managed mount transaction.
 * @tparam Children Consuming child Description types, including custom final widgets.
 * @param children Owning recipes mounted in sibling order; no placement is inferred.
 * @return Single-use grid recipe; failed child mount rolls back the whole subtree.
 * Configure tracks and identity-based placements with setGridOptions after successful
 * mount, before running the scene. Published NodeRef observers supply those identities.
 * Construction may allocate; mounting retains the existing typed error and cleanup rules.
 */
template <detail::DescriptionInput... Children> Description<Grid> grid(Children &&...children) {
  auto description = custom<Grid>();

  (description.state_->children.push_back(
     [child =
        std::forward<Children>(children)](ManagedScene &scene, Node<Widget> parent,
                                          detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
       auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

       return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
     }),
   ...);

  return description;
}

/** @tparam Children Rvalue Description types.
 * @param children Owning child recipes in left-to-right order.
 * @return Single-use row recipe with default layout settings.
 */
template <detail::DescriptionInput... Children> Description<Row> row(Children &&...children) {
  return linear<Row>({}, std::forward<Children>(children)...);
}

/** @tparam Children Rvalue Description types.
 * @param options Owning vertical layout settings.
 * @param children Owning child recipes in top-to-bottom order.
 * @return Single-use column recipe.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(LinearOptions options, Children &&...children) {
  return linear<Column>(options, std::forward<Children>(children)...);
}

/** @tparam Children Rvalue Description types.
 * @param children Owning child recipes in top-to-bottom order.
 * @return Single-use column recipe with default layout settings.
 */
template <detail::DescriptionInput... Children> Description<Column> column(Children &&...children) {
  return linear<Column>({}, std::forward<Children>(children)...);
}

/** @param value Semantic non-character key; raw input is matched before router normalization.
 * @param modifiers Exact modifiers required by ManagedScene::onKey.
 * @return Owning chord value; PRESS and REPEAT are admitted by onKey.
 */
inline KeyEvent key(InputKey value, InputModifiers modifiers = {}) {
  return {value, {}, modifiers, KeyAction::PRESS};
}

/** @param scalar Character scalar, including existing control-key representations.
 * @param modifiers Exact modifiers required by ManagedScene::onKey.
 * @return Owning CHARACTER chord; no keyboard layout or scan code is inferred.
 */
inline KeyEvent character(char32_t scalar, InputModifiers modifiers = {}) {
  return {InputKey::CHARACTER, scalar, modifiers, KeyAction::PRESS};
}

/** @param text Caption copied into an unbound button recipe.
 * @return Consuming recipe; add onClick before mount or Ref.onClick after successful mount.
 */
inline Description<Button> button(std::string_view text) {
  return control<Button>(text, {}, {});
}

/** @param reference Shared empty slot, published after the whole mount succeeds.
 * @param text Caption copied now.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Label recipe sharing the existing reference transaction.
 */
inline Description<Label> label(Ref<Label> reference, std::string_view text, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return label(text, style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text Caption copied now.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Unbound button recipe for late Ref.onClick registration before run.
 */
inline Description<Button> button(Ref<Button> reference, std::string_view text, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return control<Button>(text, style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @param reference Empty observing slot published only after the complete mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Label recipe retaining the original transactional reference publication.
 */
template <class Caption>
  requires(std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>>)
inline Description<Label> label(Ref<Label> reference, Caption &&text, ControlStyle style = {}, SizePolicy policy = {}) {
  return label(std::forward<Caption>(text), style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @param reference Empty observing slot published only after the complete mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Button caption recipe; action registration remains independent.
 */
template <class Caption>
  requires(std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>>)
inline Description<Button> button(Ref<Button> reference, Caption &&text, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @tparam Callback Owning compact managed action, optionally borrowing ClickEvent.
 * @param reference Empty observing slot published after the whole mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param action Independent checked activation callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Bound button recipe preserving existing callback and rollback contracts.
 */
template <class Caption, class Callback>
  requires((std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>>) &&
           portals::detail::ManagedAction<Callback, ClickEvent>)
inline Description<Button> button(Ref<Button> reference, Caption &&text, Callback action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), std::move(action), style, policy).reference(std::move(reference));
}

/** @tparam Callback Owning compact action, optionally taking ClickEvent; void/expected/command result.
 * @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param action Owning callback; captures transfer to the scene at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Bound button recipe; registration errors participate in subtree rollback.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
Description<Button> button(Ref<Button> reference, std::string_view text, Callback action, ControlStyle style = {},
                           SizePolicy policy = {}) {
  return button(text, std::move(action), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param model Existing managed bool state.
 * @param changed Optional typed receipt callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Checkbox recipe retaining its original commit semantics.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text, ManagedState<bool> model,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return checkbox(text, model, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param model Existing managed explicit checkbox state.
 * @param changed Optional typed receipt callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param options Typed user activation cycle.
 * @return Checkbox recipe retaining its original commit semantics.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text, ManagedState<CheckState> model,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return checkbox(text, model, std::move(changed), style, policy, options).reference(std::move(reference));
}

/** @param reference Empty observing editor slot published after complete mount.
 * @param model Existing managed text state.
 * @param config Owning editor callbacks/options; existing EditMode/history behavior is retained.
 * @return Bound editor recipe using the original factory and reference transaction.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, ManagedState<std::string> model,
                                      LineEditConfig config = {}) {
  return lineEdit(model, std::move(config)).reference(std::move(reference));
}

/** @tparam T Public widget subtype compatible with the forwarded constructor arguments.
 * @tparam Args Owning constructor argument types, following the existing custom factory contract.
 * @param reference Empty observing slot published after complete mount.
 * @param args Constructor arguments forwarded into owning recipe storage.
 * @return Existing custom recipe with deferred reference publication.
 */
template <class T, class... Args> Description<T> custom(Ref<T> reference, Args &&...args) {
  return custom<T>(std::forward<Args>(args)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty layout slot published after complete mount.
 * @param children Owning child recipes.
 * @return Existing column recipe with one additional observation.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(Ref<Column> reference, Children &&...children) {
  return column(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty layout slot published after complete mount.
 * @param children Owning child recipes.
 * @return Existing row recipe with one additional observation.
 */
template <detail::DescriptionInput... Children> Description<Row> row(Ref<Row> reference, Children &&...children) {
  return row(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty grid slot published after complete mount.
 * @param children Owning child recipes; placement remains explicit after mount.
 * @return Existing grid recipe with one additional observation.
 */
template <detail::DescriptionInput... Children> Description<Grid> grid(Ref<Grid> reference, Children &&...children) {
  return grid(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes.
 * @param reference Empty row observation published after complete mount.
 * @param options Existing owning linear layout options.
 * @param children Owning child recipes, mounted in argument order.
 * @return Existing row recipe retaining its layout configuration.
 */
template <detail::DescriptionInput... Children>
Description<Row> row(Ref<Row> reference, LinearOptions options, Children &&...children) {
  return row(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes.
 * @param reference Empty column observation published after complete mount.
 * @param options Existing owning linear layout options.
 * @param children Owning child recipes, mounted in argument order.
 * @return Existing column recipe retaining its layout configuration.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(Ref<Column> reference, LinearOptions options, Children &&...children) {
  return column(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam T Control subtype compatible with the existing caption constructor.
 * @param reference Empty control observation published after complete mount.
 * @param text UTF-8 caption copied into owning storage.
 * @param style Existing manual appearances.
 * @param policy Existing layout constraints.
 * @return Existing control recipe with additive reference publication.
 */
template <class T>
Description<T> control(Ref<T> reference, std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<T>(text, style, policy).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty view observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param columns Column descriptions copied by the original factory.
 * @param options Existing table configuration.
 * @param policy Existing layout constraints.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing table recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<VirtualTable<T>> virtualTable(Ref<VirtualTable<T>> reference, ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          VirtualTableOptions options = {}, SizePolicy policy = {},
                                          std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return virtualTable(model, columns, options, policy, std::move(prepared)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty view observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param delegate Owning row formatter retained by the original recipe.
 * @param options Existing list configuration.
 * @param policy Existing layout constraints.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing list recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<VirtualList<T>> virtualList(Ref<VirtualList<T>> reference, ManagedModel<T> model,
                                        std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        VirtualListOptions options = {}, SizePolicy policy = {},
                                        std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return virtualList(model, std::move(delegate), options, policy, std::move(prepared)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty chart observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param accessor Owning OHLC projection retained by the original recipe.
 * @param options Existing chart configuration.
 * @param first Owning origin callback; empty starts at row zero.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing chart recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<Chart<T>> chart(Ref<Chart<T>> reference, ManagedModel<T> model,
                            std::type_identity_t<ManagedChartAccessor<T>> accessor, ChartOptions options = {},
                            ManagedChartOrigin first = {}, std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return chart(model, std::move(accessor), options, std::move(first), std::move(prepared))
    .reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original VirtualTableConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param columns Owning column descriptions copied by the original factory.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named table recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualTableConfig<T>>
Description<VirtualTable<T>> virtualTable(Ref<VirtualTable<T>> reference, ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          Config &&config) {
  return virtualTable(model, columns, std::forward<Config>(config)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original VirtualListConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param delegate Owning row formatter retained by the original recipe.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named list recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualListConfig<T>>
Description<VirtualList<T>> virtualList(Ref<VirtualList<T>> reference, ManagedModel<T> model,
                                        std::type_identity_t<ManagedListFormatter<T>> delegate, Config &&config) {
  return virtualList(model, std::move(delegate), std::forward<Config>(config)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original ChartConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param accessor Owning OHLC projection retained by the original recipe.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named chart recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, ChartConfig<T>>
Description<Chart<T>> chart(Ref<Chart<T>> reference, ManagedModel<T> model,
                            std::type_identity_t<ManagedChartAccessor<T>> accessor, Config &&config) {
  return chart(model, std::move(accessor), std::forward<Config>(config)).reference(std::move(reference));
}

/** @brief Consume a recipe into the existing managed subtree, with failure cleanup.
 * @tparam T Concrete root Widget type constructed by the recipe.
 * @param scene Existing UI-affine owner, outliving all mounted widgets.
 * @param description Owning rvalue recipe, consumed even when mounting fails.
 * @return Observing root or original typed failure plus independent cleanup error. Earlier
 * scene nodes and states survive. Reusing an empty/moved/consumed recipe returns INVALID_ARGUMENT.
 * References publish only after all nodes remain live. External callback effects are not undone.
 */
template <class T> std::expected<Node<T>, ManagedError> mount(ManagedScene &scene, Description<T> &&description) try {
  struct Mounting {
    ManagedScene &scene;

    ~Mounting() {
      detail::Access::endMount(scene);
    }
  } guard{scene};

  detail::Access::beginMount(scene);
  detail::MountContext context;
  std::expected<Node<T>, ManagedError> result =
    std::unexpected(detail::error(scene.root(), TreeError::INVALID_ARGUMENT));

  try {
    result = detail::Access::mount(std::move(description), scene, scene.root(), context);

    if (result) {
      for (auto &finalize : context.finalize) {
        auto completed = finalize();
        if (!completed) {
          result = std::unexpected(completed.error());
          break;
        }
      }
    }
    if (result) {
      for (auto node : context.nodes) {
        auto live = detail::Access::check(scene, node);

        if (!live) {
          result = std::unexpected(live.error());

          break;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    result = std::unexpected(detail::error(context.root, TreeError::NO_MEMORY));
  } catch (...) {
    result = std::unexpected(detail::error(context.root, TreeError::CALLBACK_FAILED));
  }

  if (!result && context.root != Node<Widget>{}) {
    auto removed = scene.remove(context.root);

    if (!removed) {
      if (const auto *cause = std::get_if<TreeError>(&removed.error().cause)) {
        if (*cause != TreeError::STALE_NODE && *cause != TreeError::CLOSED) {
          result.error().cleanup = *cause;
        }
      }
    }
  }

  if (result) {
    context.committed = true;
    for (auto &publish : context.publish) {
      publish();
    }
  }

  return result;
} catch (const std::bad_alloc &) {
  // Transaction storage is constructed before any node exists; preserve the checked allocation boundary too.
  auto consumed = std::move(description);

  return std::unexpected(detail::error(scene.root(), TreeError::NO_MEMORY));
}

/** @tparam Args Arguments accepted by an existing row overload, including references and layout options.
 * @param args Owning child recipes and optional row configuration, forwarded unchanged.
 * @return The existing horizontal row recipe; ownership, checked failures and layout semantics are unchanged.
 */
template <class... Args>
  requires requires(Args &&...args) { row(std::forward<Args>(args)...); }
auto hbox(Args &&...args) {
  return row(std::forward<Args>(args)...);
}

/** @tparam Args Arguments accepted by an existing column overload, including references and layout options.
 * @param args Owning child recipes and optional column configuration, forwarded unchanged.
 * @return The existing vertical column recipe; ownership, checked failures and layout semantics are unchanged.
 */
template <class... Args>
  requires requires(Args &&...args) { column(std::forward<Args>(args)...); }
auto vbox(Args &&...args) {
  return column(std::forward<Args>(args)...);
}

/** @brief Owning compact size stage; uses the same methods and existing solver as ordinary composition. */
struct SizeModifier {
  detail::SizeChange change; ///< Owning checked policy operation; no scene/widget borrow.

  /** @tparam T SizedWidget subtype described by the consuming input.
   * @param description Owning recipe consumed by this stage.
   * @param modifier Owning axis/weight update, applied in pipe order at mount.
   * @return Updated owning recipe; invalid values preserve transactional mount failure semantics.
   */
  template <std::derived_from<SizedWidget> T>
  friend Description<T> operator|(Description<T> &&description, SizeModifier modifier) {
    switch (modifier.change.dimension) {
    case detail::SizeDimension::WIDTH:
      return std::move(description).width(modifier.change.extent);
    case detail::SizeDimension::HEIGHT:
      return std::move(description).height(modifier.change.extent);
    case detail::SizeDimension::FLEX_X:
      return std::move(description).flexX(modifier.change.weight);
    case detail::SizeDimension::FLEX_Y:
      return std::move(description).flexY(modifier.change.weight);
    case detail::SizeDimension::FLEX:
      return std::move(description).flex(modifier.change.weight);
    }

    return std::move(description).flex(0);
  }
};

/** @param cells Nonnegative fixed horizontal extent; checked at mount.
 * @return Owning size stage replacing only the horizontal policy.
 */
constexpr SizeModifier width(std::int32_t cells) {
  return {{detail::SizeDimension::WIDTH, cells}};
}

/** @param cells Nonnegative fixed vertical extent; checked at mount.
 * @return Owning size stage replacing only the vertical policy.
 */
constexpr SizeModifier height(std::int32_t cells) {
  return {{detail::SizeDimension::HEIGHT, cells}};
}

/** @param weight Positive horizontal flex weight; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds and vertical policy.
 */
constexpr SizeModifier flexX(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX_X, 0, weight}};
}

/** @param weight Positive vertical flex weight; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds and horizontal policy.
 */
constexpr SizeModifier flexY(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX_Y, 0, weight}};
}

/** @param weight Positive weight for both axes; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds.
 */
constexpr SizeModifier flex(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX, 0, weight}};
}

template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Child &&child, ScrollAreaOptions options,
                                   std::shared_ptr<ScrollController> controller, ControlStyle style) {
  auto description = detail::Access::describe<ScrollArea>(
    [options, controller = std::move(controller), style](ManagedScene &scene, Node<Widget> parent) {
      return scene.emplace<ScrollArea>(parent, controller, scene.widthPolicy(), options, style);
    });
  description.state_->children.push_back(
    [child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> parent,
                                         detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
      auto mounted = detail::Access::mount(std::move(child), scene, parent, context);
      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    });
  return description;
}

/** @tparam Child Consuming ordinary content recipe.
 * @param reference Empty area slot, published only after the complete subtree mounts.
 * @param child Single ordinary content root.
 * @param options Owning placement/input settings.
 * @param controller Optional owning shared values.
 * @param style Manual appearance defaults.
 * @return Ref-first recipe with automatic lifecycle and checked rollback.
 */
template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Ref<ScrollArea> reference, Child &&child, ScrollAreaOptions options = {},
                                   std::shared_ptr<ScrollController> controller = {}, ControlStyle style = {}) {
  return scrollArea(std::forward<Child>(child), options, std::move(controller), style).reference(std::move(reference));
}

/** @param axis Projected axis.
 * @param options Owning input/paint settings.
 * @param style Manual defaults beneath themes.
 * @return Standalone bar with automatic default controller and sole metrics-writer lease.
 */
inline Description<ScrollBar> scrollBar(ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return detail::Access::describe<ScrollBar>([axis, options, style](ManagedScene &scene, Node<Widget> parent) {
    return scene.scrollBar(parent, {}, axis, options, style);
  });
}

/** @param controller Owning values shared with an area's metrics writer.
 * @param axis Projected axis.
 * @param options Owning input/paint settings.
 * @param style Manual defaults beneath themes.
 * @return Read projection; a missing controller is rejected rather than creating orphan metrics.
 */
inline Description<ScrollBar> scrollBar(std::shared_ptr<ScrollController> controller,
                                        ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return detail::Access::describe<ScrollBar>(
    [controller = std::move(controller), axis, options,
     style](ManagedScene &scene, Node<Widget> parent) -> std::expected<Node<ScrollBar>, ManagedError> {
      if (!controller) {
        return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
      }
      return scene.scrollBar(parent, controller, axis, options, style, false);
    });
}

/** @param reference Empty bar observer published after successful mount.
 * @param axis Selected standalone axis.
 * @param options Owning bar settings.
 * @param style Manual defaults.
 * @return Ref-first standalone writer recipe.
 */
inline Description<ScrollBar> scrollBar(Ref<ScrollBar> reference, ScrollAxis axis = ScrollAxis::VERTICAL,
                                        ScrollBarOptions options = {}, ControlStyle style = {}) {
  return scrollBar(axis, options, style).reference(std::move(reference));
}

/** @param reference Empty observer published after successful mount.
 * @param controller Owning area-shared values.
 * @param axis Selected projection axis.
 * @param options Owning bar settings.
 * @param style Manual defaults.
 * @return Ref-first area projection recipe with automatic binding and cleanup.
 */
inline Description<ScrollBar> scrollBar(Ref<ScrollBar> reference, std::shared_ptr<ScrollController> controller,
                                        ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return scrollBar(std::move(controller), axis, options, style).reference(std::move(reference));
}

template <detail::DescriptionInput Child>
Description<Border> border(Child &&child, BorderOptions options, ControlStyle style) {
  auto description = detail::Access::describe<Border>([options, style](ManagedScene &scene, Node<Widget> parent) {
    const auto valid = Border::validate(options);

    if (!valid) {
      return std::expected<Node<Border>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
    }

    return scene.emplace<Border>(parent, scene.widthPolicy(), options, style);
  });

  description.state_->children.push_back(
    [child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> parent,
                                         detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
      const auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    });

  return description;
}

/** @tparam Child Consuming Description type.
 * @param child Owning child mounted within the frame transaction.
 * @param options Owning frame glyph/paint-work policy.
 * @param style Owning manual appearances beneath themes.
 * @return Same owning frame recipe as border, with identical layout and failure semantics.
 */
template <detail::DescriptionInput Child>
Description<Border> frame(Child &&child, BorderOptions options = {}, ControlStyle style = {}) {
  return border(std::forward<Child>(child), options, style);
}

/** @brief Owning frame pipe stage over the same transactional border factory. */
struct BorderModifier {
  BorderOptions options; ///< Owning glyph/work settings.
  ControlStyle style;    ///< Owning manual appearances beneath themes.

  /** @tparam T Widget subtype described by the consuming input.
   * @param description Owning child recipe consumed by the frame.
   * @param modifier Owning glyph, style and finite work settings.
   * @return Owning frame containing the input in the same mount transaction.
   */
  template <class T> friend Description<Border> operator|(Description<T> &&description, BorderModifier modifier) {
    return border(std::move(description), modifier.options, modifier.style);
  }
};

/** @param options Owning frame glyph selection and finite paint cap.
 * @param style Owning manual appearances beneath themes.
 * @return Frame stage for a consuming description; no view/widget borrow is retained.
 */
inline BorderModifier border(BorderOptions options = {}, ControlStyle style = {}) {
  return {options, style};
}

/** @param options Owning frame glyph selection and finite paint cap.
 * @param style Owning manual appearances beneath themes.
 * @return The same frame stage as border(options, style).
 */
inline BorderModifier frame(BorderOptions options = {}, ControlStyle style = {}) {
  return {options, style};
}

/** @param value Finite value in the inclusive configured range; checked at mount.
 * @param options Owning finite bounds, intrinsic width, glyph mode and allocated-area paint cap.
 * @param style Owning manual appearances beneath theme declarations.
 * @param policy Owning layout constraints, validated by the existing solver.
 * @return Single-use determinate gauge recipe; invalid values/settings preserve typed mount errors.
 */
inline Description<Gauge> gauge(double value, GaugeOptions options = {}, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return detail::Access::describe<Gauge>([value, options, style, policy](ManagedScene &scene, Node<Widget> parent) {
    const auto valid = Gauge::validate(value, options);

    if (!valid) {
      return std::expected<Node<Gauge>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
    }

    return scene.emplace<Gauge>(parent, value, scene.widthPolicy(), options, style, policy);
  });
}

/** @param text UTF-8 caption copied now and checked at mount under the receiving WidthPolicy.
 * @param style Owning manual normal/focused/disabled appearances beneath themes.
 * @param policy Owning layout constraints, checked by the existing solver.
 * @return Existing Button recipe without brackets; activation still uses existing bindings/router.
 */
inline Description<Button> plainButton(std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::describe<Button>(
    [text = std::string(text), style, policy](ManagedScene &scene, Node<Widget> parent) {
      auto caption = GraphemeText::from(text, scene.widthPolicy());

      if (!caption) {
        auto failure = detail::textError(parent, caption.error());

        return std::expected<Node<Button>, ManagedError>(std::unexpected(failure));
      }

      return scene.emplace<Button>(parent, std::move(*caption), style, policy, ButtonChrome::NONE);
    });
}

/** @tparam Callback Existing compact action accepting optionally const ClickEvent&.
 * @param text UTF-8 caption copied immediately.
 * @param action Owning action transferred to the existing managed click binding at mount.
 * @param style Owning manual appearances beneath themes.
 * @param policy Owning layout constraints.
 * @return Existing unbracketed Button recipe; action errors preserve their checked boundary/provenance.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
Description<Button> plainButton(std::string_view text, Callback action, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return plainButton(text, style, policy).onClick(std::move(action));
}

/** @param reference Empty shared observing slot, published only after complete mount.
 * @param text UTF-8 caption copied immediately.
 * @param style Owning manual appearances beneath themes.
 * @param policy Owning layout constraints.
 * @return Existing unbracketed Button recipe sharing normal transactional reference publication.
 */
inline Description<Button> plainButton(Ref<Button> reference, std::string_view text, ControlStyle style = {},
                                       SizePolicy policy = {}) {
  return plainButton(text, style, policy).reference(std::move(reference));
}

namespace detail {
/** @brief Admit only owning descriptions or their exact managed checked result.
 * @tparam T Factory result after cv/ref normalization.
 */
template <class T> struct IsSceneRecipe : IsDescription<T> {};

/** @brief Admit a factory preserving an original managed error.
 * @tparam T Concrete widget constructed by the returned description.
 */
template <class T> struct IsSceneRecipe<std::expected<Description<T>, ManagedError>> : std::true_type {};

/** @brief Select the legacy factory signature before considering an Application borrow.
 * @tparam Factory Owning scene factory returning an owning description or exact checked description.
 */
template <class Factory>
concept SceneFactory =
  (std::invocable<Factory &, ManagedScene &> && !std::is_reference_v<std::invoke_result_t<Factory &, ManagedScene &>> &&
   IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &, ManagedScene &>>>::value) ||
  (!std::invocable<Factory &, ManagedScene &> && std::invocable<Factory &, ManagedScene &, Application &> &&
   !std::is_reference_v<std::invoke_result_t<Factory &, ManagedScene &, Application &>> &&
   IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &, ManagedScene &, Application &>>>::value);

/** @brief Deliver a pre-owner factory failure through the already available final sink.
 * @param sink Owning handler borrowed for this synchronous call; no copy is required on the error path.
 * @param failure Original typed cause, independent of terminal/widget lifetime.
 * @param origin Factory/setup invocation site.
 */
inline void reportFactoryFailure(const SceneReportHandler &sink, ManagedError failure,
                                 std::source_location origin) noexcept {
  SetupFailureScope reportingScope({}, nullptr, nullptr);
  SceneReport report;

  if (!failure.operation) {
    failure.operation = "scene.factory";
    failure.operationOrigin = origin;
  }

  failure.execution = "scene.factory";

  report.failure = std::move(failure);
  report.origin = report.failure->operationOrigin.value_or(origin);

  try {
    if (sink) {
      sink(report);
    } else {
      reportToStderr(report);
    }
  } catch (...) {
    reportToStderr(report);
  }
}

/** @brief Admit exact stationary setup results.
 * @tparam Setup Callable taking Scene& and returning void or exact expected<void,ManagedError>.
 */
template <class Setup>
concept AppSetup = requires(Setup &setup, Scene &owner) {
  { setup(owner) } -> std::same_as<void>;
} || requires(Setup &setup, Scene &owner) {
  { setup(owner) } -> std::same_as<std::expected<void, ManagedError>>;
};

} // namespace detail

/** @tparam T Concrete Widget type described by the consuming recipe.
 * @param description Owning root recipe consumed once by prepare; creation/capture allocation may throw.
 * @param options Existing runtime policies and bounded pending-configuration budget.
 * @param origin Construction call site retained across lazy mount.
 * @return UI-affine lazy facade; neither mount nor terminal I/O occurs during construction.
 */
template <class T>
Scene scene(Description<T> &&description, SceneOptions options = {},
            std::source_location origin = std::source_location::current()) {
  return detail::SceneAccess::create(
    [description = std::move(description)](ManagedScene &owner) mutable -> std::expected<void, ManagedError> {
      auto mounted = mount(owner, std::move(description));

      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    },
    options, origin);
}

/** @tparam Factory Owning callable returning Description<T> or expected<Description<T>, ManagedError>.
 * @param factory Invoked once at prepare with ManagedScene&, optionally followed by Application&;
 * the one-argument form has priority. May create states/models and application-backed services.
 * Explicitly borrowed services must outlive their use; construction/capture allocation may throw.
 * Application remains UI-only and facade-owned. It supplies UI services; run this owner through Scene,
 * never
 * Application::run directly from factory, configuration or shutdown. Stop external work with onStop before managed
 * teardown, and release borrowed services before the facade is destroyed.
 * @param options Existing runtime policies and finite facade limits.
 * @param origin Factory registration call site retained in mount diagnostics.
 * @return UI-affine lazy facade; the factory does not execute during construction.
 */
template <detail::SceneFactory Factory>
Scene scene(Factory factory, SceneOptions options = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneAccess::create(
    [factory = std::move(factory)](ManagedScene &owner) mutable -> std::expected<void, ManagedError> {
      auto recipe = [&] {
        if constexpr (std::invocable<Factory &, ManagedScene &>) {
          return std::invoke(factory, owner);
        } else {
          return std::invoke(factory, owner, detail::SceneAccess::application(owner));
        }
      }();

      if constexpr (detail::IsDescription<decltype(recipe)>::value) {
        auto mounted = mount(owner, std::move(recipe));

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      } else {
        if (!recipe) {
          return std::unexpected(recipe.error());
        }

        auto mounted = mount(owner, std::move(*recipe));

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      }
    },
    options, origin);
}

namespace detail {
/** @brief Owning minimal pipe adapter forwarding to the same facade method.
 * @tparam Action Callable accepting Scene&; no separate configuration implementation.
 */
template <class Action> struct SceneStage {
  Action action; ///< Owning method adapter; explicitly borrowed services remain borrowed.

  /** @param owner Stationary facade; no ownership move occurs.
   * @param stage Borrowed owning adapter.
   * @return The same owner reference after exactly one dispatch.
   */
  friend Scene &operator|(Scene &owner, SceneStage &stage) {
    stage.action(owner);

    return owner;
  }

  /** @param owner Stationary facade.
   * @param stage Temporary owning adapter.
   * @return The same owner reference after exactly one dispatch.
   */
  friend Scene &operator|(Scene &owner, SceneStage &&stage) {
    stage.action(owner);

    return owner;
  }

  /** @param input Consumed idle facade; move admission precedes action dispatch.
   * @param stage Borrowed owning adapter.
   * @return Owning facade preserving the existing early move guard.
   */
  friend Scene operator|(Scene &&input, SceneStage &stage) {
    Scene owner(std::move(input));

    stage.action(owner);

    return owner;
  }

  /** @param input Consumed idle facade; move admission precedes action dispatch.
   * @param stage Temporary owning adapter.
   * @return Owning facade preserving the existing early move guard.
   */
  friend Scene operator|(Scene &&input, SceneStage &&stage) {
    Scene owner(std::move(input));

    stage.action(owner);

    return owner;
  }
};

/** @brief Terminal pipe adapter; the borrowed transport is optional for native run. */
struct RunStage {
  ApplicationTerminal *terminal; ///< Borrowed transport, or null for the native run overload.
  std::stop_token stop;          ///< Existing runner cancellation.
  std::source_location origin;   ///< Pipe registration call site.

  /** @param owner Consumed idle facade.
   * @param stage Runner selection and call site.
   * @return Same terminal checked result as Scene::run, including explicit cleanup.
   */
  friend std::expected<ApplicationStats, ManagedError> operator|(Scene owner, RunStage stage) {
    return stage.terminal ? owner.run(*stage.terminal, stage.stop, stage.origin) : owner.run(stage.stop, stage.origin);
  }
};

/** @brief Final int stage over the same runApp boundary; transport remains borrowed. */
struct RunAppStage {
  ApplicationTerminal *terminal; ///< Borrowed transport, or null for native terminal ownership.
  std::stop_token stop;          ///< Optional worker-safe cancellation.
  std::source_location origin;   ///< Final stage call site.

  /** @param owner Borrowed lvalue scene or live temporary; it is never moved by this stage.
   * @param stage Transport/cancellation selection.
   * @return Same zero/one status and final diagnostic delivery as Scene::runApp.
   */
  friend int operator|(Scene &owner, RunAppStage stage) {
    return stage.terminal ? owner.runApp(*stage.terminal, stage.stop, stage.origin)
                          : owner.runApp(stage.stop, stage.origin);
  }

  /** @param owner Live temporary scene; its lifetime covers the entire run and reporting.
   * @param stage Final runner selection.
   * @return Same status as the lvalue overload, without relocating callback self-references.
   */
  friend int operator|(Scene &&owner, RunAppStage stage) {
    return owner | stage;
  }
};

/** @brief Lazy command-line preflight followed by the existing final runner boundary. */
struct CommandLineRunStage {
  int argc{};                      ///< Input argument count, including program name.
  const char *const *argv{};       ///< Borrowed input array, copied at consumption.
  CommandLineParser parser;        ///< Owning parser adapter; bound options remain caller-owned.
  ApplicationTerminal *terminal{}; ///< Optional borrowed custom transport.
  std::stop_token stop;            ///< Optional worker-safe cancellation.
  std::source_location origin;     ///< Final stage call site.

  /** @param owner Existing UI-affine lazy scene.
   * @param stage Single-use command-line stage.
   * @return Zero for success/help, two for launch errors, one for runtime failure.
   */
  friend int operator|(Scene &owner, CommandLineRunStage stage) {
    return stage.terminal
             ? owner.runApp(*stage.terminal, stage.argc, stage.argv, std::move(stage.parser), stage.stop, stage.origin)
             : owner.runApp(stage.argc, stage.argv, std::move(stage.parser), stage.stop, stage.origin);
  }

  /** @param owner Live temporary scene, kept in place throughout preflight and run.
   * @param stage Owning parser and borrowed transport selection.
   * @return Same status as the lvalue overload, without moving the scene.
   */
  friend int operator|(Scene &&owner, CommandLineRunStage stage) {
    return owner | std::move(stage);
  }
};

} // namespace detail

/** @param origin Preparation stage call site.
 * @return Owning stage forwarding to Scene::prepare without terminal I/O.
 */
inline auto prepare(std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[origin](Scene &owner) {
    owner.prepare(origin);
  }};
}

/** @param sheet Owning replacement sheet captured before entry; construction may throw.
 * @param origin Theme stage registration call site.
 * @return Consuming stage forwarding to Scene::theme.
 */
inline auto theme(StyleSheet sheet, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[sheet = std::move(sheet), origin](Scene &owner) mutable {
    detail::SceneAccess::theme(owner, sheet, origin);
  }};
}

/** @param options Aggregate logical layer budgets, including permanent base slots.
 * @param origin Configuration call site retained on failure.
 * @return Consuming opt-in stage over the existing managed style engine; no second runtime.
 */
inline auto styleLayers(StyleLayerOptions options = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[options, origin](Scene &owner) {
    owner.styleLayers(options, origin);
  }};
}

/** @param tier APPLICATION or USER override tier; enable styleLayers before this stage.
 * @param name Owning tier-unique name retained until removal or scene cleanup.
 * @param sheet Owning rules, independent of theme replacement.
 * @param origin Configuration call site retained on typed failure.
 * @return Consuming stage; the scene owns the layer without client token storage.
 * Constructing stage captures may throw before the checked facade entry.
 */
inline auto styleLayer(StyleTier tier, std::string name, StyleSheet sheet,
                       std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[tier, name = std::move(name), sheet = std::move(sheet), origin](Scene &owner) {
    detail::SceneAccess::styleLayer(owner, tier, name, sheet, origin);
  }};
}

/** @param observer Owning observer, never a veto; capture construction may throw.
 * @param origin Observer stage registration call site.
 * @return Consuming stage forwarding to Scene::onError.
 */
inline auto onError(ManagedErrorHandler observer, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[observer = std::move(observer), origin](Scene &owner) mutable {
    owner.onError(std::move(observer), origin);
  }};
}

/** @param sink Owning copyable callable receiving const SceneReport&, or empty for default stderr.
 * @param origin Reporting stage registration call site.
 * @return Consuming stage forwarding to Scene::reportOnError; independent of onError order.
 */
inline auto reportOnError(SceneReportHandler sink = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[sink = std::move(sink), origin](Scene &owner) mutable {
    owner.reportOnError(std::move(sink), origin);
  }};
}

/** @tparam Callback Owning void/expected<void, ManagedError> configuration.
 * @param callback Existing facade configuration action; capture construction may throw.
 * @param origin Stage registration call site.
 * @return Consuming stage forwarding to Scene::configure.
 */
template <detail::SceneConfiguration Callback>
auto configure(Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[callback = std::move(callback), origin](Scene &owner) mutable {
    detail::SceneAccess::configure(owner, callback, origin);
  }};
}

/** @tparam Callback Existing compact key action preserving typed command receipts.
 * @param chord Exact key/scalar/modifiers admitted for PRESS and REPEAT.
 * @param callback Owning action, optionally receiving KeyEvent; capture construction may throw.
 * @param origin Stage registration call site.
 * @return Consuming stage forwarding to Scene::onKey and its existing bounded composition.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeyEvent chord, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[chord, callback = std::move(callback), origin](Scene &owner) mutable {
    detail::SceneAccess::onKey(owner, chord, callback, origin);
  }};
}

/** @tparam Source String-view-convertible text, chord or owning Shortcut normalized during this call.
 * @param source
 * Borrowed input copied or moved into immutable owned alternatives before return.
 * @param actions Nonempty action
 * set; duplicates are idempotent, invalid values retain their index.
 * @param options Positive finite normalization
 * budgets.
 * @return Owning selection or deferred ShortcutError; onKey supplies the registration origin.
 * The action set applies independently to every sequence stroke without synthesizing phases.
 */
template <portals::detail::ShortcutSource Source>
KeySelection keyActions(Source &&source, std::initializer_list<KeyAction> actions, ShortcutOptions options = {}) {
  return portals::detail::KeySelectionAccess::make(
    portals::detail::prepareShortcut(std::forward<Source>(source), options), actions, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param actions Nonempty action set,
 * with duplicates admitted once.
 * @param options Positive finite normalization budgets.
 * @return Immutable owning
 * selection or deferred typed failure.
 */
inline KeySelection keyActions(std::initializer_list<key::Chord> source, std::initializer_list<KeyAction> actions,
                               ShortcutOptions options = {}) {
  return portals::detail::KeySelectionAccess::make(portals::detail::prepareShortcut(source, options), actions, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting PRESS only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source> KeySelection press(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::PRESS}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting PRESS only, or deferred typed failure.
 */
inline KeySelection press(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::PRESS}, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting REPEAT only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source> KeySelection repeat(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::REPEAT}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting REPEAT only, or deferred typed failure.
 */
inline KeySelection repeat(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::REPEAT}, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting RELEASE only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source> KeySelection release(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::RELEASE}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting RELEASE only, or deferred typed failure.
 */
inline KeySelection release(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::RELEASE}, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting PRESS or REPEAT only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source>
KeySelection pressRepeat(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::PRESS, KeyAction::REPEAT}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting PRESS or REPEAT only, or deferred typed failure.
 */
inline KeySelection pressRepeat(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::PRESS, KeyAction::REPEAT}, options);
}

/** @tparam Callback Existing owning compact key action, optionally taking const KeyEvent&.
 * @param selection
 * Immutable owning alternatives and exact selected native phases.
 * @param callback Action stored until checked stage
 * consumption; caller construction/moves may throw.
 * @param origin Registration call site, independent of helper
 * construction.
 * @return Consuming stage using the existing bounded newest-first registration.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeySelection selection, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{
    [selection = std::move(selection), callback = std::move(callback), origin](Scene &owner) mutable {
      detail::SceneAccess::onSelection(owner, selection, callback, origin);
    }};
}

/** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
 * @tparam Callback
 * Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Source normalized during stage
 * construction; caller text is never retained.
 * @param callback Action stored until checked stage consumption; caller
 * callback construction/moves may throw.
 * @param origin Registration site preserved across deferred configuration.
 *
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <portals::detail::ShortcutSource Source, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(Source &&source, Callback callback, std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), {}));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, {}, {}, origin);
    }};
}

/** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
 * @tparam Callback Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Source normalized during stage construction; caller text is never retained.
 * @param callback Action stored until checked stage consumption; caller callback construction/moves may throw.
 * @param behavior PRESS-only default; repeat requires explicit opt-in.
 * @param options Positive finite parser budgets.
 * @param origin Registration site preserved across deferred configuration.
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <portals::detail::ShortcutSource Source, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(Source &&source, Callback callback, ShortcutBehavior behavior, ShortcutOptions options = {},
           std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), options));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), behavior, options, origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, behavior, options, origin);
    }};
}

/** @tparam Callback Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Alternatives copied during stage construction.
 * @param callback Action stored until checked stage consumption; caller callback construction/moves may throw.
 * @param origin Registration site preserved across deferred configuration.
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(std::initializer_list<key::Chord> source, Callback callback,
           std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), {}));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, {}, {}, origin);
    }};
}

/** @tparam Callback Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Alternatives copied during stage construction.
 * @param callback Action stored until checked stage consumption; caller callback construction/moves may throw.
 * @param behavior PRESS-only default; repeat requires explicit opt-in.
 * @param options Positive finite parser budgets.
 * @param origin Registration site preserved across deferred configuration.
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(std::initializer_list<key::Chord> source, Callback callback, ShortcutBehavior behavior,
           ShortcutOptions options = {}, std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), options));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), behavior, options, origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, behavior, options, origin);
    }};
}

/** @tparam Callback Owning no-argument or ClickParams callback returning void, checked TreeError or editor receipt.
 * @param reference Button observation published by this scene's recipe; no scene/widget ownership is retained.
 * @param callback Owning callback invoked FIFO with other pipe callbacks, including modified activations.
 * @param origin Registration site retained through deferred mount and input diagnostics.
 * @return Owning pipe stage over one existing managed/router binding. stop/stopPropagation end the remaining
 * chain; the button already consumes ancestor propagation. Failure stops the chain without rollback.
 * Callback parameters are borrowed synchronously; copies retain an owning event snapshot only.
 * Runtime append affects the next event; a new binding during dispatch returns existing WOULD_BLOCK.
 * UI affinity, finite ManagedOptions limits and sticky facade failures apply. Capture construction may throw.
 */
template <class Callback = detail::QuitCommand>
  requires detail::ClickCallback<Callback>
auto onClick(Ref<Button> reference, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::ClickAccess::attach(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onClick(owner, registration, origin);
  }};
}

/** @tparam T Observed widget type.
 * @tparam Callback Owning compact action accepting geometry or no arguments.
 * @param reference Ref published by this scene's description.
 * @param callback Void, checked TreeError or EditorCommandResult action retained by the scene.
 * @param origin Registration site retained in diagnostics.
 * @return Scene pipe stage registering deferred final local/surface/clip observations.
 * Callbacks run on UI outside tree frames. Equal bounds do not notify or request redraw.
 */
template <class T, class Callback>
  requires portals::detail::ManagedAction<Callback, NodeGeometry>
auto onLayout(Ref<T> reference, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::LayoutAccess::attach(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onLayout(owner, registration, origin);
  }};
}

/** @tparam Factory No-argument callable returning Description<T> or exact expected<Description<T>,ManagedError>.
 * @tparam Setup Callable taking the stationary Scene&, returning void or exact expected<void,ManagedError>.
 * @param factory Invoked inside the checked construction boundary, after final sink availability.
 * @param setup Invoked on one stationary facade; capture SceneRef for later callbacks across facade moves.
 * @param options Existing scene/runner policies, borrowed terminal, owning final sink and cancellation.
 * @param origin Upper runner invocation site retained in typed failures.
 * @return Zero after successful run, one for factory/construction/setup/run failure after cleanup.
 * Callable and argument construction before actual invocation can throw outside this boundary.
 * Factory must not return Scene. Setup exceptions map to NO_MEMORY or CALLBACK_FAILED.
 * A rejected short mutation inside setup latches its original typed cause before observers; the current void
 * callback finishes, while later setup/run is skipped. Unbound targets can be attributed to this entered scope.
 */
template <class Factory, detail::AppSetup Setup>
  requires std::invocable<Factory &> && (!std::is_reference_v<std::invoke_result_t<Factory &>>) &&
           detail::IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &>>>::value
int runApp(Factory &&factory, Setup &&setup, const RunAppOptions &options = {},
           std::source_location origin = std::source_location::current()) {
  detail::SetupFailureScope invocationScope({}, nullptr, nullptr);

  try {
    auto description = std::invoke(factory);

    auto create = [&]() {
      if constexpr (detail::IsDescription<std::remove_cvref_t<decltype(description)>>::value) {
        return scene(std::move(description), options.scene, origin);
      } else {
        return scene(std::move(*description), options.scene, origin);
      }
    };

    if constexpr (!detail::IsDescription<std::remove_cvref_t<decltype(description)>>::value) {
      if (!description) {
        detail::reportFactoryFailure(options.report, description.error(), origin);

        return 1;
      }
    }

    // This owner stays stationary during setup and run; callbacks observe stable State through SceneRef.
    auto owner = create();

    owner.reportOnError(options.report, origin);

    if (owner.checked()) {
      std::optional<ManagedError> setupFailure;

      {
        auto setupScope = detail::SceneAccess::setupScope(owner);
        auto currentScope = detail::SceneAccess::currentScope(owner);

        try {
          if constexpr (std::same_as<std::invoke_result_t<Setup &, Scene &>, void>) {
            std::invoke(setup, owner);
          } else {
            auto configured = std::invoke(setup, owner);

            if (!configured) {
              setupFailure = configured.error();
            }
          }
        } catch (const std::bad_alloc &) {
          setupFailure = ManagedError{ManagedPhase::BIND, {}, TreeError::NO_MEMORY, {}, {}};
        } catch (...) {
          setupFailure = ManagedError{ManagedPhase::BIND, {}, TreeError::CALLBACK_FAILED, {}, {}};
        }
      }

      // Finalize the short-error scope before cleanup can revoke its weak delivery publication.
      if (setupFailure) {
        detail::SceneAccess::setupFailure(owner, *setupFailure, origin);
      }
    }

    return options.terminal ? owner.runApp(*options.terminal, options.stop, origin)
                            : owner.runApp(options.stop, origin);
  } catch (const std::bad_alloc &) {
    detail::reportFactoryFailure(options.report, ManagedError{ManagedPhase::MOUNT, {}, TreeError::NO_MEMORY, {}, {}},
                                 origin);
  } catch (...) {
    detail::reportFactoryFailure(options.report,
                                 ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED, {}, {}}, origin);
  }

  return 1;
}

/** @tparam Factory No-argument checked owning description factory, never a Scene factory.
 * @param factory Invoked inside the checked upper construction boundary.
 * @param options Existing runtime policies and final sink, available before factory invocation.
 * @param origin Upper invocation site.
 * @return Same zero/one status and cleanup as the stationary setup overload with no setup action.
 */
template <class Factory>
  requires std::invocable<Factory &> && (!std::is_reference_v<std::invoke_result_t<Factory &>>) &&
           detail::IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &>>>::value
int runApp(Factory &&factory, const RunAppOptions &options = {},
           std::source_location origin = std::source_location::current()) {
  return runApp(
    std::forward<Factory>(factory),
    [](Scene &) {
    },
    options, origin);
}

/** @param stop Optional existing runner cancellation.
 * @param origin Stage registration call site.
 * @return Terminal stage opening the native terminal only when consumed.
 */
inline auto run(std::stop_token stop = {}, std::source_location origin = std::source_location::current()) {
  return detail::RunStage{nullptr, stop, origin};
}

/** @param terminal Borrowed existing transport, outliving stage consumption.
 * @param stop Optional cancellation.
 * @param origin Stage registration call site.
 * @return Terminal checked stage using the same Scene::run implementation.
 */
inline auto run(ApplicationTerminal &terminal, std::stop_token stop = {},
                std::source_location origin = std::source_location::current()) {
  return detail::RunStage{&terminal, stop, origin};
}

/** @param stop Optional existing runner cancellation.
 * @param origin Final stage registration call site.
 * @return Native final stage, equivalent to ui::runApp(scene), returning zero or one.
 */
inline auto runApp(std::stop_token stop = {}, std::source_location origin = std::source_location::current()) {
  return detail::RunAppStage{nullptr, stop, origin};
}

/** @param terminal Borrowed existing custom/replay transport, outliving consumption.
 * @param stop Optional cancellation.
 * @param origin Final stage registration call site.
 * @return Same final reporting stage with custom transport; construction before evaluation is outside its boundary.
 */
inline auto runApp(ApplicationTerminal &terminal, std::stop_token stop = {},
                   std::source_location origin = std::source_location::current()) {
  return detail::RunAppStage{&terminal, stop, origin};
}

/** @param callback Owning settings factory, evaluated after CLI success and before Application creation.
 * @param origin Registration call site.
 * @return Lazy application policy modifier; borrowed option storage must outlive run.
 */
inline auto applicationOptions(std::function<ApplicationOptions()> callback,
                               std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[callback = std::move(callback), origin](Scene &owner) mutable {
    owner.applicationOptions(std::move(callback), origin);
  }};
}

/** @param argc Input argument count, including program name.
 * @param argv Borrowed input strings, copied when the final stage is consumed.
 * @param parser Owning parser adapter, invoked before mount or terminal acquisition.
 * @param origin Final stage call site.
 * @return Single-use launch stage; help/version and failures never open a terminal.
 */
inline auto runApp(int argc, const char *const *argv, CommandLineParser parser,
                   std::source_location origin = std::source_location::current()) {
  return detail::CommandLineRunStage{argc, argv, std::move(parser), nullptr, {}, origin};
}

/** @param terminal Borrowed custom transport, untouched on preflight help/failure.
 * @param argc Input argument count.
 * @param argv Borrowed input array.
 * @param parser Owning generic command-line adapter.
 * @param origin Final stage call site.
 * @return The same checked launch stage with an explicit test/embedding transport.
 */
inline auto runApp(ApplicationTerminal &terminal, int argc, const char *const *argv, CommandLineParser parser,
                   std::source_location origin = std::source_location::current()) {
  return detail::CommandLineRunStage{argc, argv, std::move(parser), &terminal, {}, origin};
}
} // namespace portals::ui

#include <portals/ui_scene_resource.hpp>
