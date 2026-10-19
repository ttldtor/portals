// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/message_fixture.hpp"

namespace {
void ownerRemoval(bool chosen) {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Column> owner;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::unsavedChangesDialog(box, "Unsaved", "Changes").reference(dialog))));
  int callbacks{};
  command(box.open(owner, [&] {
    ++callbacks;
  }));
  std::shared_ptr<const MessageResult> saved;
  if (chosen) {
    choose(f, dialog.node(), "Save");
    saved = answer(box, MessageChoice::SAVE);
  }
  take(f.scene->remove(owner.node()));
  flushMessages(f);
  check(callbacks == 0, "removed owner revokes callback");
  auto result = take(box.lastResult());
  if (chosen) {
    check(result == saved && **result == MessageChoice::SAVE, "removal after latch preserves pointer/value");
  } else {
    check(!*result && treeCause(result->error(), TreeError::CLOSED) && result->error().node == owner.node().id(),
          "owner failure is full technical result");
  }
  check(!take(dialog.opened()), "orphan panel safely dismissed");
}

void admittedRemoval() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::messageBox(box, "Info", "Body").reference(dialog))));
  int calls{};
  command(box.open([&](const MessageResult &result) {
    ++calls;
    take(f.scene->remove(dialog.node()));
    check(result && *result == MessageChoice::ACKNOWLEDGE, "admitted result survives own dialog removal");
    auto busy = box.open([] {
    });
    check(!busy, "removed facade cannot reopen");
  }));
  choose(f, dialog.node(), "OK");
  auto snapshot = answer(box, MessageChoice::ACKNOWLEDGE);
  flushMessages(f);
  check(calls == 1 && **snapshot == MessageChoice::ACKNOWLEDGE, "admitted callback entered exactly once");
}

void structuralBodyRemoval() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Column> body;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::label("Body")).reference(body),
                                           ui::confirmDialog(box, "Confirm", "Body").reference(dialog))));
  int calls{};
  command(box.open(body, [&] {
    ++calls;
  }));
  take(f.scene->remove(body.node()));
  auto cleanup = f.scene->afterDispatch(*f.app);
  check(!cleanup && cleanup.error() == TreeError::STALE_NODE, "existing P22 exact-body cleanup error preserved");
  auto snapshot = take(box.lastResult());
  check(!*snapshot && snapshot->error().phase == ManagedPhase::CLEANUP && snapshot->error().node == body.node().id() &&
          treeCause(snapshot->error(), TreeError::CLOSED),
        "ownerClosed domain survives independent cleanup failure");
  f.dispatch();
  check(calls == 0 && take(dialog.opened()), "failed cleanup preserves committed picture and revokes result callback");
}

void publishedOpenFailure() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Widget> background;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::custom<Widget>().reference(background),
                                           ui::confirmDialog(box, "Confirm", "Body").reference(dialog))));
  auto binding = take(f.app->router().bind(
    background.node(),
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [](Widget &, bool active) -> std::expected<void, TreeError> {
      return active ? std::expected<void, TreeError>{} : std::unexpected(TreeError::NO_MEMORY);
    }));
  take(f.app->router().setFocus(background.node()));
  int calls{};
  auto opened = box.open([&](const MessageResult &result) {
    check(!result && treeCause(result.error(), TreeError::NO_MEMORY), "full focus failure delivered");
    ++calls;
  });
  check(!opened && treeCause(opened.error(), TreeError::NO_MEMORY) && take(dialog.opened()),
        "postpublication open error retains committed panel");
  auto snapshot = take(box.lastResult());
  check(!*snapshot && treeCause(snapshot->error(), TreeError::NO_MEMORY), "original open failure domain latched");
  command(box.cancel());
  check(!take(dialog.opened()) && take(box.lastResult()) == snapshot,
        "explicit cancel cleans failed-open panel without replacing failure");
  flushMessages(f);
  check(calls == 1 && take(f.scene->diagnostics()).errors.size() == 1,
        "failed-open outcome and diagnostic each delivered once");
}

void removalBeforeLatch(bool failure) {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Column> owner;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::unsavedChangesDialog(box, "Unsaved", "Body")
                                             .reference(dialog)
                                             .onDismiss([&](DialogDismissReason) -> std::expected<void, TreeError> {
                                               take(f.scene->remove(owner.node()));
                                               return failure ? std::expected<void, TreeError>(
                                                                  std::unexpected(TreeError::NODE_LIMIT))
                                                              : std::expected<void, TreeError>{};
                                             }))));
  int calls{};
  command(box.open(owner, [&] {
    ++calls;
  }));
  choose(f, dialog.node(), "Save");
  auto snapshot = take(box.lastResult());
  check(!*snapshot && treeCause(snapshot->error(), failure ? TreeError::NODE_LIMIT : TreeError::CLOSED),
        "postpublication owner removal before latch cannot become SAVE; original command failure wins");
  check(snapshot->error().phase == (failure ? ManagedPhase::INPUT : ManagedPhase::CLEANUP),
        "exact failure boundary priority");
  flushMessages(f);
  check(calls == 0 && take(box.lastResult()) == snapshot, "removed owner never admits pending business callback");
}

void nestedDeliveryAndRevocation() {
  Fixture f;
  ui::MessageBox first, second;
  ui::Ref<Dialog> one, two;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::messageBox(first, "First", "Body").reference(one),
                                           ui::confirmDialog(second, "Second", "Body").reference(two))));
  int calls{};
  command(first.open([&] {
    ++calls;
    command(second.open([] {
    }));
  }));
  choose(f, one.node(), "OK");
  flushMessages(f);
  check(calls == 1 && take(two.opened()) && take(f.app->router().focused()) == findButton(f, two.node(), "Cancel"),
        "other dialog can open from later admitted result callback using existing focus transaction");
  command(second.cancel());
  flushMessages(f);
  command(first.open([] {
  }));
  take(f.app->tree().setVisible(one.node(), false));
  flushMessages(f);
  auto revoked = take(first.lastResult());
  check(!*revoked && treeCause(revoked->error(), TreeError::CLOSED),
        "raw presentation revocation is a technical failure, never fake CANCEL");
}
} // namespace
int main() try {
  ownerRemoval(false);
  ownerRemoval(true);
  admittedRemoval();
  structuralBodyRemoval();
  publishedOpenFailure();
  removalBeforeLatch(false);
  removalBeforeLatch(true);
  nestedDeliveryAndRevocation();
  std::cout << "message-box-lifetime: owner removal before/after latch and admitted self-removal PASS\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
