// Copyright (c) 2025 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <portals.hpp>
#include <thread>

using namespace std::literals;

int main() {
  std::cout << org::ttldtor::VirtualTerminal::enable() << std::endl;
  std::cout << org::ttldtor::VirtualTerminal::enable() << std::endl;
  auto esc = "\x1b";
  auto clearFormat = "\x1b[0m";
  auto eraseFromCursorToBegin = "\x1b[1J";
  auto toBegin = "\x1b[0G";
  auto makeCurInvisible = "\x1b[?25l";
  auto makeCurVisible = "\x1b[?25h";

  for (int i = 0; i < 256; i++) {
    std::cout << std::format("\x1b[38;5;{0}m{0}{1} ", i, clearFormat);
  }

  std::cout << std::endl;

  auto width = 50;

  std::cout << makeCurInvisible;

  std::atomic<bool> stop{};
  std::thread t{[width, toBegin, clearFormat, &stop] {
    auto minId = 233;
    auto maxId = 255;

    for (int i = 0; i < 1000000; i++) {
      for (int j = 0; j < width; j++) {
        if (stop) {
          return;
        }

        auto id = static_cast<int>(
          std::floor(minId + (maxId - minId) * (std::sin(((i + j) / (width / 2.0)) * 3.14) + 1.0) / 2.0));

        std::cout << std::format("\x1b[38;5;{0}m#", id);
      }

      std::cout << toBegin << clearFormat;

      std::this_thread::sleep_for(50ms);
    }
  }};

  std::cin.ignore();
  std::cout << clearFormat;
  stop = true;
  std::cout << makeCurVisible;

  t.join();
}