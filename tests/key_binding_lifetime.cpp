// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/key_binding_state.hpp"
#include "support/managed_text_fixture.hpp"

namespace {
void lifetimeAndScope() {
  Fixture f;
  ui::Ref<Column> group;
  ui::Ref<Button> editor;
  take(ui::mount(*f.scene, ui::column(ui::button(editor, "Editor",
                                                 [] {
                                                 }))
                             .reference(group)));
  auto scope = take(f.app->router().focusScope(group.node(), editor.node()));
  take(f.scene->focus(editor.node()));
  KeyInfo info(TextValue("Scoped"));
  info.owner = editor.node();
  info.scope = group.node();
  int calls{};
  auto binding = take(f.scene->onKey(ui::press("Ctrl+S"), std::move(info), [&] {
    ++calls;
  }));
  auto cached = take(f.scene->keyBindings());
  check(cached.bindings.back().eligibility == KeyEligibility::ELIGIBLE, "scoped projection eligible");
  const auto focus = cached.focusGeneration;
  const auto input = cached.inputGeneration;
  std::thread worker([token = std::move(scope)]() mutable {
    token.disconnect();
  });
  worker.join();
  auto disconnected = take(f.scene->keyBindings());
  check(focus == disconnected.focusGeneration && input == disconnected.inputGeneration,
        "disconnect observation has no invented UI generation");
  check(disconnected.scopeGeneration != cached.scopeGeneration &&
          disconnected.bindings.back().eligibility == KeyEligibility::SCOPE_INACTIVE,
        "exact cancellation state invalidates cached owning projection");
  auto replacement = take(f.app->router().focusScope(group.node(), editor.node()));
  check(take(f.scene->keyBindings()).bindings.back().eligibility == KeyEligibility::SCOPE_INACTIVE,
        "same-root scope re-registration cannot revive old cancellation identity");
  bool wrong{};
  std::thread foreign([&] {
    auto failed = binding.setVisible(false);
    wrong = !failed && treeCause(failed.error(), TreeError::WRONG_THREAD);
  });
  foreign.join();
  check(wrong, "immutable affinity checked before mutable binding observation");
  take(f.scene->remove(editor.node()));
  check(treeCause(binding.setEnabled(false).error(), TreeError::STALE_NODE), "owner retirement is permanent");
}

Fixture *hookFixture{};
KeyBinding *hookBinding{};
Node<Widget> hookOwner;
Node<Widget> hookFocus;
bool hookRan{}, hookBlocked{};
int hookMode{};

void stagingHook() {
  hookRan = true;
  if (hookMode == 1) {
    auto removed = hookFixture->app->tree().remove(hookOwner);
    hookBlocked = bool(removed);
  } else if (hookMode == 2) {
    hookBlocked = bool(hookFixture->app->router().setFocus(hookFocus));
  } else {
    auto removed = hookBinding->remove();
    auto replaced = hookFixture->scene->onInput([](const InputEvent &) {
      return std::expected<bool, ManagedError>(false);
    });
    hookBlocked = !removed && treeCause(removed.error(), TreeError::WOULD_BLOCK) && !replaced &&
                  treeCause(replaced.error(), TreeError::WOULD_BLOCK);
  }
}

void stagingReentry(int mode) {
  bool exhausted{};
  for (std::ptrdiff_t point{}; point < 512; ++point) {
    Fixture f;
    auto owner = f.node();
    auto focus = f.node();
    auto focusBinding = take(f.app->router().bind(focus, [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    }));
    auto old = take(f.scene->onKey(ui::press("A"), KeyInfo(TextValue("Existing")), [] {
    }));
    hookFixture = &f;
    hookBinding = &old;
    hookOwner = owner;
    hookFocus = focus;
    hookMode = mode;
    hookRan = hookBlocked = false;
    KeyInfo info(i18n("outer"));
    info.owner = owner;
    info.resolver = [point](std::string_view) -> std::expected<std::string, TextResourceError> {
      allocationTest::hookAfter = point;
      allocationTest::hook = stagingHook;
      return std::string("Outer");
    };
    auto result = f.scene->onKey(ui::press("B"), std::move(info), [] {
    });
    allocationTest::hook = nullptr;
    if (!hookRan) {
      exhausted = true;
      break;
    }
    check(hookBlocked, "one-shot staging hook reaches admitted guard/raw tree work");
    if (mode == 1) {
      check(!result && treeCause(result.error(), TreeError::STALE_NODE),
            "raw owner retirement rejects stale outer publication");
    } else if (mode == 2) {
      check(!result && treeCause(result.error(), TreeError::WOULD_BLOCK),
            "raw focus mutation rejects an outdated outer publication stamp");
    } else {
      check(result.has_value(), "guarded inner mutations cannot overwrite outer coherent publication");
    }
    check(take(f.scene->keyBindings()).bindings.front().id == old.id(),
          "original binding never resurrected or overwritten");
  }
  check(exhausted, "finite complete staging allocation-hook sweep");
}

void selfRemoval() {
  Fixture f;
  std::optional<KeyBinding> binding;
  int calls{};
  binding =
    take(f.scene->onKey(ui::press("A"), KeyInfo(TextValue("Remove self")), [&]() -> std::expected<void, ManagedError> {
      ++calls;
      return binding->remove();
    }));
  take(f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, U'a', {}, KeyAction::PRESS}));
  check(calls == 1 && take(f.scene->keyBindings()).bindings.empty(),
        "entered callable pinned through own canonical removal");
}

void transientOwnerAndConservativeReset() {
  Fixture f;
  ui::Ref<Column> group;
  ui::Ref<Button> editor;
  take(ui::mount(*f.scene, ui::column(ui::button(editor, "Editor",
                                                 [] {
                                                 }))
                             .reference(group)));
  auto scope = take(f.app->router().focusScope(group.node(), editor.node()));
  take(f.scene->focus(editor.node()));
  int calls{};
  KeyInfo scoped(TextValue("Scoped sibling"));
  scoped.scope = group.node();
  scoped.owner = editor.node();
  take(f.scene->onKey(ui::press("Ctrl+K, Ctrl+U"), std::move(scoped), [] {
  }));
  KeyInfo global(TextValue("Global sequence"));
  global.owner = editor.node();
  take(f.scene->onKey(ui::press("Ctrl+K, Ctrl+C"), std::move(global), [&] {
    ++calls;
  }));
  const auto stroke = [&](char32_t key) {
    return take(f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, key, {.control = true}, KeyAction::PRESS}));
  };
  check(stroke(U'k'), "global sequence starts while scoped sibling is eligible");
  scope.disconnect();
  check(!stroke(U'c') && calls == 0, "one scoped disconnect conservatively clears every existing pending candidate");
  check(stroke(U'k') && stroke(U'c') && calls == 1, "unscoped command remains usable after cancellation observation");
  take(f.app->tree().setEnabled(group.node(), false));
  check(take(f.scene->keyBindings()).bindings.back().eligibility == KeyEligibility::OWNER_INELIGIBLE,
        "original owner inherits ancestor disable without retiring identity");
  take(f.app->tree().setEnabled(group.node(), true));
  take(f.app->tree().setVisible(group.node(), false));
  check(!stroke(U'k'), "hidden original owner never routes");
  take(f.app->tree().setVisible(group.node(), true));
  check(stroke(U'k') && stroke(U'c') && calls == 2,
        "transient hide/disable does not permanently retire the original owner");
}

struct RetireAction {
  ManagedScene *scene;
  bool *called;
  bool *admitted;

  ~RetireAction() {
    *called = true;
    try {
      *admitted = bool(scene->onKey(ui::press("B"), KeyInfo(TextValue("After retirement")), [] {
      }));
    } catch (...) {
      *admitted = false;
    }
  }
};

void postCommitDestruction() {
  Fixture f;
  bool called{}, admitted{};
  auto capture = std::shared_ptr<RetireAction>(new RetireAction{f.scene.get(), &called, &admitted});
  auto binding = take(f.scene->onKey(ui::press("A"), KeyInfo(TextValue("Retire")), [capture] {
  }));
  capture.reset();
  take(binding.remove());
  auto snapshot = take(f.scene->keyBindings());
  check(called && admitted && snapshot.bindings.size() == 1 &&
          snapshot.bindings.front().label.bytes() == "After retirement",
        "old capture destructor reentry sees the committed chain after the staging guard is released");
}
} // namespace
int main() try {
  lifetimeAndScope();
  stagingReentry(0);
  stagingReentry(1);
  stagingReentry(2);
  selfRemoval();
  transientOwnerAndConservativeReset();
  postCommitDestruction();
  std::cout << "key-binding-lifetime: scope/affinity/staging/self-removal PASS\n";
} catch (const std::exception &error) {
  allocationTest::hook = nullptr;
  std::cerr << error.what() << '\n';
  return 1;
}
