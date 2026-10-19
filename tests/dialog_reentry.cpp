// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <utility>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local void (*callback)(void *){};
thread_local void *context{};
thread_local std::ptrdiff_t live{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::callback && allocationTest::remaining-- == 0) {
    auto callback = std::exchange(allocationTest::callback, nullptr);
    callback(allocationTest::context);
  }
  if (auto result = std::malloc(size ? size : 1)) {
    ++allocationTest::live;
    return result;
  }
  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  if (value) {
    --allocationTest::live;
  }
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
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

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;
constexpr std::size_t MAX_FAILURE_POINTS = 1024;

void check(bool condition, const char *message, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(message) + " at " + std::to_string(where.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  check(bool(result), "unexpected checked failure", where);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Removal {
  WidgetTree *tree;
  Node<Widget> target;
  bool fired{};
  bool removed{};

  static void invoke(void *context) {
    auto &self = *static_cast<Removal *>(context);
    self.fired = true;
    self.removed = bool(self.tree->remove(self.target));
    // Isolate invalidation caused by the authorized removal from any later command write.
    (void)self.tree->consumeFrameRequest();
  }
};

enum class Target {
  BODY,
  PANEL,
  ROOT,
  HOST
};

void removedDuringStaging(Target target) {
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    ui::Ref<OverlayHost> host;
    ui::Ref<Dialog> dialog;
    ui::Ref<Button> opener;
    take(ui::mount(*scene, ui::overlayHost(host, ui::button("Opener").reference(opener).onClick([] {
    }),
                                           ui::dialog(dialog, "First", ui::label("Nonfocusable retained content")),
                                           ui::dialog("Second", ui::label("Other")))));
    take(opener.focus());
    const auto siblings = take(app->tree().inspect(host.node())).children;
    const auto panel = take(app->tree().inspect(dialog.node())).children.front();
    const auto content = take(app->tree().inspect(panel)).children.front();
    const auto body = take(app->tree().inspect(content)).children.back();
    Removal removal{&app->tree(), target == Target::BODY    ? body
                                  : target == Target::PANEL ? panel
                                  : target == Target::ROOT  ? Node<Widget>(dialog.node())
                                                            : Node<Widget>(host.node())};
    (void)take(app->tree().consumeFrameRequest());
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    allocationTest::context = &removal;
    allocationTest::callback = &Removal::invoke;
    auto result = scene->openDialog(dialog.node());
    allocationTest::callback = nullptr;
    if (!removal.fired) {
      take(std::move(result));
      std::cout << "staging removal target=" << static_cast<int>(target) << "; points=" << point << '\n';
      return;
    }
    check(removal.removed, "allocation hook removed exact target");
    const auto *actual = result ? nullptr : std::get_if<TreeError>(&result.error().cause);
    if (!actual || *actual != TreeError::STALE_NODE) {
      std::cerr << "target=" << static_cast<int>(target) << "; point=" << point
                << "; cause=" << (actual ? static_cast<int>(*actual) : -1) << '\n';
    }
    check(actual && *actual == TreeError::STALE_NODE, "staging rechecks removed exact identity before mutation");
    check(take(app->tree().consumeFrameRequest()) == Invalidation::NONE, "failed staging adds no visual invalidation");
    if (target == Target::BODY || target == Target::PANEL) {
      check(!take(dialog.opened()), "removed child cannot partially activate root");
      check(!take(app->tree().inspect(dialog.node())).visible, "root visibility is unchanged");
      check(take(app->tree().inspect(host.node())).children == siblings, "host ordering is unchanged");
      check(take(app->router().focused()) == opener.node(), "focus remains at opener");
    }
  }
  check(false, "staging allocation sweep reached finite fixture bound");
}

void reopenedActivationStorage() {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Button> opener;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*scene, ui::overlayHost(ui::button("Opener").reference(opener).onClick([] {
  }),
                                         ui::dialog(dialog, "Reopen", ui::button("Inside").onClick([] {
                                         })))));
  take(opener.focus());
  take(dialog.open().result());
  const auto reopen = [&] {
    take(app->tree().setVisible(dialog.node(), false));
    take(app->tree().setVisible(dialog.node(), true));
    take(dialog.open().result());
  };
  constexpr std::size_t WARMUP_ACTIVATIONS = 8;
  constexpr std::size_t REPEATED_ACTIVATIONS = 64;
  for (std::size_t i = 0; i != WARMUP_ACTIVATIONS; ++i) {
    reopen();
  }
  const auto before = allocationTest::live;
  for (std::size_t i = 0; i != REPEATED_ACTIVATIONS; ++i) {
    reopen();
  }
  const auto after = allocationTest::live;
  std::cout << "reopen live allocations before=" << before << "; after=" << after << '\n';
  check(after <= before, "repeated revoked activation does not retain unbounded return records");
  take(dialog.dismiss().result());
  check(take(app->router().focused()) == opener.node(), "reopen retains original exact external opener");
}

void focusReentryDuringStaging() {
  struct Attempt {
    EventRouter *router;
    Node<Widget> target;
    bool fired{};
    bool rejected{};

    static void invoke(void *context) {
      auto &self = *static_cast<Attempt *>(context);
      self.fired = true;
      auto result = self.router->setFocus(self.target);
      self.rejected = !result && result.error() == TreeError::WOULD_BLOCK;
    }
  };

  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    ui::Ref<Button> opener, other;
    ui::Ref<Dialog> dialog;
    take(ui::mount(*scene, ui::overlayHost(ui::column(ui::button("Opener").reference(opener).onClick([] {
                                           }),
                                                      ui::button("Other").reference(other).onClick([] {
                                                      })),
                                           ui::dialog(dialog, "Modal", ui::button("Inside").onClick([] {
                                           })))));
    take(opener.focus());
    Attempt attempt{&app->router(), other.node()};
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    allocationTest::context = &attempt;
    allocationTest::callback = &Attempt::invoke;
    auto result = scene->openDialog(dialog.node());
    allocationTest::callback = nullptr;
    take(std::move(result));
    if (!attempt.fired) {
      std::cout << "staging focus reentry points=" << point << '\n';
      return;
    }
    check(attempt.rejected, "public focus mutation is blocked while dialog staging borrows its ancestry");
    take(dialog.dismiss().result());
    take(other.focus());
    check(take(app->router().focused()) == other.node(), "router staging guard clears after publication");
  }
  check(false, "focus reentry sweep reached finite fixture bound");
}

void captureAdmissionReentry() {
  struct Opening {
    ManagedScene *scene;
    Node<Dialog> dialog;
    bool fired{};
    bool opened{};

    static void invoke(void *context) {
      auto &self = *static_cast<Opening *>(context);
      self.fired = true;
      self.opened = bool(self.scene->openDialog(self.dialog));
    }
  };

  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    ui::Ref<OverlayHost> host;
    ui::Ref<Label> owner;
    ui::Ref<Dialog> dialog;
    take(ui::mount(
      *scene, ui::overlayHost(host, ui::column(ui::label("Drag").reference(owner), ui::button("Background").onClick([] {
                              })),
                              ui::dialog(dialog, "Modal", ui::button("Inside").onClick([] {
                              })))));
    take(app->router().setPointerCaptureCapabilities({true, true, true}));
    Opening opening{scene.get(), dialog.node()};
    std::optional<PointerCapture> capture;
    std::optional<TreeError> failure;
    auto binding = take(app->router().bind(
      owner.node(),
      [&](Label &, const RoutedEvent &event) {
        const auto *mouse = std::get_if<MouseInputEvent>(&event);
        if (mouse && mouse->action == MouseAction::PRESS) {
          allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
          allocationTest::context = &opening;
          allocationTest::callback = &Opening::invoke;
          auto result = app->router().capturePointer(owner.node(), host.node());
          allocationTest::callback = nullptr;
          if (result) {
            capture.emplace(std::move(*result));
          } else {
            failure = result.error();
          }
        }
        return EventReply::HANDLED;
      },
      false));
    constexpr CellSize VIEWPORT{40, 12};
    take(scene->prepare(*app, VIEWPORT));
    take(app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, VIEWPORT));
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
    }));
    const auto bounds = take(app->tree().inspect(owner.node())).geometry.surface;
    MouseInputEvent press;
    press.column = bounds.origin().x;
    press.row = bounds.origin().y;
    press.button = MouseButton::LEFT;
    press.action = MouseAction::PRESS;
    take(app->router().dispatch(press));
    if (!opening.fired) {
      check(capture && !failure, "ordinary capture acquisition remains supported");
      std::cout << "capture modal-admission reentry points=" << point << '\n';
      return;
    }
    check(opening.opened && failure == TreeError::INVALID_ARGUMENT && !capture,
          "capture validates actual modal owner after every fallible acquisition boundary");
    check(take(app->router().pointerCapture()) == Node<Widget>{}, "refused capture leaves no capture owner");
  }
  check(false, "capture modal-admission sweep reached finite fixture bound");
}
} // namespace

int main() {
  try {
    removedDuringStaging(Target::BODY);
    removedDuringStaging(Target::PANEL);
    removedDuringStaging(Target::ROOT);
    removedDuringStaging(Target::HOST);
    reopenedActivationStorage();
    focusReentryDuringStaging();
    captureAdmissionReentry();
    return 0;
  } catch (const std::exception &error) {
    allocationTest::callback = nullptr;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
