// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "browser_terminal.hpp"
#include "demo_scene.hpp"
#include "level_scene.hpp"

#include <emscripten.h>
#include <format>
#include <limits>

namespace {
EM_JS(int, writeHost, (const char *bytes, std::size_t length), {
  try {
    return Module['portalsWrite'](HEAPU8.subarray(bytes, bytes + length));
  } catch (failure) {
    console.error(failure);
    return -1;
  }
});

EM_JS(int, clearHost, (), {
  try {
    Module['portalsClear']?.();
    return 0;
  } catch (failure) {
    console.error(failure);
    return -1;
  }
});

// Scene owners retire before their borrowed transport, settings and shared theme.
struct BrowserApplication {
  example::Options options;
  std::size_t theme{example::themeIndex(options.theme)};
  portals::CellSize viewport;
  std::unique_ptr<wasm_gallery::BrowserTerminal> terminal;
  std::unique_ptr<wasm_gallery::DemoScene> gallery;
  std::unique_ptr<wasm_gallery::LevelScene> level;
  std::optional<int> requested;
  int active{-1};
  unsigned session{};
  bool retiring{};

  portals::ui::Scene *scene() noexcept {
    return gallery ? &gallery->scene() : level ? &level->scene() : nullptr;
  }

  void retire() {
    gallery.reset();
    level.reset();
    terminal.reset();
    active = -1;
    retiring = false;
  }
};

std::unique_ptr<BrowserApplication> application;
std::string lastError;
unsigned lastSession{};

int fail(std::string message) {
  lastError = std::move(message);

  return -1;
}

int managedFailure(std::string_view operation, const portals::ManagedError &error) {
  return fail(std::format("{} failed (phase {}, cause {}).", operation, static_cast<int>(error.phase),
                          error.cause.index()));
}

int startScene(BrowserApplication &owner, int id) {
  if (lastSession == std::numeric_limits<unsigned>::max()) {
    return fail("Browser session identifiers exhausted; reload the page.");
  }

  auto terminal = wasm_gallery::BrowserTerminal::create(owner.viewport, [](std::string_view bytes) -> std::expected<std::size_t, portals::TerminalError> {
    const auto accepted = writeHost(bytes.data(), bytes.size());

    if (accepted < 0) {
      return std::unexpected(portals::TerminalError{portals::TerminalErrorKind::OUTPUT_UNCERTAIN});
    }

    return static_cast<std::size_t>(accepted);
  });

  if (!terminal) {
    return fail("Cannot create the browser terminal.");
  }

  owner.terminal = std::move(*terminal);

  if (id == 0) {
    owner.gallery = std::make_unique<wasm_gallery::DemoScene>(portals::ui::opts(owner.options), owner.theme);
  } else {
    owner.level = std::make_unique<wasm_gallery::LevelScene>(portals::ui::opts(owner.options), owner.theme);
  }

  auto started = owner.scene()->start(*owner.terminal);

  if (!started) {
    return managedFailure("Application startup", started.error());
  }

  owner.session = ++lastSession;
  owner.active = id;

  return 0;
}

bool receivesSensor(unsigned session) noexcept {
  return application && application->active == 1 && !application->requested && !application->retiring &&
         application->session == session;
}
} // namespace

extern "C" {
/** @param columns Initial nonnegative terminal width in cells.
 * @param rows Initial nonnegative terminal height in cells.
 * @return Zero after starting Gallery, or minus one with an owning diagnostic.
 */
EMSCRIPTEN_KEEPALIVE int portals_start(int columns, int rows) {
  if (application) {
    return fail("The browser application has already started.");
  }

  if (columns < 0 || rows < 0) {
    return fail("Invalid terminal viewport.");
  }

  try {
    application = std::make_unique<BrowserApplication>();
    application->viewport = {columns, rows};

    return startScene(*application, 0);
  } catch (const std::exception &error) {
    return fail(error.what());
  } catch (...) {
    return fail("Unknown application startup failure.");
  }
}

/** @param id Zero for a fresh Gallery, one for a fresh Level.
 * @return Zero after retaining latest intent, or minus one for an invalid request.
 * No scene is destroyed or started during this export or inside a UI callback.
 */
EMSCRIPTEN_KEEPALIVE int portals_select_app(int id) {
  if (!application || (id != 0 && id != 1)) {
    return fail("Invalid application selection.");
  }

  application->requested = id;

  return 0;
}

/** @return Active app identity, or minus one while retiring, switching or idle. */
EMSCRIPTEN_KEEPALIVE int portals_app() {
  return application && !application->requested && !application->retiring ? application->active : -1;
}

/** @return Monotonic session token; zero before startup, unchanged while idle. */
EMSCRIPTEN_KEEPALIVE unsigned portals_session() {
  return application ? application->session : 0;
}

/** @return Zero to continue/switch, one while idle after Quit, or minus one after failure.
 * Each call advances at most one bounded runtime step. Transport restoration completes
 * before owner retirement; replacement creation happens on a subsequent host tick.
 */
EMSCRIPTEN_KEEPALIVE int portals_step() {
  if (!application) {
    return fail("The browser application has not started.");
  }

  try {
    auto &owner = *application;
    auto *scene = owner.scene();

    if (!scene) {
      if (!owner.requested) {
        return 1;
      }

      const int next = *owner.requested;

      owner.requested.reset();

      if (clearHost() < 0) {
        return fail("Cannot clear the previous application.");
      }

      return startScene(owner, next);
    }

    if (owner.requested && !owner.retiring) {
      auto requested = scene->quit();

      if (!requested) {
        return managedFailure("Application stop request", requested.error());
      }

      owner.retiring = true;
    }

    if (owner.level && !owner.retiring) {
      owner.level->beforeStep();
    }

    auto step = scene->step();

    if (!step) {
      return managedFailure("Application step", step.error());
    }

    if (!step->finished) {
      return 0;
    }

    owner.retire();

    if (owner.requested) {
      return 0;
    }

    return 1;
  } catch (const std::exception &error) {
    return fail(error.what());
  } catch (...) {
    return fail("Unknown application step failure.");
  }
}

/** @param session Expected owning Level generation; stale or non-Level deliveries are ignored.
 * @param beta Finite pitch in radians, normalized internally.
 * @param gamma Finite roll in radians, normalized internally.
 * @param angle Screen rotation in degrees: 0, 90, 180 or 270.
 * @return Zero, including ignored malformed/stale deliveries; no borrowed values survive.
 */
EMSCRIPTEN_KEEPALIVE int portals_orientation(unsigned session, double beta, double gamma, int angle) {
  if (receivesSensor(session) && std::isfinite(beta) && std::isfinite(gamma) &&
      (angle == 0 || angle == 90 || angle == 180 || angle == 270)) {
    application->level->orientation(beta, gamma, angle);
  }

  return 0;
}

/** @param session Expected owning Level generation; stale/non-Level deliveries are ignored.
 * @param state Zero unavailable, one waiting, two paused; invalid values are ignored.
 * @return Zero; owning status is applied at the next runtime step.
 */
EMSCRIPTEN_KEEPALIVE int portals_sensor_state(unsigned session, int state) {
  if (receivesSensor(session) && state >= 0 && state <= 2) {
    application->level->sensorState(state);
  }

  return 0;
}

/** @param bytes Borrowed UTF-8 fragment, copied before return.
 * @param length Byte count; null is accepted only for an empty fragment.
 * @return Zero after admission or deliberate idle/transition discard; minus one on transport failure.
 */
EMSCRIPTEN_KEEPALIVE int portals_input(const char *bytes, std::size_t length) {
  if (!application || (!bytes && length != 0)) {
    return fail("Invalid terminal input.");
  }

  if (!application->scene() || application->requested || application->retiring) {
    return 0;
  }

  try {
    auto pushed = application->terminal->pushInput(length ? std::string_view(bytes, length) : std::string_view{});

    return pushed ? 0 : fail("Terminal input failed or exceeded its queue limit.");
  } catch (...) {
    return fail("Cannot retain terminal input.");
  }
}

/** @param columns Latest nonnegative terminal width in cells, retained across idle/switching.
 * @param rows Latest nonnegative terminal height in cells.
 * @return Zero after admission, or minus one on invalid dimensions/transport failure.
 */
EMSCRIPTEN_KEEPALIVE int portals_resize(int columns, int rows) {
  if (!application || columns < 0 || rows < 0) {
    return fail("Invalid terminal viewport.");
  }

  application->viewport = {columns, rows};

  if (!application->scene() || application->retiring || application->requested) {
    return 0;
  }

  auto resized = application->terminal->resize(application->viewport);

  return resized ? 0 : fail("Terminal resize failed.");
}

/** @return Borrowed null-terminated diagnostic, valid until the next failed export call. */
EMSCRIPTEN_KEEPALIVE const char *portals_error() {
  return lastError.c_str();
}

/** @brief Cancel active startup/animation/switch, then destroy scene before transport/options.
 * UI thread only, outside all exports and host callbacks; repeated calls are harmless.
 */
EMSCRIPTEN_KEEPALIVE void portals_shutdown() {
  application.reset();
}
}

int main() {
  return 0;
}
