// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/common.hpp"
#include "../gallery/themes.hpp"

namespace wasm_gallery {
/** @param index Shared preset index, owned by the browser application.
 * @param titleId Identity of this scene's title label.
 * @return Owning preset with the table HEADER role adapted to a normal label.
 */
inline portals::StyleSheet sampleTheme(std::size_t index, std::string_view titleId) {
  using namespace portals;

  auto sheet = example::THEME_PRESETS[index].make();
  StyleTarget table;
  StyleRule title;

  table.type = "Table";

  const auto header = resolveStyle(sheet, table).header;

  title.selector.id = titleId;
  title.declarations.foreground = header.foreground;
  title.declarations.background = header.background;
  title.declarations.bold = header.bold;
  title.declarations.italic = header.italic;
  title.declarations.underline = header.underline;
  sheet.rules.push_back(std::move(title));

  return sheet;
}
} // namespace wasm_gallery
