#include <portals/ui.hpp>

#include <optional>

int main() {
  using namespace portals;

  std::optional<ui::SceneReport> finalReport;
  ui::Ref<Button> exit;

  const int result = ui::scene(ui::button(exit, "Exit"))
    | ui::reportOnError([&finalReport](const ui::SceneReport &report) {
        finalReport = report;
      })
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();

  if (finalReport) {
    ui::reportToStderr(*finalReport);
  }

  return result;
}
