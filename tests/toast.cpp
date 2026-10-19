// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/toast_fixture.hpp"
#include "../src/widgets/toast_state.hpp"

#include <thread>

namespace {
using namespace toastTest;

void boundedQueue() {
  Fixture f;
  f.mount();
  std::vector<ToastId> ids(0);
  for (int i = 0; i != 32; ++i) {
    auto posted = take(f.center.post(ui::toast(std::to_string(i))));
    check(posted.status == (i < 3 ? ToastPostStatus::ACCEPTED_VISIBLE : ToastPostStatus::ACCEPTED_QUEUED),
          "exact logical receipt");
    ids.push_back(*posted.id);
  }
  auto overflow = take(f.center.post(ui::toast("Replacement")));
  check(overflow.evictedId == ids[3] && overflow.status == ToastPostStatus::ACCEPTED_QUEUED,
        "oldest queued transient evicted");
  const auto before = f.snapshot();
  check(before->items.size() == 32 && before->evicted == 1 && before->dropped == 0,
        "total budget includes selected items");
  take(f.center.clear());
  for (int i = 0; i != 32; ++i) {
    (void)take(f.center.post(ui::toast("Persistent", {.duration = std::nullopt})));
  }
  auto dropped = take(f.center.post(ui::toast("No victim")));
  check(dropped.status == ToastPostStatus::DROPPED && !dropped.id && !dropped.evictedId,
        "persistent backlog is protected");
  check(f.snapshot()->dropped == 1 && f.snapshot()->items.size() == 32, "drop counter with no queue growth");
  check(before->items.front().data->text.bytes() == "0", "saved snapshot independent of later clear");
}

void dedupAndValidation() {
  Fixture f;
  f.mount();
  int first{}, replacement{};
  auto original =
    take(f.center.post(ui::toast("Original", {.duration = std::nullopt, .dedupKey = "save"}).action("Details", [&] {
      ++first;
    })));
  auto duplicate =
    take(f.center.post(ui::toast("Discarded", {.duration = 1, .dedupKey = "save"}).action("Wrong action", [&] {
      ++replacement;
    })));
  check(duplicate.status == ToastPostStatus::DEDUPLICATED && duplicate.id == original.id, "dedup retains original ID");
  check(f.snapshot()->items.front().data->text.bytes() == "Original", "dedup retains full payload");
  f.frame();
  take(f.center.activate(*original.id));
  check(first == 1 && replacement == 0 && f.snapshot()->items.empty(),
        "dedup keeps original action and default dismissal");
  auto invalid = f.center.post(ui::toast("Zero", {.duration = 0}));
  check(!invalid && treeCause(invalid.error(), TreeError::INVALID_ARGUMENT), "zero duration rejected before adoption");
  auto indicator = f.center.post(ui::toast("Bad glyph", {.indicator = {.enabled = true, .glyph = TextValue("ab")}}));
  check(!indicator && treeCause(indicator.error(), TreeError::INVALID_ARGUMENT),
        "indicator requires one cell and one cluster");
  check(f.snapshot()->items.empty(), "invalid requests leave queue unchanged");
}

void affinityAndIdentity() {
  Fixture f;
  f.mount();
  const auto id = *take(f.center.post(ui::toast("Identity"))).id;
  std::optional<ManagedError> worker;
  std::thread thread([&] {
    auto result = f.center.snapshot();
    if (!result) {
      worker = result.error();
    }
  });
  thread.join();
  check(worker && treeCause(*worker, TreeError::WRONG_THREAD), "immutable affinity checked before scene access");
  auto foreign = id;
  ++foreign.center.generation;
  check(treeCause(f.center.dismiss(foreign).error(), TreeError::WRONG_SESSION),
        "full foreign center identity rejected");
  take(f.scene->remove(f.wrapper.node()));
  check(treeCause(f.center.dismiss(id).error(), TreeError::STALE_NODE), "removed center does not retarget");
  take(f.scene->close());
  check(treeCause(f.center.snapshot().error(), TreeError::CLOSED),
        "closed scene differs from an independently removed center");
}

void owningLocalizationAndClipping() {
  Fixture f;
  NotificationOptions options;
  options.margin = 0;
  options.maximumWidth = 8;
  options.textResolver = [](std::string_view key) -> std::expected<std::string, TextResourceError> {
    if (key == "message") {
      return std::string("界éxyz\nFull details");
    }
    if (key == "action") {
      return std::string("Details");
    }
    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  };
  f.mount(options);
  (void)take(f.center.post(ui::toast(i18n("message"), {.duration = std::nullopt}).action(i18n("action"), [] {
  })));
  f.frame({8, 3});
  const auto record = take(ui::detail::NotificationAccess::lock(f.center));
  check(record->slots.front()->picture->summary.bytes() == "界éx…",
        "summary preserves complete wide/combining graphemes and ellipsis");
  const auto saved = f.snapshot();
  check(saved->items.front().data->text.bytes() == "界éxyz\nFull details" &&
          saved->items.front().data->actionLabel->bytes() == "Details",
        "full localized values remain owned");
  f.frame({0, 0});
  check(f.advance(10000).timers == 0, "zero extent persistent presentation creates no timer");
  take(f.center.clear());
  check(saved->items.front().data->text.bytes() == "界éxyz\nFull details",
        "snapshot survives clear and clipping changes");
}

void contentRemovalDoesNotOwnNotifications() {
  Fixture f;
  ui::Ref<Label> body;
  take(ui::mount(*f.scene,
                 ui::notifications(f.center, ui::label("Replaceable content").reference(body)).reference(f.wrapper)));
  take(f.scene->remove(body.node()));
  const auto posted = take(f.center.post(ui::toast("Application-owned", {.duration = std::nullopt})));
  f.frame();
  check(f.snapshot()->items.size() == 1 && f.snapshot()->items.front().firstPresented,
        "ordinary content removal leaves the notification owner and viewport alive");
  take(f.center.dismiss(*posted.id));
  check(f.snapshot()->items.empty(), "commands remain live after independent body-content removal");
}

void themeOnlySubjectRequestsPaint() {
  Fixture f;
  f.mount();
  (void)take(f.center.post(ui::toast("Theme", {.duration = std::nullopt})));
  f.frame();
  StyleSheet sheet;
  sheet.rules.push_back(
    {.selector = {.classes = {"toast-message"}}, .declarations = {.foreground = RgbColor{10, 20, 30}}});
  (void)take(f.app->tree().consumeFrameRequest());
  take(f.scene->theme(sheet));
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT,
        "toast-only theme publication wakes its real cached subject");
  f.frame();
  const auto record = take(ui::detail::NotificationAccess::lock(f.center));
  check(record->slots.front()->picture->styles[1].foreground == RgbColor{10, 20, 30},
        "prepared message uses the existing resolver result");
  (void)take(f.app->tree().consumeFrameRequest());
  take(f.scene->theme(sheet));
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "equal resolved toast theme requests no frame");
}
} // namespace

int main() try {
  boundedQueue();
  dedupAndValidation();
  affinityAndIdentity();
  owningLocalizationAndClipping();
  contentRemovalDoesNotOwnNotifications();
  themeOnlySubjectRequestsPaint();
  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
