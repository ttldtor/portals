#include <portals/ui.hpp>

#include <chrono>

int main() {
  using namespace portals;
  using namespace std::chrono_literals;

  // clang-format off
  return [] {
    ui::Ref<Label> status;
    ui::Ref<Button> start;

    return ui::scene(ui::vbox(
      ui::label(status, "Ready"),
      ui::button(start, "Start"),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::onClick(start, ui::async([status](ui::AsyncContext context) -> UiTask {
          status.text() = "Waiting";

          auto waited = co_await context.after(1s);

          status.text() = waited ? "Completed" : "Wait failed";

          co_return waited;
        }))
      | ui::onKey("F10", ui::quit);
  }
  | ui::runApp();
  // clang-format on
}
