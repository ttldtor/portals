#include <portals/ui_style.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(
      ui::vbox(ui::lineEdit("").id("message"), ui::button("Add Hello").id("add"), ui::button("Exit").id("exit")));
    auto message = scene.get("message");
    ui::Ref<Button> add = scene.get("add");
    auto exit = scene.get<Button>("exit");

    return std::move(scene)
      | ui::style(add, "* { bold: true; }")
      | ui::onClick(add, [message] { message.text() += "Hello!"; })
      | ui::onClick(exit, ui::quit)
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
