#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> draft;

    auto scene = ui::scene(ui::vbox(
      ui::label("Quick note"),
      ui::lineEdit(draft, "Remember the milk"),
      ui::label("Tab: focus | F10: quit")));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
