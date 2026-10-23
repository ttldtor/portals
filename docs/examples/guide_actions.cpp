#include <portals/ui.hpp>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> message;
    ui::ActionSet actions;
    auto add = actions.action("add", "Add Hello", [message] {
      message.text() += "Hello!";
    });
    auto exit = actions.action("exit", "Exit", ui::quit);

    return ui::scene(ui::vbox(
      ui::lineEdit(message, ""),
      ui::button(add),
      ui::button(exit)))
      | ui::onKey("Ctrl+H", add)
      | ui::onKey("F10 | Ctrl+C", exit);
  } | ui::runApp();
  // clang-format on
}
