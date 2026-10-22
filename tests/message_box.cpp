// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/message_box_state.hpp"
#include "support/message_fixture.hpp"

namespace {
void canonicalReferences() {
  ui::MessageBox legacy;
  const auto before = allocationTest::count;
  ui::Ref<MessageDialog> reference(legacy);
  auto copied = reference;
  auto moved = std::move(copied);
  ui::MessageBox oldCopy = reference;

  check(allocationTest::count == before, "canonical adapters reuse the legacy slot without allocating");
  check(treeCause(copied.lastResult().error(), TreeError::CLOSED), "moved-from composite reference is cold");
  check(treeCause(moved.lastResult().error(), TreeError::CLOSED), "copied cold references remain unpublished");

  Fixture f;
  ui::Ref<Dialog> view;
  ui::MessageOptions invalid;

  invalid.maxBodyLines = 0;

  const auto rejected = ui::mount(
    *f.scene,
    ui::overlayHost(ui::label("Body"), ui::confirmDialog(reference, "Confirm", "Retry after rejected mount", invalid)));

  check(!rejected && treeCause(rejected.error(), TreeError::INVALID_ARGUMENT), "partial canonical mount rejected");
  check(treeCause(reference.lastResult().error(), TreeError::CLOSED), "failed mount never publishes adapter identity");
  take(ui::mount(*f.scene,
                 ui::overlayHost(ui::label("Body"),
                                 ui::confirmDialog(reference, "Confirm", "Shared legacy identity").reference(view))));

  std::optional<ManagedError> foreign;
  std::thread worker([&] {
    const auto result = moved.lastResult();

    if (!result) {
      foreign = result.error();
    }
  });

  worker.join();
  check(foreign && treeCause(*foreign, TreeError::WRONG_THREAD), "canonical operations retain immutable UI affinity");

  int delivered{};

  command(legacy.open([&, reference](const MessageResult &result) {
    ++delivered;
    check(result && *result == MessageChoice::CONFIRM, "canonical callback keeps its exact owning domain result");
    take(f.scene->remove(view.node()));
    check(treeCause(reference.lastResult().error(), TreeError::STALE_NODE), "callback self-removal retires the facade");
  }));
  choose(f, view.node(), "Continue");

  const auto result = answer(moved, MessageChoice::CONFIRM);

  check(take(oldCopy.lastResult()) == result, "legacy and canonical copies observe the same result identity");
  flushMessages(f);
  check(delivered == 1 && result->has_value() && **result == MessageChoice::CONFIRM,
        "callback captures and immutable result survive self-removal without duplicate delivery");
  check(treeCause(reference.lastResult().error(), TreeError::STALE_NODE), "retired adapter never retargets");
}

void presets() {
  Fixture f;
  ui::Ref<MessageDialog> information, confirm, unsaved;
  ui::Ref<Dialog> one, two, three;
  ui::Ref<Button> opener;
  take(ui::mount(*f.scene, ui::overlayHost(
                             ui::button("Open").reference(opener).onClick([] {
                             }),
                             ui::messageBox(information, "Information", "First\r\nsecond\rthird\n🙂é界").reference(one),
                             ui::confirmDialog(confirm, "Confirm", "Proceed?").reference(two),
                             ui::unsavedChangesDialog(unsaved, "Unsaved", "Keep the changes?").reference(three))));
  int delivered{};
  command(information.open([&](const MessageResult &result) {
    check(result.has_value(), "successful information");
    ++delivered;
  }));
  check(take(f.app->router().focused()) == findButton(f, one.node(), "OK"), "information initial focus");
  choose(f, one.node(), "OK");
  answer(information, MessageChoice::ACKNOWLEDGE);
  check(delivered == 0, "choice never calls result inline");
  auto busy = information.open([] {
  });
  check(!busy && treeCause(busy.error(), TreeError::WOULD_BLOCK), "queued request remains busy");
  flushMessages(f);
  check(delivered == 1, "one callback");
  for (const auto choice : {std::string_view("Continue"), std::string_view("Cancel")}) {
    command(confirm.open([&] {
      ++delivered;
    }));
    check(take(f.app->router().focused()) == findButton(f, two.node(), "Cancel"), "confirm safe default");
    choose(f, two.node(), choice);
    answer(confirm, choice == "Continue" ? MessageChoice::CONFIRM : MessageChoice::CANCEL);
    flushMessages(f);
  }
  for (const auto choice : {std::string_view("Save"), std::string_view("Discard"), std::string_view("Cancel")}) {
    command(unsaved.open([] {
    }));
    check(take(f.app->router().focused()) == findButton(f, three.node(), "Cancel"),
          "unsaved safe default every request");
    take(f.app->router().setFocus(findButton(f, three.node(), choice)));
    take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    answer(unsaved, choice == "Save"      ? MessageChoice::SAVE
                    : choice == "Discard" ? MessageChoice::DISCARD
                                          : MessageChoice::CANCEL);
    flushMessages(f);
  }
  command(information.open([] {
  }));
  command(information.cancel());
  auto cancelled = answer(information, MessageChoice::CANCEL);
  command(information.cancel());
  check(take(information.lastResult()) == cancelled, "cancel cannot overwrite latch");
  flushMessages(f);
}

void automaticScenePartialMount() {
  ui::Ref<Label> body;
  ui::Ref<MessageDialog> first, second;
  ui::Ref<Dialog> firstView, secondView;
  ui::MessageOptions invalid;
  int stopped{};

  invalid.maxBodyLines = 0;

  auto owner = ui::scene(ui::label(body, "Body"), ui::confirmDialog(first, "First", "Ready").reference(firstView),
                         ui::confirmDialog(second, "Second", "Rejected", invalid).reference(secondView));

  owner.onStop([&] {
    ++stopped;
  });

  const auto result = owner.prepareChecked();

  check(!result && treeCause(result.error(), TreeError::INVALID_ARGUMENT), "automatic message mount preserves failure");
  check(body.node() == Node<Label>{} && firstView.node() == Node<Dialog>{} && secondView.node() == Node<Dialog>{},
        "later message failure publishes no earlier body or view references");
  check(treeCause(first.lastResult().error(), TreeError::CLOSED) &&
          treeCause(second.lastResult().error(), TreeError::CLOSED) && stopped == 1,
        "partial automatic message mount retires both unpublished facades before one scene stop completes");
}

void validation() {
  Fixture f;
  ui::MessageBox box;
  check(!box.open([] {
  }) && treeCause(box.lastResult().error(), TreeError::CLOSED),
        "cold facade");
  ui::MessageOptions options;
  options.defaultChoice = MessageChoice::CONFIRM;
  auto rejected =
    ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::confirmDialog(box, "Confirm", "Body", options)));
  check(!rejected && treeCause(rejected.error(), TreeError::INVALID_ARGUMENT), "unsafe default requires opt-in");
  check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "failed cold mount is atomic");
  options.allowDestructiveDefault = true;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"),
                                           ui::confirmDialog(box, "Confirm", "Body", options).reference(dialog))));
  auto generic = dialog.open();
  check(!generic && treeCause(generic.error(), TreeError::INVALID_ARGUMENT), "unarmed generic open rejected");
  command(box.open([] {
  }));
  check(take(f.app->router().focused()) == findButton(f, dialog.node(), "Continue"), "explicit unsafe default");
  std::optional<ManagedError> foreign;
  std::thread worker([&] {
    foreign = box.lastResult().error();
  });
  worker.join();
  check(foreign && treeCause(*foreign, TreeError::WRONG_THREAD), "affinity before mutable access");
  take(f.scene->remove(dialog.node()));
  check(treeCause(box.lastResult().error(), TreeError::STALE_NODE), "original identity stale");
}

void wrapping() {
  auto text = take(GraphemeText::from("界é\r\n🙂\nlast"));
  portals::detail::MessageParagraph paragraph(std::move(text), 8);
  auto narrow = take(paragraph.measure({1, 1}));
  check(narrow.width == 1 && narrow.height == 7, "whole graphemes and hard breaks wrap");
  const auto allocations = allocationTest::count;
  allocationTest::remaining = 0;
  auto same = paragraph.measure({1, 100});
  allocationTest::remaining = -1;
  check(same && *same == narrow && allocationTest::count == allocations, "same width cache allocation-free");
  allocationTest::remaining = 0;
  auto oom = paragraph.measure({3, 1});
  allocationTest::remaining = -1;
  check(!oom && oom.error() == TreeError::NO_MEMORY && take(paragraph.measure({1, 1})) == narrow,
        "wrap OOM keeps cache");
  check(take(paragraph.measure({0, 0})) == CellSize{}, "zero width empty");
  portals::detail::MessageParagraph bounded(take(GraphemeText::from("123")), 2);
  auto limit = bounded.measure({1, 1});
  check(!limit && limit.error() == TreeError::NODE_LIMIT, "line cap never truncates");
}

void escapeAndBackpressure() {
  Fixture f({}, 1);
  ui::MessageBox box;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::messageBox(box, "Info", "Body").reference(dialog))));
  SchedulerConnection<int> filler;
  take(f.app->tree().withScope(f.scene->root(), [&](const UiScope &scope) {
    filler = take(f.app->scheduler().connect<int>(scope, [](auto) {
    }));
  }));
  int callbacks{};
  command(box.open([&] {
    ++callbacks;
  }));
  take(f.app->router().dispatch(KeyEvent{.key = InputKey::ESCAPE, .action = KeyAction::REPEAT}));
  take(f.app->router().dispatch(KeyEvent{.key = InputKey::ESCAPE, .action = KeyAction::RELEASE}));
  check(take(dialog.opened()), "Escape repeat/release never selects");
  take(filler.publisher.publish(std::make_shared<const int>(0)));
  take(f.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  auto selected = answer(box, MessageChoice::CANCEL);
  (void)take(f.app->tree().consumeFrameRequest());
  take(f.scene->afterDispatch(*f.app));
  check(callbacks == 0 && f.app->scheduler().metrics().data.rejected == 1, "full queue rejects publication only");
  check(take(box.lastResult()) == selected && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "pressure keeps same result without redraw");
  f.dispatch();
  check(callbacks == 0, "draining filler does not inline retry callback");
  take(f.scene->afterDispatch(*f.app));
  check(callbacks == 0, "afterDispatch accepts without inline delivery");
  f.dispatch();
  take(f.scene->afterDispatch(*f.app));
  check(callbacks == 1 && f.app->scheduler().metrics().timer_callbacks == 0, "single DATA result and zero timers");
}

void nativeRunner(bool failure) {
  example::ReplayTerminal terminal({{10, "\r", {}}, {100, "\r", {}}}, 1000, {20, 8});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<MessageDialog> box;
  int callbacks{}, notifications{}, reports{}, dismissals{};
  std::shared_ptr<const MessageResult> retained;
  auto scene = ui::scene(
    options,
    ui::button("Open",
               [box, &callbacks, &retained] {
                 box.open([&callbacks, box, &retained](const MessageResult &result) {
                   check(result && *result == MessageChoice::CANCEL, "native safe default is actual focused Cancel");
                   retained = answer(box, MessageChoice::CANCEL);
                   ++callbacks;
                   ui::quit();
                 });
               }),
    ui::confirmDialog(box, "Confirm", "Complete native runner")
      .onDismiss([&](DialogDismissReason) -> std::expected<void, TreeError> {
        ++dismissals;
        return failure ? std::expected<void, TreeError>(std::unexpected(TreeError::CALLBACK_FAILED))
                       : std::expected<void, TreeError>{};
      }));
  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    check(failure && treeCause(diagnostic.error, TreeError::CALLBACK_FAILED), "native exact technical failure");
    ++notifications;
  });
  scene.reportOnError([&](const ui::SceneReport &report) {
    check(terminal.closed && report.failure && treeCause(*report.failure, TreeError::CALLBACK_FAILED),
          "report follows terminal cleanup");
    ++reports;
  });
  check(scene.runApp(terminal) == (failure ? 1 : 0), "ordinary native runner status");
  check(terminal.closed && !terminal.output.empty() && dismissals == 1, "native paint and exact one dismissal");
  check(callbacks == (failure ? 0 : 1) && notifications == (failure ? 1 : 0) && reports == (failure ? 1 : 0),
        "technical failure is diagnosed once before callback admission and uses STOP");
  check(failure ? !retained : retained && retained->has_value() && **retained == MessageChoice::CANCEL,
        "automatic scene callback observes exact owning lastResult which survives shutdown");
}

struct MessagePaintSink : PaintContext {
  NodeGeometry geometry;
  std::vector<RenderCommand> &commands;

  MessagePaintSink(NodeGeometry bounds, std::vector<RenderCommand> &output) : geometry(bounds), commands(output) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> input) override {
    for (const auto &command : input) {
      std::visit(
        [&](const auto &value) {
          const auto clip = take(value.clip.translated(geometry.surface.origin())).intersection(geometry.clip);
          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            commands.emplace_back(TextCommand{take(translated(value.origin, geometry.surface.origin())), value.text,
                                              clip, value.style, value.z});
          } else {
            commands.emplace_back(
              FillCommand{take(value.area.translated(geometry.surface.origin())), clip, value.style, value.z});
          }
        },
        command);
    }
    return {};
  }
};

void paintMessages(WidgetTree &tree, Node<Widget> node, std::vector<RenderCommand> &commands) {
  const auto snapshot = take(tree.inspect(node));
  if (!snapshot.visible || !snapshot.geometry.valid) {
    return;
  }
  MessagePaintSink sink(snapshot.geometry, commands);
  take(tree.paint(node, sink));
  for (auto child : snapshot.children) {
    paintMessages(tree, child, commands);
  }
}

CellBuffer drawMessages(Fixture &f, CellSize size) {
  take(f.scene->prepare(*f.app, size));
  bool stable{};
  for (int pass = 0; pass < 3; ++pass) {
    std::optional<LayoutSnapshot> snapshot;
    take(f.app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, size));
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
      snapshot = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(),
                                              f.scene->usesGeometrySnapshot() ? LayoutSnapshotProfile::GEOMETRY
                                                                              : LayoutSnapshotProfile::DIMENSIONS));
    }));
    (void)take(f.app->tree().consumeFrameRequest());
    take(f.scene->prepareViews(*f.app, *snapshot));
    if (take(f.app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      stable = true;
      break;
    }
  }
  check(stable, "small-screen scroll/reveal converges in existing three passes");
  std::optional<CellBuffer> output;
  take(f.app->tree().frame([&] {
    std::vector<RenderCommand> commands;
    paintMessages(f.app->tree(), f.app->root(), commands);
    output.emplace(take(renderFullFrame(size, commands)));
  }));
  take(f.scene->completedLayout(*f.app));
  return std::move(*output);
}

void smallScreens() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Backdrop"),
                                           ui::unsavedChangesDialog(
                                             box, "Title", "first\nsecond\nthird\nfourth\nfifth\nfinal explanation")
                                             .reference(dialog))));
  for (const auto size : {CellSize{}, CellSize{1, 1}, CellSize{10, 6}, CellSize{24, 10}}) {
    command(box.open([] {
    }));
    const auto output = drawMessages(f, size);
    const auto cancel = findButton(f, dialog.node(), "Cancel");
    check(cancel != Node<Widget>{} && findButton(f, dialog.node(), "Save") != Node<Widget>{} &&
            findButton(f, dialog.node(), "Discard") != Node<Widget>{},
          "all logical actions retained at every viewport");
    if (size.width >= 10 && size.height >= 6) {
      const auto geometry = take(f.app->tree().inspect(cancel)).geometry;
      check(!geometry.surface.intersection(geometry.clip).empty(), "existing ScrollArea reveals final focused action");
      bool painted{};
      for (std::int32_t y = 0; y < size.height; ++y) {
        for (std::int32_t x = 0; x < size.width; ++x) {
          painted = painted || take(output.at({x, y})).glyph == "C";
        }
      }
      check(painted, "real pixels include final Cancel action");
    }
    take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    answer(box, MessageChoice::CANCEL);
    flushMessages(f);
  }
  (void)drawMessages(f, {24, 10});
  (void)take(f.app->tree().consumeFrameRequest());
  take(f.scene->prepare(*f.app, {24, 10}));
  take(f.scene->afterDispatch(*f.app));
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE &&
          f.app->scheduler().metrics().timer_callbacks == 0,
        "hidden idle message creates no redraw/timer churn");
}

void sparseMessageParity() {
  std::optional<CellBuffer> baseline;

  for (int path = 0; path < 5; ++path) {
    Fixture f;
    ui::MessageBox box;
    ui::Ref<Dialog> view;
    ui::MessageOptions actual;
    ui::ControlContext controls;
    int lookups{};

    actual.dialog.maximumPanelSize = {24, 10};
    actual.dialog.viewportMargin = 0;
    actual.labels.confirm = TextValue("Factory action");
    actual.labels.cancel = TextValue("Factory cancel");
    actual.maxBodyLines = 11;
    actual.textLimits = {90, 20, 70, 90};
    actual.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      ++lookups;

      return key == "action" ? "Proceed" : "Resolved";
    };
    controls.defaults("Dialog", {.title = text("Hostile ambient title"),
                                 .dialog = {.mode = DialogMode::NON_MODAL, .closeOnEscape = false}});
    controls.defaults("Label", {.width = 9});
    ui::ControlMessageSettings patch;

    patch.labels.confirm = i18n("action");
    patch.labels.save = i18n("unused");
    patch.labels.cancel = text("Back");
    patch.defaultChoice = {.value = MessageChoice::CONFIRM};
    patch.allowDestructiveDefault = true;
    patch.textLimits.maxInputBytes = 80;
    patch.maxBodyLines = 8;

    if (path == 4) {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
        check(false, "explicit native resolver must win over resource fallback");

        return std::unexpected(TextResourceError::MISSING_RESOURCE);
      };
      take(options.registry.add<Dialog>(
        "Notice",
        [&, actual](ui::SceneResourceArguments) {
          return ui::confirmDialog(box, "Factory title", "Factory paragraph", actual)
            .messageSettings({.maxBodyLines = 6})
            .reference(view);
        },
        ui::controlPropertySchema("MessageBox")));
      take(ui::mount(*f.scene,
                     ui::resource(R"(
        definitions{Inherited=Notice{title="Caption" message{labels{confirm=i18n("action") save=i18n("unused")} defaultChoice{value=CONFIRM} allowDestructiveDefault=true}}}
        defaults{Notice{message{labels{cancel="Back"} textLimits{maxInputBytes=80} maxBodyLines=8}}}
        scene{OverlayHost{Label{"Backdrop"} Inherited{body="Paragraph" message={maxBodyLines=8}}}}
      )",
                                  options),
                     controls));
    } else {
      auto recipe = [&] {
        if (path == 0) {
          auto options = actual;

          options.labels.confirm = *patch.labels.confirm;
          options.labels.save = *patch.labels.save;
          options.labels.cancel = *patch.labels.cancel;
          options.defaultChoice = MessageChoice::CONFIRM;
          options.allowDestructiveDefault = true;
          options.textLimits.max_input_bytes = 80;
          options.maxBodyLines = 8;

          return ui::confirmDialog(box, "Caption", "Paragraph", options);
        }
        if (path == 1) {
          return ui::confirmDialog(box, "Factory title", "Factory paragraph", actual)
            .title("Caption")
            .body("Paragraph")
            .messageSettings(patch);
        }
        if (path == 2) {
          return ui::confirmDialog(box, "Factory title", "Factory paragraph", actual) | ui::title("Caption") |
                 ui::body("Paragraph") | ui::messageSettings(patch);
        }

        return ui::confirmDialog(box, "Factory title", "Factory paragraph", actual)
          .settings({.title = text("Caption"), .body = text("Paragraph"), .message = patch});
      }();

      take(ui::mount(*f.scene, ui::overlayHost(ui::label("Backdrop"), std::move(recipe).reference(view)), controls));
    }
    check(lookups == 1, "only the active action uses the explicit native resolver");
    take(f.app->tree().read(view.node(), [&](const Dialog &widget) {
      const auto options = widget.options();

      check(options.mode == DialogMode::MODAL && options.closeOnEscape,
            "ambient ordinary Dialog defaults must not override already validated message safety");
      check(options.maximumPanelSize == CellSize{24, 10} && options.viewportMargin == 0,
            "unspecified actual native panel options survive every sparse path");
    }));
    command(box.open([] {
    }));
    check(take(f.app->router().focused()) == findButton(f, view.node(), "Proceed"),
          "explicit value-only default selects the actual active action");
    auto frame = drawMessages(f, {30, 12});

    if (!baseline) {
      baseline.emplace(std::move(frame));
    } else {
      for (std::int32_t y = 0; y < 12; ++y) {
        for (std::int32_t x = 0; x < 30; ++x) {
          check(take(frame.at({x, y})).glyph == take(baseline->at({x, y})).glyph,
                "native/method/pipe/settings/CFG paint parity uses actual caption and paragraph");
        }
      }
    }
    choose(f, view.node(), "Back");
    answer(box, MessageChoice::CANCEL);
    flushMessages(f);
    take(f.scene->remove(view.node()));
    check(!f.app->tree().inspect(view.node()) && treeCause(box.lastResult().error(), TreeError::STALE_NODE),
          "real facade and view retire together after sparse construction");
  }
}

void sparseChoiceAndValidation() {
  for (int kind = 0; kind < 3; ++kind) {
    for (int option = 0; option < 3; ++option) {
      Fixture f;
      ui::MessageBox box;
      ui::Ref<Dialog> view;
      ui::MessageOptions actual;
      ui::ControlMessageSettings patch;

      actual.defaultChoice = kind == 0   ? MessageChoice::ACKNOWLEDGE
                             : kind == 1 ? MessageChoice::CONFIRM
                                         : MessageChoice::DISCARD;
      actual.allowDestructiveDefault = true;
      if (option == 0) {
        patch.defaultChoice = {.enabled = false, .value = MessageChoice::SAVE};
      } else if (option == 1) {
        patch.defaultChoice.enabled = true;
      } else {
        actual.defaultChoice.reset();
        patch.defaultChoice.enabled = true;
      }
      auto recipe = kind == 0   ? ui::messageBox(box, "Title", "Paragraph", actual)
                    : kind == 1 ? ui::confirmDialog(box, "Title", "Paragraph", actual)
                                : ui::unsavedChangesDialog(box, "Title", "Paragraph", actual);

      take(ui::mount(*f.scene,
                     ui::overlayHost(ui::label("Body"), std::move(recipe).messageSettings(patch).reference(view))));
      command(box.open([] {
      }));
      const auto expected = kind == 0 ? "OK" : option == 1 ? kind == 1 ? "Continue" : "Discard" : "Cancel";

      check(
        take(f.app->router().focused()) == findButton(f, view.node(), expected),
        "disabled ignores a valid inactive choice; enabled preserves captured choice or chooses kind-safe fallback");
      command(box.cancel());
      flushMessages(f);
    }
  }
  for (int invalid = 0; invalid < 5; ++invalid) {
    Fixture f;
    ui::MessageBox box;
    ui::Ref<Dialog> view;
    ui::ControlMessageSettings patch;

    if (invalid == 0) {
      patch.defaultChoice = {.enabled = false, .value = static_cast<MessageChoice>(99)};
    } else if (invalid == 1) {
      patch.defaultChoice.value = MessageChoice::CONFIRM;
    } else if (invalid == 2) {
      patch.defaultChoice.value = MessageChoice::SAVE;
    } else if (invalid == 3) {
      patch.labels.confirm = text("");
    } else {
      patch.labels.cancel = text("two\nlines");
    }
    auto failed = ui::mount(
      *f.scene, ui::overlayHost(ui::label("Body"),
                                ui::confirmDialog(box, "Title", "Paragraph").messageSettings(patch).reference(view)));

    check(!failed && treeCause(failed.error(), TreeError::INVALID_ARGUMENT),
          "invalid message patch remains a typed setup failure");
    check(view.node() == Node<Dialog>{} && treeCause(box.lastResult().error(), TreeError::CLOSED),
          "failed mount publishes neither observer nor facade");
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(),
          "failed patch rolls back all internal children");
    take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::confirmDialog(box, "Retry", "Paragraph"))));
    command(box.open([] {
    }));
    command(box.cancel());
    flushMessages(f);
  }
  Fixture f;
  auto ordinary =
    ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::dialog("Plain", ui::label("body")).messageSettings({})));

  check(!ordinary && treeCause(ordinary.error(), TreeError::INVALID_ARGUMENT),
        "even an empty explicit message group must reject ordinary Dialog");
}

void sparseBodyLimitsAndNativeContext() {
  for (const bool pipe : {false, true}) {
    Fixture f;
    ui::MessageBox box;
    ui::MessageOptions actual;

    actual.labels.confirm = TextValue("y");
    actual.labels.cancel = TextValue("n");
    auto recipe = ui::confirmDialog(box, "T", "x", actual);

    if (pipe) {
      recipe = std::move(recipe) | ui::body("four") | ui::messageSettings({.textLimits = {.maxInputBytes = 3}});
    } else {
      recipe = std::move(recipe).body("four").messageSettings({.textLimits = {.maxInputBytes = 3}});
    }
    const auto failed = ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), std::move(recipe)));

    check(!failed && failed.error().text.has_value(),
          "body and lower text-limit patch use the native typed text error");
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(),
          "text limit failure removes all partial internals");
  }
  for (const bool lazy : {false, true}) {
    ui::MessageBox box;
    ui::Ref<Dialog> view;
    ui::MessageOptions actual;
    ui::ControlContext controls;
    int factoryCalls{};

    actual.labels.confirm = TextValue("y");
    actual.labels.cancel = TextValue("n");
    controls.defaults("MessageBox", {.body = text("body"), .message = {.textLimits = {.maxInputBytes = 4}}});
    controls.define("Ask", "MessageBox",
                    {.message = {.labels = {.confirm = text("Yes")},
                                 .defaultChoice = {.value = MessageChoice::CONFIRM},
                                 .allowDestructiveDefault = true}});
    auto factory = [&] {
      ++factoryCalls;

      return ui::overlayHost(ui::label("Body"),
                             ui::confirmDialog(box, "T", "captured body exceeds the effective limit", actual)
                               .variant("Ask")
                               .reference(view));
    };

    if (lazy) {
      ui::SceneOptions options;
      ManagedScene *managed{};
      Application *application{};

      options.controls = controls;
      auto owner = ui::scene(
        [&](ManagedScene &) {
          return factory();
        },
        options);

      owner.configure([&](ManagedScene &scene, Application &app) {
        managed = &scene;
        application = &app;
      });
      check(factoryCalls == 0, "scene factory stays lazy before control preflight");
      take(owner.prepareChecked());
      command(box.open([] {
      }));
      check(view.node() != Node<Dialog>{}, "lazy named message reference publishes after inherited defaults resolve");
      take(application->tree().read(take(application->router().focused()), [&](const Widget &widget) {
        const auto *button = dynamic_cast<const Button *>(&widget);

        check(button && button->text().bytes() == "Yes", "lazy defaults and named patch choose the real active button");
      }));
      command(box.cancel());
      take(managed->afterDispatch(*application));
      take(application->scheduler().dispatch());
      take(owner.close());
    } else {
      Fixture f;

      take(ui::mount(*f.scene, factory(), controls));
      command(box.open([] {
      }));
      check(take(f.app->router().focused()) == findButton(f, view.node(), "Yes"),
            "direct native mount inherits MessageBox defaults and named patch");
      command(box.cancel());
      flushMessages(f);
    }
    check(factoryCalls == 1, "each direct or lazy factory runs once after successful schema preflight");
  }
  Fixture f;
  auto plain = ui::mount(
    *f.scene, ui::overlayHost(ui::label("Body"), ui::dialog("Plain", ui::label("body")).body("message-only")));

  check(!plain && treeCause(plain.error(), TreeError::INVALID_ARGUMENT),
        "body modifier remains message-only even though both recipes return Description<Dialog>");
}
} // namespace

int main() try {
  sparseBodyLimitsAndNativeContext();
  sparseMessageParity();
  sparseChoiceAndValidation();
  canonicalReferences();
  automaticScenePartialMount();
  presets();
  validation();
  wrapping();
  escapeAndBackpressure();
  nativeRunner(false);
  nativeRunner(true);
  smallScreens();
  std::cout << "message-box: presets/defaults/deferred/cold/affinity/wrapping PASS\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
