// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "glyph_profiles.hpp"
#include "scroll_controls_state.hpp"

#include <algorithm>

namespace portals::detail {
namespace {
constexpr std::array<std::string_view, PROFILE_SCROLL_GLYPH_COUNT> ASCII_SOURCES{".", ":", "[", "=", "]", "#", "+",
                                                                                 "#", "+", "#", "<", ">", "^", "v"};
constexpr std::array<std::string_view, PROFILE_SCROLL_GLYPH_COUNT> UNICODE_SOURCES{"─", "│", "◖", "█", "◗", "●", "▄",
                                                                                   "█", "▀", "█", "◀", "▶", "▲", "▼"};

GlyphSelection effectiveSelection(GlyphSelection manual, const GlyphAppearance &style) noexcept {
  return {style.profile && *style.profile != GlyphProfile::DEFAULT ? *style.profile : manual.profile,
          style.fallback.value_or(manual.fallback)};
}

bool oneCell(const std::expected<GraphemeText, TextError> &text) noexcept {
  return text && text->graphemes().size() == 1 && text->measure() == 1 &&
         text->graphemes().front().display == TextDisplay::PRINTABLE;
}

std::expected<std::shared_ptr<const FractionalScrollGlyphs>, TreeError> fractionalGlyphs(WidthPolicy policy) {
  thread_local std::weak_ptr<const FractionalScrollGlyphs> cached;
  if (auto value = cached.lock(); value && (*value)[0]->policy() == policy) {
    return value;
  }
  constexpr std::array<std::string_view, 15> SOURCES{"▏", "▎", "▍", "▌", "▋", "▊", "▉", "▁",
                                                     "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  auto value = std::make_shared<FractionalScrollGlyphs>();
  for (std::size_t index = 0; index < SOURCES.size(); ++index) {
    auto text = GraphemeText::from(SOURCES[index], policy);
    if (!oneCell(text)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    (*value)[index].emplace(std::move(*text));
  }
  cached = value;
  return std::shared_ptr<const FractionalScrollGlyphs>(std::move(value));
}

bool compatibleCoverage(const ScrollWidgetState &state, const PreparedScrollbarSkin *skin,
                        const ScrollbarAppearance &appearance, GlyphSelection selection,
                        const ScrollbarGlyphRefs &literals) {
  const auto track = appearance.trackPattern ? appearance.trackPattern : skin ? skin->trackPattern : std::nullopt;
  if (selection.profile != GlyphProfile::DEFAULT ||
      (state.isArea ? state.area.glyphs : state.bar.glyphs) == PrimitiveGlyphs::ASCII) {
    return false;
  }
  for (std::size_t first : {std::size_t{0}, std::size_t{4}}) {
    const auto literalFirst = first == 0 ? 0 : SCROLLBAR_AXIS_LITERAL_GLYPH_COUNT;
    const bool literalTrack = skin && skin->customGlyphs[first];
    const bool blank = literalTrack             ? skin->glyphs[first]->bytes() == " "
                       : literals[literalFirst] ? literals[literalFirst]->text == " "
                                                : track && (*track == ScrollbarPattern::MINIMAL ||
                                                            *track == ScrollbarPattern::FRACTIONAL_BLOCK);
    if (!blank || (skin && skin->customGlyphs[first + 1]) || literals[literalFirst + 1]) {
      return false;
    }
    for (std::size_t segment = SCROLLBAR_LITERAL_THUMB_SEGMENTS; segment < SCROLLBAR_AXIS_LITERAL_GLYPH_COUNT;
         ++segment) {
      if (literals[literalFirst + segment]) {
        return false;
      }
    }
  }
  return !skin || std::ranges::none_of(skin->thumbSegments, [](const auto &glyph) {
    return glyph.has_value();
  });
}
} // namespace

bool validGlyphSelection(GlyphSelection selection) noexcept {
  return selection.profile >= GlyphProfile::DEFAULT && selection.profile <= GlyphProfile::NF_BACKSLASH &&
         selection.fallback >= GlyphFallback::UNICODE_GLYPHS && selection.fallback <= GlyphFallback::ASCII;
}

bool fractionalScrollbar(const ScrollWidgetState &state) noexcept {
  const auto index = !state.enabled ? 2 : state.focused ? 1 : 0;
  return state.glyphProfiles && state.glyphProfiles->states[index] && state.glyphProfiles->states[index]->fractional;
}

std::expected<GraphemeText, TreeError> profileGlyph(std::string_view source, std::string_view unicode,
                                                    std::string_view ascii, WidthPolicy policy,
                                                    GlyphFallback fallback) {
  auto text = GraphemeText::from(source, policy);

  if (oneCell(text)) {
    return std::move(*text);
  }

  if (fallback == GlyphFallback::UNICODE_GLYPHS) {
    text = GraphemeText::from(unicode, policy);

    if (oneCell(text)) {
      return std::move(*text);
    }
  }

  text = GraphemeText::from(ascii, policy);

  return oneCell(text) ? std::expected<GraphemeText, TreeError>(std::move(*text))
                       : std::unexpected(TreeError::INVALID_ARGUMENT);
}

std::array<std::string_view, PROFILE_SCROLL_GLYPH_COUNT> profileSources(GlyphSelection selection) noexcept {
  if (selection.profile == GlyphProfile::ASCII) {
    return ASCII_SOURCES;
  }

  auto result = UNICODE_SOURCES;

  switch (selection.profile) {
  case GlyphProfile::NF_ROUNDED:
    result[2] = "\uE0B6";
    result[4] = "\uE0B4";

    break;
  case GlyphProfile::NF_CHEVRON:
    result[2] = "\uE0B2";
    result[4] = "\uE0B0";
    result[5] = "◆";

    break;
  case GlyphProfile::NF_SLASH:
    result[0] = "┄";
    result[2] = "\uE0BA";
    result[4] = "\uE0BC";
    result[5] = "█";

    break;
  case GlyphProfile::NF_BACKSLASH:
    result[0] = "┄";
    result[2] = "\uE0BE";
    result[4] = "\uE0B8";
    result[5] = "█";

    break;
  default:
    break;
  }

  if (selection.profile >= GlyphProfile::NF_ROUNDED && selection.fallback == GlyphFallback::ASCII) {
    for (std::size_t index = PROFILE_VERTICAL_THUMB; index < PROFILE_ARROWS; ++index) {
      result[index] = ASCII_SOURCES[index];
    }
  }

  return result;
}

std::expected<CustomTitleDecoration, TreeError> profileTitleEnds(GlyphSelection selection, WidthPolicy policy) {
  const auto sources = profileSources(selection);
  auto left = profileGlyph(sources[2], UNICODE_SOURCES[2], "[", policy, selection.fallback);
  auto right = profileGlyph(sources[4], UNICODE_SOURCES[4], "]", policy, selection.fallback);

  if (!left || !right) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return CustomTitleDecoration{std::string(left->bytes()) + " ", " " + std::string(right->bytes())};
}

std::expected<std::shared_ptr<const PreparedGlyphProfiles>, TreeError>
GlyphProfileAccess::prepare(Widget &widget, ComputedStyleView style, const GlyphSelection *manualScroll,
                            const PreparedScrollbarSkin *manualSkin, std::size_t *failedLiteral) try {
  auto *border = dynamic_cast<Border *>(&widget);
  auto *scroll = ScrollWidgetAccess::state(widget);

  if (!border && !scroll) {
    return nullptr;
  }

  if (border && !border->text_) {
    return std::unexpected(border->preparationError_.value_or(TreeError::INVALID_ARGUMENT));
  }

  const auto *frameOptions = border ? &border->options_ : scroll ? scrollFrameOptions(*scroll) : nullptr;
  const auto frameManual = frameOptions ? frameOptions->border.glyphSelection : GlyphSelection{};
  const auto scrollManual = manualScroll ? *manualScroll : scroll ? scroll->glyphSelection : GlyphSelection{};
  const auto &previous = border ? border->glyphProfiles_ : scroll->glyphProfiles;
  const auto *skin = manualSkin ? manualSkin : scroll ? scroll->scrollbarSkin.get() : nullptr;
  static const ScrollbarGlyphStates EMPTY_LITERALS{};
  const auto &literalStates = style.scrollbarGlyphs ? *style.scrollbarGlyphs : EMPTY_LITERALS;
  const bool hasLiterals = scroll && style.scrollbarGlyphs;
  if (hasLiterals && failedLiteral) {
    for (std::size_t index = 0; index < literalStates.size(); ++index) {
      const auto found = std::ranges::find_if(literalStates[index], [](const auto &leaf) {
        return bool(leaf);
      });
      if (found != literalStates[index].end()) {
        *failedLiteral =
          index * SCROLLBAR_LITERAL_GLYPH_COUNT + static_cast<std::size_t>(found - literalStates[index].begin());
        break;
      }
    }
  }
  std::array<bool, 6> coverage{};
  if (scroll) {
    for (std::size_t index = 0; index < coverage.size(); ++index) {
      const auto pattern = style.scrollbar[index].thumbPattern ? style.scrollbar[index].thumbPattern
                           : skin                              ? skin->thumbPattern
                                                               : std::nullopt;
      coverage[index] = pattern == ScrollbarThumbPattern::FRACTIONAL_BLOCK;
      if (coverage[index] &&
          !compatibleCoverage(*scroll, skin, style.scrollbar[index],
                              effectiveSelection(scrollManual, style.glyphs[index]), literalStates[index])) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
  }

  if (!previous && !hasLiterals &&
      std::ranges::none_of(coverage,
                           [](bool value) {
                             return value;
                           }) &&
      frameManual.profile == GlyphProfile::DEFAULT && scrollManual.profile == GlyphProfile::DEFAULT &&
      std::ranges::none_of(style.glyphs, [](const auto &value) {
        return value.profile && *value.profile != GlyphProfile::DEFAULT;
      })) {
    return nullptr;
  }

  const auto policy = border ? border->text_->title.policy() : scroll->glyphs[0].policy();
  std::array<GlyphSelection, 6> frameSelections;
  std::array<GlyphSelection, 6> scrollSelections;
  bool any{};
  bool unchanged = static_cast<bool>(previous);

  for (std::size_t index = 0; index < style.glyphs.size(); ++index) {
    frameSelections[index] = effectiveSelection(frameManual, style.glyphs[index]);
    scrollSelections[index] = effectiveSelection(scrollManual, style.glyphs[index]);

    if (!validGlyphSelection(frameSelections[index]) || !validGlyphSelection(scrollSelections[index])) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    any = any || hasLiterals || coverage[index] ||
          (frameOptions && frameSelections[index].profile != GlyphProfile::DEFAULT) ||
          (scroll && scrollSelections[index].profile != GlyphProfile::DEFAULT);
    unchanged = unchanged && previous->states[index] &&
                previous->states[index]->frameSelection == frameSelections[index] &&
                previous->states[index]->scrollSelection == scrollSelections[index] &&
                static_cast<bool>(previous->states[index]->fractional) == coverage[index];
    if (unchanged) {
      unchanged = equalScrollbarGlyphs(previous->states[index]->literals ? previous->states[index]->literals->sources
                                                                         : EMPTY_LITERALS[index],
                                       literalStates[index]);
    }
  }

  if (!any) {
    return nullptr;
  }

  if (unchanged) {
    return previous;
  }

  auto prepared = std::make_shared<PreparedGlyphProfiles>();

  for (std::size_t index = 0; index < style.glyphs.size(); ++index) {
    const auto matches = [&](const auto &value) {
      return value && value->frameSelection == frameSelections[index] &&
             value->scrollSelection == scrollSelections[index] &&
             static_cast<bool>(value->fractional) == coverage[index] &&
             ((!value->literals && !style.scrollbarGlyphs) ||
              equalScrollbarGlyphs(value->literals ? value->literals->sources : EMPTY_LITERALS[index],
                                   literalStates[index]));
    };

    if (previous) {
      for (const auto &value : previous->states) {
        if (matches(value)) {
          prepared->states[index] = value;

          break;
        }
      }
    }

    for (std::size_t prior = 0; !prepared->states[index] && prior < index; ++prior) {
      if (matches(prepared->states[prior])) {
        prepared->states[index] = prepared->states[prior];
      }
    }

    if (prepared->states[index]) {
      continue;
    }

    auto value = std::make_shared<PreparedGlyphState>();

    value->frameSelection = frameSelections[index];
    value->scrollSelection = scrollSelections[index];
    if (scroll && std::ranges::any_of(literalStates[index], [](const auto &leaf) {
          return bool(leaf);
        })) {
      value->literals = std::make_unique<PreparedStyleScrollbarGlyphs>();
      value->literals->sources = literalStates[index];
      for (std::size_t glyph = 0; glyph < SCROLLBAR_LITERAL_GLYPH_COUNT; ++glyph) {
        const auto &source = value->literals->sources[glyph];
        if (!source) {
          continue;
        }
        const auto part = glyph % SCROLLBAR_AXIS_LITERAL_GLYPH_COUNT;
        const bool decorative = glyph == SCROLLBAR_LITERAL_CORNER || part == 2 || part == 3;
        if (source->text.empty() && decorative) {
          continue;
        }
        if (failedLiteral) {
          *failedLiteral = index * SCROLLBAR_LITERAL_GLYPH_COUNT + glyph;
        }
        auto text = GraphemeText::from(source->text, policy);
        if (source->text.size() > DEFAULT_SCROLLBAR_GLYPH_BYTES || !oneCell(text)) {
          if (failedLiteral) {
            *failedLiteral = index * SCROLLBAR_LITERAL_GLYPH_COUNT + glyph;
          }
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        value->literals->glyphs[glyph].emplace(std::move(*text));
      }
    }
    if (coverage[index]) {
      auto palette = fractionalGlyphs(policy);
      if (!palette) {
        return std::unexpected(palette.error());
      }
      value->fractional = std::move(*palette);
    }

    if (scroll && value->scrollSelection.profile != GlyphProfile::DEFAULT) {
      value->scrollbar = std::make_unique<ProfileScrollGlyphs>();

      const auto sources = profileSources(value->scrollSelection);

      for (std::size_t glyph = 0; glyph < sources.size(); ++glyph) {
        auto text = profileGlyph(sources[glyph], UNICODE_SOURCES[glyph], ASCII_SOURCES[glyph], policy,
                                 value->scrollSelection.fallback);

        if (!text) {
          return std::unexpected(text.error());
        }

        (*value->scrollbar)[glyph].emplace(std::move(*text));
      }
    }

    if (frameOptions && value->frameSelection.profile != GlyphProfile::DEFAULT) {
      auto options = *frameOptions;

      options.border.glyphSelection = value->frameSelection;

      auto text = prepareFrameText(policy, options);

      if (!text) {
        return std::unexpected(text.error());
      }

      value->frame = std::make_unique<PreparedFrameText>(std::move(*text));
    }

    prepared->states[index] = std::move(value);
  }

  return prepared;
} catch (const std::bad_alloc &) {
  return std::unexpected(TreeError::NO_MEMORY);
}

void GlyphProfileAccess::commit(Widget &widget, std::shared_ptr<const PreparedGlyphProfiles> prepared) noexcept {
  if (auto *border = dynamic_cast<Border *>(&widget)) {
    border->glyphProfiles_ = std::move(prepared);
  } else if (auto *scroll = ScrollWidgetAccess::state(widget)) {
    scroll->glyphProfiles = std::move(prepared);
  }
}
} // namespace portals::detail
