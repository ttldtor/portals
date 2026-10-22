// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/toast_state.hpp"
#include "support/toast_fixture.hpp"

#include <thread>

namespace {
using namespace toastTest;

void visualReference() {
  Fixture f;
  ui::Ref<Overlay> view;
  auto copy = view;

  take(ui::mount(*f.scene, ui::notifications(view, f.center, ui::label("Body"))));
  check(view.node() != Node<Overlay>{} && copy.node() == view.node(), "ref-first notification view publishes once");

  const auto receipt = take(f.center.post(ui::toast("Owning message", {.duration = std::nullopt})));

  check(receipt.status == ToastPostStatus::ACCEPTED_VISIBLE && receipt.id,
        "visual reference leaves exact service post receipt available");
  take(f.scene->remove(view.node()));
  check(treeCause(copy.geometry().error(), TreeError::STALE_NODE), "removed real Overlay observation stays stale");
  check(treeCause(f.center.snapshot().error(), TreeError::STALE_NODE) && receipt.id,
        "service retires with its layer while owning post identity survives");
}

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

void declarativeConstructionParity() {
  for (int path = 0; path != 5; ++path) {
    Fixture f;
    int ownResolutions{}, resourceResolutions{}, factories{};
    NotificationOptions captured;

    captured.capacity = 7;
    captured.maxVisible = 3;
    captured.maximumWidth = 19;
    captured.margin = 2;
    captured.gap = 2;
    captured.actionLabelBytes = 9;
    captured.dedupKeyBytes = 9;
    captured.textResolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++ownResolutions;

      return "Own";
    };
    const ui::ControlNotificationSettings patch{
      .capacity = 4,
      .maxVisible = 2,
      .placement = ToastPlacement::TOP_START,
      .maximumWidth = 12,
      .margin = 1,
      .gap = 0,
      .overflow = ToastOverflowPolicy::REJECT_NEW,
      .refreshDeduplicated = true,
      .reducedMotion = true,
      .textLimits = {.maxInputBytes = 64, .maxClusterBytes = 12, .maxGraphemes = 32, .maxCells = 32},
      .actionLabelBytes = 4,
      .dedupKeyBytes = 4,
      .maxPaintCells = 512};
    ui::ControlContext controls;

    controls.define("Overlay", "Label");
    controls.defaults("Overlay", {.text = text("Unrelated")});
    if (path == 4) {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resourceResolutions;

        return "Resource must remain unused";
      };
      options.control<Overlay>(
        "Center",
        [&](ui::SceneResourceArguments) {
          ++factories;

          return ui::notifications(f.center, ui::label("Body"), captured)
            .notificationSettings({.maximumWidth = 30, .actionLabelBytes = 9})
            .reference(f.wrapper);
        },
        ui::controlPropertySchema("Notifications"));
      take(ui::mount(*f.scene,
                     ui::resource(R"(defaults{Center{notification{capacity=4 maxVisible=2}}}
                       definitions{Inbox=Center{notification{placement=TOP_START maximumWidth=12 margin=1 gap=0
                         overflow=REJECT_NEW refreshDeduplicated=true reducedMotion=true
                         textLimits{maxInputBytes=64 maxClusterBytes=12 maxGraphemes=32 maxCells=32}}}}
                       scene{Inbox{notification={actionLabelBytes=4 dedupKeyBytes=4 maxPaintCells=512}}})",
                                  options),
                     controls));
    } else {
      auto actual = captured;

      if (path == 0) {
        actual.capacity = 4;
        actual.maxVisible = 2;
        actual.placement = ToastPlacement::TOP_START;
        actual.maximumWidth = 12;
        actual.margin = 1;
        actual.gap = 0;
        actual.overflow = ToastOverflowPolicy::REJECT_NEW;
        actual.refreshDeduplicated = true;
        actual.reducedMotion = true;
        actual.textLimits = {64, 12, 32, 32};
        actual.actionLabelBytes = 4;
        actual.dedupKeyBytes = 4;
        actual.maxPaintCells = 512;
      }
      auto recipe = ui::notifications(f.center, ui::label("Body"), actual).reference(f.wrapper);

      if (path == 1) {
        (void)std::move(recipe).notificationSettings(patch);
      } else if (path == 2) {
        recipe = std::move(recipe) | ui::notificationSettings(patch);
      } else if (path == 3) {
        (void)std::move(recipe).settings({.notification = patch});
      }
      take(ui::mount(*f.scene, std::move(recipe), controls));
    }
    const auto record = take(ui::detail::NotificationAccess::lock(f.center));
    const auto &actual = *record->options;

    check(actual.capacity == 4 && actual.maxVisible == 2 && actual.placement == ToastPlacement::TOP_START &&
            actual.maximumWidth == 12 && actual.margin == 1 && actual.gap == 0 &&
            actual.overflow == ToastOverflowPolicy::REJECT_NEW && actual.refreshDeduplicated && actual.reducedMotion,
          "native/method/pipe/settings/CFG preserve effective notification policy");
    check(actual.textLimits.max_input_bytes == 64 && actual.textLimits.max_cluster_bytes == 12 &&
            actual.textLimits.max_graphemes == 32 && actual.textLimits.max_cells == 32 &&
            actual.actionLabelBytes == 4 && actual.dedupKeyBytes == 4 && actual.maxPaintCells == 512,
          "effective construction limits reach the real center");
    check(factories == (path == 4 ? 1 : 0) && resourceResolutions == 0,
          "numeric construction does not invoke resource localization");
    const auto first = take(f.center.post(ui::toast(i18n("future"), {.duration = 100, .dedupKey = "K"})));

    check(ownResolutions == 1, "future posts preserve the explicit code-owned resolver");
    for (int index = 1; index != 4; ++index) {
      const auto posted = take(f.center.post(ui::toast("Next", {.duration = std::nullopt})));

      check(posted.status == (index == 1 ? ToastPostStatus::ACCEPTED_VISIBLE : ToastPostStatus::ACCEPTED_QUEUED),
            "effective maxVisible selects only two rows");
    }
    const auto rejected = f.center.post(ui::toast("Overflow"));

    check(!rejected && treeCause(rejected.error(), TreeError::NODE_LIMIT) && f.snapshot()->items.size() == 4,
          "effective capacity and REJECT_NEW preserve the full queue");
    f.frame();
    const auto presented = f.snapshot();
    const auto &item = presented->items.front();

    check(item.firstPresented && item.rectangle && item.rectangle->size().width <= 12 && item.deadline == 100,
          "actual presentation honors the width cap and first-exposure timer");
    f.now = 40;
    const auto duplicate = take(f.center.post(ui::toast("New", {.duration = 9000, .dedupKey = "K"})));

    check(duplicate.id == first.id && f.snapshot()->items.front().deadline == 140,
          "declarative refresh retains native duration and identity");
    take(f.center.clear());
    const auto action = f.center.post(ui::toast("Short").action("12345", [] {
    }));
    const auto key = f.center.post(ui::toast("Short", {.dedupKey = "12345"}));
    const auto textLimit = f.center.post(ui::toast(std::string(65, 'x')));

    check(!action && !key && !textLimit && f.snapshot()->items.empty(),
          "effective action, dedup and input limits reject before adoption");
    (void)take(f.center.post(
      ui::toast("Still", {.duration = std::nullopt, .indicator = {.enabled = true, .alternateBackground = true}})));
    f.frame();
    check(f.advance(1000).timers == 0, "declarative reduced motion suppresses the persistent effect timer");
    take(f.center.clear());
    f.frame();
    check(f.advance(2000).timers == 0 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
          "empty declarative center has no periodic redraw");
    take(f.scene->remove(f.wrapper.node()));
    check(treeCause(f.center.snapshot().error(), TreeError::STALE_NODE),
          "declarative center retires with the real Overlay");
    check(resourceResolutions == 0, "prepare-only resolver was never retained by the center");
  }
}

void scopedDefaultsAndLegacyOverlayVariant() {
  for (const bool lazy : {false, true}) {
    ui::NotificationCenter center;
    ui::Ref<Overlay> wrapper;
    ui::ControlContext controls;
    int factories{};

    controls.define("Overlay", "Label");
    controls.defaults("Overlay", {.text = text("unrelated")});
    controls.defaults("Notifications", {.notification = {.capacity = 4, .actionLabelBytes = 2}});
    controls.define("Inbox", "Notifications", {.notification = {.maxVisible = 1}});
    auto factory = [&] {
      ++factories;
      NotificationOptions captured;

      captured.capacity = 8;
      captured.maxVisible = 3;
      return ui::notifications(center, ui::custom<Widget>(), captured).variant("Inbox").reference(wrapper);
    };

    if (lazy) {
      ui::SceneOptions options;

      options.controls = controls;
      auto owner = ui::scene(
        [&](ManagedScene &) {
          return factory();
        },
        options);

      check(factories == 0, "semantic notification SceneFactory stays lazy before preflight");
      take(owner.prepareChecked());
      const auto record = take(ui::detail::NotificationAccess::lock(center));

      check(record->options->capacity == 4 && record->options->maxVisible == 1 &&
              record->options->actionLabelBytes == 2,
            "lazy Notifications defaults and definitions reach actual service options");
      check(take(center.post(ui::toast("Now"))).status == ToastPostStatus::ACCEPTED_VISIBLE,
            "lazy semantic binding publishes the real center");
      take(owner.close());
    } else {
      Fixture f;

      take(ui::mount(*f.scene, factory(), controls));
      const auto record = take(ui::detail::NotificationAccess::lock(center));

      check(record->options->capacity == 4 && record->options->maxVisible == 1 &&
              record->options->actionLabelBytes == 2,
            "mount preflight recognizes native Notifications defaults and variants");
    }
    check(factories == 1, "one native notification factory invocation");
  }
  Fixture f;
  ui::ControlContext controls;

  controls.define("MyOverlay", "Overlay", {.width = 99});
  controls.defaults("Overlay", {.width = 98});
  controls.defaults("Notifications", {.notification = {.capacity = 1}});
  take(ui::mount(
    *f.scene,
    ui::notifications(f.center, ui::custom<Widget>()).variant("MyOverlay").width(17).id("legacy").reference(f.wrapper),
    controls));
  StyleSheet sheet;

  sheet.rules.push_back({.selector = {.type = "Overlay"}, .declarations = {.bold = true}});
  sheet.rules.push_back({.selector = {.type = "MyOverlay", .id = "legacy"}, .declarations = {.italic = true}});
  take(f.scene->theme(sheet));
  f.frame();
  bool nativeStyle{}, variantStyle{};
  std::int32_t width{};

  take(f.app->tree().read(f.wrapper.node(), [&](const Overlay &widget) {
    width = widget.sizePolicy().width.preferred;
    nativeStyle = widget.computedStyle() && widget.computedStyle()->normal.bold;
    variantStyle = widget.computedStyle() && widget.computedStyle()->normal.italic;
  }));
  check(width == 17 && nativeStyle && variantStyle,
        "legacy Overlay-derived notification variant retains ID/style and ignores inherited width");
  const auto record = take(ui::detail::NotificationAccess::lock(f.center));

  check(record->options->capacity == 32, "legacy style-only Overlay variant does not adopt Notifications defaults");
  (void)take(f.center.post(ui::toast("First")));
  (void)take(f.center.post(ui::toast("Second")));
  check(f.snapshot()->items.size() == 2, "legacy style-only notification variant still owns the real queue");
}

} // namespace

int main() try {
  declarativeConstructionParity();
  scopedDefaultsAndLegacyOverlayVariant();
  visualReference();
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
