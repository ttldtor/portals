// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/application.hpp>
#include <portals/detail/setup_failure.hpp>
#include <portals/editor_command.hpp>
#include <portals/line_edit.hpp>
#include <portals/managed_diagnostics.hpp>
#include <portals/managed_model.hpp>
#include <portals/primitives.hpp>
#include <portals/style.hpp>
#include <portals/style_source.hpp>
#include <portals/virtual_preparation.hpp>

#include <ranges>

namespace portals {

class ManagedScene;

namespace ui::detail {
struct Access;
struct RefAccess;
struct SceneAccess;
struct ClickAccess;
struct NamedAccess;
struct LayoutAccess;
class SetupFailureTicket;
} // namespace ui::detail

namespace style::ct::detail {
struct Access;
} // namespace style::ct::detail

namespace detail {
template <class T> inline constexpr bool IS_MANAGED_TREE_RESULT = false;
template <class Value> inline constexpr bool IS_MANAGED_TREE_RESULT<std::expected<Value, TreeError>> = true;

/** @brief Exact checked TreeError result, including owning success values.
 * @tparam T Exact callback result type; unrelated error types are not adapted.
 */
template <class T>
concept ManagedTreeResult = IS_MANAGED_TREE_RESULT<T>;

/** @brief Accepted compact callback results; command receipts retain exact reporting provenance.
 * @tparam T Exact callback result type.
 */
template <class T>
concept ManagedActionResult = std::same_as<T, void> || ManagedTreeResult<T> || std::same_as<T, EditorCommandResult>;

/** @brief Ignore a successful payload and destroy it before leaving the entered callback scope.
 * @tparam Callback Callable returning void or an exact checked TreeError result.
 * @tparam Args Borrowed invocation argument types.
 * @param callback Entered user action; exceptions retain the caller's existing handling.
 * @param args Arguments forwarded for this synchronous invocation only.
 * @return Success or the unchanged TreeError; a false bool payload is still success.
 */
template <class Callback, class... Args>
  requires(std::same_as<std::invoke_result_t<Callback &, Args...>, void> ||
           ManagedTreeResult<std::invoke_result_t<Callback &, Args...>>)
std::expected<void, TreeError> invokeIgnoringValue(Callback &callback, Args &&...args) {
  if constexpr (std::same_as<std::invoke_result_t<Callback &, Args...>, void>) {
    std::invoke(callback, std::forward<Args>(args)...);
  } else {
    auto result = std::invoke(callback, std::forward<Args>(args)...);

    if (!result) {
      return std::unexpected(result.error());
    }
  }

  return {};
}

/** @brief A compact action optionally receives owning event parameters.
 * @tparam Callback Copyable callable owned by a managed registration.
 * @tparam Event Event type borrowed for an admitted call only.
 */
template <class Callback, class Event>
concept ManagedAction = requires(Callback &action, const Event &event) {
  { action(event) } -> ManagedActionResult;
} || requires(Callback &action) {
  { action() } -> ManagedActionResult;
};

/** @brief Recognize only the supported checked transform result.
 * @tparam T Exact normalized transform result type.
 */
template <class T> inline constexpr bool IS_DERIVED_EXPECTED = false;
template <class T> inline constexpr bool IS_DERIVED_EXPECTED<std::expected<T, TreeError>> = true;
/** @brief Keep unrelated expected error types out of the owning-value overload.
 * @tparam T Exact normalized transform result type.
 */
template <class T> inline constexpr bool IS_UNSUPPORTED_DERIVED_EXPECTED = false;
template <class T, class E>
inline constexpr bool IS_UNSUPPORTED_DERIVED_EXPECTED<std::expected<T, E>> = !std::same_as<E, TreeError>;

/** @brief Unwrap the supported checked transform result without constructing an output.
 * @tparam T Normalized owning result or expected with TreeError.
 */
template <class T> struct DerivedValue {
  /// @brief Owning output type.
  using Type = T;
};

/** @brief Successful owning payload of a checked computation.
 * @tparam T Owning output type, without requiring a default constructor.
 */
template <class T> struct DerivedValue<std::expected<T, TreeError>> {
  /// @brief Owning successful payload type.
  using Type = T;
};
/** @brief Identify common borrowed outputs that cannot own their callback input.
 * @tparam T Normalized computed value type.
 */
template <class T> inline constexpr bool IS_BORROWED_DERIVED_VALUE = false;
template <class Char, class Traits>
inline constexpr bool IS_BORROWED_DERIVED_VALUE<std::basic_string_view<Char, Traits>> = true;
template <class T, std::size_t Extent> inline constexpr bool IS_BORROWED_DERIVED_VALUE<std::span<T, Extent>> = true;
template <class T> inline constexpr bool IS_BORROWED_DERIVED_VALUE<std::reference_wrapper<T>> = true;

/** @brief Owning payload type of one pure managed computation.
 * @tparam Callback Callable invoked with const In&.
 * @tparam In Declared owning input type.
 */
template <class Callback, class In>
using DerivedOutput = typename DerivedValue<std::remove_cvref_t<std::invoke_result_t<Callback &, const In &>>>::Type;

/** @brief Reject known borrowed outputs; arbitrary user values still satisfy Property's owning contract.
 * @tparam Callback Owning synchronous computation taking the declared input by const reference.
 * @tparam In Equality-comparable source value retained for one call.
 */
template <class Callback, class In>
concept ManagedDerivation =
  std::copy_constructible<std::decay_t<Callback>> && std::invocable<Callback &, const In &> &&
  std::equality_comparable<DerivedOutput<Callback, In>> &&
  std::constructible_from<DerivedOutput<Callback, In>, DerivedOutput<Callback, In> &&> &&
  (!std::is_reference_v<std::invoke_result_t<Callback &, const In &>>) &&
  (!IS_UNSUPPORTED_DERIVED_EXPECTED<std::remove_cvref_t<std::invoke_result_t<Callback &, const In &>>>) &&
  (!std::is_pointer_v<DerivedOutput<Callback, In>>) && (!IS_BORROWED_DERIVED_VALUE<DerivedOutput<Callback, In>>) &&
  (!std::ranges::view<DerivedOutput<Callback, In>>);
} // namespace detail

/** @brief Owning, copyable custom input adapter; arguments are borrowed for one invocation.
 * @tparam T Concrete Widget subtype, including final types.
 */
template <class T>
using ManagedEventHandler = std::function<std::expected<EventReply, TreeError>(T &, const RoutedEvent &)>;

/** @brief Owning, copyable custom presentation adapter at the dirty-frame boundary.
 * @tparam T Concrete Widget subtype, borrowed only during the invocation.
 */
template <class T> using ManagedPrepareHandler = std::function<std::expected<void, TreeError>(T &, Node<Widget>)>;

/** @brief Finite managed bookkeeping budgets, independent of the existing tree/queue limits. */
struct ManagedOptions {
  std::size_t maxNodes{4096};              ///< Maximum retained entries, including removals pending next preparation.
  std::size_t maxStates{256};              ///< Maximum scene-owned properties; they live until scene destruction.
  std::size_t maxModels{64};               ///< Maximum scene-owned models; they live until close().
  ManagedDiagnosticOptions diagnostics{};  ///< Positive limits for error history and owning causal traces.
  std::size_t maxKeyHandlers{64};          ///< Positive maximum owning onKey registrations; onInput resets the count.
  std::size_t maxClickHandlers{256};       ///< Positive maximum retained pipe click callbacks across the scene.
  std::size_t maxClickHandlersPerNode{64}; ///< Positive maximum pipe click callbacks for one button.
  SchedulerTime keySequenceTimeout{1000};  ///< Positive inactivity timeout between sequence strokes, in milliseconds.
  std::size_t maxLayoutHandlers{256}; ///< Positive maximum scene-owned geometry observers; scheduler limits apply too.
  std::size_t maxLayoutPublicationRetries{
    3};                                ///< Positive queue-full attempt cap before one typed rejection and cancellation.
  std::size_t maxObservers{256};       ///< Positive scene-owned state observer limit.
  std::size_t maxObserversPerNode{64}; ///< Positive state observer limit per receiving node.
  std::size_t maxObserverRecoveryPasses{16}; ///< Positive dirty-prepare convergence bound.
};

/** @brief Weak UI handle to a scene-owned Property, never an owning alias.
 * Copies and destruction of the handle are thread independent. Operations check the
 * captured UI identity before locking the property. Immutable snapshots follow Property's
 * transfer requirements. The Application must outlive the scene, but not this weak handle.
 * @tparam T Equality-comparable, transitively owning value satisfying Property<T>'s contract.
 */
template <std::equality_comparable T> class ManagedState {
public:
  /** @brief Construct an empty handle; operations return CLOSED. */
  ManagedState() = default;

  /** @param value Candidate copied by Property after validation.
   * @return Original commit/delivery receipt, WRONG_THREAD, CLOSED, or Property's error.
   */
  std::expected<PropertyUpdate<T>, TreeError> set(const T &value) const {
    ui::detail::SetupFailurePause setup(facade_);
    ui::detail::CurrentSceneScope mask;
    auto model = lock();
    ui::detail::CurrentSceneScope current(facade_);

    return model ? (*model)->set(value) : std::unexpected(model.error());
  }

  /** @param value Candidate moved by Property after validation.
   * @return Original commit/delivery receipt, WRONG_THREAD, CLOSED, or Property's error.
   */
  std::expected<PropertyUpdate<T>, TreeError> set(T &&value) const {
    ui::detail::SetupFailurePause setup(facade_);
    ui::detail::CurrentSceneScope mask;
    auto model = lock();
    ui::detail::CurrentSceneScope current(facade_);

    return model ? (*model)->set(std::move(value)) : std::unexpected(model.error());
  }

  /** @return Owning immutable snapshot, WRONG_THREAD, or CLOSED after owner destruction.
   * A stopped scene retains its properties until destruction, allowing a final snapshot.
   */
  std::expected<std::shared_ptr<const PropertyChange<T>>, TreeError> snapshot() const {
    auto model = lock();

    return model ? (*model)->snapshot() : std::unexpected(model.error());
  }

private:
  friend class ManagedScene;

  std::expected<std::shared_ptr<Property<T>>, TreeError> lock() const {
    if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }

    if (lifetime_.expired()) {
      return std::unexpected(TreeError::CLOSED);
    }

    auto model = model_.lock();

    if (!model) {
      return std::unexpected(TreeError::CLOSED);
    }

    return model;
  }

  std::weak_ptr<Property<T>> model_;
  std::weak_ptr<ui::detail::SceneAnchor> facade_;
  std::weak_ptr<const void> lifetime_;
  std::thread::id owner_{};
  NodeId scene_{};
};

/** @brief Read-only weak scene-owned derived value; no writable alias or subscription escapes.
 * @tparam T Equality-comparable, transitively owning immutable Property value.
 * Snapshots remain readable after close until scene destruction, matching ManagedState.
 */
template <std::equality_comparable T> class ManagedSource {
public:
  /** @brief Construct an empty source whose operations return CLOSED. */
  ManagedSource() = default;

  /** @return Independent immutable value/revision owner, WRONG_THREAD, or CLOSED after owner destruction. */
  std::expected<std::shared_ptr<const PropertyChange<T>>, TreeError> snapshot() const {
    return state_.snapshot();
  }

private:
  friend class ManagedScene;

  explicit ManagedSource(ManagedState<T> state) : state_(std::move(state)) {
  }

  ManagedState<T> state_;
};

/** @brief Additive lifecycle owner over one existing Application subtree.
 * UI affine and nonmovable. Application/tree/router/scheduler must outlive this owner.
 * Destroy outside callbacks and tree frames; self/ancestor node removal in callbacks is
 * supported. Standard controls retain their existing binding and LineEdit semantics.
 * Use run() to preserve detailed managed failures alongside runner and restoration errors.
 * No raw owning Property or individual binding token escapes this owner.
 */
class ManagedScene final : public ApplicationScene {
public:
  /** @param app Borrowed existing runtime, used exclusively by this scene during run.
   * @param options Positive limits on managed entries and state ownership.
   * @return Owner of a newly mounted plain subtree, or typed allocation/mount error.
   */
  static std::expected<std::unique_ptr<ManagedScene>, ManagedError> create(Application &app,
                                                                           ManagedOptions options = {});

  /** @brief Retire bindings and the owned subtree on UI; wrong-thread/frame destruction terminates. */
  ~ManagedScene();
  ManagedScene(const ManagedScene &) = delete;
  ManagedScene &operator=(const ManagedScene &) = delete;

  /** @return Observing subtree root; put a Row/Column/custom layout below it. */
  Node<Widget> root() const noexcept {
    return root_;
  }

  /** @return Immutable text width policy shared with the borrowed Application. */
  WidthPolicy widthPolicy() const noexcept {
    return app_.widthPolicy();
  }

  /// @brief Retire one managed subtree and immediately reclaim its managed entry budget.
  /// @param node Managed node other than root(); an already retired managed identity is accepted.
  /// @return Success or typed boundary/removal error. State owners remain scene-owned.
  /// Managed mutations reentering this remove call return WOULD_BLOCK.
  /// An outer tree borrow can defer unmount beyond this call; those hooks follow the existing
  /// tree contract and may remove other nodes. External callback effects are not rolled back.
  /// Existing style metadata is pruned on the next style transaction, before budget admission.
  std::expected<void, ManagedError> remove(Node<Widget> node);

  /** @brief Checked mount with automatic standard presentation and style registration.
   * Failure retires only the attempted child; earlier children remain owned. Destroying
   * the scene on a failed form construction cleans the entire partial form.
   * @tparam T Public Widget subtype constructible from Args; final classes are supported.
   * @tparam Args Perfect-forwarded constructor argument types.
   * @param parent Root or node created by this scene; low-level external children are not adopted.
   * @param args Constructor arguments, borrowed only during construction.
   * @return Typed observer or mount/setup error; no child escapes a failed operation.
   */
  template <class T, class... Args>
    requires std::derived_from<T, Widget>
  std::expected<Node<T>, ManagedError> emplace(Node<Widget> parent, Args &&...args) {
    auto allowed = boundary(ManagedPhase::MOUNT, parent);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (parent != root_ && !find(parent)) {
      return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::INVALID_ARGUMENT));
    }

    if (entries_.size() >= options_.maxNodes) {
      return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::NODE_LIMIT));
    }

    auto node = app_.tree().emplaceChecked<T>(parent, std::forward<Args>(args)...);

    if (!node) {
      return std::unexpected(error(ManagedPhase::MOUNT, parent, node.error()));
    }

    try {
      auto entry = std::make_shared<Entry>();

      entry->node = *node;

      if constexpr (std::derived_from<T, Control> || std::same_as<T, LineEdit> || std::same_as<T, Border> ||
                    std::same_as<T, Gauge>) {
        const char *type = "Control";

        if constexpr (std::same_as<T, Border>) {
          type = "Border";
        } else if constexpr (std::same_as<T, Gauge>) {
          type = "Gauge";
        }

        entry->target = std::make_shared<StyleTarget>(StyleTarget{.type = type});
        entry->stylePending = true;
        stylesDirty_ = true;
      }

      if constexpr (std::derived_from<T, Control>) {
        entry->sync = [this, node = *node](Node<Widget> focused) {
          return syncControl(app_.tree(), node, focused);
        };
      } else if constexpr (std::same_as<T, Border>) {
        entry->sync = [this, node = *node](Node<Widget>) {
          return syncBorder(app_.tree(), node);
        };
      } else if constexpr (std::same_as<T, Gauge>) {
        entry->sync = [this, node = *node](Node<Widget>) {
          return syncGauge(app_.tree(), node);
        };
      }

      entries_.push_back(std::move(entry));

      return *node;
    } catch (const std::bad_alloc &) {
      return rollback(*node, TreeError::NO_MEMORY);
    } catch (...) {
      return rollback(*node, TreeError::CALLBACK_FAILED);
    }
  }

  /** @brief Create UI-owned state with guaranteed dirty-frame recovery of missed notifications.
   * @tparam T Equality-comparable, owning value satisfying Property<T>'s contract.
   * @param initial Initial value moved into revision zero, except string copied inside the checked boundary;
   * argument construction precedes this call.
   * @param kind PAINT or LAYOUT; NONE is rejected because it cannot guarantee recovery.
   * @param options Positive existing Property observer capacity.
   * @return Weak typed state handle, or validation/allocation/construction error.
   */
  template <std::equality_comparable T>
  std::expected<ManagedState<T>, ManagedError> state(T initial, Invalidation kind = Invalidation::PAINT,
                                                     PropertyOptions options = {}) {
    auto allowed = boundary(ManagedPhase::STATE, root_);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (kind != Invalidation::PAINT && kind != Invalidation::LAYOUT) {
      return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::INVALID_ARGUMENT));
    }

    if (properties_.size() >= options_.maxStates) {
      return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NODE_LIMIT));
    }

    auto created = [&] {
      if constexpr (std::same_as<T, std::string>) {
        // MSVC Debug string moves may allocate a proxy in noexcept; copy inside Property's checked boundary.
        return Property<T>::create(app_.tree(), root_, std::as_const(initial), kind, options);
      } else {
        return Property<T>::create(app_.tree(), root_, std::move(initial), kind, options);
      }
    }();

    if (!created) {
      return std::unexpected(error(ManagedPhase::STATE, root_, created.error()));
    }

    try {
      std::shared_ptr<Property<T>> model(std::move(*created));
      ManagedState<T> handle;

      handle.model_ = model;
      handle.facade_ = facade_;
      handle.lifetime_ = lifetime_;
      handle.owner_ = owner_;
      handle.scene_ = root_.id();
      properties_.push_back(std::move(model));

      return handle;
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NO_MEMORY));
    }
  }

  /** @brief Own a one-way presentation observer until its receiver or scene retires.
   * Initial delivery reads the latest value at dirty preparation after complete declarative mount.
   * Later queued revisions may coalesce. Callbacks borrow an immutable value only for their invocation;
   * captures must not destroy this scene. Equal updates do not deliver again. Registration never invokes user code.
   * @tparam W Managed receiving widget type.
   * @tparam T Owning equality-comparable state value.
   * @tparam Callback Copyable callable taking const T&, returning void or expected<void, TreeError>.
   * @param receiver Live managed identity, or this scene's root.
   * @param model State created by this scene.
   * @param callback Owned presentation action; node removal during invocation is allowed.
   * @return Success or BIND validation, capacity, allocation or callback-construction error.
   */
  template <class W, std::equality_comparable T, class Callback>
    requires std::copy_constructible<std::decay_t<Callback>> &&
             (std::same_as<std::invoke_result_t<Callback &, const T &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const T &>, std::expected<void, TreeError>>)
  std::expected<void, ManagedError> observe(Node<W> receiver, ManagedState<T> model, Callback &&callback) {
    ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
    auto admitted = observerAdmission(receiver);
    if (!admitted) {
      return admitted;
    }

    auto property = model.lock();
    if (!property || model.scene_ != root_.id()) {
      return std::unexpected(
        error(ManagedPhase::BIND, receiver, property ? TreeError::INVALID_ARGUMENT : property.error()));
    }

    try {
      if constexpr (std::is_pointer_v<std::decay_t<Callback>> || requires { callback.operator bool(); }) {
        if (!static_cast<bool>(callback)) {
          return std::unexpected(error(ManagedPhase::BIND, receiver, TreeError::INVALID_ARGUMENT));
        }
      }

      auto record = std::make_shared<ObserverRecord>();
      record->scene = this;
      record->node = receiver;
      prepareObserverRecord(record, *property, std::forward<Callback>(callback));

      // User capture construction/destruction may have changed either identity or the budget.
      admitted = observerAdmission(receiver);
      property = model.lock();
      if (!admitted) {
        return admitted;
      }
      if (!property || model.scene_ != root_.id()) {
        return std::unexpected(
          error(ManagedPhase::BIND, receiver, property ? TreeError::INVALID_ARGUMENT : property.error()));
      }
      observers_.reserve(observers_.size() + 1);
      auto token = connectObserverRecord(receiver, *property, record);
      if (!token) {
        return std::unexpected(error(ManagedPhase::BIND, receiver, token.error()));
      }

      auto dirty = app_.tree().invalidate(root_, Invalidation::PAINT);
      if (!dirty) {
        return std::unexpected(error(ManagedPhase::BIND, receiver, dirty.error()));
      }
      record->subscription = std::move(*token);
      observers_.push_back(std::move(record));
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, receiver, TreeError::NO_MEMORY));
    } catch (const std::length_error &) {
      return std::unexpected(error(ManagedPhase::BIND, receiver, TreeError::NODE_LIMIT));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, receiver, TreeError::CALLBACK_FAILED));
    }
  }

  /** @tparam T Owning equality-comparable state value.
   * @tparam Callback Copyable callback with the receiving observe overload's result contract.
   * @param model State created by this scene.
   * @param callback Presentation action retained until scene retirement.
   * @return The receiving overload's typed registration result, scoped to root().
   */
  template <std::equality_comparable T, class Callback>
    requires std::copy_constructible<std::decay_t<Callback>> &&
             (std::same_as<std::invoke_result_t<Callback &, const T &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const T &>, std::expected<void, TreeError>>)
  std::expected<void, ManagedError> observe(ManagedState<T> model, Callback &&callback) {
    return observe(root_, model, std::forward<Callback>(callback));
  }

  /** @tparam W Receiving managed widget type.
   * @tparam T Owning derived value.
   * @tparam Callback Copyable presentation callback with the writable source overload's result contract.
   * @param receiver Live node that scopes the observation.
   * @param source Read-only source owned by this scene.
   * @param callback Owned callback, first invoked after complete mount.
   * @return Existing typed observation admission result; no writable handle escapes.
   */
  template <class W, std::equality_comparable T, class Callback>
    requires std::copy_constructible<std::decay_t<Callback>> &&
             (std::same_as<std::invoke_result_t<Callback &, const T &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const T &>, std::expected<void, TreeError>>)
  std::expected<void, ManagedError> observe(Node<W> receiver, ManagedSource<T> source, Callback &&callback) {
    return observe(receiver, source.state_, std::forward<Callback>(callback));
  }

  /** @tparam T Owning derived value.
   * @tparam Callback Copyable presentation callback with the receiving overload's result contract.
   * @param source Read-only source owned by this scene.
   * @param callback Action scoped to the scene root.
   * @return The receiving overload's checked observation admission result.
   */
  template <std::equality_comparable T, class Callback>
    requires std::copy_constructible<std::decay_t<Callback>> &&
             (std::same_as<std::invoke_result_t<Callback &, const T &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const T &>, std::expected<void, TreeError>>)
  std::expected<void, ManagedError> observe(ManagedSource<T> source, Callback &&callback) {
    return observe(root_, source.state_, std::forward<Callback>(callback));
  }

  /** @brief Create an owning read-only computation using the existing observer runtime.
   * @tparam In Owning equality-comparable input value.
   * @tparam Transform Copyable pure function returning an owning Out or expected<Out, TreeError>.
   * @param input Existing source owned by this scene; its immutable value is borrowed during computation.
   * @param transform Owned synchronous computation; must not mutate UI or depend on Ref publication.
   * @param kind PAINT or LAYOUT invalidation for changed output; NONE is rejected.
   * @param options Positive existing output Property observer capacity.
   * @param origin Derivation registration retained in failure diagnostics.
   * @return Immediately readable source, or checked initial error with no retained output/attachment.
   * Runtime failures preserve the last good output and follow the existing managed stop policy.
   */
  template <std::equality_comparable In, class Transform>
    requires detail::ManagedDerivation<Transform, In>
  std::expected<ManagedSource<detail::DerivedOutput<Transform, In>>, ManagedError>
  derive(ManagedState<In> input, Transform &&transform, Invalidation kind = Invalidation::PAINT,
         PropertyOptions options = {}, std::source_location origin = std::source_location::current());

  /** @tparam In Owning equality-comparable input value.
   * @tparam Transform Copyable pure owning computation with the writable input overload's contract.
   * @param input Previously created read-only source; fresh outputs make declared dependency cycles unexpressible.
   * @param transform Owned synchronous computation.
   * @param kind Output invalidation, PAINT or LAYOUT.
   * @param options Positive output Property observer capacity.
   * @param origin Derivation registration retained in diagnostics.
   * @return Immediately readable source or unchanged typed creation failure.
   */
  template <std::equality_comparable In, class Transform>
    requires detail::ManagedDerivation<Transform, In>
  std::expected<ManagedSource<detail::DerivedOutput<Transform, In>>, ManagedError>
  derive(ManagedSource<In> input, Transform &&transform, Invalidation kind = Invalidation::PAINT,
         PropertyOptions options = {}, std::source_location origin = std::source_location::current()) {
    return derive(input.state_, std::forward<Transform>(transform), kind, options, origin);
  }

  /** @param node Managed button without a prior event binding.
   * @param action Typed business callback, run after standard activation admission; may remove its node.
   * @return Success with scene-owned token or typed registration error.
   */
  std::expected<void, ManagedError> onClick(Node<Button> node, std::function<std::expected<void, TreeError>()> action);

  /** @tparam Callback Owning void, expected<Value, TreeError> or EditorCommandResult action, optionally taking
   * ClickEvent.
   * @param node Live managed button without another action binding.
   * @param action Owning checked editor command callback; return Ref.undo/redo directly to preserve provenance.
   * @return Scoped registration success or typed binding failure. Parameterized callbacks admit modifiers;
   * no-argument callbacks retain legacy unmodified admission. An already reported exact returned failure
   * is not recorded again in the same live collector; independent failures and throws are still reported.
   * Successful expected payloads are ignored within the entered callback scope.
   */
  template <class Callback>
    requires detail::ManagedAction<Callback, ClickEvent>
  std::expected<void, ManagedError> onClick(Node<Button> node, Callback action) {
    try {
      return onButtonClick(node, actionAdapter<ClickEvent>(std::move(action)),
                           std::invocable<Callback &, const ClickEvent &>);
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
    }
  }

  /** @tparam Callback Owning compact action, optionally borrowing the admitted KeyEvent.
   * @param chord Exact key/scalar/modifiers; PRESS and REPEAT are admitted, RELEASE is ignored.
   * @param action Void, expected<Value, TreeError> or EditorCommandResult callback; success payloads are ignored.
   * @return Registration result with typed allocation/affinity errors; empty callbacks are rejected.
   * Composes owning registrations in the single existing onInput slot. Newest matching chord wins;
   * nonmatches delegate to the previous interceptor, then standard routing. Matching success consumes
   * input; errors follow existing input failure handling. Direct onInput replaces the whole chain.
   * Command errors already reported to this scene are not reported twice.
   */
  template <class Callback>
    requires detail::ManagedAction<Callback, KeyEvent>
  std::expected<void, ManagedError> onKey(KeyEvent chord, Callback action) {
    auto allowed = boundary(ManagedPhase::BIND, root_);

    if (!allowed) {
      return allowed;
    }

    if (keyHandlers_ >= options_.maxKeyHandlers) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NODE_LIMIT));
    }

    try {
      auto adapter = actionAdapter<KeyEvent>(std::move(action));

      if (!adapter) {
        return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::INVALID_ARGUMENT));
      }

      return registerLegacyKey(chord, std::move(adapter));
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::CALLBACK_FAILED));
    }
  }

  /** @tparam Value Owning Shortcut value, borrowed until copied/moved inside the checked boundary.
   * @tparam Callback Owning compact action, optionally receiving the admitted semantic KeyEvent.
   * @param shortcut Normalized alternatives, validated before owning transfer and publication.
   * @param action Checked callback invoked once, preserving the existing command-error receipt.
   * @param behavior PRESS-only default; REPEAT is explicit.
   * @param options Positive finite alternative budgets.
   * @param origin Registration site retained for errors from this admitted action, including throws.
   * @return Atomic registration or typed failure; newest matching registration wins in the existing input slot.
   * Modified text produces a CHARACTER PRESS value with the original scalar and semantic modifiers.
   * Callback storage stays pinned through dispatch, replacement and scene cleanup.
   * Each sequence stroke uses this admission policy. A prefix consumes without invoking;
   * completion resets pending state before calling the action with the final stroke.
   * Timeout and mismatch never invoke a suppressed terminal command or replay old events.
   */
  template <class Value, class Callback>
    requires std::same_as<std::remove_cvref_t<Value>, Shortcut> && detail::ManagedAction<Callback, KeyEvent>
  std::expected<void, ManagedError> onKey(Value &&shortcut, Callback action, ShortcutBehavior behavior = {},
                                          ShortcutOptions options = {},
                                          std::source_location origin = std::source_location::current()) {
    return onShortcutActions(std::forward<Value>(shortcut), action,
                             behavior.repeat ? detail::ShortcutActions::PRESS_REPEAT : detail::ShortcutActions::PRESS,
                             options, origin);
  }

  /** @tparam Callback Owning compact action, optionally receiving const KeyEvent&.
   * @param selection Immutable
   * owning alternatives or a deferred helper error.
   * @param action Checked callback invoked once for an admitted
   * native phase.
   * @param origin Registration site preserved for configuration and action failures.
   * @return
   * Atomic registration or typed failure, preserving previous handlers and pending input on failure.
   * The explicit phase set applies to every sequence stroke; no backend phases are synthesized.
   */
  template <class Callback>
    requires detail::ManagedAction<Callback, KeyEvent>
  std::expected<void, ManagedError> onKey(const KeySelection &selection, Callback action,
                                          std::source_location origin = std::source_location::current()) {
    auto allowed = boundary(ManagedPhase::BIND, root_);

    if (!allowed) {
      return allowed;
    }

    const auto &shortcut = detail::KeySelectionAccess::shortcut(selection);

    if (!shortcut || !*shortcut) {
      const auto cause = shortcut ? ShortcutError{ShortcutErrorKind::INVALID_CHORD, 0} : shortcut.error();
      auto failure =
        error(ManagedPhase::BIND, root_,
              cause.cause == ShortcutErrorKind::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT);

      failure.shortcut = cause;
      failure.registrationOrigin = origin;

      return std::unexpected(std::move(failure));
    }

    return onShortcutActions(**shortcut, action, detail::KeySelectionAccess::actions(selection),
                             detail::KeySelectionAccess::options(selection), origin);
  }

  /** @param node Managed checkbox without a prior event binding.
   * @param model State created by this scene; shared internally on UI only.
   * @param changed Optional callback after the standard toggle, receiving its exact commit receipt.
   * @return Success or typed registration error. Returning an error cannot undo a committed toggle.
   */
  std::expected<void, ManagedError> bind(Node<CheckBox> node, ManagedState<bool> model,
                                         std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {});

  /** @param node Managed checkbox without a prior event binding.
   * @param model State created by this scene; shared internally on UI only.
   * @param changed Optional callback after the standard toggle, receiving its exact commit receipt.
   * @param options Typed activation cycle; invalid values return INVALID_ARGUMENT.
   * @return Success or typed registration error. Returning an error cannot undo a committed toggle.
   */
  std::expected<void, ManagedError> bind(Node<CheckBox> node, ManagedState<CheckState> model,
                                         std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                         CheckBoxOptions options = {});

  /** @param node Managed editor without a prior event binding.
   * @param model State created by this scene.
   * @param result Optional callback after standard editing/commit/conflict handling; cannot undo a commit.
   * @param clipboard Existing clipboard adapter retained by the binding, or null.
   * @param validate Existing commit validator; false rejects Enter without changing the model.
   * @return Success or typed registration error; draft, revision conflict, Escape and blur rules are unchanged.
   */
  std::expected<void, ManagedError> bind(Node<LineEdit> node, ManagedState<std::string> model,
                                         std::function<std::expected<void, TreeError>(LineEditResult)> result = {},
                                         std::shared_ptr<Clipboard> clipboard = {},
                                         std::function<bool(std::string_view)> validate = {});

  /** @brief Observe accepted text as a one-way caption, independently of the control's action.
   * @param node Live managed control without an existing caption binding.
   * @param model Text state created by this scene; ownership stays with the scene.
   * @return Success after initial synchronization, or typed validation/allocation error without attachment.
   * Duplicate attachment returns INVALID_ARGUMENT. Changed revisions synchronize before dirty frames;
   * failed synchronization preserves the caption and retries at the next dirty or explicit preparation.
   * A manual caption change remains visible until the next changed model revision. No reverse write occurs.
   */
  std::expected<void, ManagedError> bindText(Node<Control> node, ManagedState<std::string> model);

  /** @param node Live managed caption receiver.
   * @param source Read-only owning string source from this scene.
   * @return Existing caption binding result; updates are synchronized at dirty preparation.
   */
  std::expected<void, ManagedError> bindText(Node<Control> node, ManagedSource<std::string> source) {
    return bindText(node, source.state_);
  }

  /** @brief Declare a nested focus scope; this scene owns its disconnect token.
   * @param node Live managed scope root, possibly also an ordinary bound control.
   * @return Success or the existing typed router/binding error; no token escapes.
   */
  std::expected<void, ManagedError> focusScope(Node<Widget> node);
  /** @brief Set a live managed node as its nearest scope's default.
   * @param node Default descendant; hidden/disabled defaults are skipped on entry.
   * @return Success, INVALID_ARGUMENT without an enclosing scope, or typed validation error.
   */
  std::expected<void, ManagedError> focusDefault(Node<Widget> node);
  /** @brief Enter a scope or focus an ordinary registered control through the same blur path.
   * @param node Live managed target; scope selection uses last/default/preorder fallback.
   * @return Success or typed validation/callback error. Published focus is not rolled back.
   */
  std::expected<void, ManagedError> focus(Node<Widget> node);
  /** @brief Enter a declared scope while saving exact previous focus in the existing router.
   * This focus-only operation does not restrict input or global Tab traversal.
   * @param node Live eligible managed scope root, absent from the return stack.
   * @return Success or typed validation/allocation/callback error. Published entry errors retain
   * the return record; leaveFocusScope can still restore focus after such an error.
   */
  std::expected<void, ManagedError> enterFocusScope(Node<Widget> node);
  /** @brief Leave the top focus-return scope; callback errors retain the published return.
   * @param node Live managed root matching the top stack record.
   * @return Success, INVALID_ARGUMENT for another root, or typed boundary/router error.
   * Removed roots recover lazily through the router rather than through a stale managed reference.
   */
  std::expected<void, ManagedError> leaveFocusScope(Node<Widget> node);

  /** @brief Own one custom event callback, without subclassing or an additional router.
   * @tparam T Custom Widget type; callable signature is checked through std::function.
   * @param node Managed node without another binding; standard bound controls reject replacement.
   * @param callback Typed handler borrowing T and event for this invocation only. Errors stop run.
   * @return Success or typed registration error. An admitted handler survives self-removal.
   */
  template <class T>
  std::expected<void, ManagedError> onEvent(Node<T> node, std::type_identity_t<ManagedEventHandler<T>> callback) {
    ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
    ManagedEventHandler<T> incoming;

    incoming.swap(callback);
    auto entry = bindingEntry(node);

    if (!entry) {
      return std::unexpected(entry.error());
    }

    if (!incoming) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }

    try {
      auto bound = app_.router().bind(
        node, [this, node, callback = ui::detail::currentSceneCallback(facadeContext(), std::move(incoming))](
                T &widget, const RoutedEvent &event) {
          try {
            auto result = callback(widget, event);

            if (result) {
              return *result;
            }

            remember(error(ManagedPhase::INPUT, node, result.error()));
          } catch (...) {
            remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED));
          }

          return EventReply::HANDLED;
        });

      if (!bound) {
        return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
      }

      (*entry)->binding.template emplace<ActionBinding>(std::move(*bound));

      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
    }
  }

  /** @brief Register custom presentation work at the existing dirty-frame boundary.
   * @tparam T Concrete custom Widget type, including final types.
   * @param node Managed target, borrowed through tree.edit while the callback runs.
   * @param callback Called with widget and recovered focus. Read latest state, change only unequal
   * presentation values, and invalidate PAINT/LAYOUT if needed. Do not retain widget references.
   * @return Success or typed registration error; no idle polling is introduced.
   */
  template <class T>
  std::expected<void, ManagedError> onPrepare(Node<T> node, std::type_identity_t<ManagedPrepareHandler<T>> callback) {
    ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
    ManagedPrepareHandler<T> incoming;

    incoming.swap(callback);
    auto allowed = boundary(ManagedPhase::BIND, node);

    if (!allowed) {
      return allowed;
    }

    auto entry = find(node);

    if (!entry || !incoming) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }

    try {
      entry->customSync =
        [this, node,
         callback = ui::detail::currentSceneCallback(facadeContext(), std::move(incoming))](Node<Widget> focused) {
          std::expected<void, TreeError> result;
          auto edited = app_.tree().edit(node, [&](T &widget) {
            result = callback(widget, focused);
          });

          return edited ? result : edited;
        };

      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
    }
  }

  /** @param node Managed node whose style metadata will be staged.
   * @param target Owning custom identity; standard controls already have type Control.
   * @return Success staging the metadata for the next dirty frame, or validation error.
   */
  std::expected<void, ManagedError> style(Node<Widget> node, StyleTarget target);

  /** @param node Live scene-owned node whose style identity is changed.
   * @param value Case-sensitive id copied during this call; existing type/classes are preserved.
   * @return Atomic metadata update or typed style/liveness/allocation error.
   */
  std::expected<void, ManagedError> id(Node<Widget> node, std::string_view value);

  /** @param sheet Owning stylesheet installed with the existing atomic style engine.
   * @return Success or original StyleError. Layered mode updates reserved THEME with pending metadata
   * atomically and revokes its prior controller; legacy mode flushes registrations before whole replacement.
   */
  std::expected<void, ManagedError> theme(StyleSheet sheet);

  /** @brief Opt into owning style tiers; failure preserves pending registrations and legacy controllers.
   * @param options Aggregate owning layer limits, fixed after successful opt-in.
   * @return Success or the original STYLE boundary, budget or staging error.
   */
  std::expected<void, ManagedError> enableStyleLayers(StyleLayerOptions options = {});

  /**
   * @brief Atomically install owning declarations and provenance in an APPLICATION or USER layer.
   *
   * Legacy mode becomes layered in the same commit, retaining the old sheet in THEME.
   * Failure preserves mode, authority, pending registrations, computed values, and provenance.
   * Equal declarations with changed metadata alone do not call defaults, advance revision, or request redraw.
   *
   * @param tier APPLICATION or USER priority; base tiers are rejected.
   * @param name Nonempty tier identity; replacement preserves its handle and installation order.
   * @param document Borrowed mutable document, validated and deep-copied into immutable ownership before publication.
   * @param options Omitted limits adopt established budgets, or defaults at first enable; incompatible values reject.
   * @return Weak identity or typed STYLE/PSS boundary, validation, budget, identity, or allocation failure.
   *
   * Metadata has a separate MAX_PSS_DOCUMENT_BYTES logical budget per document.
   * Checked maxLayers times that limit bounds resident logical metadata; records also incur the layer payload charge.
   * Document copies and candidate sheets may coexist while staging. These limits do not cap RSS.
   * Coordinates are checked structurally; original bytes are unavailable for proving their exact positions.
   * Non-allocation errors retain a best-effort owning source label. NO_MEMORY never allocates a fallback label.
   * A facade may attach its already-owned immutable identity without allocating or changing the original cause.
   */
  std::expected<StyleLayerHandle, ManagedError> styleDocument(StyleTier tier, std::string_view name,
                                                              const PssDocument &document,
                                                              std::optional<StyleLayerOptions> options = std::nullopt);

  /**
   * @brief Copy every actual layer row in installation order on the UI thread.
   *
   * @return Owning names and shared immutable documents, or typed boundary/allocation failure.
   *
   * Unknown sources have null metadata. Legacy mode returns an empty snapshot.
   * Retained documents may outlive cleanup; observing handles never keep the scene alive.
   * Allocator reentry cannot mutate or retire the scene while the snapshot is copied.
   */
  std::expected<std::vector<StyleLayerSource>, ManagedError> styleSources() const;

  /**
   * @brief Validate an observing layer identity without resolving styles or invoking callbacks.
   *
   * @param handle UI-affine weak identity issued by this scene.
   * @return Success or typed STYLE boundary, wrong-thread, foreign, removed, or stale identity failure.
   */
  std::expected<void, ManagedError> validateStyleLayer(StyleLayerHandle handle) const;

  /** @param tier APPLICATION or USER priority.
   * @param name Nonempty tier-unique name, copied after budget admission.
   * @param sheet Borrowed declarations, normalized into engine ownership on success.
   * @return Weak observer or atomic STYLE failure.
   */
  std::expected<StyleLayerHandle, ManagedError> addLayer(StyleTier tier, std::string_view name,
                                                         const StyleSheet &sheet);
  /** @param handle Live non-base layer observer.
   * @param sheet Borrowed replacement preserving identity and installation order.
   * @return Effective invalidation or atomic STYLE failure.
   */
  std::expected<Invalidation, ManagedError> replaceLayer(StyleLayerHandle handle, const StyleSheet &sheet);
  /** @param handle Live layer observer; base removal clears declarations but retains the slot.
   * @return Effective invalidation or atomic STYLE failure.
   */
  std::expected<Invalidation, ManagedError> removeLayer(StyleLayerHandle handle);
  /** @param updates Borrowed unique existing-handle updates, committed together with pending metadata.
   * @return Effective invalidation or atomic STYLE failure; empty input without pending metadata is a no-op.
   */
  std::expected<Invalidation, ManagedError> updateLayers(std::span<const StyleLayerUpdate> updates);
  /** @param tier DEFAULTS or THEME slot.
   * @return Weak observer or STYLE boundary/argument error.
   */
  std::expected<StyleLayerHandle, ManagedError> baseLayer(StyleTier tier) const;
  /** @param sheet Borrowed DEFAULTS declarations below THEME and application/user overrides.
   * @return Effective invalidation or atomic STYLE failure.
   */
  std::expected<Invalidation, ManagedError> styleDefaults(const StyleSheet &sheet);

  /** @param callback Scene-level interceptor before routing; true suppresses routing, false continues.
   * Mutable captures persist between events. Empty clears the interceptor; replacement retains an active call.
   * @return Success or boundary/allocation error. Callback errors retain their complete ManagedError.
   */
  std::expected<void, ManagedError>
  onInput(std::function<std::expected<bool, ManagedError>(const InputEvent &)> callback);

  /** @brief Record an explicit error without changing runtime stop/recovery policy.
   * @param failure Original typed error; argument construction precedes this call.
   * @param contexts Borrowed breadcrumbs copied under the configured count and byte budgets.
   * @param origin Report call site; supply the earlier origin for deferred operations.
   * @return Success, WRONG_THREAD before any state access, or CLOSED. Overflow/context OOM
   * retain the original cause through bounded history/counters and the observer fallback.
   * Nested reports are collected but do not recursively invoke onError. No frame is requested.
   * Checked setup operations remain explicit expected results and are not automatically recorded.
   */
  std::expected<void, TreeError> reportError(ManagedError failure,
                                             std::span<const ManagedDiagnosticContext> contexts = {},
                                             std::source_location origin = std::source_location::current()) noexcept;

  /** @brief Retain or clear the UI error observer; empty clears it.
   * @param callback Borrowing observer called after recording, including history overflow.
   * It must not retain the diagnostic reference. Exceptions only increment diagnostic counters.
   * @return Success or typed registration error; WOULD_BLOCK while an observer is active.
   * Notifications precede terminal restoration; no output or continuation policy is imposed.
   */
  std::expected<void, ManagedError> onError(ManagedErrorHandler callback);

  /** @brief Copy the bounded error history on UI, including after close.
   * @return Owning snapshot, WRONG_THREAD, or NO_MEMORY with the history unchanged.
   * Inspection is allowed during onError; its current observerFailed flag is not final yet.
   */
  std::expected<ManagedDiagnosticBatch, TreeError> diagnostics() const;

  /** @brief Clear history and counters while preserving capacity and runtime first failure.
   * @return Success, WRONG_THREAD, or WOULD_BLOCK during onError; allowed after close.
   */
  std::expected<void, TreeError> clearErrors() noexcept;

  /** @brief Copy then clear history and counters; allocation failure leaves them unchanged.
   * @return Owning batch, WRONG_THREAD, WOULD_BLOCK during onError, or NO_MEMORY.
   * Allowed after close. Does not reset a runtime first failure or change stop policy.
   */
  std::expected<ManagedDiagnosticBatch, TreeError> drainErrors();

  /** @brief Create a scene-owned model and a weak typed UI handle.
   * @tparam T Immutable domain row satisfying Model<T>'s provider contract.
   * @param initial Non-null immutable initial snapshot, retained by the model.
   * @param options Positive existing model range, consumer and work limits.
   * @return Handle or original allocation/validation error in STATE phase.
   */
  template <class T>
  std::expected<ManagedModel<T>, ManagedError> model(std::shared_ptr<const ModelSnapshot<T>> initial,
                                                     ModelOptions options = {});

  /** @brief Mount a table with owned range, input binding, style and automatic preparation.
   * @tparam T Immutable model row accepted by the owning column delegates.
   * @param parent Managed parent; failure removes only the attempted child.
   * @param model Model belonging to this scene; no owner or pin escapes.
   * @param columns Column descriptions copied by the table.
   * @param options Existing table configuration, validated by preparation.
   * @param policy Existing layout policy for the table.
   * @param prepared Optional callback after each preparation, including retries; empty rejects pending work with
   * INVALID_ARGUMENT.
   * @return Observing node or typed error; standard input semantics are preserved.
   */
  template <class T>
  std::expected<Node<VirtualTable<T>>, ManagedError>
  virtualTable(Node<Widget> parent, ManagedModel<T> model,
               std::type_identity_t<std::span<const TableColumn<T>>> columns, VirtualTableOptions options = {},
               SizePolicy policy = {}, std::type_identity_t<ManagedViewHandler<T>> prepared = {});

  /** @brief Mount a list with owned range, input binding, style and automatic preparation.
   * @tparam T Immutable row accepted by the owning text delegate.
   * @param parent Managed parent; failure removes only the attempted child.
   * @param model Model belonging to this scene.
   * @param delegate Owning row formatter, invoked only for visible rows.
   * @param options Existing list configuration, validated by preparation.
   * @param policy Existing layout policy for the list.
   * @param prepared Optional callback after each preparation; empty rejects pending work with INVALID_ARGUMENT.
   * @return Observing node or typed mount/bind error.
   */
  template <class T>
  std::expected<Node<VirtualList<T>>, ManagedError>
  virtualList(Node<Widget> parent, ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
              VirtualListOptions options = {}, SizePolicy policy = {},
              std::type_identity_t<ManagedViewHandler<T>> prepared = {});

  /** @brief Mount a chart with owned range, style and automatic preparation.
   * @tparam T Immutable row projected into an optional OHLC value.
   * @param parent Managed parent; failure removes only the attempted child.
   * @param model Model belonging to this scene.
   * @param accessor Owning OHLC projection; nullopt introduces a gap.
   * @param options Existing chart mode, width and glyph configuration.
   * @param first Optional source of the first row; empty means zero. Runs in registration order.
   * @param prepared Optional callback after each preparation; empty rejects pending work with INVALID_ARGUMENT.
   * @return Observing node or typed mount/bind error; chart adds no input binding.
   */
  template <class T>
  std::expected<Node<Chart<T>>, ManagedError> chart(Node<Widget> parent, ManagedModel<T> model,
                                                    std::type_identity_t<ManagedChartAccessor<T>> accessor,
                                                    ChartOptions options = {}, ManagedChartOrigin first = {},
                                                    std::type_identity_t<ManagedViewHandler<T>> prepared = {});

  /// @return True while a managed virtual view requires layout-driven preparation.
  bool usesPreparedLayout() const noexcept override;

  /** @brief Read an owning final arranged value without performing layout or reporting errors.
   * @param node Full identity of a managed widget in this scene.
   * @return Final geometry, or typed affinity/liveness/reentry/INVALID_GEOMETRY failure.
   * Invalidation or any rearrangement revokes it until the next successful final frame.
   */
  std::expected<NodeGeometry, ManagedError> geometry(Node<Widget> node) const;

  /** @tparam Callback Owning compact action accepting const NodeGeometry& or no arguments.
   * @param node Live managed widget whose local, surface and clip bounds are observed.
   * @param callback Void, checked TreeError or EditorCommandResult callback owned by this scene.
   * @param origin Registration site retained in diagnostics.
   * @return Success or typed admission/budget/allocation failure, without partial registration.
   * Initial valid and changed values arrive on a subsequent UI scheduler dispatch outside frames.
   * Hidden/removal/close cancel pending values. Copy the callback argument to retain its owning value.
   */
  template <class Callback>
    requires detail::ManagedAction<Callback, NodeGeometry>
  std::expected<void, ManagedError> onLayout(Node<Widget> node, Callback callback,
                                             std::source_location origin = std::source_location::current()) {
    auto allowed = boundary(ManagedPhase::BIND, node);
    if (!allowed) {
      return allowed;
    }
    try {
      return registerLayout(node, actionAdapter<NodeGeometry>(std::move(callback)), origin);
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
    }
  }

  /** @param app Same borrowed application used at creation.
   * @return Success after final observation publication or original typed failure.
   */
  std::expected<void, TreeError> completedLayout(Application &app) override;
  /** @param app Same borrowed application used at creation.
   * @return Success after bounded pending-publication recovery or original typed failure.
   */
  std::expected<void, TreeError> afterDispatch(Application &app) override;
  /** @return Whether this scene owns undelivered layout work; unrelated DATA does not extend EOF. */
  bool hasPendingLayout() const noexcept override;

  /** @brief Prepare registered views from the pins captured by prepare(), in registration order.
   * @param app Same application used at creation.
   * @param snapshot Owning layout for this pass; hidden views retain cancellation semantics.
   * @return Original TreeError; run() also preserves the managed phase and failing node.
   */
  std::expected<void, TreeError> prepareViews(Application &app, const LayoutSnapshot &snapshot) override;

  /** @param stop Optional cooperative cancellation.
   * @return Runner statistics or complete managed, runner/restoration and cleanup error.
   */
  std::expected<ApplicationStats, ManagedError> run(std::stop_token stop = {});

  /** @param terminal Borrowed existing replay/custom transport, required to outlive run.
   * @param stop Optional cooperative cancellation.
   * @return Runner statistics or complete managed, runner/restoration and cleanup error.
   */
  std::expected<ApplicationStats, ManagedError> run(ApplicationTerminal &terminal, std::stop_token stop = {});

  /** @param terminal Ready-only transport borrowed until terminal step or destruction.
   * @param stop Cooperative cancellation sampled by each ready operation.
   * @return Success or complete managed start failure. UI-only and single-use.
   */
  std::expected<void, ManagedError> start(ApplicationReadyTerminal &terminal, std::stop_token stop = {});

  /** @return One bounded ready step or terminal managed failure; terminal results are retained.
   * UI-only; reentry is rejected without changing the active session.
   */
  std::expected<ApplicationStep, ManagedError> step();

  /** @brief Idempotently retire callbacks and subtree; properties survive until scene destruction.
   * A facade-installed shutdown action runs once before root/model retirement. Its failure does not skip
   * subtree cleanup; an independent tree cleanup failure is retained in the shutdown error's cleanup field.
   * @return Success, WRONG_THREAD, WOULD_BLOCK during an active session, managed preparation/frame or close reentry,
   * an exact facade shutdown failure, or tree cleanup error.
   */
  std::expected<void, ManagedError> close();

  /** @param app The same borrowed Application used at creation.
   * @param size Current viewport; custom layout remains the existing tree layout's responsibility.
   * @return Compatibility bridge error; run() retains complete managed details.
   */
  std::expected<void, TreeError> prepare(Application &app, CellSize size) override;

  /** @param app The same borrowed Application used at creation.
   * @param event Owning input borrowed for this call only.
   * @return Whether interception consumed the event, or compatibility bridge error.
   */
  std::expected<bool, TreeError> input(Application &app, const InputEvent &event) override;

  /** @param app The same runtime, already stopped by Application; cleanup failure is retained by run(). */
  void stop(Application &app) override;

private:
  struct ObserverRecord {
    ManagedScene *scene{};
    Node<Widget> node;
    Subscription subscription;
    std::function<std::expected<std::uint64_t, TreeError>()> revision;
    std::function<std::expected<std::uint64_t, TreeError>(const std::shared_ptr<ObserverRecord> &)> deliver;
    std::optional<std::uint64_t> successful;
    std::optional<ManagedError> shortFailure;
    const void *shortAttempt{};
    bool promotesFailure{};
    bool runtime{true};
    const char *operation{};
    std::source_location origin{};
    bool initialPending{true};
    bool running{};
    bool retired{};

    static void collectShortFailure(void *owner, ui::detail::SetupFailureEvent event, ManagedError *failure,
                                    const void *attempt) noexcept {
      auto &record = *static_cast<ObserverRecord *>(owner);
      if (event == ui::detail::SetupFailureEvent::LATCH && !record.shortFailure && failure) {
        record.shortFailure = *failure;
        record.shortAttempt = attempt;
        auto &scene = *record.scene;
        if (record.runtime && !scene.failure_ && scene.shortReported(*failure)) {
          // LATCH precedes recordError without intervening user code. Reserve that exact diagnostic receipt;
          // clearErrors already revokes it if user code explicitly clears history after the short operation.
          scene.failure_ = *failure;
          scene.failureDiagnostic_ =
            scene.errorCount_ < scene.errors_.size() ? std::optional<std::size_t>(scene.errorCount_) : std::nullopt;
          record.promotesFailure = true;
        }
      }
      if (event == ui::detail::SetupFailureEvent::NOTIFY && attempt == record.shortAttempt && record.promotesFailure) {
        record.scene->failureNotified_ = true;
      }
      if (event == ui::detail::SetupFailureEvent::END_ATTEMPT && attempt == record.shortAttempt) {
        record.shortAttempt = nullptr;
      }
    }
  };

  std::expected<void, ManagedError> observerAdmission(Node<Widget> node);
  template <class T, class Callback>
  void prepareObserverRecord(const std::shared_ptr<ObserverRecord> &record, const std::shared_ptr<Property<T>> &source,
                             Callback &&callback);
  template <class W, class T>
  std::expected<Subscription, TreeError> connectObserverRecord(Node<W> receiver,
                                                               const std::shared_ptr<Property<T>> &source,
                                                               const std::shared_ptr<ObserverRecord> &record);
  std::expected<void, TreeError> invokeObserver(const std::shared_ptr<ObserverRecord> &record, ManagedPhase phase);
  std::expected<void, TreeError> recoverObservers();
  void pruneObservers() noexcept;

  bool observerMountPending() const noexcept {
    return mountDepth_ && !observers_.empty();
  }

  template <class Value, class Callback>
  std::expected<void, ManagedError> onShortcutActions(Value &&shortcut, Callback &action,
                                                      detail::ShortcutActions actions, ShortcutOptions options,
                                                      std::source_location origin) {
    auto allowed = boundary(ManagedPhase::BIND, root_);

    if (!allowed) {
      return allowed;
    }

    const auto valid = validateShortcut(shortcut, options);

    if (!valid) {
      auto failure = error(ManagedPhase::BIND, root_, TreeError::INVALID_ARGUMENT);

      failure.shortcut = valid.error();

      return std::unexpected(std::move(failure));
    }

    if (keyHandlers_ >= options_.maxKeyHandlers) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NODE_LIMIT));
    }

    try {
      // Avoid both caller-side transfers and noexcept debug-vector moves before the checked boundary.
      Shortcut owned(std::forward<Value>(shortcut));
      auto adapter = actionAdapter<KeyEvent>(std::move(action));

      if (!adapter) {
        return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::INVALID_ARGUMENT));
      }

      return registerShortcut(std::move(owned), std::move(adapter), actions, origin);
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::CALLBACK_FAILED));
    }
  }

  struct KeyRecord;
  struct KeySnapshot;
  struct KeyPending;

  std::expected<void, ManagedError> registerLegacyKey(KeyEvent chord,
                                                      std::function<EditorCommandResult(const KeyEvent &)> action);
  std::expected<void, ManagedError> registerShortcut(Shortcut &&shortcut,
                                                     std::function<EditorCommandResult(const KeyEvent &)> action,
                                                     detail::ShortcutActions actions, std::source_location origin);
  std::expected<void, ManagedError> publishKey(std::shared_ptr<KeyRecord> record);
  std::expected<bool, ManagedError> dispatchKeys(const std::shared_ptr<KeySnapshot> &snapshot, const InputEvent &event);
  std::expected<bool, ManagedError> invokeKey(const std::shared_ptr<KeyRecord> &record, const KeyEvent &event);
  std::expected<bool, ManagedError> waitForKey(const std::shared_ptr<KeySnapshot> &snapshot,
                                               const std::shared_ptr<KeyRecord> &winner, std::size_t depth);
  static void resetKeys(KeyPending &pending) noexcept;

  friend struct style::ct::detail::Access;
  friend struct ui::detail::Access;
  friend struct ui::detail::RefAccess;
  friend struct ui::detail::SceneAccess;
  friend struct ui::detail::ClickAccess;
  friend struct ui::detail::NamedAccess;
  friend struct ui::detail::LayoutAccess;

  struct Entry;

  struct LayoutValue {
    NodeGeometry geometry;
    std::uint64_t revision{};
  };

  struct LayoutRecord {
    Node<Widget> node;
    std::function<EditorCommandResult(const NodeGeometry &)> action;
    std::source_location origin;
    SchedulerConnection<LayoutValue> connection;
    std::shared_ptr<const LayoutValue> pending;
    std::optional<NodeGeometry> delivered;
    std::uint64_t visibilityRevision{};
    bool queued{};
    std::size_t rejected{};
    bool retired{};
  };

  std::expected<void, ManagedError> registerLayout(Node<Widget> node,
                                                   std::function<EditorCommandResult(const NodeGeometry &)> action,
                                                   std::source_location origin);
  void retireLayouts(Entry &entry) noexcept;
  void deliverLayout(const std::shared_ptr<LayoutRecord> &record, std::shared_ptr<const LayoutValue> value);
  std::expected<void, TreeError> publishLayout(const std::shared_ptr<LayoutRecord> &record);

  struct ClickRecord {
    std::function<EditorCommandResult(ui::ClickParams &)> action;
    std::source_location origin;
  };

  struct ClickChain {
    // MSVC Debug's noexcept default constructor allocates a proxy; the count constructor is checked.
    std::vector<std::shared_ptr<ClickRecord>> records = std::vector<std::shared_ptr<ClickRecord>>(0);
    bool retired{};
  };

  struct CaptionBinding {
    Node<Control> node;
    ManagedState<std::string> model;
    std::optional<std::uint64_t> revision;
  };

  struct Entry {
    Node<Widget> node;
    std::shared_ptr<StyleTarget> target;
    std::variant<std::monostate, ActionBinding, CheckBoxBinding, CheckStateBinding, LineEditBinding> binding;
    std::unique_ptr<CaptionBinding> caption;
    std::unique_ptr<ManagedState<std::string>> textModel;
    ActionBinding focusScope;
    std::function<std::expected<void, TreeError>(Node<Widget>)> sync;
    std::function<std::expected<void, TreeError>(Node<Widget>)> customSync;
    std::function<std::expected<void, TreeError>(const LayoutSnapshot &)> viewPrepare;
    bool stylePending{};
    bool removed{};
    std::shared_ptr<ClickChain> clicks;
  };

  std::expected<void, ManagedError> syncCaption(Node<Control> node, CaptionBinding &binding, ManagedPhase phase);

  template <class T>
  std::expected<std::shared_ptr<detail::ManagedModelSourceOf<T>>, ManagedError> modelSource(ManagedModel<T> model,
                                                                                            Node<Widget> parent);

  template <class T, class W, class Prepare>
  std::expected<Node<W>, ManagedError> attachView(Node<W> node, std::shared_ptr<detail::ManagedModelSourceOf<T>> source,
                                                  Prepare prepare, ManagedViewHandler<T> handler);

  explicit ManagedScene(Application &app, ManagedOptions options);

  std::weak_ptr<ui::detail::SceneAnchor> facadeContext() const noexcept {
    if (owner_ != std::this_thread::get_id() || closed_) {
      return {};
    }

    return facade_;
  }

  std::expected<void, ManagedError>
  setInput(std::function<std::expected<bool, ManagedError>(const InputEvent &)> callback, std::size_t keyHandlers,
           std::shared_ptr<KeySnapshot> keys = {});

  std::expected<void, ManagedError>
  onButtonClick(Node<Button> node, std::function<EditorCommandResult(const ClickEvent &)> action, bool allowModified);

  std::expected<void, ManagedError> composeButtonClick(Node<Button> node,
                                                       std::function<EditorCommandResult(ui::ClickParams &)> action,
                                                       std::source_location origin);
  void retireClicks(Entry &entry) noexcept;
  void dispatchClicks(Node<Button> node, const std::shared_ptr<ClickChain> &chain,
                      const std::weak_ptr<const void> &lifetime, const ClickEvent &event);

  template <class Callback> std::function<EditorCommandResult(ui::ClickParams &)> clickAdapter(Callback action) {
    ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
    if constexpr (std::is_pointer_v<Callback> || requires { action.operator bool(); }) {
      if (!static_cast<bool>(action)) {
        return {};
      }
    }

    return ui::detail::currentSceneCallback(
      facadeContext(), [action = std::move(action)]([[maybe_unused]] ui::ClickParams &params) mutable {
        const auto invoke = [&]() -> decltype(auto) {
          if constexpr (std::invocable<Callback &, ui::ClickParams &>) {
            return action(params);
          } else {
            return action();
          }
        };

        if constexpr (std::same_as<decltype(invoke()), EditorCommandResult>) {
          return invoke();
        } else {
          return EditorCommandResult(detail::invokeIgnoringValue(invoke), {}, false);
        }
      });
  }

  static bool commandReported(const EditorCommandResult &command, const std::weak_ptr<const void> &lifetime) {
    return command.reported_ && !lifetime.expired() && !command.collector_.expired() &&
           !lifetime.owner_before(command.collector_) && !command.collector_.owner_before(lifetime);
  }

  template <class Event, class Callback>
  std::function<EditorCommandResult(const Event &)> actionAdapter(Callback action) {
    ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
    if constexpr (std::is_pointer_v<Callback> || requires { action.operator bool(); }) {
      if (!static_cast<bool>(action)) {
        return {};
      }
    }

    return ui::detail::currentSceneCallback(
      facadeContext(), [action = std::move(action)]([[maybe_unused]] const Event &event) mutable {
        const auto invoke = [&]() -> decltype(auto) {
          if constexpr (std::invocable<Callback &, const Event &>) {
            return action(event);
          } else {
            return action();
          }
        };

        if constexpr (std::same_as<decltype(invoke()), EditorCommandResult>) {
          return invoke();
        } else {
          return EditorCommandResult(detail::invokeIgnoringValue(invoke), {}, false);
        }
      });
  }

  static ManagedError error(ManagedPhase phase, Node<Widget> node, TreeError cause);
  std::expected<void, ManagedError> boundary(ManagedPhase phase, Node<Widget> node) const;
  std::shared_ptr<Entry> find(Node<Widget> node) const;
  std::expected<std::shared_ptr<Entry>, ManagedError> bindingEntry(Node<Widget> node);
  std::unexpected<ManagedError> rollback(Node<Widget> node, TreeError cause);
  std::expected<void, ManagedError> flushStyles();
  std::expected<void, ManagedError> pendingStyles(std::optional<std::vector<StyleRegistration>> &destination);
  void publishStyleMetadata() noexcept;
  std::expected<Invalidation, ManagedError>
  mutateStyles(StyleEngine::Mutation mutation, StyleLayerOptions options, StyleTier tier, std::string_view name,
               const StyleSheet *sheet, std::span<const StyleLayerUpdate> updates, StyleLayerHandle *added = nullptr,
               const detail::StyleThemeLease *lease = nullptr, const PssDocument *document = nullptr,
               std::optional<StyleLayerOptions> admissionOptions = std::nullopt);
  std::expected<void, ManagedError> checkThemeLease(detail::StyleThemeLease lease) const;
  detail::StyleThemeLease captureThemeLease() const noexcept;
  std::expected<detail::StyleThemeLease, ManagedError> attachTheme(const StyleSheet &sheet,
                                                                   detail::StyleThemeLease lease);
  std::expected<void, ManagedError> applyLegacyTheme(const StyleSheet &sheet, detail::StyleThemeLease lease);
  std::expected<void, ManagedError> applyTheme(const StyleSheet &sheet, detail::StyleThemeLease lease);
  std::optional<std::size_t> recordError(ManagedError failure, std::span<const ManagedDiagnosticContext> contexts,
                                         std::source_location origin, bool *observerInvoked = nullptr,
                                         bool runFailure = false,
                                         const ui::detail::SetupFailureTicket *shortTicket = nullptr,
                                         bool contextUnavailable = false) noexcept;
  bool notifyError(ManagedDiagnostic &diagnostic, bool runFailure,
                   const ui::detail::SetupFailureTicket *shortTicket = nullptr) noexcept;
  void reportShortError(ManagedError &failure, const char *operation, std::string_view name,
                        std::string_view expectedType, std::source_location origin) noexcept;

  bool shortReported(const ManagedError &failure) const noexcept {
    const auto &collector = failure.reportReceipt.collector_;

    return !collector.expired() && !lifetime_.owner_before(collector) && !collector.owner_before(lifetime_);
  }

  static bool focusReported(const ManagedError &failure) noexcept {
    const auto &collector = failure.reportReceipt.collector_;
    const std::weak_ptr<const void> empty;

    // Focus propagation preserves a delivered receipt even after its source scene is destroyed.
    return collector.owner_before(empty) || empty.owner_before(collector);
  }

  void remember(ManagedError failure, std::source_location origin = std::source_location::current(),
                std::span<const ManagedDiagnosticContext> contexts = {});
  std::expected<ApplicationStats, ManagedError> finish(std::expected<ApplicationStats, ApplicationError> result);
  void retainCloseFailure();
  std::expected<ApplicationStep, ManagedError> finishStep(std::expected<ApplicationStep, ApplicationError> result);

  Application &app_;
  std::thread::id owner_;
  ManagedOptions options_;
  std::shared_ptr<const void> lifetime_;
  std::weak_ptr<ui::detail::SceneAnchor> facade_;
  Node<Widget> root_;
  StyleEngine styles_;
  std::vector<std::shared_ptr<Entry>> entries_;
  std::vector<std::shared_ptr<void>> properties_;
  std::vector<std::shared_ptr<ObserverRecord>> observers_ = std::vector<std::shared_ptr<ObserverRecord>>(0);
  std::vector<std::shared_ptr<detail::ManagedModelSource>> models_;
  std::shared_ptr<std::function<std::expected<bool, ManagedError>(const InputEvent &)>> input_;
  std::shared_ptr<KeySnapshot> keys_;
  std::size_t keyHandlers_{};
  std::size_t clickHandlers_{};
  std::vector<std::shared_ptr<LayoutRecord>> layouts_ = std::vector<std::shared_ptr<LayoutRecord>>(0);
  std::uint64_t layoutRevision_{};
  std::shared_ptr<ManagedErrorHandler> errorObserver_;
  std::vector<ManagedDiagnostic> errors_;
  ManagedDiagnostic overflowDiagnostic_;
  std::size_t errorCount_{};
  std::size_t droppedErrors_{};
  std::size_t contextFailures_{};
  std::size_t observerFailures_{};
  std::size_t suppressedNotifications_{};
  std::optional<std::size_t> failureDiagnostic_;
  std::optional<ManagedError> failure_;
  std::optional<ManagedError> closeFailure_;
  bool closeFailureRetained_{};
  std::optional<TreeError> cleanup_;
  struct FocusReport;
  bool stylesDirty_{};
  mutable bool styling_{};
  bool preparing_{};
  bool recoveringObservers_{};
  bool pruningObservers_{};
  std::size_t observerCallbacks_{};
  std::size_t mountDepth_{};
  bool reporting_{};
  bool failureNotified_{};
  bool runFailureNotified_{};
  bool notifyingRunFailure_{};
  void *closeContext_{};
  std::expected<void, ManagedError> (*beforeClose_)(void *){};
  std::optional<std::expected<ApplicationStep, ManagedError>> sessionResult_;
  bool sessionActive_{}, sessionOperating_{}, stoppingSession_{};
  bool closing_{};
  bool closed_{};
  std::shared_ptr<bool> textMutating_ = std::make_shared<bool>(false);
};
} // namespace portals

#include <portals/detail/managed_derive.hpp>
#include <portals/detail/managed_views.hpp>
