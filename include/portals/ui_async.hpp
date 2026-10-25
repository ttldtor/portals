// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/background.hpp>
#include <portals/ui_scene_pipeline.hpp>

#include <chrono>
#include <tuple>

namespace portals::ui {
class AsyncContext;

namespace detail {
/// @cond INTERNAL
struct AsyncSite {
  ManagedScene *scene{};
  Application *application{};
  Node<Widget> node;
  std::weak_ptr<const void> lifetime;
  std::weak_ptr<SceneAnchor> anchor;
  std::source_location origin;
};

ManagedError asyncFailure(const AsyncSite &site, AsyncError error,
                          std::optional<BackgroundError> background = {}) noexcept;
AsyncError backgroundAsyncError(BackgroundError error) noexcept;

struct AsyncAccess {
  static std::expected<AsyncSite, ManagedError> site(ManagedScene &, Node<Widget>, std::source_location);
  static std::expected<Node<Button>, ManagedError> node(ManagedScene &, const Ref<Button> &);
  static std::expected<void, AsyncError> start(UiAsync &, UiTask &&, UiTaskRestart, portals::detail::AsyncHooks);
  static EditorCommandResult result(const AsyncSite &, std::expected<void, ManagedError>);
  static AsyncTimer rejectedTimer(AsyncError error) noexcept;
};

class AsyncInvocation : public SceneActivity, public std::enable_shared_from_this<AsyncInvocation> {
public:
  AsyncInvocation(AsyncSite site, UiAsyncContext context) : site_(std::move(site)), context_(std::move(context)) {
  }

  ~AsyncInvocation() override;
  bool active() const noexcept override;
  void service() noexcept override;
  void cancel() noexcept override;
  bool live() const noexcept;
  bool entered() const noexcept;
  portals::detail::AsyncHooks hooks();
  AsyncContext context();
  AsyncTimer after(std::chrono::milliseconds delay) const noexcept;
  void submit(std::unique_ptr<BackgroundJob> job, std::function<std::expected<void, AsyncError>(AsyncError)> fail);
  void fail(BackgroundError error, std::function<std::expected<void, AsyncError>(AsyncError)> publish);

private:
  struct OperationFailure {
    AsyncError error;
    std::optional<BackgroundError> background;
  };

  static bool resume(void *, std::coroutine_handle<>) noexcept;
  static void complete(void *, const UiTaskOutcome &) noexcept;
  void fail(AsyncError, std::optional<BackgroundError>) noexcept;
  void publishFailure() noexcept;

  AsyncSite site_;
  UiAsyncContext context_;
  std::unique_ptr<BackgroundLatest> background_;
  std::function<std::expected<void, AsyncError>(AsyncError)> failurePublisher_;
  std::optional<AsyncError> pendingFailure_;
  std::optional<OperationFailure> operationFailure_;
  bool active_{true};
  bool entered_{};
};

/// @endcond
} // namespace detail

/** @brief Weak UI invocation context using the existing UiTask awaiters and scheduler.
 * Copies never retain a scene, coroutine closure, node or background executor. Use owning
 * coroutine parameters/captures; only background's separate callable runs outside UI.
 */
class AsyncContext {
public:
  /** @brief Construct an unavailable invocation context. */
  AsyncContext() = default;

  /** @param deadline Absolute monotonic milliseconds in this invocation's scheduler.
   * @return Existing one-shot timer; even due timers resume only through scheduler dispatch.
   */
  AsyncTimer until(SchedulerTime deadline) const {
    return context_.until(deadline);
  }

  /** @param delay Nonnegative delay in milliseconds, relative to the existing application clock.
   * @return Existing one-shot timer; zero delay still waits for scheduler dispatch. Negative duration
   * or deadline overflow yields INVALID_ARGUMENT; expired context yields CLOSED, foreign use WRONG_THREAD.
   */
  AsyncTimer after(std::chrono::milliseconds delay) const noexcept;

  /** @tparam T Transitively immutable owning result with a worker-safe deleter.
   * @param start Short UI callback receiving only the existing worker-safe reply endpoint.
   * @return Existing bounded data awaiter; provider must retain rejected publication for retry.
   */
  template <class T> AsyncReceive<T> receive(std::function<void(AsyncReply<T>)> start) const {
    return context_.receive<T>(std::move(start));
  }

  /** @tparam Output Transitively immutable result; expected<DomainValue, DomainError> remains domain data.
   * @tparam Input Transitively immutable input with no concurrently mutated aliases.
   * @tparam Work Copyable, nothrow-movable worker-safe callable under makeBackgroundJob's result contract.
   * @param input Non-null owning immutable input; impose application byte limits independently.
   * @param work Separate worker callable; never capture UI refs, this context, frame or invocation closure.
   * @return The existing AsyncReceive, with UI-affine deferred continuation and cooperative cancellation.
   * Full admission retains one latest intent and retries on application cycles without new input.
   * Terminal admission retires that intent and reports ui.async.background once, even if handled.
   * Returning its AsyncError from UiTask separately reports ui.async without a background cause;
   * these are observations at the operation and task boundaries, not distinct underlying failures.
   * Argument construction may throw before entry. No worker is created by this call itself.
   */
  template <class Output, class Input, class Work>
    requires(std::copy_constructible<Work> && std::is_nothrow_move_constructible_v<Work>)
  AsyncReceive<Output> background(std::shared_ptr<const Input> input, Work work) const {
    return context_.receive<Output>(
      [weak = invocation_, input = std::move(input), work = std::move(work)](AsyncReply<Output> reply) mutable {
        auto invocation = weak.lock();

        if (!invocation || !invocation->live()) {
          (void)reply.fail(AsyncError::CLOSED);

          return;
        }

        auto job = makeBackgroundJob<Input, Output>(std::move(input), reply, std::move(work));
        auto fail = [reply](AsyncError error) -> std::expected<void, AsyncError> {
          auto result = reply.fail(error);

          return result ? std::expected<void, AsyncError>{} : std::unexpected(result.error());
        };

        if (!job) {
          invocation->fail(job.error(), std::move(fail));

          return;
        }

        invocation->submit(std::move(*job), std::move(fail));
      });
  }

private:
  friend class detail::AsyncInvocation;

  AsyncContext(UiAsyncContext context, std::weak_ptr<detail::AsyncInvocation> invocation)
      : context_(std::move(context)), invocation_(std::move(invocation)), owner_(std::this_thread::get_id()) {
  }

  UiAsyncContext context_;
  std::weak_ptr<detail::AsyncInvocation> invocation_;
  std::thread::id owner_{};
};

namespace detail {
/// @cond INTERNAL
template <class Function, class... Args> struct AsyncHandler {
  Function function;
  std::tuple<Args...> arguments;
  UiTaskRestart restart{UiTaskRestart::REJECT_RUNNING};
};

template <class Function, class... Args> class OwnedAsyncInvocation final : public AsyncInvocation {
public:
  OwnedAsyncInvocation(AsyncSite site, UiAsyncContext context, const AsyncHandler<Function, Args...> &prototype)
      : AsyncInvocation(std::move(site), std::move(context)), function_(prototype.function),
        arguments_(prototype.arguments) {
  }

  ~OwnedAsyncInvocation() override {
    // Arbitrary capture/argument destructors must not inherit a caller's unrelated UI attribution.
    SetupFailureScope setup({}, nullptr, nullptr);
    CurrentSceneScope mask;

    arguments_.reset();
    function_.reset();
  }

  UiTask task() {
    return std::apply(
      [this](auto &...arguments) {
        return std::invoke(*function_, context(), arguments...);
      },
      *arguments_);
  }

private:
  std::optional<Function> function_;
  std::optional<std::tuple<Args...>> arguments_;
};

template <class Function, class... Args> class AsyncBinding {
public:
  AsyncBinding(AsyncSite site, std::unique_ptr<UiAsync> owner, AsyncHandler<Function, Args...> prototype)
      : site_(std::move(site)), prototype_(std::move(prototype)), owner_(std::move(owner)) {
  }

  EditorCommandResult invoke() {
    try {
      return AsyncAccess::result(site_, invokeChecked());
    } catch (const std::bad_alloc &) {
      return AsyncAccess::result(site_, std::unexpected(asyncFailure(site_, AsyncError::NO_MEMORY)));
    } catch (...) {
      return AsyncAccess::result(site_, std::unexpected(asyncFailure(site_, AsyncError::CALLBACK_FAILED)));
    }
  }

private:
  std::expected<void, ManagedError> invokeChecked() {
    if (starting_) {
      return std::unexpected(asyncFailure(site_, AsyncError::WOULD_BLOCK));
    }

    struct Starting {
      bool &value;

      explicit Starting(bool &flag) : value(flag) {
        value = true;
      }

      ~Starting() {
        value = false;
      }
    } starting(starting_);

    auto outcome = owner_->outcome();

    if (!outcome) {
      return std::unexpected(asyncFailure(site_, outcome.error()));
    }

    if (outcome->status == UiTaskStatus::RUNNING && prototype_.restart == UiTaskRestart::REJECT_RUNNING) {
      return std::unexpected(asyncFailure(site_, AsyncError::WOULD_BLOCK));
    }

    auto invocation = std::make_shared<OwnedAsyncInvocation<Function, Args...>>(site_, owner_->context(), prototype_);
    auto admitted = SceneAccess::admitActivity(*site_.scene, invocation);

    if (!admitted) {
      if (const auto *cause = std::get_if<TreeError>(&admitted.error().cause)) {
        admitted.error().async = portals::detail::asyncError(*cause);
      }

      return admitted;
    }

    // Declaration order also protects an unadopted/rejected task: its frame retires before invocation storage.
    auto task = invocation->task();
    auto started = AsyncAccess::start(*owner_, std::move(task), prototype_.restart, invocation->hooks());

    if (!started && !(invocation->entered() &&
                      (started.error() == AsyncError::CLOSED || started.error() == AsyncError::CANCELLED))) {
      invocation->cancel();

      return std::unexpected(asyncFailure(site_, started.error()));
    }

    return {};
  }

  AsyncSite site_;
  AsyncHandler<Function, Args...> prototype_;
  // Reverse member destruction closes the frame before releasing the callable prototype.
  std::unique_ptr<UiAsync> owner_;
  bool starting_{};
};

template <class Function, class... Args>
auto asyncBinding(ManagedScene &scene, Node<Widget> node, AsyncHandler<Function, Args...> handler,
                  std::source_location origin)
  -> std::expected<std::shared_ptr<AsyncBinding<Function, Args...>>, ManagedError> {
  auto site = AsyncAccess::site(scene, node, origin);

  if (!site) {
    return std::unexpected(site.error());
  }

  if (handler.restart != UiTaskRestart::REJECT_RUNNING && handler.restart != UiTaskRestart::CANCEL_PREVIOUS) {
    return std::unexpected(asyncFailure(*site, AsyncError::INVALID_ARGUMENT));
  }

  try {
    auto owner = UiAsync::create(site->application->scheduler(), site->application->tree(), node);

    if (!owner) {
      return std::unexpected(asyncFailure(*site, owner.error()));
    }

    return std::make_shared<AsyncBinding<Function, Args...>>(*site, std::move(*owner), std::move(handler));
  } catch (const std::bad_alloc &) {
    return std::unexpected(asyncFailure(*site, AsyncError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(asyncFailure(*site, AsyncError::CALLBACK_FAILED));
  }
}

/// @endcond
} // namespace detail

/** @tparam Function Copyable callable returning exactly UiTask from AsyncContext and owning arguments.
 * @tparam Args Copyable decayed owning invocation arguments; explicit references remain caller borrows.
 * @param restart Explicit repeated-action policy; active reentrant resume always rejects.
 * @param function Coroutine function or capturing coroutine lambda, retained separately from its frame.
 * @param args Prototype values copied separately for each invocation.
 * @return Owning descriptor for existing fluent click/key registration, opting the scene into background service.
 * Native run owns the configured pool; a ready host must supply a borrowed executor. Argument construction may throw.
 */
template <class Function, class... Args>
  requires(std::copy_constructible<Function> && (std::copy_constructible<Args> && ...) &&
           std::same_as<std::invoke_result_t<Function &, AsyncContext, Args & ...>, UiTask>)
auto async(UiTaskRestart restart, Function function, Args... args) {
  return detail::AsyncHandler<Function, Args...>{std::move(function), {std::move(args)...}, restart};
}

/** @tparam Function Copyable UiTask callable receiving AsyncContext first.
 * @tparam Args Copyable owning parameters.
 * @param function Retained coroutine callable, including an owning captured lambda.
 * @param args Values copied for each event, retained through complete frame destruction.
 * @return Fluent descriptor with REJECT_RUNNING policy; no coroutine body or worker runs here.
 */
template <class Function, class... Args>
  requires(std::copy_constructible<Function> && (std::copy_constructible<Args> && ...) &&
           std::same_as<std::invoke_result_t<Function &, AsyncContext, Args & ...>, UiTask>)
auto async(Function function, Args... args) {
  return async(UiTaskRestart::REJECT_RUNNING, std::move(function), std::move(args)...);
}

/** @tparam Function Owned coroutine callable. @tparam Args Owned invocation parameters.
 * @param reference Same-scene button observation.
 * @param handler Lazy owning async descriptor; no work runs during registration.
 * @param origin Registration identity retained through resumes and diagnostics.
 * @return Existing bounded managed click stage; failures use the existing setup/runtime collectors.
 */
template <class Function, class... Args>
auto onClick(Ref<Button> reference, detail::AsyncHandler<Function, Args...> handler,
             std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), handler = std::move(handler),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    auto node = detail::AsyncAccess::node(scene, reference);

    if (!node) {
      return std::unexpected(node.error());
    }

    auto binding = detail::asyncBinding(scene, *node, std::move(handler), origin);

    if (!binding) {
      return std::unexpected(binding.error());
    }

    return detail::ClickAccess::attach(
      scene, reference,
      [binding = std::move(*binding)] {
        return binding->invoke();
      },
      origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onClick(owner, registration, origin);
  }};
}

/** @tparam Source Existing shortcut source. @tparam Function Owned coroutine callable.
 * @tparam Args Owned coroutine parameters.
 * @param source Owning alternatives or text copied into the existing parser.
 * @param handler Lazy async descriptor, scoped to the managed scene root.
 * @param behavior Existing PRESS/repeat policy. @param options Existing finite parser budgets.
 * @param origin Original key registration site.
 * @return Existing bounded key stage; no separate key routing or async runtime.
 */
template <portals::detail::ShortcutSource Source, class Function, class... Args>
auto onKey(Source &&source, detail::AsyncHandler<Function, Args...> handler, ShortcutBehavior behavior,
           ShortcutOptions options = {}, std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<Source>(source), options));

  return detail::SceneStage{
    [registration = [shortcut = std::move(shortcut), handler = std::move(handler), behavior, options,
                     origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
      if (!shortcut) {
        ManagedError failure{ManagedPhase::BIND, scene.root().id(), TreeError::INVALID_ARGUMENT, {}, {}};

        failure.shortcut = shortcut.error();

        return std::unexpected(failure);
      }

      auto binding = detail::asyncBinding(scene, scene.root(), std::move(handler), origin);

      if (!binding) {
        return std::unexpected(binding.error());
      }

      return scene.onKey(
        std::move(**shortcut),
        [binding = std::move(*binding)] {
          return binding->invoke();
        },
        behavior, options, origin);
    },
     origin](Scene &owner) mutable {
      detail::SceneAccess::onClick(owner, registration, origin);
    }};
}

/** @tparam Source Existing owning shortcut source. @tparam Function Coroutine callable.
 * @tparam Args Owning invocation parameters.
 * @param source Existing parsed/text shortcut source. @param handler Owning lazy descriptor.
 * @param origin Registration identity. @return PRESS-only existing key stage.
 */
template <portals::detail::ShortcutSource Source, class Function, class... Args>
auto onKey(Source &&source, detail::AsyncHandler<Function, Args...> handler,
           std::source_location origin = std::source_location::current()) {
  return onKey(std::forward<Source>(source), std::move(handler), ShortcutBehavior{}, ShortcutOptions{}, origin);
}

/** @tparam Function Coroutine callable. @tparam Args Owning invocation parameters.
 * @param chord Existing exact semantic key chord. @param handler Owning async descriptor.
 * @param origin Registration identity. @return Existing exact-key stage scoped to the scene root.
 */
template <class Function, class... Args>
auto onKey(KeyEvent chord, detail::AsyncHandler<Function, Args...> handler,
           std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [chord, handler = std::move(handler),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    auto binding = detail::asyncBinding(scene, scene.root(), std::move(handler), origin);

    if (!binding) {
      return std::unexpected(binding.error());
    }

    return scene.onKey(chord, [binding = std::move(*binding)] {
      return binding->invoke();
    });
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onClick(owner, registration, origin);
  }};
}
} // namespace portals::ui
