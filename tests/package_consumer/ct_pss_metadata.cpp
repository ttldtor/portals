// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style_pss.hpp>

#include <iostream>

namespace sc = portals::style::ct;
using namespace portals;

int main() try {
  // The descriptor's arena dies before its owning document is inspected.
  const auto compiled = [] {
    constexpr auto description = sc::pss(R"pss(
* { foreground: #d2d7e1; }
Label { italic: true; }
#action { bold: true; min-width: 6; }
#action:focused { background: #283c50; underline: true; }
)pss",
                                         "application.pss");

    return sc::materializeDocument(description);
  }();

  // Runtime source bytes and source name also die before the result is inspected.
  const auto runtime = [] {
    const std::string bytes = R"pss(
* { foreground: #d2d7e1; }
Label { italic: true; }
#action { bold: true; min-width: 6; }
#action:focused { background: #283c50; underline: true; }
)pss";
    const std::string sourceName = "application.pss";

    return parsePssDocument(bytes, sourceName);
  }();

  if (!compiled || !runtime) {
    const auto &error = compiled ? runtime.error() : compiled.error();

    std::cerr << "PSS document code=" << static_cast<int>(error.code) << " offset=" << error.location.offset << '\n';

    return 1;
  }

  if (*compiled != *runtime || compiled->sourceName != "application.pss" || compiled->origins.size() != 4 ||
      compiled->declarations.size() != 6) {
    std::cerr << "Owning CT/runtime document or origins differed\n";

    return 1;
  }

  // Plain materialize returns only StyleSheet and deliberately discards source metadata.
  constexpr auto description = sc::pss(R"pss(
* { foreground: #d2d7e1; }
Label { italic: true; }
#action { bold: true; min-width: 6; }
#action:focused { background: #283c50; underline: true; }
)pss",
                                       "application.pss");

  if (sc::materialize(description) != compiled->sheet) {
    std::cerr << "Plain stylesheet materialization differed\n";

    return 1;
  }

  return 0;
} catch (const std::bad_alloc &) {
  std::cerr << "PSS document setup ran out of memory\n";

  return 1;
}
