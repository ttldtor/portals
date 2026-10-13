// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "style_layers_support.hpp"

namespace ctPssConsumer {
using namespace styleLayersConsumer;

/** @brief Keep the original reactive CT-002 theme alongside scalar PSS overrides. */
struct Controller {
  ManagedScene *scene;                                   ///< Observes the stable managed owner for this run only.
  sc::LiveTheme<std::remove_cv_t<decltype(THEME)>> live; ///< Owns the existing theme parameter handle.

  /** @return Checked replacement theme result; application and user layers retain priority. */
  auto switchTheme() {
    return scene->theme(themeFromTokens(staticStylesConsumer::SECOND));
  }

  /** @return Checked attachment result; success retains the original LiveTheme handle. */
  std::expected<void, ManagedError> attachLive() {
    auto result = sc::live(*scene, THEME);

    if (!result) {
      return std::unexpected(result.error());
    }

    live = *result;

    return {};
  }

  /** @param value Nonnegative button minimum width in terminal cells.
   * @return Checked style update, including the intentional negative-width failure.
   */
  std::expected<void, ManagedError> width(std::int32_t value) {
    const auto result = live.set<"width">(value);

    return result ? std::expected<void, ManagedError>{} : std::unexpected(result.error());
  }
};

/** @param scene Stable owner supplied by configure after the scene is mounted.
 * @return Attached controller or the original typed managed failure.
 */
inline std::expected<Controller, ManagedError> initialize(ManagedScene &scene) {
  Controller result{&scene, {}};
  const auto attached = result.attachLive();

  return attached ? std::expected<Controller, ManagedError>(result) : std::unexpected(attached.error());
}
} // namespace ctPssConsumer
