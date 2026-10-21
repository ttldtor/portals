#include <portals/ui.hpp>

#include <optional>

int main() {
  using namespace portals;

  std::optional<ui::SceneReport> finalReport;

  // clang-format off
  const int result = [&] {
    ui::Ref<Button> exit;

    return ui::scene(ui::button(exit, "Exit"))
      | ui::reportOnError([&finalReport](const ui::SceneReport &report) {
          finalReport = report;
        })
      | ui::onClick(exit, ui::quit)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on

  if (finalReport) {
    ui::reportToStderr(*finalReport);
  }

  return result;
}
