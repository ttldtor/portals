#include <portals/ui.hpp>

#include <string>

int main() {
  using namespace portals;

  // clang-format off
  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.state(std::string("Portals")).transform([](auto model) {
      ui::Ref<Label> summary;

      return ui::vbox(
        ui::lineEdit(model),
        ui::label(summary, "").observe(model, [summary](const std::string &value) {
          summary.text() = text("Accepted: {}", value);
        }),
        ui::button("Quit", [] { ui::quit(); }));
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
  // clang-format on
}
