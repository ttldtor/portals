// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/toast_fixture.hpp"

namespace {
using namespace toastTest;

void exactHitDoesNotAcquireAncestorFocus() {
  Fixture f;
  ui::Ref<Column> ancestor;
  ui::Ref<LineEdit> editor;
  int blur{}, gain{}, actions{};
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
    blur += result.trigger == LineEditTrigger::BLUR;
    return {};
  };
  take(ui::mount(*f.scene,
                 ui::column(ui::notifications(f.center, ui::column(ui::lineEdit(editor, "Draft", config).height(1),
                                                                   ui::label("Body background").flex()))
                              .flex())
                   .reference(ancestor)));
  auto binding = take(f.app->router().bind(
    ancestor.node(),
    [](Column &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Column &, bool focused) -> std::expected<void, TreeError> {
      gain += focused;
      return {};
    }));
  take(editor.focus());
  take(f.app->router().dispatch(TextInputEvent{U'e'}));
  const auto posted = take(
    f.center.post(ui::toast("Message", {.duration = std::nullopt, .dismissOnAction = false}).action("Details", [&] {
      check(take(f.app->router().focused()) == editor.node() && blur == 0 && gain == 0,
            "zero blur/gain before user callback entry");
      ++actions;
    })));
  f.frame();
  take(f.app->router().traverse());
  const auto traversed = take(f.app->router().focused());
  check(traversed == editor.node() || traversed == ancestor.node(), "toast rows create no Tab entries");
  take(editor.focus());
  blur = 0;
  gain = 0;
  take(f.app->router().dispatch(TextInputEvent{U'x'}));
  f.frame();
  const auto rectangle = *f.snapshot()->items.front().rectangle;
  const auto press = [&](CellPoint point) {
    return take(f.app->router().dispatch(MouseInputEvent{point.x, point.y, MouseButton::LEFT, MouseAction::PRESS}));
  };
  const auto background = press({rectangle.origin().x, rectangle.origin().y});
  const auto text = press({rectangle.origin().x + 4, rectangle.origin().y});
  const auto action = press({rectangle.origin().x, rectangle.origin().y + 1});
  if (actions != 1 || blur != 0 || gain != 0 || take(f.app->router().focused()) != editor.node()) {
    std::cerr << "mouse actions=" << actions << " blur=" << blur << " gain=" << gain
              << " rectangle=" << rectangle.origin().x << ',' << rectangle.origin().y << ',' << rectangle.size().width
              << ',' << rectangle.size().height << " targets=" << background.target.slot << ',' << text.target.slot
              << ',' << action.target.slot << " editor=" << editor.node().id().slot << '\n';
  }
  check(actions == 1 && blur == 0 && gain == 0 && take(f.app->router().focused()) == editor.node(),
        "text, background and action keep exact focus");
  (void)press({rectangle.right() - 1, rectangle.origin().y});
  check(f.snapshot()->items.empty() && blur == 0 && gain == 0, "dismiss also excludes autofocus before removal");
  f.frame();
  // Body text occupies the first row; the empty plane must not replace its original body/ancestor hit.
  (void)press({2, 8});
  check(take(f.app->router().focused()) == ancestor.node() && blur == 1 && gain == 1,
        "empty-plane input retains ordinary ancestor autofocus");
  auto stale = f.center.activate(*posted.id);
  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE), "dismissed identity never replays");
}

void actionRemovalAndRecursiveAdmission() {
  Fixture f;
  f.mount();
  int calls{};
  bool pinned{}, recursive{};
  ToastId id;
  auto marker = std::make_shared<int>(7);
  std::weak_ptr<int> weak = marker;
  auto message = ui::toast("Full message\nStill owned", {.duration = std::nullopt})
                   .action("Remove", [&, marker](const ToastActionEvent &event) {
                     ++calls;
                     pinned = !weak.expired() && event.data->text.bytes() == "Full message\nStill owned";
                     auto nested = f.center.activate(id);
                     recursive = !nested && treeCause(nested.error(), TreeError::WOULD_BLOCK);
                     take(f.scene->remove(f.wrapper.node()));
                     check(!weak.expired(), "admitted capture survives self-removal");
                   });
  id = *take(f.center.post(message)).id;
  marker.reset();
  message = ui::toast("Release builder ownership");
  f.frame();
  take(f.center.activate(id));
  check(calls == 1 && pinned && recursive && weak.expired(), "action owner released only after admitted return");
  check(treeCause(f.center.activate(id).error(), TreeError::STALE_NODE), "removed center blocks new action");
}

void modalAndErrorReceipt() {
  Fixture f;
  ui::Ref<Dialog> modal;
  int actions{};
  take(ui::mount(*f.scene, ui::overlayHost(ui::notifications(f.center, ui::label("Body")),
                                           ui::dialog(modal, "Modal", ui::button("Close")))));
  const auto posted = take(
    f.center.post(ui::toast("Diagnostic", {.duration = std::nullopt, .dismissOnAction = false}).action("Details", [&] {
      ++actions;
      take(modal.open());
    })));
  f.frame();
  take(f.center.activate(*posted.id));
  check(actions == 1 && take(modal.opened()), "only explicit action opens modal and changes focus");
  auto blocked = f.center.activate(*posted.id);
  check(!blocked && treeCause(blocked.error(), TreeError::INVALID_ARGUMENT) && actions == 1,
        "top modal blocks toast command");
  take(modal.dismiss());
  take(f.center.clear());
  const auto failure = take(f.center.post(ui::toast("Throw", {.duration = std::nullopt}).action("Fail", [] {
    throw std::runtime_error("action failure");
  })));
  f.frame();
  auto result = f.center.activate(*failure.id);
  check(!result && treeCause(result.error(), TreeError::CALLBACK_FAILED), "thrown callback remains typed INPUT error");
  check(f.snapshot()->items.empty(), "failed dismissed action is never restored");
  const auto diagnostics = take(f.scene->diagnostics());
  check(!diagnostics.errors.empty(), "full failure retained in existing history");
  take(f.scene->clearErrors());
}

void nestedActionsKeepEveryOriginalAdmission() {
  Fixture f;
  f.mount();
  ToastId outer, inner;
  int outerCalls{}, innerCalls{};
  bool rejectedInside{}, rejectedAfter{};
  outer = *take(f.center.post(ui::toast("Outer", {.duration = std::nullopt, .dismissOnAction = false})
                                .action("Outer action",
                                        [&] {
                                          ++outerCalls;
                                          take(f.center.activate(inner));
                                          const auto recursive = f.center.activate(outer);
                                          rejectedAfter =
                                            !recursive && treeCause(recursive.error(), TreeError::WOULD_BLOCK);
                                        })))
             .id;
  inner = *take(f.center.post(ui::toast("Inner", {.duration = std::nullopt, .dismissOnAction = false})
                                .action("Inner action",
                                        [&] {
                                          ++innerCalls;
                                          const auto recursive = f.center.activate(outer);
                                          rejectedInside =
                                            !recursive && treeCause(recursive.error(), TreeError::WOULD_BLOCK);
                                        })))
             .id;
  f.frame();
  take(f.center.activate(outer));
  check(outerCalls == 1 && innerCalls == 1 && rejectedInside && rejectedAfter,
        "nested distinct actions cannot erase the still-running original admission");
  take(f.center.activate(outer));
  check(outerCalls == 2 && innerCalls == 2, "later explicit persistent activation is a new admission");
}

void messageChoiceCanPostTheNextNotification() {
  Fixture f;
  ui::MessageBox question;
  int choices{}, blockedActions{};
  take(ui::mount(*f.scene,
                 ui::overlayHost(ui::notifications(f.center, ui::label("Body")),
                                 ui::confirmDialog(question, "Continue?", "Choose before the next notification."))));
  const auto opener = take(f.center.post(ui::toast("Open question", {.duration = std::nullopt}).action("Choose", [&] {
    take(question.open([&](const MessageResult &result) {
      check(result && *result == MessageChoice::CANCEL, "deferred callback receives the immutable choice");
      ++choices;
      take(f.center.show(ui::toast("Choice received", {.duration = std::nullopt})));
    }));
  })));
  (void)take(f.center.post(ui::toast("Expires after the modal closes", {.duration = 100})));
  const auto other = take(f.center.post(ui::toast("Another action", {.duration = std::nullopt}).action("Other", [&] {
    ++blockedActions;
  })));
  f.frame();
  f.now = 10;
  take(f.center.activate(*opener.id));
  take(f.scene->afterDispatch(*f.app));
  const auto blocked = f.center.activate(*other.id);
  check(!blocked && treeCause(blocked.error(), TreeError::INVALID_ARGUMENT) && blockedActions == 0,
        "MessageBox modal blocks the original notification action");
  check(f.advance(200).timers == 0 && f.snapshot()->items.size() == 2,
        "modal suppresses the notification timer without resetting its absolute deadline");
  const auto choice = take(f.app->router().focused());
  check(choice != Node<Widget>{}, "MessageBox supplies its safe default choice");
  take(f.app->router().invoke(choice, {Action::ACTIVATE}));
  const auto answer = take(question.lastResult());
  check(answer && *answer && **answer == MessageChoice::CANCEL && choices == 0,
        "choice is latched before deferred callback delivery");
  take(f.scene->afterDispatch(*f.app));
  take(f.app->scheduler().dispatch());
  take(f.scene->afterDispatch(*f.app));
  f.frame();
  check(choices == 1 && f.snapshot()->items.size() == 2 &&
          f.snapshot()->items.back().data->text.bytes() == "Choice received",
        "deferred choice posts the next notification and expired content never flashes");
  take(f.center.clear());
  f.frame();
  (void)take(f.app->tree().consumeFrameRequest());
  check(f.advance(10000).timers == 0 && choices == 1 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "completed modal and notification sequence leaves no recurring work");
}
} // namespace

int main() try {
  exactHitDoesNotAcquireAncestorFocus();
  actionRemovalAndRecursiveAdmission();
  modalAndErrorReceipt();
  nestedActionsKeepEveryOriginalAdmission();
  messageChoiceCanPostTheNextNotification();
  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
