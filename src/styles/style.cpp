// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../widgets/glyph_profiles.hpp"
#include "theme_receiver.hpp"
#include <portals/detail/layout_traversal.hpp>
#include <portals/detail/pss_data.hpp>
#include <portals/style.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <tuple>

namespace portals {
namespace {
// Owning layered staging must not retain references over a nested allocator mutation.
// The guard also rejects opt-in during legacy copies; sequential legacy replacement is preserved.
struct LayerMutationGuard {
  bool &staging;
  bool active;

  LayerMutationGuard(bool &value, bool enabled) : staging(value), active(enabled) {
    if (active) {
      staging = true;
    }
  }

  ~LayerMutationGuard() {
    if (active) {
      staging = false;
    }
  }
};

StyleError error(TreeError cause, std::string_view message) {
  return {cause, 1, 1, message};
}

bool matches(const StyleSelector &selector, const StyleTarget &target, StyleState state) {
  const auto bases = std::span(target.baseTypes).first(std::min(target.baseTypes.size(), MAX_STYLE_BASE_TYPES));

  return (selector.type.empty() || selector.type == "*" || selector.type == target.type ||
          std::ranges::find(bases, selector.type) != bases.end() || (target.control && selector.type == "Control")) &&
         (selector.id.empty() || selector.id == target.id) &&
         (selector.state == StyleState::ANY || selector.state == state) &&
         std::ranges::all_of(selector.classes, [&](const auto &name) {
           return std::ranges::find(target.classes, name) != target.classes.end();
         });
}

using Specificity = std::tuple<bool, std::size_t, bool>;

Specificity specificity(const StyleSelector &selector) {
  return {!selector.id.empty(), selector.classes.size() + (selector.state != StyleState::ANY),
          !selector.type.empty() && selector.type != "*"};
}

using Rank = std::tuple<StyleTier, Specificity, std::uint64_t, std::size_t>;

bool hasPaintDeclarations(const StylePaintDeclarations &values) noexcept {
  return values.foreground || values.background || values.bold || values.italic || values.underline;
}

CellStyle applyPaintOverrides(const StylePaintDeclarations &source, CellStyle resolved) noexcept {
  if (source.foreground) {
    resolved.foreground = source.foreground;
  }

  if (source.background) {
    resolved.background = source.background;
  }

  resolved.bold = source.bold.value_or(resolved.bold);
  resolved.italic = source.italic.value_or(resolved.italic);
  resolved.underline = source.underline.value_or(resolved.underline);

  return resolved;
}

template <class CascadeRank, bool FrameParts, bool ScrollbarParts, bool ThumbSegments, class Visit>
CellStyle resolvePaint(const Visit &visit, const StyleTarget &target, StyleState state, CellStyle base,
                       FrameAppearance &parts, ScrollbarAppearance &scrollbar, ScrollbarThumbAppearance *segments,
                       bool preserveSegments) {
  // Ordinary rules retain the original five ranks; frame ranks exist only in the part specialization.
  constexpr std::size_t PAINT_ATTRIBUTE_COUNT = 5;
  constexpr std::size_t FRAME_PART_COUNT = FrameParts ? 3 : 0;
  constexpr std::size_t SCROLLBAR_PART_COUNT = ScrollbarParts ? 5 : 0;
  constexpr std::size_t THUMB_SEGMENT_COUNT = ThumbSegments ? 4 : 0;
  constexpr std::size_t PART_COUNT = FRAME_PART_COUNT + SCROLLBAR_PART_COUNT + THUMB_SEGMENT_COUNT;
  std::array<std::optional<CascadeRank>, PAINT_ATTRIBUTE_COUNT *(PART_COUNT + 1) + (ScrollbarParts ? 3 : 0)> ranks;
  [[maybe_unused]] std::array<StylePaintDeclarations, PART_COUNT> overrides;
  const auto assign = [&]<class T>(const std::optional<T> &value, auto &destination, std::size_t index,
                                   CascadeRank rank) {
    if (value && (!ranks[index] || rank >= *ranks[index])) {
      destination = *value;
      ranks[index] = rank;
    }
  };
  visit([&](const StyleRule &rule, auto &&makeRank) {
    if (!matches(rule.selector, target, state)) {
      return;
    }

    const CascadeRank rank = makeRank();
    const auto &values = rule.declarations;
    assign(values.foreground, base.foreground, 0, rank);
    assign(values.background, base.background, 1, rank);
    assign(values.bold, base.bold, 2, rank);
    assign(values.italic, base.italic, 3, rank);
    assign(values.underline, base.underline, 4, rank);

    const auto applyParts = [&](const auto &sources, std::size_t first) {
      for (std::size_t part = 0; part < sources.size(); ++part) {
        const auto &source = *sources[part];

        if (!hasPaintDeclarations(source)) {
          continue;
        }

        auto &destination = overrides[first + part];
        const auto offset = PAINT_ATTRIBUTE_COUNT * (first + part + 1);
        assign(source.foreground, destination.foreground, offset, rank);
        assign(source.background, destination.background, offset + 1, rank);
        assign(source.bold, destination.bold, offset + 2, rank);
        assign(source.italic, destination.italic, offset + 3, rank);
        assign(source.underline, destination.underline, offset + 4, rank);
      }
    };

    if constexpr (FrameParts) {
      applyParts(std::array{&values.frameLine, &values.frameTitle, &values.frameEnds}, 0);
    }

    if constexpr (ScrollbarParts) {
      constexpr auto FIRST_PATTERN = PAINT_ATTRIBUTE_COUNT * (PART_COUNT + 1);
      assign(values.scrollbarTrackPattern, scrollbar.trackPattern, FIRST_PATTERN, rank);
      assign(values.scrollbarThumbPattern, scrollbar.thumbPattern, FIRST_PATTERN + 1, rank);
      assign(values.scrollbarArrowPattern, scrollbar.arrowPattern, FIRST_PATTERN + 2, rank);
      applyParts(std::array{&values.scrollbarTrack, &values.scrollbarThumb, &values.scrollbarEnds,
                            &values.scrollbarArrows, &values.scrollbarCorner},
                 FRAME_PART_COUNT);
    }

    if constexpr (ThumbSegments) {
      applyParts(std::array{&values.scrollbarThumbStart, &values.scrollbarThumbBody, &values.scrollbarThumbEnd,
                            &values.scrollbarThumbSingle},
                 FRAME_PART_COUNT + SCROLLBAR_PART_COUNT);
    }
  });

  const auto finishParts = [&](const auto &destinations, std::size_t first) {
    for (std::size_t index = 0; index < destinations.size(); ++index) {
      const auto &source = overrides[first + index];

      if (!hasPaintDeclarations(source)) {
        continue;
      }

      auto &destination = *destinations[index];
      destination = applyPaintOverrides(source, destination.value_or(base));
    }
  };

  if constexpr (FrameParts) {
    finishParts(std::array{&parts.line, &parts.title, &parts.ends}, 0);
  }

  if constexpr (ScrollbarParts) {
    finishParts(std::array{&scrollbar.track, &scrollbar.thumb, &scrollbar.ends, &scrollbar.arrows, &scrollbar.corner},
                FRAME_PART_COUNT);
  }

  if constexpr (ThumbSegments) {
    const auto destinations = std::array{&segments->start, &segments->body, &segments->end, &segments->single};
    const auto common = scrollbar.thumb.value_or(base);

    for (std::size_t index = 0; index < destinations.size(); ++index) {
      const auto &source = overrides[FRAME_PART_COUNT + SCROLLBAR_PART_COUNT + index];
      auto &destination = *destinations[index];

      destination = applyPaintOverrides(source, preserveSegments ? destination : common);
    }
  }

  return base;
}

template <class CascadeRank, class Visit>
ComputedStyle resolveRules(const Visit &visit, const StyleTarget &target, ComputedStyle defaults) noexcept {
  bool frameParts{};
  bool scrollbarParts{};
  bool thumbSegments{};
  bool glyphDeclarations{};
  const bool preserveSegments = defaults.scrollbarThumbSegments.has_value();
  std::optional<CascadeRank> rank;
  visit([&](const StyleRule &rule, auto &&makeRank) {
    const auto &values = rule.declarations;

    glyphDeclarations = glyphDeclarations || ((values.glyphProfile || values.glyphFallback) &&
                                              matches(rule.selector, target, rule.selector.state));

    // Reuse the metric pass; selector states here test applicability, not the current paint state.
    frameParts = frameParts || ((hasPaintDeclarations(values.frameLine) || hasPaintDeclarations(values.frameTitle) ||
                                 hasPaintDeclarations(values.frameEnds)) &&
                                matches(rule.selector, target, rule.selector.state));

    scrollbarParts =
      scrollbarParts || ((hasPaintDeclarations(values.scrollbarTrack) || hasPaintDeclarations(values.scrollbarThumb) ||
                          hasPaintDeclarations(values.scrollbarEnds) || hasPaintDeclarations(values.scrollbarArrows) ||
                          hasPaintDeclarations(values.scrollbarCorner) || values.scrollbarTrackPattern ||
                          values.scrollbarThumbPattern || values.scrollbarArrowPattern) &&
                         matches(rule.selector, target, rule.selector.state));

    thumbSegments =
      thumbSegments ||
      ((hasPaintDeclarations(values.scrollbarThumbStart) || hasPaintDeclarations(values.scrollbarThumbBody) ||
        hasPaintDeclarations(values.scrollbarThumbEnd) || hasPaintDeclarations(values.scrollbarThumbSingle)) &&
       matches(rule.selector, target, rule.selector.state));

    if (!rule.declarations.minWidth || !matches(rule.selector, target, StyleState::ANY)) {
      return;
    }
    const CascadeRank candidate = makeRank();
    if (!rank || candidate >= *rank) {
      rank = candidate;
      defaults.minWidth = *rule.declarations.minWidth;
    }
  });

  if (glyphDeclarations) {
    constexpr std::array STATES{StyleState::NORMAL,   StyleState::FOCUSED, StyleState::DISABLED,
                                StyleState::SELECTED, StyleState::HEADER,  StyleState::CARET};

    for (std::size_t index = 0; index < STATES.size(); ++index) {
      std::optional<CascadeRank> profileRank;
      std::optional<CascadeRank> fallbackRank;

      visit([&](const StyleRule &rule, auto &&makeRank) {
        if (!matches(rule.selector, target, STATES[index])) {
          return;
        }

        const auto candidate = makeRank();
        const auto &values = rule.declarations;

        if (values.glyphProfile && (!profileRank || candidate >= *profileRank)) {
          defaults.glyphs[index].profile = *values.glyphProfile;
          profileRank = candidate;
        }

        if (values.glyphFallback && (!fallbackRank || candidate >= *fallbackRank)) {
          defaults.glyphs[index].fallback = *values.glyphFallback;
          fallbackRank = candidate;
        }
      });
    }
  }

  if (thumbSegments && !preserveSegments) {
    defaults.scrollbarThumbSegments.emplace();
  }

  const auto applyPaintStates = [&]<bool FrameParts, bool ScrollbarParts, bool ThumbSegments>() {
    auto *segments = defaults.scrollbarThumbSegments ? defaults.scrollbarThumbSegments->data() : nullptr;
    defaults.normal = resolvePaint<CascadeRank, FrameParts, ScrollbarParts, ThumbSegments>(
      visit, target, StyleState::NORMAL, defaults.normal, defaults.frame[0], defaults.scrollbar[0], segments,
      preserveSegments);
    defaults.focused = resolvePaint<CascadeRank, FrameParts, ScrollbarParts, ThumbSegments>(
      visit, target, StyleState::FOCUSED, defaults.focused, defaults.frame[1], defaults.scrollbar[1],
      segments ? segments + 1 : nullptr, preserveSegments);
    defaults.disabled = resolvePaint<CascadeRank, FrameParts, ScrollbarParts, ThumbSegments>(
      visit, target, StyleState::DISABLED, defaults.disabled, defaults.frame[2], defaults.scrollbar[2],
      segments ? segments + 2 : nullptr, preserveSegments);
    defaults.selected = resolvePaint<CascadeRank, FrameParts, ScrollbarParts, ThumbSegments>(
      visit, target, StyleState::SELECTED, defaults.selected, defaults.frame[3], defaults.scrollbar[3],
      segments ? segments + 3 : nullptr, preserveSegments);
    defaults.header = resolvePaint<CascadeRank, FrameParts, ScrollbarParts, ThumbSegments>(
      visit, target, StyleState::HEADER, defaults.header, defaults.frame[4], defaults.scrollbar[4],
      segments ? segments + 4 : nullptr, preserveSegments);
    defaults.caret = resolvePaint<CascadeRank, FrameParts, ScrollbarParts, ThumbSegments>(
      visit, target, StyleState::CARET, defaults.caret, defaults.frame[5], defaults.scrollbar[5],
      segments ? segments + 5 : nullptr, preserveSegments);
  };
  const auto applyScrollbarStates = [&]<bool FrameParts>() {
    if (thumbSegments) {
      applyPaintStates.template operator()<FrameParts, true, true>();
    } else if (scrollbarParts) {
      applyPaintStates.template operator()<FrameParts, true, false>();
    } else {
      applyPaintStates.template operator()<FrameParts, false, false>();
    }
  };

  if (frameParts) {
    applyScrollbarStates.template operator()<true>();
  } else {
    applyScrollbarStates.template operator()<false>();
  }

  return defaults;
}

std::expected<void, StyleError> validate(const StyleSheet &sheet) {
  if (sheet.rules.size() > MAX_STYLE_RULES) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Too many style rules"));
  }

  for (const auto &rule : sheet.rules) {
    const auto &d = rule.declarations;

    if ((d.glyphProfile && (*d.glyphProfile < GlyphProfile::DEFAULT || *d.glyphProfile > GlyphProfile::NF_BACKSLASH)) ||
        (d.glyphFallback &&
         (*d.glyphFallback < GlyphFallback::UNICODE_GLYPHS || *d.glyphFallback > GlyphFallback::ASCII)) ||
        (d.scrollbarTrackPattern &&
         (*d.scrollbarTrackPattern < ScrollbarPattern::LEGACY || *d.scrollbarTrackPattern > ScrollbarPattern::ASCII)) ||
        (d.scrollbarThumbPattern && (*d.scrollbarThumbPattern < ScrollbarThumbPattern::LEGACY ||
                                     *d.scrollbarThumbPattern > ScrollbarThumbPattern::ASCII)) ||
        (d.scrollbarArrowPattern && (*d.scrollbarArrowPattern < ScrollbarArrowPattern::LEGACY ||
                                     *d.scrollbarArrowPattern > ScrollbarArrowPattern::ASCII))) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Invalid scrollbar pattern"));
    }

    if (rule.selector.state < StyleState::ANY || rule.selector.state > StyleState::CARET ||
        (rule.declarations.minWidth && (*rule.declarations.minWidth < 0 || rule.selector.state != StyleState::ANY))) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Invalid state or state-dependent metric"));
    }
  }

  return {};
}
} // namespace

std::expected<void, StyleError> validateStyleTarget(const StyleTarget &target) noexcept {
  if (target.baseTypes.size() > MAX_STYLE_BASE_TYPES) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style type lineage is too deep"));
  }

  if (!target.baseTypes.empty() && (target.type.empty() || target.type == "*")) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style lineage requires a concrete type"));
  }

  for (std::size_t index = 0; index < target.baseTypes.size(); ++index) {
    const auto &name = target.baseTypes[index];

    if (name.empty() || name == "*" || name == target.type) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Invalid style ancestor name"));
    }

    const auto preceding = std::span(target.baseTypes).first(index);

    if (std::ranges::find(preceding, name) != preceding.end()) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Duplicate style ancestor name"));
    }
  }

  return {};
}

ComputedStyle resolveStyle(const StyleSheet &sheet, const StyleTarget &target, ComputedStyle defaults) noexcept {
  if (!validateStyleTarget(target)) {
    return defaults;
  }

  return resolveRules<Specificity>(
    [&](auto &&apply) {
      // Source order resolves specificity ties through >=; legacy needs no layer rank fields.
      for (const auto &rule : sheet.rules) {
        apply(rule, [&rule] {
          return specificity(rule.selector);
        });
      }
    },
    target, defaults);
}

StyleSheet themeFromTokens(ThemeTokens tokens) {
  StyleSheet sheet;
  StyleRule normal;

  normal.declarations.foreground = tokens.foreground;
  normal.declarations.background = tokens.background;
  sheet.rules.push_back(normal);

  const auto stateRule = [&](StyleState state, RgbColor foreground, RgbColor background, bool bold) {
    StyleRule rule;

    rule.selector.state = state;
    rule.declarations.foreground = foreground;
    rule.declarations.background = background;
    rule.declarations.bold = bold;
    sheet.rules.push_back(std::move(rule));
  };

  stateRule(StyleState::FOCUSED, tokens.background, tokens.accent, true);
  // Focused chrome uses accent ink on the surface instead of an action fill.
  auto &focused = sheet.rules.back().declarations;
  const StylePaintDeclarations accentOnSurface{
    .foreground = tokens.accent, .background = tokens.background, .bold = true};

  focused.frameLine = accentOnSurface;
  focused.frameTitle = accentOnSurface;
  focused.frameEnds = accentOnSurface;
  // A full-block thumb uses foreground ink; an action fill would hide it in the surface color.
  focused.scrollbarTrack = accentOnSurface;
  focused.scrollbarThumb = accentOnSurface;
  focused.scrollbarEnds = accentOnSurface;
  focused.scrollbarArrows = accentOnSurface;
  focused.scrollbarCorner = accentOnSurface;

  stateRule(StyleState::DISABLED, tokens.muted, tokens.background, false);
  stateRule(StyleState::SELECTED, tokens.foreground, tokens.selection, true);
  sheet.rules.back().declarations.italic = false;
  stateRule(StyleState::HEADER, tokens.heading, tokens.background, true);
  // Reverse the focused field palette so the caret remains visible over any glyph.
  stateRule(StyleState::CARET, tokens.accent, tokens.background, false);

  return sheet;
}

StyleSheet modernTheme() {
  return themeFromTokens(
    {{220, 225, 232}, {24, 28, 36}, {75, 180, 220}, {125, 130, 140}, {45, 75, 110}, {75, 180, 220}});
}

StyleSheet turboTheme() {
  return themeFromTokens({{255, 255, 85}, {0, 0, 170}, {0, 170, 170}, {170, 170, 170}, {170, 0, 0}, {255, 255, 255}});
}

// The following four palettes adapt Textual's MIT-licensed theme definitions.
// Copyright (c) 2021 Will McGugan. See vendor/textual/LICENSE and README.md for provenance.
StyleSheet nordTheme() {
  return themeFromTokens({.foreground = {0xD8, 0xDE, 0xE9},
                          .background = {0x2E, 0x34, 0x40},
                          .accent = {0x88, 0xC0, 0xD0},
                          .muted = {0x81, 0xA1, 0xC1},
                          .selection = {0x43, 0x4C, 0x5E},
                          .heading = {0xB4, 0x8E, 0xAD}});
}

StyleSheet gruvboxTheme() {
  return themeFromTokens({.foreground = {0xFB, 0xF1, 0xC7},
                          .background = {0x28, 0x28, 0x28},
                          .accent = {0x85, 0xA5, 0x98},
                          .muted = {0xA8, 0x9A, 0x85},
                          .selection = {0x50, 0x49, 0x45},
                          .heading = {0xFA, 0xBD, 0x2F}});
}

StyleSheet tokyoNightTheme() {
  return themeFromTokens({.foreground = {0xA9, 0xB1, 0xD6},
                          .background = {0x1A, 0x1B, 0x26},
                          .accent = {0xBB, 0x9A, 0xF7},
                          .muted = {0x7A, 0xA2, 0xF7},
                          .selection = {0x41, 0x48, 0x68},
                          .heading = {0xFF, 0x9E, 0x64}});
}

StyleSheet catppuccinLatteTheme() {
  return themeFromTokens({.foreground = {0x4C, 0x4F, 0x69},
                          .background = {0xEF, 0xF1, 0xF5},
                          .accent = {0x88, 0x39, 0xEF},
                          .muted = {0xDC, 0x8A, 0x78},
                          .selection = {0xCC, 0xD0, 0xDA},
                          .heading = {0xFE, 0x64, 0x0B}});
}

StyleEngine::StyleEngine(WidgetTree &tree, std::size_t maxBindings) : tree_(tree), maxBindings_(maxBindings) {
}

ComputedStyle StyleEngine::resolve(const StyleSheet &sheet, const LayerState &state, const StyleTarget &target,
                                   ComputedStyle defaults, NodeId node) const noexcept {
  if (!state.enabled) {
    return !sheet.scope || *sheet.scope == node ? resolveStyle(sheet, target, defaults) : defaults;
  }
  return resolveRules<Rank>(
    [&](auto &&apply) {
      for (const auto &layer : state.layers) {
        if (state.preview && layer.id == state.themeBase) {
          continue;
        }

        if (layer.sheet.scope && *layer.sheet.scope != node) {
          continue;
        }

        for (std::size_t index = 0; index < layer.sheet.rules.size(); ++index) {
          const auto &rule = layer.sheet.rules[index];
          apply(rule, [&layer, &rule, index] {
            return Rank{layer.tier, specificity(rule.selector), layer.order, index};
          });
        }
      }
    },
    target, defaults);
}

StyleLayerHandle StyleEngine::handle(std::uint64_t id, std::uint64_t epoch) const noexcept {
  StyleLayerHandle result;
  result.lifetime_ = lifetime_;
  result.owner_ = owner_;
  result.epoch_ = epoch;
  result.id_ = id;
  return result;
}

std::expected<void, StyleError> StyleEngine::validateHandle(const StyleLayerHandle &value) const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(TreeError::WRONG_THREAD, "Style layer requires its UI thread"));
  }
  if (value.owner_ != owner_ || value.lifetime_.lock() != lifetime_ || !layerState_.enabled ||
      value.epoch_ != layerState_.epoch ||
      std::ranges::find(layerState_.layers, value.id_, &Layer::id) == layerState_.layers.end()) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style layer handle is foreign or stale"));
  }
  return {};
}

detail::StyleThemeLease StyleEngine::themeLease() const noexcept {
  return {layerState_.enabled, layerState_.epoch, layerState_.enabled ? layerState_.themeBase : 0, layerState_.owner};
}

StyleEngine::PreviewCapture StyleEngine::capturePreview() const noexcept {
  return {revision_, themeLease(), lifetime_};
}

std::expected<void, StyleError> StyleEngine::validatePreviewCapture(const PreviewCapture &capture) const {
  if (capture.lifetime.lock() != lifetime_ || capture.revision != revision_) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Theme selection changed during staging"));
  }

  return validateLease(capture.lease);
}

bool StyleEngine::ownsPreview(const StyleLayerHandle &value) const noexcept {
  return value.owner_ == owner_ && value.lifetime_.lock() == lifetime_ && layerState_.enabled &&
         value.epoch_ == layerState_.epoch && value.id_ && value.id_ == layerState_.preview;
}

std::expected<void, StyleError> StyleEngine::validateLease(detail::StyleThemeLease lease) const {
  auto allowed = checkBoundary(true);
  if (!allowed) {
    return allowed;
  }
  const auto current = themeLease();
  if (lease.layered != current.layered || lease.epoch != current.epoch ||
      (lease.layered && (lease.id != current.id || lease.owner != current.owner))) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Live theme authority is stale"));
  }
  return {};
}

std::expected<StyleLayerHandle, StyleError> StyleEngine::baseLayer(StyleTier tier) const {
  auto allowed = checkBoundary();
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (!layerState_.enabled || (tier != StyleTier::DEFAULTS && tier != StyleTier::THEME)) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Requested style base slot is unavailable"));
  }
  return handle(tier == StyleTier::DEFAULTS ? layerState_.layers[0].id : layerState_.themeBase, layerState_.epoch);
}

std::expected<Invalidation, StyleError> StyleEngine::enableLayers(StyleLayerOptions options) {
  return mutate(Mutation::ENABLE, options, {}, {}, nullptr, {}, {});
}

std::expected<StyleLayerHandle, StyleError> StyleEngine::addLayer(StyleTier tier, std::string_view name,
                                                                  const StyleSheet &sheet) {
  StyleLayerHandle added;
  auto installed = mutate(Mutation::ADD, {}, tier, name, &sheet, {}, {}, &added);
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return added;
}

std::expected<Invalidation, StyleError> StyleEngine::replaceLayer(StyleLayerHandle value, const StyleSheet &sheet) {
  // Keep borrowed input borrowed through budget admission; updateLayers owns its supplied values.
  return mutate(Mutation::UPDATE, {}, {}, {}, &sheet, std::span<const StyleLayerUpdate>{}, {}, &value);
}

std::expected<Invalidation, StyleError> StyleEngine::removeLayer(StyleLayerHandle value) {
  const StyleLayerUpdate update{value, std::nullopt};
  return updateLayers(std::span(&update, 1));
}

std::expected<Invalidation, StyleError> StyleEngine::updateLayers(std::span<const StyleLayerUpdate> updates) {
  return mutate(Mutation::UPDATE, {}, {}, {}, nullptr, updates, {});
}

std::expected<Invalidation, StyleError> StyleEngine::defaults(const StyleSheet &sheet) {
  return mutate(Mutation::DEFAULTS, {}, {}, {}, &sheet, {}, {});
}

std::expected<Invalidation, StyleError> StyleEngine::theme(const StyleSheet &sheet) {
  auto allowed = checkBoundary();
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (layerState_.enabled) {
    return mutate(Mutation::THEME, {}, {}, {}, &sheet, {}, {});
  }
  try {
    return replace(sheet);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style theme allocation failed"));
  }
}

std::expected<Invalidation, StyleError>
StyleEngine::mutate(Mutation mutation, StyleLayerOptions options, StyleTier tier, std::string_view name,
                    const StyleSheet *sheet, std::span<const StyleLayerUpdate> updates,
                    std::span<const StyleRegistration> registrations, StyleLayerHandle *added,
                    const detail::StyleThemeLease *lease, const PssDocument *document,
                    std::optional<StyleLayerOptions> admissionOptions, const PreviewCapture *previewCapture) {
  auto allowed = checkBoundary();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (previewCapture) {
    allowed = validatePreviewCapture(*previewCapture);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }
  }

  if (lease) {
    allowed = validateLease(*lease);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }
  }

  const bool admitting = mutation == Mutation::ADMIT;
  const bool previewing = mutation == Mutation::PREVIEW;
  const bool promoting = mutation == Mutation::PREVIEW_COMMIT;
  const bool cancelling = mutation == Mutation::PREVIEW_CANCEL;

  if (((previewing || promoting || cancelling) && (!added || !previewCapture)) || (previewing && !sheet)) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Theme preview staging is incomplete"));
  }

  const bool updatingPreview = previewing && added && added->id_;
  const bool addingPreview = previewing && !updatingPreview;
  const bool enabling = !layerState_.enabled && (mutation == Mutation::ENABLE || admitting || addingPreview);

  if (addingPreview && layerState_.preview) {
    return std::unexpected(error(TreeError::WOULD_BLOCK, "A theme preview is already active"));
  }

  if ((updatingPreview || promoting || cancelling) && (!added || !ownsPreview(*added))) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Theme preview authority is stale"));
  }

  if (mutation == Mutation::ENABLE && layerState_.enabled) {
    if (options != layerState_.options) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style layer options already established"));
    }

    if (registrations.empty()) {
      return Invalidation::NONE;
    }

    return bindBatch(registrations);
  }

  if (admitting || previewing) {
    options = admissionOptions.value_or(layerState_.enabled ? layerState_.options : StyleLayerOptions{});

    if (layerState_.enabled && options != layerState_.options) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style layer options already established"));
    }
  } else if (layerState_.enabled) {
    options = layerState_.options;
  } else if (!enabling) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style layers are not enabled"));
  }

  if (enabling && (options.maxLayers < 2 || !options.maxRules || !options.maxDeclarations || !options.maxOwnedBytes)) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Invalid style layer budgets"));
  }

  if (admitting &&
      (!document || options.maxLayers > std::numeric_limits<std::size_t>::max() / MAX_PSS_DOCUMENT_BYTES)) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Invalid style document aggregate budget"));
  }

  std::uint64_t admittedId{};

  if (admitting) {
    for (const auto &layer : layerState_.layers) {
      if (layer.tier == tier && layer.name == name) {
        admittedId = layer.id;

        break;
      }
    }
  }

  const bool adding = mutation == Mutation::ADD || (admitting && !admittedId);
  const bool directReplacement = mutation == Mutation::UPDATE && sheet;
  const bool baseRemoval = std::ranges::any_of(updates, [&](const StyleLayerUpdate &update) {
    return !update.sheet && update.handle.id_ == layerState_.themeBase;
  });
  const bool externalTheme = mutation == Mutation::THEME || mutation == Mutation::OWNED_THEME || baseRemoval;
  const auto previewLayer = std::ranges::find(layerState_.layers, layerState_.preview, &Layer::id);

  const bool invalidTier = tier != StyleTier::APPLICATION && tier != StyleTier::USER;
  const bool invalidName = name.empty() || name == "DEFAULTS" || name == "THEME";
  const bool duplicateName =
    mutation == Mutation::ADD && std::ranges::any_of(layerState_.layers, [&](const Layer &layer) {
      return layer.tier == tier && layer.name == name;
    });

  if ((adding || admitting) && (invalidTier || invalidName || duplicateName)) {
    return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Invalid or duplicate style layer name/tier"));
  }

  if (directReplacement) {
    allowed = validateHandle(*added);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    const auto &layer = *std::ranges::find(layerState_.layers, added->id_, &Layer::id);

    if (layer.tier == StyleTier::DEFAULTS || layer.tier == StyleTier::THEME) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Base replacement requires its dedicated operation"));
    }
  }

  for (std::size_t index = 0; index < updates.size(); ++index) {
    allowed = validateHandle(updates[index].handle);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (std::ranges::any_of(updates.first(index), [&](const StyleLayerUpdate &other) {
          return other.handle.id_ == updates[index].handle.id_;
        })) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Duplicate handle in style layer batch"));
    }

    const auto &layer = *std::ranges::find(layerState_.layers, updates[index].handle.id_, &Layer::id);

    if (updates[index].sheet && (layer.tier == StyleTier::DEFAULTS || layer.tier == StyleTier::THEME)) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Base replacement requires its dedicated operation"));
    }
  }

  const auto replacement = [&](const Layer &layer) -> const StyleSheet * {
    if (layer.id == layerState_.preview && (externalTheme || promoting || cancelling)) {
      return nullptr;
    }

    if ((updatingPreview && layer.id == layerState_.preview) || (promoting && layer.id == layerState_.themeBase)) {
      return promoting ? &previewLayer->sheet : sheet;
    }

    if ((admitting && layer.id == admittedId) || (directReplacement && layer.id == added->id_)) {
      return sheet;
    }

    if ((mutation == Mutation::DEFAULTS && layer.tier == StyleTier::DEFAULTS) ||
        ((mutation == Mutation::THEME || mutation == Mutation::OWNED_THEME) && layer.id == layerState_.themeBase)) {
      return sheet;
    }

    for (const auto &update : updates) {
      if (update.handle.id_ == layer.id) {
        return update.sheet ? &*update.sheet : nullptr;
      }
    }

    return &layer.sheet;
  };

  const auto removing = [&](const Layer &layer) {
    return !replacement(layer) && (layer.id == layerState_.preview ||
                                   (layer.tier != StyleTier::DEFAULTS && layer.id != layerState_.themeBase));
  };

  const bool handoff = mutation == Mutation::THEME || baseRemoval || addingPreview;
  bool changed = enabling || adding || handoff || previewing || promoting || cancelling;
  for (const auto &layer : layerState_.layers) {
    const auto next = replacement(layer);

    changed = changed || (next ? *next != layer.sheet : !layer.sheet.rules.empty() || removing(layer));
  }

  std::shared_ptr<const PssDocument> admittedDocument;
  const auto sourceFor = [&](const Layer &layer) -> std::shared_ptr<const PssDocument> {
    if (admitting && layer.id == admittedId) {
      return admittedDocument;
    }

    if ((directReplacement && layer.id == added->id_) ||
        (mutation == Mutation::DEFAULTS && layer.tier == StyleTier::DEFAULTS) ||
        ((mutation == Mutation::THEME || mutation == Mutation::OWNED_THEME || promoting) &&
         layer.id == layerState_.themeBase) ||
        (updatingPreview && layer.id == layerState_.preview) ||
        std::ranges::any_of(updates, [&](const StyleLayerUpdate &update) {
          return update.handle.id_ == layer.id;
        })) {
      return {};
    }

    return layer.document;
  };

  if (!changed && registrations.empty()) {
    // Metadata publication has no style callbacks or invalidation, even for equal low-level replacements.
    LayerMutationGuard guard(stagingLayers_, true);

    try {
      if (admitting) {
        const auto &layer = *std::ranges::find(layerState_.layers, admittedId, &Layer::id);

        if (layer.document && *layer.document == *document) {
          *added = handle(admittedId, layerState_.epoch);

          return Invalidation::NONE;
        }

        admittedDocument = std::make_shared<const PssDocument>(*document);
      }

      for (auto &layer : layerState_.layers) {
        layer.document = sourceFor(layer);
      }

      if (admitting) {
        *added = handle(admittedId, layerState_.epoch);
      }

      return Invalidation::NONE;
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(TreeError::NO_MEMORY, "Style document allocation failed"));
    }
  }

  constexpr auto MAX_ID = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t identities = (enabling ? 2 : 0) + (adding ? 1 : 0) + (addingPreview ? 1 : 0);

  if ((enabling && layerState_.epoch == MAX_ID) || identities > MAX_ID - layerState_.nextId ||
      identities > MAX_ID - layerState_.nextOrder || (handoff && layerState_.owner == MAX_ID)) {
    return std::unexpected(error(TreeError::IDENTITY_EXHAUSTED, "Style layer identity exhausted"));
  }

  // Account borrowed final inputs before any owning copy, including replaced slots only once.
  std::size_t bytes{}, rules{}, declarations{}, layerCount{};
  const auto charge = [](std::size_t &used, std::size_t amount, std::size_t limit) {
    if (amount > limit - used) {
      return false;
    }
    used += amount;

    return true;
  };

  const auto chargeString = [&](std::string_view value) {
    return charge(bytes, value.size(), options.maxOwnedBytes) && charge(bytes, 1, options.maxOwnedBytes);
  };

  const auto account = [&](std::string_view layerName, const StyleSheet *value) -> std::expected<void, StyleError> {
    if (!charge(layerCount, 1, options.maxLayers) || !charge(bytes, sizeof(Layer), options.maxOwnedBytes) ||
        !chargeString(layerName)) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style layer payload budget exhausted"));
    }

    if (!value) {
      return {};
    }
    auto valid = validate(*value);

    if (!valid) {
      return valid;
    }

    if (!charge(rules, value->rules.size(), options.maxRules)) {
      return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Aggregate style rule budget exhausted"));
    }

    for (const auto &rule : value->rules) {
      const auto &d = rule.declarations;
      const auto partCount = [](const StylePaintDeclarations &part) {
        return part.foreground.has_value() + part.background.has_value() + part.bold.has_value() +
               part.italic.has_value() + part.underline.has_value();
      };
      const std::size_t count =
        partCount(d.frameLine) + partCount(d.frameTitle) + partCount(d.frameEnds) + partCount(d.scrollbarTrack) +
        partCount(d.scrollbarThumb) + partCount(d.scrollbarEnds) + partCount(d.scrollbarArrows) +
        partCount(d.scrollbarCorner) + partCount(d.scrollbarThumbStart) + partCount(d.scrollbarThumbBody) +
        partCount(d.scrollbarThumbEnd) + partCount(d.scrollbarThumbSingle) + d.foreground.has_value() +
        d.background.has_value() + d.bold.has_value() + d.italic.has_value() + d.underline.has_value() +
        d.minWidth.has_value() + d.scrollbarTrackPattern.has_value() + d.scrollbarThumbPattern.has_value() +
        d.scrollbarArrowPattern.has_value();
      if (!charge(declarations, count, options.maxDeclarations) ||
          !charge(bytes, sizeof(StyleRule), options.maxOwnedBytes) || !chargeString(rule.selector.type) ||
          !chargeString(rule.selector.id)) {
        return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style declaration/payload budget exhausted"));
      }

      for (const auto &className : rule.selector.classes) {
        if (!charge(bytes, sizeof(std::string), options.maxOwnedBytes) || !chargeString(className)) {
          return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Style class payload budget exhausted"));
        }
      }
    }

    return {};
  };

  if (enabling) {
    allowed = account("DEFAULTS", nullptr);

    if (allowed) {
      allowed = account("THEME", &sheet_);
    }
  } else {
    for (const auto &layer : layerState_.layers) {
      if (!removing(layer)) {
        allowed = account(layer.name, replacement(layer));

        if (!allowed) {
          break;
        }
      }
    }
  }

  if (allowed && (adding || addingPreview)) {
    allowed = account(addingPreview ? "PREVIEW" : name, sheet);
  }

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  LayerMutationGuard guard(stagingLayers_, true);

  try {
    if (admitting) {
      admittedDocument = std::make_shared<const PssDocument>(*document);
    }

    const auto copySheetInto = [](StyleSheet &result, const StyleSheet *source) {
      if (!source) {
        return;
      }

      result.scope = source->scope;
      result.rules.reserve(source->rules.size());

      for (const auto &rule : source->rules) {
        StyleRule copied;

        copied.selector.type.assign(rule.selector.type.data(), rule.selector.type.size());
        copied.selector.id.assign(rule.selector.id.data(), rule.selector.id.size());
        copied.selector.state = rule.selector.state;
        copied.selector.classes.reserve(rule.selector.classes.size());

        for (const auto &className : rule.selector.classes) {
          copied.selector.classes.emplace_back(className.data(), className.size());
        }

        copied.declarations = rule.declarations;
        result.rules.emplace_back();

        auto &destination = result.rules.back();

        destination.selector.type.swap(copied.selector.type);
        destination.selector.id.swap(copied.selector.id);
        destination.selector.classes.swap(copied.selector.classes);
        destination.selector.state = copied.selector.state;
        destination.declarations = copied.declarations;
      }
    };

    LayerState candidate;

    candidate.options = options;
    candidate.enabled = true;
    candidate.epoch = layerState_.epoch + enabling;
    candidate.nextId = layerState_.nextId;
    candidate.nextOrder = layerState_.nextOrder;
    candidate.owner = layerState_.owner + handoff;
    candidate.themeBase = layerState_.themeBase;
    candidate.preview = externalTheme || promoting || cancelling ? 0 : layerState_.preview;
    candidate.layers.reserve(layerCount);

    const auto append = [&](StyleTier nextTier, std::string_view nextName, const StyleSheet *value, std::uint64_t id,
                            std::uint64_t order, std::shared_ptr<const PssDocument> source = {}) {
      // Empty construction plus swaps avoids duplicate owning replacement payload in staging.
      candidate.layers.emplace_back(id, order, nextTier);

      auto &destination = candidate.layers.back();

      destination.name.assign(nextName.data(), nextName.size());
      copySheetInto(destination.sheet, value);
      destination.document = std::move(source);
    };

    if (enabling) {
      append(StyleTier::DEFAULTS, "DEFAULTS", nullptr, candidate.nextId++, candidate.nextOrder++);
      candidate.themeBase = candidate.nextId;
      append(StyleTier::THEME, "THEME", &sheet_, candidate.nextId++, candidate.nextOrder++);
    } else {
      for (const auto &layer : layerState_.layers) {
        if (!removing(layer)) {
          append(layer.tier, layer.name, replacement(layer), layer.id, layer.order, sourceFor(layer));
        }
      }
    }

    if (adding) {
      admittedId = candidate.nextId++;

      append(tier, name, sheet, admittedId, candidate.nextOrder++, admittedDocument);
    }

    if (addingPreview) {
      candidate.preview = candidate.nextId++;
      append(StyleTier::THEME, "PREVIEW", sheet, candidate.preview, candidate.nextOrder++);
    }

    std::optional<std::vector<Binding>> bindings;
    auto merged = mergeBindings(registrations, bindings);

    if (!merged) {
      return std::unexpected(merged.error());
    }

    if (lease) {
      allowed = validateLease(*lease);

      if (!allowed) {
        return std::unexpected(allowed.error());
      }
    }

    // A layered table owns all rules; the inactive legacy sheet is released at this commit.
    auto installed = install({}, std::move(*bindings), &candidate, false, lease, previewCapture,
                             previewing || promoting || cancelling || externalTheme);

    if (installed && (adding || admitting)) {
      *added = handle(admittedId, layerState_.epoch);
    }

    if (installed && previewing) {
      *added = handle(layerState_.preview, layerState_.epoch);
    }

    return installed;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style layer staging allocation failed"));
  }
}

std::expected<void, StyleError> StyleEngine::checkBoundary(bool completing) const {
  auto root = tree_.root();

  if (!root) {
    return std::unexpected(error(root.error(), "Style tree is unavailable"));
  }

  auto access = tree_.access(root->id(), true);

  if (!access) {
    return std::unexpected(error(access.error(), "Style mutation requires an idle UI tree"));
  }

  if (stagingLayers_ && !completing) {
    return std::unexpected(error(TreeError::WOULD_BLOCK, "Style layered staging is active"));
  }
  return {};
}

std::expected<Invalidation, StyleError> StyleEngine::bind(Node<Widget> node, StyleTarget target) {
  auto boundary = checkBoundary();

  if (!boundary) {
    return std::unexpected(boundary.error());
  }

  auto access = tree_.access(node.id(), true);

  if (!access) {
    return std::unexpected(error(access.error(), "Style target is unavailable"));
  }

  auto valid = validateStyleTarget(target);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  LayerMutationGuard guard(stagingLayers_, true);

  try {
    auto bindings = bindings_;
    auto found = std::ranges::find(bindings, node, &Binding::node);

    if (found == bindings.end()) {
      // Prune stale observers before enforcing capacity; reuse cannot resurrect an old generation.
      std::erase_if(bindings, [&](const Binding &binding) {
        return !tree_.access(binding.node.id(), true);
      });

      if (bindings.size() >= maxBindings_) {
        return std::unexpected(error(TreeError::NODE_LIMIT, "Style binding budget exhausted"));
      }

      bindings.push_back({node, std::move(target)});
    } else {
      found->target = std::move(target);
    }

    return install(sheet_, std::move(bindings));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style allocation failed"));
  }
}

std::expected<Invalidation, StyleError> StyleEngine::bindBatch(std::span<const StyleRegistration> registrations) {
  auto boundary = checkBoundary();
  if (!boundary) {
    return std::unexpected(boundary.error());
  }
  LayerMutationGuard guard(stagingLayers_, true);

  try {
    std::optional<std::vector<Binding>> bindings;
    auto merged = mergeBindings(registrations, bindings);
    if (!merged) {
      return std::unexpected(merged.error());
    }
    return install(sheet_, std::move(*bindings));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style batch allocation failed"));
  }
}

std::expected<void, StyleError> StyleEngine::mergeBindings(std::span<const StyleRegistration> registrations,
                                                           std::optional<std::vector<Binding>> &destination) {
  auto boundary = checkBoundary(true);

  if (!boundary) {
    return std::unexpected(boundary.error());
  }

  bool identitiesValidated{};

  try {
    // Validate full identities before using slots as keys. Every admitted target belongs
    // to this tree's current generation; the temporary index owns no widget or metadata.
    std::map<std::size_t, std::optional<std::size_t>> positions;

    for (const auto &registration : registrations) {
      const auto node = registration.node;
      auto access = tree_.access(node.id(), true);

      if (!access) {
        return std::unexpected(error(access.error(), "Style target is unavailable"));
      }

      if (!positions.try_emplace(node.id().slot, std::nullopt).second) {
        return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Duplicate target in style batch"));
      }

      auto valid = validateStyleTarget(registration.target);

      if (!valid) {
        return std::unexpected(valid.error());
      }
    }

    identitiesValidated = true;

    destination.emplace(bindings_);
    auto &bindings = *destination;

    std::erase_if(bindings, [&](const Binding &binding) {
      return !tree_.access(binding.node.id(), true);
    });

    // Index surviving bindings without sorting them: custom defaults keep their original
    // evaluation order. Ordered lookup bounds identity work even for adversarial slot keys.
    for (std::size_t index = 0; index < bindings.size(); ++index) {
      auto found = positions.find(bindings[index].node.id().slot);

      if (found != positions.end()) {
        found->second = index;
      }
    }

    for (const auto &registration : registrations) {
      const auto position = positions.find(registration.node.id().slot)->second;

      if (position) {
        bindings[*position].target = registration.target;

        continue;
      }

      if (bindings.size() >= maxBindings_) {
        return std::unexpected(error(TreeError::NODE_LIMIT, "Style binding budget exhausted"));
      }

      bindings.push_back({registration.node, registration.target});
    }

    return {};
  } catch (const std::bad_alloc &) {
    // Argument errors preceded allocation in the original API. If building the index
    // fails, preserve that priority with an allocation-free scan; only this OOM path
    // pays quadratic duplicate detection. A completed validation needs no replay.
    if (!identitiesValidated) {
      for (std::size_t index = 0; index < registrations.size(); ++index) {
        const auto node = registrations[index].node;
        auto access = tree_.access(node.id(), true);

        if (!access) {
          return std::unexpected(error(access.error(), "Style target is unavailable"));
        }

        const auto preceding = registrations.first(index);

        if (std::ranges::find(preceding, node, &StyleRegistration::node) != preceding.end()) {
          return std::unexpected(error(TreeError::INVALID_ARGUMENT, "Duplicate target in style batch"));
        }

        auto valid = validateStyleTarget(registrations[index].target);

        if (!valid) {
          return std::unexpected(valid.error());
        }
      }
    }

    return std::unexpected(error(TreeError::NO_MEMORY, "Style batch allocation failed"));
  }
}

std::expected<Invalidation, StyleError> StyleEngine::replace(StyleSheet sheet) {
  auto boundary = checkBoundary();

  if (!boundary) {
    return std::unexpected(boundary.error());
  }

  LayerMutationGuard guard(stagingLayers_, true);

  try {
    return install(std::move(sheet), std::vector<Binding>(bindings_), nullptr, true);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style allocation failed"));
  }
}

std::expected<Invalidation, StyleError> StyleEngine::replaceOwnedTheme(const StyleSheet &sheet,
                                                                       detail::StyleThemeLease lease) {
  auto boundary = checkBoundary();
  if (!boundary) {
    return std::unexpected(boundary.error());
  }
  auto authority = validateLease(lease);
  if (!authority) {
    return std::unexpected(authority.error());
  }
  LayerMutationGuard guard(stagingLayers_, true);
  try {
    return install(sheet, std::vector<Binding>(bindings_), nullptr, true, &lease);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Live theme transfer allocation failed"));
  }
}

std::expected<Invalidation, StyleError> StyleEngine::replacePss(std::string_view source) {
  auto boundary = checkBoundary();

  if (!boundary) {
    return std::unexpected(boundary.error());
  }

  LayerMutationGuard guard(stagingLayers_, true);
  auto sheet = parsePss(source);

  if (!sheet) {
    return std::unexpected(sheet.error());
  }

  try {
    return install(*sheet, std::vector<Binding>(bindings_), nullptr, true);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style transfer allocation failed"));
  }
}

std::expected<Invalidation, StyleError> StyleEngine::refresh() {
  auto boundary = checkBoundary();

  if (!boundary) {
    return std::unexpected(boundary.error());
  }

  LayerMutationGuard guard(stagingLayers_, true);

  try {
    return install(sheet_, std::vector<Binding>(bindings_));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(TreeError::NO_MEMORY, "Style allocation failed"));
  }
}

std::uint64_t StyleEngine::revision() const noexcept {
  return revision_;
}

std::expected<Invalidation, StyleError> StyleEngine::install(StyleSheet sheet, std::vector<Binding> &&bindings,
                                                             LayerState *candidate, bool wholeReplace,
                                                             const detail::StyleThemeLease *lease,
                                                             const PreviewCapture *previewCapture, bool notifyTheme) {
  if (previewCapture) {
    auto valid = validatePreviewCapture(*previewCapture);

    if (!valid) {
      return std::unexpected(valid.error());
    }
  }
  if (lease) {
    auto validLease = validateLease(*lease);
    if (!validLease) {
      return std::unexpected(validLease.error());
    }
  }
  if (wholeReplace && layerState_.enabled && layerState_.epoch == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(error(TreeError::IDENTITY_EXHAUSTED, "Style layer epoch exhausted"));
  }
  auto valid = validate(sheet);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(error(TreeError::IDENTITY_EXHAUSTED, "Style revision exhausted"));
  }

  auto root = tree_.root();

  if (!root) {
    return std::unexpected(error(root.error(), "Style tree is unavailable"));
  }

  std::expected<Invalidation, StyleError> result = Invalidation::NONE;
  auto edited = tree_.edit(*root, [&](Widget &) {
    try {
      struct PreparedStyle {
        ComputedStyle style;
        std::shared_ptr<const detail::PreparedGlyphProfiles> glyphs;
      };
      std::vector<PreparedStyle> staged(0);
      Invalidation invalidation = Invalidation::NONE;

      std::erase_if(bindings, [&](const Binding &binding) {
        return !tree_.access(binding.node.id(), true);
      });
      staged.reserve(bindings.size());

      {
        // Treat custom defaults as extension hooks: frame borrow blocks mutation and engine reentry.
        WidgetTree::Borrow frameGuard(tree_, true);

        for (const auto &binding : bindings) {
          auto widget = tree_.access(binding.node.id(), false);
          if (!widget) {
            result = std::unexpected(error(widget.error(), "Style target became unavailable during staging"));
            return;
          }
          auto defaults = (*widget)->styleDefaults();
          auto next =
            wholeReplace
              ? (!sheet.scope || *sheet.scope == binding.node.id() ? resolveStyle(sheet, binding.target, defaults)
                                                                   : defaults)
              : resolve(sheet, candidate ? *candidate : layerState_, binding.target, defaults, binding.node.id());

          if (next.minWidth < 0) {
            result = std::unexpected(error(TreeError::INVALID_ARGUMENT, "Widget default minimum width is negative"));

            return;
          }

          const auto policy = detail::effectiveSizePolicy(**widget, next.minWidth);

          if (!policy) {
            result = std::unexpected(
              error(policy.error(), "Style min-width conflicts with widget SizePolicy bounds (including exact width)"));

            return;
          }

          const auto previous = (*widget)->computedStyle_.value_or(defaults);

          if (previous.minWidth != next.minWidth) {
            invalidation = Invalidation::LAYOUT;
          } else if (previous != next && invalidation == Invalidation::NONE) {
            invalidation = Invalidation::PAINT;
          }

          auto glyphs = detail::GlyphProfileAccess::prepare(**widget, next);

          if (!glyphs) {
            result = std::unexpected(error(glyphs.error(), "Glyph profile preparation failed"));

            return;
          }

          staged.push_back({next, std::move(*glyphs)});
        }
      }

      // No external code runs from here through commit. Revalidate before the sole invalidation.
      auto rootAccess = tree_.access(root->id(), true);
      if (!rootAccess) {
        result = std::unexpected(error(rootAccess.error(), "Style tree changed during staging"));
        return;
      }
      for (const auto &binding : bindings) {
        auto widget = tree_.access(binding.node.id(), true);

        if (!widget) {
          result = std::unexpected(error(widget.error(), "Style target changed during staging"));

          return;
        }
      }

      if (lease) {
        auto validLease = validateLease(*lease);
        if (!validLease) {
          result = std::unexpected(validLease.error());
          return;
        }
      }

      if (previewCapture) {
        auto valid = validatePreviewCapture(*previewCapture);

        if (!valid) {
          result = std::unexpected(valid.error());

          return;
        }
      }

      auto receiver = notifyTheme || wholeReplace ? themeReceiver_.lock() : nullptr;
      if (receiver && (receiver->retired || !tree_.access(receiver->node.id(), true))) {
        receiver.reset();
      }
      if (receiver && receiver->visible && invalidation == Invalidation::NONE) {
        const auto eligible = tree_.inputEligible(receiver->node);
        if (eligible && *eligible) {
          invalidation = Invalidation::PAINT;
        }
      }

      if (invalidation != Invalidation::NONE) {
        auto requested = tree_.invalidate(*root, invalidation);

        if (!requested) {
          result = std::unexpected(error(requested.error(), "Style invalidation rejected"));

          return;
        }
      }

      for (std::size_t index = 0; index < bindings.size(); ++index) {
        auto widget = tree_.access(bindings[index].node.id(), true);

        detail::GlyphProfileAccess::commit(**widget, std::move(staged[index].glyphs));
        (*widget)->computedStyle_ = staged[index].style;
        (*widget)->styleRevision_ = revision_ + 1;
      }

      sheet_.rules.swap(sheet.rules);
      sheet_.scope = sheet.scope;
      bindings_.swap(bindings);
      if (candidate) {
        layerState_.layers.swap(candidate->layers);
        layerState_.options = candidate->options;
        layerState_.epoch = candidate->epoch;
        layerState_.nextId = candidate->nextId;
        layerState_.nextOrder = candidate->nextOrder;
        layerState_.owner = candidate->owner;
        layerState_.themeBase = candidate->themeBase;
        layerState_.preview = candidate->preview;
        layerState_.enabled = candidate->enabled;
      } else if (wholeReplace && layerState_.enabled) {
        layerState_.layers.clear();
        layerState_.enabled = false;
        layerState_.themeBase = 0;
        layerState_.preview = 0;
        ++layerState_.epoch;
      }
      ++revision_;
      if (receiver) {
        receiver->dirty = true;
      }
      result = invalidation;
    } catch (const std::bad_alloc &) {
      result = std::unexpected(error(TreeError::NO_MEMORY, "Style staging allocation failed"));
    } catch (...) {
      result = std::unexpected(error(TreeError::CALLBACK_FAILED, "Widget style defaults threw"));
    }
  });

  if (!edited) {
    return std::unexpected(error(edited.error(), "Style transaction rejected"));
  }

  return result;
}
} // namespace portals
