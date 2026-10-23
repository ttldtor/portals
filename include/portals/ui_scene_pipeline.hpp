// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_scene_recipes.hpp>

namespace portals::ui {

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
  RunAppOptions options{};       ///< Upper factory policies; callable construction precedes consumption.

  /** @param owner Borrowed lvalue scene or live temporary; it is never moved by this stage.
   * @param stage Transport/cancellation selection.
   * @return Same zero/one status and final diagnostic delivery as Scene::runApp.
   */
  friend int operator|(Scene &owner, RunAppStage stage) {
    if (stage.options.report) {
      owner.reportOnError(std::move(stage.options.report), stage.origin);
    }

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
  RunAppOptions options{};         ///< Pre-factory policies; returned Scene policies remain its own.

  /** @param owner Existing UI-affine lazy scene.
   * @param stage Single-use command-line stage.
   * @return Zero for success/help, two for launch errors, one for runtime failure.
   */
  friend int operator|(Scene &owner, CommandLineRunStage stage) {
    if (stage.options.report) {
      owner.reportOnError(std::move(stage.options.report), stage.origin);
    }

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
  return detail::SceneStage{[chord, declaration = detail::retainAction(callback), callback = std::move(callback), origin](Scene &owner) mutable {
      (void)declaration;
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
 * @param info Owning metadata resolved once after complete mount.
 * @param callback Action stored until checked stage
 * consumption; caller construction/moves may throw.
 * @param origin Registration call site, independent of helper
 * construction.
 * @return Consuming stage using the existing bounded newest-first registration.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeySelection selection, KeyInfo info, Callback callback,
           std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[selection = std::move(selection), info = std::move(info), declaration = detail::retainAction(callback), callback = std::move(callback),
                             origin](Scene &owner) mutable {
      (void)declaration;
    owner.onKey(std::move(selection), std::move(info), std::move(callback), origin);
  }};
}

/** @tparam Source Existing shortcut source. @tparam Callback Exact managed action.
 * @param source Owning normalization input. @param info Metadata registered once with the action.
 * @param callback Action. @param origin Original registration site. @return Consuming annotated stage. */
template <portals::detail::ShortcutSource Source, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(Source &&source, KeyInfo info, Callback callback,
           std::source_location origin = std::source_location::current()) {
  return onKey(press(std::forward<Source>(source)), std::move(info), std::move(callback), origin);
}

/** @tparam Callback Existing compact key action preserving old callback result forms.
 * @param selection Owning alternatives and exact native phases.
 * @param callback Owning action. @param origin Original registration site.
 * @return Consuming unannotated stage with existing routing behavior. */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeySelection selection, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{
    [selection = std::move(selection), declaration = detail::retainAction(callback), callback = std::move(callback), origin](Scene &owner) mutable {
      (void)declaration;
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
    [shortcut = std::move(shortcut), declaration = detail::retainAction(callback), callback = std::move(callback), origin](Scene &owner) mutable {
      (void)declaration;
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
    [shortcut = std::move(shortcut), declaration = detail::retainAction(callback), callback = std::move(callback), behavior, options, origin](Scene &owner) mutable {
      (void)declaration;
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
    [shortcut = std::move(shortcut), declaration = detail::retainAction(callback), callback = std::move(callback), origin](Scene &owner) mutable {
      (void)declaration;
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
    [shortcut = std::move(shortcut), declaration = detail::retainAction(callback), callback = std::move(callback), behavior, options, origin](Scene &owner) mutable {
      (void)declaration;
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

/** @tparam T CheckBox or its ThreeState derived view.
 * @tparam Callback Copyable action taking the actual bool/CheckState value or its const PropertyUpdate reference.
 * @param reference Published checkbox whose existing binding supplies activation changes.
 * @param callback FIFO post-commit notification; errors cannot veto the committed model value.
 * @param origin Registration site retained in diagnostics.
 * @return Scene stage with checked affinity, liveness, model-type and bounded-storage admission.
 * Value invocation wins over receipt invocation. Generic value bodies must compile for both bool and CheckState;
 * invalid generic bodies can produce hard compiler errors. Use a concrete parameter for a single model type.
 * Failed/no-op updates and external model writes do not notify; rejected observer publication still commits.
 * Legacy handlers run first. Appending during either legacy or late delivery affects the next activation.
 * Removal stops remaining delivery; captures and exact receipt snapshots survive the executing callback.
 */
template <class T, class Callback>
  requires(std::derived_from<T, CheckBox> && std::copy_constructible<Callback> &&
           (portals::detail::ChangeAction<Callback, bool> || portals::detail::ChangeAction<Callback, CheckState>))
auto onChange(Ref<T> reference, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::ChangeAccess::attach(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onEditing(owner, registration, origin);
  }};
}

/** @tparam Callback Owning managed action taking TextEditedEvent or no arguments.
 * @param reference Editor published by this scene's recipe.
 * @param callback FIFO callback for actual accepted user byte changes, including Undo/Redo and clear.
 * @param origin Registration site retained through deferred mounting and input diagnostics.
 * @return Pipe stage over the existing editor binding, with no extra subscription or manual token.
 * Cursor/selection changes, fluent writes, model synchronization and failed edits without a delta do not notify.
 * The legacy result runs first; node/scene removal suppresses subsequent typed delivery. Callback errors stop
 * this event's remaining chain and use existing diagnostics. Appending during delivery affects the next event.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, TextEditedEvent>
auto onTextEdited(Ref<LineEdit> reference, Callback callback,
                  std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::EditorEventAccess::attach<TextEditedEvent>(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onEditing(owner, registration, origin);
  }};
}

/** @tparam Callback Owning managed action taking EditingFinishedEvent or no arguments.
 * @param reference Editor published by this scene's recipe.
 * @param callback FIFO observation of Enter or focus loss in every edit mode, including rejected attempts.
 * @param origin Registration site retained through deferred mounting and input diagnostics.
 * @return Pipe stage reusing the existing commit result; no second validator or model update runs.
 * Deferred/clean blur has no result receipt. Events own their values; the callback borrows them synchronously.
 * Legacy-result removal suppresses delivery; reentrant focus from that attempt does not duplicate completion.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, EditingFinishedEvent>
auto onEditingFinished(Ref<LineEdit> reference, Callback callback,
                       std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::EditorEventAccess::attach<EditingFinishedEvent>(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onEditing(owner, registration, origin);
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

namespace detail {
/** @param sink Optional upper callable copied before any user factory/parser executes.
 * @return Pinned owning sink, empty for default delivery, or exact allocation/copy failure.
 */
inline std::expected<std::shared_ptr<SceneReportHandler>, ManagedError> pinAppSink(const SceneReportHandler &sink) {
  try {
    return sink ? std::make_shared<SceneReportHandler>(sink) : nullptr;
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED, {}, {}});
  }
}

/** @tparam Factory Owning-result factory admitted by the public upper boundary.
 * @tparam Setup Stationary setup callback, returning void or exact managed expected.
 * @param factory Synchronously invoked factory.
 * @param setup Callback running against one stationary facade.
 * @param options Borrowed Description construction and runner policies.
 * @param finalSink Optional owning upper sink, already pinned before user code executes.
 * @param origin Upper invocation site.
 * @return Existing zero/one runtime status with final diagnostics after cleanup.
 */
template <AppFactory Factory, AppSetup Setup>
int runAppFactory(Factory &&factory, Setup &&setup, const RunAppOptions &options,
                  std::shared_ptr<SceneReportHandler> finalSink, std::source_location origin) {
  FactoryCleanupScope factoryMask;
  SetupFailureScope invocationScope({}, nullptr, nullptr);
  CurrentSceneScope currentMask;
  SceneReportHandler fallbackSink;
  FactoryCleanup cleanup;

  try {
    auto description = [&] {
      FactoryCleanupScope factoryScope(&cleanup);

      return std::invoke(factory);
    }();

    using Result = std::remove_cvref_t<decltype(description)>;
    constexpr bool DIRECT_RESULT = IsDescription<Result>::value || std::same_as<Result, Scene>;

    auto create = [&]() {
      if constexpr (std::same_as<Result, Scene>) {
        return std::move(description);
      } else if constexpr (std::same_as<Result, std::expected<Scene, ManagedError>>) {
        return std::move(*description);
      } else if constexpr (IsDescription<Result>::value) {
        return scene(std::move(description), options.scene, origin);
      } else {
        return scene(std::move(*description), options.scene, origin);
      }
    };

    if constexpr (!DIRECT_RESULT) {
      if (!description) {
        reportFactoryFailure(finalSink ? *finalSink : fallbackSink, description.error(), origin, cleanup);

        return 1;
      }
    }

    // This owner stays stationary during setup and run; callbacks observe stable State through SceneRef.
    auto owner = create();
    auto admitted = SceneAccess::factoryResultChecked(owner);

    if (!admitted) {
      auto failure = admitted.error();
      const auto closed = owner.close(origin);

      if (!closed && !failure.cleanup && closed.error().cleanup) {
        failure.cleanup = closed.error().cleanup;
      }

      const auto report = factoryFailureReport(std::move(failure), origin, cleanup);

      if (!SceneAccess::deliverFactoryReport(owner, report, finalSink)) {
        deliverFactoryReport(finalSink ? *finalSink : fallbackSink, report);
      }

      return 1;
    }

    const auto droppedErrors = SceneAccess::applyFactoryCleanup(owner, cleanup, origin);

    if (owner.checked()) {
      std::optional<ManagedError> setupFailure;

      {
        auto setupScope = SceneAccess::setupScope(owner);
        auto currentScope = SceneAccess::currentScope(owner);

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
        SceneAccess::setupFailure(owner, *setupFailure, origin);
      }
    }

    return SceneAccess::runApp(owner, options, std::move(finalSink), origin, droppedErrors);
  } catch (const std::bad_alloc &) {
    reportFactoryFailure(finalSink ? *finalSink : fallbackSink,
                         ManagedError{ManagedPhase::MOUNT, {}, TreeError::NO_MEMORY, {}, {}}, origin, cleanup);
  } catch (...) {
    reportFactoryFailure(finalSink ? *finalSink : fallbackSink,
                         ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED, {}, {}}, origin, cleanup);
  }

  return 1;
}
} // namespace detail

/** @tparam Factory No-argument callable returning owning Scene, Description<T>, or their exact expected with
 * ManagedError.
 * @tparam Setup Callable taking the stationary Scene&, returning void or exact expected<void,ManagedError>.
 * @param factory Invoked inside the checked construction boundary, after final sink availability.
 * @param setup Invoked on one stationary facade; capture SceneRef for later callbacks across facade moves.
 * @param options Description construction policies, borrowed terminal, upper final sink and cancellation.
 * A returned Scene retains its own policies and final sink unless options.report is nonempty.
 * The explicit sink is pinned before invoking the factory; if pinning fails, stderr receives the typed failure.
 * @param origin Upper runner invocation site retained in typed failures.
 * @return Zero after successful run, one for factory/construction/setup/run failure after cleanup.
 * Callable and argument construction before actual invocation can throw outside this boundary.
 * Factory/setup exceptions map to NO_MEMORY or CALLBACK_FAILED. A factory returning Scene must retain
 * callback data by ownership or SceneRef; references to its local facade do not survive the return move.
 * Cold and prepared, unrun scenes are admitted; completed, closed and moved-from results fail before terminal access.
 * Abandoned local cleanup retains its first typed error; further failures increment report.droppedErrors.
 * A rejected short mutation inside setup latches its original typed cause before observers; the current void
 * callback finishes, while later setup/run is skipped. Unbound targets can be attributed to this entered scope.
 */
template <detail::AppFactory Factory, detail::AppSetup Setup>
int runApp(Factory &&factory, Setup &&setup, const RunAppOptions &options = {},
           std::source_location origin = std::source_location::current()) {
  detail::FactoryCleanupScope factoryMask;
  detail::SetupFailureScope invocationScope({}, nullptr, nullptr);
  detail::CurrentSceneScope currentMask;
  auto sink = detail::pinAppSink(options.report);

  if (!sink) {
    detail::reportFactoryFailure({}, sink.error(), origin);

    return 1;
  }

  return detail::runAppFactory(std::forward<Factory>(factory), std::forward<Setup>(setup), options, std::move(*sink),
                               origin);
}

/** @tparam Factory No-argument factory returning owning Scene, Description<T>, or their exact managed expected.
 * @param factory Invoked inside the checked upper construction boundary.
 * @param options Existing runtime policies and final sink, available before factory invocation.
 * @param origin Upper invocation site.
 * @return Same zero/one status and cleanup as the stationary setup overload with no setup action.
 */
template <detail::AppFactory Factory>
int runApp(Factory &&factory, const RunAppOptions &options = {},
           std::source_location origin = std::source_location::current()) {
  return runApp(
    std::forward<Factory>(factory),
    [](Scene &) {
    },
    options, origin);
}

/** @tparam Factory No-argument owning Scene/Description factory, optionally returning exact managed expected.
 * @param factory Invoked once only after successful command-line preflight; captures must outlive this call.
 * @param argc Input argument count, including the program name.
 * @param argv Borrowed argument array, copied before parser invocation.
 * @param parser Owning parser, retired before factory invocation, including on help/failure.
 * @param options Description policies, optional explicit final sink, borrowed terminal and cancellation.
 * A returned Scene keeps its policies and configured sink unless the upper sink is nonempty.
 * @param origin Upper launch call site retained in diagnostics.
 * @return Zero on success/help, two on command-line failure, one on factory/runtime failure after cleanup.
 * Callable/argument construction before invocation remains outside this checked boundary.
 */
template <detail::AppFactory Factory>
int runApp(Factory &&factory, int argc, const char *const *argv, CommandLineParser parser,
           const RunAppOptions &options = {}, std::source_location origin = std::source_location::current()) {
  detail::FactoryCleanupScope factoryMask;
  detail::SetupFailureScope invocationScope({}, nullptr, nullptr);
  detail::CurrentSceneScope currentMask;
  auto sink = detail::pinAppSink(options.report);

  if (!sink) {
    parser = nullptr;
    detail::reportFactoryFailure({}, sink.error(), origin);

    return 1;
  }

  auto action = detail::commandLinePreflight(argc, argv, parser);

  if (!action) {
    SceneReport report;
    SceneReportHandler fallbackSink;

    report.origin = origin;
    report.commandLineFailure = std::move(action.error());
    detail::deliverFactoryReport(*sink ? **sink : fallbackSink, report);

    return 2;
  }

  if (action->disposition == LaunchDisposition::FINISH) {
    if (!action->output.empty()) {
      std::fwrite(action->output.data(), 1, action->output.size(), stdout);
    }

    return 0;
  }

  return detail::runAppFactory(
    std::forward<Factory>(factory),
    [](Scene &) {
    },
    options, std::move(*sink), origin);
}

namespace detail {
/** @tparam Factory Owning-result factory borrowed through synchronous stage consumption.
 * @param factory Callable evaluated inside the existing checked upper boundary.
 * @param stage Upper policies, final sink and optional borrowed transport.
 * @return Same zero/one result as the function form.
 */
template <AppFactory Factory> int operator|(Factory &&factory, RunAppStage stage) {
  stage.options.terminal = stage.terminal;
  stage.options.stop = stage.stop;

  return ui::runApp(std::forward<Factory>(factory), stage.options, stage.origin);
}

/** @tparam Factory Owning-result factory borrowed through synchronous command-line launch.
 * @param factory Callable evaluated only after successful preflight.
 * @param stage Owning parser, upper sink and optional borrowed transport.
 * @return Same zero/two/one result as the command-line function form.
 */
template <AppFactory Factory> int operator|(Factory &&factory, CommandLineRunStage stage) {
  stage.options.terminal = stage.terminal;
  stage.options.stop = stage.stop;

  return ui::runApp(std::forward<Factory>(factory), stage.argc, stage.argv, std::move(stage.parser), stage.options,
                    stage.origin);
}
} // namespace detail

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

/** @tparam AppOptions Exact RunAppOptions type; deduction keeps legacy braced stop-token calls unambiguous.
 * @param options Upper factory policies and explicit final sink, copied into the consuming stage.
 * The optional terminal remains borrowed through synchronous consumption; Scene policies remain its own.
 * @param origin Final stage registration site.
 * @return Checked final stage accepting an owning-result factory or an existing Scene.
 */
template <class AppOptions>
  requires std::same_as<std::remove_cvref_t<AppOptions>, RunAppOptions>
inline auto runApp(AppOptions &&options, std::source_location origin = std::source_location::current()) {
  return detail::RunAppStage{options.terminal, options.stop, origin, options};
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

/** @tparam AppOptions Exact RunAppOptions type; braced legacy origin arguments do not deduce this overload.
 * @param argc Input argument count, including the program name.
 * @param argv Borrowed input array copied at consumption.
 * @param parser Owning parser retired before invoking a factory or opening a terminal.
 * @param options Upper factory policies, explicit final sink, borrowed terminal and cancellation.
 * @param origin Final launch stage registration site.
 * @return Command-line stage with the same zero/two/one result as the function form.
 */
template <class AppOptions>
  requires std::same_as<std::remove_cvref_t<AppOptions>, RunAppOptions>
inline auto runApp(int argc, const char *const *argv, CommandLineParser parser, AppOptions &&options,
                   std::source_location origin = std::source_location::current()) {
  return detail::CommandLineRunStage{argc, argv, std::move(parser), options.terminal, options.stop, origin, options};
}

} // namespace portals::ui
