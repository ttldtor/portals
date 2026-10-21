#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(
      ui::grid(ui::label("Name"), ui::label("Ada"))
        .columns(2)
        .columnGap(2));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
