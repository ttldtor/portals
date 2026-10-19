// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

namespace {
using namespace portals;
using VoidAction = decltype([] {
});
using TreeAction = decltype([]() -> std::expected<void, TreeError> {
  return {};
});
using ValueAction = decltype([]() -> std::expected<int, TreeError> {
  return 7;
});
using RichAction = decltype([]() -> std::expected<void, ManagedError> {
  return {};
});
using WrongValue = decltype([]() -> std::expected<int, ManagedError> {
  return 7;
});
using WrongError = decltype([]() -> std::expected<void, TextResourceError> {
  return {};
});

template <class Callback>
concept OldForms = requires(ManagedScene &managed, ui::Scene &facade, Callback action, const KeySelection &selection,
                            Node<Button> button) {
  { managed.onKey(KeyEvent{}, action) } -> std::same_as<std::expected<void, ManagedError>>;
  { managed.onKey(selection, action) } -> std::same_as<std::expected<void, ManagedError>>;
  { managed.onClick(button, action) } -> std::same_as<std::expected<void, ManagedError>>;
  { facade.onKey("Ctrl+S", action) } -> std::same_as<ui::Scene &>;
  { facade.onKey(selection, action) } -> std::same_as<ui::Scene &>;
  ui::onKey("Ctrl+S", action);
  ui::onKey(selection, action);
};

template <class Callback>
concept AnnotatedForms =
  requires(ManagedScene &managed, ui::Scene &facade, Callback action, const KeySelection &selection) {
    {
      managed.onKey(selection, KeyInfo(TextValue("Save")), action)
    } -> std::same_as<std::expected<KeyBinding, ManagedError>>;
    { facade.onKey(selection, ui::keyInfo("Save"), action) } -> std::same_as<ui::Scene &>;
    ui::onKey(selection, ui::keyInfo("Save"), action);
  };

static_assert(OldForms<VoidAction> && OldForms<TreeAction> && OldForms<ValueAction>);
static_assert(OldForms<RichAction> && AnnotatedForms<RichAction>);
static_assert(AnnotatedForms<VoidAction> && AnnotatedForms<TreeAction> && AnnotatedForms<ValueAction>);
static_assert(!AnnotatedForms<WrongValue> && !AnnotatedForms<WrongError>);
} // namespace
