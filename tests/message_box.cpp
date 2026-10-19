// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/message_box_state.hpp"
#include "support/message_fixture.hpp"

namespace {
void presets() {
  Fixture f;
  ui::MessageBox information, confirm, unsaved;
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
  ui::MessageBox box;
  int callbacks{}, notifications{}, reports{}, dismissals{};
  auto scene =
    ui::scene(ui::overlayHost(ui::button("Open",
                                         [box, &callbacks] {
                                           box.open([&callbacks](const MessageResult &result) {
                                             check(result && *result == MessageChoice::CANCEL,
                                                   "native safe default is actual focused Cancel");
                                             ++callbacks;
                                             ui::quit();
                                           });
                                         }),
                              ui::confirmDialog(box, "Confirm", "Complete native runner")
                                .onDismiss([&](DialogDismissReason) -> std::expected<void, TreeError> {
                                  ++dismissals;
                                  return failure
                                           ? std::expected<void, TreeError>(std::unexpected(TreeError::CALLBACK_FAILED))
                                           : std::expected<void, TreeError>{};
                                })),
              options);
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
} // namespace

int main() try {
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
