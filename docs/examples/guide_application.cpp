#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<Label> status;
  ui::Ref<Button> exit;

  return ui::scene(ui::vbox(
    ui::label(status, "Ready"),
    ui::button("Apply", [status] {
      status.text() = "Applied";
    }),
    ui::button(exit, "Exit")))
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
