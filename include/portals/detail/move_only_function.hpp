// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace portals::detail {

/** @brief Internal owning callable fallback for libraries without C++23 move_only_function.
 * @tparam Signature Unqualified function signature R(Args...); cv/ref/noexcept signatures are unsupported.
 */
template <class Signature> class BasicMoveOnlyFunction;

/** @brief Heap-owned, move-only callable invoked as a mutable lvalue.
 * @tparam R Result type, including void.
 * @tparam Args Arguments forwarded according to the function signature.
 * @details Moving transfers ownership and empties the source. Destruction releases the callable and its captures.
 * Construction can throw allocation or callable-construction exceptions; invocation propagates callable exceptions.
 * An empty object must not be invoked. Concurrent access requires external synchronization.
 */
template <class R, class... Args> class BasicMoveOnlyFunction<R(Args...)> {
public:
  /** @brief Construct an empty callable without allocation. */
  BasicMoveOnlyFunction() noexcept = default;

  /** @brief Construct an empty callable.
   * @param empty Null marker; no target is allocated.
   */
  BasicMoveOnlyFunction(std::nullptr_t empty) noexcept {
    (void)empty;
  }

  /** @brief Own a callable by copying or moving its decayed value.
   * @tparam F Callable constructible from the argument and invocable as a mutable lvalue with the declared result.
   * @param callable Target to own; null function or member pointers produce an empty object.
   */
  // Reject noncallable conversion candidates before inspecting their potentially recursive constructors.
  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, BasicMoveOnlyFunction> &&
             std::is_invocable_r_v<R, std::decay_t<F> &, Args...> && std::constructible_from<std::decay_t<F>, F>)
  BasicMoveOnlyFunction(F &&callable) {
    if constexpr (std::is_pointer_v<std::decay_t<F>> || std::is_member_pointer_v<std::decay_t<F>>) {
      if (callable == nullptr) {
        return;
      }
    }

    target_ = std::make_unique<Target<std::decay_t<F>>>(std::forward<F>(callable));
  }

  /** @brief Transfer the target, leaving the source empty.
   * @param other Callable whose target ownership is transferred.
   */
  BasicMoveOnlyFunction(BasicMoveOnlyFunction &&other) noexcept = default;

  /** @brief Release the old target and transfer ownership from another callable.
   * @param other Callable whose target is transferred; empty after a non-self move.
   * @return This callable after transfer.
   */
  BasicMoveOnlyFunction &operator=(BasicMoveOnlyFunction &&other) noexcept = default;

  BasicMoveOnlyFunction(const BasicMoveOnlyFunction &) = delete;
  BasicMoveOnlyFunction &operator=(const BasicMoveOnlyFunction &) = delete;

  /** @brief Release the owned callable and its captures.
   * @param empty Null marker requesting an empty state.
   * @return This callable, now empty.
   */
  BasicMoveOnlyFunction &operator=(std::nullptr_t empty) noexcept {
    (void)empty;
    target_.reset();

    return *this;
  }

  /** @return Whether a callable is owned and can be invoked. */
  explicit operator bool() const noexcept {
    return static_cast<bool>(target_);
  }

  /** @brief Invoke the owned target without consuming it.
   * @param args Arguments forwarded to the target with their declared value categories.
   * @return Target result, converted to R; no value for void.
   * @pre This object is nonempty.
   * @throws Any exception thrown by the callable or conversion of its result.
   */
  R operator()(Args... args) {
    return target_->invoke(std::forward<Args>(args)...);
  }

private:
  struct TargetBase {
    virtual ~TargetBase() = default;
    virtual R invoke(Args... args) = 0;
  };

  template <class F> struct Target final : TargetBase {
    template <class Input> explicit Target(Input &&callable) : callable_(std::forward<Input>(callable)) {
    }

    R invoke(Args... args) override {
      if constexpr (std::is_void_v<R>) {
        std::invoke(callable_, std::forward<Args>(args)...);
      } else {
        return std::invoke(callable_, std::forward<Args>(args)...);
      }
    }

    F callable_;
  };

  std::unique_ptr<TargetBase> target_;
};

/** @brief Preserve the standard callable type when available, otherwise use the internal owning fallback.
 * @tparam Signature Unqualified function signature R(Args...) used by the UI construction and launch layers.
 */
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
template <class Signature> using MoveOnlyFunction = std::move_only_function<Signature>;
#else
template <class Signature> using MoveOnlyFunction = BasicMoveOnlyFunction<Signature>;
#endif

} // namespace portals::detail
