// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/toast_fixture.hpp"
#include "../src/widgets/toast_state.hpp"

namespace {
using namespace toastTest;

void perSlotFirstExposure() {
  Fixture f;
  f.mount({.margin = 0, .gap = 0});
  const auto persistent = take(f.center.post(ui::toast("Persistent", {.duration = std::nullopt})));
  (void)take(f.center.post(ui::toast("Second")));
  (void)take(f.center.post(ui::toast("Third")));
  check(!f.snapshot()->items[0].firstPresented && !f.snapshot()->items[1].deadline, "logical selection starts no TTL");
  f.frame({30, 1});
  auto tiny = f.snapshot();
  check(tiny->items[0].firstPresented && !tiny->items[0].deadline && !tiny->items[1].firstPresented &&
          !tiny->items[2].firstPresented,
        "only the positively clipped slot is first-presented");
  check(f.advance(6000).timers == 0, "never-presented selected rows do not create expiry timers");
  f.frame({30, 5});
  auto expanded = f.snapshot();
  check(expanded->items[1].deadline == 11000 && expanded->items[2].deadline == 11000,
        "TTL starts at new eligible frame time");
  f.frame({30, 1});
  check(f.advance(10999).timers == 0, "resize demotion retains original absolute deadline");
  check(f.advance(11000).timers == 1, "simultaneous deadlines use one logical callback");
  check(f.snapshot()->items.size() == 1 && f.snapshot()->items[0].id == *persistent.id,
        "persistent survives other expiries");
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(20000).timers == 0 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "last transient removal leaves no periodic work");
}

void hiddenAndEffects() {
  Fixture f;
  f.mount({.margin = 0, .gap = 0});
  (void)take(f.center.post(ui::toast("Transient", {.duration = 100})));
  f.frame();
  take(f.center.setVisible(false));
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(200).timers == 0, "explicit hide cancels immediately");
  take(f.center.setVisible(true));
  check(f.snapshot()->items.empty(), "expired hidden message removed before reappearance");
  auto diagnostic =
    ui::toast(
      "Owning diagnostic details",
      {.duration = std::nullopt, .dismissOnAction = false, .indicator = {.enabled = true, .alternateBackground = true}})
      .action("Details", [] {
      });
  (void)take(f.center.post(diagnostic));
  take(f.center.setReducedMotion(true));
  f.frame();
  check(f.advance(1000).timers == 0, "persistent reduced-motion diagnostic has zero timer");
  take(f.center.setReducedMotion(false));
  f.frame();
  check(f.advance(1500).timers == 1, "one visible alternating indicator timer");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT, "indicator phase requests only paint");
  take(f.center.clear());
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(5000).timers == 0 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "clear removes last effect timer and redraw requests");
}

void refreshKeepsExistingDuration() {
  Fixture f;
  f.mount({.refreshDeduplicated = true});
  const auto original = take(f.center.post(ui::toast("Original", {.duration = 100, .dedupKey = "K"})));
  f.frame();
  f.now = 50;
  const auto duplicate = take(f.center.post(ui::toast("Incoming", {.duration = 9000, .dedupKey = "K"})));
  check(duplicate.id == original.id && f.snapshot()->items.front().deadline == 150,
        "refresh uses existing duration and identity");
  check(f.advance(100).timers == 0 && f.advance(150).timers == 1 && f.snapshot()->items.empty(),
        "replaced old token never fires");
}

void schedulerCapacityIsNotSuccessfulExposure() {
  ApplicationOptions options;
  options.scheduler.subscription_capacity = 1;
  Fixture f(options);
  f.mount();
  auto scope = take(f.app->scheduler().makeScope());
  auto blocker = take(f.app->scheduler().at(scope, 100000, [] {
  }));
  (void)take(f.center.post(ui::toast("Waiting for a timer slot")));
  f.frame({50, 12}, false);
  auto exposed = f.scene->completedLayout(*f.app);
  check(!exposed && !f.snapshot()->items.front().firstPresented && !f.snapshot()->items.front().deadline,
        "capacity rejection cannot commit first presentation or TTL");
  const auto diagnostics = take(f.scene->diagnostics());
  check(!diagnostics.errors.empty(), "first exposure retains full layout error");
  const auto &failure = diagnostics.errors.back().error;
  const auto application = std::get_if<ApplicationError>(&failure.cause);
  const auto scheduler = application ? std::get_if<SchedulerError>(&application->cause) : nullptr;
  check(failure.phase == ManagedPhase::LAYOUT && application && application->phase == ApplicationPhase::RENDER &&
          scheduler && *scheduler == SchedulerError::WOULD_BLOCK,
        "full scheduler cause and actual exposure boundary retained");
  take(blocker.disconnect());
  const auto retry = f.scene->completedLayout(*f.app);
  check(!retry && !f.snapshot()->items.front().firstPresented && !f.snapshot()->items.front().deadline,
        "manual retry cannot resurrect the scheduler stopped by the existing error boundary");
  const auto after = take(f.scene->diagnostics());
  const auto stoppedApplication = std::get_if<ApplicationError>(&after.errors.back().error.cause);
  const auto stoppedScheduler = stoppedApplication ? std::get_if<SchedulerError>(&stoppedApplication->cause) : nullptr;
  check(stoppedScheduler && *stoppedScheduler == SchedulerError::CLOSED,
        "stopped services retain their exact scheduler cause on retry");
}

void disabledAssignedSlotDoesNotPresentOrAdmit() {
  Fixture f;
  f.mount();
  int actions{};
  const auto posted = take(f.center.post(
    ui::toast("Disabled row",
              {.duration = 5000, .dismissOnAction = false, .indicator = {.enabled = true, .alternateBackground = true}})
      .action("Details", [&] {
        ++actions;
      })));
  const auto layer = take(f.app->tree().inspect(f.wrapper.node())).children.back();
  const auto row = take(f.app->tree().inspect(layer)).children.front();
  f.frame({50, 12}, false);
  take(f.app->tree().setEnabled(row, false));
  const auto disabled = take(f.app->tree().inspect(row));
  check(disabled.geometry.valid && !disabled.geometry.surface.intersection(disabled.geometry.clip).empty(),
        "disabling preserves the arranged positive clip used by this regression");
  take(f.scene->completedLayout(*f.app));
  const auto rejected = f.center.activate(*posted.id);
  check(!rejected && treeCause(rejected.error(), TreeError::INVALID_ARGUMENT) && actions == 0 &&
          !f.snapshot()->items.front().firstPresented && !f.snapshot()->items.front().deadline,
        "disabled exact slot starts neither first exposure nor action");
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(6000).timers == 0 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "disabled slot creates no effect, premature timeout or redraw");

  take(f.app->tree().setEnabled(row, true));
  f.frame();
  check(f.snapshot()->items.front().firstPresented && f.snapshot()->items.front().deadline == 11000,
        "enabling permits its first actual presentation with a new absolute deadline");
  take(f.center.activate(*posted.id));
  check(actions == 1, "enabled assigned slot admits the original action");

  take(f.app->tree().setVisible(row, false));
  take(f.scene->afterDispatch(*f.app));
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(12000).timers == 0 && f.snapshot()->items.front().deadline == 11000 &&
          take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "hidden assigned slot preserves its deadline without recurring work");
  take(f.app->tree().setVisible(row, true));
  f.frame();
  check(f.snapshot()->items.empty(), "expired hidden slot is removed before reappearance");

  const auto removed =
    take(f.center.post(ui::toast("Removed row", {.duration = std::nullopt,
                                                 .dismissOnAction = false,
                                                 .indicator = {.enabled = true, .alternateBackground = true}})
                         .action("Details", [&] {
                           ++actions;
                         })));
  f.frame();
  take(f.app->tree().remove(row));
  f.frame();
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(12500).timers == 0 && actions == 1 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "removed assigned slot cancels its scheduled effect without redraw or replacement");
  const auto stale = f.center.activate(*removed.id);
  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE) && actions == 1,
        "removed full slot identity never admits or retargets its original action");
  check(f.advance(13000).timers == 0 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "removed slot never rearms its effect timer");
}
} // namespace

int main() try {
  perSlotFirstExposure();
  hiddenAndEffects();
  refreshKeepsExistingDuration();
  schedulerCapacityIsNotSuccessfulExposure();
  disabledAssignedSlotDoesNotPresentOrAdmit();
  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
