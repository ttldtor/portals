// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style_pss.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <string_view>

namespace {
using namespace portals;
constexpr std::size_t MAX_FUZZ_BYTES = 16 * 1024;

void require(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "PSS fuzz invariant failed at %s:%u\n", location.file_name(), location.line());
    std::abort();
  }
}

void exercise(std::string_view source) {
  auto sheet = parsePss(source);
  const auto document = parsePssDocument(source, "fuzz.pss");

  // ASCII input without NUL has identical legacy and strict byte admission.
  const bool sameAdmission = std::ranges::all_of(source, [](unsigned char byte) {
    return byte != 0 && byte < 128;
  });

  if (sameAdmission) {
    require(sheet.has_value() == document.has_value());

    if (sheet) {
      require(*sheet == document->sheet);
      require(document->origins.size() == sheet->rules.size());
    } else {
      require(sheet.error().line == document.error().location.line);
      require(sheet.error().column == document.error().location.column);
    }
  }

  if (!sheet) {
    require(sheet.error().line > 0 && sheet.error().column > 0);
    require(!sheet.error().message.empty());

    return;
  }

  require(sheet->rules.size() <= MAX_STYLE_RULES);

  // Exercise successfully parsed owning data after a temporary input has gone away.
  const auto copyFromTemporary = [&] {
    const std::string temporary(source);

    return parsePss(temporary);
  }();

  require(copyFromTemporary.has_value());
  require(*sheet == *copyFromTemporary);

  for (const auto &target : std::array{StyleTarget{"Button", "action", {"primary"}},
                                       StyleTarget{"LineEdit", "message", {}}, StyleTarget{}}) {
    const auto computed = resolveStyle(*copyFromTemporary, target);

    require(computed.minWidth >= 0);
  }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  if (size > MAX_FUZZ_BYTES) {
    return 0;
  }

  exercise({reinterpret_cast<const char *>(data), size});

  return 0;
}
