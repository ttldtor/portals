#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<Label> message;
  ui::Ref<Button> greet;

  auto scene = ui::scene(ui::vbox(
    ui::label(message, "Ready"),
    ui::button(greet, "Greet")));

  return scene
    | ui::onClick(greet,
                  [message] {
                    message.text() = "Hello!";
                  })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
