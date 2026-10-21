#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> name;

    auto scene = ui::scene(ui::vbox(
      ui::lineEdit(name, "Ada"),
      ui::button("Undo",
                 [name] {
                   return name.undo();
                 }),
      ui::button("Redo",
                 [name] {
                   return name.redo();
                 })));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
