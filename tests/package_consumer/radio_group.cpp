// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style_static.hpp>
#include <portals/ui.hpp>

/** @brief Exercise installed radio declarations and state without starting terminal I/O. */
int main() {
  using namespace portals;

  constexpr ThemeTokens TOKENS{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}, {16, 17, 18}};
  constexpr auto DESCRIPTION = style::ct::themeFromTokens(TOKENS);
  const auto theme = style::ct::materialize(DESCRIPTION);
  const auto readOnly = resolveStyle(theme, {.type = "RadioGroup"}, {}, true);
  const auto editable = resolveStyle(theme, {.type = "RadioGroup"});

  if (readOnly.normal.foreground != TOKENS.muted || readOnly.normal.background != TOKENS.background ||
      readOnly.normal.bold || readOnly.normal.italic || readOnly.normal.underline ||
      readOnly.focused != editable.focused || readOnly.frame[1] != editable.frame[1] ||
      readOnly.selected.background != TOKENS.background || editable.selected.background != TOKENS.background) {
    return 6;
  }

  ui::Ref<RadioGroup> mode;
  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::radioGroup(mode, owner.radioState({"Fast", "Balanced", "Quiet"})).immediate(false);
  });

  if (!scene.prepareChecked()) {
    return 1;
  }

  const auto value = mode.value();
  if (!value || value->selectedText() != "Fast") {
    return 2;
  }

  const auto cleared = mode.select(std::nullopt);
  if (!cleared || !cleared->changed || cleared->snapshot->value.selectedId()) {
    return 3;
  }

  if (!scene.close()) {
    return 4;
  }

  return mode.value() ? 5 : 0;
}
