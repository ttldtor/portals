// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"
#include <portals/key_binding.hpp>

namespace {
bool key(Fixture &f, char32_t scalar, InputModifiers modifiers = {}, KeyAction action = KeyAction::PRESS) {
  return take(f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, scalar, modifiers, action}));
}

void canonical() {
  Fixture f;
  int first{}, newest{}, fallback{};
  take(f.scene->onInput([&](const InputEvent &) {
    ++fallback;
    return std::expected<bool, ManagedError>(false);
  }));
  auto old = take(f.scene->onKey(ui::press("Ctrl+S"), KeyInfo(TextValue("Old Save")), [&] {
    ++first;
  }));
  auto active = take(f.scene->onKey(ui::pressRepeat("Ctrl+S"), KeyInfo(TextValue("Save")), [&] {
    ++newest;
  }));
  const auto identity = active.id();
  auto before = take(f.scene->keyBindings());
  check(before.bindings.size() == 2 && !before.bindings.back().conflicts.empty(), "one canonical conflict source");
  check(key(f, U's', {.control = true}) && newest == 1 && first == 0, "newest exact matching command");
  take(active.setEnabled(false));
  check(key(f, U's', {.control = true}) && first == 1, "disabled annotation falls through to older command");
  auto disabled = take(f.scene->keyBindings());
  check(disabled.bindings.back().eligibility == KeyEligibility::DISABLED, "full help retains disabled state");
  take(active.setVisible(false));
  check(take(f.scene->keyBindings()).bindings.size() == 1, "hidden annotation excluded from owning projection");
  take(active.setVisible(true));
  take(active.setEnabled(true));
  take(active.rebind(ui::release("Ctrl+Shift+S")));
  check(active.id() == identity, "rebind preserves generated identity");
  check(!key(f, U's', {.shift = true, .control = true}), "release-only does not admit PRESS");
  check(key(f, U's', {.shift = true, .control = true}, KeyAction::RELEASE), "explicit RELEASE routing");
  take(active.unbind());
  check(take(f.scene->keyBindings()).bindings.back().eligibility == KeyEligibility::UNBOUND,
        "unbind preserves explanatory row");
  take(active.rebind(ui::press("Ctrl+S")));
  take(f.scene->setKeyCapabilities({KeySupport::UNSUPPORTED, KeySupport::UNKNOWN, KeySupport::UNKNOWN}));
  check(!key(f, U's', {.control = true}), "explicit unsupported phase excludes annotated commands");
  take(f.scene->setKeyCapabilities({}));
  const auto revision = take(f.scene->keyBindings()).revision;
  take(active.setEnabled(true));
  take(active.setVisible(true));
  take(f.scene->setKeyCapabilities({}));
  check(take(f.scene->keyBindings()).revision == revision, "effective no-ops retain revision");
  take(active.remove());
  check(treeCause(active.remove().error(), TreeError::STALE_NODE), "identity is never reused after removal");
  take(f.scene->onKey(KeyEvent{InputKey::CHARACTER, U'x', {}, KeyAction::PRESS}, [&] {
    ++newest;
  }));
  check(key(f, U'x', {}, KeyAction::REPEAT), "legacy repeat remains admitted");
  check(!key(f, U'x', {}, KeyAction::RELEASE), "legacy release remains excluded");
  auto legacy = take(f.scene->keyBindings());
  check(!legacy.bindings.back().id && legacy.bindings.back().label.bytes().empty(),
        "legacy rows have no invented metadata ID");
  take(f.scene->onInput([](const InputEvent &) {
    return std::expected<bool, ManagedError>(false);
  }));
  check(treeCause(old.setEnabled(false).error(), TreeError::STALE_NODE), "direct onInput withdraws all canonical IDs");
  take(f.scene->close());
  check(before.bindings.size() == 2 && before.bindings.back().label.bytes() == "Save",
        "owning historical DTO survives close");
  check(treeCause(active.setEnabled(false).error(), TreeError::CLOSED), "closed handle never borrows dead UI");
  check(fallback > 0, "existing fallback retained once per unhandled event");
}

void prefixes() {
  Fixture f;
  int oldTerminal{}, comment{}, uncomment{};
  take(f.scene->onKey(ui::press("Ctrl+K"), KeyInfo(TextValue("Old terminal")), [&] {
    ++oldTerminal;
  }));
  auto sequence = take(f.scene->onKey(ui::press("Ctrl+K, Ctrl+C"), KeyInfo(TextValue("Comment")), [&] {
    ++comment;
  }));
  take(f.scene->onKey(ui::press("Ctrl+K, Ctrl+U"), KeyInfo(TextValue("Uncomment")), [&] {
    ++uncomment;
  }));
  check(key(f, U'k', {.control = true}), "newest prefix starts existing timer");
  const auto timers = f.app->scheduler().metrics().timer_callbacks;
  take(sequence.setText(TextValue("Comment translated"), TextValue("Owning text")));
  check(f.app->scheduler().metrics().timer_callbacks == timers,
        "text-only publication never fires prefix timer inline");
  check(key(f, U'c', {.control = true}) && comment == 1 && uncomment == 0 && oldTerminal == 0,
        "older matching shared-prefix candidate remains reachable after label update");
  check(key(f, U'k', {.control = true}), "prefix starts again");
  take(sequence.setText(TextValue("Comment again"), TextValue("Owning text")));
  (void)f.terminal.read(TerminalDeadline(std::chrono::milliseconds(1001)), {});
  f.dispatch();
  check(f.app->scheduler().metrics().timer_callbacks == timers + 1 && oldTerminal == 0,
        "text-only publication preserves the actual scheduled timeout, with no terminal replay");
  check(key(f, U'k', {.control = true}), "prefix starts after timeout");
  take(sequence.setEnabled(false));
  check(!key(f, U'c', {.control = true}) && comment == 1, "admission change resets old candidates");
}

void normalizationAndLimits() {
  ManagedOptions options;
  options.maxKeyLabelBytes = 16;
  options.maxKeyDescriptionBytes = 32;
  Fixture f(options);
  int resolves{};
  KeyInfo info(i18n("label"));
  info.description = i18n("detail");
  info.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++resolves;
    return key == "label" ? std::string("Resolved") : std::string("Owning\nexplanation");
  };
  auto binding = take(f.scene->onKey(ui::press("A"), std::move(info), [] {
  }));
  auto before = take(f.scene->keyBindings());
  check(resolves == 2 && before.bindings.front().label.bytes() == "Resolved" &&
          before.bindings.front().description.bytes() == "Owning\nexplanation",
        "explicit resolver normalizes owning text once");
  (void)take(f.scene->keyBindings());
  check(resolves == 2, "query never retains or reinvokes a resource resolver");
  auto invalid = binding.setText(TextValue("line\nbreak"), TextValue(""));
  check(!invalid && treeCause(invalid.error(), TreeError::INVALID_ARGUMENT), "label is one required nonempty line");
  auto limited = binding.setText(TextValue("label beyond sixteen bytes"), TextValue(""));
  check(!limited && treeCause(limited.error(), TreeError::NODE_LIMIT), "input text budget fails before publication");
  auto resource = binding.setText(i18n("missing"), TextValue(""),
                                  [](std::string_view) -> std::expected<std::string, TextResourceError> {
                                    return std::unexpected(TextResourceError::MISSING_RESOURCE);
                                  });
  check(!resource && resource.error().resource == TextResourceError::MISSING_RESOURCE,
        "resource failure preserves its exact typed payload");
  auto parser = f.scene->onKey(ui::press("Ctrl+UnknownKey"), KeyInfo(TextValue("Invalid")), [] {
  });
  check(!parser && parser.error().shortcut && parser.error().shortcut->cause == ShortcutErrorKind::UNKNOWN_KEY,
        "annotated helper retains the original typed parser error");
  check(take(f.scene->keyBindings()).revision == before.revision,
        "every rejected text/helper attempt preserves canonical revision");
}
} // namespace
int main() try {
  canonical();
  prefixes();
  normalizationAndLimits();
  std::cout << "key-binding: canonical/legacy/phase/metadata/prefix PASS\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
