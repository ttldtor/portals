// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>
#include <stop_token>
#include <string_view>

/** @brief Check a requested account name against the deployment's immutable reserved-name catalog.
 * @param candidate Owning request's UTF-8 bytes borrowed only during this worker invocation.
 * @param stop Cooperative cancellation; no UI references or model owners are retained.
 * @return Availability, readable rejection, or a typed cancellation failure.
 */
std::expected<portals::FieldValidation, portals::TreeError> accountName(std::string_view candidate,
                                                                        std::stop_token stop) {
  static constexpr std::array RESERVED{
    std::string_view("administrator"), std::string_view("api"), std::string_view("billing"),
    std::string_view("help"), std::string_view("operator"), std::string_view("root"),
    std::string_view("security"), std::string_view("support"), std::string_view("system")};

  for (const auto name : RESERVED) {
    if (stop.stop_requested()) {
      return std::unexpected(portals::TreeError::CLOSED);
    }

    if (candidate == name) {
      return portals::FieldValidation::invalid("Reserved by the account directory; choose another name");
    }
  }

  return portals::FieldValidation::valid("Available in this account directory");
}

/** @brief Native account form; ordinary controls and key handlers remain usable during validation. */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    return ui::scene(ui::vbox(
      ui::label("Account directory | Tab moves focus | Enter checks | Esc restores | F10 closes"),
      ui::formField("Account name", "", {
        .help = "The model changes only after the background directory check",
        .required = true,
        .editor = {.options = {.editMode = EditMode::ON_COMPLETION}},
        .validateAsync = accountName}),
      ui::checkbox("Send release announcements", true),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::onKey(key::F10, ui::quit);
  }
  | ui::runApp();
  // clang-format on
}
