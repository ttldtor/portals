// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_help_state.hpp"
#include "ui_scene_resource_internal.hpp"

#include <algorithm>
#include <cmath>

namespace portals::ui::detail::resource {
std::expected<void, ManagedError> registerSchemas(ControlContext &controls, const SceneResourceOptions &options) {
  auto catalog = detail::ControlContextAccess::installThemeCatalogSchema(controls);
  if (!catalog) {
    return std::unexpected(detail::controlContextError(catalog.error()));
  }

  for (const auto &entry : detail::ResourceAccess::entries(options.registry)) {
    if (entry.factoryKind == detail::ControlFactoryKind::EXISTING_BUILTIN) {
      continue;
    }

    std::vector<std::string_view> paths(0);

    if (entry.text) {
      paths.push_back("text");
    }

    if (entry.sized) {
      paths.insert(paths.end(), {"width", "height", "flex"});
    }

    if (entry.gap) {
      paths.push_back("gap");
    }

    if (entry.checked) {
      paths.insert(paths.end(), {"state", "cycle"});
    }

    if (!entry.virtualView.empty()) {
      // An explicit old narrow allowlist remains an exact compatibility path.
      if (controls.hasRegisteredType(entry.name) && controls.matchesRegisteredType(entry.name, paths)) {
        continue;
      }

      for (const auto &property : controlPropertySchema(entry.virtualView)) {
        if (!property.path.starts_with("frame.") && std::ranges::find(paths, property.path) == paths.end()) {
          paths.push_back(property.path);
        }
      }
    }

    if (entry.sized && !(controls.hasRegisteredType(entry.name) && controls.matchesRegisteredType(entry.name, paths))) {
      // An existing exact allowlist stays narrow; only the default sized registration gains frame leaves.
      for (const auto &property : controlPropertySchema("Button")) {
        if (property.path.starts_with("frame.") && std::ranges::find(paths, property.path) == paths.end()) {
          paths.push_back(property.path);
        }
      }
    }

    if (entry.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING ||
        entry.factoryKind == detail::ControlFactoryKind::NOTIFICATION_BINDING ||
        entry.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ||
        entry.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING) {
      paths.clear();
      const auto schema = controlPropertySchema(
        entry.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING         ? "MessageBox"
        : entry.factoryKind == detail::ControlFactoryKind::NOTIFICATION_BINDING  ? "Notifications"
        : entry.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING ? "ThemeSelectorCatalog"
                                                                                 : "ThemeSelector");

      for (const auto &property : schema.first(entry.semanticPropertyCount.value_or(schema.size()))) {
        paths.push_back(property.path);
      }
    }

    if (controls.hasRegisteredType(entry.name)) {
      if (entry.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING ||
          entry.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ||
          entry.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING) {
        const auto oldCount = entry.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING          ? 24u
                              : entry.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ? 22u
                                                                                                        : 38u;

        if (controls.matchesRegisteredType(entry.name, std::span(paths).first(oldCount))) {
          paths.resize(oldCount);
        }
      }

      if (!controls.matchesRegisteredType(entry.name, paths)) {
        return std::unexpected(detail::controlContextError({ControlContextErrorCode::DUPLICATE_TYPE, {}}));
      }
    } else {
      controls.registerType(entry.name, paths);
    }
    if (entry.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ||
        entry.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING) {
      auto marked = detail::ControlContextAccess::themeFamilySchema(controls, entry.name, entry.factoryKind);
      if (!marked) {
        return std::unexpected(detail::controlContextError(marked.error()));
      }
    }
  }

  return {};
}

namespace {
ManagedError primitiveValueFailure(const ResolvedControl &resolved, std::string_view property) {
  auto diagnostic = std::make_shared<ControlContextDiagnostic>();
  diagnostic->path = resolved.type + '.' + std::string(property);
  diagnostic->origin = resolved.settings.origin;

  for (const auto &origin : resolved.settings.origins) {
    if (origin.path == property) {
      diagnostic->origin = origin.source;

      break;
    }
  }

  return detail::controlContextError({ControlContextErrorCode::INVALID_VALUE, std::move(diagnostic)});
}

} // namespace

std::expected<void, ManagedError> prepareNodes(ResourceDocument &document, const ControlContext &controls,
                                               detail::MountContext &context, ManagedScene &owner,
                                               const SceneResourceRegistry &registry) {
  std::size_t leaves{};
  const auto limit = controls.limits().maxMaterializedLeaves;
  std::vector<std::vector<std::size_t>> children(document.nodes.size(), std::vector<std::size_t>(0));

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    if (document.nodes[index].parent) {
      children[*document.nodes[index].parent].push_back(index);
    }
  }

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    auto &node = document.nodes[index];
    if (node.factory.kind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ||
        node.factory.kind == detail::ControlFactoryKind::THEME_CATALOG_BINDING) {
      node.themeSource.emplace();
      node.themeSource->instance = node.arguments.settings;
      node.themeSource->supplied = detail::controlSettingsPresenceSet(node.arguments.settings);
    }
    auto resolved = controls.resolve(node.name, node.arguments.settings);

    if (!resolved) {
      return std::unexpected(detail::controlContextError(resolved.error()));
    }

    if (node.factory.kind == detail::ControlFactoryKind::MESSAGE_BINDING) {
      const auto &entry = detail::ResourceAccess::entry(registry, node.factory.index);
      const auto choice = resolved->settings.message.defaultChoice.value;

      // Definitions may name a not-yet-registered alias while parsing their body. Only
      // the resolved concrete schema can validate its effective enum domain completely.
      // Captured C++ MessageOptions are not source settings and remain factory-owned.
      if (choice && entry.messageChoiceCount && static_cast<std::size_t>(*choice) >= entry.messageChoiceCount) {
        constexpr std::string_view PATH = "message.defaultChoice.value";
        const auto origin = std::ranges::find(resolved->settings.origins, PATH, &ControlPropertyOrigin::path);
        const auto offset =
          origin != resolved->settings.origins.end() && origin->source ? origin->source->byteOffset : node.offset;

        return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, offset, node.name, PATH));
      }

      if (node.directChildCount > 1) {
        return std::unexpected(document.propertyFailure(Code::CHILDREN_FORBIDDEN, node.offset, node.name, "children"));
      }

      if (node.directChildCount && resolved->settings.body) {
        return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "body"));
      }
    }

    if (node.factory.kind == detail::ControlFactoryKind::DATA_STATE_FALLBACK && node.directChildCount != 1) {
      return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "children"));
    }

    if (node.factory.kind == detail::ControlFactoryKind::DIALOG_FALLBACK) {
      const auto &concrete = detail::ResourceAccess::entry(registry, node.factory.index);

      if (concrete.declaredHost) {
        if (children[index].empty()) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "children"));
        }

        const auto &body = document.nodes[children[index].front()];
        const auto &bodyEntry = detail::ResourceAccess::entry(registry, body.factory.index);

        if (bodyEntry.declaredDialog) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, body.offset, node.name, "children"));
        }

        for (std::size_t layer = 1; layer < children[index].size(); ++layer) {
          const auto &source = document.nodes[children[index][layer]];
          const auto &entry = detail::ResourceAccess::entry(registry, source.factory.index);

          if (!entry.declaredDialog && !entry.unknownDialog) {
            return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, source.offset, node.name, "children"));
          }
        }

        const auto maximum = detail::controlOverlayHostOptions(resolved->settings, {}).maxLayers;

        if (children[index].size() - 1 > maximum) {
          auto failure = document.propertyFailure(Code::NODE_LIMIT, node.offset, node.name, "maxLayers");

          failure.cause = TreeError::NODE_LIMIT;

          return std::unexpected(std::move(failure));
        }
      } else {
        if (children[index].size() != 1) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "children"));
        }

        if (!resolved->settings.title) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "title"));
        }

        if (!node.parent) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "parent"));
        }

        const auto &sourceParent = document.nodes[*node.parent];
        const auto &parentEntry = detail::ResourceAccess::entry(registry, sourceParent.factory.index);

        if (!parentEntry.declaredHost && !parentEntry.unknownDialog) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "parent"));
        }

        if (!Dialog::validate(detail::controlDialogOptions(resolved->settings, {}))) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "dialog"));
        }
      }
    }

    if (node.factory.kind == detail::ControlFactoryKind::PRIMITIVE_FALLBACK) {
      const auto &settings = resolved->settings;

      if (resolved->base == "Border") {
        if (node.directChildCount != 1) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "children"));
        }

        if (!Border::validate(detail::controlBorderOptions(settings, {}))) {
          return std::unexpected(primitiveValueFailure(*resolved, "maxPaintCells"));
        }
      } else if (resolved->base == "Spinner") {
        const auto valid = Spinner::validate(owner.widthPolicy(), detail::controlSpinnerOptions(settings, {}));
        if (!valid && valid.error() == TreeError::NO_MEMORY) {
          return std::unexpected(detail::error(owner.root(), TreeError::NO_MEMORY));
        }
        if (!valid) {
          return std::unexpected(primitiveValueFailure(*resolved, "frames"));
        }
      } else {
        const auto options = detail::controlGaugeOptions(settings, {});
        const auto value = settings.value.value_or(0);

        if (!Gauge::validate(value, options)) {
          std::string_view property = "value";

          if (options.minimum >= options.maximum || !std::isfinite(options.maximum - options.minimum)) {
            property = settings.maximum ? "maximum" : "minimum";
          } else if (!settings.value) {
            property = value < options.minimum ? "minimum" : "maximum";
          }

          return std::unexpected(primitiveValueFailure(*resolved, property));
        }
      }
    }

    if (node.factory.kind == detail::ControlFactoryKind::SCROLL_FALLBACK) {
      const auto &settings = resolved->settings;

      if (resolved->base == "LogView") {
        if (node.directChildCount || !validateLogLimits(detail::controlLogViewConfig(settings, {}).limits)) {
          return std::unexpected(primitiveValueFailure(*resolved, node.directChildCount ? "children" : "maxLines"));
        }
      } else if (resolved->base == "ScrollArea") {
        if (node.directChildCount != 1) {
          return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "children"));
        }

        if (!detail::controlScrollAreaOptions(settings, {})) {
          std::string_view property = "placement";
          const bool frameEnabled = settings.frame.enabled.value_or(false);

          if (!frameEnabled && (settings.frame.glyphs || settings.frame.maxPaintCells)) {
            property = settings.frame.glyphs ? "frame.glyphs" : "frame.maxPaintCells";
          } else if (settings.frame.enabled == false) {
            property = "frame.enabled";
          }

          return std::unexpected(primitiveValueFailure(*resolved, property));
        }
      } else if (!detail::controlScrollBarOptions(settings, {})) {
        return std::unexpected(primitiveValueFailure(*resolved, "wheelStep"));
      }
    }

    if (node.factory.kind == detail::ControlFactoryKind::KEY_HELP_FALLBACK) {
      std::string_view property;
      auto validated = resolved->base == "HelpView"
                         ? detail::KeyHelpAccess::validate(
                             owner, detail::controlHelpViewOptions(resolved->settings, {}), {}, &property)
                         : detail::KeyHelpAccess::validate(
                             owner, detail::controlKeyHintsOptions(resolved->settings, {}), {}, &property);
      if (!validated) {
        auto failure = validated.error();
        failure.sceneResource = primitiveValueFailure(*resolved, property).sceneResource;
        return std::unexpected(std::move(failure));
      }
    }

    if (context.controlLeaves > limit || leaves > limit - context.controlLeaves ||
        resolved->materializedLeaves > limit - context.controlLeaves - leaves) {
      return std::unexpected(document.failure(Code::MATERIALIZATION_LIMIT, node.offset));
    }

    if (node.themeSource) {
      node.themeSource->materialized = detail::controlSettingsPresenceSet(resolved->settings);
    }
    leaves += resolved->materializedLeaves;
    node.lineage = std::move(resolved->lineage);
    node.arguments.settings = std::move(resolved->settings);
    node.arguments.width = node.arguments.settings.width;
    node.arguments.height = node.arguments.settings.height;
    node.arguments.flex = node.arguments.settings.flex;
    node.arguments.gap = node.arguments.settings.gap.value_or(0);
    node.arguments.checked = node.arguments.settings.state == CheckState::CHECKED;
  }

  // Reserve the complete resource before any resolver/factory effect. The enclosing
  // transaction owns rollback; sibling resources share this same aggregate counter.
  context.controlLeaves += leaves;

  return {};
}

std::expected<void, ManagedError> prepareHelpReferences(ResourceDocument &document,
                                                        const SceneResourceRegistry &registry) {
  std::optional<std::size_t> singleHelp;
  bool ambiguous{};
  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    const auto &node = document.nodes[index];
    if (detail::ResourceAccess::entry(registry, node.factory.index).declaredHelp) {
      if (singleHelp) {
        ambiguous = true;
      } else {
        singleHelp = index;
      }
    }
  }
  for (auto &node : document.nodes) {
    const auto &entry = detail::ResourceAccess::entry(registry, node.factory.index);
    if (node.factory.kind != detail::ControlFactoryKind::KEY_HELP_FALLBACK || entry.name != "KeyHints") {
      continue;
    }
    const auto failure = [&] {
      return document.propertyFailure(Code::INVALID_VALUE, node.helpId ? node.helpOffset : node.offset, node.name,
                                      "help");
    };
    if (!node.helpId) {
      if (!singleHelp || ambiguous) {
        return std::unexpected(failure());
      }
      node.helpTarget = *singleHelp;
      continue;
    }
    if (node.helpId->empty()) {
      return std::unexpected(failure());
    }
    const auto target =
      std::ranges::find(document.nodes, *node.helpId, [](const ResourceNode &candidate) -> const std::string & {
        return candidate.arguments.id;
      });
    if (target == document.nodes.end()) {
      return std::unexpected(failure());
    }
    const auto &targetEntry = detail::ResourceAccess::entry(registry, target->factory.index);
    if (!targetEntry.declaredHelp && !targetEntry.unknownHelp) {
      return std::unexpected(failure());
    }
    node.helpTarget = static_cast<std::size_t>(target - document.nodes.begin());
  }
  return {};
}

namespace {
DeclarativeGridOptions gridSettings(const ControlGridSettings &settings) {
  DeclarativeGridOptions result;
  result.columns = settings.columns;
  result.rows = settings.rows;
  result.flow = settings.flow.value_or(result.flow);
  result.columnGap = settings.columnGap.value_or(result.columnGap);
  result.rowGap = settings.rowGap.value_or(result.rowGap);
  result.padding = {settings.paddingLeft.value_or(0), settings.paddingTop.value_or(0),
                    settings.paddingRight.value_or(0), settings.paddingBottom.value_or(0)};
  result.maxTracksPerAxis = settings.maxTracksPerAxis.value_or(result.maxTracksPerAxis);
  result.maxPlacements = settings.maxPlacements.value_or(result.maxPlacements);
  result.maxPlacementWork = settings.maxPlacementWork.value_or(result.maxPlacementWork);

  return result;
}

} // namespace

std::expected<void, ManagedError> prepareGrids(ResourceDocument &document, bool typed) {
  ControlContext fallback;
  if (!typed) {
    auto installed = detail::ControlContextAccess::installFallbackSchema(fallback);
    if (!installed) {
      return std::unexpected(detail::controlContextError(installed.error()));
    }
  }

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    auto &node = document.nodes[index];
    if (node.parent &&
        document.nodes[*node.parent].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      document.nodes[*node.parent].gridChildren.push_back(index);
    }

    if (node.itemRequested && (!node.parent || document.nodes[*node.parent].factory.kind !=
                                                 detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK)) {
      return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "row"));
    }
  }

  for (auto &node : document.nodes) {
    if (node.factory.kind != detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      continue;
    }

    if (!typed) {
      auto checked = fallback.resolve("Grid", node.arguments.settings);
      if (!checked) {
        return std::unexpected(detail::controlContextError(checked.error()));
      }

      node.arguments.width = node.arguments.settings.width;
      node.arguments.height = node.arguments.settings.height;
      node.arguments.flex = node.arguments.settings.flex;
    }

    node.gridOptions = gridSettings(node.arguments.settings.grid);
    node.gridOptions.regions = node.regions;
    node.gridOptions.stickyHeader = node.stickyHeader;
    std::vector<GridItemOptions> items(0);
    items.reserve(node.gridChildren.size());
    for (const auto child : node.gridChildren) {
      items.push_back(document.nodes[child].gridItem);
    }

    auto resolved = resolveGridPlacements(node.gridOptions, items);
    if (!resolved) {
      auto failure = document.failure(resolved.error() == TreeError::NODE_LIMIT  ? Code::NODE_LIMIT
                                      : resolved.error() == TreeError::NO_MEMORY ? Code::NO_MEMORY
                                                                                 : Code::INVALID_VALUE,
                                      node.offset);
      failure.cause = resolved.error();

      return std::unexpected(std::move(failure));
    }

    node.gridPlan = *resolved;
    node.gridNode = std::make_shared<Node<Grid>>();
  }

  return {};
}

} // namespace portals::ui::detail::resource
