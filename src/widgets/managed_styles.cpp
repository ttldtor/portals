// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "managed_error_helpers.hpp"
#include <portals/detail/pss_parser.hpp>
#include <portals/managed.hpp>

#include <algorithm>

namespace portals {
namespace detail {
// Mutable runtime documents are admitted in two passes: shape before any linked index, then values.
std::expected<void, PssError> validateStyleDocument(const PssDocument &document) {
  const auto invalid = [](PssLocation location, std::string_view message) {
    return std::unexpected(detail::pssError(PssErrorCode::INVALID_VALUE, location, message));
  };

  if (document.originKind != PssOriginKind::PSS_BYTES && document.originKind != PssOriginKind::SYNTHETIC_TYPED) {
    return invalid({}, "Invalid PSS document origin kind");
  }

  if (document.sourceName.size() > MAX_PSS_SOURCE_NAME_BYTES) {
    return std::unexpected(detail::pssError(PssErrorCode::SOURCE_NAME_LIMIT, {}, "PSS source identity exceeds budget"));
  }

  auto utf8 = detail::validatePssUtf8(document.sourceName);

  if (!utf8) {
    return utf8;
  }

  if (document.sheet.rules.size() > MAX_STYLE_RULES) {
    return std::unexpected(detail::pssError(PssErrorCode::RULE_LIMIT, {}, "Too many PSS document rules"));
  }

  if (document.origins.size() != document.sheet.rules.size()) {
    return invalid({}, "PSS document requires one origin per rule");
  }

  std::size_t ownedBytes{};
  const auto charge = [&](std::size_t bytes) {
    if (bytes > MAX_PSS_DOCUMENT_BYTES - ownedBytes) {
      return false;
    }

    ownedBytes += bytes;

    return true;
  };
  const auto chargeRecords = [&](std::size_t count, std::size_t size) {
    return count <= (MAX_PSS_DOCUMENT_BYTES - ownedBytes) / size && charge(count * size);
  };

  if (!charge(document.sourceName.size()) || !chargeRecords(document.sheet.rules.size(), sizeof(StyleRule)) ||
      !chargeRecords(document.origins.size(), sizeof(PssRuleOrigin)) ||
      !chargeRecords(document.declarations.size(), sizeof(PssDeclarationOrigin))) {
    return std::unexpected(detail::pssError(PssErrorCode::DOCUMENT_LIMIT, {}, "PSS document exceeds payload budget"));
  }

  const auto byteLocation = [](PssLocation location) {
    return location.offset <= MAX_PSS_BYTES && location.line > 0 && location.column > 0 &&
           location.line <= location.offset + 1 && location.column <= location.offset + 1;
  };
  const auto ordered = [](PssLocation previous, PssLocation next) {
    return previous.offset <= next.offset && previous.line <= next.line &&
           (previous.line != next.line || previous.column <= next.column);
  };
  std::size_t consumed{};
  PssLocation previous;

  for (std::size_t ruleIndex = 0; ruleIndex < document.origins.size(); ++ruleIndex) {
    const auto &origin = document.origins[ruleIndex];
    const auto &rule = document.sheet.rules[ruleIndex];
    const bool synthetic = document.originKind == PssOriginKind::SYNTHETIC_TYPED;

    if ((synthetic && origin.selector != PssLocation{ruleIndex, ruleIndex + 1, 1}) ||
        (!synthetic && (!byteLocation(origin.selector) || (ruleIndex && !ordered(previous, origin.selector))))) {
      return invalid(origin.selector, "Invalid PSS selector coordinates");
    }

    if (origin.declarationBegin != consumed || origin.declarationCount > document.declarations.size() - consumed) {
      return invalid(origin.selector, "Invalid PSS contiguous occurrence range");
    }

    const auto end = consumed + origin.declarationCount;

    for (const auto index : origin.effectiveDeclaration) {
      if (index != NO_PSS_DECLARATION && (index < consumed || index >= end)) {
        return invalid(origin.selector, "PSS effective declaration escapes its rule");
      }
    }

    if (rule.selector.state < StyleState::ANY || rule.selector.state > StyleState::CARET) {
      return invalid(origin.selector, "Invalid PSS selector state");
    }

    if (rule.declarations.minWidth && (*rule.declarations.minWidth < 0 || rule.selector.state != StyleState::ANY)) {
      return std::unexpected(
        detail::pssError(PssErrorCode::INVALID_WIDTH, origin.selector, "Invalid PSS document minimum width"));
    }

    if (!charge(rule.selector.type.size()) || !charge(rule.selector.id.size()) ||
        !chargeRecords(rule.selector.classes.size(), sizeof(std::string))) {
      return std::unexpected(
        detail::pssError(PssErrorCode::DOCUMENT_LIMIT, origin.selector, "PSS selectors exceed document budget"));
    }

    for (const auto &name : rule.selector.classes) {
      if (!charge(name.size())) {
        return std::unexpected(
          detail::pssError(PssErrorCode::DOCUMENT_LIMIT, origin.selector, "PSS classes exceed document budget"));
      }
    }

    previous = origin.selector;

    for (std::size_t index = consumed; index < end; ++index) {
      const auto &declaration = document.declarations[index];

      if (declaration.ruleIndex != ruleIndex || declaration.property < PssProperty::FOREGROUND ||
          static_cast<std::size_t>(declaration.property) >= PSS_PROPERTY_COUNT) {
        return invalid(declaration.location, "Invalid PSS declaration rule or property index");
      }

      const auto property = static_cast<std::size_t>(declaration.property);

      if ((synthetic && declaration.location != PssLocation{property, ruleIndex + 1, property + 1}) ||
          (!synthetic && (!byteLocation(declaration.location) || !ordered(previous, declaration.location)))) {
        return invalid(declaration.location, "Invalid PSS declaration coordinates");
      }

      previous = declaration.location;
    }

    consumed = end;
  }

  if (consumed != document.declarations.size()) {
    return invalid({}, "PSS document contains unclaimed declarations");
  }

  // Every enum, containment range and effective link is now safe to inspect.
  for (std::size_t ruleIndex = 0; ruleIndex < document.origins.size(); ++ruleIndex) {
    const auto &origin = document.origins[ruleIndex];
    const auto &rule = document.sheet.rules[ruleIndex];
    std::array<std::size_t, PSS_PROPERTY_COUNT> last;
    StyleDeclarations effective;

    last.fill(NO_PSS_DECLARATION);

    for (std::size_t index = origin.declarationBegin; index < origin.declarationBegin + origin.declarationCount;
         ++index) {
      const auto &declaration = document.declarations[index];
      const auto property = static_cast<std::size_t>(declaration.property);
      const auto &value = declaration.value;
      const bool color = declaration.property == PssProperty::FOREGROUND ||
                         declaration.property == PssProperty::BACKGROUND ||
                         declaration.property == PssProperty::FRAME_LINE_FOREGROUND ||
                         declaration.property == PssProperty::FRAME_LINE_BACKGROUND ||
                         declaration.property == PssProperty::FRAME_TITLE_FOREGROUND ||
                         declaration.property == PssProperty::FRAME_TITLE_BACKGROUND ||
                         declaration.property == PssProperty::FRAME_ENDS_FOREGROUND ||
                         declaration.property == PssProperty::FRAME_ENDS_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_TRACK_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_TRACK_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_START_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_START_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_BODY_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_BODY_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_END_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_END_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_SINGLE_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_SINGLE_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_THUMB_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_ENDS_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_ENDS_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_ARROWS_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_ARROWS_BACKGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_CORNER_FOREGROUND ||
                         declaration.property == PssProperty::SCROLLBAR_CORNER_BACKGROUND;
      const bool width = declaration.property == PssProperty::MIN_WIDTH;
      const bool pattern = declaration.property == PssProperty::SCROLLBAR_TRACK_PATTERN ||
                           declaration.property == PssProperty::SCROLLBAR_THUMB_PATTERN ||
                           declaration.property == PssProperty::SCROLLBAR_ARROW_PATTERN ||
                           declaration.property == PssProperty::GLYPH_PROFILE ||
                           declaration.property == PssProperty::GLYPH_FALLBACK;

      if ((!color && value.color != RgbColor{}) || ((color || width || pattern) && value.boolean) ||
          (!width && value.width != 0) || (!pattern && value.pattern != 0) ||
          (pattern &&
           (value.pattern < 0 || value.pattern > (declaration.property == PssProperty::GLYPH_PROFILE
                                                    ? static_cast<std::int32_t>(GlyphProfile::NF_BACKSLASH)
                                                  : declaration.property == PssProperty::GLYPH_FALLBACK
                                                    ? static_cast<std::int32_t>(GlyphFallback::ASCII)
                                                  : declaration.property == PssProperty::SCROLLBAR_TRACK_PATTERN
                                                    ? static_cast<std::int32_t>(ScrollbarPattern::FRACTIONAL_BLOCK)
                                                  : declaration.property == PssProperty::SCROLLBAR_THUMB_PATTERN
                                                    ? static_cast<std::int32_t>(ScrollbarThumbPattern::FRACTIONAL_BLOCK)
                                                    : static_cast<std::int32_t>(ScrollbarArrowPattern::ASCII))))) {
        return invalid(declaration.location, "Noncanonical PSS scalar fields");
      }

      if (width && (value.width < 0 || rule.selector.state != StyleState::ANY)) {
        return std::unexpected(
          detail::pssError(PssErrorCode::INVALID_WIDTH, declaration.location, "Invalid PSS declaration minimum width"));
      }

      last[property] = index;
      detail::assignPssDeclaration(declaration.property, value, effective);
    }

    if (last != origin.effectiveDeclaration || effective != rule.declarations) {
      return invalid(origin.selector, "PSS last-wins provenance disagrees with final declarations");
    }
  }

  return {};
}

} // namespace detail

namespace {
struct StyleMutationGuard {
  bool &active;

  explicit StyleMutationGuard(bool &value) : active(value) {
    active = true;
  }

  ~StyleMutationGuard() {
    active = false;
  }
};

void retainStyleSource(ManagedError &error, const PssDocument &document) {
  // The style boundary emits TreeError or StyleError; never allocate fallback identity after NO_MEMORY.
  if (bridge(error) == TreeError::NO_MEMORY || document.sourceName.size() > MAX_PSS_SOURCE_NAME_BYTES) {
    return;
  }

  try {
    error.styleSource = std::make_shared<const std::string>(document.sourceName);
  } catch (const std::bad_alloc &) {
    // Retain the original cause even when the optional owning label cannot be allocated.
  }
}

} // namespace

std::expected<void, ManagedError> ManagedScene::style(Node<Widget> node, StyleTarget target) {
  auto allowed = boundary(ManagedPhase::STYLE, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto prepared = std::make_shared<StyleTarget>(target);
    auto dirty = app_.tree().invalidate(node, Invalidation::PAINT);

    if (!dirty) {
      return std::unexpected(error(ManagedPhase::STYLE, node, dirty.error()));
    }

    // Publish only the prepared owner: checked string/container moves can allocate on MSVC.
    entry->target = std::move(prepared);
    entry->stylePending = true;
    stylesDirty_ = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> ManagedScene::id(Node<Widget> node, std::string_view value) {
  auto allowed = boundary(ManagedPhase::STYLE, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto target = entry->target ? *entry->target : StyleTarget{};

    target.id = value;

    // Unlike style(), an explicit empty id must clear the previous identity.
    auto prepared = std::make_shared<StyleTarget>(target);
    auto dirty = app_.tree().invalidate(node, Invalidation::PAINT);

    if (!dirty) {
      return std::unexpected(error(ManagedPhase::STYLE, node, dirty.error()));
    }

    entry->target = std::move(prepared);
    entry->stylePending = true;
    stylesDirty_ = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> ManagedScene::classes(Node<Widget> node, std::vector<std::string> values) {
  auto allowed = boundary(ManagedPhase::STYLE, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto target = entry->target ? *entry->target : StyleTarget{};

    target.classes.swap(values);

    auto prepared = std::make_shared<StyleTarget>(target);
    auto dirty = app_.tree().invalidate(node, Invalidation::PAINT);

    if (!dirty) {
      return std::unexpected(error(ManagedPhase::STYLE, node, dirty.error()));
    }

    entry->target = std::move(prepared);
    entry->stylePending = true;
    stylesDirty_ = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError>
ManagedScene::pendingStyles(std::optional<std::vector<StyleRegistration>> &destination) {
  try {
    if (!stylesDirty_) {
      return {};
    }
    destination.emplace(0);
    auto &registrations = *destination;
    if (entries_.size() >= registrations.max_size()) {
      return std::unexpected(error(ManagedPhase::STYLE, root_, TreeError::NODE_LIMIT));
    }
    registrations.reserve(entries_.size() + 1);
    // Infrastructure participates in the same atomic cascade without consuming a user entry.
    registrations.push_back({root_, StyleTarget{.type = "Scene"}});
    for (const auto &entry : entries_) {
      auto live = app_.tree().read(entry->node, [](const Widget &) {
      });
      if (!live) {
        if (retired(live.error())) {
          continue;
        }
        return std::unexpected(error(ManagedPhase::STYLE, entry->node, live.error()));
      }
      if (entry->target && entry->stylePending) {
        registrations.push_back({entry->node, *entry->target});
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, root_, TreeError::NO_MEMORY));
  }
}

void ManagedScene::publishStyleMetadata() noexcept {
  for (const auto &entry : entries_) {
    entry->stylePending = false;
  }
  stylesDirty_ = false;
}

std::expected<void, ManagedError> ManagedScene::flushStyles() {
  if (!stylesDirty_) {
    return {};
  }
  std::optional<std::vector<StyleRegistration>> registrations;
  auto staged = pendingStyles(registrations);
  if (!staged) {
    return std::unexpected(staged.error());
  }
  auto installed = styles_.bindBatch(*registrations);
  if (!installed) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}});
  }
  publishStyleMetadata();
  (void)invalidateNotificationStyles();
  return {};
}

std::expected<Invalidation, ManagedError>
ManagedScene::mutateStyles(StyleEngine::Mutation mutation, StyleLayerOptions options, StyleTier tier,
                           std::string_view name, const StyleSheet *sheet, std::span<const StyleLayerUpdate> updates,
                           StyleLayerHandle *added, const detail::StyleThemeLease *lease, const PssDocument *document,
                           std::optional<StyleLayerOptions> admissionOptions,
                           const StyleEngine::PreviewCapture *previewCapture) {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  StyleMutationGuard guard(styling_);

  if (document) {
    auto valid = detail::validateStyleDocument(*document);

    if (!valid) {
      const auto &pss = valid.error();
      ManagedError failure{ManagedPhase::STYLE,
                           root_.id(),
                           StyleError{pss.cause, pss.location.line, pss.location.column, pss.message},
                           {},
                           {}};

      failure.pss = pss;
      retainStyleSource(failure, *document);

      return std::unexpected(failure);
    }
  }

  // Pending registrations and layer declarations share the engine's sole publication boundary.
  std::optional<std::vector<StyleRegistration>> registrations;
  auto staged = pendingStyles(registrations);

  if (!staged) {
    return std::unexpected(staged.error());
  }

  // A scoped sheet can style an otherwise unregistered layout/custom widget. Stage its
  // metadata together with the candidate layer, and publish ownership only after commit.
  std::shared_ptr<Entry> scopedEntry;
  std::shared_ptr<StyleTarget> scopedTarget;

  if (sheet && sheet->scope) {
    const auto found = std::ranges::find_if(entries_, [&](const auto &entry) {
      return entry->node.id() == *sheet->scope && !entry->removed;
    });

    if (found == entries_.end()) {
      const auto cause = sheet->scope->session != root_.id().session ? TreeError::WRONG_SESSION : TreeError::STALE_NODE;

      return std::unexpected(ManagedError{ManagedPhase::STYLE, *sheet->scope, cause, {}, {}});
    }

    auto live = app_.tree().withScope((*found)->node, [](const UiScope &) {
    });

    if (!live) {
      return std::unexpected(error(ManagedPhase::STYLE, (*found)->node, live.error()));
    }

    if (!(*found)->target) {
      try {
        scopedEntry = *found;
        scopedTarget = std::make_shared<StyleTarget>();

        if (!registrations) {
          registrations.emplace(0);
        }

        registrations->push_back({scopedEntry->node, *scopedTarget});
      } catch (const std::bad_alloc &) {
        return std::unexpected(error(ManagedPhase::STYLE, scopedEntry->node, TreeError::NO_MEMORY));
      }
    }
  }

  auto installed = styles_.mutate(mutation, options, tier, name, sheet, updates,
                                  registrations ? std::span<const StyleRegistration>(*registrations)
                                                : std::span<const StyleRegistration>{},
                                  added, lease, document, admissionOptions, previewCapture);

  if (!installed) {
    ManagedError failure{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}};

    if (document) {
      if (installed.error().cause == TreeError::NO_MEMORY) {
        auto pss = detail::pssError(PssErrorCode::ALLOCATION, {}, "Style document allocation failed");

        pss.cause = TreeError::NO_MEMORY;
        failure.pss = pss;
      } else {
        retainStyleSource(failure, *document);
      }
    }

    return std::unexpected(failure);
  }

  if (scopedEntry) {
    scopedEntry->target = std::move(scopedTarget);
  }

  publishStyleMetadata();
  const auto notifications = invalidateNotificationStyles();

  if (*installed == Invalidation::LAYOUT || notifications == Invalidation::NONE) {
    return *installed;
  }
  return notifications;
}

std::expected<StyleLayerHandle, ManagedError> ManagedScene::styleDocument(StyleTier tier, std::string_view name,
                                                                          const PssDocument &document,
                                                                          std::optional<StyleLayerOptions> options) {
  StyleLayerHandle admitted;
  auto installed = mutateStyles(StyleEngine::Mutation::ADMIT, {}, tier, name, &document.sheet, {}, &admitted, nullptr,
                                &document, options);

  if (!installed) {
    return std::unexpected(installed.error());
  }

  return admitted;
}

std::expected<std::vector<StyleLayerSource>, ManagedError> ManagedScene::styleSources() const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  StyleMutationGuard guard(styling_);

  try {
    std::vector<StyleLayerSource> sources(0);

    sources.reserve(styles_.layerState_.layers.size());

    for (const auto &layer : styles_.layerState_.layers) {
      if (layer.id == styles_.layerState_.preview) {
        continue;
      }
      sources.emplace_back();

      auto &source = sources.back();

      source.handle = styles_.handle(layer.id, styles_.layerState_.epoch);
      source.tier = layer.tier;
      source.name.assign(layer.name);
      source.document = layer.document;
    }

    // MSVC Debug vector moves can allocate inside noexcept: copy directly into the checked result.
    return std::expected<std::vector<StyleLayerSource>, ManagedError>(std::in_place, sources);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, root_, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> ManagedScene::validateStyleLayer(StyleLayerHandle handle) const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto valid = styles_.validateHandle(handle);

  if (!valid) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), valid.error(), {}, {}});
  }

  return {};
}

std::expected<void, ManagedError> ManagedScene::enableStyleLayers(StyleLayerOptions options) {
  auto installed = mutateStyles(StyleEngine::Mutation::ENABLE, options, {}, {}, nullptr, {});
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return {};
}

std::expected<StyleLayerHandle, ManagedError> ManagedScene::addLayer(StyleTier tier, std::string_view name,
                                                                     const StyleSheet &sheet) {
  StyleLayerHandle added;
  auto installed = mutateStyles(StyleEngine::Mutation::ADD, {}, tier, name, &sheet, {}, &added);
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return added;
}

std::expected<Invalidation, ManagedError> ManagedScene::replaceLayer(StyleLayerHandle value, const StyleSheet &sheet) {
  return mutateStyles(StyleEngine::Mutation::UPDATE, {}, {}, {}, &sheet, {}, &value);
}

std::expected<Invalidation, ManagedError> ManagedScene::removeLayer(StyleLayerHandle value) {
  const StyleLayerUpdate update{value, std::nullopt};
  return updateLayers(std::span(&update, 1));
}

std::expected<Invalidation, ManagedError> ManagedScene::updateLayers(std::span<const StyleLayerUpdate> updates) {
  return mutateStyles(StyleEngine::Mutation::UPDATE, {}, {}, {}, nullptr, updates);
}

std::expected<StyleLayerHandle, ManagedError> ManagedScene::baseLayer(StyleTier tier) const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  auto found = styles_.baseLayer(tier);
  if (!found) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), found.error(), {}, {}});
  }
  return *found;
}

std::expected<Invalidation, ManagedError> ManagedScene::styleDefaults(const StyleSheet &sheet) {
  return mutateStyles(StyleEngine::Mutation::DEFAULTS, {}, {}, {}, &sheet, {});
}

detail::StyleThemeLease ManagedScene::captureThemeLease() const noexcept {
  return styles_.themeLease();
}

std::expected<void, ManagedError> ManagedScene::checkThemeLease(detail::StyleThemeLease lease) const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);
  if (!allowed) {
    return allowed;
  }
  auto valid = styles_.validateLease(lease);
  if (!valid) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), valid.error(), {}, {}});
  }
  return {};
}

std::expected<detail::StyleThemeLease, ManagedError> ManagedScene::attachTheme(const StyleSheet &sheet,
                                                                               detail::StyleThemeLease lease) {
  auto allowed = checkThemeLease(lease);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (lease.layered) {
    auto installed = mutateStyles(StyleEngine::Mutation::THEME, {}, {}, {}, &sheet, {}, nullptr, &lease);
    if (!installed) {
      return std::unexpected(installed.error());
    }
  } else {
    auto installed = applyLegacyTheme(sheet, lease);
    if (!installed) {
      return std::unexpected(installed.error());
    }
  }
  return captureThemeLease();
}

std::expected<void, ManagedError> ManagedScene::applyLegacyTheme(const StyleSheet &sheet,
                                                                 detail::StyleThemeLease lease) {
  auto allowed = checkThemeLease(lease);
  if (!allowed) {
    return allowed;
  }

  struct Guard {
    bool &active;

    explicit Guard(bool &value) : active(value) {
      active = true;
    }

    ~Guard() {
      active = false;
    }
  } guard(styling_);

  auto flushed = flushStyles();
  if (!flushed) {
    return flushed;
  }
  auto installed = styles_.replaceOwnedTheme(sheet, lease);
  if (!installed) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}});
  }
  (void)invalidateNotificationStyles();
  return {};
}

std::expected<void, ManagedError> ManagedScene::applyTheme(const StyleSheet &sheet, detail::StyleThemeLease lease) {
  auto allowed = checkThemeLease(lease);
  if (!allowed) {
    return allowed;
  }
  if (!lease.layered) {
    return applyLegacyTheme(sheet, lease);
  }
  auto installed = mutateStyles(StyleEngine::Mutation::OWNED_THEME, {}, {}, {}, &sheet, {}, nullptr, &lease);
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::theme(StyleSheet sheet) {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return allowed;
  }

  if (styles_.layerState_.enabled) {
    auto installed = mutateStyles(StyleEngine::Mutation::THEME, {}, {}, {}, &sheet, {});
    if (!installed) {
      return std::unexpected(installed.error());
    }
    return {};
  }

  StyleMutationGuard guard(styling_);

  auto flushed = flushStyles();

  if (!flushed) {
    return flushed;
  }

  auto installed = styles_.replace(std::move(sheet));

  if (!installed) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}});
  }
  (void)invalidateNotificationStyles();
  return {};
}

} // namespace portals
