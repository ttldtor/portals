// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "style_pss_support.hpp"

using namespace stylePssConsumer;

/** @brief Source modifiers and weak observation of the Scene-owned live controller. */
struct Client {
  ui::LiveThemeRef<Theme> live;

  /** @param scene Stationary receiving facade; styles enable tiers before live attachment. */
  void install(ui::Scene &scene) {
    scene.style(APPLICATION, {.name = "application"}) | ui::style(USER, {.tier = StyleTier::USER, .name = "user"});
    (void)attach(scene);
  }

  /** @return Original scalar value or live authority/lifetime failure. */
  auto value() const {
    return live.get<"width">();
  }

  /** @param width Candidate minimum width.
   * @return Original atomic update/no-change result or typed style failure.
   */
  auto width(std::int32_t width) {
    return live.set<"width">(width);
  }

  /** @param scene Same stationary facade receiving an explicitly fresh live stage.
   * @return Checked scene result; controller ownership and diagnostics stay in Scene.
   */
  std::expected<void, ManagedError> attach(ui::Scene &scene) {
    const auto stage = ui::liveTheme(THEME);

    live = stage.ref();
    scene | stage;

    return scene.checked();
  }
};

int main(int argc, char **argv) try { return run<Client>(argc, argv); } catch (const std::bad_alloc &) {
  std::cerr << "Stylesheet setup ran out of memory\n";

  return 1;
}
