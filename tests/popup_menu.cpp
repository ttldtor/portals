// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/managed_text_fixture.hpp"
#include <portals/ui_style.hpp>

namespace {
static_assert(std::is_aggregate_v<PopupEntry>);
static_assert(!noexcept(PopupEntry{}));
static_assert(!noexcept(portals::detail::emptyPopupEntries()));

struct PopupFixture : Fixture {
  ui::Ref<Button> opener;
  ui::Ref<PopupMenu> menu;
  ui::Ref<OverlayHost> host;

  void mount(std::initializer_list<PopupEntry> entries, PopupMenuOptions options = {}) {
    take(ui::mount(*scene, ui::overlayHost(host, ui::vbox(ui::button(opener, "Open").onClick([] {
                                           })),
                                           ui::popupMenu(menu, entries, options))));
    layout();
    take(scene->focus(opener.node()));
  }

  void layout(CellSize size = {60, 16}) {
    std::optional<LayoutSnapshot> snapshot;

    take(scene->prepare(*app, size));
    take(app->tree().frame([&] {
      const auto rectangle = take(CellRect::from({}, size));

      take(layoutTree(app->tree(), take(app->tree().root()), rectangle, rectangle));
      snapshot = take(LayoutSnapshot::capture(app->tree(), scene->root(), LayoutSnapshotProfile::GEOMETRY));
    }));
    take(scene->prepareViews(*app, *snapshot));
    take(scene->completedLayout(*app));
  }

  void open() {
    take(menu.open(opener).result());
    layout();
  }

  void key(InputKey key, char32_t scalar = 0, KeyAction phase = KeyAction::PRESS) {
    take(app->router().dispatch(KeyEvent{key, scalar, {}, phase}));
  }

  std::size_t panels() {
    std::size_t count{};

    take(app->tree().read(menu.node(), [&](const PopupMenu &popup) {
      count = popup.panelCount();
    }));

    return count;
  }

  std::size_t bytes(std::string_view bytes) {
    auto decoder = take(InputDecoder::create());
    std::size_t consumed{}, events{};

    for (std::size_t pulls = 0; pulls < bytes.size() * 3 + 4; ++pulls) {
      auto step = take(decoder.next(bytes.substr(consumed), 0));

      consumed += step.consumed;

      if (step.event) {
        ++events;
        take(app->router().dispatch(*step.event));
      }

      if (consumed == bytes.size() && step.status == InputStatus::NEED_INPUT) {
        return events;
      }
    }

    throw std::runtime_error("finite decoder did not drain");
  }
};

struct PopupPaintCapture : PaintContext {
  std::vector<CellRect> fills = std::vector<CellRect>(0);
  std::vector<CellStyle> fillStyles = std::vector<CellStyle>(0);
  std::vector<std::string> text = std::vector<std::string>(0);

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *fill = std::get_if<FillCommand>(&command)) {
        fills.push_back(fill->area);
        fillStyles.push_back(fill->style);
      } else if (const auto *glyph = std::get_if<TextCommand>(&command)) {
        text.emplace_back(glyph->text.bytes());
      }
    }

    return {};
  }
};
} // namespace

TEST_CASE("popup closes and restores opener focus before invoking shared leaf") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Save", [&] {
    ++calls;
    CHECK_FALSE(take(fixture.menu.opened()));
    CHECK(take(fixture.app->router().focused()) == Node<Widget>(fixture.opener.node()));
  });

  fixture.mount({ui::menuItem(leaf, U's')});
  fixture.open();
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
  CHECK_FALSE(take(fixture.menu.opened()));
}

TEST_CASE("popup opens branches and Escape first closes deepest panel") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Leaf", [&] {
    ++calls;
  });

  fixture.mount({ui::submenu("Branch", {ui::menuItem(leaf)}, U'b')});
  fixture.open();
  fixture.key(InputKey::RIGHT);
  CHECK(fixture.panels() == 2);
  fixture.layout();
  fixture.key(InputKey::ESCAPE);
  CHECK(fixture.panels() == 1);
  CHECK(take(fixture.menu.opened()));
  fixture.layout();
  fixture.key(InputKey::ESCAPE);
  CHECK_FALSE(take(fixture.menu.opened()));
  CHECK(calls == 0);
}

TEST_CASE("popup mnemonic uses explicit ASCII fold and ignores following TextInput and repeats") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Literal &Save", [&] {
    ++calls;
  });

  fixture.mount({ui::menuItem(leaf, U's')});
  fixture.open();
  fixture.key(InputKey::CHARACTER, U'S', KeyAction::REPEAT);
  CHECK(calls == 0);
  fixture.key(InputKey::CHARACTER, U'S');
  take(fixture.app->router().dispatch(TextInputEvent{U'S'}));
  CHECK(calls == 1);
}

TEST_CASE("popup duplicate mnemonics select without invoking and disabled actions are skipped") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto first = actions.action("first", "First", [&] {
    ++calls;
  });
  auto second = actions.action("second", "Second", [&] {
    ++calls;
  });

  fixture.mount({ui::menuItem(first, U'x'), ui::menuItem(second, U'x')});
  fixture.open();
  fixture.key(InputKey::CHARACTER, U'x');
  CHECK(calls == 0);
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
}

TEST_CASE("legacy VT bytes emit one mnemonic event per scalar through branch and shared leaf") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Save as", [&] {
    ++calls;
  });

  fixture.mount({ui::submenu("More", {ui::menuItem(leaf, U'a')}, U'm')});
  fixture.open();
  CHECK(fixture.bytes("m") == 1);
  CHECK(fixture.panels() == 2);
  CHECK(calls == 0);
  fixture.layout();
  CHECK(fixture.bytes("a") == 1);
  CHECK(calls == 1);
  CHECK_FALSE(take(fixture.menu.opened()));
}

TEST_CASE("popup scene facade hosts typed and code-bound loaded layers without host boilerplate") {
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Leaf", [] {
  });
  ui::Ref<PopupMenu> typed;
  auto scene = ui::scene(ui::label("Body"), ui::popupMenu(typed, {ui::menuItem(leaf)}).maxWidth(24));

  REQUIRE(scene.prepareChecked());
  CHECK(typed.node() != Node<PopupMenu>{});
  CHECK_FALSE(take(typed.opened()));
  REQUIRE(scene.close());

  ui::ActionSet loadedActions;
  auto loadedLeaf = loadedActions.action("loaded", "Loaded leaf", [] {
  });
  ui::Ref<PopupMenu> loaded;
  ui::SceneResourceOptions options;

  CHECK(&options.popupMenu("FileMenu", loaded, {ui::menuItem(loadedLeaf)}) == &options);

  auto loadedScene =
    ui::scene(ui::label("Body"),
              ui::resource("FileMenu#loaded-menu{classes=\"loaded-accent\" fitWidth=true maxWidth=24}", options)) |
    ui::style("PopupMenu#loaded-menu.loaded-accent{foreground:#123456;}");
  auto named = loadedScene.get<PopupMenu>("loaded-menu");
  ManagedScene *managed{};
  Application *application{};

  loadedScene.configure([&](ManagedScene &scene, Application &app) {
    managed = &scene;
    application = &app;
  });

  REQUIRE(loadedScene.prepareChecked());
  CHECK(loaded.node() != Node<PopupMenu>{});
  CHECK(named.node() == loaded.node());
  take(managed->prepare(*application, {40, 12}));
  take(application->tree().read(named.node(), [](const PopupMenu &popup) {
    REQUIRE(popup.computedStyle());
    CHECK(popup.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56});
  }));
  CHECK_FALSE(take(loaded.opened()));
  REQUIRE(loadedScene.close());
}

TEST_CASE("popup invocation pins callback while removing its shared action and logical menu") {
  PopupFixture fixture;
  ui::ActionSet actions;
  ui::ActionRef action;
  auto capture = std::make_shared<int>(7);
  std::weak_ptr<int> weak = capture;
  Node<Dialog> root;
  int calls{};

  action = actions.action("self", "Self", [&, capture] {
    ++calls;
    REQUIRE(action.remove());
    take(fixture.scene->remove(fixture.menu.node()));
    CHECK_FALSE(weak.expired());
  });
  capture.reset();
  fixture.mount({ui::menuItem(action)});
  fixture.open();
  take(fixture.app->tree().read(fixture.menu.node(), [&](const PopupMenu &menu) {
    root = menu.dialogRoot();
  }));
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
  CHECK(weak.expired());
  CHECK_FALSE(fixture.app->tree().inspect(root));
}

TEST_CASE("popup leaf may destroy its scene after checked dismissal") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("close", "Close", [&] {
    fixture.scene.reset();
  });

  fixture.mount({ui::menuItem(leaf)});
  fixture.open();
  fixture.key(InputKey::ENTER);
  CHECK_FALSE(fixture.scene);
  CHECK(leaf.snapshot().error() == TreeError::CLOSED);
}

TEST_CASE("popup does not invoke leaf when focus return callback fails") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Leaf", [&] {
    ++calls;
  });

  fixture.mount({ui::menuItem(leaf)});
  const auto body = take(fixture.app->tree().inspect(fixture.opener.node())).parent;
  auto target = take(fixture.scene->emplace<Widget>(body));
  bool failReturn{};
  auto binding = take(fixture.app->router().bind(
    target,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool active) -> std::expected<void, TreeError> {
      return active && failReturn ? std::unexpected(TreeError::CALLBACK_FAILED) : std::expected<void, TreeError>{};
    }));

  fixture.layout();
  take(fixture.app->router().setFocus(target));
  fixture.layout();
  fixture.open();
  failReturn = true;
  fixture.key(InputKey::ENTER);
  CHECK(calls == 0);
  CHECK_FALSE(take(fixture.menu.opened()));
}

TEST_CASE("popup admission checks total tree and shared presenter budgets") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Leaf", [] {
  });
  auto limited = ui::mount(
    *fixture.scene, ui::overlayHost(ui::label("Body"),
                                    ui::popupMenu({ui::submenu("Nested", {ui::menuItem(leaf)})}, {.maxEntries = 1})));

  CHECK_FALSE(limited);
  CHECK(treeCause(limited.error(), TreeError::NODE_LIMIT));
}

TEST_CASE("popup tiny resize clips safely and keeps navigation available") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Wide 日本語", [] {
  });

  fixture.mount({ui::submenu("Branch", {ui::menuItem(leaf)})});
  fixture.open();
  fixture.key(InputKey::RIGHT);
  fixture.layout({8, 4});
  CHECK(fixture.panels() == 2);
  fixture.key(InputKey::LEFT);
  fixture.layout({1, 1});
  CHECK(fixture.panels() == 1);
}

TEST_CASE("popup hidden or disabled logical owner revokes activation and rejects reopening") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Leaf", [&] {
    ++calls;
  });

  fixture.mount({ui::menuItem(leaf)});
  fixture.open();
  take(fixture.app->tree().setVisible(fixture.menu.node(), false));
  CHECK_FALSE(take(fixture.menu.opened()));
  CHECK_FALSE(fixture.menu.open(fixture.opener));
  fixture.key(InputKey::ENTER);
  CHECK(calls == 0);
}

TEST_CASE("loaded popup retains factory-local cold action declarations and rejects unknown scalar fields") {
  ui::Ref<PopupMenu> loaded;
  auto resources = [&] {
    ui::ActionSet local;
    auto leaf = local.action("local", "Local", [] {
    });

    return ui::SceneResourceOptions{}.popupMenu("LocalMenu", loaded, {ui::menuItem(leaf)});
  }();
  auto scene = ui::scene(ui::label("Body"), ui::resource("LocalMenu{maxWidth=24}", resources));

  REQUIRE(scene.prepareChecked());
  CHECK(loaded.node() != Node<PopupMenu>{});
  REQUIRE(scene.close());

  auto invalid = ui::scene(ui::label("Body"), ui::resource("LocalMenu{readOnly=true}", resources));
  auto failure = invalid.prepareChecked();

  REQUIRE_FALSE(failure);
  REQUIRE(failure.error().sceneResource);
  CHECK(failure.error().sceneResource->code == SceneResourceErrorCode::UNKNOWN_PROPERTY);
}

TEST_CASE("popup reuses rounded frame painter and preserves transparent panel gaps") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Leaf", [] {
  });
  PopupMenuOptions options;

  options.frame.pattern = FramePattern::ROUNDED;
  fixture.mount({ui::submenu("More", {ui::menuItem(leaf)})}, options);
  fixture.open();
  fixture.key(InputKey::RIGHT);
  fixture.layout();

  PopupPaintCapture capture;

  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(fixture.menu.node(), capture));
  }));

  REQUIRE(capture.fills.size() >= 2);

  const CellPoint gap{capture.fills.front().right(), 1};

  CHECK_FALSE(std::ranges::any_of(capture.fills, [&](auto rectangle) {
    return rectangle.contains(gap);
  }));
  CHECK(std::ranges::find(capture.text, "╭") != capture.text.end());
}

TEST_CASE("popup plane cap rejects fixed and PSS minima contradictions with typed errors") {
  for (const bool pss : {false, true}) {
    PopupFixture fixture;
    ui::ActionSet actions;
    auto leaf = actions.action("leaf", "Leaf", [] {
    });
    auto recipe = ui::popupMenu(fixture.menu, {ui::menuItem(leaf)});

    if (!pss) {
      recipe = std::move(recipe).width(61);
    }

    take(ui::mount(*fixture.scene, ui::overlayHost(fixture.host, ui::button(fixture.opener, "Open").onClick([] {
    }),
                                                   std::move(recipe))));

    if (pss) {
      take(fixture.scene->theme(take(parsePss("PopupMenu{min-width:61;}"))));
    }

    fixture.layout({100, 20});
    take(fixture.menu.open(fixture.opener).result());
    take(fixture.scene->prepare(*fixture.app, {100, 20}));

    std::expected<CellSize, TreeError> measured;

    take(fixture.app->tree().frame([&] {
      measured = fixture.app->tree().measure(fixture.menu.node(), {100, 20});
    }));

    REQUIRE_FALSE(measured);
    CHECK(measured.error() == TreeError::INVALID_GEOMETRY);
  }
}

TEST_CASE("popup submenu flips left at the actual right viewport edge and remains bounded after resize") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Leaf", [] {
  });

  take(ui::mount(
    *fixture.scene,
    ui::overlayHost(fixture.host,
                    ui::overlay(OverlayOptions{.horizontal = LayoutAlign::END, .vertical = LayoutAlign::START},
                                ui::button(fixture.opener, "Open")
                                  .onClick([] {
                                  })
                                  .width(8)
                                  .height(1)),
                    ui::popupMenu(fixture.menu, {ui::submenu("More", {ui::menuItem(leaf)})}))));
  fixture.layout({40, 8});
  take(fixture.scene->focus(fixture.opener.node()));
  take(fixture.menu.open(fixture.opener).result());
  fixture.layout({40, 8});
  fixture.key(InputKey::RIGHT);
  fixture.layout({40, 8});

  PopupPaintCapture capture;

  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(fixture.menu.node(), capture));
  }));

  std::vector<CellRect> panels(0);

  for (auto rectangle : capture.fills) {
    if (rectangle.size().height > 1) {
      panels.push_back(rectangle);
    }
  }

  REQUIRE(panels.size() == 2);
  CHECK(panels[1].right() < panels[0].origin().x);
  fixture.layout({24, 5});
  CHECK(fixture.panels() == 2);
}

TEST_CASE("popup bottom frame never activates a clipped leaf") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Leaf", [&] {
    ++calls;
  });
  std::vector<PopupEntry> rows(25, ui::menuItem(leaf));
  auto recipe = ui::detail::popupMenuRecipe(rows, {}, {}, DEFAULT_MENU_POLICY).reference(fixture.menu);

  take(ui::mount(*fixture.scene, ui::overlayHost(fixture.host, ui::button(fixture.opener, "Open").onClick([] {
  }),
                                                 std::move(recipe))));
  fixture.layout({60, 24});
  take(fixture.scene->focus(fixture.opener.node()));
  take(fixture.menu.open(fixture.opener).result());
  fixture.layout({60, 24});

  const auto bounds = take(fixture.scene->geometry(fixture.menu.node())).surface;
  MouseInputEvent click;

  click.column = bounds.origin().x + 1;
  click.row = bounds.bottom() - 1;
  click.button = MouseButton::LEFT;
  click.action = MouseAction::PRESS;
  take(fixture.app->router().dispatch(click));
  CHECK(calls == 0);
  CHECK(take(fixture.menu.opened()));
}

TEST_CASE("popup preserves selected branch identity and coherent child path when an earlier sibling hides") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calledB{}, calledC{};
  auto a = actions.action("a", "A", [] {
  });
  auto b = actions.action("b", "B leaf", [&] {
    ++calledB;
  });
  auto c = actions.action("c", "C leaf", [&] {
    ++calledC;
  });

  fixture.mount({ui::menuItem(a), ui::submenu("B", {ui::menuItem(b)}), ui::submenu("C", {ui::menuItem(c)})});
  fixture.open();
  fixture.key(InputKey::DOWN);
  fixture.key(InputKey::RIGHT);
  fixture.layout();

  auto metadata = **a.snapshot();

  metadata.visible = false;
  REQUIRE(a.set(metadata));
  fixture.layout();
  CHECK(fixture.panels() == 2);
  fixture.key(InputKey::LEFT);

  std::optional<std::size_t> selected;

  take(fixture.app->tree().read(fixture.menu.node(), [&](const PopupMenu &menu) {
    selected = menu.selectedIndex();
  }));
  CHECK(selected == 1);
  fixture.key(InputKey::RIGHT);
  fixture.key(InputKey::ENTER);
  CHECK(calledB == 1);
  CHECK(calledC == 0);
}

TEST_CASE("popup selected index remains the original ordinal after a preceding leaf hides") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto first = actions.action("first", "First", [] {
  });
  auto second = actions.action("second", "Second", [] {
  });

  fixture.mount({ui::menuItem(first), ui::menuItem(second)});
  fixture.open();
  fixture.key(InputKey::DOWN);

  auto metadata = **first.snapshot();

  metadata.visible = false;
  REQUIRE(first.set(metadata));
  fixture.layout();

  std::optional<std::size_t> selected;

  take(fixture.app->tree().read(fixture.menu.node(), [&](const PopupMenu &menu) {
    selected = menu.selectedIndex();
  }));
  CHECK(selected == 1);
}

TEST_CASE("reported Escape repeat and release do not dismiss additional popup depths") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Leaf", [] {
  });

  fixture.mount({ui::submenu("More", {ui::menuItem(leaf)})});
  fixture.open();
  fixture.key(InputKey::RIGHT);
  fixture.key(InputKey::ESCAPE, 0, KeyAction::REPEAT);
  CHECK(fixture.panels() == 2);
  fixture.key(InputKey::ESCAPE, 0, KeyAction::RELEASE);
  CHECK(fixture.panels() == 2);
  fixture.key(InputKey::ESCAPE);
  CHECK(fixture.panels() == 1);
  fixture.key(InputKey::ESCAPE, 0, KeyAction::REPEAT);
  CHECK(take(fixture.menu.opened()));
}

TEST_CASE("popup accepts decoded branch and leaf mnemonics before the first open presentation") {
  PopupFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto leaf = actions.action("leaf", "Leaf", [&] {
    ++calls;
  });

  fixture.mount({ui::submenu("More", {ui::menuItem(leaf, U'a')}, U'm')});
  take(fixture.menu.open(fixture.opener).result());
  CHECK(fixture.bytes("m") == 1);
  CHECK(fixture.bytes("a") == 1);
  CHECK(calls == 1);
  CHECK_FALSE(take(fixture.menu.opened()));
}

TEST_CASE("popup allocation failure during metadata preparation preserves the published selection and path") {
  PopupFixture fixture;
  ui::ActionSet actions;
  auto leaf = actions.action("leaf", "Leaf", [] {
  });

  fixture.mount({ui::submenu("More", {ui::menuItem(leaf)})});
  fixture.open();
  fixture.key(InputKey::RIGHT);
  fixture.layout();

  auto metadata = **leaf.snapshot();

  metadata.caption = "Changed caption";
  REQUIRE(leaf.set(metadata));

  struct FailAllocation {
    FailAllocation() {
      allocationTest::remaining = 0;
    }

    ~FailAllocation() {
      allocationTest::remaining = -1;
    }
  };

  std::expected<void, TreeError> prepared;

  {
    FailAllocation fail;

    prepared = fixture.scene->prepare(*fixture.app, {60, 16});
  }

  REQUIRE_FALSE(prepared);
  CHECK(prepared.error() == TreeError::NO_MEMORY);
  CHECK(fixture.panels() == 2);
  fixture.layout();
  CHECK(fixture.panels() == 2);
}

TEST_CASE("fluent popup registration retains first errors before factories files and terminal") {
  for (const auto name : {"Label", "bad-name"}) {
    int created{}, captured{}, notified{};
    ui::Ref<PopupMenu> popup;
    ui::SceneResourceOptions options;

    options.control<Label>("Probe", [&](ui::SceneResourceArguments) {
      ++created;

      return ui::label("unused");
    });
    options.popupMenu(name, popup, {});

    const auto expected =
      name == std::string_view("Label") ? SceneResourceErrorCode::DUPLICATE_TYPE : SceneResourceErrorCode::UNKNOWN_TYPE;

    struct LaterFactory {
      int *copies;

      explicit LaterFactory(int &counter) : copies(&counter) {
      }

      LaterFactory(const LaterFactory &other) : copies(other.copies) {
        ++*copies;
      }

      ui::Description<Label> operator()(ui::SceneResourceArguments) {
        return ui::label("unused");
      }
    } later(captured);

    options.control<Label>("Later", later).popupMenu("Label", popup, {});
    CHECK(captured == 0);

    auto copy = options;
    auto owner = ui::scene("Probe{}", std::move(copy));
    const auto prepared = owner.prepareChecked();

    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    CHECK(prepared.error().sceneResource->code == expected);
    CHECK(std::get<TreeError>(prepared.error().cause) == TreeError::INVALID_ARGUMENT);
    CHECK(created == 0);

    auto fileOwner = ui::scene(ui::SceneResourceFile{"not-read-after-popup-registration-failure.scene"}, options);

    fileOwner.onError([&](const ManagedDiagnostic &diagnostic) {
      ++notified;
      REQUIRE(diagnostic.error.sceneResource);
      CHECK(diagnostic.error.sceneResource->code == expected);
      CHECK(diagnostic.error.sceneResource->line == 0);
    });

    example::ReplayTerminal terminal;

    CHECK(fileOwner.runApp(terminal) == 1);
    CHECK(notified == 1);
    CHECK(created == 0);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);
  }
}

TEST_CASE("fluent popup option copies collect allocation failure for lvalues and rvalues") {
  for (const bool temporary : {false, true}) {
    ui::ActionSet actions;
    auto leaf = actions.action("leaf", "Leaf", [] {
    });
    ui::Ref<PopupMenu> reference;
    const std::initializer_list<PopupEntry> entries{ui::menuItem(leaf)};
    const PopupMenuOptions popupOptions = [] {
      PopupMenuOptions options;

      options.frame.title = std::string(256, 'T');

      return options;
    }();
    const std::string_view name = "LoadedPopup";
    ui::SceneResourceOptions resources;

    static_assert(noexcept(resources.popupMenu(name, reference, entries, popupOptions)));
    static_assert(noexcept(std::move(resources).popupMenu(name, reference, entries, popupOptions)));

    struct FailNextAllocation {
      FailNextAllocation() {
        allocationTest::remaining = 0;
      }

      ~FailNextAllocation() {
        allocationTest::remaining = -1;
      }
    };

    {
      FailNextAllocation fail;

      if (temporary) {
        std::move(resources).popupMenu(name, reference, entries, popupOptions);
      } else {
        resources.popupMenu(name, reference, entries, popupOptions);
      }
    }

    auto owner = ui::scene("LoadedPopup{}", std::move(resources));
    const auto prepared = owner.prepareChecked();

    REQUIRE_FALSE(prepared);
    CHECK(treeCause(prepared.error(), TreeError::NO_MEMORY));
    REQUIRE(prepared.error().sceneResource);
    CHECK(prepared.error().sceneResource->code == SceneResourceErrorCode::NO_MEMORY);
    CHECK(reference.node() == Node<PopupMenu>{});
  }
}

TEST_CASE("popup retains natural control and menu selectors in state styles and panel paint") {
  for (const std::string_view selector : {"Control", "Menu", "PopupMenu", ".popup-menu"}) {
    INFO(selector);
    PopupFixture fixture;
    ui::ActionSet actions;
    auto leaf = actions.action("leaf", "Leaf", [] {
    });

    fixture.mount({ui::menuItem(leaf)});

    const std::string subject(selector);
    const auto sheet = take(parsePss(subject + "{foreground:#123456;background:#234567;}" + subject +
                                     ":focused{foreground:#345678;background:#456789;}" + subject +
                                     ":disabled{foreground:#56789a;background:#6789ab;}"));

    take(fixture.scene->theme(sheet));
    fixture.open();
    take(fixture.app->tree().read(fixture.menu.node(), [&](const PopupMenu &popup) {
      REQUIRE(popup.computedStyle());
      CHECK(popup.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56});
      CHECK(popup.computedStyle()->normal.background == RgbColor{0x23, 0x45, 0x67});
      CHECK(popup.computedStyle()->focused.foreground == RgbColor{0x34, 0x56, 0x78});
      CHECK(popup.computedStyle()->disabled.foreground == RgbColor{0x56, 0x78, 0x9a});
    }));

    PopupPaintCapture capture;

    take(fixture.app->tree().frame([&] {
      take(fixture.app->tree().paint(fixture.menu.node(), capture));
    }));

    REQUIRE(capture.fillStyles.size() >= 2);
    CHECK(capture.fillStyles.front().foreground == RgbColor{0x12, 0x34, 0x56});
    CHECK(capture.fillStyles.front().background == RgbColor{0x23, 0x45, 0x67});
    CHECK(capture.fillStyles[1].foreground == RgbColor{0x34, 0x56, 0x78});
    CHECK(capture.fillStyles[1].background == RgbColor{0x45, 0x67, 0x89});
  }
}

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL == 2
TEST_CASE("popup empty entry storage propagates debug proxy allocation failure") {
  bool allocationFailed{};

  struct FailNextAllocation {
    FailNextAllocation() {
      allocationTest::remaining = 0;
    }

    ~FailNextAllocation() {
      allocationTest::remaining = -1;
    }
  };

  {
    FailNextAllocation fail;

    try {
      (void)portals::detail::emptyPopupEntries();
    } catch (const std::bad_alloc &) {
      allocationFailed = true;
    }
  }

  CHECK(allocationFailed);
}
#endif
