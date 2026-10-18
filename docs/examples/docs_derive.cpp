// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <format>
#include <portals/ui.hpp>
#include <string>

int main() {
  using namespace portals;

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

  // clang-format off
  return window
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
  // clang-format on
}
