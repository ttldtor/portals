#include <portals/ui.hpp>
#include <string>

int main() {
  using namespace portals;

  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.state(std::string("Portals")).transform([](auto model) {
      return ui::vbox(ui::lineEdit(model), ui::lineEdit(model));
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
