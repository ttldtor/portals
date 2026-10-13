// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "ct_pss_support.hpp"
#include <portals/style_pss.hpp>

using namespace ctPssConsumer;

// DSL slice: the same seven constant overrides, with owned source metadata.
inline constexpr auto APPLICATION = sc::pss(R"pss(
* { foreground: #d2d7e1; }
Label { italic: true; }
#action { bold: true; min-width: 6; }
#action:focused { background: #283c50; underline: true; }
)pss",
                                            "application.pss");
inline constexpr auto USER = sc::pss(R"pss(
#action { foreground: #ffd246; }
#action { bold: false; }
Label { italic: false; underline: true; }
)pss",
                                     "user.pss");

int main(int argc, char **argv) try {
  auto application = sc::materializeDocument(APPLICATION);
  auto user = sc::materializeDocument(USER);

  if (!application || !user) {
    const auto &error = application ? user.error() : application.error();

    std::cerr << "PSS setup code=" << static_cast<int>(error.code) << " offset=" << error.location.offset << '\n';

    return 1;
  }

  return styleLayersConsumer::run(argc, argv, initialize, true, &application->sheet, &user->sheet);
} catch (const std::bad_alloc &) {
  std::cerr << "Stylesheet setup ran out of memory\n";

  return 1;
}
