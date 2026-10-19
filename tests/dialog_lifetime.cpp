// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
void take(ui::DialogCommandResult result, std::source_location where = std::source_location::current()) {
  take(result.result(), where);
}

void publishedErrorsAndOnceOnly() {
  Fixture f;
  ui::Ref<Widget> background, inside;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::custom<Widget>().reference(background),
                                           ui::dialog(dialog, "Errors", ui::custom<Widget>().reference(inside)))));
  int lost{}, gained{}, dismissed{};
  auto backgroundBinding = take(f.app->router().bind(
    background.node(),
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool active) -> std::expected<void, TreeError> {
      if (!active) {
        ++lost;
        return std::unexpected(TreeError::NO_MEMORY);
      }
      return {};
    }));
  auto insideBinding = take(f.app->router().bind(
    inside.node(),
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool active) -> std::expected<void, TreeError> {
      ++gained;
      return active ? std::expected<void, TreeError>{} : std::unexpected(TreeError::CALLBACK_FAILED);
    }));
  take(f.scene->onDismiss(dialog.node(), [&](DialogDismissReason reason) -> std::expected<void, TreeError> {
    check(reason == DialogDismissReason::PROGRAMMATIC, "exact dismissal reason");
    ++dismissed;
    return std::unexpected(TreeError::NODE_LIMIT);
  }));
  take(f.app->router().setFocus(background.node()));
  auto opened = f.scene->openDialog(dialog.node());
  check(!opened && treeCause(opened.error(), TreeError::NO_MEMORY), "first blur failure retained");
  check(take(dialog.opened()) && lost == 1 && gained == 1, "open published and both focus callbacks attempted");
  auto closed = f.scene->dismissDialog(dialog.node());
  check(!closed && treeCause(closed.error(), TreeError::CALLBACK_FAILED), "dismiss preserves first focus failure");
  check(!take(dialog.opened()) && gained == 2 && dismissed == 1, "dismiss consumes receipt before every callback");
  take(f.scene->dismissDialog(dialog.node()));
  check(dismissed == 1, "retry does not replay user effects");
}

void admittedRemovalAndNestedCommand() {
  Fixture f;
  ui::Ref<Dialog> dialog;
  ui::Ref<Button> background, inside;
  int callbacks{};
  bool nestedRejected{}, aliveDuring{};
  auto marker = std::make_shared<int>(7);
  std::weak_ptr<int> weak = marker;
  take(ui::mount(*f.scene,
                 ui::overlayHost(ui::button("Open").reference(background).onClick([] {
                 }),
                                 ui::dialog(dialog, "Self removal", ui::button("Inside").reference(inside).onClick([] {
                                 }))
                                   .onDismiss([&, owner = marker](DialogDismissReason) {
                                     ++callbacks;
                                     auto nested = f.scene->openDialog(dialog.node());
                                     nestedRejected = !nested && treeCause(nested.error(), TreeError::WOULD_BLOCK);
                                     take(background.focus());
                                     take(f.scene->remove(dialog.node()));
                                     aliveDuring = owner && !weak.expired();
                                   }))));
  marker.reset();
  take(background.focus());
  take(dialog.open());
  take(dialog.dismiss());
  check(callbacks == 1 && nestedRejected && aliveDuring, "admitted owner survives self-removal and rejects recursion");
  check(weak.expired(), "removed layer releases captures after admitted return");
  auto stale = dialog.dismiss();
  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE), "new stale command cannot replay dismissal");
}

void revokedBeforeAdmission() {
  Fixture f;
  ui::Ref<Widget> background, inside;
  ui::Ref<Dialog> dialog;
  int dismissals{};
  take(ui::mount(*f.scene, ui::overlayHost(ui::custom<Widget>().reference(background),
                                           ui::dialog(dialog, "Removed on blur", ui::custom<Widget>().reference(inside))
                                             .onDismiss([&](DialogDismissReason) {
                                               ++dismissals;
                                             }))));
  auto backgroundBinding = take(f.app->router().bind(background.node(), [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  }));
  auto insideBinding = take(f.app->router().bind(
    inside.node(),
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool active) -> std::expected<void, TreeError> {
      if (!active) {
        take(f.app->tree().remove(dialog.node()));
      }
      return {};
    }));
  take(background.focus());
  take(dialog.open());
  take(dialog.dismiss());
  check(dismissals == 0, "revoked before onDismiss admission skips callback");
}

void staleOpenerAndClose() {
  Fixture f;
  ui::Ref<Dialog> dialog;
  ui::Ref<Button> opener, fallback;
  ui::Ref<Column> body;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::button("First").reference(opener).onClick([] {
                                           }),
                                                      ui::button("Fallback").reference(fallback).onClick([] {
                                                      }))
                                             .reference(body),
                                           ui::dialog(dialog, "Return", ui::button("Done").onClick([] {
                                           })))));
  take(opener.focus());
  take(dialog.open());
  const auto old = opener.node().id();
  take(f.scene->remove(opener.node()));
  auto replacement = take(f.scene->emplace<Button>(body.node(), take(GraphemeText::from("Replacement"))));
  take(f.scene->onClick(replacement, [] {
  }));
  check(replacement.id() != old, "reused storage has distinct complete identity");
  take(dialog.dismiss());
  check(take(f.app->router().focused()) == fallback.node(), "stale exact opener falls back without slot aliasing");
  int dismissals{};
  take(f.scene->onDismiss(dialog.node(), [&](DialogDismissReason) {
    ++dismissals;
  }));
  take(dialog.open());
  take(f.scene->close());
  auto closed = dialog.open();
  check(!closed && treeCause(closed.error(), TreeError::CLOSED) && dismissals == 0,
        "teardown invokes no dismissal action");
}

void shortFailureAndFacade() {
  example::ReplayTerminal terminal({{10, "\r", {}}}, 100, VIEWPORT);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Dialog> dialog;
  ManagedScene *managed{};
  int notified{};
  auto scene = ui::scene(
    [&](ManagedScene &owner) {
      managed = &owner;
      return ui::overlayHost(ui::button("Trigger").onClick([&] {
        take(managed->remove(dialog.node()));
        dialog.open();
      }),
                             ui::dialog(dialog, "Retired", ui::label("body")));
    },
    options);
  scene.onError([&](const ManagedDiagnostic &) {
    ++notified;
  });
  auto result = scene.run(terminal);
  std::cerr << "short run success=" << bool(result) << "; notified=" << notified;
  if (!result) {
    const auto *actual = std::get_if<TreeError>(&result.error().cause);
    std::cerr << "; phase=" << static_cast<int>(result.error().phase)
              << "; cause=" << (actual ? static_cast<int>(*actual) : -1);
  }
  std::cerr << '\n';
  check(!result && treeCause(result.error(), TreeError::STALE_NODE), "ignored short command preserves exact failure");
  check(notified == 1 && take(managed->diagnostics()).errors.size() == 1, "short command reports exactly once");

  example::ReplayTerminal successful({{10, "\r", {}}, {30, "\x1b", {}}}, 100, VIEWPORT);
  options.clock = successful.clock();
  ui::Ref<Dialog> confirmation;
  bool currentFacade{}, returned{};
  auto owner = ui::scene(
    ui::overlayHost(
      ui::button("Open").onClick([&] {
        confirmation.open();
      }),
      ui::dialog(confirmation, "Confirm", ui::label("Press Escape")).onDismiss([&](DialogDismissReason reason) {
        currentFacade = !ui::detail::CurrentSceneScope::snapshot().expired();
        check(reason == DialogDismissReason::CANCEL, "root escape reason retained");
        ui::quit();
        returned = true;
      })),
    options);
  take(owner.run(successful));
  check(currentFacade && returned, "dismiss callback retains current facade and returns after short quit");
}

void suppressBackgroundShortcuts() {
  example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\x1bOQ", {}}, {30, "\x1b", {}}, {80, "\x1bOQ", {}}}, 120,
                                   VIEWPORT);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Dialog> dialog;
  int shortcuts{};
  auto scene = ui::scene(ui::overlayHost(ui::button("Open").onClick([&] {
                           dialog.open();
                         }),
                                         ui::dialog(dialog, "Modal", ui::button("Inside").onClick([] {
                                         }))),
                         options);
  scene.onKey("F2", [&] {
    ++shortcuts;
    ui::quit();
  });
  take(scene.run(terminal));
  check(shortcuts == 1, "modal suppresses scene-global shortcut before router; dismissal restores it");
}

void facadeObserverReentry() {
  example::ReplayTerminal terminal({{10, "\r", {}}}, 100, VIEWPORT);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Dialog> dialog;
  ManagedScene *managed{};
  int primaryNotifications{};
  bool rejectedClear{};
  auto scene = ui::scene(
    [&](ManagedScene &owner) {
      managed = &owner;
      return ui::overlayHost(ui::button("Trigger").onClick([&] {
        take(managed->remove(dialog.node()));
        dialog.open();
      }),
                             ui::dialog(dialog, "Retired", ui::label("body")));
    },
    options);
  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    if (treeCause(diagnostic.error, TreeError::STALE_NODE)) {
      ++primaryNotifications;
      auto clear = managed->clearErrors();
      rejectedClear = !clear && clear.error() == TreeError::WOULD_BLOCK;
      scene.onError([](const ManagedDiagnostic &) {
      });
      ManagedError secondary{ManagedPhase::BIND, {}, TreeError::NODE_LIMIT, {}, {}};
      secondary.operation = "observer.secondary";
      take(managed->reportError(secondary));
    }
  });
  auto result = scene.run(terminal);
  check(!result && treeCause(result.error(), TreeError::STALE_NODE) && result.error().phase == ManagedPhase::INPUT &&
          result.error().node == dialog.node().id() && result.error().operation &&
          std::string_view(result.error().operation) == "Dialog.open",
        "observer configuration rejection cannot replace facade primary");
  const auto diagnostics = take(managed->diagnostics());
  check(primaryNotifications == 1 && rejectedClear && diagnostics.errors.size() == 2 &&
          treeCause(diagnostics.errors[0].error, TreeError::STALE_NODE) &&
          treeCause(diagnostics.errors[1].error, TreeError::NODE_LIMIT) && diagnostics.errors[1].notificationSuppressed,
        "primary actual slot and distinct suppressed secondary survive finish");
}

void primaryHistory(bool full, bool clear, bool observer) {
  ManagedOptions options;
  if (full) {
    options.diagnostics.maxErrors = 1;
  }
  Fixture f(options);
  ui::Ref<Button> trigger;
  ui::Ref<Dialog> dialog;
  std::optional<ManagedError> returned;

  struct Stamp {
    std::optional<ManagedError> latched;
  };

  auto stamp = std::make_shared<Stamp>();
  const auto primaryOrigin = std::source_location::current();
  const auto secondaryOrigin = std::source_location::current();
  int primaryNotifications{};
  bool noReportingAllocation{};
  take(ui::mount(*f.scene, ui::overlayHost(ui::button("Trigger").reference(trigger).onClick([&] {
    take(f.scene->remove(dialog.node()));
    ui::detail::SetupFailureScope collector(
      stamp,
      [](void *owner, ui::detail::SetupFailureEvent event, ManagedError *failure, const void *) noexcept {
        if (event == ui::detail::SetupFailureEvent::LATCH) {
          static_cast<Stamp *>(owner)->latched = *failure;
        }
      },
      "dialog-test.callback");
    const auto before = allocationTest::count;
    allocationTest::remaining = 0;
    auto result = dialog.open(primaryOrigin);
    if (!result) {
      returned = result.error();
    }
    allocationTest::remaining = -1;
    noReportingAllocation = allocationTest::count == before;
    if (clear) {
      take(f.scene->clearErrors());
      ManagedError secondary{ManagedPhase::BIND, trigger.node().id(), TreeError::NODE_LIMIT, {}, {}};
      secondary.operation = "secondary.after-clear";
      take(f.scene->reportError(secondary, {}, secondaryOrigin));
    }
  }),
                                           ui::dialog(dialog, "Retired", ui::label("body")))));
  if (full) {
    ManagedError prior{ManagedPhase::BIND, trigger.node().id(), TreeError::INVALID_ARGUMENT, {}, {}};
    prior.operation = "prior.full-history";
    take(f.scene->reportError(prior, {}, secondaryOrigin));
  }
  if (observer) {
    take(f.scene->onError([&](const ManagedDiagnostic &diagnostic) {
      if (treeCause(diagnostic.error, TreeError::STALE_NODE)) {
        ++primaryNotifications;
        check(diagnostic.contexts.empty() && !diagnostic.contextUnavailable && diagnostic.error.execution &&
                std::string_view(diagnostic.error.execution) == "dialog-test.callback" && diagnostic.error.setupStopped,
              "primary intentionally omits labels and retains collector stamps");
      }
    }));
  }
  take(f.app->router().invoke(trigger.node(), ActionEvent{Action::ACTIVATE}));
  check(returned && stamp->latched && noReportingAllocation && returned->setupStopped &&
          returned->execution == stamp->latched->execution,
        "short error transport/stamping allocates nothing");
  auto result = f.scene->run(f.terminal);
  check(!result && treeCause(result.error(), TreeError::STALE_NODE) && result.error().node == dialog.node().id() &&
          result.error().execution == returned->execution && result.error().setupStopped &&
          result.error().operationOrigin->line() == primaryOrigin.line(),
        "retained primary equals stamped returned failure after finish");
  const auto diagnostics = take(f.scene->diagnostics());
  check(primaryNotifications == (observer ? 1 : 0) && diagnostics.errors.size() == 1,
        "full/revoked/absent-observer primary never records or notifies twice");
  const auto &entry = diagnostics.errors.front();
  check(treeCause(entry.error, clear  ? TreeError::NODE_LIMIT
                               : full ? TreeError::INVALID_ARGUMENT
                                      : TreeError::STALE_NODE) &&
          entry.origin.line() == (clear || full ? secondaryOrigin.line() : primaryOrigin.line()),
        "finish updates only the actual live primary slot, never a cleared/reused slot");
}

void nestedAdmissionAndOutsideResponsibility() {
  {
    Fixture f;
    ui::Ref<Button> outer;
    ui::Ref<Dialog> retired, nested;
    bool resumed{}, nestedReturned{};
    take(ui::mount(*f.scene,
                   ui::overlayHost(ui::button("Outer").reference(outer).onClick([&] {
                     take(nested.open());
                     take(nested.dismiss());
                     retired.open();
                     resumed = true;
                   }),
                                   ui::dialog(retired, "Retired", ui::label("body")),
                                   ui::dialog(nested, "Nested", ui::label("body")).onDismiss([&](DialogDismissReason) {
                                     nestedReturned = true;
                                   }))));
    take(f.scene->remove(retired.node()));
    take(f.app->router().invoke(outer.node(), ActionEvent{Action::ACTIVATE}));
    auto result = f.scene->run(f.terminal);
    check(resumed && nestedReturned && !result && treeCause(result.error(), TreeError::STALE_NODE),
          "nested action restores outer admission and stop waits for admitted return");
  }
  {
    Fixture receiver;
    ui::Ref<Dialog> retired;
    take(ui::mount(*receiver.scene,
                   ui::overlayHost(ui::label("Body"), ui::dialog(retired, "Retired", ui::label("body")))));
    take(receiver.scene->remove(retired.node()));
    auto outside = retired.open();
    check(!outside && treeCause(outside.error(), TreeError::STALE_NODE), "outside result remains explicitly checked");
    Fixture sender;
    ui::Ref<Button> trigger;
    take(ui::mount(*sender.scene, ui::button("Other owner").reference(trigger).onClick([&] {
      retired.open();
    })));
    take(sender.app->router().invoke(trigger.node(), ActionEvent{Action::ACTIVATE}));
    take(sender.scene->run(sender.terminal));
    take(receiver.scene->run(receiver.terminal));
    check(take(sender.scene->diagnostics()).errors.empty() && take(receiver.scene->diagnostics()).errors.size() == 2,
          "another owner's admitted callback does not promote or attribute receiver failures to its sender");
  }
}

void shortFailureInKeyOrDismiss(bool fromDismiss) {
  auto input = fromDismiss ? std::vector<example::ReplayTerminal::Input>{{10, "\r", {}}, {30, "\x1b", {}}}
                           : std::vector<example::ReplayTerminal::Input>{{10, "\x1bOQ", {}}};
  example::ReplayTerminal terminal(std::move(input), 100, VIEWPORT);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ManagedScene *managed{};
  ui::Ref<Dialog> active, retired;
  bool returned{};
  int notified{};
  const auto fail = [&] {
    take(managed->remove(retired.node()));
    retired.open();
    returned = true;
  };
  auto scene = ui::scene(
    [&](ManagedScene &owner) {
      managed = &owner;
      return ui::overlayHost(ui::button("Open").onClick([&] {
        active.open();
      }),
                             ui::dialog(active, "Active", ui::label("Escape")).onDismiss([&](DialogDismissReason) {
                               if (fromDismiss) {
                                 fail();
                               }
                             }),
                             ui::dialog(retired, "Retired", ui::label("body")));
    },
    options);
  scene.onKey("F2", fail);
  scene.onError([&](const ManagedDiagnostic &) {
    ++notified;
  });
  auto result = scene.run(terminal);
  check(returned && !result && treeCause(result.error(), TreeError::STALE_NODE) &&
          result.error().node == retired.node().id() && notified == 1 &&
          take(managed->diagnostics()).errors.size() == 1,
        "short key/dismiss failure keeps exact primary and reports once after callback returns");
}

void cancelTypedFailure(bool full = false) {
  example::ReplayTerminal terminal({{10, "\r", {}}, {30, "\x1b", {}}, {80, "\x1b", {}}}, 120, VIEWPORT);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  if (full) {
    options.managed.diagnostics.maxErrors = 1;
  }
  ManagedScene *managed{};
  ui::Ref<Dialog> dialog;
  int dismissed{}, notified{};
  bool published{};
  auto scene = ui::scene(
    [&](ManagedScene &owner) {
      managed = &owner;
      return ui::overlayHost(ui::button("Open").onClick([&] {
        if (full) {
          take(managed->reportError(
            ManagedError{ManagedPhase::BIND, dialog.node().id(), TreeError::INVALID_ARGUMENT, {}, {}}));
        }
        dialog.open();
      }),
                             ui::dialog(dialog, "Failure", ui::label("Escape"))
                               .onDismiss([&](DialogDismissReason reason) -> std::expected<void, TreeError> {
                                 ++dismissed;
                                 published = reason == DialogDismissReason::CANCEL && !take(dialog.opened());
                                 return std::unexpected(TreeError::NODE_LIMIT);
                               }));
    },
    options);
  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    if (treeCause(diagnostic.error, TreeError::NODE_LIMIT)) {
      ++notified;
      throw std::bad_alloc();
    }
  });
  auto result = scene.run(terminal);
  const auto diagnostics = take(managed->diagnostics());
  std::cout << "typed CANCEL full=" << full << "; notifications=" << notified
            << "; dropped=" << diagnostics.droppedErrors << '\n';
  check(!result && treeCause(result.error(), TreeError::NODE_LIMIT) && result.error().phase == ManagedPhase::INPUT &&
          result.error().node == dialog.node().id() && published && dismissed == 1 && notified == 1,
        "Escape typed dismissal error stops once after publication and survives failing error observer");
  check(diagnostics.errors.size() == 1 && diagnostics.observerFailures == 1 &&
          (!full || (diagnostics.droppedErrors == 1 &&
                     treeCause(diagnostics.errors.front().error, TreeError::INVALID_ARGUMENT))),
        "typed CANCEL failure retains one diagnostic and records observer failure independently");
}

void idleHostFrames(bool open) {
  auto input =
    open ? std::vector<example::ReplayTerminal::Input>{{50, "\r", {}}} : std::vector<example::ReplayTerminal::Input>{};
  example::ReplayTerminal terminal(std::move(input), 5000, VIEWPORT);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Dialog> dialog;
  auto scene = ui::scene(ui::overlayHost(ui::button("Open").onClick([&] {
                           dialog.open();
                         }),
                                         ui::dialog(dialog, "Idle", ui::label("Retained content"))),
                         options);
  const auto result = take(scene.run(terminal));
  std::cout << "idle host open=" << open << "; frames=" << result.frames << '\n';
  check(result.frames == (open ? 2u : 1u) && terminal.closed,
        "visible or hidden retained dialog causes no idle redraw and closes its finite terminal");
}

void suppressedRuntimePrimary(bool full) {
  ManagedOptions options;
  if (full) {
    options.diagnostics.maxErrors = 1;
  }
  Fixture f(options);
  ui::Ref<Button> trigger;
  ui::Ref<Dialog> retired;
  bool returned{};
  int observed{};
  take(ui::mount(*f.scene, ui::overlayHost(ui::button("Trigger").reference(trigger).onClick([&] {
    retired.open();
    returned = true;
  }),
                                           ui::dialog(retired, "Retired", ui::label("body")))));
  take(f.scene->remove(retired.node()));
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++observed;
    take(f.app->router().invoke(trigger.node(), ActionEvent{Action::ACTIVATE}));
  }));
  ManagedError prior{ManagedPhase::BIND, {}, TreeError::NODE_LIMIT, {}, {}};
  take(f.scene->reportError(prior));
  auto result = f.scene->run(f.terminal);
  check(returned && !result && treeCause(result.error(), TreeError::WOULD_BLOCK) &&
          result.error().node == retired.node().id() && observed == 1,
        "new primary suppressed inside an existing observer is not notified again by finish");
  const auto diagnostics = take(f.scene->diagnostics());
  check(diagnostics.errors.size() == (full ? 1u : 2u) &&
          treeCause(diagnostics.errors.front().error, TreeError::NODE_LIMIT) &&
          diagnostics.suppressedNotifications == 1,
        "nested suppression/full history preserves original distinct diagnostic identities");
}
} // namespace

int main() {
  try {
    publishedErrorsAndOnceOnly();
    admittedRemovalAndNestedCommand();
    revokedBeforeAdmission();
    staleOpenerAndClose();
    shortFailureAndFacade();
    suppressBackgroundShortcuts();
    facadeObserverReentry();
    primaryHistory(false, false, true);
    primaryHistory(true, false, true);
    primaryHistory(false, true, true);
    primaryHistory(false, false, false);
    primaryHistory(true, false, false);
    nestedAdmissionAndOutsideResponsibility();
    shortFailureInKeyOrDismiss(false);
    shortFailureInKeyOrDismiss(true);
    cancelTypedFailure();
    cancelTypedFailure(true);
    idleHostFrames(false);
    idleHostFrames(true);
    suppressedRuntimePrimary(false);
    suppressedRuntimePrimary(true);
    std::cout << "dialog lifetime/first-error/once-only passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
