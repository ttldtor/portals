// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace portals::ui::detail {
struct SceneAnchor;
class SetupFailureScope;

/** @brief Suspend an unrelated setup collector during an independently owned callback. */
class SetupFailurePause {
public:
  /** @param scene Weak identity of the callback owner; the same active setup keeps its attribution. */
  explicit SetupFailurePause(const std::weak_ptr<SceneAnchor> &scene) noexcept;
  /** @brief A linked stack pause cannot be copied. */
  SetupFailurePause(const SetupFailurePause &) = delete;
  /** @brief A linked stack pause cannot be reassigned. */
  SetupFailurePause &operator=(const SetupFailurePause &) = delete;
  /** @brief Restore the setup attribution active before this entered callback. */
  ~SetupFailurePause();

private:
  SetupFailureScope *previous_{};
};

/** @param storage Borrowed callback storage, valid only for this synchronous call.
 * @param reset Non-throwing destruction bridge; both setup and current-scene attribution are suspended.
 */
void retireSceneCallback(void *storage, void (*reset)(void *) noexcept) noexcept;

/** @brief Allocation-free UI execution publication; an empty scope suspends the caller's scene. */
class CurrentSceneScope {
public:
  /** @param scene Weak facade executing this synchronous boundary, or empty to mask attribution. */
  explicit CurrentSceneScope(std::weak_ptr<SceneAnchor> scene = {}) noexcept
      : scene_(std::move(scene)), previous_(current_) {
    current_ = this;
  }

  /** @brief A linked stack publication cannot be copied. */
  CurrentSceneScope(const CurrentSceneScope &) = delete;
  /** @brief A linked stack publication cannot be reassigned. */
  CurrentSceneScope &operator=(const CurrentSceneScope &) = delete;

  /** @brief Restore the previous publication before returning to the caller. */
  ~CurrentSceneScope() {
    current_ = previous_;
  }

  /** @return One weak snapshot of this thread's entered facade; never a previous completed scene. */
  static std::weak_ptr<SceneAnchor> snapshot() noexcept {
    return current_ ? current_->scene_ : std::weak_ptr<SceneAnchor>{};
  }

private:
  std::weak_ptr<SceneAnchor> scene_;
  CurrentSceneScope *previous_{};
  static inline thread_local CurrentSceneScope *current_{};
};

/** @brief Facade-aware callback storage; invocation publishes weak ownership, retirement masks its caller.
 * @tparam Callback Existing owning callable; construction/moves remain within the registration boundary.
 */
template <class Callback> class CurrentSceneCallback {
public:
  /** @param scene Weak facade, empty for a standalone low-level managed owner.
   * @param callback Callable transferred into this owning storage.
   */
  CurrentSceneCallback(std::weak_ptr<SceneAnchor> scene, Callback &&callback)
      : scene_(std::move(scene)), callback_(std::in_place, std::move(callback)) {
  }

  /** @brief Copy the weak publication and the original owning callable. */
  CurrentSceneCallback(const CurrentSceneCallback &) = default;

  /** @param other Owning callback transferred under the checked registration boundary. */
  CurrentSceneCallback(CurrentSceneCallback &&other)
      : scene_(std::move(other.scene_)), callback_(std::move(other.callback_)) {
  }

  /** @brief Retire owned captures while both setup and current-scene attribution are suspended. */
  ~CurrentSceneCallback() {
    retireSceneCallback(&callback_, [](void *storage) noexcept {
      static_cast<std::optional<Callback> *>(storage)->reset();
    });
  }

  /** @return False only for empty storage or an existing nullable callable with no target. */
  explicit operator bool() const {
    if (!callback_) {
      return false;
    }

    if constexpr (requires { static_cast<bool>(*callback_); }) {
      return static_cast<bool>(*callback_);
    } else {
      return true;
    }
  }

  /** @tparam Args Original borrowed event arguments.
   * @param args Forwarded only for this invocation.
   * @return The original callback result without changing its checked contract.
   */
  template <class... Args>
    requires std::invocable<Callback &, Args...>
  decltype(auto) operator()(Args &&...args) const {
    SetupFailurePause setup(scene_);
    CurrentSceneScope scope(scene_);

    return std::invoke(*callback_, std::forward<Args>(args)...);
  }

private:
  std::weak_ptr<SceneAnchor> scene_;
  mutable std::optional<Callback> callback_;
};

} // namespace portals::ui::detail
