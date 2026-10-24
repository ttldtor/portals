// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

using namespace portals;

/** @param candidate Editor candidate borrowed only for this synchronous heuristic.
 * @return A basic address-shape check, not a complete email-address validator.
 */
FieldValidation emailRule(std::string_view candidate) {
  const auto at = candidate.find('@');

  if (at == std::string_view::npos || at == 0 || at + 1 == candidate.size()) {
    return FieldValidation::invalid("Enter an address such as alex@example.org");
  }

  return FieldValidation::valid("Address looks complete");
}

/** @brief Account settings: ordinary defaults first, optional editor/style customization alongside them. */
int main() {
  ui::Ref<FormField> name;
  ui::Ref<FormField> email;
  ui::Ref<Label> status;

  auto form = ui::scene(ui::vbox(
    ui::label("Account settings | Tab moves focus | F10 closes"),
    ui::formField(name, "Display name", "Alex", {.help = "Shown next to your posts", .required = true}),
    ui::formField(email, "Email", "alex@example.org",
                  {.help = "Checked when you press Enter or leave the field; drafts remain editable",
                   .required = true,
                   .validate = emailRule,
                   .editor = {.options = {.editMode = EditMode::ON_COMPLETION}},
                   .appearance = {.messageStyle = {.normal = {.bold = true}}}}),
    ui::formField("Updates", ui::checkbox("Send release announcements", true),
                  {.help = "A custom editor uses the same label/help composition"}),
    ui::hbox(
      ui::button("Validate").onClick([=] {
        if (!name.valid()) {
          (void)name.focus();

          return;
        }

        if (!email.valid()) {
          (void)email.focus();

          return;
        }

        status.text() = "Form valid";
      }),
      ui::button("Reset draft").onClick([=] {
        name.text() = "Alex";
        email.text() = "alex@example.org";
        status.text() = "Draft restored; Enter confirms the email editor";
      })),
    ui::label(status, "Edit a field to see its normal publication policy")));

  return std::move(form)
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
