// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "gallery.hpp"
#include <iostream>
#include <portals/diff.hpp>
#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

int main(int argc, char **argv) {
  const std::string_view stage = argc == 2 ? argv[1] : "initial";

  if (stage != "initial" && stage != "keyboard" && stage != "mouse" && stage != "disabled" && stage != "--check") {
    std::cerr << "Usage: portals_gallery [initial|keyboard|mouse|disabled|--check]\n";

    return 2;
  }

  try {
    gallery::Scene scene;
    auto initial = scene.render();

    scene.stage(stage == "--check" ? "disabled" : stage);

    if (stage == "--check") {
      auto final = scene.render();

      return scene.calls == 2 && scene.changes == 1 && scene.disabled_calls == 0 ? 0 : 1;
    }

#ifdef _WIN32
    portals::NativeTerminalHandles handles{reinterpret_cast<std::intptr_t>(GetStdHandle(STD_INPUT_HANDLE)),
                                           reinterpret_cast<std::intptr_t>(GetStdHandle(STD_OUTPUT_HANDLE))};
#else
    portals::NativeTerminalHandles handles{0, 1};
#endif
    auto terminal = gallery::take(portals::TerminalSession::open(handles));
    auto buffer = scene.render(gallery::take(terminal.size()));
    portals::TerminalFrameOutput output(terminal);
    portals::FrameWriter writer(output);

    gallery::take(writer.submit(std::move(buffer)));

    const auto written = writer.flush(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    const bool complete = written.status == portals::TerminalStatus::PROGRESS && !written.error && !writer.pending();

    gallery::take(terminal.close());

    return complete ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
