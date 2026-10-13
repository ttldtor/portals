// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "ct_pss_support.hpp"

using namespace ctPssConsumer;

int main(int argc, char **argv) try {
  // DSL slice: seven constant rules, including partial and explicit false overrides.
  const StyleSheet application{{
    {{}, {.foreground = RgbColor{210, 215, 225}}},
    {{.type = "Label"}, {.italic = true}},
    {{.id = "action"}, {.bold = true, .minWidth = 6}},
    {{.id = "action", .state = StyleState::FOCUSED}, {.background = RgbColor{40, 60, 80}, .underline = true}},
  }};
  const StyleSheet user{{
    {{.id = "action"}, {.foreground = RgbColor{255, 210, 70}}},
    {{.id = "action"}, {.bold = false}},
    {{.type = "Label"}, {.italic = false, .underline = true}},
  }};

  return styleLayersConsumer::run(argc, argv, initialize, true, &application, &user);
} catch (const std::bad_alloc &) {
  std::cerr << "Stylesheet setup ran out of memory\n";

  return 1;
}
