#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> name;
  ui::Ref<Label> greeting;

  auto scene = ui::scene(ui::vbox(
    ui::lineEdit(name, "Ada", {.options = {.editMode = EditMode::EXPLICIT}}),
    ui::label(greeting, "Press F2 to read the draft")));

  return scene
    | ui::onKey("F2",
                [name, greeting] {
                  greeting.text() = name.text() + "!";
                })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
