#include <portals/ui.hpp>
#include <string>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene([](ManagedScene &owner) {
      return owner.state(std::string("Portals")).transform([](auto model) {
        return ui::vbox(
          ui::lineEdit(model),
          ui::label(model),
          ui::button(model, [] { ui::quit(); }));
      });
    });

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
