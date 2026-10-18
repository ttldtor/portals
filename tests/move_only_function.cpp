// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/detail/move_only_function.hpp>
#include <portals/ui.hpp>

#ifdef __EMSCRIPTEN__
#  include "../examples/gallery/fluent_form.hpp"
#endif

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
int failures = 0;

static_assert(std::is_nothrow_move_assignable_v<portals::detail::BasicMoveOnlyFunction<int(int)>>);

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <template <class> class Function> void exerciseCallable() {
  static_assert(!std::is_copy_constructible_v<Function<int(int)>>);
  static_assert(!std::is_copy_assignable_v<Function<int(int)>>);
  static_assert(std::is_nothrow_move_constructible_v<Function<int(int)>>);
  static_assert(std::is_move_assignable_v<Function<int(int)>>);

  Function<void()> empty;
  Function<void()> null = nullptr;
  void (*nullPointer)() = nullptr;
  Function<void()> fromNullPointer = nullPointer;

  check(!empty && !null && !fromNullPointer, "default and null targets must be empty");

  Function<int(int)> value = [owned = std::make_unique<int>(7)](int delta) mutable {
    *owned += delta;

    return *owned;
  };

  check(value(3) == 10 && value(2) == 12, "move-only capture retains mutable state and result");

  Function<int(int)> moved = std::move(value);

  check(!value && static_cast<bool>(moved), "move construction transfers target and empties source");
  check(moved(1) == 13, "moved target retains capture state");

  auto previous = std::make_shared<int>(0);
  std::weak_ptr<int> previousObserver = previous;
  Function<int(int)> assigned = [owned = std::move(previous)](int delta) {
    return *owned + delta;
  };

  assigned = std::move(moved);
  check(previousObserver.expired(), "move assignment releases previous captures");
  check(!moved && static_cast<bool>(assigned), "move assignment transfers target and empties source");
  check(assigned(1) == 14, "move-assigned target retains capture state");

  auto lifetime = std::make_shared<int>(0);
  std::weak_ptr<int> observer = lifetime;
  Function<void()> released = [owned = std::move(lifetime)] {
  };

  check(!observer.expired(), "callable owns captured lifetime");
  released = nullptr;
  check(!released && observer.expired(), "null assignment releases target captures");

  int result = 0;
  Function<void(std::unique_ptr<int>, int &)> consume = [&result](std::unique_ptr<int> input, int &increment) {
    result = *input + increment;
    ++increment;

    return 1;
  };
  int increment = 4;

  consume(std::make_unique<int>(6), increment);
  check(result == 10 && increment == 5, "void result discards return and forwards owning/reference arguments");

  Function<int()> throwing = []() -> int {
    throw std::runtime_error("callable failure");
  };
  bool caught = false;

  try {
    (void)throwing();
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "callable failure";
  }

  check(caught && static_cast<bool>(throwing), "call exceptions propagate without clearing target");

  auto destroyed = std::make_shared<int>(0);
  std::weak_ptr<int> destroyedObserver = destroyed;

  {
    Function<void()> scoped = [owned = std::move(destroyed)] {
    };

    check(!destroyedObserver.expired(), "scoped callable owns capture");
  }

  check(destroyedObserver.expired(), "destruction releases target captures");
}

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
static_assert(std::is_same_v<portals::detail::MoveOnlyFunction<void()>, std::move_only_function<void()>>);
static_assert(std::is_same_v<portals::ui::Setup<portals::Widget>,
                             std::move_only_function<std::expected<void, portals::ManagedError>(
                               portals::ManagedScene &, portals::Node<portals::Widget>)>>);
static_assert(
  std::is_same_v<portals::ui::CommandLineParser,
                 std::move_only_function<std::expected<portals::ui::LaunchAction, portals::ui::CommandLineError>(
                   const portals::ui::app::Arguments &)>>);
#else
static_assert(
  std::is_same_v<portals::detail::MoveOnlyFunction<void()>, portals::detail::BasicMoveOnlyFunction<void()>>);
#endif
} // namespace

int main() {
  exerciseCallable<portals::detail::BasicMoveOnlyFunction>();
  exerciseCallable<portals::detail::MoveOnlyFunction>();

#ifdef __EMSCRIPTEN__
  example::Options options;
  auto scene = gallery::form(portals::ui::opts(options));
#endif

  return failures == 0 ? 0 : 1;
}
