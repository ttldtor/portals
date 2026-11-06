// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0 AND MIT
// Portals implementation and test logic: BSL-1.0.
// Adapted Textual palette values: Copyright (c) 2021 Will McGugan, MIT.
// See vendor/textual/LICENSE and README.md for the retained notice and provenance.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <source_location>
#include <thread>
#include <type_traits>

using namespace portals;

static_assert(std::derived_from<StatusBar, Row>);
static_assert(!std::derived_from<StatusBar, Control>);

namespace {
template <class T, class E>
T take(std::expected<T, E> value, std::source_location origin = std::source_location::current()) {
  INFO("API call at line " << origin.line());
  if constexpr (std::same_as<E, ManagedError>) {
    if (!value) {
      const auto *cause = std::get_if<TreeError>(&value.error().cause);
      const auto resource = value.error().sceneResource;

      INFO("TreeError " << (cause ? static_cast<int>(*cause) : -1));
      INFO("ResourceError " << (resource ? static_cast<int>(resource->code) : -1));
      INFO("Property " << (resource && resource->propertyPath ? *resource->propertyPath : ""));
      REQUIRE(value.has_value());
    }
  }

  REQUIRE(value.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));

  void layout(CellSize size = {40, 6}) {
    take(scene->prepare(*app, size));

    const auto bounds = take(CellRect::from({}, size));

    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
    }));
    take(scene->completedLayout(*app));
  }
};

struct Sink : PaintContext {
  std::vector<FillCommand> fills;
  bool fail{};

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    if (fail) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    for (const auto &command : commands) {
      if (const auto *fill = std::get_if<FillCommand>(&command)) {
        fills.push_back(*fill);
      }
    }

    return {};
  }
};

FillCommand fill(Fixture &f, Node<StatusBar> bar) {
  Sink sink;

  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(bar, sink));
  }));
  REQUIRE(sink.fills.size() == 1);

  return sink.fills.front();
}
} // namespace

TEST_CASE("status bar fills empty, padded and unused cells with its theme") {
  Fixture f;
  ui::Ref<StatusBar> empty, sections;
  ui::Ref<Label> message, right;

  take(ui::mount(*f.scene,
                 ui::column(ui::statusBar(empty),
                            ui::statusBar(sections, {.gap = 2, .padding = {.left = 1, .right = 1}},
                                          ui::label(message, "Ready"), ui::spacer(), ui::label(right, "UTF-8")))));
  take(f.scene->theme(nordTheme()));
  f.layout();

  CHECK(take(empty.geometry()).local.size() == CellSize{40, 1});
  CHECK(take(sections.geometry()).local.size() == CellSize{40, 1});
  CHECK(take(right.geometry()).local.origin().x == 34);
  CHECK(take(message.geometry()).local.origin().x == 1);

  const auto painted = fill(f, sections.node());

  CHECK(painted.area == take(CellRect::from({}, {40, 1})));
  CHECK(painted.clip == painted.area);
  CHECK(painted.style.background == RgbColor{0x2e, 0x34, 0x40});
  CHECK(fill(f, empty.node()).style == painted.style);
}

TEST_CASE("status bar inherits sizing and hidden-section allocation without another solver") {
  Fixture f;
  ui::Ref<StatusBar> bar;
  ui::Ref<Label> first, hidden, last;

  take(ui::mount(*f.scene, ui::statusBar(bar, {.gap = 2}, ui::label(first, "abc"),
                                         ui::label(hidden, "hidden").visible(false), ui::label(last, "xy"))
                             .minWidth(8)
                             .maxWidth(12)
                             .height(2)));
  f.layout();
  CHECK(take(bar.geometry()).local.size() == CellSize{12, 2});
  CHECK(take(last.geometry()).local.origin().x == 5);

  take(setLinearOptions(f.app->tree(), bar.node(), {.gap = 1}));
  take(setSizePolicy(f.app->tree(), bar.node(), DEFAULT_STATUS_BAR_POLICY));
  f.layout({4, 3});
  CHECK(take(bar.geometry()).local.size() == CellSize{4, 1});
  CHECK(take(last.geometry()).local.size().width == 0);
  f.layout({0, 3});

  Sink sink;

  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(bar.node(), sink));
  }));
  CHECK(sink.fills.empty());
}

TEST_CASE("managed status disabled self and ancestry synchronize fill and child input") {
  Fixture f;
  ui::Ref<Column> parent;
  ui::Ref<StatusBar> bar;
  ui::Ref<Button> button;
  int clicked{};

  take(ui::mount(*f.scene, ui::column(ui::statusBar(bar, ui::button("Act").reference(button).onClick([&] {
                             ++clicked;
                           })))
                             .reference(parent)));
  take(f.scene->theme(nordTheme()));
  f.layout();

  const auto normal = fill(f, bar.node()).style;

  take(f.app->tree().setEnabled(parent.node(), false));
  f.layout();
  CHECK(fill(f, bar.node()).style.foreground == RgbColor{0x81, 0xa1, 0xc1});
  CHECK_FALSE(take(f.app->tree().inputEligible(button.node())));
  take(button.activate().result());
  CHECK(clicked == 0);
  take(f.app->tree().setEnabled(parent.node(), true));
  f.layout();
  CHECK(fill(f, bar.node()).style == normal);
  take(button.activate().result());
  CHECK(clicked == 1);
  CHECK(take(f.app->router().focused()) != bar.node());

  take(f.app->tree().setEnabled(bar.node(), false));
  f.layout();
  CHECK(fill(f, bar.node()).style != normal);
  take(f.app->tree().setVisible(parent.node(), false));
  Sink sink;

  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(bar.node(), sink));
  }));
  CHECK(sink.fills.empty());
}

TEST_CASE("status bar low-level sync and paint preserve checked errors and snapshot") {
  Fixture f;
  const ControlStyle styles{.normal = {.bold = true}, .disabled = {.italic = true}};
  const auto bar = take(f.app->tree().emplace<StatusBar>(f.scene->root(), LinearOptions{}, styles));

  f.layout();
  take(f.app->tree().setEnabled(bar, false));

  TreeError reentry{};
  take(f.app->tree().frame([&] {
    const auto result = syncStatusBar(f.app->tree(), bar);

    REQUIRE_FALSE(result);
    reentry = result.error();
  }));
  CHECK(reentry == TreeError::WOULD_BLOCK);
  CHECK(fill(f, bar).style.bold);
  take(syncStatusBar(f.app->tree(), bar));
  CHECK(fill(f, bar).style.italic);

  Sink sink;
  sink.fail = true;
  take(f.app->tree().frame([&] {
    const auto result = f.app->tree().paint(bar, sink);

    REQUIRE_FALSE(result);
    CHECK(result.error() == TreeError::WOULD_BLOCK);
  }));
  std::expected<void, TreeError> worker;
  std::thread thread([&] {
    worker = syncStatusBar(f.app->tree(), bar);
  });

  thread.join();
  REQUIRE_FALSE(worker);
  CHECK(worker.error() == TreeError::WRONG_THREAD);
  take(f.app->tree().remove(bar));
  CHECK(syncStatusBar(f.app->tree(), bar).error() == TreeError::STALE_NODE);
}

TEST_CASE("status Label ref remains dynamic while failed children roll back all refs") {
  Fixture f;
  ui::Ref<StatusBar> bar;
  ui::Ref<Label> label;

  std::array descriptions{ui::label(label, "Ready")};
  take(ui::mount(*f.scene, ui::statusBar(std::move(descriptions)).reference(bar)));
  label.text() = "Saved e\xcc\x81 \xe7\x95\x8c";
  CHECK(take(label.text().get()) == "Saved e\xcc\x81 \xe7\x95\x8c");
  f.layout();
  take(f.scene->remove(bar.node()));
  CHECK_FALSE(label.text().get());

  ui::Ref<StatusBar> failed;
  ui::Ref<Label> unpublished;
  const auto before = take(f.app->tree().inspect(f.scene->root())).children.size();
  auto result = ui::mount(*f.scene, ui::statusBar(failed, ui::label(unpublished, "ok"), ui::label("\xff")));

  REQUIRE_FALSE(result);
  CHECK(failed.node() == Node<StatusBar>{});
  CHECK(unpublished.node() == Node<Label>{});
  CHECK(take(f.app->tree().inspect(f.scene->root())).children.size() == before);
}

TEST_CASE("status canonical variants and loaded layouts share properties refs and styles") {
  for (const int mode : {0, 1, 2}) {
    CAPTURE(mode);
    ui::ControlContext controls;

    controls.defaults("StatusBar", {.gap = 2}).define("Footer", "StatusBar", {.height = 2});

    ui::SceneResourceOptions options;
    options.controls = controls;
    options.format = mode == 2 ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    const std::string body = "Footer#bar{gap=0 " +
                             std::string(mode == 1 ? "width=12 " : "minWidth=8 maxWidth=12 padding{left=1} ") +
                             R"(Label#message{text="Ready"} Spacer{} Label{text="UTF-8"}})";
    auto owner = mode != 0 ? ui::scene(mode == 2 ? "scene{" + body + "}" : body, options)
                           : ui::scene(ui::statusBar(ui::label("Ready").id("message"), ui::spacer(), ui::label("UTF-8"))
                                         .variant("Footer")
                                         .id("bar")
                                         .settings({.gap = 0, .grid = {.paddingLeft = 1}})
                                         .minWidth(8)
                                         .maxWidth(12),
                                       {.controls = controls});
    auto bar = owner.get<StatusBar>("bar");
    auto label = owner.get<Label>("message");

    owner.configure([&](ManagedScene &scene, Application &app) {
      take(scene.theme(nordTheme()));
      take(scene.prepare(app, {30, 5}));

      const auto bounds = take(CellRect::from({}, {30, 5}));

      take(app.tree().frame([&] {
        take(layoutTree(app.tree(), app.root(), bounds, bounds));
      }));
      take(scene.completedLayout(app));
      CHECK(take(bar.geometry()).local.size() == CellSize{12, 2});
      take(app.tree().read(bar.node(), [&](const StatusBar &widget) {
        CHECK(widget.options().gap == 0);
        CHECK(widget.options().padding.left == (mode == 1 ? 0 : 1));
        REQUIRE(widget.computedStyle());
        CHECK(widget.computedStyle()->normal.background == RgbColor{0x2e, 0x34, 0x40});
      }));
      label.text() = "Updated";
      CHECK(take(label.text().get()) == "Updated");
    });
    take(owner.prepareChecked());

    auto wrong = owner.get<Button>("bar");

    CHECK_FALSE(wrong.geometry());
    take(owner.close());
    CHECK_FALSE(label.text().get());
  }

  auto invalid = ui::scene(R"(StatusBar{text="unsupported"})");

  CHECK_FALSE(invalid.prepareChecked());
}

TEST_CASE("typed legacy bounds retain the existing common origin error") {
  for (const auto type : {"HBox", "StatusBar"}) {
    for (const auto bound : {"minWidth", "maxWidth"}) {
      CAPTURE(type);
      CAPTURE(bound);
      ui::ControlContext controls;

      controls.defaults("HBox", {.gap = 1});

      const std::string source = std::string(type) + "{" + bound + "=8}";
      auto owner = ui::scene(source, {.controls = controls});
      const auto result = owner.prepareChecked();

      REQUIRE_FALSE(result);
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == SceneResourceErrorCode::DUPLICATE_PROPERTY);
      REQUIRE(result.error().sceneResource->propertyPath);
      CHECK(*result.error().sceneResource->propertyPath == std::string(type) + "." + bound);
      REQUIRE(std::holds_alternative<TreeError>(result.error().cause));
      CHECK(std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT);
    }
  }
}
