#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(
      ui::grid(
        ui::header(ui::label("Name"), ui::label("Role")).sticky(),
        ui::body(
          ui::label("Ada"), ui::label("Programmer"),
          ui::label("Grace"), ui::label("Admiral"),
          ui::label("Margaret"), ui::label("Engineer")))
        .columns(2)
        .columnGap(2)
        .scroll()
        .height(3));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
