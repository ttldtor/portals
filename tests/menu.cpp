// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/managed_text_fixture.hpp"
#include <portals/ui_menu.hpp>

#include <array>
#include <limits>

namespace {
struct MenuFixture : Fixture {
  ui::Ref<Menu> menu;

  void mount(std::initializer_list<ui::ActionRef> actions) {
    take(ui::mount(*scene, ui::menu(menu, actions)));
    prepareLayout();
    take(scene->focus(menu.node()));
  }

  void prepareLayout() {
    constexpr CellSize SIZE{60, 12};
    std::optional<LayoutSnapshot> layout;

    auto prepared = scene->prepare(*app, SIZE);

    REQUIRE_MESSAGE(prepared, "menu preparation TreeError=", prepared ? -1 : static_cast<int>(prepared.error()));
    take(app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, SIZE));

      take(layoutTree(app->tree(), take(app->tree().root()), bounds, bounds));
      layout = take(LayoutSnapshot::capture(app->tree(), scene->root(), LayoutSnapshotProfile::GEOMETRY));
    }));
    take(app->tree().consumeFrameRequest());
    take(scene->prepareViews(*app, *layout));
    take(scene->completedLayout(*app));
  }

  std::optional<std::size_t> selected() {
    std::optional<std::size_t> result;

    take(app->tree().read(menu.node(), [&](const Menu &view) {
      result = view.selectedIndex();
    }));

    return result;
  }

  std::size_t rows() {
    std::size_t result{};

    take(app->tree().read(menu.node(), [&](const Menu &view) {
      result = view.visibleRowCount();
    }));

    return result;
  }

  void key(InputKey key, KeyAction phase = KeyAction::PRESS, char32_t scalar = 0) {
    take(app->router().dispatch(KeyEvent{key, scalar, {}, phase}));
  }

  std::vector<std::string> text() {
    struct Capture : PaintContext {
      std::vector<std::string> lines = std::vector<std::string>(0);

      std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
        for (const auto &command : commands) {
          if (const auto *text = std::get_if<TextCommand>(&command)) {
            lines.emplace_back(text->text.bytes());
          }
        }

        return {};
      }
    } capture;

    take(app->tree().frame([&] {
      take(app->tree().paint(menu.node(), capture));
    }));

    return capture.lines;
  }
};
} // namespace

TEST_CASE("menu shares action state and invocation with button and explicit key binding") {
  MenuFixture fixture;
  ui::ActionSet actions;
  std::vector<ui::ActionInvocation> calls(0);
  auto save = actions.action("save", "Save", [&](const ui::ActionInvocation &invocation) {
    calls.push_back(invocation);
  });
  ui::Ref<Button> button;

  take(ui::mount(*fixture.scene, ui::vbox(ui::menu(fixture.menu, {save}), ui::button(button, save))));
  take(fixture.scene->onKey(KeyEvent{InputKey::F2}, save));
  fixture.prepareLayout();
  take(fixture.scene->focus(fixture.menu.node()));
  fixture.key(InputKey::ENTER);
  take(fixture.app->router().invoke(button.node(), {Action::ACTIVATE}));
  CHECK(take(fixture.scene->input(*fixture.app, KeyEvent{InputKey::F2})));
  REQUIRE(calls.size() == 3);
  CHECK(calls[0].source == ClickSource::KEYBOARD);
  CHECK(calls[1].source == ClickSource::PROGRAMMATIC);
  CHECK(calls[2].source == ClickSource::KEYBOARD);

  auto metadata = *take(save.snapshot());

  metadata.caption = "Сохранить 界é🙂";
  metadata.shortcut = "Ctrl+S";
  metadata.checked = true;
  REQUIRE(save.set(metadata));
  fixture.prepareLayout();
  CHECK(take(button.text().get()) == metadata.caption);
  CHECK(fixture.text() == std::vector<std::string>{"[x] Сохранить 界é🙂  Ctrl+S"});
  take(fixture.app->tree().read(button.node(), [](const Button &view) {
    CHECK(view.checked());
  }));

  metadata.enabled = false;
  REQUIRE(save.set(metadata));
  fixture.key(InputKey::ENTER);
  REQUIRE(save.invoke());
  CHECK(calls.size() == 3);
  fixture.prepareLayout();
  CHECK_FALSE(fixture.selected());
  CHECK(fixture.rows() == 1);
  CHECK_FALSE(take(fixture.app->tree().inputEligible(button.node())));

  metadata.visible = false;
  REQUIRE(save.set(metadata));
  fixture.prepareLayout();
  CHECK(fixture.rows() == 0);
}

TEST_CASE("menu accepts its initial keyboard command before the first presentation cycle") {
  Fixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto action = actions.action("first", "First", [&] {
    ++calls;
  });
  ui::Ref<Menu> menu;

  take(ui::mount(*fixture.scene, ui::menu(menu, {action})));
  take(fixture.scene->focus(menu.node()));
  take(fixture.app->router().dispatch(KeyEvent{.key = InputKey::ENTER}));
  CHECK(calls == 1);
}

TEST_CASE("menu navigation wraps and skips hidden disabled and retired entries") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto first = actions.action("first", "First", [] {
  });
  auto disabled = actions.action("disabled", "Disabled", [] {
  });
  auto hidden = actions.action("hidden", "Hidden", [] {
  });
  auto last = actions.action("last", "Last", [] {
  });
  auto state = *take(disabled.snapshot());

  state.enabled = false;
  REQUIRE(disabled.set(state));
  state = *take(hidden.snapshot());
  state.visible = false;
  REQUIRE(hidden.set(state));
  fixture.mount({first, disabled, hidden, last});
  CHECK(fixture.rows() == 3);
  CHECK(fixture.selected() == 0);
  fixture.key(InputKey::UP);
  CHECK(fixture.selected() == 3);
  fixture.key(InputKey::DOWN, KeyAction::REPEAT);
  CHECK(fixture.selected() == 0);
  fixture.key(InputKey::END);
  CHECK(fixture.selected() == 3);
  fixture.key(InputKey::HOME);
  CHECK(fixture.selected() == 0);
  REQUIRE(first.remove());
  fixture.prepareLayout();
  CHECK(fixture.selected() == 3);
  CHECK(fixture.rows() == 2);
  state = *take(last.snapshot());
  state.enabled = false;
  REQUIRE(last.set(state));
  fixture.prepareLayout();
  CHECK_FALSE(fixture.selected());
  fixture.key(InputKey::DOWN);
  CHECK_FALSE(fixture.selected());
  state = *take(disabled.snapshot());
  state.enabled = true;
  REQUIRE(disabled.set(state));
  fixture.prepareLayout();
  CHECK(fixture.selected() == 1);
}

TEST_CASE("menu activation accepts only press while navigation repeats and focus retains selection") {
  MenuFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto first = actions.action("first", "First", [&] {
    ++calls;
  });
  auto last = actions.action("last", "Last", [&] {
    ++calls;
  });
  ui::Ref<Button> other;

  take(ui::mount(*fixture.scene, ui::vbox(ui::menu(fixture.menu, {first, last}), ui::button(other, "Other", [] {
                                          }))));
  fixture.prepareLayout();
  take(fixture.scene->focus(fixture.menu.node()));
  fixture.key(InputKey::END);
  take(fixture.scene->focus(other.node()));
  take(fixture.scene->focus(fixture.menu.node()));
  CHECK(fixture.selected() == 1);
  fixture.key(InputKey::ENTER, KeyAction::REPEAT);
  fixture.key(InputKey::ENTER, KeyAction::RELEASE);
  fixture.key(InputKey::CHARACTER, KeyAction::REPEAT, U' ');
  fixture.key(InputKey::CHARACTER, KeyAction::RELEASE, U' ');
  CHECK(calls == 0);
  fixture.key(InputKey::ENTER);
  fixture.key(InputKey::CHARACTER, KeyAction::PRESS, U' ');
  take(fixture.app->router().dispatch(TextInputEvent{U' '}));
  CHECK(calls == 3);
  fixture.key(InputKey::UP, KeyAction::REPEAT);
  CHECK(fixture.selected() == 0);
  fixture.key(InputKey::DOWN, KeyAction::RELEASE);
  CHECK(fixture.selected() == 0);
}

TEST_CASE("router preserves observed keyboard phase without changing semantic action equality") {
  Fixture fixture;
  auto node = take(fixture.scene->emplace<Widget>(fixture.scene->root()));
  std::optional<ActionEvent> observed;
  auto binding = take(fixture.app->router().bind(node, [&](Widget &, const RoutedEvent &event) {
    if (const auto *action = std::get_if<ActionEvent>(&event)) {
      observed = *action;
    }

    return EventReply::HANDLED;
  }));

  take(fixture.app->router().setFocus(node));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER, 0, {.control = true}, KeyAction::REPEAT}));
  REQUIRE(observed);
  CHECK(observed->keyAction == KeyAction::REPEAT);
  CHECK(observed->source == ClickSource::KEYBOARD);
  CHECK(*observed == ActionEvent{Action::ACTIVATE, {.control = true}});
}

TEST_CASE("menu mouse selects the visible row and preserves source and modifiers") {
  MenuFixture fixture;
  ui::ActionSet actions;
  std::vector<ui::ActionInvocation> calls(0);
  auto first = actions.action("first", "First", [&](const ui::ActionInvocation &event) {
    calls.push_back(event);
  });
  auto disabled = actions.action("disabled", "Disabled", [] {
    FAIL("disabled row cannot execute");
  });
  auto last = actions.action("last", "Last", [&](const ui::ActionInvocation &event) {
    calls.push_back(event);
  });
  auto metadata = *take(disabled.snapshot());

  metadata.enabled = false;
  REQUIRE(disabled.set(metadata));
  fixture.mount({first, disabled, last});

  const auto geometry = take(fixture.scene->geometry(fixture.menu.node()));
  MouseInputEvent mouse{.column = geometry.surface.origin().x + 1,
                        .row = geometry.surface.origin().y + 2,
                        .button = MouseButton::LEFT,
                        .action = MouseAction::PRESS,
                        .modifiers = {.shift = true, .control = true}};

  take(fixture.app->router().dispatch(mouse));
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].source == ClickSource::MOUSE);
  CHECK(calls[0].modifiers.control);
  CHECK(calls[0].modifiers.shift);
  CHECK(fixture.selected() == 2);
  mouse.row -= 1;
  take(fixture.app->router().dispatch(mouse));
  CHECK(calls.size() == 1);
  CHECK(fixture.selected() == 2);
  mouse.row += 1;
  mouse.action = MouseAction::RELEASE;
  take(fixture.app->router().dispatch(mouse));
  CHECK(calls.size() == 1);
}

TEST_CASE("menu duplicate identities execute once and self removal pins callback captures") {
  MenuFixture fixture;
  ui::ActionSet actions;
  ui::ActionRef action;
  int calls{};
  auto capture = std::make_shared<int>(17);
  std::weak_ptr<int> weak = capture;

  action = actions.action("self", "Self", [&, capture] {
    ++calls;
    REQUIRE(action.remove());
    take(fixture.scene->remove(fixture.menu.node()));
    CHECK_FALSE(weak.expired());
    CHECK(*capture == 17);
  });
  capture.reset();
  fixture.mount({action, action});
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
  CHECK(weak.expired());
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
  CHECK(action.snapshot().error() == TreeError::STALE_NODE);
}

TEST_CASE("menu action can destroy the scene and retires remaining callbacks") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto action = actions.action("close", "Close", [&] {
    fixture.scene.reset();
  });

  fixture.mount({action});
  fixture.key(InputKey::ENTER);
  CHECK_FALSE(fixture.scene);
  CHECK(action.snapshot().error() == TreeError::CLOSED);
}

TEST_CASE("menu action failure preserves selection and uses one existing diagnostic receipt") {
  MenuFixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto action = actions.action("failure", "Failure", [&]() -> std::expected<void, TreeError> {
    ++calls;

    return std::unexpected(TreeError::INVALID_ARGUMENT);
  });

  fixture.mount({action});
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
  CHECK(fixture.selected() == 0);
  const auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(treeCause(diagnostics.errors[0].error, TreeError::INVALID_ARGUMENT));
}

TEST_CASE("menu recipes own range entries and factory local cold declarations") {
  MenuFixture fixture;
  int calls{};
  auto recipe = [&] {
    ui::ActionSet local;
    std::array entries{local.action("local", "Local", [&] {
      ++calls;
    })};
    auto menu = ui::menu(fixture.menu, entries);

    entries[0] = {};

    return menu;
  }();

  take(ui::mount(*fixture.scene, std::move(recipe)));
  fixture.prepareLayout();
  take(fixture.scene->focus(fixture.menu.node()));
  fixture.key(InputKey::ENTER);
  CHECK(calls == 1);
}

TEST_CASE("empty missing and retired menu entries are unavailable without fabricated rows") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto retired = actions.action("retired", "Retired", [] {
  });

  REQUIRE(retired.remove());
  fixture.mount({{}, retired});
  CHECK(fixture.rows() == 0);
  CHECK_FALSE(fixture.selected());
  fixture.key(InputKey::ENTER);
  fixture.key(InputKey::DOWN);
  CHECK_FALSE(fixture.selected());

  MenuFixture emptyFixture;

  emptyFixture.mount({});
  CHECK(emptyFixture.rows() == 0);
  CHECK_FALSE(emptyFixture.selected());
}

TEST_CASE("menu mount reports finite entry and shared presenter budgets transactionally") {
  Fixture fixture;
  ui::ActionSet actions({.maxPresenters = 2});
  auto action = actions.action("one", "One", [] {
  });
  ui::Ref<Menu> reference;
  auto tooMany = ui::mount(*fixture.scene, ui::menu(reference, {action, action}, {.maxEntries = 1}));

  REQUIRE_FALSE(tooMany);
  CHECK(treeCause(tooMany.error(), TreeError::NODE_LIMIT));
  CHECK(reference.node() == Node<Menu>{});
  auto invalid = ui::mount(*fixture.scene, ui::menu({}, {.maxEntries = 0}));
  REQUIRE_FALSE(invalid);
  CHECK(treeCause(invalid.error(), TreeError::INVALID_ARGUMENT));
  invalid = ui::mount(*fixture.scene, ui::menu({}, {.maxEntries = std::numeric_limits<std::size_t>::max()}));
  REQUIRE_FALSE(invalid);
  CHECK(treeCause(invalid.error(), TreeError::INVALID_ARGUMENT));
  take(ui::mount(*fixture.scene, ui::menu(reference, {action, action})));
  auto full = ui::mount(*fixture.scene, ui::button(action));
  REQUIRE_FALSE(full);
  CHECK(treeCause(full.error(), TreeError::NODE_LIMIT));
  take(fixture.scene->remove(reference.node()));
  take(ui::mount(*fixture.scene, ui::button(action)));
}

TEST_CASE("menu rejects failed cold declarations and actions attached to another scene") {
  Fixture first;
  Fixture second;
  ui::ActionSet actions({.maxActions = 1});
  auto action = actions.action("one", "One", [] {
  });
  auto failed = actions.action("two", "Two", [] {
  });
  auto invalid = ui::mount(*first.scene, ui::menu({failed}));

  REQUIRE_FALSE(invalid);
  CHECK(treeCause(invalid.error(), TreeError::NODE_LIMIT));
  take(ui::mount(*first.scene, ui::menu({action})));
  invalid = ui::mount(*second.scene, ui::menu({action}));
  REQUIRE_FALSE(invalid);
  CHECK(treeCause(invalid.error(), TreeError::WRONG_SESSION));
}

TEST_CASE("failed menu admission leaves cold action ownership available to another scene") {
  Fixture first;
  Fixture second;
  ui::ActionSet actions({.maxActions = 1});
  auto good = actions.action("good", "Good", [] {
  });
  auto failed = actions.action("failed", "Failed", [] {
  });
  auto rejected = ui::mount(*first.scene, ui::menu({good, failed}));

  REQUIRE_FALSE(rejected);
  CHECK(treeCause(rejected.error(), TreeError::NODE_LIMIT));
  auto attachedElsewhere = ui::mount(*second.scene, ui::menu({good}));
  REQUIRE(attachedElsewhere);
}

TEST_CASE("presenter budget rejection leaves all menu action sets cold") {
  Fixture first;
  Fixture second;
  ui::ActionSet actions({.maxPresenters = 1});
  auto good = actions.action("good", "Good", [] {
  });
  auto rejected = ui::mount(*first.scene, ui::menu({good, good}));

  REQUIRE_FALSE(rejected);
  CHECK(treeCause(rejected.error(), TreeError::NODE_LIMIT));
  auto attachedElsewhere = ui::mount(*second.scene, ui::menu({good}));
  REQUIRE(attachedElsewhere);
}

TEST_CASE("later sibling mount failure restores only newly attached menu action ownership") {
  Fixture first;
  Fixture second;
  ui::ActionSet actions;
  int calls{};
  auto good = actions.action("good", "Good", [&] {
    ++calls;
  });

  const auto fail = [] {
    return ui::label("").setup([](ManagedScene &, Node<Label> parent) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::MOUNT, parent.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    });
  };

  SUBCASE("new menu attachment becomes cold again") {
    auto rejected = ui::mount(*first.scene, ui::vbox(ui::menu({good}), fail()));

    REQUIRE_FALSE(rejected);
    CHECK(treeCause(rejected.error(), TreeError::INVALID_ARGUMENT));
    REQUIRE(ui::mount(*second.scene, ui::menu({good})));
  }

  SUBCASE("previously attached action store remains with its original scene") {
    take(ui::mount(*first.scene, ui::button(good)));
    auto rejected = ui::mount(*first.scene, ui::vbox(ui::menu({good}), fail()));

    REQUIRE_FALSE(rejected);
    REQUIRE(good.invoke());
    CHECK(calls == 1);
    auto wrongScene = ui::mount(*second.scene, ui::menu({good}));

    REQUIRE_FALSE(wrongScene);
    CHECK(treeCause(wrongScene.error(), TreeError::WRONG_SESSION));
  }

  SUBCASE("independently published live presenter retains the new scene owner") {
    ui::Ref<Button> independent;
    auto publishAndFail =
      ui::label("").setup([&](ManagedScene &owner, Node<Label> parent) -> std::expected<void, ManagedError> {
        auto published = ui::mount(owner, ui::button(independent, good));

        if (!published) {
          return std::unexpected(published.error());
        }

        return std::unexpected(ManagedError{ManagedPhase::MOUNT, parent.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      });
    auto rejected = ui::mount(*first.scene, ui::vbox(ui::menu({good}), std::move(publishAndFail)));

    REQUIRE_FALSE(rejected);
    take(first.app->tree().read(independent.node(), [](const Button &) {
    }));
    REQUIRE(good.invoke());
    CHECK(calls == 1);
    auto wrongScene = ui::mount(*second.scene, ui::menu({good}));

    REQUIRE_FALSE(wrongScene);
    CHECK(treeCause(wrongScene.error(), TreeError::WRONG_SESSION));
  }
}

TEST_CASE("unchanged menu preparation and unchanged selection allocate and invalidate nothing") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto action = actions.action("one", "One", [] {
  });

  fixture.mount({action});
  take(fixture.app->tree().consumeFrameRequest());
  allocationTest::remaining = 0;
  auto prepared = ui::detail::ActionAccess::prepare(*fixture.scene);
  allocationTest::remaining = -1;
  REQUIRE(prepared);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  fixture.key(InputKey::HOME);
  fixture.key(InputKey::DOWN);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("menu refreshes help and group metadata without additional presentation invalidation") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto action = actions.action("one", "One", [] {
  });

  fixture.mount({action});

  auto metadata = *take(action.snapshot());

  metadata.help = "New help";
  metadata.exclusiveGroup = "group";
  REQUIRE(action.set(metadata));
  take(fixture.app->tree().consumeFrameRequest());
  take(ui::detail::ActionAccess::prepare(*fixture.scene));
  CHECK(fixture.selected() == 0);
  CHECK(fixture.rows() == 1);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  allocationTest::remaining = 0;
  auto unchanged = ui::detail::ActionAccess::prepare(*fixture.scene);
  allocationTest::remaining = -1;
  CHECK(unchanged);
}

TEST_CASE("menu defers changed Unicode presentation beneath a hidden ancestor") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto action = actions.action("one", "One", [] {
  });
  ui::Ref<Column> parent;

  take(ui::mount(*fixture.scene, ui::vbox(ui::menu(fixture.menu, {action})).reference(parent)));
  fixture.prepareLayout();
  take(fixture.app->tree().setVisible(parent.node(), false));

  auto metadata = *take(action.snapshot());

  metadata.caption = "Changed 界🙂é";
  REQUIRE(action.set(metadata));
  take(fixture.app->tree().consumeFrameRequest());
  allocationTest::remaining = 0;
  auto hidden = ui::detail::ActionAccess::prepare(*fixture.scene);
  allocationTest::remaining = -1;
  REQUIRE(hidden);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.app->tree().setVisible(parent.node(), true));
  fixture.prepareLayout();
  CHECK(fixture.text() == std::vector<std::string>{"    Changed 界🙂é"});
}

TEST_CASE("menu stages fallible Unicode rows before publishing selection and invalidation") {
  MenuFixture fixture;
  ui::ActionSet actions;
  auto first = actions.action("first", "First", [] {
  });
  auto last = actions.action("last", "Last", [] {
  });

  fixture.mount({first, last});
  auto metadata = *take(first.snapshot());

  metadata.visible = false;
  REQUIRE(first.set(metadata));
  take(fixture.app->tree().consumeFrameRequest());

  std::size_t failures{};
  bool committed{};

  for (std::ptrdiff_t budget = 0; budget < 256; ++budget) {
    allocationTest::remaining = budget;
    auto result = ui::detail::ActionAccess::prepare(*fixture.scene);
    allocationTest::remaining = -1;

    if (result) {
      committed = true;
      break;
    }

    ++failures;
    CHECK(result.error() == TreeError::NO_MEMORY);
    CHECK(fixture.selected() == 0);
    CHECK(fixture.rows() == 2);
    CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  }

  CHECK(failures > 0);
  REQUIRE(committed);
  CHECK(fixture.selected() == 1);
  CHECK(fixture.rows() == 1);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::LAYOUT);
}
