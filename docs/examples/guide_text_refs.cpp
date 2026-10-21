#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<Label> message;
    ui::Ref<Button> greet;

    auto scene = ui::scene(ui::vbox(
      ui::label(message, "Ready"),
      ui::button(greet, "Greet")));

    return std::move(scene)
      | ui::onClick(greet,
                    [message] {
                      message.text() = "Hello!";
                    })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
