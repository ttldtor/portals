// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/theme_selection.hpp>

#include "support/managed_text_fixture.hpp"

namespace {
constexpr RgbColor RED{220, 10, 10};
constexpr RgbColor BLUE{10, 10, 220};

StyleSheet color(RgbColor foreground, std::optional<std::int32_t> width = {}) {
  StyleRule rule;
  rule.declarations.foreground = foreground;
  rule.declarations.minWidth = width;

  return {.rules = {rule}};
}

bool styleCause(const ManagedError &failure, TreeError expected) {
  const auto *value = std::get_if<StyleError>(&failure.cause);

  return failure.phase == ManagedPhase::STYLE && value && value->cause == expected;
}

struct SelectionFixture {
  ui::Ref<Button> button;
  ui::ThemeSelectionRef selection;
  std::function<void()> factoryHook;

  std::size_t factoryCalls{};
  ManagedScene *managed{};
  Application *app{};
  ui::Scene scene = ui::scene(ui::button(button, "Subject").style({"Button", "subject", {}}));

  explicit SelectionFixture(ui::ThemeSelectionOptions options = {}) {
    scene.configure([this](ManagedScene &owner, Application &application) {
      managed = &owner;
      app = &application;
    });
    scene | ui::themeSelection(selection,
                               {{"red", "Red",
                                 [this] {
                                   ++factoryCalls;

                                   if (factoryHook) {
                                     factoryHook();
                                   }

                                   return color(RED);
                                 }},
                                {"blue", "Blue",
                                 [] {
                                   return color(BLUE, 7);
                                 }}},
                               options);
    take(scene.prepareChecked());
    take(managed->prepare(*app, VIEWPORT));
  }

  ComputedStyle read() {
    ComputedStyle result;
    take(app->tree().read(button.node(), [&](const Button &value) {
      result = *value.computedStyle();
    }));

    return result;
  }

  std::uint64_t revision() {
    std::uint64_t result{};
    take(app->tree().read(button.node(), [&](const Button &value) {
      result = value.styleRevision();
    }));

    return result;
  }
};

void lifecycle() {
  ui::ThemeSelectionRef cold;
  check(styleCause(cold.preview("red").error(), TreeError::CLOSED), "cold selection is checked CLOSED");
  SelectionFixture f;

  auto initial = take(f.selection.snapshot());
  check(initial->catalog->choices.size() == 2 && initial->status == ui::ThemeSelectionStatus::IDLE &&
          !initial->previewedId && !initial->lastCommittedId && f.factoryCalls == 0,
        "setup normalizes without factories");
  take(f.managed->theme(color(BLUE, 19)));
  take(f.selection.preview("red"));
  check(f.read().normal.foreground == RED && f.read().minWidth != 19, "preview masks all base declarations");
  check(take(f.managed->styleSources()).size() == 2, "private contribution has no public handle");

  auto candidate = take(f.selection.snapshot());
  check(candidate->status == ui::ThemeSelectionStatus::ACTIVE && candidate->previewedId == "red",
        "active owning selection");
  take(f.selection.preview("blue"));
  check(f.read().normal.foreground == BLUE && f.read().minWidth == 7, "explicit update installs latest candidate");
  take(f.selection.cancel());
  check(f.read().normal.foreground == BLUE && f.read().minWidth == 19, "cancel restores original base");
  check(candidate->previewedId == "red" && candidate->status == ui::ThemeSelectionStatus::ACTIVE,
        "old snapshot independent");
  take(f.selection.preview("red"));
  take(f.selection.commit());

  auto committed = take(f.selection.snapshot());
  check(committed->status == ui::ThemeSelectionStatus::IDLE && committed->lastCommittedId == "red" &&
          !committed->previewedId && f.read().normal.foreground == RED,
        "commit promotes candidate and stages historical result");

  const auto second = f.selection.commit();
  check(!second && styleCause(second.error(), TreeError::INVALID_ARGUMENT),
        "idle commit cannot deliver another result");
  allocationTest::remaining = 0;

  const auto idleCancel = f.selection.cancel();
  allocationTest::remaining = -1;
  check(bool(idleCancel), "idle cancel allocation free");

  std::optional<ManagedError> foreign;
  std::thread thread([&] {
    auto result = f.selection.snapshot();

    if (!result) {
      foreign = result.error();
    }
  });
  thread.join();
  check(foreign && styleCause(*foreign, TreeError::WRONG_THREAD), "affinity precedes mutable observation");
  take(f.scene.close());
  check(styleCause(f.selection.snapshot().error(), TreeError::CLOSED) && committed->lastCommittedId == "red",
        "owning metadata survives shutdown without retaining scene");
}

void takeoverAndOverrides() {
  SelectionFixture f;
  take(f.managed->theme(color(BLUE, 19)));
  take(f.selection.preview("red"));

  auto application = take(f.managed->addLayer(StyleTier::APPLICATION, "application", color(BLUE, 23)));
  take(f.selection.cancel());
  check(f.read().normal.foreground == BLUE && f.read().minWidth == 23, "newer application edits survive restore");
  take(f.managed->removeLayer(application));
  take(f.selection.preview("red"));
  StyleSheet invalid = color(BLUE, -1);
  check(!f.managed->theme(invalid), "invalid external takeover rejected");
  check(take(f.selection.snapshot())->status == ui::ThemeSelectionStatus::ACTIVE, "failed takeover retains authority");
  take(f.managed->theme(color(RED)));

  const auto superseded = take(f.selection.snapshot());
  check(superseded->status == ui::ThemeSelectionStatus::SUPERSEDED && superseded->previewedId == "red",
        "equal external takeover exposes historical selection");

  const auto origin = std::source_location::current();
  auto cancelled = f.selection.cancel(origin);
  check(!cancelled && styleCause(cancelled.error(), TreeError::INVALID_ARGUMENT) && cancelled.error().operationOrigin &&
          cancelled.error().operationOrigin->line() == origin.line(),
        "superseded cancel retains exact STYLE cause and operation origin");
  check(!f.selection.commit(), "superseded commit rejected");
  take(f.selection.preview("blue"));
  take(f.selection.cancel());
  check(f.read().normal.foreground == RED, "fresh preview after takeover restores current external base");
  take(f.selection.preview("red"));

  auto base = take(f.managed->baseLayer(StyleTier::THEME));
  take(f.managed->removeLayer(base));
  check(take(f.selection.snapshot())->status == ui::ThemeSelectionStatus::SUPERSEDED && !f.selection.cancel(),
        "public permanent THEME removal also revokes preview atomically");
}

void factoryReentry() {
  SelectionFixture f;
  take(f.managed->theme(color(BLUE)));
  f.factoryHook = [&] {
    check(styleCause(f.selection.cancel().error(), TreeError::WOULD_BLOCK), "factory cannot cancel entered transition");
    check(styleCause(f.selection.snapshot().error(), TreeError::WOULD_BLOCK),
          "factory cannot expose partial transition");
    take(f.managed->theme(color(BLUE)));
  };

  const auto result = f.selection.preview("red");
  check(!result && styleCause(result.error(), TreeError::INVALID_ARGUMENT) && f.factoryCalls == 1,
        "equal legacy mutation invalidates captured revision with no factory retry");
  check(f.read().normal.foreground == BLUE && take(f.selection.snapshot())->status == ui::ThemeSelectionStatus::IDLE,
        "failed preview preserves independent external theme and old selection");
  take(f.managed->enableStyleLayers());

  std::optional<StyleLayerHandle> external;
  f.factoryHook = [&] {
    external = take(f.managed->addLayer(StyleTier::APPLICATION, "factory edit", color(BLUE, 23)));
  };

  const auto changedApplication = f.selection.preview("red");
  check(!changedApplication && styleCause(changedApplication.error(), TreeError::INVALID_ARGUMENT) &&
          f.factoryCalls == 2 && f.read().minWidth == 23,
        "factory application edit is preserved and candidate is not retried");
  take(f.managed->removeLayer(*external));
  f.factoryHook = [] {
    throw std::bad_alloc();
  };
  check(styleCause(f.selection.preview("red").error(), TreeError::NO_MEMORY), "factory allocation failure typed");
  f.factoryHook = [] {
    throw std::runtime_error("factory");
  };
  check(styleCause(f.selection.preview("red").error(), TreeError::CALLBACK_FAILED), "factory exception typed");
  f.factoryHook = [&] {
    take(f.scene.close());
  };
  check(!f.selection.preview("red"), "factory close cannot publish a candidate");
}

void admissionAndSingleModel() {
  ui::ThemeSelectionOptions options;
  options.layers = StyleLayerOptions{.maxLayers = 2};
  SelectionFixture f(options);

  const auto appearance = f.read();
  check(!f.selection.preview("red") && f.read() == appearance,
        "saved base and candidate both count against layer budget");
  check(take(f.selection.snapshot())->status == ui::ThemeSelectionStatus::IDLE, "failed admission remains idle");

  const auto first = take(f.selection.snapshot());
  ui::ThemeSelectionRef second;
  f.scene | ui::themeSelection(second, {{"other", "Other", [] {
                                           return color(RED);
                                         }}});
  check(!f.scene.checked() && styleCause(f.scene.checked().error(), TreeError::INVALID_ARGUMENT),
        "second model setup rejected");
  check(styleCause(second.snapshot().error(), TreeError::CLOSED) && first->catalog->choices.size() == 2 &&
          styleCause(f.selection.snapshot().error(), TreeError::CLOSED),
        "existing facade cleanup closes observers; owning first catalog survives unchanged");
}

void normalizationAndExactFactoryError() {
  ui::ThemeSelectionRef selection;
  ui::ThemeSelectionOptions options;

  std::size_t resolutions{};
  std::size_t factories{};
  options.textResolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return std::string(key == "theme.label" ? "Resolved label" : "Resolved description");
  };

  auto scene = ui::scene(ui::label("Catalog"));
  scene | ui::themeSelection(selection,
                             {{"checked", i18n("theme.label"),
                               [&]() -> std::expected<StyleSheet, StyleError> {
                                 ++factories;

                                 return std::unexpected(
                                   StyleError{TreeError::CALLBACK_FAILED, 17, 9, "Catalog factory rejected candidate"});
                               },
                               i18n("theme.description")}},
                             options);
  take(scene.prepareChecked());

  const auto normalized = take(selection.snapshot());
  check(resolutions == 2 && factories == 0 && normalized->catalog->choices[0]->label.bytes() == "Resolved label" &&
          normalized->catalog->choices[0]->description->bytes() == "Resolved description",
        "local resources normalize once at setup");

  const auto origin = std::source_location::current();
  const auto preview = selection.preview("checked", origin);
  const auto *failure = !preview ? std::get_if<StyleError>(&preview.error().cause) : nullptr;
  check(failure && failure->cause == TreeError::CALLBACK_FAILED && failure->line == 17 && failure->column == 9 &&
          std::string_view(failure->message) == "Catalog factory rejected candidate" &&
          preview.error().operationOrigin && preview.error().operationOrigin->line() == origin.line() && factories == 1,
        "exact factory error and origin survive without retry");
  check(take(selection.snapshot()) == normalized, "factory rejection retains owning model pointer");

  ui::ThemeSelectionRef duplicate;

  auto rejected = ui::scene(ui::label("Duplicate catalog"));
  rejected | ui::themeSelection(duplicate, {{"same", "First", nordTheme}, {"same", "Second", nordTheme}});

  const auto setup = rejected.prepareChecked();
  check(!setup && styleCause(setup.error(), TreeError::INVALID_ARGUMENT) &&
          styleCause(duplicate.snapshot().error(), TreeError::CLOSED),
        "invalid catalog never publishes observing reference");
}

void sharedTargetResolverReentry() {
  ui::ThemeSelectionRef shared;
  std::size_t outerFactories{};
  std::size_t innerFactories{};
  std::size_t resolutions{};
  bool innerSucceeded{};
  NodeId outerRoot;
  auto inner = ui::scene(ui::label("Inner scene"));
  inner | ui::themeSelection(shared, {{"inner", "Inner", [&] {
                                         ++innerFactories;

                                         return color(BLUE);
                                       }}});
  ui::ThemeSelectionOptions options;
  options.textResolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    innerSucceeded = bool(inner.prepareChecked());

    return std::string("Outer label");
  };
  auto outer = ui::scene(ui::label("Outer scene"));
  outer.configure([&](ManagedScene &scene) {
    outerRoot = scene.root().id();
  });
  const auto origin = std::source_location::current();
  outer | ui::themeSelection(shared,
                             {{"outer", i18n("outer.label"),
                               [&] {
                                 ++outerFactories;

                                 return color(RED);
                               }}},
                             options, origin);

  const auto prepared = outer.prepareChecked();

  check(resolutions == 1 && innerSucceeded, "resolver independently prepares the inner scene once");
  check(!prepared && styleCause(prepared.error(), TreeError::INVALID_ARGUMENT) && prepared.error().node == outerRoot &&
          prepared.error().operationOrigin && prepared.error().operationOrigin->line() == origin.line(),
        "outer installer rejects a target published during normalization with exact admission failure");
  const auto surviving = take(shared.snapshot());

  check(surviving->catalog->choices.size() == 1 && surviving->catalog->choices[0]->id == "inner" &&
          surviving->status == ui::ThemeSelectionStatus::IDLE,
        "failed outer cleanup preserves the independently published inner catalog");
  take(shared.preview("inner"));
  take(shared.commit());
  check(innerFactories == 1 && outerFactories == 0 && take(shared.snapshot())->lastCommittedId == "inner" &&
          bool(inner.checked()),
        "shared commands keep controlling the inner scene after outer rejection and cleanup");
}

void allocationAtomicity() {
  enum class Transition {
    BEGIN,
    UPDATE,
    COMMIT,
    CANCEL
  };

  for (const auto transition : {Transition::BEGIN, Transition::UPDATE, Transition::COMMIT, Transition::CANCEL}) {
    bool completed{};

    for (std::size_t point = 0; point < 4096; ++point) {
      SelectionFixture f;
      take(f.managed->theme(color(BLUE, 19)));

      if (transition != Transition::BEGIN) {
        take(f.selection.preview("red"));
      }

      const auto before = take(f.selection.snapshot());
      const auto appearance = f.read();
      const auto revision = f.revision();
      allocationTest::remaining = static_cast<std::ptrdiff_t>(point);

      const auto result = transition == Transition::BEGIN    ? f.selection.preview("red")
                          : transition == Transition::UPDATE ? f.selection.preview("blue")
                          : transition == Transition::COMMIT ? f.selection.commit()
                                                             : f.selection.cancel();
      allocationTest::remaining = -1;

      if (result) {
        completed = true;
        std::cout << "theme transition=" << static_cast<int>(transition) << " OOM points=" << point << '\n';

        break;
      }

      check(styleCause(result.error(), TreeError::NO_MEMORY), "transition allocation failure remains exact");
      check(take(f.selection.snapshot()) == before && f.read() == appearance && f.revision() == revision,
            "failed publication preserves owning selection, computed styles and revision");

      if (transition != Transition::BEGIN) {
        take(f.selection.cancel());
        check(f.read().normal.foreground == BLUE && f.read().minWidth == 19,
              "failed transition retains restorable base");
      }
    }

    check(completed, "bounded allocation sweep reached success");
  }
}
} // namespace

int main() try {
  lifecycle();
  takeoverAndOverrides();
  factoryReentry();
  admissionAndSingleModel();
  normalizationAndExactFactoryError();
  sharedTargetResolverReentry();
  allocationAtomicity();

  return 0;
} catch (const std::exception &failure) {
  allocationTest::remaining = -1;
  std::cerr << failure.what() << '\n';

  return 1;
}
