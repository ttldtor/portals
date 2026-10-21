#include <portals/ui.hpp>
#include <utility>

#include <string>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
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

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
