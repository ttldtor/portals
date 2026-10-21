// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <format>
#include <portals/ui.hpp>
#include <string>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    const std::string initialName{"Ada"};
    const auto greetingFor = [](const std::string &value) {
      return std::format("Hello, {}", value);
    };

    auto window = ui::scene([initialName, greetingFor](ManagedScene &owner) {
      return owner.state(initialName).and_then([&owner, greetingFor](auto name) {
        return owner.derive(name, greetingFor).transform([name](auto greeting) {
          return ui::vbox(ui::lineEdit(name), ui::label(greeting), ui::button(greeting, [] {
                            ui::quit();
                          }));
        });
      });
    });
    return std::move(window)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
