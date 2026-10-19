// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace portals;

void check(bool condition, const char *message, std::source_location origin = std::source_location::current()) {
  if (!condition) {
    std::cerr << message << " at " << origin.line() << '\n';

    throw std::runtime_error(std::string(message) + " at " + std::to_string(origin.line()));
  }
}

template <class T, class E> T checked(std::expected<T, E> result) {
  check(bool(result), "unexpected checked failure");

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == expected;
}

// Test-only adapter: native semantic input enters the real Application loop.
// Spaced input allows normal preparation; no manual layout, scheduler or router loop.
class ScriptTerminal final : public example::ReplayTerminal {
public:
  explicit ScriptTerminal(std::size_t steps = 0) : ReplayTerminal(inputs(steps), 2000, {48, 16}) {
  }

  PointerCaptureCapabilities pointerCaptureCapabilities() const noexcept override {
    return {true, true, true};
  }

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    ++reads;

    auto result = ReplayTerminal::read(deadline, stop);

    if (!result.bytes.empty()) {
      result.bytes.clear();
      step(next_++, result);
    }

    return result;
  }

  std::size_t reads{};
  std::function<void(std::size_t, TerminalRead &)> step;

private:
  static std::vector<Input> inputs(std::size_t count) {
    std::vector<Input> result;

    for (std::size_t index = 0; index != count; ++index) {
      result.push_back({(index + 1) * 100, "x", {}});
    }

    return result;
  }

  std::size_t next_{};
};

void quietEof(bool open) {
  ScriptTerminal terminal(open ? 1 : 0);
  ui::SceneOptions options;
  ui::Ref<Dialog> dialog;

  options.clock = terminal.clock();

  const auto cold = dialog.open();

  check(!cold && cause(cold.error(), TreeError::CLOSED), "cold Ref has checked CLOSED result");

  auto scene = ui::scene(ui::overlayHost(ui::button("Main",
                                                    [dialog] {
                                                      dialog.open();
                                                    }),
                                         ui::dialog(dialog, "Retained", ui::lineEdit("retained"))),
                         options);
  terminal.step = [](std::size_t, TerminalRead &read) {
    read.events.emplace_back(KeyEvent{InputKey::ENTER});
  };

  const auto stats = checked(scene.run(terminal));

  check(stats.frames == (open ? 2u : 1u) && terminal.closed && !terminal.output.empty(),
        "unchanged closed/open layer settles before quiet EOF");
}

void routedComposition() {
  ScriptTerminal terminal(8);
  ui::SceneOptions options;
  ui::Ref<OverlayHost> host;
  ui::Ref<Dialog> modal, help;
  ui::Ref<Button> opener, inside;
  ui::Ref<Label> drag;
  ManagedState<int> revision;
  Application *application{};
  std::optional<ActionBinding> binding;
  std::optional<PointerCapture> capture;
  int notifications{}, cancellations{}, backgroundKeys{};
  std::array<std::vector<int>, 2> observed;
  bool rejectedNotification{};

  options.clock = terminal.clock();
  options.application.scheduler.data_capacity = 1;

  // [dialog-options]
  DialogOptions helpOptions{
    .mode = DialogMode::NON_MODAL, .maximumPanelSize = {32, 8}, .viewportMargin = 2, .anchorGap = 0};
  // [dialog-options]

  auto scene = ui::scene(
    [&](ManagedScene &owner) {
      revision = checked(owner.state(0));
      checked(owner.observe(revision, [&](const int &value) {
        notifications += value;
        observed[0].push_back(value);
      }));
      checked(owner.observe(revision, [&](const int &value) {
        notifications += value;
        observed[1].push_back(value);
      }));

      return ui::overlayHost(host,
                             ui::vbox(ui::label("Drag owner").reference(drag),
                                      ui::button(opener, "Open",
                                                 [&] {
                                                   const auto update = checked(revision.set(1));

                                                   rejectedNotification =
                                                     update.changed && update.accepted == 1 && update.rejected == 1 &&
                                                     update.notification_error == SchedulerError::WOULD_BLOCK;
                                                   modal.open();
                                                 })),
                             ui::dialog(modal, "Modal",
                                        ui::button(inside, "Inside",
                                                   [] {
                                                   }))
                               .onDismiss([&](DialogDismissReason reason) {
                                 check(reason == DialogDismissReason::CANCEL, "Escape has CANCEL reason");
                                 ++cancellations;
                               }),
                             ui::dialog(help, "Help", ui::label("Nonmodal content"), helpOptions)
                               .anchor(opener, DialogPlacement::BELOW_START));
    },
    options);

  scene.configure([&](ManagedScene &, Application &app) {
    application = &app;
    binding.emplace(checked(app.router().bind(
      drag.node(),
      [&](Label &, const RoutedEvent &event) {
        const auto *mouse = std::get_if<MouseInputEvent>(&event);

        if (mouse && mouse->action == MouseAction::PRESS) {
          capture.emplace(checked(application->router().capturePointer(drag.node(), host.node())));
          modal.open();
        }

        return EventReply::HANDLED;
      },
      false)));
  });
  scene.onKey("F2", [help] {
    help.open();
  });
  scene.onKey("F3", [help] {
    help.dismiss();
  });
  scene.onKey("F10", [&] {
    ++backgroundKeys;
    ui::quit();
  });

  terminal.step = [&](std::size_t index, TerminalRead &read) {
    const auto key = [&](InputKey value) {
      read.events.emplace_back(KeyEvent{value});
    };

    switch (index) {
    case 0:
      check(checked(application->router().focused()) == opener.node(), "main opener initially focused");
      key(InputKey::ENTER);
      break;
    case 1:
      check(checked(modal.opened()) && checked(application->router().focused()) == inside.node(), "modal enters focus");
      check(rejectedNotification && notifications == 2 && observed[0] == std::vector<int>{0, 1} &&
              observed[1] == std::vector<int>{0, 1},
            "both observers recover latest revision once after one accepted/one rejected publication");
      key(InputKey::F10);
      break;
    case 2: {
      const auto equal = checked(revision.set(1));

      check(!equal.changed && equal.accepted == 0 && equal.rejected == 0, "equal assignment creates no retry delivery");
      check(backgroundKeys == 0 && checked(modal.opened()), "modal blocks background F10");
      key(InputKey::ESCAPE);
      break;
    }
    case 3: {
      check(!checked(modal.opened()) && checked(application->router().focused()) == opener.node(),
            "cancel returns focus");

      const auto geometry = checked(application->tree().inspect(drag.node())).geometry.surface;
      MouseInputEvent press;

      press.column = geometry.origin().x;
      press.row = geometry.origin().y;
      press.button = MouseButton::LEFT;
      press.action = MouseAction::PRESS;
      read.events.emplace_back(press);
      break;
    }
    case 4:
      check(capture && !capture->connected() && checked(modal.opened()), "modal cancels admitted background capture");
      check(checked(application->router().pointerCapture()) == Node<Widget>{}, "no outside capture survives");
      key(InputKey::ESCAPE);
      break;
    case 5:
      check(cancellations == 2 && checked(application->router().focused()) == opener.node(),
            "second cancellation returns opener");
      key(InputKey::F2);
      break;
    case 6:
      check(checked(help.opened()) && checked(application->router().focused()) == opener.node(),
            "anchored nonmodal preserves body focus");
      key(InputKey::F3);
      break;
    case 7:
      check(!checked(help.opened()) && notifications == 2 && observed[0] == std::vector<int>{0, 1} &&
              observed[1] == std::vector<int>{0, 1},
            "dialog changes do not repeat recovered observer revision");
      key(InputKey::F10);
      break;
    default:
      check(false, "unexpected input step");
    }
  };

  check(scene.runApp(terminal) == 0 && terminal.closed && backgroundKeys == 1,
        "ordinary runner quits only after modal closes");
}

void dismissalErrors(bool shortCall) {
  ScriptTerminal terminal(2);
  ui::SceneOptions options;
  ui::Ref<Dialog> dialog;
  int notifications{}, dismissed{}, reports{};
  bool closedAtCallback{}, reportedAfterClose{};
  const auto expected = shortCall ? TreeError::WOULD_BLOCK : TreeError::CALLBACK_FAILED;

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::overlayHost(ui::button("Open",
                                                    [dialog] {
                                                      dialog.open();
                                                    }),
                                         ui::dialog(dialog, "Failure",
                                                    ui::button("Inside",
                                                               [] {
                                                               }))
                                           .onDismiss([&](DialogDismissReason) -> std::expected<void, TreeError> {
                                             ++dismissed;
                                             closedAtCallback = !checked(dialog.opened());

                                             if (shortCall) {
                                               dialog.open();

                                               return {};
                                             }

                                             return std::unexpected(TreeError::CALLBACK_FAILED);
                                           })),
                         options);

  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    check(cause(diagnostic.error, expected), "live diagnostic preserves original cause");
    ++notifications;
  });
  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    reportedAfterClose = terminal.closed && report.failure && cause(*report.failure, expected);
  });
  terminal.step = [](std::size_t index, TerminalRead &read) {
    read.events.emplace_back(KeyEvent{index == 0 ? InputKey::ENTER : InputKey::ESCAPE});
  };

  check(scene.runApp(terminal) == 1, "short/callback failure stops standard runner");
  check(closedAtCallback && dismissed == 1 && notifications == 1,
        "published dismissal remains closed and is not replayed");
  check(reports == 1 && reportedAfterClose, "owning final error is delivered after terminal cleanup");
}

void failedMountCleanup() {
  ScriptTerminal terminal;
  ui::Ref<OverlayHost> host;
  ui::Ref<Dialog> dialog;
  ui::Ref<Button> inside;
  std::weak_ptr<int> lifetime;
  int reports{};

  {
    auto ticket = std::make_shared<int>(0);

    lifetime = ticket;

    auto scene = ui::scene(ui::overlayHost(host, ui::button("Main"),
                                           ui::dialog(dialog, "Invalid anchor",
                                                      ui::button(inside, "Inside",
                                                                 [ticket] {
                                                                   ++*ticket;
                                                                 }))
                                             .anchor(inside)));

    ticket.reset();
    scene.reportOnError([&](const ui::SceneReport &report) {
      check(report.failure && cause(*report.failure, TreeError::INVALID_ARGUMENT),
            "dialog-owned anchor is a typed mount failure");
      ++reports;
    });

    check(scene.runApp(terminal) == 1, "failed complete mount uses standard short status");
    check(host.node() == Node<OverlayHost>{} && dialog.node() == Node<Dialog>{} && inside.node() == Node<Button>{},
          "failed complete mount publishes no partial refs");
    check(terminal.reads == 0 && terminal.output.empty(), "mount failure performs no terminal input or paint");
  }

  check(lifetime.expired() && reports == 1, "failed subtree releases callback ownership without replay");
}
} // namespace

int main() {
  try {
    quietEof(false);
    quietEof(true);
    routedComposition();
    dismissalErrors(false);
    dismissalErrors(true);
    failedMountCleanup();
    std::cout << "Installed dialogs: 6 scenarios, 0 failures\n";

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
