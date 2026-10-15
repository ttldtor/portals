#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<Label> status;
  ui::Ref<Button> apply;

  auto scene = ui::scene(ui::vbox(
    ui::label(status, "Ready").id("status"),
    ui::button(apply, "Apply")));

  return scene
    | ui::onClick(apply,
                  [status](ui::ClickParams &params) {
                    if (params.mods.has(key::SHIFT)) {
                      status.text() = "Apply with Shift";
                      params.stop();
                    }
                  })
    | ui::onClick(apply,
                  [status] {
                    status.text() = "Applied";
                  })
    | ui::onKey("F2",
                [status] {
                  status.text() = "F2";
                })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
