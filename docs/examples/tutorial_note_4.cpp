#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> draft;
    ui::Ref<Label> saved;
    ui::Ref<Button> save;
    ui::Ref<Button> clear;

    auto scene = ui::scene(ui::vbox(
      ui::label("Quick note"),
      ui::lineEdit(draft, "Remember the milk"),
      ui::label(saved, "Nothing saved yet"),
      ui::button(save, "Save"),
      ui::button(clear, "Clear draft"),
      ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));

    auto saveNote = [draft, saved] {
      saved.text() = draft.text();
    };

    auto clearDraft = [draft] {
      draft.text() = "";
    };

    return std::move(scene)
      | ui::theme(nordTheme())
      | ui::style("Button { min-width: 16; bold: true; }")
      | ui::onClick(save, saveNote)
      | ui::onClick(clear, clearDraft)
      | ui::onKey("F2", saveNote)
      | ui::onKey("F3", clearDraft)
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
