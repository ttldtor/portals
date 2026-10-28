// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/message_fixture.hpp"
#include <doctest.h>
#include <portals/ui_scene_resource.hpp>

namespace {
void layout(Fixture &fixture, CellSize size = {30, 14}) {
  take(fixture.scene->prepare(*fixture.app, size));

  for (int pass = 0; pass < 3; ++pass) {
    std::optional<LayoutSnapshot> snapshot;

    take(fixture.app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, size));

      take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
      snapshot = take(LayoutSnapshot::capture(
        fixture.app->tree(), fixture.scene->root(),
        fixture.scene->usesGeometrySnapshot() ? LayoutSnapshotProfile::GEOMETRY : LayoutSnapshotProfile::DIMENSIONS));
    }));
    (void)take(fixture.app->tree().consumeFrameRequest());
    take(fixture.scene->prepareViews(*fixture.app, *snapshot));

    if (take(fixture.app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      break;
    }
  }
}

std::vector<std::string> captions(Fixture &fixture, Node<Widget> root) {
  std::vector<std::string> result(0);

  take(fixture.app->tree().read(root, [&](const Widget &widget) {
    if (const auto *button = dynamic_cast<const Button *>(&widget)) {
      result.emplace_back(button->text().bytes());
    }
  }));

  for (auto child : take(fixture.app->tree().inspect(root)).children) {
    auto values = captions(fixture, child);

    result.insert(result.end(), values.begin(), values.end());
  }

  return result;
}

ActionBinding unhandled(Fixture &fixture, Node<Label> body) {
  return take(fixture.app->router().bind(body, [](Label &, const RoutedEvent &) {
    return EventReply::IGNORED;
  }));
}
} // namespace

TEST_CASE("dialog action presets preserve numeric outcomes and exact safe action sets") {
  static_assert(static_cast<int>(MessageChoice::ACKNOWLEDGE) == 0);
  static_assert(static_cast<int>(MessageChoice::CANCEL) == 4);
  static_assert(static_cast<int>(MessageChoice::YES) == 5);
  static_assert(static_cast<int>(MessageChoice::NO) == 6);

  const std::array presets{ui::DialogButtons::OK,
                           ui::DialogButtons::OK_CANCEL,
                           ui::DialogButtons::YES_NO,
                           ui::DialogButtons::YES_NO_CANCEL,
                           ui::DialogButtons::CONFIRM_CANCEL,
                           ui::DialogButtons::SAVE_DISCARD_CANCEL};
  const std::array<std::vector<std::string>, 6> expected{{{"OK"},
                                                          {"OK", "Cancel"},
                                                          {"Yes", "No"},
                                                          {"Yes", "No", "Cancel"},
                                                          {"Continue", "Cancel"},
                                                          {"Save", "Discard", "Cancel"}}};

  for (std::size_t i = 0; i < presets.size(); ++i) {
    Fixture fixture;
    ui::Ref<MessageDialog> result;
    ui::Ref<Dialog> view;

    take(
      ui::mount(*fixture.scene, ui::overlayHost(ui::label("Backdrop"),
                                                ui::dialog(result, "Title", "Paragraph", presets[i]).reference(view))));
    CHECK(captions(fixture, view.node()) == expected[i]);
    command(result.open([] {
    }));

    const auto safe = i == 0 ? "OK" : i == 2 ? "No" : "Cancel";

    CHECK(take(fixture.app->router().focused()) == findButton(fixture, view.node(), safe));
    take(fixture.app->router().dispatch(KeyEvent{.key = InputKey::ENTER, .action = KeyAction::REPEAT}));
    take(fixture.app->router().dispatch(KeyEvent{.key = InputKey::ENTER, .action = KeyAction::RELEASE}));
    CHECK(take(view.opened()));
    take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    answer(result, i == 0 ? MessageChoice::ACKNOWLEDGE : i == 2 ? MessageChoice::NO : MessageChoice::CANCEL);
    flushMessages(fixture);
    command(result.open([] {
    }));
    take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    answer(result, MessageChoice::CANCEL);
  }
}

TEST_CASE("owning custom dialog body mounts once and outcome callback may remove its dialog") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;
  ui::Ref<Label> body;
  int mounts{}, delivered{};

  auto recipe = ui::dialog(result, "Unicode 🙂",
                           ui::label("界é").reference(body).setup(
                             [&](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
                               ++mounts;

                               return {};
                             }),
                           ui::DialogButtons::YES_NO)
                  .reference(view);

  take(ui::mount(*fixture.scene, ui::overlayHost(ui::label("Backdrop"), std::move(recipe))));
  CHECK(mounts == 1);
  command(result.open([] {
  }));
  choose(fixture, view.node(), "Yes");
  answer(result, MessageChoice::YES);
  flushMessages(fixture);
  command(result.open([&](const MessageResult &answer) {
    CHECK(answer == MessageChoice::NO);
    ++delivered;
    take(fixture.scene->remove(view.node()));
  }));
  choose(fixture, view.node(), "No");

  auto snapshot = answer(result, MessageChoice::NO);

  flushMessages(fixture);
  CHECK(mounts == 1);
  CHECK(delivered == 1);
  CHECK(**snapshot == MessageChoice::NO);
  CHECK(treeCause(result.lastResult().error(), TreeError::STALE_NODE));
  CHECK_FALSE(fixture.app->tree().inspect(body.node()));
}

TEST_CASE("unhandled physical Enter falls back once while LineEdit keeps commit") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;
  ui::Ref<Label> body;
  ui::Ref<LineEdit> editor;

  take(ui::mount(*fixture.scene,
                 ui::overlayHost(ui::label("Backdrop"), ui::dialog(result, "Title",
                                                                   ui::column(ui::label("Body").reference(body),
                                                                              ui::lineEdit("Value").reference(editor)),
                                                                   ui::DialogButtons::YES_NO)
                                                          .reference(view))));

  auto binding = unhandled(fixture, body.node());

  command(result.open([] {
  }));
  layout(fixture);
  take(fixture.app->router().setFocus(editor.node()));
  CHECK(take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER})).status == DispatchStatus::HANDLED);
  CHECK(take(view.opened()));
  take(fixture.app->router().setFocus(body.node()));
  take(fixture.app->router().dispatch(KeyEvent{.key = InputKey::ENTER, .action = KeyAction::REPEAT}));
  CHECK(take(view.opened()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  answer(result, MessageChoice::NO);
  flushMessages(fixture);
  command(result.open([] {
  }));
  layout(fixture);
  take(fixture.app->tree().setEnabled(findButton(fixture, view.node(), "No"), false));
  take(fixture.app->router().setFocus(body.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(take(view.opened()));
  CHECK(treeCause(result.lastResult().error(), TreeError::WOULD_BLOCK));
}

TEST_CASE("action order captions and defaults validate active roles before publication") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;
  ui::MessageOptions options;

  options.labels.yes = TextValue("Да 🙂");
  options.labels.no = TextValue("Нет 界");
  options.order = {MessageChoice::NO, MessageChoice::YES};
  options.defaultChoice = MessageChoice::YES;

  take(ui::mount(
    *fixture.scene,
    ui::overlayHost(ui::label("Backdrop"),
                    ui::dialog(result, "Title", "Body", ui::DialogButtons::YES_NO, options).reference(view))));
  CHECK(captions(fixture, view.node()) == std::vector<std::string>{"Нет 界", "Да 🙂"});
  command(result.open([] {
  }));
  CHECK(take(fixture.app->router().focused()) == findButton(fixture, view.node(), "Да 🙂"));
  command(result.cancel());
  flushMessages(fixture);

  for (const auto invalid : {std::vector{MessageChoice::YES}, std::vector{MessageChoice::YES, MessageChoice::YES},
                             std::vector{MessageChoice::YES, MessageChoice::CANCEL}}) {
    Fixture rejected;
    ui::Ref<MessageDialog> unpublished;

    options.order = invalid;

    const auto mounted = ui::mount(
      *rejected.scene, ui::overlayHost(ui::label("Backdrop"),
                                       ui::dialog(unpublished, "Title", "Body", ui::DialogButtons::YES_NO, options)));

    CHECK_FALSE(mounted);
    CHECK(treeCause(mounted.error(), TreeError::INVALID_ARGUMENT));
    CHECK(treeCause(unpublished.lastResult().error(), TreeError::CLOSED));
  }
}

TEST_CASE("close chrome occupies symmetric top-edge cells and cancels without a visible cancel role") {
  for (const auto placement : {DialogClosePlacement::START, DialogClosePlacement::END}) {
    Fixture fixture;
    ui::Ref<MessageDialog> result;
    ui::Ref<Dialog> view;
    int delivered{};

    take(ui::mount(*fixture.scene,
                   ui::overlayHost(ui::label("Backdrop"),
                                   ui::dialog(result, "A title spanning the panel", "Body", ui::DialogButtons::YES_NO)
                                     .closeButton({.enabled = true, .placement = placement, .focusable = true})
                                     .reference(view))));
    command(result.open([&] {
      ++delivered;
    }));
    layout(fixture);

    const auto close = findButton(fixture, view.node(), "[x]");
    const auto root = take(fixture.app->tree().inspect(view.node()));
    const auto panel = take(fixture.app->tree().inspect(root.children.front())).geometry.surface;
    const auto geometry = take(fixture.app->tree().inspect(close)).geometry.surface;

    CHECK(geometry.size() == CellSize{3, 1});
    CHECK(geometry.origin().y == panel.origin().y);
    CHECK(
      geometry.origin().x ==
      (placement == DialogClosePlacement::START ? panel.origin().x + 1 : panel.origin().x + panel.size().width - 4));
    CHECK(take(fixture.app->router().focused()) == findButton(fixture, view.node(), "No"));
    take(fixture.app->router().setFocus(close));
    take(fixture.app->router().dispatch(KeyEvent{.key = InputKey::ENTER, .action = KeyAction::REPEAT}));
    CHECK(take(view.opened()));
    take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    answer(result, MessageChoice::CANCEL);
    flushMessages(fixture);
    CHECK(delivered == 1);
    CHECK_FALSE(take(view.opened()));
  }
}

TEST_CASE("close chrome rejects a conflicting hot style before publishing any new values") {
  Fixture fixture;
  ui::Ref<Dialog> view;

  take(ui::mount(*fixture.scene,
                 ui::overlayHost(ui::label("Backdrop"), ui::dialog(view, "Title", ui::label("Body")).closeButton())));
  command(view.open());
  layout(fixture);

  const auto close = findButton(fixture, view.node(), "[x]");

  REQUIRE(close != Node<Widget>{});

  StyleSheet valid;

  valid.rules.push_back(
    {.selector = {.classes = {"dialog-close"}}, .declarations = {.foreground = RgbColor{10, 20, 30}, .minWidth = 3}});
  take(fixture.scene->theme(valid));
  layout(fixture);

  ComputedStyle before;
  std::uint64_t revision{};
  const auto geometry = take(fixture.app->tree().inspect(close));

  take(fixture.app->tree().read(close, [&](const Widget &widget) {
    before = widget.computedStyle()->flatten();
    revision = widget.styleRevision();
  }));

  valid.rules.front().declarations.minWidth = 7;
  valid.rules.front().declarations.foreground = RgbColor{90, 80, 70};

  const auto rejected = fixture.scene->theme(valid);

  REQUIRE_FALSE(rejected);

  const auto *error = std::get_if<StyleError>(&rejected.error().cause);

  REQUIRE(error);
  CHECK(error->cause == TreeError::INVALID_ARGUMENT);
  CHECK(take(fixture.app->tree().inspect(close)).geometryEpoch == geometry.geometryEpoch);
  take(fixture.app->tree().read(close, [&](const Widget &widget) {
    CHECK(widget.styleRevision() == revision);
    REQUIRE(widget.computedStyle());
    CHECK(*widget.computedStyle() == before);
  }));
  CHECK(take(fixture.app->tree().inspect(close)).geometry.surface.size() == CellSize{3, 1});
  CHECK(take(view.opened()));
}

TEST_CASE("tiny close chrome has no hit focus or explicit activation target") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;

  take(ui::mount(*fixture.scene,
                 ui::overlayHost(ui::label("Backdrop"), ui::dialog(result, "Title", "Body", ui::DialogButtons::YES_NO)
                                                          .maximumPanelSize({4, 5})
                                                          .closeButton({.enabled = true, .focusable = true})
                                                          .reference(view))));
  command(result.open([] {
  }));
  layout(fixture);

  const auto close = findButton(fixture, view.node(), "[x]");
  const auto geometry = take(fixture.app->tree().inspect(close)).geometry.surface;

  CHECK(geometry.empty());
  CHECK_FALSE(fixture.app->router().setFocus(close));
  CHECK_FALSE(fixture.app->router().invoke(close, {Action::ACTIVATE}));
  CHECK(take(view.opened()));
}

TEST_CASE("partial custom-body failure rolls back result and ordinary references") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Label> body;
  ui::Ref<Dialog> view;

  const auto failed =
    ui::mount(*fixture.scene,
              ui::overlayHost(ui::label("Backdrop"),
                              ui::dialog(result, "Title", ui::label("Body").reference(body), ui::DialogButtons::YES_NO)
                                .closeButton()
                                .reference(view)
                                .setup([](ManagedScene &, Node<Dialog> node) -> std::expected<void, ManagedError> {
                                  return std::unexpected(ui::detail::error(node, TreeError::NO_MEMORY));
                                })));

  CHECK_FALSE(failed);
  CHECK(treeCause(failed.error(), TreeError::NO_MEMORY));
  CHECK(treeCause(result.lastResult().error(), TreeError::CLOSED));
  CHECK(body.node() == Node<Label>{});
  CHECK(view.node() == Node<Dialog>{});
  take(ui::mount(
    *fixture.scene,
    ui::overlayHost(ui::label("Backdrop"),
                    ui::dialog(result, "Retry", "Body", ui::DialogButtons::YES_NO).closeButton().reference(view))));
  command(result.open([] {
  }));
  choose(fixture, view.node(), "No");
  answer(result, MessageChoice::NO);
}

TEST_CASE("resource custom body preset order reset and close options use the same native transaction") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;
  ui::SceneResourceOptions resources;

  resources.format = ui::SceneResourceFormat::CONFIGURATION;
  take(resources.registry.add<Dialog>(
    "Question",
    [&](ui::SceneResourceArguments) {
      ui::MessageOptions native;

      native.defaultChoice = MessageChoice::YES;

      return ui::dialog(result, "Captured", "Replaced paragraph", ui::DialogButtons::OK, native).reference(view);
    },
    ui::controlPropertySchema("MessageBox")));
  take(ui::mount(*fixture.scene, ui::resource(R"(
    definitions{Prompt=Question{message{buttons=YES_NO labels{yes="Да" no="Нет"} order{first=NO second=YES}
      defaultChoice{enabled=false}} dialog{closeButton{enabled=true placement=START focusable=true}}}}
    scene{OverlayHost{Label{"Backdrop"} Prompt{title="Question" VBox{Label{"界é"} Label{"Second"}}}}}
  )",
                                              resources)));
  CHECK(captions(fixture, view.node()) == std::vector<std::string>{"Нет", "Да", "[x]"});
  command(result.open([] {
  }));
  CHECK(take(fixture.app->router().focused()) == findButton(fixture, view.node(), "Нет"));
  layout(fixture);
  choose(fixture, view.node(), "[x]");
  answer(result, MessageChoice::CANCEL);
}

TEST_CASE("conflicting resource text and custom body leave result unpublished") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::SceneResourceOptions resources;
  int factories{};

  resources.format = ui::SceneResourceFormat::CONFIGURATION;
  take(resources.registry.add<Dialog>(
    "Question",
    [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::dialog(result, "Captured", "Body", ui::DialogButtons::YES_NO);
    },
    ui::controlPropertySchema("MessageBox")));

  const auto failed = ui::mount(
    *fixture.scene,
    ui::resource(R"(scene{OverlayHost{Label{"Backdrop"} Question{body="Conflicting" Label{"Custom"}}}})", resources));

  CHECK_FALSE(failed);
  REQUIRE(failed.error().sceneResource);
  CHECK(failed.error().sceneResource->code == SceneResourceErrorCode::INVALID_VALUE);
  CHECK(factories == 0);
  CHECK(treeCause(result.lastResult().error(), TreeError::CLOSED));
}

TEST_CASE("disabled hidden removed and inactive modal close captures cannot activate") {
  Fixture fixture;
  ui::Ref<MessageDialog> first, second;
  ui::Ref<Dialog> one, two;

  take(ui::mount(
    *fixture.scene,
    ui::overlayHost(ui::label("Backdrop"),
                    ui::dialog(first, "First", "Body", ui::DialogButtons::YES_NO).closeButton().reference(one),
                    ui::dialog(second, "Second", "Body", ui::DialogButtons::OK).closeButton().reference(two))));
  command(first.open([] {
  }));
  layout(fixture);

  const auto close = findButton(fixture, one.node(), "[x]");

  CHECK_FALSE(fixture.app->router().setFocus(close));
  for (int step = 0; step < 3; ++step) {
    take(fixture.app->router().dispatch(KeyEvent{InputKey::TAB}));
    CHECK(take(fixture.app->router().focused()) != close);
  }
  take(fixture.app->tree().setEnabled(close, false));
  const auto ignored = fixture.app->router().invoke(close, {Action::ACTIVATE});

  CHECK((!ignored || ignored->status != DispatchStatus::HANDLED));
  CHECK(take(one.opened()));
  take(fixture.app->tree().setEnabled(close, true));
  take(fixture.app->tree().setVisible(close, false));

  const auto hidden = fixture.app->router().invoke(close, {Action::ACTIVATE});

  CHECK((!hidden || hidden->status != DispatchStatus::HANDLED));
  CHECK(take(one.opened()));
  take(fixture.app->tree().setVisible(close, true));
  command(second.open([] {
  }));
  layout(fixture);
  CHECK_FALSE(fixture.app->router().invoke(close, {Action::ACTIVATE}));
  CHECK(take(one.opened()));
  CHECK(take(two.opened()));
  command(second.cancel());
  flushMessages(fixture);
  take(fixture.scene->remove(close));
  CHECK_FALSE(fixture.app->router().invoke(close, {Action::ACTIVATE}));
  CHECK(take(one.opened()));
}

TEST_CASE("close chrome preserves dismissal error precedence in the immutable domain result") {
  for (const bool chrome : {false, true}) {
    Fixture fixture;
    ui::Ref<MessageDialog> result;
    ui::Ref<Dialog> view;
    int delivered{};

    take(ui::mount(*fixture.scene,
                   ui::overlayHost(ui::label("Backdrop"),
                                   ui::dialog(result, "Title", "Body", ui::DialogButtons::YES_NO)
                                     .closeButton()
                                     .reference(view)
                                     .onDismiss([](DialogDismissReason reason) -> std::expected<void, TreeError> {
                                       CHECK(reason == DialogDismissReason::CANCEL);
                                       return std::unexpected(TreeError::NODE_LIMIT);
                                     }))));
    command(result.open([&](const MessageResult &answer) {
      CHECK_FALSE(answer);
      CHECK(treeCause(answer.error(), TreeError::NODE_LIMIT));
      ++delivered;
    }));
    layout(fixture);
    if (chrome) {
      choose(fixture, view.node(), "[x]");
    } else {
      take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    }

    const auto snapshot = take(result.lastResult());

    CHECK_FALSE(*snapshot);
    CHECK(treeCause(snapshot->error(), TreeError::NODE_LIMIT));
    CHECK(snapshot->error().phase == ManagedPhase::INPUT);
    flushMessages(fixture);
    CHECK(delivered == 0);
    CHECK(take(result.lastResult()) == snapshot);
    CHECK_FALSE(take(fixture.scene->diagnostics()).errors.empty());
  }
}

TEST_CASE("native custom body remains mandatory at the choice transaction") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;
  ui::Ref<Label> body;

  take(ui::mount(
    *fixture.scene,
    ui::overlayHost(
      ui::label("Backdrop"),
      ui::dialog(result, "Title", ui::label("Body").reference(body), ui::DialogButtons::YES_NO).reference(view))));
  command(result.open([] {
  }));
  take(fixture.scene->remove(body.node()));
  choose(fixture, view.node(), "No");

  CHECK(treeCause(result.lastResult().error(), TreeError::WOULD_BLOCK));
  CHECK(take(view.opened()));

  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE_FALSE(diagnostics.errors.empty());
  CHECK(treeCause(diagnostics.errors.back().error, TreeError::STALE_NODE));
}

TEST_CASE("close chrome paints and hits exactly its three top cells") {
  struct Sink final : PaintContext {
    std::vector<RenderCommand> commands;

    std::expected<void, TreeError> submit(std::span<const RenderCommand> input) override {
      for (const auto &command : input) {
        commands.emplace_back(command);
      }

      return {};
    }
  };

  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;

  take(ui::mount(
    *fixture.scene,
    ui::overlayHost(ui::label("Backdrop"),
                    ui::dialog(result, "界 title", "Body", ui::DialogButtons::YES_NO).closeButton().reference(view))));
  command(result.open([] {
  }));
  layout(fixture);

  const auto close = findButton(fixture, view.node(), "[x]");
  const auto geometry = take(fixture.app->tree().inspect(close)).geometry.surface;

  for (int cell = 0; cell < 3; ++cell) {
    CHECK(take(fixture.app->router().hitTest({geometry.origin().x + cell, geometry.origin().y})) == close);
  }

  CHECK(take(fixture.app->router().hitTest({geometry.origin().x - 1, geometry.origin().y})) != close);
  CHECK(take(fixture.app->router().hitTest({geometry.origin().x + 3, geometry.origin().y})) != close);
  take(fixture.app->tree().frame([&] {
    Sink sink;

    take(fixture.app->tree().paint(close, sink));

    const auto frame = take(renderFullFrame({3, 1}, sink.commands));

    CHECK(take(frame.at({0, 0})).glyph == "[");
    CHECK(take(frame.at({1, 0})).glyph == "x");
    CHECK(take(frame.at({2, 0})).glyph == "]");
  }));

  const auto origin = geometry.origin();

  take(fixture.app->router().dispatch(MouseInputEvent{
    .column = origin.x + 1, .row = origin.y, .button = MouseButton::LEFT, .action = MouseAction::PRESS}));
  answer(result, MessageChoice::CANCEL);
}

TEST_CASE("new cold resource-body storage allocation failure stays a typed atomic mount failure") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::Ref<Dialog> view;
  auto layer = ui::dialog(result, "Title", "Body", ui::DialogButtons::YES_NO).closeButton().reference(view);
  auto body = ui::detail::Access::erase(ui::label("Custom"));
  auto recipe = ui::detail::Access::transaction(
    [layer = std::move(layer),
     body = std::move(body)](ManagedScene &scene, Node<Widget> parent,
                             ui::detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      struct RestoreAllocation {
        ~RestoreAllocation() {
          allocationTest::remaining = -1;
        }
      } restore;

      // Fail only the new owning cold-body packet allocation, inside the real mount boundary.
      allocationTest::remaining = 0;

      auto staged = ui::detail::Access::messageBody(layer, std::move(body));

      allocationTest::remaining = -1;

      if (!staged) {
        return std::unexpected(staged.error());
      }

      auto mounted =
        ui::detail::Access::mount(ui::overlayHost(ui::label("Backdrop"), std::move(layer)), scene, parent, context);

      return mounted ? std::expected<Node<Widget>, ManagedError>(*mounted) : std::unexpected(mounted.error());
    });

  const auto failed = ui::mount(*fixture.scene, std::move(recipe));

  CHECK_FALSE(failed);
  CHECK(treeCause(failed.error(), TreeError::NO_MEMORY));
  CHECK(treeCause(result.lastResult().error(), TreeError::CLOSED));
  CHECK(view.node() == Node<Dialog>{});
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());
}

TEST_CASE("close modifier placement opts in and explicit false preserves the old appearance") {
  static_assert(std::is_aggregate_v<DialogOptions>);
  static_assert(std::is_aggregate_v<DialogCloseOptions>);
  static_assert(!DialogOptions{}.closeButton.enabled);
  static_assert(DialogCloseOptions{}.enabled);

  for (int path = 0; path < 3; ++path) {
    Fixture fixture;
    ui::Ref<Dialog> view;
    auto recipe = ui::dialog(view, "Title", ui::label("Body"));

    if (path == 0) {
      recipe = std::move(recipe).closeButton({.placement = DialogClosePlacement::START});
    } else if (path == 1) {
      recipe = std::move(recipe) | ui::closeButton({.placement = DialogClosePlacement::START});
    } else {
      recipe = std::move(recipe).closeButton({.enabled = false});
    }

    take(ui::mount(*fixture.scene, ui::overlayHost(ui::label("Backdrop"), std::move(recipe))));

    const auto close = findButton(fixture, view.node(), "[x]");

    CHECK((close != Node<Widget>{}) == (path != 2));
  }
}

TEST_CASE("ordinary close chrome uses generic cancellation independently of Escape policy") {
  Fixture fixture;
  ui::Ref<Dialog> view;
  std::optional<DialogDismissReason> reason;

  take(ui::mount(*fixture.scene, ui::overlayHost(ui::label("Backdrop"),
                                                 ui::dialog(view, "Title", ui::label("Body"), {.closeOnEscape = false})
                                                   .closeButton()
                                                   .onDismiss([&](DialogDismissReason value) {
                                                     reason = value;
                                                   }))));
  command(view.open());
  layout(fixture);
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  CHECK(take(view.opened()));
  choose(fixture, view.node(), "[x]");
  CHECK_FALSE(take(view.opened()));
  CHECK(reason == DialogDismissReason::CANCEL);
}

TEST_CASE("message settings presets order and safe reset agree across method pipe and defaults") {
  for (int path = 0; path < 4; ++path) {
    Fixture fixture;
    ui::Ref<MessageDialog> result;
    ui::Ref<Dialog> view;
    ui::MessageOptions options;
    ui::ControlContext controls;
    const ui::ControlMessageSettings patch{.labels = {.yes = TextValue("Proceed"), .no = TextValue("Back")},
                                           .defaultChoice = {.enabled = false},
                                           .buttons = ui::DialogButtons::YES_NO,
                                           .order = {.first = MessageChoice::NO, .second = MessageChoice::YES}};

    options.defaultChoice = MessageChoice::YES;

    if (path == 0) {
      options.labels.yes = TextValue("Proceed");
      options.labels.no = TextValue("Back");
      options.defaultChoice.reset();
      options.order = {MessageChoice::NO, MessageChoice::YES};
    }

    auto recipe =
      ui::dialog(result, "Title", "Body", path == 0 ? ui::DialogButtons::YES_NO : ui::DialogButtons::OK, options)
        .reference(view);

    if (path == 1) {
      recipe = std::move(recipe).messageSettings(patch);
    } else if (path == 2) {
      recipe = std::move(recipe) | ui::messageSettings(patch);
    } else if (path == 3) {
      controls.defaults("MessageBox", {.message = patch});
    }

    take(ui::mount(*fixture.scene, ui::overlayHost(ui::label("Backdrop"), std::move(recipe)), controls));
    CHECK(captions(fixture, view.node()) == std::vector<std::string>{"Back", "Proceed"});
    command(result.open([] {
    }));
    CHECK(take(fixture.app->router().focused()) == findButton(fixture, view.node(), "Back"));
    choose(fixture, view.node(), "Back");
    answer(result, MessageChoice::NO);
  }
}

TEST_CASE("old exact semantic resource schemas remain narrow and do not gain new option leaves") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::SceneResourceOptions resources;
  const auto canonical = ui::controlPropertySchema("MessageBox");
  std::vector<ui::ControlPropertyDescriptor> old(canonical.begin(), canonical.begin() + 24);
  const std::array<std::string_view, 5> names{"ACKNOWLEDGE", "CONFIRM", "SAVE", "DISCARD", "CANCEL"};
  int factories{};

  for (auto &property : old) {
    if (property.path == "message.defaultChoice.value") {
      property.enumNames = names;
    }
  }

  resources.format = ui::SceneResourceFormat::CONFIGURATION;
  take(resources.registry.add<Dialog>(
    "Question",
    [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::confirmDialog(result, "Title", "Body");
    },
    std::span<const ui::ControlPropertyDescriptor>(old)));

  const auto failed =
    ui::mount(*fixture.scene,
              ui::resource(R"(scene{OverlayHost{Label{"Backdrop"} Question{message{buttons=YES_NO}}}})", resources));

  CHECK_FALSE(failed);
  CHECK(factories == 0);
  CHECK(treeCause(result.lastResult().error(), TreeError::CLOSED));
}

TEST_CASE("unsupported resource cancel mappings are rejected before any factory runs") {
  Fixture fixture;
  ui::Ref<MessageDialog> result;
  ui::SceneResourceOptions resources;
  int factories{};

  resources.format = ui::SceneResourceFormat::CONFIGURATION;
  take(resources.registry.add<Dialog>(
    "Question",
    [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::dialog(result, "Title", "Body", ui::DialogButtons::YES_NO);
    },
    ui::controlPropertySchema("MessageBox")));

  const auto failed =
    ui::mount(*fixture.scene,
              ui::resource(R"(scene{OverlayHost{Label{"Backdrop"} Question{message{cancelChoice=NO}}}})", resources));

  CHECK_FALSE(failed);
  REQUIRE(failed.error().sceneResource);
  CHECK(failed.error().sceneResource->code == SceneResourceErrorCode::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
  CHECK(treeCause(result.lastResult().error(), TreeError::CLOSED));
}

TEST_CASE("old exact message choice enum rejects YES and NO before factory admission") {
  for (const auto role : {std::string_view("YES"), std::string_view("NO")}) {
    for (int path = 0; path < 6; ++path) {
      Fixture fixture;
      ui::Ref<MessageDialog> result;
      ui::SceneResourceOptions resources;
      const auto canonical = ui::controlPropertySchema("MessageBox");
      std::vector<ui::ControlPropertyDescriptor> old(canonical.begin(), canonical.begin() + 24);
      const std::array<std::string_view, 5> names{"ACKNOWLEDGE", "CONFIRM", "SAVE", "DISCARD", "CANCEL"};
      int factories{};

      for (auto &property : old) {
        if (property.path == "message.defaultChoice.value") {
          property.enumNames = names;
        }
      }

      resources.format = ui::SceneResourceFormat::CONFIGURATION;
      take(resources.registry.add<Dialog>(
        "Question",
        [&](ui::SceneResourceArguments) {
          ++factories;
          return ui::dialog(result, "Title", "Body", ui::DialogButtons::YES_NO);
        },
        std::span<const ui::ControlPropertyDescriptor>(old)));

      const auto value = std::string(role);
      const auto source =
        path == 0 ? "scene{OverlayHost{Label{\"Backdrop\"} Question{message{defaultChoice{value=" + value + "}}}}}"
        : path == 1
          ? "definitions{Prompt=Question{}} scene{OverlayHost{Label{\"Backdrop\"} Prompt{message{defaultChoice{value=" +
              value + "}}}}}"
        : path == 2 ? "defaults{Question{message{defaultChoice{value=" + value +
                        "}}}} scene{OverlayHost{Label{\"Backdrop\"} Question{}}}"
        : path == 3
          ? "scene{OverlayHost{Label{\"Backdrop\"} Question{message{defaultChoice{enabled=false value=" + value +
              "}}}}}"
        : path == 4 ? "definitions{Prompt=Question{message{defaultChoice{value=" + value +
                        "}}}} scene{OverlayHost{Label{\"Backdrop\"} Prompt{}}}"
                    : "definitions{Prompt=Later{message{defaultChoice{value=" + value +
                        "}}} Later=Question{}} scene{OverlayHost{Label{\"Backdrop\"} Prompt{}}}";
      const auto failed = ui::mount(*fixture.scene, ui::resource(source, resources));

      CHECK_FALSE(failed);

      if (!failed) {
        REQUIRE(failed.error().sceneResource);
        CHECK(failed.error().sceneResource->code == SceneResourceErrorCode::INVALID_VALUE);
      }

      CHECK(factories == 0);
      CHECK(treeCause(result.lastResult().error(), TreeError::CLOSED));
    }
  }
}

TEST_CASE("registered old choice values remain usable and modern enum domains retain YES") {
  const auto canonical = ui::controlPropertySchema("MessageBox");
  const std::array<std::string_view, 5> oldNames{"ACKNOWLEDGE", "CONFIRM", "SAVE", "DISCARD", "CANCEL"};

  for (std::size_t role = 0; role < oldNames.size() + 3; ++role) {
    Fixture fixture;
    ui::Ref<MessageDialog> result;
    ui::Ref<Dialog> view;
    ui::SceneResourceOptions resources;
    std::vector<ui::ControlPropertyDescriptor> metadata(canonical.begin(),
                                                        canonical.begin() + (role == 5 ? canonical.size() : 24));
    int factories{};

    if (role < oldNames.size() || role == 7) {
      for (auto &property : metadata) {
        if (property.path == "message.defaultChoice.value") {
          property.enumNames = oldNames;
        }
      }
    }

    resources.format = ui::SceneResourceFormat::CONFIGURATION;
    take(resources.registry.add<Dialog>(
      "Question",
      [&](ui::SceneResourceArguments) {
        ++factories;
        ui::MessageOptions options;

        options.allowDestructiveDefault = true;

        if (role == 7) {
          options.defaultChoice = MessageChoice::YES;
        }

        const auto preset = role == 0                ? ui::DialogButtons::OK
                            : role == 1 || role == 4 ? ui::DialogButtons::CONFIRM_CANCEL
                            : role == 2 || role == 3 ? ui::DialogButtons::SAVE_DISCARD_CANCEL
                                                     : ui::DialogButtons::YES_NO;

        return ui::dialog(result, "Title", "Body", preset, options).reference(view);
      },
      std::span<const ui::ControlPropertyDescriptor>(metadata)));

    const auto overrideRole = role < oldNames.size() ? oldNames[role] : std::string_view("YES");
    // The last case has no explicit override: a narrow old leaf set does not constrain native captures.
    const auto source = role == 7 ? std::string("scene{OverlayHost{Label{\"Backdrop\"} Question{}}}")
                                  : "scene{OverlayHost{Label{\"Backdrop\"} Question{message{defaultChoice{value=" +
                                      std::string(overrideRole) + "}}}}}";

    take(ui::mount(*fixture.scene, ui::resource(source, resources)));
    CHECK(factories == 1);
    command(result.open([] {
    }));
    take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    answer(result, role < oldNames.size() ? static_cast<MessageChoice>(role) : MessageChoice::YES);
  }
}

TEST_CASE("theme selector frame close cancels preview through its semantic owner") {
  for (const bool resource : {false, true}) {
    for (const bool conflict : {false, true}) {
      Fixture fixture;
      ui::Ref<ThemeSelector> selector;
      ui::Ref<Dialog> view;
      ui::ThemeSelectorOptions options;
      const auto theme = [] {
        StyleRule rule;

        rule.declarations.foreground = RgbColor{220, 10, 10};

        return StyleSheet{.rules = {rule}};
      };

      options.dialog.closeButton.enabled = !resource;

      if (resource) {
        ui::SceneResourceOptions resources;

        resources.format = ui::SceneResourceFormat::CONFIGURATION;
        take(resources.registry.add<Dialog>(
          "Picker",
          [&](ui::SceneResourceArguments) {
            return ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red", theme}}}, options)
              .reference(view);
          },
          ui::controlPropertySchema("ThemeSelectorCatalog")));
        take(
          ui::mount(*fixture.scene,
                    ui::resource(R"(scene{OverlayHost{Label{"Backdrop"} Picker{dialog{closeButton{enabled=true}}}}})",
                                 resources)));
      } else {
        take(
          ui::mount(*fixture.scene,
                    ui::overlayHost(
                      ui::label("Backdrop"),
                      ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red", theme}}}, options)
                        .reference(view))));
      }
      layout(fixture, {80, 24});
      take(selector.open());
      layout(fixture, {80, 24});
      choose(fixture, view.node(), "Red");
      CHECK(take(selector.snapshot())->status == ui::ThemeSelectionStatus::ACTIVE);

      if (conflict) {
        take(fixture.scene->theme(theme()));
      }

      layout(fixture, {80, 24});
      choose(fixture, view.node(), "[x]");

      if (conflict) {
        CHECK(take(view.opened()));

        const auto diagnostics = take(fixture.scene->diagnostics());

        REQUIRE_FALSE(diagnostics.errors.empty());

        const auto *cause = std::get_if<StyleError>(&diagnostics.errors.back().error.cause);

        REQUIRE(cause);
        CHECK(cause->cause == TreeError::INVALID_ARGUMENT);
      } else {
        CHECK_FALSE(take(view.opened()));
        CHECK(take(selector.snapshot())->status == ui::ThemeSelectionStatus::IDLE);
        CHECK_FALSE(take(selector.snapshot())->previewedId);
      }
    }
  }
}

TEST_CASE("theme selector frame close respects Apply callback reentry admission") {
  Fixture fixture;
  ui::Ref<ThemeSelector> selector;
  ui::Ref<Dialog> view;
  ui::ThemeSelectorOptions options;
  int delivered{};

  options.dialog.closeButton.enabled = true;
  options.onCommitted = [&](std::string_view id) -> std::expected<void, ManagedError> {
    ++delivered;
    CHECK(id == "red");

    const auto cancelled = selector.cancel();

    REQUIRE_FALSE(cancelled);
    CHECK(treeCause(cancelled.error(), TreeError::WOULD_BLOCK));
    CHECK(take(view.opened()));
    choose(fixture, view.node(), "[x]");
    CHECK(take(view.opened()));
    CHECK_FALSE(take(selector.lastApply()));

    const auto diagnostics = take(fixture.scene->diagnostics());

    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(treeCause(diagnostics.errors.back().error, TreeError::WOULD_BLOCK));
    CHECK(std::string_view(diagnostics.errors.back().error.operation) == "Dialog.closeButton");
    CHECK(diagnostics.errors.back().error.operationOrigin.has_value());

    return {};
  };
  take(ui::mount(*fixture.scene,
                 ui::overlayHost(ui::label("Backdrop"),
                                 ui::themeSelector(selector,
                                                   ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                                         [] {
                                                                                           return StyleSheet{};
                                                                                         }}}},
                                                   options)
                                   .reference(view))));
  layout(fixture, {80, 24});
  take(selector.open());
  layout(fixture, {80, 24});
  choose(fixture, view.node(), "Red");
  layout(fixture, {80, 24});
  // Programmatic Apply leaves the router available during synchronous delivery.
  take(selector.apply());
  CHECK(delivered == 1);
  CHECK_FALSE(take(view.opened()));

  const auto outcome = take(selector.lastApply());

  REQUIRE(outcome);
  CHECK(outcome->committedId == "red");
  CHECK(outcome->closeDisposition == ui::ThemeSelectorCloseDisposition::CLOSED);
  CHECK_FALSE(outcome->callbackError);
  CHECK_FALSE(outcome->closeError);
}
