// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/application.hpp>
#include <portals/editor_command.hpp>
#include <portals/line_edit.hpp>
#include <portals/managed_diagnostics.hpp>
#include <portals/managed_model.hpp>
#include <portals/style.hpp>
#include <portals/style_source.hpp>
#include <portals/virtual_preparation.hpp>

namespace portals {

class ManagedScene;

namespace ui::detail {
struct Access;
struct RefAccess;
struct SceneAccess;
struct ClickAccess;
} // namespace ui::detail

namespace style::ct::detail {
struct Access;
} // namespace style::ct::detail

namespace detail {
/** @brief Accepted compact callback results; command receipts retain exact reporting provenance.
 * @tparam T Exact callback result type.
 */
template <class T>
concept ManagedActionResult =
  std::same_as<T, void> || std::same_as<T, std::expected<void, TreeError>> || std::same_as<T, EditorCommandResult>;

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
  std::size_t maxNodes{4096};             ///< Maximum retained entries, including removals pending next preparation.
  std::size_t maxStates{256};             ///< Maximum scene-owned properties; they live until scene destruction.
  std::size_t maxModels{64};              ///< Maximum scene-owned models; they live until close().
  ManagedDiagnosticOptions diagnostics{}; ///< Positive limits for error history and owning causal traces.
  std::size_t maxKeyHandlers{64};    ///< Positive maximum composed onKey closures; direct onInput resets the count.
  std::size_t maxClickHandlers{256}; ///< Positive maximum retained pipe click callbacks across the scene.
  std::size_t maxClickHandlersPerNode{64}; ///< Positive maximum pipe click callbacks for one button.
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
    auto model = lock();

    return model ? (*model)->set(value) : std::unexpected(model.error());
  }

  /** @param value Candidate moved by Property after validation.
   * @return Original commit/delivery receipt, WRONG_THREAD, CLOSED, or Property's error.
   */
  std::expected<PropertyUpdate<T>, TreeError> set(T &&value) const {
    auto model = lock();

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
  std::weak_ptr<const void> lifetime_;
  std::thread::id owner_{};
  NodeId scene_{};
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

      if constexpr (std::derived_from<T, Control> || std::same_as<T, LineEdit>) {
        entry->target = std::make_shared<StyleTarget>(StyleTarget{.type = "Control"});
        entry->stylePending = true;
        stylesDirty_ = true;
      }

      if constexpr (std::derived_from<T, Control>) {
        entry->sync = [this, node = *node](Node<Widget> focused) {
          return syncControl(app_.tree(), node, focused);
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
      handle.lifetime_ = lifetime_;
      handle.owner_ = owner_;
      handle.scene_ = root_.id();
      properties_.push_back(std::move(model));

      return handle;
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NO_MEMORY));
    }
  }

  /** @param node Managed button without a prior event binding.
   * @param action Typed business callback, run after standard activation admission; may remove its node.
   * @return Success with scene-owned token or typed registration error.
   */
  std::expected<void, ManagedError> onClick(Node<Button> node, std::function<std::expected<void, TreeError>()> action);

  /** @tparam Callback Owning void, expected<void, TreeError> or EditorCommandResult action, optionally taking
   * ClickEvent.
   * @param node Live managed button without another action binding.
   * @param action Owning checked editor command callback; return Ref.undo/redo directly to preserve provenance.
   * @return Scoped registration success or typed binding failure. Parameterized callbacks admit modifiers;
   * no-argument callbacks retain legacy unmodified admission. An already reported exact returned failure
   * is not recorded again in the same live collector; independent failures and throws are still reported.
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
   * @param action Void, expected<void, TreeError> or EditorCommandResult callback.
   * @return Registration result with typed allocation/affinity errors; empty callbacks are rejected.
   * Composes owning closures in the single existing onInput slot. Newest matching chord wins;
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

      const auto handlerCount = keyHandlers_ + 1;
      return setInput(
        [this, chord, lifetime = std::weak_ptr(lifetime_), previous = input_,
         adapter = std::move(adapter)](const InputEvent &event) -> std::expected<bool, ManagedError> {
          const auto *key = std::get_if<KeyEvent>(&event);

          if (!key || key->action == KeyAction::RELEASE || key->key != chord.key || key->scalar != chord.scalar ||
              key->modifiers != chord.modifiers) {
            return previous ? (*previous)(event) : std::expected<bool, ManagedError>{false};
          }

          const auto command = adapter(*key);

          if (!command && !commandReported(command, lifetime)) {
            return std::unexpected(error(ManagedPhase::INPUT, root_, command.result_.error()));
          }

          return true;
        },
        handlerCount);
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::CALLBACK_FAILED));
    }
  }

  /** @param node Managed checkbox without a prior event binding.
   * @param model State created by this scene; shared internally on UI only.
   * @param changed Optional callback after the standard toggle, receiving its exact commit receipt.
   * @return Success or typed registration error. Returning an error cannot undo a committed toggle.
   */
  std::expected<void, ManagedError> bind(Node<CheckBox> node, ManagedState<bool> model,
                                         std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {});

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

  /** @brief Own one custom event callback, without subclassing or an additional router.
   * @tparam T Custom Widget type; callable signature is checked through std::function.
   * @param node Managed node without another binding; standard bound controls reject replacement.
   * @param callback Typed handler borrowing T and event for this invocation only. Errors stop run.
   * @return Success or typed registration error. An admitted handler survives self-removal.
   */
  template <class T>
  std::expected<void, ManagedError> onEvent(Node<T> node, std::type_identity_t<ManagedEventHandler<T>> callback) {
    auto entry = bindingEntry(node);

    if (!entry) {
      return std::unexpected(entry.error());
    }

    if (!callback) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }

    auto bound =
      app_.router().bind(node, [this, node, callback = std::move(callback)](T &widget, const RoutedEvent &event) {
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
    auto allowed = boundary(ManagedPhase::BIND, node);

    if (!allowed) {
      return allowed;
    }

    auto entry = find(node);

    if (!entry || !callback) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }

    try {
      entry->customSync = [this, node, callback = std::move(callback)](Node<Widget> focused) {
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

  /** @brief Idempotently retire callbacks and subtree; properties survive until scene destruction.
   * @return Success, WRONG_THREAD, WOULD_BLOCK inside managed preparation/frame, or tree cleanup error.
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
  friend struct style::ct::detail::Access;
  friend struct ui::detail::Access;
  friend struct ui::detail::RefAccess;
  friend struct ui::detail::SceneAccess;
  friend struct ui::detail::ClickAccess;

  struct ClickRecord {
    std::function<EditorCommandResult(ui::ClickParams &)> action;
    std::source_location origin;
  };

  struct ClickChain {
    // MSVC Debug's noexcept default constructor allocates a proxy; the count constructor is checked.
    std::vector<std::shared_ptr<ClickRecord>> records = std::vector<std::shared_ptr<ClickRecord>>(0);
    bool retired{};
  };

  struct Entry {
    Node<Widget> node;
    std::shared_ptr<StyleTarget> target;
    std::variant<std::monostate, ActionBinding, CheckBoxBinding, LineEditBinding> binding;
    std::function<std::expected<void, TreeError>(Node<Widget>)> sync;
    std::function<std::expected<void, TreeError>(Node<Widget>)> customSync;
    std::function<std::expected<void, TreeError>(const LayoutSnapshot &)> viewPrepare;
    bool stylePending{};
    bool removed{};
    std::shared_ptr<ClickChain> clicks;
  };

  template <class T>
  std::expected<std::shared_ptr<detail::ManagedModelSourceOf<T>>, ManagedError> modelSource(ManagedModel<T> model,
                                                                                            Node<Widget> parent);

  template <class T, class W, class Prepare>
  std::expected<Node<W>, ManagedError> attachView(Node<W> node, std::shared_ptr<detail::ManagedModelSourceOf<T>> source,
                                                  Prepare prepare, ManagedViewHandler<T> handler);

  explicit ManagedScene(Application &app, ManagedOptions options);

  std::expected<void, ManagedError>
  setInput(std::function<std::expected<bool, ManagedError>(const InputEvent &)> callback, std::size_t keyHandlers);

  std::expected<void, ManagedError>
  onButtonClick(Node<Button> node, std::function<EditorCommandResult(const ClickEvent &)> action, bool allowModified);

  std::expected<void, ManagedError> composeButtonClick(Node<Button> node,
                                                       std::function<EditorCommandResult(ui::ClickParams &)> action,
                                                       std::source_location origin);
  void retireClicks(Entry &entry) noexcept;
  void dispatchClicks(Node<Button> node, const std::shared_ptr<ClickChain> &chain,
                      const std::weak_ptr<const void> &lifetime, const ClickEvent &event);

  template <class Callback> static std::function<EditorCommandResult(ui::ClickParams &)> clickAdapter(Callback action) {
    if constexpr (std::is_pointer_v<Callback> || requires { action.operator bool(); }) {
      if (!static_cast<bool>(action)) {
        return {};
      }
    }

    return [action = std::move(action)](ui::ClickParams &params) mutable {
      const auto invoke = [&]() -> decltype(auto) {
        if constexpr (std::invocable<Callback &, ui::ClickParams &>) {
          return action(params);
        } else {
          return action();
        }
      };

      if constexpr (std::same_as<decltype(invoke()), EditorCommandResult>) {
        return invoke();
      } else if constexpr (std::same_as<decltype(invoke()), void>) {
        invoke();

        return EditorCommandResult({}, {}, false);
      } else {
        return EditorCommandResult(invoke(), {}, false);
      }
    };
  }

  static bool commandReported(const EditorCommandResult &command, const std::weak_ptr<const void> &lifetime) {
    return command.reported_ && !lifetime.expired() && !command.collector_.expired() &&
           !lifetime.owner_before(command.collector_) && !command.collector_.owner_before(lifetime);
  }

  template <class Event, class Callback>
  static std::function<EditorCommandResult(const Event &)> actionAdapter(Callback action) {
    if constexpr (std::is_pointer_v<Callback> || requires { action.operator bool(); }) {
      if (!static_cast<bool>(action)) {
        return {};
      }
    }

    return [action = std::move(action)]([[maybe_unused]] const Event &event) mutable {
      const auto invoke = [&]() -> decltype(auto) {
        if constexpr (std::invocable<Callback &, const Event &>) {
          return action(event);
        } else {
          return action();
        }
      };

      if constexpr (std::same_as<decltype(invoke()), EditorCommandResult>) {
        return invoke();
      } else if constexpr (std::same_as<decltype(invoke()), void>) {
        invoke();

        return EditorCommandResult({}, {}, false);
      } else {
        return EditorCommandResult(invoke(), {}, false);
      }
    };
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
                                         bool runFailure = false) noexcept;
  bool notifyError(ManagedDiagnostic &diagnostic, bool runFailure) noexcept;
  void remember(ManagedError failure, std::source_location origin = std::source_location::current(),
                std::span<const ManagedDiagnosticContext> contexts = {});
  std::expected<ApplicationStats, ManagedError> finish(std::expected<ApplicationStats, ApplicationError> result);

  Application &app_;
  std::thread::id owner_;
  ManagedOptions options_;
  std::shared_ptr<const void> lifetime_;
  Node<Widget> root_;
  StyleEngine styles_;
  std::vector<std::shared_ptr<Entry>> entries_;
  std::vector<std::shared_ptr<void>> properties_;
  std::vector<std::shared_ptr<detail::ManagedModelSource>> models_;
  std::shared_ptr<std::function<std::expected<bool, ManagedError>(const InputEvent &)>> input_;
  std::size_t keyHandlers_{};
  std::size_t clickHandlers_{};
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
  std::optional<TreeError> cleanup_;
  std::optional<TreeError> reportedFocusFailure_;
  bool stylesDirty_{};
  mutable bool styling_{};
  bool preparing_{};
  bool recoveringFocus_{};
  bool reporting_{};
  bool failureNotified_{};
  bool runFailureNotified_{};
  bool notifyingRunFailure_{};
  bool closed_{};
  std::shared_ptr<bool> textMutating_ = std::make_shared<bool>(false);
};
} // namespace portals

#include <portals/detail/managed_views.hpp>
