// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/application.hpp>
#include <portals/async.hpp>
#include <portals/diagnostics.hpp>

#include <cstdio>
#include <format>

namespace {
struct SmokeReady {};

portals::UiTask smokeTask() {
  co_return std::expected<void, portals::AsyncError>{};
}
} // namespace

/** @brief Exercise SDK language/library support without opening a native terminal. */
int main() {
  try {
    auto application = portals::Application::create();
    auto frame = portals::renderFullFrame({1, 1}, {});
    auto layout = portals::Row{}.arrangeChildren({}, {1, 1});
    portals::Diagnostics diagnostics;
    auto evidence = portals::exportDiagnostics(diagnostics.snapshot());
    auto task = smokeTask();

    if (!application || !frame || !layout || evidence.empty()) {
      return 1;
    }

    const auto message = std::format("Portals WASM SDK smoke: {}", "ready");

    std::puts(message.c_str());

    throw SmokeReady{};
  } catch (const SmokeReady &) {
    return 0;
  } catch (...) {
    return 1;
  }
}
