#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> field;

    auto scene = ui::scene(ui::vbox(
      ui::lineEdit(field, "Portals", {.options = {.editMode = EditMode::EXPLICIT}}),
      ui::label(field.text()),
      ui::label(field.committedText()),
      ui::button(field.committedText(), [] { ui::quit(); })));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
