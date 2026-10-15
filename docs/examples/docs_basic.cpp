#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;
  ui::Ref<Button> exit;

  auto scene = ui::scene(ui::vbox(
    ui::lineEdit(message, ""),
    ui::button(add, "Add Hello"),
    ui::button(exit, "Exit")));

  return scene
    | ui::style("Button { bold: true; }")
    | ui::onClick(add,
                  [message] {
                    message.text() += "Hello!";
                  })
    | ui::onClick(exit, ui::quit)
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
