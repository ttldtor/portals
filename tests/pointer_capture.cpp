// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/action_state.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local int allocationFailureAfter{-1};
thread_local void (*allocationHook)(){};
} // namespace

void *operator new(std::size_t size) {
  if (auto hook = allocationHook) {
    allocationHook = nullptr;
    hook();
  }

  if (allocationFailureAfter == 0) {
    allocationFailureAfter = -1;

    throw std::bad_alloc();
  }

  if (allocationFailureAfter > 0) {
    --allocationFailureAfter;
  }

  if (auto *storage = std::malloc(size == 0 ? 1 : size)) {
    return storage;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *storage) noexcept {
  std::free(storage);
}

void operator delete[](void *storage) noexcept {
  std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept {
  std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept {
  std::free(storage);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

namespace {
using namespace portals;
int failures{};

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    std::cerr << "unexpected fixture error " << static_cast<int>(result.error()) << '\n';
    std::abort();
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

template <class T> bool error(const std::expected<T, TreeError> &result, TreeError expected) {
  return !result && result.error() == expected;
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  Node<Widget> parent = take(tree->emplace<Widget>(root));
  Node<Widget> owner = take(tree->emplace<Widget>(parent));
  Node<Widget> other = take(tree->emplace<Widget>(root));
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  PointerCapture capture;

  Fixture() {
    take(router->setPointerCaptureCapabilities({true, true, true}));
    layout();
  }

  void arrange(Node<Widget> node, CellPoint origin, CellSize size) {
    auto rectangle = CellRect::from(origin, size);
    auto viewport = CellRect::from({}, {20, 8});

    check(rectangle.has_value() && viewport.has_value(), "fixture rectangles are valid");
    take(tree->arrange(node, *rectangle, *viewport));
  }

  void layout() {
    arrange(root, {}, {20, 8});
    arrange(parent, {}, {8, 8});
    arrange(owner, {1, 1}, {4, 2});
    arrange(other, {10, 1}, {5, 2});
  }

  DispatchResult mouse(MouseAction action, MouseButton button = MouseButton::LEFT, int x = 1, int y = 1) {
    return take(router->dispatch(MouseInputEvent{x, y, button, action, {}}));
  }

  void acquire() {
    capture = take(router->capturePointer(owner));
  }
};

thread_local Fixture *allocationTarget{};

void hideOwnerDuringAllocation() {
  take(allocationTarget->tree->setVisible(allocationTarget->owner, false));
}

void routingAndGeometry() {
  Fixture f;
  int moves{}, releases{}, outside{}, ancestors{};
  auto parent = take(f.router->bind(
    f.parent,
    [&](Widget &, const RoutedEvent &) {
      ++ancestors;

      return EventReply::IGNORED;
    },
    false));
  auto sibling = take(f.router->bind(f.other, [&](Widget &, const RoutedEvent &) {
    ++outside;

    return EventReply::HANDLED;
  }));
  auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
    const auto *mouse = std::get_if<MouseInputEvent>(&event);

    if (!mouse) {
      return EventReply::IGNORED;
    }

    if (mouse->action == MouseAction::PRESS) {
      f.acquire();

      return EventReply::HANDLED;
    }

    if (mouse->action == MouseAction::MOVE) {
      ++moves;
      take(f.tree->invalidate(f.owner, Invalidation::LAYOUT));
    }

    if (mouse->action == MouseAction::RELEASE) {
      ++releases;
      check(!f.capture.connected() && take(f.router->pointerCapture()) == Node<Widget>{},
            "release unhooks capture before the callback");
      check(error(f.router->capturePointer(f.owner), TreeError::WOULD_BLOCK), "release cannot reacquire capture");
    }

    return EventReply::IGNORED;
  }));

  f.mouse(MouseAction::PRESS);

  check(f.capture.connected() && take(f.router->pointerCapture()) == f.owner, "press acquires its own registration");

  take(f.tree->consumeFrameRequest());
  take(f.router->pointerCapture());

  check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "idle capture query requests no redraw");

  f.mouse(MouseAction::SCROLL, MouseButton::WHEEL_DOWN, 11, 1);

  check(outside == 1 && f.capture.connected(), "wheel still uses normal hit testing outside the capture owner");

  const auto first = f.mouse(MouseAction::MOVE, MouseButton::LEFT, 200, -40);
  const auto second = f.mouse(MouseAction::MOVE, MouseButton::LEFT, 300, 100);

  check(moves == 2 && first.target == f.owner.id() && second.target == f.owner.id() &&
          first.status == DispatchStatus::UNHANDLED && ancestors == 0,
        "outside moves remain exclusive through LAYOUT and ignored replies never bubble");

  f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 300, 100);

  check(releases == 1 && !f.capture.connected(), "one outside release terminates the sequence");

  f.layout();
  f.mouse(MouseAction::MOVE, MouseButton::NO_BUTTON, 11, 1);
  f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 300, 100);

  check(outside == 2 && releases == 1, "default routing resumes without duplicate captured release");
}

void capabilities() {
  Fixture f;
  int moves{};
  TreeError rejected{TreeError::CLOSED};
  auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
    const auto *mouse = std::get_if<MouseInputEvent>(&event);

    if (mouse && mouse->action == MouseAction::PRESS) {
      auto acquired = f.router->capturePointer(f.owner);

      if (acquired) {
        f.capture = std::move(*acquired);
      } else {
        rejected = acquired.error();
      }
    } else if (mouse && mouse->action == MouseAction::MOVE) {
      ++moves;
    }

    return EventReply::HANDLED;
  }));

  take(f.router->setPointerCaptureCapabilities({}));
  f.mouse(MouseAction::PRESS);

  check(rejected == TreeError::INVALID_ARGUMENT && !f.capture.connected(), "default absent capability denies drag");

  take(f.router->setPointerCaptureCapabilities({true, false, true}));
  f.mouse(MouseAction::PRESS);

  check(rejected == TreeError::INVALID_ARGUMENT, "motion alone cannot promise matching release");

  take(f.router->setPointerCaptureCapabilities({true, true, false}));
  f.mouse(MouseAction::PRESS);
  f.mouse(MouseAction::MOVE, MouseButton::NO_BUTTON, 100, 100);
  f.mouse(MouseAction::MOVE, MouseButton::RIGHT, 100, 100);

  check(moves == 2 && f.capture.connected(), "unknown button identity is not proof that LEFT was released");

  f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 100, 100);
  take(f.router->setPointerCaptureCapabilities({true, true, true}));
  f.mouse(MouseAction::PRESS);
  f.mouse(MouseAction::MOVE, MouseButton::NO_BUTTON, 100, 100);

  check(moves == 2 && !f.capture.connected(), "reliable no-button motion cancels without delivery");

  f.mouse(MouseAction::PRESS);
  f.mouse(MouseAction::MOVE, MouseButton::RIGHT, 100, 100);

  check(moves == 2 && !f.capture.connected(), "reliable different-button motion cancels without delivery");

  f.mouse(MouseAction::PRESS);
  take(f.router->setPointerCaptureCapabilities({}));

  check(!f.capture.connected() && take(f.router->pointerCapture()) == Node<Widget>{}, "capability revocation cancels");
}

void focusAndHostCancellation() {
  Fixture f;
  int moves{};
  auto outside = take(f.router->bind(f.other, [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  }));
  auto binding = take(f.router->bind(
    f.owner,
    [&](Widget &, const RoutedEvent &event) {
      const auto *mouse = std::get_if<MouseInputEvent>(&event);

      if (mouse && mouse->action == MouseAction::PRESS) {
        check(error(f.router->capturePointer(f.other), TreeError::INVALID_ARGUMENT),
              "handler cannot capture another live registration");
        check(error(f.router->capturePointer(f.owner, f.other), TreeError::INVALID_ARGUMENT),
              "focus root must contain the owner");
        check(error(f.router->setPointerCaptureCapabilities({}), TreeError::WOULD_BLOCK),
              "capability mutation cannot reenter input delivery");
        f.acquire();
      } else if (mouse && mouse->action == MouseAction::MOVE) {
        ++moves;
      }

      return EventReply::HANDLED;
    },
    false));

  take(f.router->setFocus(f.other));
  f.mouse(MouseAction::PRESS);
  f.mouse(MouseAction::MOVE, MouseButton::LEFT, 90, 90);
  take(f.router->setFocus(f.other));

  check(f.capture.connected() && moves == 1, "unchanged external focus does not cancel a non-focusable owner");

  auto child = take(f.tree->emplace<Widget>(f.owner));
  auto childBinding = take(f.router->bind(child, [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  }));

  take(f.router->setFocus(child));

  check(f.capture.connected(), "actual focus transition into owner subtree retains capture");

  take(f.router->setFocus(f.other));

  check(!f.capture.connected(), "actual focus transition outside owner subtree cancels");

  f.layout();
  f.arrange(child, {}, {});
  f.mouse(MouseAction::PRESS);
  take(f.tree->consumeFrameRequest());
  take(f.router->cancelPointerCapture(false));

  check(!f.capture.connected() && take(f.tree->consumeFrameRequest()) == Invalidation::NONE,
        "host stop/cleanup cancellation does not request a new frame");

  f.mouse(MouseAction::PRESS);
  take(f.router->cancelPointerCapture());

  check(!f.capture.connected(), "host resize or policy cancellation unhooks capture");

  f.mouse(MouseAction::PRESS);
  take(f.router->dispatch(KeyEvent{.key = InputKey::ESCAPE}));

  check(!f.capture.connected(), "Escape cancels before ordinary keyboard routing");

  Fixture empty;
  auto nonfocusable = take(empty.router->bind(
    empty.owner,
    [&](Widget &, const RoutedEvent &event) {
      if (const auto *mouse = std::get_if<MouseInputEvent>(&event); mouse && mouse->action == MouseAction::PRESS) {
        empty.acquire();
      }

      return EventReply::HANDLED;
    },
    false));

  empty.mouse(MouseAction::PRESS);
  empty.mouse(MouseAction::MOVE, MouseButton::LEFT, 90, 90);

  check(empty.capture.connected(), "pre-existing empty focus is legal throughout capture");
}

void lifecycle() {
  for (int mutation = 0; mutation < 6; ++mutation) {
    Fixture f;
    int moves{};
    auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
      if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
        if (mouse->action == MouseAction::PRESS) {
          f.acquire();
        } else if (mouse->action == MouseAction::MOVE) {
          ++moves;
        }
      }

      return EventReply::HANDLED;
    }));

    f.mouse(MouseAction::PRESS);

    const auto target = mutation % 2 == 0 ? f.owner : f.parent;

    if (mutation < 2) {
      take(f.tree->setVisible(target, false));
    } else if (mutation < 4) {
      take(f.tree->setEnabled(target, false));
    } else {
      take(f.tree->remove(target));
    }

    check(take(f.router->pointerCapture()) == Node<Widget>{} && !f.capture.connected(),
          "hide/disable/remove owner or ancestor cancels before another admission");

    f.mouse(MouseAction::MOVE, MouseButton::LEFT, 100, 100);

    check(moves == 0, "retired sequence receives no continuation");
  }

  Fixture f;
  int moves{};
  auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
    if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
      if (mouse->action == MouseAction::PRESS) {
        f.acquire();
      } else if (mouse->action == MouseAction::MOVE) {
        ++moves;
      }
    }

    return EventReply::HANDLED;
  }));

  f.mouse(MouseAction::PRESS);

  std::thread disconnect([&] {
    binding.disconnect();
  });

  disconnect.join();

  check(!f.capture.connected(), "worker action disconnect immediately closes its capture admission flag");

  auto replacement = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &) {
    ++moves;

    return EventReply::HANDLED;
  }));

  f.mouse(MouseAction::MOVE, MouseButton::LEFT, 100, 100);

  check(moves == 0 && take(f.router->pointerCapture()) == Node<Widget>{},
        "replacement registration cannot inherit old capture at the same NodeId");
}

void conservativeEligibilityHistory() {
  for (int mutation = 0; mutation < 6; ++mutation) {
    Fixture f;
    auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
      if (const auto *mouse = std::get_if<MouseInputEvent>(&event); mouse && mouse->action == MouseAction::PRESS) {
        f.acquire();
      }

      return EventReply::HANDLED;
    }));

    f.mouse(MouseAction::PRESS);

    if (mutation < 2) {
      const auto node = mutation == 0 ? f.owner : f.parent;

      take(f.tree->setVisible(node, false));
      take(f.tree->setVisible(node, true));
    } else if (mutation < 4) {
      const auto node = mutation == 2 ? f.owner : f.parent;

      take(f.tree->setEnabled(node, false));
      take(f.tree->setEnabled(node, true));
    } else if (mutation == 4) {
      take(f.tree->setVisible(f.other, false));
      take(f.tree->setVisible(f.other, true));
    } else {
      take(f.tree->remove(f.other));
    }

    check(take(f.tree->inputEligible(f.owner)), "fixture owner is eligible again before router collection");
    check(take(f.router->pointerCapture()) == Node<Widget>{} && !f.capture.connected(),
          "rapid toggles and unrelated eligibility/removal retire the old conservative stamp");
  }

  Fixture f;
  auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
    if (const auto *mouse = std::get_if<MouseInputEvent>(&event); mouse && mouse->action == MouseAction::PRESS) {
      f.acquire();
    }

    return EventReply::HANDLED;
  }));

  f.mouse(MouseAction::PRESS);
  take(f.tree->invalidate(f.owner, Invalidation::LAYOUT));

  check(take(f.router->pointerCapture()) == f.owner && f.capture.connected(),
        "a pure layout request does not change the eligibility stamp");

  take(f.tree->close());

  check(error(f.router->cancelPointerCapture(false), TreeError::CLOSED) && !f.capture.connected(),
        "closed-tree host cleanup retires capture before returning the original CLOSED error");
}

struct PinnedWidget : Widget {
  int &destroyed;
  int value{7};

  explicit PinnedWidget(int &counter) : destroyed(counter) {
  }

  ~PinnedWidget() override {
    ++destroyed;
  }
};

struct CaptureProbe {
  int &destroyed;
  bool &inside;

  CaptureProbe(int &counter, bool &active) : destroyed(counter), inside(active) {
  }

  ~CaptureProbe() {
    check(!inside, "executing callable captures cannot be destroyed inside its body");
    ++destroyed;
  }
};

void admittedLifetime() {
  int widgets{}, captures{};
  bool inside{};
  Fixture f;

  take(f.tree->remove(f.owner));

  auto node = take(f.tree->emplace<PinnedWidget>(f.parent, widgets));

  f.owner = node;
  f.layout();

  auto probe = std::make_shared<CaptureProbe>(captures, inside);
  ActionBinding binding;

  binding = take(f.router->bind(node, [&, probe](PinnedWidget &widget, const RoutedEvent &event) {
    const auto *mouse = std::get_if<MouseInputEvent>(&event);

    if (mouse && mouse->action == MouseAction::PRESS) {
      f.acquire();
    } else if (mouse && mouse->action == MouseAction::MOVE) {
      inside = true;
      binding.disconnect();
      take(f.tree->remove(node));

      check(take(f.router->pointerCapture()) == Node<Widget>{}, "self-removal cancels the weak capture record");

      ++widget.value;

      check(widget.value == 8 && widgets == 0 && captures == 0, "admitted widget and callable survive self-retirement");

      inside = false;
    }

    return EventReply::HANDLED;
  }));
  probe.reset();

  f.mouse(MouseAction::PRESS);
  f.mouse(MouseAction::MOVE, MouseButton::LEFT, 90, 90);

  check(widgets == 1 && captures == 1 && !f.capture.connected(), "pins release on UI after the callback returns");
}

void errorsAndAffinity() {
  Fixture f;
  bool releaseUnhooked{};
  int failMode{};
  auto binding = take(detail::EventRouterAccess::bindChecked(
    *f.router, f.owner, [&](Widget &, const RoutedEvent &event) -> std::expected<EventReply, TreeError> {
      const auto *mouse = std::get_if<MouseInputEvent>(&event);

      if (mouse && mouse->action == MouseAction::PRESS) {
        f.acquire();
        check(error(f.router->capturePointer(f.owner), TreeError::WOULD_BLOCK), "active capture cannot be stolen");
      } else if (mouse && mouse->action == MouseAction::MOVE && failMode == 1) {
        return std::unexpected(TreeError::DEPTH_LIMIT);
      } else if (mouse && mouse->action == MouseAction::MOVE && failMode == 2) {
        throw std::runtime_error("capture callback");
      } else if (mouse && mouse->action == MouseAction::RELEASE) {
        releaseUnhooked = !f.capture.connected();

        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      return EventReply::HANDLED;
    }));

  check(error(f.router->capturePointer(f.owner), TreeError::WOULD_BLOCK), "acquire outside LEFT PRESS is rejected");

  Fixture foreign;

  check(error(f.router->capturePointer(foreign.owner), TreeError::WRONG_SESSION), "foreign capture owner is rejected");

  f.mouse(MouseAction::PRESS);

  bool threadChecks{};

  std::thread worker([&] {
    threadChecks = error(f.router->pointerCapture(), TreeError::WRONG_THREAD) &&
                   error(f.router->cancelPointerCapture(), TreeError::WRONG_THREAD) &&
                   error(f.router->setPointerCaptureCapabilities({}), TreeError::WRONG_THREAD) &&
                   error(f.router->capturePointer(f.owner), TreeError::WRONG_THREAD);
  });

  worker.join();

  check(threadChecks && f.capture.connected(), "all checked capture boundaries reject worker calls without mutation");

  failMode = 1;

  auto typed = f.router->dispatch(MouseInputEvent{90, 90, MouseButton::LEFT, MouseAction::MOVE, {}});

  check(error(typed, TreeError::DEPTH_LIMIT) && !f.capture.connected(),
        "typed erased callback error propagates exactly");

  f.mouse(MouseAction::PRESS);

  failMode = 2;

  auto thrown = f.router->dispatch(MouseInputEvent{90, 90, MouseButton::LEFT, MouseAction::MOVE, {}});

  check(error(thrown, TreeError::CALLBACK_FAILED) && !f.capture.connected(),
        "callback exception cancels and propagates");

  f.mouse(MouseAction::PRESS);

  auto released = f.router->dispatch(MouseInputEvent{90, 90, MouseButton::LEFT, MouseAction::RELEASE, {}});

  check(error(released, TreeError::INVALID_ARGUMENT) && releaseUnhooked && !f.capture.connected(),
        "release unhooks even when its checked callback fails");
}

void tokenAndAllocation() {
  PointerCapture survivor;

  {
    Fixture f;
    bool allocationRejected{};
    bool inject{};
    bool reenter{};
    auto binding = take(f.router->bind(f.owner, [&](Widget &, const RoutedEvent &event) {
      if (const auto *mouse = std::get_if<MouseInputEvent>(&event); mouse && mouse->action == MouseAction::PRESS) {
        allocationFailureAfter = inject ? 0 : -1;
        allocationTarget = &f;
        allocationHook = reenter ? hideOwnerDuringAllocation : nullptr;

        auto acquired = f.router->capturePointer(f.owner);

        allocationFailureAfter = -1;
        allocationHook = nullptr;
        allocationTarget = nullptr;

        if (reenter) {
          allocationRejected = error(acquired, TreeError::INVALID_ARGUMENT);
        } else if (inject) {
          allocationRejected = error(acquired, TreeError::NO_MEMORY);
        } else {
          f.capture = take(std::move(acquired));
        }
      }

      return EventReply::HANDLED;
    }));

    inject = true;
    f.mouse(MouseAction::PRESS);

    check(allocationRejected && take(f.router->pointerCapture()) == Node<Widget>{},
          "allocation failure publishes no capture or partial token");

    inject = false;
    reenter = true;
    f.mouse(MouseAction::PRESS);

    check(allocationRejected && take(f.router->pointerCapture()) == Node<Widget>{},
          "reentrant allocation cannot publish capture after hiding its owner");

    take(f.tree->setVisible(f.owner, true));
    f.layout();

    reenter = false;
    f.mouse(MouseAction::PRESS);

    std::thread disconnect([token = std::move(f.capture)]() mutable {
      token.disconnect();
    });

    disconnect.join();

    check(take(f.router->pointerCapture()) == Node<Widget>{}, "worker token destruction closes future admission");

    f.mouse(MouseAction::PRESS);
    survivor = std::move(f.capture);

    check(survivor.connected() && !f.capture.connected(), "moving token transfers cancellation responsibility");

    survivor = std::move(survivor);

    check(survivor.connected(), "self move-assignment keeps the same live capture");

    f.router.reset();

    check(!survivor.connected(), "router retirement disconnects capture before the action token is destroyed");
  }

  check(!survivor.connected(), "token safely observes router destruction");

  std::thread finalDisconnect([token = std::move(survivor)]() mutable {
    token.disconnect();
  });

  finalDisconnect.join();
}
} // namespace

int main() {
  routingAndGeometry();
  capabilities();
  focusAndHostCancellation();
  lifecycle();
  conservativeEligibilityHistory();
  admittedLifetime();
  errorsAndAffinity();
  tokenAndAllocation();

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
