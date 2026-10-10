// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);

  if (!scene) {
    return 2;
  }

  ui::Ref<Label> message;
  ui::Ref<Label> amount;
  ui::Ref<Label> currency;
  ui::Ref<LineEdit> editor;
  auto model = (*scene)->state(std::string("initial"));

  if (!model) {
    return 3;
  }

  const auto mounted = ui::mount(
    **scene, ui::column(ui::label("old").reference(message), ui::label("123 ").reference(amount),
                        ui::label("USD").reference(currency),
                        ui::lineEdit(*model, {.options = {.editMode = EditMode::EXPLICIT}}).reference(editor)));

  if (!mounted) {
    return 4;
  }

  auto assignment = message.text() = amount.text() + currency.text();
  auto suffix = message.text() += text(" ({:.2f})", 123.0);
  auto localized = message.text([](std::string_view key) -> std::expected<std::string, TextResourceError> {
    return key == "currency" ? std::expected<std::string, TextResourceError>{" USD"}
                             : std::unexpected(TextResourceError::MISSING_RESOURCE);
  }) += i18n("currency");
  const auto caption = message.text().get();

  if (!assignment.result() || !suffix.result() || !localized.result() || !caption ||
      *caption != "123 USD (123.00) USD") {
    return 5;
  }

  auto draft = editor.text() = message.text();
  auto snapshot = model->snapshot();

  if (!draft.result() || !snapshot || (*snapshot)->value != "initial" || !(*app)->router().setFocus(editor.node()) ||
      !(*app)->router().dispatch(KeyEvent{InputKey::ENTER})) {
    return 6;
  }

  snapshot = model->snapshot();

  if (!snapshot || (*snapshot)->value != *caption) {
    return 7;
  }

  auto missing = message.text() += i18n("missing");
  const auto diagnostics = (*scene)->diagnostics();
  const auto unchanged = message.text().get();

  if (missing.result() || !diagnostics || diagnostics->errors.size() != 1 ||
      diagnostics->errors[0].error.resource != TextResourceError::RESOLVER_REQUIRED || !unchanged ||
      *unchanged != *caption) {
    return 8;
  }

  return 0;
}
