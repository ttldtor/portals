// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdio>

namespace {
using namespace portals;

class Retirement final : public Widget {
public:
  explicit Retirement(int &retired) : retired_(retired) {
  }

  ~Retirement() override {
    ++retired_;
  }

private:
  int &retired_;
};

bool run(bool pipe) {
  example::ReplayTerminal terminal({}, 100, {40, 8});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  ui::Ref<Label> caption;
  int tail{}, later{}, retired{}, notifications{}, reports{};
  bool typed{}, source{};

  auto scene = ui::scene(ui::column(ui::label(caption, "ready"), ui::custom<Retirement>(std::ref(retired))), options);

  auto observe = [&](const ManagedDiagnostic &) {
    ++notifications;
  };

  auto report = [&](const ui::SceneReport &value) {
    ++reports;
    typed = value.failure && value.failure->phase == ManagedPhase::TEXT &&
            value.failure->text == TextError{TextErrorKind::INVALID_UTF8, 0} && value.failure->setupStopped &&
            value.failure->operation && std::string_view(value.failure->operation) == "text.set";
    source = value.failure && value.failure->operationOrigin && value.failure->operationOrigin->line() != 0;
  };

  auto configure = [&](ManagedScene &) {
    caption.text() = std::string_view("\xff", 1);

    ++tail;
  };

  auto dependent = [&](ManagedScene &) {
    ++later;
  };
  int code{};

  if (pipe) {
    auto configured = std::move(scene) | ui::onError(observe) | ui::configure(configure) | ui::configure(dependent) |
                      ui::reportOnError(report);
    code = configured.runApp(terminal);
  } else {
    scene.onError(observe).configure(configure).configure(dependent).reportOnError(report);
    code = scene.runApp(terminal);
  }

  std::printf("%s tail=%d later=%d retired=%d notifications=%d reports=%d typed=%d source=%d terminal=%d\n",
              pipe ? "pipe" : "ordinary", tail, later, retired, notifications, reports, typed, source, terminal.closed);

  return code == 1 && tail == 1 && later == 0 && retired == 1 && notifications == 1 && reports == 1 && typed &&
         source && !terminal.closed;
}
} // namespace

int main() {
  return run(false) && run(true) ? 0 : 1;
}
