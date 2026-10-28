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
} // namespace

bool validGlyphSelection(GlyphSelection selection) noexcept {
  return selection.profile >= GlyphProfile::DEFAULT && selection.profile <= GlyphProfile::NF_BACKSLASH &&
         selection.fallback >= GlyphFallback::UNICODE_GLYPHS && selection.fallback <= GlyphFallback::ASCII;
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
GlyphProfileAccess::prepare(Widget &widget, ComputedStyleView style, const GlyphSelection *manualScroll) {
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

  if (!previous && frameManual.profile == GlyphProfile::DEFAULT && scrollManual.profile == GlyphProfile::DEFAULT &&
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

    any = any || (frameOptions && frameSelections[index].profile != GlyphProfile::DEFAULT) ||
          (scroll && scrollSelections[index].profile != GlyphProfile::DEFAULT);
    unchanged = unchanged && previous->states[index] &&
                previous->states[index]->frameSelection == frameSelections[index] &&
                previous->states[index]->scrollSelection == scrollSelections[index];
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
             value->scrollSelection == scrollSelections[index];
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
}

void GlyphProfileAccess::commit(Widget &widget, std::shared_ptr<const PreparedGlyphProfiles> prepared) noexcept {
  if (auto *border = dynamic_cast<Border *>(&widget)) {
    border->glyphProfiles_ = std::move(prepared);
  } else if (auto *scroll = ScrollWidgetAccess::state(widget)) {
    scroll->glyphProfiles = std::move(prepared);
  }
}
} // namespace portals::detail
