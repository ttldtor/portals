// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
void registration() {
  bool completed{};
  for (std::ptrdiff_t point{}; point < 1024; ++point) {
    Fixture f;
    int calls{};
    auto original = take(f.scene->onKey(ui::press("A"), KeyInfo(TextValue("Original")), [&] {
      ++calls;
    }));
    const auto revision = take(f.scene->keyBindings()).revision;
    auto selection = ui::press("B");
    KeyInfo info(TextValue("Candidate"));
    allocationTest::remaining = point;
    std::optional<std::expected<KeyBinding, ManagedError>> result;
    try {
      result = f.scene->onKey(selection, std::move(info), [] {
      });
    } catch (const std::bad_alloc &) {
    }
    allocationTest::remaining = -1;
    if (result && *result) {
      completed = true;
      break;
    }
    if (result) {
      check(treeCause(result->error(), TreeError::NO_MEMORY), "new annotated publication exact NO_MEMORY");
    }
    auto snapshot = take(f.scene->keyBindings());
    check(snapshot.revision == revision && snapshot.bindings.size() == 1 &&
            snapshot.bindings.front().id == original.id(),
          "every failed staging allocation preserves canonical chain and metadata revision");
    take(f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, U'a', {}, KeyAction::PRESS}));
    check(calls == 1, "old command remains usable after failed registration");
  }
  check(completed, "finite registration allocation sweep reaches success");
}

void mutationAndProjection() {
  for (int operation{}; operation < 3; ++operation) {
    bool completed{};
    for (std::ptrdiff_t point{}; point < 1024; ++point) {
      Fixture f;
      auto binding = take(f.scene->onKey(ui::press("A"), KeyInfo(TextValue("Original")), [] {
      }));
      auto before = take(f.scene->keyBindings());
      auto expectedRevision = before.revision;
      if (operation == 2) {
        take(f.scene->setKeyCapabilities({KeySupport::SUPPORTED, KeySupport::UNKNOWN, KeySupport::UNKNOWN}));
        ++expectedRevision;
      }
      auto selection = ui::press("B");
      TextValue caption("Translated"), description("Owning explanation");
      allocationTest::remaining = point;
      std::optional<std::expected<void, ManagedError>> result;
      std::optional<std::expected<KeyBindingsSnapshot, ManagedError>> query;
      try {
        if (operation == 0) {
          result = binding.rebind(selection);
        }
        if (operation == 1) {
          result = binding.setText(caption, description);
        }
        if (operation == 2) {
          query = f.scene->keyBindings();
        }
      } catch (const std::bad_alloc &) {
      }
      allocationTest::remaining = -1;
      if ((result && *result) || (query && *query)) {
        completed = true;
        break;
      }
      if (result) {
        check(treeCause(result->error(), TreeError::NO_MEMORY), "mutation exact NO_MEMORY");
      }
      if (query) {
        check(treeCause(query->error(), TreeError::NO_MEMORY), "fresh query returns error instead of stale DTO");
      }
      auto after = take(f.scene->keyBindings());
      check(after.revision == expectedRevision && after.bindings.front().label.bytes() == "Original",
            "failed mutation/query preserves old canonical registration");
    }
    check(completed, "finite new-allocation sweep reaches success");
  }
}
} // namespace
int main() try {
  registration();
  mutationAndProjection();
  std::cout << "key-binding-oom: new registration/mutation/query publication PASS\n";
} catch (const std::exception &error) {
  allocationTest::remaining = -1;
  std::cerr << error.what() << '\n';
  return 1;
}
