// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/message_fixture.hpp"

namespace {
struct Outcome {
  std::shared_ptr<const MessageResult> result;
  std::optional<ManagedError> initial;
  std::optional<AsyncError> outer;
  std::optional<AsyncReceive<MessageResult>> copied;
  int resumes{};
};

UiTask receive(UiAsyncContext context, ui::MessageBox box, ui::Ref<Column> owner, std::shared_ptr<Outcome> outcome,
               bool copy) {
  auto waiting = box.receive(context, owner);
  if (!waiting) {
    outcome->initial = waiting.error();
    co_return std::expected<void, AsyncError>{};
  }
  if (copy) {
    outcome->copied = *waiting;
  }
  auto delivered = co_await std::move(*waiting);
  if (!delivered) {
    co_return std::unexpected(delivered.error());
  }
  ++outcome->resumes;
  outcome->result = *delivered;
  co_return std::expected<void, AsyncError>{};
}

UiTask idle() {
  co_return std::expected<void, AsyncError>{};
}

UiTask awaitReserved(AsyncReceive<MessageResult> waiting, std::shared_ptr<Outcome> outcome) {
  auto delivered = co_await std::move(waiting);
  if (!delivered) {
    outcome->outer = delivered.error();
    co_return std::unexpected(delivered.error());
  }
  outcome->result = *delivered;
  ++outcome->resumes;
  co_return std::expected<void, AsyncError>{};
}

void normalAndCancel(bool cancellation, bool queued, bool replace, bool pressure = false,
                     bool allocationFailure = false, bool commandFailure = false) {
  Fixture f({}, 1);
  ui::MessageBox box;
  ui::Ref<Column> owner;
  ui::Ref<Dialog> dialog;
  take(ui::mount(
    *f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                              ui::unsavedChangesDialog(box, "Unsaved", "Changes")
                                .reference(dialog)
                                .onDismiss([commandFailure](DialogDismissReason) -> std::expected<void, TreeError> {
                                  return commandFailure
                                           ? std::expected<void, TreeError>(std::unexpected(TreeError::CALLBACK_FAILED))
                                           : std::expected<void, TreeError>{};
                                }))));
  auto async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto outcome = std::make_shared<Outcome>();
  take(async->start(receive(async->context(), box, owner, outcome, true)));
  check(!outcome->initial && take(dialog.opened()), "actual provider opens panel");
  SchedulerConnection<int> filler;
  if (pressure) {
    take(f.app->tree().withScope(f.scene->root(), [&](const UiScope &scope) {
      filler = take(f.app->scheduler().connect<int>(scope, [](auto) {
      }));
    }));
    take(filler.publisher.publish(std::make_shared<const int>(0)));
  }
  choose(f, dialog.node(), "Save");
  auto selected = take(box.lastResult());
  if (commandFailure) {
    check(!*selected && treeCause(selected->error(), TreeError::CALLBACK_FAILED) &&
            selected->error().phase == ManagedPhase::INPUT && selected->error().node == dialog.node().id(),
          "exact full command failure latch");
  } else {
    check(*selected && **selected == MessageChoice::SAVE, "SAVE latch");
  }
  if (pressure) {
    take(f.scene->afterDispatch(*f.app));
    check(f.app->scheduler().metrics().data.rejected == (commandFailure ? 0u : 1u) && outcome->resumes == 0 &&
            take(box.lastResult()) == selected,
          "full queue retains domain pointer/value before cancellation");
  }
  if (allocationFailure) {
    allocationTest::remaining = 0;
    auto failed = f.scene->afterDispatch(*f.app);
    allocationTest::remaining = -1;
    check(!failed && failed.error() == TreeError::NO_MEMORY && take(box.lastResult()) == selected,
          "AsyncCompletion OOM preserves full immutable domain result");
    auto diagnostics = take(f.scene->diagnostics());
    check(!diagnostics.errors.empty() &&
            diagnostics.errors.back().error.operation == std::string_view("MessageBox.deliver") &&
            treeCause(diagnostics.errors.back().error, TreeError::NO_MEMORY),
          "transport fault has its separate exact local diagnostic");
  }
  if (queued) {
    take(f.scene->afterDispatch(*f.app));
  }
  if (cancellation) {
    if (replace) {
      take(async->start(idle(), UiTaskRestart::CANCEL_PREVIOUS));
    } else {
      take(async->cancel());
    }
    flushMessages(f);
    check(outcome->resumes == 0 && take(box.lastResult()) == selected, "cancel preserves immutable latched result");
    if (!replace) {
      check(take(async->outcome()).status == UiTaskStatus::CANCELLED &&
              take(async->outcome()).error == AsyncError::CANCELLED,
            "exact task cancellation");
    }
    if (!commandFailure && !allocationFailure) {
      command(box.open([] {
      }));
    }
    outcome->copied.reset();
    take(f.scene->afterDispatch(*f.app));
    if (!commandFailure && !allocationFailure) {
      check(take(dialog.opened()), "old copied provider cleanup cannot close reopened panel");
      command(box.cancel());
    }
  } else {
    if (pressure) {
      f.dispatch();
    }
    flushMessages(f);
    if (commandFailure || allocationFailure) {
      check(outcome->resumes == 0 && take(box.lastResult()) == selected,
            "default STOP closes transport without changing latched domain outcome");
    } else {
      check(outcome->resumes == 1 && outcome->result == selected &&
              take(async->outcome()).status == UiTaskStatus::SUCCEEDED,
            "normal await_resume retains exact result and does not fake cancellation");
      if (!pressure && !queued) {
        auto duplicate = std::make_shared<Outcome>();
        auto reused = async->start(awaitReserved(*outcome->copied, duplicate));
        check(reused && take(async->outcome()).status == UiTaskStatus::FAILED &&
                take(async->outcome()).error == AsyncError::CALLBACK_FAILED &&
                duplicate->outer == AsyncError::CALLBACK_FAILED && !take(dialog.opened()) &&
                take(box.lastResult()) == selected,
              "copied provider cannot start twice or replace the original invocation/wait identity");
      }
    }
    outcome->copied.reset();
    take(f.scene->afterDispatch(*f.app));
    check(take(box.lastResult()) == selected, "normal copied provider release preserves result");
  }
}

void queuedOwnerOrDialogClosed() {
  bool allRevoked = true;
  for (const bool removeDialog : {false, true}) {
    for (const bool prepareBeforeDispatch : {false, true}) {
      ManagedOptions options;
      options.maxMessageRequests = 1;
      Fixture f(options);
      ui::MessageBox box;
      ui::Ref<Column> owner;
      ui::Ref<Dialog> dialog;
      take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                               ui::unsavedChangesDialog(box, "Unsaved", "Changes").reference(dialog))));
      auto async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
      auto outcome = std::make_shared<Outcome>();
      take(async->start(receive(async->context(), box, owner, outcome, false)));
      choose(f, dialog.node(), "Save");
      auto selected = take(box.lastResult());
      take(f.scene->afterDispatch(*f.app));
      check(f.app->scheduler().metrics().data.pending == 1, "result queued before owner/dialog removal");
      take(f.scene->remove(removeDialog ? Node<Widget>(dialog.node()) : Node<Widget>(owner.node())));
      if (prepareBeforeDispatch) {
        take(f.scene->afterDispatch(*f.app));
      }
      f.dispatch();
      std::cout << "queued closure dialog=" << removeDialog << " prepare=" << prepareBeforeDispatch
                << " resumes=" << outcome->resumes << '\n';
      allRevoked = allRevoked && outcome->resumes == 0;
      const auto task = take(async->outcome());
      check(task.status == UiTaskStatus::CANCELLED && task.error == AsyncError::CLOSED,
            "revoked queued delivery closes its exact wait/task without resume");
      check(selected && *selected && **selected == MessageChoice::SAVE, "closure preserves immutable SAVE snapshot");
      if (!removeDialog) {
        check(take(box.lastResult()) == selected, "owner closure preserves lastResult pointer");
      }
      take(f.scene->afterDispatch(*f.app));
      if (!removeDialog) {
        command(box.open([] {
        }));
        command(box.cancel());
      }
    }
  }
  check(allRevoked, "post-latch owner/dialog closure revokes already queued coroutine delivery");
}

void ownerClosed() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Column> owner;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::confirmDialog(box, "Confirm", "Changes"))));
  auto async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto outcome = std::make_shared<Outcome>();
  take(async->start(receive(async->context(), box, owner, outcome, false)));
  take(f.scene->remove(owner.node()));
  flushMessages(f);
  check(outcome->resumes == 1 && outcome->result && !*outcome->result, "independent context receives ownerClosed");
  check(treeCause(outcome->result->error(), TreeError::CLOSED) && outcome->result->error().node == owner.node().id(),
        "full owner error");
}

void ownerFailureRace(bool queued, bool replacement) {
  Fixture f({}, 1);
  ui::MessageBox box;
  ui::Ref<Column> owner;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::confirmDialog(box, "Confirm", "Body"))));
  auto async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto outcome = std::make_shared<Outcome>();
  take(async->start(receive(async->context(), box, owner, outcome, true)));
  SchedulerConnection<int> filler;
  take(f.app->tree().withScope(f.scene->root(), [&](const UiScope &scope) {
    filler = take(f.app->scheduler().connect<int>(scope, [](auto) {
    }));
  }));
  take(filler.publisher.publish(std::make_shared<const int>(0)));
  take(f.scene->remove(owner.node()));
  take(f.scene->afterDispatch(*f.app));
  auto failed = take(box.lastResult());
  check(!*failed && failed->error().phase == ManagedPhase::CLEANUP && failed->error().node == owner.node().id() &&
          treeCause(failed->error(), TreeError::CLOSED) && f.app->scheduler().metrics().data.rejected == 1,
        "full owner failure latches before WOULD_BLOCK transport rejection");
  if (queued) {
    f.dispatch();
    take(f.scene->afterDispatch(*f.app));
  }
  if (replacement) {
    take(async->start(idle(), UiTaskRestart::CANCEL_PREVIOUS));
  } else {
    take(async->cancel());
  }
  flushMessages(f);
  check(outcome->resumes == 0 && take(box.lastResult()) == failed && !*failed &&
          failed->error().node == owner.node().id(),
        "pending/queued full ManagedError pointer/value survives cancel and replacement");
}

void cancellationBeforeLatch(bool failure) {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Column> owner;
  ui::Ref<Dialog> dialog;
  std::unique_ptr<UiAsync> async;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::unsavedChangesDialog(box, "Unsaved", "Changes")
                                             .reference(dialog)
                                             .onDismiss([&](DialogDismissReason) -> std::expected<void, TreeError> {
                                               take(async->cancel());
                                               return failure ? std::expected<void, TreeError>(
                                                                  std::unexpected(TreeError::CALLBACK_FAILED))
                                                              : std::expected<void, TreeError>{};
                                             }))));
  async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto outcome = std::make_shared<Outcome>();
  take(async->start(receive(async->context(), box, owner, outcome, true)));
  choose(f, dialog.node(), "Save");
  auto latched = take(box.lastResult());
  check(!*latched && treeCause(latched->error(), failure ? TreeError::CALLBACK_FAILED : TreeError::CLOSED),
        "postpublication callback cancellation before latch cannot become successful SAVE; original failure wins");
  flushMessages(f);
  check(outcome->resumes == 0 && take(async->outcome()).error == AsyncError::CANCELLED &&
          take(box.lastResult()) == latched,
        "prelatch cancellation has exact task outcome and no late resume");
}

void reservationAndContext() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Column> owner;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::confirmDialog(box, "Confirm", "Body").reference(dialog))));
  auto first = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto second = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto outcome = std::make_shared<Outcome>();
  {
    auto unawaited = take(box.receive(first->context(), owner));
    (void)take(f.app->scheduler().waitFor(std::chrono::milliseconds(0)));
    check(take(f.app->scheduler().waitFor(std::chrono::milliseconds(0))) == SchedulerWake::TIMEOUT,
          "reserved receive has no timer or queued work");
  }
  check(take(f.app->scheduler().waitFor(std::chrono::milliseconds(0))) == SchedulerWake::READY &&
          f.app->scheduler().metrics().data.pending == 0,
        "last unawaited provider lease wakes existing scheduler without a DATA registration/publication");
  f.dispatch();
  take(f.scene->afterDispatch(*f.app));
  check(!take(dialog.opened()), "dispatch cleanup releases reservation without opening UI");
  {
    auto waiting = take(box.receive(first->context(), owner));
    check(!take(dialog.opened()), "reservation is lazy");
    auto duplicate = box.receive(first->context(), owner);
    check(!duplicate && treeCause(duplicate.error(), TreeError::WOULD_BLOCK),
          "copies/reservations do not create new request");
    take(second->start(awaitReserved(waiting, outcome)));
    check(outcome->outer == AsyncError::INVALID_ARGUMENT && outcome->resumes == 0 && !take(dialog.opened()),
          "wrong actual coroutine context fails outer await without starting provider");
  }
  take(f.scene->afterDispatch(*f.app));
  command(box.open([] {
  }));
  command(box.cancel());
  flushMessages(f);
  auto previous = take(box.lastResult());
  auto waiting = take(box.receive(first->context(), owner));
  take(f.scene->remove(owner.node()));
  outcome = std::make_shared<Outcome>();
  take(first->start(awaitReserved(std::move(waiting), outcome)));
  flushMessages(f);
  check(outcome->resumes == 1 && outcome->result && !*outcome->result &&
          treeCause(outcome->result->error(), TreeError::CLOSED) &&
          outcome->result->error().phase == ManagedPhase::CLEANUP,
        "owner closes between reservation and actual await: full domain failure, not outer async flattening");
  check(previous && **previous == MessageChoice::CANCEL, "saved old snapshot survives new reservation");

  Fixture foreign;
  auto other = take(UiAsync::create(foreign.app->scheduler(), foreign.app->tree(), foreign.scene->root()));
  auto rejected = box.receive(other->context());
  check(!rejected && treeCause(rejected.error(), TreeError::WRONG_SESSION),
        "foreign receiver rejected before reservation");
}

void retiredBudget() {
  ManagedOptions options;
  options.maxMessageRequests = 1;
  Fixture f(options);
  ui::MessageBox old, next;
  ui::Ref<OverlayHost> host;
  ui::Ref<Column> owner;
  take(ui::mount(*f.scene, ui::overlayHost(host, ui::column(ui::column(ui::label("Old")).reference(owner)),
                                           ui::confirmDialog(old, "Old", "Body"))));
  auto async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  auto outcome = std::make_shared<Outcome>();
  take(async->start(receive(async->context(), old, owner, outcome, true)));
  take(f.scene->remove(host.node()));
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Replacement"), ui::messageBox(next, "Next", "Body"))));
  auto limited = next.open([] {
  });
  check(!limited && treeCause(limited.error(), TreeError::NODE_LIMIT),
        "retired live receiver still consumes request budget");
  flushMessages(f);
  check(outcome->resumes == 1 && outcome->result && !*outcome->result &&
          treeCause(outcome->result->error(), TreeError::STALE_NODE),
        "removed layer retains one full result for independent live receiver");
  command(next.open([] {
  }));
  command(next.cancel());
}
} // namespace
int main() try {
  normalAndCancel(false, false, false);
  normalAndCancel(true, false, false);
  normalAndCancel(true, true, false);
  normalAndCancel(true, false, true);
  normalAndCancel(true, true, true);
  queuedOwnerOrDialogClosed();
  ownerClosed();
  normalAndCancel(false, false, false, true);
  normalAndCancel(true, false, false, true);
  normalAndCancel(true, false, true, true);
  normalAndCancel(true, false, false, false, true);
  normalAndCancel(true, false, true, false, true);
  normalAndCancel(false, false, false, false, true);
  normalAndCancel(true, false, false, true, false, true);
  normalAndCancel(true, false, true, true, false, true);
  normalAndCancel(true, false, false, false, true, true);
  normalAndCancel(true, true, false, false, false, true);
  cancellationBeforeLatch(false);
  cancellationBeforeLatch(true);
  reservationAndContext();
  retiredBudget();
  ownerFailureRace(false, false);
  ownerFailureRace(false, true);
  ownerFailureRace(true, false);
  ownerFailureRace(true, true);
  std::cout << "message-box-async: normal/cancel/replacement/copied-await/queued/ownerClosed PASS\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
