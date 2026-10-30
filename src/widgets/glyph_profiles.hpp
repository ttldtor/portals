// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "frame_text.hpp"

namespace portals::detail {
struct PreparedScrollbarSkin;
using FractionalScrollGlyphs = std::array<std::optional<GraphemeText>, 15>;
// Track H/V, four thumb segments H/V, four functional arrows. Prepared only on opt-in.
inline constexpr std::size_t PROFILE_HORIZONTAL_THUMB = 2;
inline constexpr std::size_t PROFILE_VERTICAL_THUMB = 6;
inline constexpr std::size_t PROFILE_ARROWS = 10;
inline constexpr std::size_t PROFILE_SCROLL_GLYPH_COUNT = 14;
using ProfileScrollGlyphs = std::array<std::optional<GraphemeText>, PROFILE_SCROLL_GLYPH_COUNT>;

struct PreparedStyleScrollbarGlyphs {
  ScrollbarGlyphRefs sources;
  std::array<std::optional<GraphemeText>, SCROLLBAR_LITERAL_GLYPH_COUNT> glyphs;
};

struct PreparedGlyphState {
  GlyphSelection scrollSelection;
  GlyphSelection frameSelection;
  std::unique_ptr<ProfileScrollGlyphs> scrollbar;
  std::unique_ptr<PreparedFrameText> frame;
  std::shared_ptr<const FractionalScrollGlyphs> fractional;
  std::unique_ptr<PreparedStyleScrollbarGlyphs> literals;
};

struct PreparedGlyphProfiles {
  std::array<std::shared_ptr<const PreparedGlyphState>, 6> states{};
};

// Semantic admission errors are typed. Allocation errors propagate to the existing transaction boundary.
std::expected<GraphemeText, TreeError> profileGlyph(std::string_view source, std::string_view unicode,
                                                    std::string_view ascii, WidthPolicy policy, GlyphFallback fallback);
std::array<std::string_view, PROFILE_SCROLL_GLYPH_COUNT> profileSources(GlyphSelection selection) noexcept;
std::expected<CustomTitleDecoration, TreeError> profileTitleEnds(GlyphSelection selection, WidthPolicy policy);
bool validGlyphSelection(GlyphSelection selection) noexcept;

struct GlyphProfileAccess {
  static std::expected<std::shared_ptr<const PreparedGlyphProfiles>, TreeError>
  prepare(Widget &widget, ComputedStyleView style, const GlyphSelection *manualScroll = nullptr,
          const PreparedScrollbarSkin *manualSkin = nullptr, std::size_t *failedLiteral = nullptr);
  static void commit(Widget &widget, std::shared_ptr<const PreparedGlyphProfiles> prepared) noexcept;
};
} // namespace portals::detail
