#include <portals/async.hpp>
#include <portals/ui.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace {
struct AsyncAction {
  std::unique_ptr<portals::UiAsync> owner;
  std::optional<portals::AsyncError> error;
};

portals::UiTask finishLater(portals::UiAsyncContext context, portals::SchedulerTime deadline,
                            portals::ui::Ref<portals::Label> status, std::shared_ptr<AsyncAction> action) {
  status.text() = "Waiting";

  auto waited = co_await context.until(deadline);

  if (!waited) {
    action->error = waited.error();
    status.text() = "Wait failed";

    co_return std::unexpected(waited.error());
  }

  status.text() = "Completed";

  co_return std::expected<void, portals::AsyncError>{};
}
} // namespace

int main() {
  using namespace portals;

  std::shared_ptr<AsyncAction> action;

  // clang-format off
  const int result = [&] {
    constexpr SchedulerTime delayMilliseconds = 1000;
    action = std::make_shared<AsyncAction>();
    ui::Ref<Label> status;
    ui::Ref<Button> start;
    ui::Ref<Button> exit;

    auto scene = ui::scene(ui::vbox(
      ui::label(status, "Ready"),
      ui::button(start, "Start"),
      ui::button(exit, "Exit")));

    scene.onStop([action] {
      if (action->owner) {
        auto closed = action->owner->close();

        if (!closed) {
          action->error = closed.error();
        }

        action->owner.reset();
      }
    });

    return std::move(scene)
      | ui::configure([action, status](ManagedScene &, Application &app) {
          auto created = UiAsync::create(app.scheduler(), app.tree(), app.root());

          if (!created) {
            action->error = created.error();
            status.text() = "Async setup failed";

            return;
          }

          action->owner = std::move(*created);
        })
      | ui::configure([action, status, start](ManagedScene &, Application &app) {
          auto bound = start.onClick([action, status, &app] {
            if (!action->owner) {
              return;
            }

            auto started = action->owner->start(
              finishLater(action->owner->context(), app.now() + delayMilliseconds, status, action));

            if (!started && started.error() != AsyncError::WOULD_BLOCK) {
              action->error = started.error();
              status.text() = "Start failed";
            }
          });

          return bound;
        })
      | ui::onClick(exit, ui::quit)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on

  return result != 0 || (action && action->error) ? 1 : 0;
}
