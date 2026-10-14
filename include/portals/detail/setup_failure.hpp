// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/current_scene.hpp>
#include <portals/managed_diagnostics.hpp>

namespace portals::ui::detail {

/** @brief Internal synchronous setup/report handshake; never a queued observer. */
enum class SetupFailureEvent {
  LATCH,       ///< Capture the first typed short failure without notifying user code.
  NOTIFY,      ///< Mark the exact primary notification before observer reentry.
  END_ATTEMPT, ///< Revoke the synchronous collector attempt's borrowed identity.
  END_SCOPE    ///< Deliver any pending primary before the setup publication can retire.
};

/** @brief Non-owning setup bridge over a weak stable facade publication. */
using SetupFailureSink = void (*)(void *, SetupFailureEvent, ManagedError *, const void *) noexcept;

/** @brief Stack-local entered-report identity, valid only during one synchronous collector call. */
class SetupFailureTicket {
public:
  SetupFailureTicket() = default;
  SetupFailureTicket(const SetupFailureTicket &) = delete;
  SetupFailureTicket &operator=(const SetupFailureTicket &) = delete;

  /** @brief Mark notification before the observer can reenter; no user callback runs here. */
  void notifying() const noexcept {
    send(SetupFailureEvent::NOTIFY);
  }

  /** @brief Revoke the borrowed attempt identity before its stack storage retires. */
  ~SetupFailureTicket() {
    send(SetupFailureEvent::END_ATTEMPT);
  }

private:
  friend class SetupFailureScope;

  SetupFailureTicket(std::weak_ptr<void> owner, SetupFailureSink sink, const void *attempt) noexcept
      : owner_(std::move(owner)), sink_(sink), attempt_(attempt) {
  }

  void send(SetupFailureEvent event) const noexcept {
    if (const auto owner = owner_.lock(); owner && sink_) {
      sink_(owner.get(), event, nullptr, attempt_);
    }
  }

  std::weak_ptr<void> owner_;
  SetupFailureSink sink_{};
  const void *attempt_{};
};

/** @brief Allocation-free current-thread setup context; nested independent scopes restore their parent. */
class SetupFailureScope {
public:
  /** @param owner Weak stable facade publication, never a borrowed Scene or ManagedScene pointer.
   * @param sink Internal non-throwing latch/delivery bridge, or null to suspend attribution.
   * @param execution Library static-storage execution label.
   */
  SetupFailureScope(std::weak_ptr<void> owner, SetupFailureSink sink, const char *execution) noexcept
      : owner_(std::move(owner)), sink_(sink), execution_(execution), previous_(current_) {
    current_ = this;
  }

  SetupFailureScope(const SetupFailureScope &) = delete;
  SetupFailureScope &operator=(const SetupFailureScope &) = delete;

  ~SetupFailureScope() {
    // Delivery belongs to this completed scope, never to its parent. A newly entered scope still works.
    current_ = nullptr;

    if (const auto owner = owner_.lock(); owner && sink_) {
      sink_(owner.get(), SetupFailureEvent::END_SCOPE, nullptr, nullptr);
    }

    // Restore before the caller retires callback captures or continues ordinary setup statements.
    current_ = previous_;
  }

  /** @param failure Original error already stamped at an admitted collector entry, or an unavailable-target error.
   * @param attempt Borrowed entered-report stack identity, null when no collector was entered.
   * @return Synchronous completion/notification bridge; no retained scene ownership or allocation.
   */
  static SetupFailureTicket latch(ManagedError &failure, const void *attempt = nullptr) noexcept {
    auto *scope = current_;

    if (!scope || !scope->sink_) {
      return {};
    }

    const auto owner = scope->owner_.lock();

    if (!owner) {
      return {};
    }

    failure.execution = scope->execution_;
    failure.setupStopped = true;
    scope->sink_(owner.get(), SetupFailureEvent::LATCH, &failure, attempt);

    return SetupFailureTicket(scope->owner_, scope->sink_, attempt);
  }

private:
  friend class SetupFailurePause;
  CurrentSceneScope currentSceneMask_;
  std::weak_ptr<void> owner_;
  SetupFailureSink sink_{};
  const char *execution_{};
  SetupFailureScope *previous_{};
  static inline thread_local SetupFailureScope *current_{};
};

/** @tparam Callback Existing owning callback type.
 * @param scene Weak facade identity, never an owning Scene or ManagedScene pointer.
 * @param callback Callable transferred under the registration's checked boundary.
 * @return Owning callback; internal transfer and retirement suspend unrelated setup attribution.
 */
template <class Callback> auto currentSceneCallback(std::weak_ptr<SceneAnchor> scene, Callback &&callback) {
  SetupFailureScope mask({}, nullptr, nullptr);

  return CurrentSceneCallback<std::decay_t<Callback>>(std::move(scene), std::forward<Callback>(callback));
}
} // namespace portals::ui::detail
