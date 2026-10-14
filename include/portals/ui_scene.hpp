// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed.hpp>
#include <portals/ui_ref.hpp>

namespace portals::ui {

/** @brief Policies for one lazy facade over the existing managed runtime. */
struct SceneOptions {
  ApplicationOptions application{};  ///< Existing runner policies, applied once at prepare.
  ManagedOptions managed{};          ///< Existing managed ownership and diagnostic budgets.
  SchedulerClock clock{};            ///< Optional borrowed replay clock; must outlive run.
  std::size_t maxConfigurations{64}; ///< Positive maximum queued configure/key/click callbacks before prepare.
};

/** @brief Final owning error envelope or bounded successful-run summary; no live widget references. */
struct SceneReport {
  std::optional<ManagedError> failure; ///< Exact primary plus independent runner/restoration/cleanup errors.
  std::source_location origin{};       ///< Primary failure call site, or final run call site for a summary.
  std::size_t retainedErrors{};        ///< Successful-run retained recoverable count; details remain in diagnostics().
  std::size_t droppedErrors{};         ///< Successful-run overflow count; not an additional failure.
};

/** @brief Owning copyable diagnostic sink; the report is borrowed only until the call returns.
 * Delivery occurs once, after terminal close/restoration was attempted and scene cleanup was attempted.
 * Delivery is read-only: typed lifecycle reentry returns WOULD_BLOCK without changing the completed outcome.
 * Fluent mutation/registration is ignored during delivery. A thrown sink exception falls back to stderr with the
 * original report, without observer recursion.
 */
using SceneReportHandler = std::function<void(const SceneReport &)>;

/** @brief Format a final report without C++ allocations, using symbolic error categories.
 * @param report Borrowed owning error/summary, never live terminal or widget state.
 * The caller must ensure terminal restoration has been attempted before calling this stderr sink.
 * A failed restoration uses ordinary stderr as the best-effort fallback; it is not declared successful.
 */
void reportToStderr(const SceneReport &report) noexcept;

namespace detail {
/** @brief Admit exact configuration results without silently discarding a checked failure.
 * @tparam Callback Owning callable receiving the borrowed ManagedScene&.
 */
template <class Callback>
concept SceneConfiguration = requires(Callback &callback, ManagedScene &scene) {
  { callback(scene) } -> std::same_as<void>;
} || requires(Callback &callback, ManagedScene &scene) {
  { callback(scene) } -> std::same_as<std::expected<void, ManagedError>>;
};

struct SceneAccess;
struct SceneAnchor;
struct LiveBinding;
struct LiveFactory;
} // namespace detail

class StyleStage;
struct StyleOptions;

/** @brief Weak facade observation, stable across Scene moves and revoked before capture cleanup.
 * Copies never retain a facade, application, managed owner, or widget.
 */
class SceneRef {
public:
  /** @brief Construct an empty observer; operations return CLOSED. */
  SceneRef() = default;

  /** @param origin Checked stop-request site.
   * @return Success or exact RUN failure; affinity precedes weak/raw access.
   * Before preparation only stop intent is recorded. This checked method never reports an error.
   */
  std::expected<void, ManagedError> quit(std::source_location origin = std::source_location::current()) const;

private:
  friend class Scene;
  friend struct detail::SceneAccess;
  friend struct QuitAction;

  SceneRef(std::weak_ptr<detail::SceneAnchor> anchor, std::thread::id owner)
      : anchor_(std::move(anchor)), owner_(owner) {
  }

  void report(const ManagedError &failure, std::source_location origin) const;
  std::weak_ptr<detail::SceneAnchor> anchor_;
  std::thread::id owner_{};
};

/** @brief Owning weak stop action for existing compact key/button registrations. */
struct QuitAction {
  SceneRef scene;                ///< Weak stable facade observation.
  std::source_location origin{}; ///< Original action site for recoverable diagnostics.

  /** @brief Request stop; report exact errors only to a live UI collector. */
  void operator()() const {
    auto result = scene.quit(origin);

    if (!result) {
      scene.report(result.error(), origin);
    }
  }
};

/** @param scene Weak facade observation, safe across owning facade moves.
 * @param origin Action construction site retained in diagnostics.
 * @return Void compact action; SceneRef::quit retains its explicit checked result.
 */
inline QuitAction quit(SceneRef scene, std::source_location origin = std::source_location::current()) {
  return {std::move(scene), origin};
}

/** @brief Checked factory/setup/run policies. */
struct RunAppOptions {
  SceneOptions scene{};            ///< Existing facade/application/managed policies.
  SceneReportHandler report{};     ///< Owning optional final sink, available before factory invocation.
  ApplicationTerminal *terminal{}; ///< Borrowed through synchronous run; null selects native.
  std::stop_token stop{};          ///< Existing optional cancellation.
};

namespace detail {
/** @brief Stable weak facade publication; mutable fields are accessed only after UI admission. */
struct SceneAnchor {
  const std::thread::id owner = std::this_thread::get_id(); ///< Immutable UI identity.
  void *state{}; ///< Borrowed stable facade State, valid only while the anchor is published.
  /** @brief Checked stop bridge, callable only while this anchor publishes its UI owner. */
  std::expected<void, ManagedError> (*quit)(void *, std::source_location){};
  std::shared_ptr<RefTarget> events = std::make_shared<RefTarget>(); ///< Existing weak collector context.
};

/** @brief Erased facade-owned binding; concrete bindings reuse the existing LiveTheme controller. */
struct LiveBinding {
  virtual ~LiveBinding() = default;

  /** @param scene Existing managed owner.
   * @param anchor Receiving facade identity; publication is weak and occurs only on successful attach.
   * @return True for first bind, false for checked reuse, or exact failure.
   * Controller/publication candidates are allocated before installing the theme.
   */
  virtual std::expected<bool, ManagedError> attach(ManagedScene &scene, const std::shared_ptr<SceneAnchor> &anchor) = 0;
};

/** @brief Erased owning live descriptor factory invoked only within facade admission.
 * Concrete descriptor identity and weak publication storage already exist before invocation.
 */
struct LiveFactory {
  virtual ~LiveFactory() = default;

  /** @param anchor Receiving facade identity checked before any controller allocation.
   * @return Existing same-scene binding, newly preallocated candidate, or exact typed failure.
   */
  virtual std::expected<std::shared_ptr<LiveBinding>, ManagedError>
  create(const std::shared_ptr<SceneAnchor> &anchor) = 0;
};

} // namespace detail

/** @brief Single-use lazy owner of one Application and ManagedScene; no second runtime.
 * Construct and operate on the UI thread. Moving preserves affinity and stable callback storage;
 * move assignment is deliberately unavailable. Destruction during an operation, on another thread,
 * or within a tree frame follows ManagedScene's terminate contract. Recipe/capture construction may
 * throw before a checked boundary. Fluent failures are sticky and observable through checked()/run().
 * Foreign-thread fluent calls leave state untouched; checked operations there return WRONG_THREAD.
 */
class Scene {
public:
  /** @brief Transfer an idle UI-affine owner; wrong-thread/active moves terminate.
   * @param other Source owner, left empty with checked CLOSED results.
   */
  Scene(Scene &&other) noexcept;
  Scene &operator=(Scene &&) = delete;
  Scene(const Scene &) = delete;
  Scene &operator=(const Scene &) = delete;
  /** @brief Close the managed owner before releasing its Application, on UI outside operations/frames. */
  ~Scene();

  /** @param origin Preparation call site retained if this boundary fails.
   * @return This owner, preserving the first failure for checked()/run(). Does not open a terminal.
   */
  Scene &prepare(std::source_location origin = std::source_location::current()) &;

  /** @param origin Preparation call site.
   * @return Consuming owner for temporary fluent chains.
   */
  Scene &&prepare(std::source_location origin = std::source_location::current()) && {
    static_cast<Scene &>(*this).prepare(origin);

    return std::move(*this);
  }

  /** @param origin Preparation call site retained in the collector.
   * @return Success for an already prepared scene, or first typed failure; no terminal I/O.
   */
  std::expected<void, ManagedError> prepareChecked(std::source_location origin = std::source_location::current());
  /** @return First facade failure or success; WRONG_THREAD/CLOSED for foreign/moved-from handles. */
  std::expected<void, ManagedError> checked() const;
  /** @return Borrowed prepared owner, or typed affinity/lifecycle error; does not prepare or record a failure.
   * The reference is UI-only and must not outlive this facade. Runtime mutations use existing boundaries.
   */
  std::expected<std::reference_wrapper<ManagedScene>, ManagedError> managed() const;
  /** @return Existing collector snapshot, or the single pre-owner fallback and notification flags.
   * Allocation failure is NO_MEMORY. Snapshotting never prepares or reports an error.
   */
  std::expected<ManagedDiagnosticBatch, TreeError> diagnostics() const;

  /** @param sheet Owning replacement theme; stored before prepare, delegated afterwards.
   * @param origin Mutation call site retained on failure.
   * @return This owner. Permitted runtime theme changes use existing managed boundaries.
   */
  Scene &theme(StyleSheet sheet, std::source_location origin = std::source_location::current()) &;

  /** @param sheet Owning replacement theme.
   * @param origin Mutation call site.
   * @return Consuming owner for temporary fluent chains.
   */
  Scene &&theme(StyleSheet sheet, std::source_location origin = std::source_location::current()) && {
    applyTheme(sheet, origin);

    return std::move(*this);
  }

  /** @brief Opt into independent theme/application/user ownership at the existing configuration boundary.
   * @param options Aggregate logical layer budgets, including the two permanent base slots.
   * @param origin Configuration call site retained in the collector on failure.
   * @return This owner; queued before prepare, applied immediately afterwards without terminal I/O.
   * The successful transition revokes existing legacy LiveTheme controllers. Equal options are a no-op.
   */
  Scene &styleLayers(StyleLayerOptions options = {}, std::source_location origin = std::source_location::current()) &;

  /** @param options Aggregate logical layer budgets.
   * @param origin Configuration call site.
   * @return Consuming owner for temporary fluent chains.
   */
  Scene &&styleLayers(StyleLayerOptions options = {},
                      std::source_location origin = std::source_location::current()) && {
    static_cast<Scene &>(*this).styleLayers(options, origin);

    return std::move(*this);
  }

  /** @return Weak UI observer of stable state; cleanup and moved-from owners produce CLOSED. */
  SceneRef ref() const;

  /** @param stage Immutable owning style payload and stage identity.
   * @param origin Registration site retained for deferred parse/read/admission failures.
   * @return This owner after registration through the existing atomic styleDocument boundary.
   */
  Scene &style(const StyleStage &stage, std::source_location origin = std::source_location::current()) &;

  /** @param stage Immutable owning style payload/identity.
   * @param origin Registration site.
   * @return Consuming owner after the same checked registration.
   */
  Scene &&style(const StyleStage &stage, std::source_location origin = std::source_location::current()) && {
    static_cast<Scene &>(*this).style(stage, origin);

    return std::move(*this);
  }

  /** @tparam Source Input accepted by an owning StyleStage constructor, available in ui_style.hpp.
   * @param source Runtime bytes, checked descriptor, document or explicit file source.
   * @param options Existing style admission/source policy.
   * @param origin Registration site.
   * @return This owner after the common stage path; source/stage construction may throw before entry.
   */
  template <class Source>
    requires std::constructible_from<StyleStage, Source &&, const StyleOptions &, std::source_location>
  Scene &style(Source &&source, const StyleOptions &options,
               std::source_location origin = std::source_location::current()) &;

  /** @tparam Source Input accepted by an owning StyleStage constructor.
   * @param source Bytes/descriptor/document/file source.
   * @param options Existing style policy.
   * @param origin Registration site.
   * @return Consuming owner after the same registration path.
   */
  template <class Source>
    requires std::constructible_from<StyleStage, Source &&, const StyleOptions &, std::source_location>
  Scene &&style(Source &&source, const StyleOptions &options,
                std::source_location origin = std::source_location::current()) &&;

  /** @brief Add one scene-owned override; enable styleLayers() before this configuration.
   * @param tier APPLICATION or USER cascade tier; base slots use their explicit managed operations.
   * @param name Owning tier-unique layer name, retained until explicit removal or scene cleanup.
   * @param sheet Owning rules, independent of the theme slot and retained by the existing style engine.
   * @param origin Configuration call site retained on typed failure.
   * @return This owner. The collector retains errors; use managed().addLayer when a removal handle is needed.
   * Argument construction may throw before entry; copies within the facade are captured as typed errors.
   */
  Scene &styleLayer(StyleTier tier, std::string name, StyleSheet sheet,
                    std::source_location origin = std::source_location::current()) &;

  /** @param tier APPLICATION or USER cascade tier.
   * @param name Owning layer name.
   * @param sheet Owning override rules.
   * @param origin Configuration call site.
   * @return Consuming owner for temporary fluent chains.
   */
  Scene &&styleLayer(StyleTier tier, std::string name, StyleSheet sheet,
                     std::source_location origin = std::source_location::current()) && {
    applyStyleLayer(tier, name, sheet, origin);

    return std::move(*this);
  }

  /** @param observer Owning non-veto observer; exceptions remain separate from the original cause.
   * @param origin Registration call site retained on failure.
   * @return This owner; registration after sticky failure may deliver its pending notification once.
   */
  Scene &onError(ManagedErrorHandler observer, std::source_location origin = std::source_location::current()) &;

  /** @param observer Owning observer.
   * @param origin Registration call site.
   * @return Consuming owner for temporary fluent chains.
   */
  Scene &&onError(ManagedErrorHandler observer, std::source_location origin = std::source_location::current()) && {
    static_cast<Scene &>(*this).onError(std::move(observer), origin);

    return std::move(*this);
  }

  /** @param sink Owning copyable callable accepting const SceneReport&; empty restores the default stderr sink.
   * @param origin Registration call site retained on failure.
   * @return This owner. Independent of onError and its registration order; no delivery during registration.
   * Capture construction before entry may throw. Explicitly borrowed services must outlive runApp.
   */
  Scene &reportOnError(SceneReportHandler sink = {}, std::source_location origin = std::source_location::current()) &;

  /** @param sink Owning copyable reporting sink, or empty for the default.
   * @param origin Registration call site.
   * @return Consuming owner for temporary fluent chains.
   */
  Scene &&reportOnError(SceneReportHandler sink = {},
                        std::source_location origin = std::source_location::current()) && {
    static_cast<Scene &>(*this).reportOnError(std::move(sink), origin);

    return std::move(*this);
  }

  /** @tparam Callback Owning action returning void or exactly expected<void, ManagedError>.
   * @param callback Configuration invoked once, in registration order after complete mount.
   * @param origin Registration call site retained across lazy preparation.
   * @return This owner; captures are owned, while explicitly borrowed services must outlive use.
   * Ignoring low-level results inside a void callback remains the caller's responsibility.
   * expected<void, ManagedError> carries no diagnostic receipt: a returned failure adds a
   * scene.configure boundary record even if the callback already reported its original failure.
   */
  template <detail::SceneConfiguration Callback>
  Scene &configure(Callback callback, std::source_location origin = std::source_location::current()) & {
    configureImpl(callback, origin);

    return *this;
  }

  /** @tparam Callback Configuration callable accepted by the lvalue overload.
   * @param callback Owning configuration.
   * @param origin Registration call site.
   * @return Consuming owner for temporary fluent chains.
   */
  template <detail::SceneConfiguration Callback>
  Scene &&configure(Callback callback, std::source_location origin = std::source_location::current()) && {
    configureImpl(callback, origin);

    return std::move(*this);
  }

  /** @tparam Callback Existing owning compact action, optionally taking const KeyEvent&.
   * @param selection Immutable owning alternatives with exact native phase admission.
   * @param callback Action transferred into deferred scene configuration.
   * @param origin Registration call site, independent of helper construction.
   * @return This owner, retaining typed helper errors in the existing collector.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &onKey(const KeySelection &selection, Callback callback,
               std::source_location origin = std::source_location::current()) & {
    onSelectionImpl(selection, callback, origin);

    return *this;
  }

  /** @tparam Callback Existing owning compact action, optionally taking const KeyEvent&.
   * @param selection Immutable owning alternatives with exact native phase admission.
   * @param callback Action transferred into deferred scene configuration.
   * @param origin Registration call site, independent of helper construction.
   * @return Consuming owner for temporary fluent chains.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &&onKey(const KeySelection &selection, Callback callback,
                std::source_location origin = std::source_location::current()) && {
    onSelectionImpl(selection, callback, origin);

    return std::move(*this);
  }

  /** @tparam Callback Existing compact void/expected<void, TreeError>/EditorCommandResult action.
   * @param chord Exact key/scalar/modifiers; PRESS and REPEAT retain the existing admission contract.
   * @param callback Owning handler, optionally receiving the borrowed KeyEvent&.
   * @param origin Registration call site retained across lazy preparation.
   * @return This owner; delegates the existing bounded newest-match-first onKey composition.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &onKey(KeyEvent chord, Callback callback, std::source_location origin = std::source_location::current()) & {
    onKeyImpl(chord, callback, origin);

    return *this;
  }

  /** @tparam Callback Existing compact key action.
   * @param chord Exact admitted chord.
   * @param callback Owning handler.
   * @param origin Registration call site.
   * @return Consuming owner for temporary fluent chains.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &&onKey(KeyEvent chord, Callback callback, std::source_location origin = std::source_location::current()) && {
    onKeyImpl(chord, callback, origin);

    return std::move(*this);
  }

  /** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
   * @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Shortcut source normalized and owned before deferral; no source text is retained.
   * @param callback Checked action invoked at most once per matching event.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return This owner. Newest registration wins through the existing managed input slot.
   */
  template <portals::detail::ShortcutSource Source, class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &onKey(Source &&source, Callback callback, std::source_location origin = std::source_location::current()) & {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, {}, {}, origin);

    return *this;
  }

  /** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
   * @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Shortcut source normalized and owned before deferral; no source text is retained.
   * @param callback Checked action invoked at most once per matching event.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return Consuming owner for fluent chains. Newest registration wins through the existing managed input slot.
   */
  template <portals::detail::ShortcutSource Source, class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &&onKey(Source &&source, Callback callback, std::source_location origin = std::source_location::current()) && {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, {}, {}, origin);

    return std::move(*this);
  }

  /** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
   * @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Shortcut source normalized and owned before deferral; no source text is retained.
   * @param callback Checked action invoked at most once per matching event.
   * @param behavior PRESS-only admission unless repeat is explicitly enabled.
   * @param options Positive finite parser budgets.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return This owner. Newest registration wins through the existing managed input slot.
   */
  template <portals::detail::ShortcutSource Source, class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &onKey(Source &&source, Callback callback, ShortcutBehavior behavior, ShortcutOptions options = {},
               std::source_location origin = std::source_location::current()) & {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, behavior, options, origin);

    return *this;
  }

  /** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
   * @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Shortcut source normalized and owned before deferral; no source text is retained.
   * @param callback Checked action invoked at most once per matching event.
   * @param behavior PRESS-only admission unless repeat is explicitly enabled.
   * @param options Positive finite parser budgets.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return Consuming owner for fluent chains. Newest registration wins through the existing managed input slot.
   */
  template <portals::detail::ShortcutSource Source, class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &&onKey(Source &&source, Callback callback, ShortcutBehavior behavior, ShortcutOptions options = {},
                std::source_location origin = std::source_location::current()) && {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, behavior, options, origin);

    return std::move(*this);
  }

  /** @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Alternatives borrowed for this call and copied before deferral.
   * @param callback Checked action invoked at most once per matching event.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return This owner. Newest registration wins through the existing managed input slot.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &onKey(std::initializer_list<key::Chord> source, Callback callback,
               std::source_location origin = std::source_location::current()) & {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, {}, {}, origin);

    return *this;
  }

  /** @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Alternatives borrowed for this call and copied before deferral.
   * @param callback Checked action invoked at most once per matching event.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return Consuming owner for fluent chains. Newest registration wins through the existing managed input slot.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &&onKey(std::initializer_list<key::Chord> source, Callback callback,
                std::source_location origin = std::source_location::current()) && {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, {}, {}, origin);

    return std::move(*this);
  }

  /** @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Alternatives borrowed for this call and copied before deferral.
   * @param callback Checked action invoked at most once per matching event.
   * @param behavior PRESS-only admission unless repeat is explicitly enabled.
   * @param options Positive finite parser budgets.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return This owner. Newest registration wins through the existing managed input slot.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &onKey(std::initializer_list<key::Chord> source, Callback callback, ShortcutBehavior behavior,
               ShortcutOptions options = {}, std::source_location origin = std::source_location::current()) & {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, behavior, options, origin);

    return *this;
  }

  /** @tparam Callback Owning compact action optionally receiving a semantic KeyEvent.
   * @param source Alternatives borrowed for this call and copied before deferral.
   * @param callback Checked action invoked at most once per matching event.
   * @param behavior PRESS-only admission unless repeat is explicitly enabled.
   * @param options Positive finite parser budgets.
   * @param origin Registration call site preserved through setup and input diagnostics.
   * @return Consuming owner for fluent chains. Newest registration wins through the existing managed input slot.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  Scene &&onKey(std::initializer_list<key::Chord> source, Callback callback, ShortcutBehavior behavior,
                ShortcutOptions options = {}, std::source_location origin = std::source_location::current()) && {
    onShortcutSourceImpl(std::forward<decltype(source)>(source), callback, behavior, options, origin);

    return std::move(*this);
  }

  /** @param stop Optional worker-safe cancellation for the single admitted native run.
   * @param origin Runner call site.
   * @return Statistics or first typed cause plus independent runner/cleanup errors; always closes after admission.
   */
  std::expected<ApplicationStats, ManagedError> run(std::stop_token stop = {},
                                                    std::source_location origin = std::source_location::current());
  /** @param terminal Borrowed existing headless/custom transport; must outlive this call.
   * @param stop Optional cancellation.
   * @param origin Runner call site.
   * @return Same single-use result and cleanup policy as native run.
   */
  std::expected<ApplicationStats, ManagedError> run(ApplicationTerminal &terminal, std::stop_token stop = {},
                                                    std::source_location origin = std::source_location::current());

  /** @brief Request orderly exit through the existing application cancellation path.
   * @param origin Exit request call site.
   * @return Success, WRONG_THREAD, CLOSED, or sticky WOULD_BLOCK during notification/close; read-only reporting rejects
   * with WOULD_BLOCK. Idempotent while live. Before complete preparation only the intent is retained; active callbacks
   * finish normally and no widget or callback capture is retired by this call.
   */
  std::expected<void, ManagedError> quit(std::source_location origin = std::source_location::current());

  /** @brief Run a standalone scene with standard post-restoration error reporting.
   * @param stop Optional worker-safe cancellation.
   * @param origin Runner call site.
   * @return Zero on successful run, one on a typed failure. reportOnError selects diagnostic delivery.
   * Recoverable-only reports preserve zero status and receive an aggregate summary by default.
   * onError remains the independent operation-reaction channel; the final report is delivered once.
   * Wrong-thread/moved-from calls return one quietly; typed run()/checked() retain their boundary cause.
   */
  int runApp(std::stop_token stop = {}, std::source_location origin = std::source_location::current());

  /** @param terminal Borrowed custom/replay transport, outliving this call.
   * @param stop Optional cancellation.
   * @param origin Runner call site.
   * @return Same exit status, cleanup and reporting policy as native runApp.
   */
  int runApp(ApplicationTerminal &terminal, std::stop_token stop = {},
             std::source_location origin = std::source_location::current());
  /** @param origin Cleanup call site.
   * @return Idempotent explicit cleanup result; reentry returns WOULD_BLOCK without retiring an active owner.
   * The first facade failure remains observable through checked()/run().
   */
  std::expected<void, ManagedError> close(std::source_location origin = std::source_location::current());

private:
  friend struct detail::SceneAccess;
  using Configuration = std::move_only_function<std::expected<void, ManagedError>(ManagedScene &)>;
  struct State;

  template <detail::SceneConfiguration Callback> void configureImpl(Callback &callback, std::source_location origin) {
    if (!admitConfiguration(false, origin)) {
      return;
    }

    try {
      addConfiguration(
        [callback = std::move(callback)](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
          if constexpr (std::same_as<std::invoke_result_t<Callback &, ManagedScene &>, void>) {
            std::invoke(callback, scene);

            return {};
          } else {
            return std::invoke(callback, scene);
          }
        },
        false, origin);
    } catch (const std::bad_alloc &) {
      configurationException(TreeError::NO_MEMORY, origin);
    } catch (...) {
      configurationException(TreeError::CALLBACK_FAILED, origin);
    }
  }

  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  void onKeyImpl(KeyEvent chord, Callback &callback, std::source_location origin) {
    if (!admitConfiguration(true, origin)) {
      return;
    }

    try {
      addConfiguration(
        [chord, callback = std::move(callback)](ManagedScene &scene) mutable {
          return scene.onKey(chord, std::move(callback));
        },
        true, origin);
    } catch (const std::bad_alloc &) {
      configurationException(TreeError::NO_MEMORY, origin);
    } catch (...) {
      configurationException(TreeError::CALLBACK_FAILED, origin);
    }
  }

  template <class Callback>
  void onSelectionImpl(const KeySelection &selection, Callback &callback, std::source_location origin) {
    if (!admitConfiguration(true, origin)) {
      return;
    }

    const auto &shortcut = portals::detail::KeySelectionAccess::shortcut(selection);

    if (!shortcut || !*shortcut) {
      shortcutConfigurationFailure(shortcut ? ShortcutError{ShortcutErrorKind::INVALID_CHORD, 0} : shortcut.error(),
                                   origin);

      return;
    }

    try {
      addConfiguration(
        [selection, callback = std::move(callback), origin](ManagedScene &scene) mutable {
          return scene.onKey(selection, std::move(callback), origin);
        },
        true, origin);
    } catch (const std::bad_alloc &) {
      configurationException(TreeError::NO_MEMORY, origin);
    } catch (...) {
      configurationException(TreeError::CALLBACK_FAILED, origin);
    }
  }

  template <class Source, class Callback>
  void onShortcutSourceImpl(Source &&source, Callback &callback, ShortcutBehavior behavior, ShortcutOptions options,
                            std::source_location origin) {
    if (!admitConfiguration(true, origin)) {
      return;
    }

    try {
      auto shortcut = portals::detail::prepareShortcut(std::forward<decltype(source)>(source), options);

      onShortcutImpl(shortcut, callback, behavior, options, origin);
    } catch (const std::bad_alloc &) {
      configurationException(TreeError::NO_MEMORY, origin);
    } catch (...) {
      configurationException(TreeError::CALLBACK_FAILED, origin);
    }
  }

  template <class Callback>
  void onShortcutImpl(std::expected<Shortcut, ShortcutError> &shortcut, Callback &callback, ShortcutBehavior behavior,
                      ShortcutOptions options, std::source_location origin) {
    if (!admitConfiguration(true, origin)) {
      return;
    }

    if (!shortcut) {
      shortcutConfigurationFailure(shortcut.error(), origin);

      return;
    }

    try {
      addConfiguration(
        [shortcut = std::move(*shortcut), callback = std::move(callback), behavior, options,
         origin](ManagedScene &scene) mutable {
          return scene.onKey(std::move(shortcut), std::move(callback), behavior, options, origin);
        },
        true, origin);
    } catch (const std::bad_alloc &) {
      configurationException(TreeError::NO_MEMORY, origin);
    } catch (...) {
      configurationException(TreeError::CALLBACK_FAILED, origin);
    }
  }

  template <detail::SceneConfiguration Callback> void onClickImpl(Callback &callback, std::source_location origin) {
    if (!admitConfiguration(true, origin)) {
      return;
    }

    try {
      addConfiguration(std::move(callback), true, origin);
    } catch (const std::bad_alloc &) {
      configurationException(TreeError::NO_MEMORY, origin);
    } catch (...) {
      configurationException(TreeError::CALLBACK_FAILED, origin);
    }
  }

  void applyLiveBinding(std::shared_ptr<detail::LiveFactory> factory, std::source_location origin);
  void setupFailure(ManagedError failure, std::source_location origin);
  void applyTheme(const StyleSheet &sheet, std::source_location origin);
  void applyStyleLayer(StyleTier tier, const std::string &name, const StyleSheet &sheet, std::source_location origin);
  Scene(Configuration recipe, SceneOptions options, std::source_location origin);
  bool admitConfiguration(bool runtime, std::source_location origin);
  void addConfiguration(Configuration callback, bool runtime, std::source_location origin);
  void configurationException(TreeError cause, std::source_location origin);
  void shortcutConfigurationFailure(ShortcutError cause, std::source_location origin);
  std::expected<ApplicationStats, ManagedError> runImpl(ApplicationTerminal *terminal, std::stop_token stop,
                                                        std::source_location origin);
  int runAppImpl(ApplicationTerminal *terminal, std::stop_token stop, std::source_location origin);

  std::thread::id owner_;
  std::unique_ptr<State> state_;
};

namespace detail {
/** @brief Internal bridge from consuming recipes to one stable facade owner. */
struct SceneAccess {

  /** @tparam Callback Owning checked click registration against the existing managed scene.
   * @param owner Facade receiving the deferred or runtime registration.
   * @param callback Owning configuration, transferred only after UI admission.
   * @param origin Original pipe registration site retained in diagnostics.
   */
  template <SceneConfiguration Callback>
  static void onClick(Scene &owner, Callback &callback, std::source_location origin) {
    owner.onClickImpl(callback, origin);
  }

  /** @param owner Stationary facade retaining an existing concrete live controller.
   * @param factory Owning descriptor factory, invoked under facade admission before attaching a controller.
   * @param origin Stage registration site.
   */
  static void live(Scene &owner, std::shared_ptr<LiveFactory> factory, std::source_location origin) {
    owner.applyLiveBinding(std::move(factory), origin);
  }

  /** @param owner Facade retaining the first typed setup failure.
   * @param failure Exact checked setup cause.
   * @param origin Setup invocation site.
   */
  static void setupFailure(Scene &owner, ManagedError failure, std::source_location origin) {
    owner.setupFailure(std::move(failure), origin);
  }

  /** @param owner Existing facade receiving its stored pipe theme.
   * @param sheet Owning stage storage borrowed only for the checked copy.
   * @param origin Stage registration call site.
   */
  static void theme(Scene &owner, const StyleSheet &sheet, std::source_location origin) {
    owner.applyTheme(sheet, origin);
  }

  /** @param owner Existing facade receiving the checked configuration.
   * @param tier APPLICATION or USER cascade tier.
   * @param name Owning stage name, copied at the checked boundary.
   * @param sheet Owning stage rules, copied at the checked boundary.
   * @param origin Stage registration call site.
   */
  static void styleLayer(Scene &owner, StyleTier tier, const std::string &name, const StyleSheet &sheet,
                         std::source_location origin) {
    owner.applyStyleLayer(tier, name, sheet, origin);
  }

  /** @tparam Callback Configuration action accepted by the facade.
   * @param owner Existing facade receiving its stored stage callback.
   * @param callback Stage storage transferred inside the checked implementation.
   * @param origin Stage registration call site.
   */
  template <detail::SceneConfiguration Callback>
  static void configure(Scene &owner, Callback &callback, std::source_location origin) {
    owner.configureImpl(callback, origin);
  }

  /** @tparam Callback Existing compact key action accepted by the facade.
   * @param owner Existing facade receiving its stored stage callback.
   * @param chord Existing exact admitted chord.
   * @param callback Stage storage transferred inside the checked implementation.
   * @param origin Stage registration call site.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  static void onKey(Scene &owner, KeyEvent chord, Callback &callback, std::source_location origin) {
    owner.onKeyImpl(chord, callback, origin);
  }

  /** @tparam Callback Existing owning compact action.
   * @param owner Facade receiving checked selected registration.
   * @param selection Immutable owning alternatives and exact action set.
   * @param callback Stored callback transferred only inside the configuration catch.
   * @param origin Original onKey registration call site.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  static void onSelection(Scene &owner, const KeySelection &selection, Callback &callback,
                          std::source_location origin) {
    owner.onSelectionImpl(selection, callback, origin);
  }

  /** @tparam Callback Existing owning compact key action.
   * @param owner Facade receiving the stored registration.
   * @param shortcut Owned parsed alternatives or allocation-free syntax error.
   * @param callback Stored action transferred after admission.
   * @param behavior Explicit repeat policy.
   * @param options Original finite parser budgets.
   * @param origin Original stage registration call site.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  static void onShortcut(Scene &owner, std::expected<Shortcut, ShortcutError> &shortcut, Callback &callback,
                         ShortcutBehavior behavior, ShortcutOptions options, std::source_location origin) {
    owner.onShortcutImpl(shortcut, callback, behavior, options, origin);
  }

  /** @tparam Callback Existing owning compact key action.
   * @param owner Facade receiving checked pending stage storage.
   * @param shortcut Shared owning transfer receipt or allocation-free syntax error.
   * @param callback Stored action transferred after admission.
   * @param behavior Explicit repeat policy.
   * @param options Original finite parser budgets.
   * @param origin Original stage registration call site.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, KeyEvent>
  static void onShortcut(Scene &owner, std::expected<std::shared_ptr<Shortcut>, ShortcutError> &shortcut,
                         Callback &callback, ShortcutBehavior behavior, ShortcutOptions options,
                         std::source_location origin) {
    if (!shortcut) {
      std::expected<Shortcut, ShortcutError> failure = std::unexpected(shortcut.error());

      owner.onShortcutImpl(failure, callback, behavior, options, origin);

      return;
    }

    owner.onShortcutSourceImpl(std::move(**shortcut), callback, behavior, options, origin);
  }

  /** @param owner Managed owner currently invoking its observer on UI.
   * @return True only for the exact primary-run diagnostic being notified; nested reports restore this marker.
   */
  static bool notifyingRunFailure(const ManagedScene &owner) noexcept {
    return owner.notifyingRunFailure_;
  }

  /** @param owner Existing managed scene whose completed run returned a failure.
   * @return Exact receipt that an observer was invoked for that returned first primary failure.
   * No diagnostic equality, allocation, capacity index or mutable collector access is involved.
   */
  static bool runFailureNotified(const ManagedScene &owner) noexcept {
    return owner.runFailureNotified_;
  }

  /** @param owner Closed managed owner; no mutable storage or diagnostic borrow escapes.
   * @return Retained and dropped report counts from the existing bounded history, without allocation.
   */
  static std::pair<std::size_t, std::size_t> reportCounts(const ManagedScene &owner) noexcept {
    return {owner.errorCount_, owner.droppedErrors_};
  }

  /** @param recipe Owning once-only mount action, constructed before entry.
   * @param options Existing runtime and finite facade policies.
   * @param origin Scene construction call site.
   * @return UI-affine lazy owner; storage allocation may throw before preparation.
   */
  static Scene create(std::move_only_function<std::expected<void, ManagedError>(ManagedScene &)> recipe,
                      SceneOptions options, std::source_location origin) {
    return Scene(std::move(recipe), options, origin);
  }
};
} // namespace detail

/** @param owner Borrowed stationary scene, living through this synchronous call.
 * @param stop Optional cancellation.
 * @param origin Final call site.
 * @return Same zero/one status, cleanup and reporting as Scene::runApp.
 * Recipe and argument construction before entry are outside the checked runtime boundaries.
 */
inline int runApp(Scene &owner, std::stop_token stop = {},
                  std::source_location origin = std::source_location::current()) {
  return owner.runApp(stop, origin);
}

/** @param owner Borrowed stationary scene.
 * @param terminal Borrowed custom/replay transport living through this call.
 * @param stop Optional cancellation.
 * @param origin Final call site.
 * @return Same exit status and reporting as Scene::runApp with a borrowed transport.
 */
inline int runApp(Scene &owner, ApplicationTerminal &terminal, std::stop_token stop = {},
                  std::source_location origin = std::source_location::current()) {
  return owner.runApp(terminal, stop, origin);
}
} // namespace portals::ui
