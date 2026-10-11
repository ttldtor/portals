// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed.hpp>

namespace portals::ui {

/** @brief Policies for one lazy facade over the existing managed runtime. */
struct SceneOptions {
  ApplicationOptions application{};  ///< Existing runner policies, applied once at prepare.
  ManagedOptions managed{};          ///< Existing managed ownership and diagnostic budgets.
  SchedulerClock clock{};            ///< Optional borrowed replay clock; must outlive run.
  std::size_t maxConfigurations{64}; ///< Positive maximum queued configure/onKey callbacks before prepare.
};

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

  void applyTheme(const StyleSheet &sheet, std::source_location origin);
  Scene(Configuration recipe, SceneOptions options, std::source_location origin);
  bool admitConfiguration(bool runtime, std::source_location origin);
  void addConfiguration(Configuration callback, bool runtime, std::source_location origin);
  void configurationException(TreeError cause, std::source_location origin);
  std::expected<ApplicationStats, ManagedError> runImpl(ApplicationTerminal *terminal, std::stop_token stop,
                                                        std::source_location origin);

  std::thread::id owner_;
  std::unique_ptr<State> state_;
};

namespace detail {
/** @brief Internal bridge from consuming recipes to one stable facade owner. */
struct SceneAccess {
  /** @param owner Existing facade receiving its stored pipe theme.
   * @param sheet Owning stage storage borrowed only for the checked copy.
   * @param origin Stage registration call site.
   */
  static void theme(Scene &owner, const StyleSheet &sheet, std::source_location origin) {
    owner.applyTheme(sheet, origin);
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
} // namespace portals::ui
