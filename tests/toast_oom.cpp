// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/toast_fixture.hpp"
#include "../src/widgets/toast_state.hpp"

#include <cstdlib>
#include <utility>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local void (*hook)(void *){};
thread_local void *context{};
thread_local bool invoked{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::remaining == 0) {
    if (auto hook = std::exchange(allocationTest::hook, nullptr)) {
      allocationTest::remaining = -1;
      allocationTest::invoked = true;
      hook(allocationTest::context);
    } else {
      throw std::bad_alloc();
    }
  }
  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }
  if (auto value = std::malloc(size ? size : 1)) {
    return value;
  }
  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
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
  ::operator delete[](value);
}

namespace {
using namespace toastTest;
constexpr std::size_t MAX_FAILURE_POINTS = 4096;

void mountRollback() {
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    auto description = ui::notifications(f.center, ui::label("Body")).reference(f.wrapper);
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto result = ui::mount(*f.scene, std::move(description));
    allocationTest::remaining = -1;
    if (result) {
      std::cout << "mount success index=" << point << '\n';
      return;
    }
    check(treeCause(result.error(), TreeError::NO_MEMORY), "cold mount failure preserves exact NO_MEMORY");
    check(f.wrapper.node() == Node<Overlay>{}, "failed complete mount publishes no Ref");
    check(treeCause(f.center.snapshot().error(), TreeError::CLOSED), "failed complete mount keeps center cold");
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "cold rollback removes all private nodes");
    f.mount();
  }
  check(false, "mount OOM sweep exceeded bound");
}

enum class Operation {
  POST,
  REFRESH,
  DISMISS,
  ACTIVATE,
  HIDE,
  MOTION
};

void atomicCommand(Operation operation) {
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    f.mount({.refreshDeduplicated = true});
    int actions{};
    const auto original = take(f.center.post(
      ui::toast("Original",
                {.duration = 100, .dedupKey = "K", .indicator = {.enabled = true, .alternateBackground = true}})
        .action("Details", [&] {
          ++actions;
        })));
    f.frame();
    f.now = 25;
    const auto before = f.snapshot();
    const auto record = take(ui::detail::NotificationAccess::lock(f.center));
    const auto marker = record->timerMarker;
    const auto picture = record->slots.front()->picture;
    const auto request =
      ui::toast("Candidate", {.duration = 100, .dedupKey = operation == Operation::REFRESH ? "K" : "N"});
    (void)take(f.app->tree().consumeFrameRequest());
    std::expected<void, ManagedError> result;
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    switch (operation) {
    case Operation::POST:
    case Operation::REFRESH: {
      const auto posted = f.center.post(request);
      if (!posted) {
        result = std::unexpected(posted.error());
      }
      break;
    }
    case Operation::DISMISS:
      result = f.center.dismiss(*original.id).result();
      break;
    case Operation::ACTIVATE:
      result = f.center.activate(*original.id).result();
      break;
    case Operation::HIDE:
      result = f.center.setVisible(false).result();
      break;
    case Operation::MOTION:
      result = f.center.setReducedMotion(true).result();
      break;
    }
    allocationTest::remaining = -1;
    if (result) {
      std::cout << "operation=" << int(operation) << " success index=" << point << '\n';
      return;
    }
    const auto application = std::get_if<ApplicationError>(&result.error().cause);
    const auto scheduler = application ? std::get_if<SchedulerError>(&application->cause) : nullptr;
    check(treeCause(result.error(), TreeError::NO_MEMORY) || (scheduler && *scheduler == SchedulerError::NO_MEMORY),
          "new command allocation failures retain their exact tree or scheduler NO_MEMORY cause");
    const auto after = f.snapshot();
    check(after->items.size() == before->items.size() && after->items.front().id == before->items.front().id &&
            after->items.front().deadline == before->items.front().deadline && after->visible == before->visible &&
            after->reducedMotion == before->reducedMotion,
          "pre-adoption OOM preserves queue, identity and policy");
    check(record->timerMarker == marker && record->slots.front()->picture == picture && actions == 0,
          "pre-adoption OOM preserves old timer, immutable picture and action admission");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "pre-adoption OOM never invalidates");
    if (operation == Operation::REFRESH) {
      const auto retried = take(f.center.post(request));
      check(retried.id == original.id && f.snapshot()->items.front().deadline == 125,
            "checked refresh failure can be retried while the original services remain live");
    }
  }
  check(false, "command OOM sweep exceeded bound");
}

void firstExposureAtomicity() {
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    f.mount();
    (void)take(f.center.post(ui::toast("First exposure")));
    f.frame({50, 12}, false);
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto result = f.scene->completedLayout(*f.app);
    allocationTest::remaining = -1;
    if (result) {
      check(f.snapshot()->items.front().firstPresented, "complete successful exposure commits deadline");
      std::cout << "first exposure success index=" << point << '\n';
      return;
    }
    check(!f.snapshot()->items.front().firstPresented && !f.snapshot()->items.front().deadline,
          "first-exposure scheduling failure retains not-started TTL");
  }
  check(false, "first exposure OOM sweep exceeded bound");
}

enum class Reentry {
  STOP,
  REMOVE_CENTER,
  DISABLE_ACTION
};

void registrationReentry(Reentry reentry) {
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    f.mount({.refreshDeduplicated = true});
    int actions{};
    const auto original =
      take(f.center.post(ui::toast("Original", {.duration = 100, .dedupKey = "K"}).action("Details", [&] {
        ++actions;
      })));
    f.frame();
    f.now = 25;
    const auto before = f.snapshot();
    const auto request = ui::toast("Duplicate", {.dedupKey = "K"});
    allocationTest::context = &f;
    allocationTest::invoked = false;
    allocationTest::hook = reentry == Reentry::REMOVE_CENTER ? +[](void *context) {
      auto &fixture = *static_cast<Fixture *>(context);
      take(fixture.scene->remove(fixture.wrapper.node()));
    } : reentry == Reentry::DISABLE_ACTION ? +[](void *context) {
      auto &fixture = *static_cast<Fixture *>(context);
      const auto record = take(ui::detail::NotificationAccess::lock(fixture.center));
      take(fixture.app->tree().setEnabled(record->slots.front()->node, false));
    } : +[](void *context) {
      take(static_cast<Fixture *>(context)->app->scheduler().stop());
    };
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto result = reentry == Reentry::DISABLE_ACTION ? f.center.activate(*original.id).result()
                                                     : [&]() -> std::expected<void, ManagedError> {
      auto posted = f.center.post(request);
      return posted ? std::expected<void, ManagedError>{} : std::unexpected(posted.error());
    }();
    allocationTest::remaining = -1;
    allocationTest::hook = nullptr;
    if (!allocationTest::invoked) {
      check(bool(result), "untriggered allocation reentry completes normal refresh");
      std::cout << "reentry=" << int(reentry) << " covered allocations=" << point << '\n';
      return;
    }
    check(!result, "consumed or closed staged registration cannot publish");
    if (reentry != Reentry::REMOVE_CENTER) {
      const auto after = f.snapshot();
      check(after->items.front().deadline == before->items.front().deadline,
            "scheduler-stop reentry keeps committed deadline");
      check(after->items.front().id == original.id && actions == 0,
            "slot eligibility change during staging never dismisses or invokes the original action");
    } else {
      check(!f.center.snapshot(), "independent center removal is never undone");
    }
  }
  check(false, "allocation reentry sweep exceeded bound");
}

void preparedPictureAtomicity(bool theme) {
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    f.mount({.margin = 0});
    (void)take(f.center.post(ui::toast("Original wide summary", {.duration = 100})));
    f.frame();
    const auto record = take(ui::detail::NotificationAccess::lock(f.center));
    const auto picture = record->slots.front()->picture;
    const auto timer = record->timerMarker;
    const auto before = f.snapshot();
    CellSize size{8, 1};
    if (theme) {
      StyleSheet sheet;
      sheet.rules.push_back(
        {.selector = {.classes = {"toast-message"}}, .declarations = {.foreground = RgbColor{10, 20, 30}}});
      take(f.scene->theme(sheet));
      size = {50, 12};
    }
    const auto bounds = take(CellRect::from({}, size));
    std::optional<LayoutSnapshot> layout;
    take(f.app->tree().frame([&] {
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
      layout.emplace(take(LayoutSnapshot::capture(f.app->tree(), f.app->root())));
    }));
    (void)take(f.app->tree().consumeFrameRequest());
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto result = f.scene->prepareViews(*f.app, *layout);
    allocationTest::remaining = -1;
    if (result) {
      check(record->slots.front()->picture != picture,
            "changed width/theme publishes a complete new immutable picture");
      std::cout << "prepared theme=" << theme << " success index=" << point << '\n';
      return;
    }
    check(result.error() == TreeError::NO_MEMORY, "new picture staging allocation remains typed PREPARE failure");
    check(record->slots.front()->picture == picture && record->timerMarker == timer &&
            f.snapshot()->items.front().deadline == before->items.front().deadline,
          "failed new picture preparation retains old cache, queue and committed timer");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
          "failed preparation requests no additional frame after the already published external change");
  }
  check(false, "prepared picture OOM sweep exceeded bound");
}
} // namespace

int main() try {
  mountRollback();
  for (const auto operation : {Operation::POST, Operation::REFRESH, Operation::DISMISS, Operation::ACTIVATE,
                               Operation::HIDE, Operation::MOTION}) {
    atomicCommand(operation);
  }
  firstExposureAtomicity();
  registrationReentry(Reentry::STOP);
  registrationReentry(Reentry::REMOVE_CENTER);
  registrationReentry(Reentry::DISABLE_ACTION);
  preparedPictureAtomicity(false);
  preparedPictureAtomicity(true);
  return 0;
} catch (const std::exception &failure) {
  allocationTest::remaining = -1;
  allocationTest::hook = nullptr;
  std::cerr << failure.what() << '\n';
  return 1;
}
