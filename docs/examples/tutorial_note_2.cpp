#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> draft;
    ui::Ref<Label> saved;
    ui::Ref<Button> save;

    auto scene = ui::scene(ui::vbox(
      ui::label("Quick note"),
      ui::lineEdit(draft, "Remember the milk"),
      ui::label(saved, "Nothing saved yet"),
      ui::button(save, "Save"),
      ui::label("Tab: focus | F10: quit")));

    auto saveNote = [draft, saved] {
      saved.text() = draft.text();
    };

    return std::move(scene)
      | ui::onClick(save, saveNote)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
