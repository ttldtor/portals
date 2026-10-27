#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<Label> status;
  ui::ActionSet actions;
  auto save = actions.action("save", "Save", [status] {
    status.text() = "Saved";
  });
  auto exit = actions.action("exit", "Quit", ui::quit);

  // clang-format off
  return ui::scene(ui::vbox(
    ui::menu({save, exit}).fitWidth().minWidth(12).maxWidth(32).framed("rounded", "File"),
    ui::button(save).fitWidth(),
    ui::label(status, "Ready")))
    | ui::onKey("Ctrl+S", save)
    | ui::onKey(key::F10, exit)
    | ui::runApp();
  // clang-format on
}
