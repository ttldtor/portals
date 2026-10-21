#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> name;
    ui::Ref<Label> greeting;

    auto scene = ui::scene(ui::vbox(
      ui::lineEdit(name, "Ada", {.options = {.editMode = EditMode::EXPLICIT}}),
      ui::label(greeting, "Press F2 to read the draft")));

    return std::move(scene)
      | ui::onKey("F2",
                  [name, greeting] {
                    greeting.text() = name.text() + "!";
                  })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
