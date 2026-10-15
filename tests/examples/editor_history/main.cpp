// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../../support/editor_history.hpp"
#include "../../support/examples/replay/terminal.hpp"

namespace {
using namespace portals;
using namespace editor_history_test;

/** @return Finite native VT input through the existing replay terminal; no synthetic binding is used. */
std::vector<example::ReplayTerminal::Input> replayInput() {
  return {{20, "A", {}},       {40, "\x1bOP", {}},  {60, "\x1a", {}},    {80, "\x1bOP", {}},    {100, "\x19", {}},
          {120, "\x1bOP", {}}, {140, "\x1bOR", {}}, {160, "\x1bOP", {}}, {180, "\x1b[17~", {}}, {200, "B", {}},
          {220, "\x1bOP", {}}, {240, "\t", {}},     {260, "\x1bOP", {}}, {280, "\x1b[18~", {}}, {300, "C", {}},
          {320, "\x1bOP", {}}, {340, "\t", {}},     {360, "\x1bOP", {}}, {380, "\x1b[18~", {}}, {400, "\r", {}},
          {420, "\x1bOP", {}}, {440, "\x1a", {}},   {460, "\x1bOP", {}}, {480, "\x19", {}},     {500, "\x1bOP", {}},
          {520, "\x1bOR", {}}, {540, "\x1b", {}},   {600, "\x1bOP", {}}, {640, "\x1b[21~", {}}};
}
} // namespace

/** @brief Run the three-mode editor interactively or through real finite input replay.
 * @param argc Argument count, including the program name.
 * @param argv Borrowed arguments, used only during option parsing.
 * @return Zero after success/help, two for unsupported options, one for any check or typed runtime failure.
 */
int main(int argc, char **argv) {
  bool check{};
  bool replay{};

  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];

    if (option == "--check") {
      check = true;
      replay = true;
    } else if (option == "--replay") {
      replay = true;
    } else if (option == "--help") {
      std::cout << "Usage: portals_editor_history [--check | --replay | --help]\n"
                   "F5/F6/F7 select IMMEDIATE/ON_COMPLETION/EXPLICIT. F3 appends !.\n"
                   "Ctrl+Z Undo, Ctrl+Y Redo; Enter confirms, Escape adopts model; Tab blurs.\n"
                   "F10 exits. --check verifies buttons, visible model rows and finite VT replay.\n";

      return 0;
    } else {
      std::cerr << "Unknown option: " << option << '\n';

      return 2;
    }
  }

  try {
    if (check) {
      Demo demo;

      demo.checkContracts();
    }

    example::ReplayTerminal terminal(replayInput(), REPLAY_END_MS, CHECK_VIEWPORT);
    ui::SceneOptions options;

    options.clock = replay ? terminal.clock() : SchedulerClock{};

    Demo demo(options);
    std::size_t checkpoints{};

    if (check) {
      struct Checkpoint {
        std::size_t row;
        std::string_view draft;
        std::string_view model;
      };

      static constexpr std::array<Checkpoint, 12> EXPECTED_CHECKPOINTS{{
        {0, "baseA", "baseA"},
        {0, "base", "base"},
        {0, "baseA", "baseA"},
        {0, "baseA!", "baseA!"},
        {1, "baseB", "base"},
        {1, "baseB", "baseB"},
        {2, "baseC", "base"},
        {2, "baseC", "base"},
        {2, "baseC", "baseC"},
        {2, "base", "baseC"},
        {2, "baseC", "baseC"},
        {2, "baseC", "baseC"},
      }};

      demo.checkpoint([&] {
        expect(checkpoints < EXPECTED_CHECKPOINTS.size(), "unexpected replay checkpoint");

        const auto &expected = EXPECTED_CHECKPOINTS[checkpoints++];

        demo.verify(expected.row, expected.draft, expected.model, false);
      });
    }

    const auto result = demo.run(replay ? &terminal : nullptr);

    if (result) {
      return result;
    }

    if (check) {
      expect(checkpoints == 12, "all twelve replay checkpoints must run");
      expect(terminal.closed && !terminal.output.empty(), "replay must render output and close its transport");
      expect(demo.values() == std::array<std::string, EDITOR_COUNT>{"baseA!", "baseB", "baseC"},
             "final replay models must preserve the three confirmation policies");
      std::cout << "editor history: buttons, read-only model rows, three modes and 12 VT checkpoints passed\n";
    } else if (replay) {
      const auto values = demo.values();

      std::cout << "Replay models: " << values[0] << " | " << values[1] << " | " << values[2] << '\n';
    }

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "editor history demo: " << error.what() << '\n';

    return 1;
  }
}
