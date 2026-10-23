// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/spinner.hpp>
#include <portals/ui_scene_resource.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace portals;

void require(bool value) {
  if (!value) {
    throw std::runtime_error("Installed Spinner contract failed");
  }
}

void nativeStartup() {
  example::ReplayTerminal terminal({{150, "\r", {}}}, 350, {24, 3});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Spinner> loading;
  // clang-format off
  auto factory = [options, loading] {
    return ui::scene(ui::vbox(
      ui::spinner(loading).label("Loading"),
      ui::button("Stop", [loading] {
        loading.stop();
        loading.label("Stopped");
      })), options)
      | ui::onKey("F10", ui::quit);
  };
  const auto status = std::move(factory)
    | ui::runApp(terminal);
  // clang-format on
  require(status == 0 && terminal.closed && terminal.output.find("Stopped") != std::string::npos);
}

void resourceAndCleanup() {
  example::ReplayTerminal terminal({}, 350, {24, 3});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::SceneResourceOptions resource;
  resource.format = ui::SceneResourceFormat::CONFIGURATION;
  auto scene = ui::scene(R"(scene { Spinner#loading {
    preset=LINE frames="a\nbb" interval=100 label="Loading" alignment=CENTER
  } })",
                         resource, options);
  auto spinner = scene.get<Spinner>("loading");
  std::optional<SchedulerMetrics> metrics;
  scene.configure([&](ManagedScene &, Application &app) {
    require(static_cast<bool>(app.tree().read(spinner.node(), [&](const Spinner &value) {
      require(value.options().frames == "a\nbb");
    })));
    metrics = app.scheduler().metrics();
  });
  const auto result = scene.run(terminal);
  require(result && result->frames >= 3 && terminal.closed && metrics.has_value());
  require(!spinner.geometry());
}
} // namespace

/** @brief Exercise public Spinner headers, animation and startup against the installed archive. */
int main() try {
  nativeStartup();
  resourceAndCleanup();
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
