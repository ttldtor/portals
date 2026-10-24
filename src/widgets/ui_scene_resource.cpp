// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_help_state.hpp"
#include "ui_scene_resource_internal.hpp"
#include <portals/ui_log_view.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>

namespace portals::ui {
namespace {
using detail::resource::identifierPart;
using detail::resource::identifierStart;

bool identifier(std::string_view value) {
  return !value.empty() && identifierStart(value.front()) && std::ranges::all_of(value.substr(1), identifierPart);
}
} // namespace

std::expected<void, SceneResourceErrorCode> SceneResourceRegistry::insert(Entry entry) {
  if (!identifier(entry.name)) {
    return std::unexpected(SceneResourceErrorCode::UNKNOWN_TYPE);
  }

  if (std::ranges::any_of(entries_, [&](const Entry &existing) {
        return existing.name == entry.name;
      })) {
    return std::unexpected(SceneResourceErrorCode::DUPLICATE_TYPE);
  }

  entries_.push_back(std::move(entry));

  return {};
}

std::optional<detail::ControlFactoryKind>
SceneResourceRegistry::semanticSchema(std::span<const ControlPropertyDescriptor> schema) noexcept {
  for (const auto kind :
       {detail::ControlFactoryKind::MESSAGE_BINDING, detail::ControlFactoryKind::NOTIFICATION_BINDING,
        detail::ControlFactoryKind::THEME_SELECTOR_BINDING, detail::ControlFactoryKind::THEME_CATALOG_BINDING}) {
    const auto canonical =
      controlPropertySchema(kind == detail::ControlFactoryKind::MESSAGE_BINDING         ? "MessageBox"
                            : kind == detail::ControlFactoryKind::NOTIFICATION_BINDING  ? "Notifications"
                            : kind == detail::ControlFactoryKind::THEME_CATALOG_BINDING ? "ThemeSelectorCatalog"
                                                                                        : "ThemeSelector");

    const auto oldSize = kind == detail::ControlFactoryKind::MESSAGE_BINDING          ? std::size_t{24}
                         : kind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ? std::size_t{22}
                         : kind == detail::ControlFactoryKind::THEME_CATALOG_BINDING  ? std::size_t{38}
                                                                                      : canonical.size();
    const auto legacy = schema.size() == oldSize && oldSize != canonical.size();
    const auto candidate = legacy ? canonical.first(oldSize) : canonical;

    if (!schema.empty() && std::ranges::equal(schema, candidate, [legacy](const auto &left, const auto &right) {
          return left.path == right.path && left.kind == right.kind && left.depth == right.depth &&
                 (std::ranges::equal(left.enumNames, right.enumNames) ||
                  (legacy && left.path == "message.defaultChoice.value" && left.enumNames.size() == 5 &&
                   std::ranges::equal(left.enumNames, right.enumNames.first(5)))) &&
                 std::ranges::equal(left.aliases, right.aliases);
        })) {
      return kind;
    }
  }

  return {};
}

SceneResourceRegistry::SceneResourceRegistry() {
  (void)add<Column>(
    "VBox",
    [](SceneResourceArguments arguments) {
      return column(LinearOptions{.gap = arguments.gap});
    },
    true);
  entries_.back().text = false;
  entries_.back().gap = true;

  (void)add<Row>(
    "HBox",
    [](SceneResourceArguments arguments) {
      return row(LinearOptions{.gap = arguments.gap});
    },
    true);
  entries_.back().text = false;
  entries_.back().gap = true;

  (void)add<Label>("Label", [](SceneResourceArguments arguments) {
    return label(arguments.text);
  });
  (void)add<LineEdit>("LineEdit", [](SceneResourceArguments arguments) {
    auto options = detail::controlLineEditOptions(arguments.settings, {}, {}, {});

    if (!options) {
      return std::expected<Description<LineEdit>, ManagedError>(std::unexpected(options.error()));
    }

    return std::expected<Description<LineEdit>, ManagedError>(
      lineEdit(arguments.text, LineEditConfig{.options = std::move(*options)}));
  });
  (void)add<FindBar>("FindBar", [](SceneResourceArguments arguments) {
    // Resource-owned text is admitted at mount with the resolved instance limits.
    return detail::Access::findBar("", std::move(arguments.text), {}).settings(arguments.settings);
  });
  (void)add<FormField>("FormField", [](SceneResourceArguments arguments) {
    return formField("", arguments.text).settings(arguments.settings);
  });
  (void)add<Password>("Password", [](SceneResourceArguments arguments) {
    return password().settings(arguments.settings);
  });
  (void)add<Button>("Button", [](SceneResourceArguments arguments) {
    return detail::textControl<Button>(TextValue(arguments.text), {}, {}, {},
                                       arguments.settings.chrome.value_or(ButtonChrome::BRACKETS));
  });
  (void)add<CheckBox>("CheckBox", [](SceneResourceArguments arguments) {
    return checkbox(arguments.text,
                    arguments.settings.state.value_or(arguments.checked ? CheckState::CHECKED : CheckState::UNCHECKED),
                    CheckBoxOptions{.cycle = arguments.settings.cycle.value_or(CheckCycle::TWO_STATE)});
  });
  entries_.back().checked = true;

  (void)add<ThreeState>("ThreeState", [](SceneResourceArguments arguments) {
    return threeState(arguments.text, arguments.settings.state.value_or(CheckState::UNCHECKED));
  });
  for (auto &entry : entries_) {
    entry.factoryKind = detail::ControlFactoryKind::EXISTING_BUILTIN;
  }
}

namespace detail {
std::optional<std::size_t> ResourceAccess::find(const SceneResourceRegistry &registry, std::string_view name) {
  for (std::size_t index = 0; index < registry.entries_.size(); ++index) {
    if (registry.entries_[index].name == name) {
      return index;
    }
  }

  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installGrid(SceneResourceRegistry &registry,
                                                                     ControlContext &controls, bool typed) {
  if (find(registry, "Grid") || ControlContextAccess::hasDeclaredType(controls, "Grid")) {
    return {};
  }

  if (typed) {
    auto installed = ControlContextAccess::installFallbackSchema(controls);
    if (!installed) {
      return installed;
    }
  }

  SceneResourceRegistry::Entry entry;
  entry.name = "Grid";
  entry.children = true;
  entry.sized = true;
  entry.text = false;
  entry.factoryKind = ControlFactoryKind::DECLARATIVE_GRID_FALLBACK;
  registry.entries_.push_back(entry);

  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installPrimitives(SceneResourceRegistry &registry,
                                                                           ControlContext &controls) {
  for (const auto name : {"Border", "Gauge", "Spinner"}) {
    if (find(registry, name) || ControlContextAccess::hasDeclaredType(controls, name)) {
      continue;
    }

    auto installed = ControlContextAccess::installPrimitiveSchema(controls, name);

    if (!installed) {
      return installed;
    }

    if (std::string_view(name) == "Border") {
      const auto added = registry.add<Border>(
        name,
        [](SceneResourceArguments arguments) {
          return Access::borderControl(controlBorderOptions(arguments.settings, {}), {});
        },
        true);

      if (!added) {
        return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
      }
    } else if (std::string_view(name) == "Spinner") {
      const auto added = registry.add<Spinner>(name, [](SceneResourceArguments arguments) {
        return spinner(controlSpinnerOptions(arguments.settings, {}));
      });
      if (!added) {
        return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
      }
    } else {
      const auto added = registry.add<Gauge>(name, [](SceneResourceArguments arguments) {
        return gauge(arguments.settings.value.value_or(0), controlGaugeOptions(arguments.settings, {}));
      });

      if (!added) {
        return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
      }
    }

    registry.entries_.back().text = false;
    registry.entries_.back().factoryKind = ControlFactoryKind::PRIMITIVE_FALLBACK;
  }

  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installScrolls(SceneResourceRegistry &registry,
                                                                        ControlContext &controls) {
  for (const auto name : {"ScrollArea", "ScrollBar", "LogView"}) {
    if (find(registry, name) || ControlContextAccess::hasDeclaredType(controls, name)) {
      continue;
    }

    auto installed = ControlContextAccess::installScrollSchema(controls, name);

    if (!installed) {
      return installed;
    }

    if (std::string_view(name) == "LogView") {
      const auto added = registry.add<LogView>(name, [](SceneResourceArguments arguments) {
        return logView(arguments.text, controlLogViewConfig(arguments.settings, {}));
      });

      if (!added) {
        return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
      }
    } else if (std::string_view(name) == "ScrollArea") {
      const auto added = registry.add<ScrollArea>(
        name,
        [](SceneResourceArguments arguments) -> std::expected<Description<ScrollArea>, ManagedError> {
          auto options = controlScrollAreaOptions(arguments.settings, {});

          if (!options) {
            return std::unexpected(error({}, options.error()));
          }

          auto recipe = Access::scrollAreaControl(*options, {}, {});

          if (hasScrollbarSettings(arguments.settings.scrollbar)) {
            (void)std::move(recipe).settings({.scrollbar = arguments.settings.scrollbar});
          }

          return recipe;
        },
        true);

      if (!added) {
        return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
      }
    } else {
      const auto added = registry.add<ScrollBar>(
        name, [](SceneResourceArguments arguments) -> std::expected<Description<ScrollBar>, ManagedError> {
          auto options = controlScrollBarOptions(arguments.settings, {});

          if (!options) {
            return std::unexpected(error({}, options.error()));
          }

          auto recipe = scrollBar(arguments.settings.axis.value_or(ScrollAxis::VERTICAL), *options);

          if (hasScrollbarSettings(arguments.settings.scrollbar)) {
            (void)std::move(recipe).settings({.scrollbar = arguments.settings.scrollbar});
          }

          return recipe;
        });

      if (!added) {
        return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
      }
    }

    registry.entries_.back().text = std::string_view(name) == "LogView";
    registry.entries_.back().factoryKind = ControlFactoryKind::SCROLL_FALLBACK;
  }

  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installOverlay(SceneResourceRegistry &registry,
                                                                        ControlContext &controls) {
  constexpr std::string_view NAME = "Overlay";
  if (find(registry, NAME) || ControlContextAccess::hasDeclaredType(controls, NAME)) {
    return {};
  }
  auto installed = ControlContextAccess::installOverlaySchema(controls);
  if (!installed) {
    return installed;
  }
  const auto added = registry.add<Overlay>(
    NAME,
    [](SceneResourceArguments arguments) -> std::expected<Description<Overlay>, ManagedError> {
      auto options = controlOverlayOptions(arguments.settings, {});
      if (!options) {
        return std::unexpected(error({}, options.error()));
      }
      return overlay(*options);
    },
    true);
  if (!added) {
    return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
  }
  registry.entries_.back().text = false;
  registry.entries_.back().factoryKind = ControlFactoryKind::OVERLAY_FALLBACK;
  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installKeyHelp(SceneResourceRegistry &registry,
                                                                        ControlContext &controls) {
  for (const auto name : {"HelpView", "KeyHints"}) {
    if (find(registry, name) || ControlContextAccess::hasDeclaredType(controls, name)) {
      continue;
    }
    auto installed = ControlContextAccess::installKeyHelpSchema(controls, name);
    if (!installed) {
      return installed;
    }
    if (std::string_view(name) == "HelpView") {
      auto entry = SceneResourceRegistry::makeEntry<HelpView>(name, [](SceneResourceArguments arguments) {
        return helpView(Ref<HelpView>{}, controlHelpViewOptions(arguments.settings, {}));
      });
      entry.text = false;
      entry.factoryKind = ControlFactoryKind::KEY_HELP_FALLBACK;
      registry.entries_.push_back(std::move(entry));
    } else {
      SceneResourceRegistry::Entry entry;
      entry.name = name;
      entry.sized = true;
      entry.text = false;
      entry.factoryKind = ControlFactoryKind::KEY_HELP_FALLBACK;
      registry.entries_.push_back(std::move(entry));
    }
  }
  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installDataState(SceneResourceRegistry &registry,
                                                                          ControlContext &controls) {
  constexpr std::string_view NAME = "DataStateView";

  if (find(registry, NAME) || ControlContextAccess::hasDeclaredType(controls, NAME)) {
    return {};
  }

  auto installed = ControlContextAccess::installDataStateSchema(controls);

  if (!installed) {
    return installed;
  }

  SceneResourceRegistry::Entry entry;

  entry.name = NAME;
  entry.children = true;
  entry.sized = true;
  entry.text = false;
  entry.factoryKind = ControlFactoryKind::DATA_STATE_FALLBACK;
  registry.entries_.push_back(std::move(entry));

  return {};
}

std::expected<void, ControlContextError> ResourceAccess::installDialogs(SceneResourceRegistry &registry,
                                                                        ControlContext &controls) {
  for (const auto name : {"OverlayHost", "Dialog"}) {
    if (find(registry, name) || ControlContextAccess::hasDeclaredType(controls, name)) {
      continue;
    }

    auto installed = ControlContextAccess::installDialogSchema(controls, name);

    if (!installed) {
      return installed;
    }

    SceneResourceRegistry::Entry entry;

    entry.name = name;
    entry.children = true;
    entry.sized = name == std::string_view("OverlayHost");
    entry.text = false;
    entry.declaredDialog = name == std::string_view("Dialog");
    entry.declaredHost = name == std::string_view("OverlayHost");
    entry.factoryKind = ControlFactoryKind::DIALOG_FALLBACK;
    registry.entries_.push_back(std::move(entry));
  }

  return {};
}

template <class Wrapper> void ResourceAccess::sourceCreate(Description<Widget> &recipe, Wrapper wrapper) {
  auto original = std::move(recipe.state_->create);

  recipe.state_->create = [original = std::move(original), wrapper = std::move(wrapper)](
                            ManagedScene &scene, Node<Widget> parent, MountContext &context) mutable {
    return wrapper(original, scene, parent, context);
  };
}

void ResourceAccess::sourceChild(Description<Widget> &parent, Description<Widget> child,
                                 std::shared_ptr<const SceneResourceError> diagnostic, bool requireDialog) {
  parent.state_->children.push_back(
    [child = std::move(child), diagnostic = std::move(diagnostic), requireDialog](
      ManagedScene &scene, Node<Widget> parent, MountContext &context) mutable -> std::expected<void, ManagedError> {
      auto result = Access::mount(std::move(child), scene, parent, context);

      if (!result) {
        if (!result.error().sceneResource) {
          result.error().sceneResource = diagnostic;
        }

        return std::unexpected(result.error());
      }

      if (requireDialog) {
        auto opened = scene.dialogOpened(*result);

        if (!opened) {
          auto failure = opened.error();

          if (!failure.sceneResource) {
            failure.sceneResource = diagnostic;
          }

          return std::unexpected(std::move(failure));
        }
      }

      return {};
    });
}

void ResourceAccess::fallbackMessageResolver(Description<Widget> &recipe, const TextResolver &resolver,
                                             std::shared_ptr<const SceneResourceError> diagnostic) {
  auto original = std::move(recipe.state_->create);

  // This pointer is captured only by a recipe consumed synchronously by compile, never a retained record.
  recipe.state_->create = [original = std::move(original), resolver = &resolver, diagnostic = std::move(diagnostic)](
                            ManagedScene &scene, Node<Widget> parent,
                            MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
    struct Restore {
      MountContext &context;
      const TextResolver *previous;

      ~Restore() {
        context.messageResolver = previous;
      }
    } restore{context, context.messageResolver};

    context.messageResolver = resolver;

    try {
      return original(scene, parent, context);
    } catch (const std::bad_alloc &) {
      auto failure = error(parent, TreeError::NO_MEMORY);

      failure.sceneResource = diagnostic;

      return std::unexpected(std::move(failure));
    } catch (...) {
      auto failure = error(parent, TreeError::CALLBACK_FAILED);

      failure.sceneResource = diagnostic;

      return std::unexpected(std::move(failure));
    }
  };
}

void ResourceAccess::fallbackThemeSelectorResolver(Description<Widget> &recipe, const TextResolver &resolver,
                                                   std::shared_ptr<const SceneResourceError> diagnostic) {
  auto original = std::move(recipe.state_->create);

  // This pointer is captured only by a recipe consumed synchronously by compile, never a retained record.
  recipe.state_->create = [original = std::move(original), resolver = &resolver, diagnostic = std::move(diagnostic)](
                            ManagedScene &scene, Node<Widget> parent,
                            MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
    struct Restore {
      MountContext &context;
      const TextResolver *previous;

      ~Restore() {
        context.themeSelectorResolver = previous;
      }
    } restore{context, context.themeSelectorResolver};

    context.themeSelectorResolver = resolver;

    try {
      return original(scene, parent, context);
    } catch (const std::bad_alloc &) {
      auto failure = error(parent, TreeError::NO_MEMORY);

      failure.sceneResource = diagnostic;

      return std::unexpected(std::move(failure));
    } catch (...) {
      auto failure = error(parent, TreeError::CALLBACK_FAILED);

      failure.sceneResource = diagnostic;

      return std::unexpected(std::move(failure));
    }
  };
}

void ResourceAccess::fallbackDataStateResolver(Description<DataStateView> &recipe, const TextResolver &resolver,
                                               std::shared_ptr<const SceneResourceError> diagnostic) {
  auto original = std::move(recipe.state_->create);

  // This pointer is captured only by a recipe consumed synchronously by compile, never a retained record.
  recipe.state_->create = [original = std::move(original), resolver = &resolver, diagnostic = std::move(diagnostic)](
                            ManagedScene &scene, Node<Widget> parent,
                            MountContext &context) mutable -> std::expected<Node<DataStateView>, ManagedError> {
    struct Restore {
      MountContext &context;
      const TextResolver *previous;

      ~Restore() {
        context.dataStateResolver = previous;
      }
    } restore{context, context.dataStateResolver};

    context.dataStateResolver = resolver;

    try {
      return original(scene, parent, context);
    } catch (const std::bad_alloc &) {
      auto failure = error(parent, TreeError::NO_MEMORY);

      failure.sceneResource = diagnostic;

      return std::unexpected(std::move(failure));
    } catch (...) {
      auto failure = error(parent, TreeError::CALLBACK_FAILED);

      failure.sceneResource = diagnostic;

      return std::unexpected(std::move(failure));
    }
  };
}

std::expected<Description<Widget>, ManagedError> ResourceAccess::keyHintsRecipe(SceneResourceArguments arguments,
                                                                                Ref<Widget> help) {
  // This transient adapter owns the same cold observer as the native recipe; no resource map escapes mount.
  auto entry = SceneResourceRegistry::makeEntry<KeyHints>("KeyHints", [help](SceneResourceArguments values) {
    return KeyHelpAccess::hints(help, controlKeyHintsOptions(values.settings, {}), {});
  });
  return entry.create(std::move(arguments));
}

} // namespace detail

namespace {
using Code = SceneResourceErrorCode;
using detail::resource::parseResourceDocument;
using detail::resource::prepareGrids;
using detail::resource::prepareHelpReferences;
using detail::resource::prepareNodes;
using detail::resource::registerSchemas;
using detail::resource::ResourceDocument;
using detail::resource::ResourceNode;
using detail::resource::valid;

GridOptions boundGridOptions(const ResourceDocument &document, const ResourceNode &node,
                             const std::vector<Node<Widget>> &mounted) {
  GridOptions result;
  result.columns.resize(node.gridPlan->columns);
  result.rows.resize(node.gridPlan->rows);
  result.header_rows.resize(node.gridPlan->headerRows);
  result.sticky_header = node.gridOptions.stickyHeader;
  result.regions = node.gridOptions.regions;
  result.column_gap = node.gridOptions.columnGap;
  result.row_gap = node.gridOptions.rowGap;
  result.padding = node.gridOptions.padding;
  result.max_tracks_per_axis = node.gridOptions.maxTracksPerAxis;
  result.max_placements = node.gridOptions.maxPlacements;
  result.cells.reserve(node.gridChildren.size());
  for (std::size_t index = 0; index < node.gridChildren.size(); ++index) {
    const auto child = node.gridChildren[index];
    const auto &item = document.nodes[child].gridItem;
    const auto position = node.gridPlan->positions[index];
    result.cells.push_back({mounted[child].id(), position.row, position.column, item.rowSpan, item.columnSpan,
                            item.horizontal, item.vertical, item.region});
  }

  return result;
}

std::expected<Description<Widget>, ManagedError> gridRecipe(ResourceNode &node) {
  auto recipe = grid()
                  .gridOptions(node.gridOptions)
                  .setup([slot = node.gridNode](ManagedScene &, Node<Grid> node) -> std::expected<void, ManagedError> {
                    *slot = node;

                    return {};
                  });
  if (node.arguments.classes) {
    (void)std::move(recipe).classes(*node.arguments.classes);
  }

  if (node.arguments.visible) {
    (void)std::move(recipe).visible(*node.arguments.visible);
  }

  if (node.arguments.enabled) {
    (void)std::move(recipe).enabled(*node.arguments.enabled);
  }

  if (node.arguments.settings.grid.scroll.value_or(false)) {
    (void)std::move(recipe).scroll();
  }

  if (!node.arguments.id.empty()) {
    (void)std::move(recipe).id(node.arguments.id);
  }

  if (node.arguments.width) {
    (void)std::move(recipe).width(*node.arguments.width);
  }

  if (node.arguments.height) {
    (void)std::move(recipe).height(*node.arguments.height);
  }

  if (node.arguments.flex) {
    (void)std::move(recipe).flex(*node.arguments.flex);
  }

  detail::Access::frameSettings(recipe, node.arguments.settings);
  detail::Access::preparedControlPresence(recipe, detail::controlSettingsPresenceSet(node.arguments.settings));

  return detail::Access::erase(std::move(recipe));
}

ManagedError textFailure(ResourceDocument &document, const ResourceNode &node, std::string_view path,
                         const TextValueError *cause = nullptr) {
  auto failure = document.propertyFailure(Code::TEXT, node.offset, node.name, path);
  const auto &settings = node.arguments.settings;
  const auto found = std::ranges::find(settings.origins, path, &ControlPropertyOrigin::path);
  const auto origin = found == settings.origins.end() ? settings.origin : found->source;

  try {
    if (origin && failure.sceneResource) {
      auto resource = *failure.sceneResource;

      resource.source = origin->sharedSourceIdentity
                          ? origin->sharedSourceIdentity
                          : std::shared_ptr<const std::string>(origin, &origin->sourceIdentity);
      resource.offset = origin->byteOffset;
      resource.line = origin->line;
      resource.column = origin->column;
      failure.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
    }
  } catch (const std::bad_alloc &) {
    // The text/resource cause below is retained even when enrichment cannot allocate.
  }

  if (cause) {
    if (const auto *text = std::get_if<TextError>(cause)) {
      failure.text = *text;
    } else {
      failure.resource = std::get<TextResourceError>(*cause);
    }
  }

  return failure;
}

std::expected<void, ManagedError> resolveSettings(ResourceDocument &document, const SceneResourceOptions &options,
                                                  WidthPolicy policy) {
  TextLimits resourceLimits;

  resourceLimits.max_input_bytes = options.limits.maxStringBytes;

  for (auto &node : document.nodes) {
    const auto *entry = node.factory.kind == detail::ControlFactoryKind::EXISTING_BUILTIN ||
                            node.factory.kind == detail::ControlFactoryKind::USER
                          ? &detail::ResourceAccess::entry(options.registry, node.factory.index)
                          : nullptr;
    auto limits = entry && entry->protectedEditor
                    ? detail::controlTextLimits(node.arguments.settings, PasswordOptions{}.limits)
                  : entry && entry->name == "LineEdit"
                    ? detail::controlTextLimits(node.arguments.settings, LineEditOptions{}.limits)
                    : resourceLimits;

    // Resource budgets still bound resolver output, independently of a control's own text limits.
    limits.max_input_bytes = std::min(limits.max_input_bytes, resourceLimits.max_input_bytes);

    for (const auto path :
         {std::string_view("text"), std::string_view("placeholder"), std::string_view("title"),
          std::string_view("frame.title"), std::string_view("label"), std::string_view("help"),
          std::string_view("requiredMessage"), std::string_view("document"), std::string_view("previousLabel"),
          std::string_view("nextLabel"), std::string_view("clearLabel")}) {
      if (path == "title" && node.factory.kind != detail::ControlFactoryKind::DIALOG_FALLBACK) {
        continue;
      }

      auto &setting = path == "text"              ? node.arguments.settings.text
                      : path == "placeholder"     ? node.arguments.settings.placeholder
                      : path == "label"           ? node.arguments.settings.spinnerLabel
                      : path == "document"        ? node.arguments.settings.findDocument
                      : path == "previousLabel"   ? node.arguments.settings.findPreviousLabel
                      : path == "nextLabel"       ? node.arguments.settings.findNextLabel
                      : path == "clearLabel"      ? node.arguments.settings.findClearLabel
                      : path == "help"            ? node.arguments.settings.fieldHelp
                      : path == "requiredMessage" ? node.arguments.settings.fieldRequiredMessage
                      : path == "frame.title"     ? node.arguments.settings.frame.title
                                                  : node.arguments.settings.title;

      if (!setting) {
        continue;
      }

      auto effectiveLimits = limits;

      if (path == "frame.title") {
        effectiveLimits = resourceLimits;
        effectiveLimits.max_input_bytes =
          std::min(resourceLimits.max_input_bytes,
                   node.arguments.settings.frame.maxTitleBytes.value_or(FrameOptions{}.maxTitleBytes));
      }

      auto value = options.resolver ? setting->resolve(policy, options.resolver, effectiveLimits)
                                    : setting->resolve(policy, effectiveLimits);

      if (!value) {
        return std::unexpected(textFailure(document, node, path, &value.error()));
      }

      if (path == "placeholder" && portals::detail::hasLineSeparator(value->bytes())) {
        return std::unexpected(textFailure(document, node, path));
      }

      setting = TextValue(value->bytes());

      if (path == "text") {
        node.arguments.text = value->bytes();
      }
    }
  }

  return {};
}

std::expected<void, ManagedError> resolve(ResourceDocument &document, const SceneResourceOptions &options,
                                          WidthPolicy policy) {
  for (auto &node : document.nodes) {
    if (!node.hasText) {
      continue;
    }

    TextLimits limits;

    limits.max_input_bytes = options.limits.maxStringBytes;

    auto value = [&] {
      if (node.resourceText) {
        if (options.resolver) {
          return i18n(node.arguments.text).resolve(policy, options.resolver, limits);
        }

        return i18n(node.arguments.text).resolve(policy, limits);
      }

      return TextValue(node.arguments.text).resolve(policy, limits);
    }();

    if (!value) {
      auto failure = document.failure(Code::TEXT, node.textOffset);

      if (const auto *text = std::get_if<TextError>(&value.error())) {
        failure.text = *text;
      } else {
        failure.resource = std::get<TextResourceError>(value.error());
      }

      return std::unexpected(failure);
    }

    node.arguments.text = value->bytes();
  }

  return {};
}

// Configuration-only adapter. Match the registry's common-property and leaf-accounting contract before erasure.
template <class T> Description<Widget> linearRecipe(const ResourceNode &node) {
  auto recipe =
    linear<T>(detail::controlLinearOptions(node.arguments.settings, LinearOptions{.gap = node.arguments.gap}));

  detail::Access::frameSettings(recipe, node.arguments.settings);
  detail::Access::preparedControlPresence(recipe, detail::controlSettingsPresenceSet(node.arguments.settings));

  if (node.arguments.classes) {
    (void)std::move(recipe).classes(*node.arguments.classes);
  }

  if (node.arguments.visible) {
    (void)std::move(recipe).visible(*node.arguments.visible);
  }

  if (node.arguments.enabled) {
    (void)std::move(recipe).enabled(*node.arguments.enabled);
  }

  if (!node.arguments.id.empty()) {
    (void)std::move(recipe).id(node.arguments.id);
  }

  if (node.arguments.width) {
    (void)std::move(recipe).width(*node.arguments.width);
  }

  if (node.arguments.height) {
    (void)std::move(recipe).height(*node.arguments.height);
  }

  if (node.arguments.flex) {
    (void)std::move(recipe).flex(*node.arguments.flex);
  }

  return detail::Access::erase(std::move(recipe));
}

Description<Widget> dataStateRecipe(const ResourceNode &node, Description<Widget> body, const TextResolver &resolver,
                                    std::shared_ptr<const SceneResourceError> diagnostic) {
  auto recipe = ui::dataStateView(DataPresentation{}, std::move(body)).settings(node.arguments.settings);

  detail::Access::preparedControlPresence(recipe, detail::controlSettingsPresenceSet(node.arguments.settings));
  detail::ResourceAccess::fallbackDataStateResolver(recipe, resolver, std::move(diagnostic));
  if (!node.arguments.id.empty()) {
    (void)std::move(recipe).id(node.arguments.id);
  }
  if (node.arguments.visible) {
    (void)std::move(recipe).visible(*node.arguments.visible);
  }
  if (node.arguments.enabled) {
    (void)std::move(recipe).enabled(*node.arguments.enabled);
  }
  if (node.arguments.classes) {
    (void)std::move(recipe).classes(*node.arguments.classes);
  }

  return detail::Access::erase(std::move(recipe));
}

/** @tparam T Exact native OverlayHost or Dialog.
 * @param node Owning source metadata borrowed during synchronous materialization only.
 * @param recipe Native recipe whose complete body has already been composed.
 * @return Erased cold recipe with the existing source presence and instance flags retained.
 */
template <class T> Description<Widget> preparedDialogRecipe(const ResourceNode &node, Description<T> recipe) {
  (void)std::move(recipe).settings(node.arguments.settings);
  detail::Access::preparedControlPresence(recipe, detail::controlSettingsPresenceSet(node.arguments.settings));

  if (!node.arguments.id.empty()) {
    (void)std::move(recipe).id(node.arguments.id);
  }

  if (node.arguments.visible) {
    (void)std::move(recipe).visible(*node.arguments.visible);
  }

  if (node.arguments.enabled) {
    (void)std::move(recipe).enabled(*node.arguments.enabled);
  }

  if (node.arguments.classes) {
    (void)std::move(recipe).classes(*node.arguments.classes);
  }

  return detail::Access::erase(std::move(recipe));
}

ManagedError attributed(ResourceDocument &document, std::size_t index, ManagedError failure) {
  if (!failure.sceneResource) {
    failure.sceneResource = document.failure(Code::FACTORY, document.nodes[index].offset).sceneResource;
  }

  return failure;
}

std::expected<Node<Widget>, ManagedError> compile(ManagedScene &owner, ResourceDocument &document,
                                                  const SceneResourceOptions &options, Node<Widget> parent,
                                                  detail::MountContext &context) try {
  auto effectiveOptions = options;
  auto controls = options.controls.value_or(context.controls);
  const bool typed = options.format == SceneResourceFormat::CONFIGURATION ||
                     detail::ControlContextAccess::hasUserConfiguration(controls);

  if (typed) {
    auto registered = registerSchemas(controls, options);

    if (!registered) {
      return std::unexpected(registered.error());
    }
  }

  auto parsed = parseResourceDocument(document, effectiveOptions, controls, typed);

  if (!parsed) {
    return std::unexpected(parsed.error());
  }

  if (typed) {
    auto prepared = prepareNodes(document, controls, context, owner, effectiveOptions.registry);

    if (!prepared) {
      return std::unexpected(prepared.error());
    }
  }

  // Check every protected node before invoking any document resolver or factory.
  for (auto &node : document.nodes) {
    if (node.factory.kind != detail::ControlFactoryKind::USER &&
        node.factory.kind != detail::ControlFactoryKind::EXISTING_BUILTIN) {
      continue;
    }
    if (!detail::ResourceAccess::entry(effectiveOptions.registry, node.factory.index).protectedEditor) {
      continue;
    }
    if (node.resourceText || !node.arguments.text.empty() ||
        (node.arguments.settings.text && !node.arguments.settings.text->isEmptyLiteral())) {
      return std::unexpected(document.propertyFailure(Code::SECRET_LITERAL, node.textOffset, node.name, "text"));
    }

    auto checked = controls.resolve("Password", node.arguments.settings);

    if (!checked) {
      auto failure = detail::controlContextError(checked.error());

      if (!failure.sceneResource) {
        failure.sceneResource = document.failure(Code::INVALID_VALUE, node.offset).sceneResource;
      }

      return std::unexpected(std::move(failure));
    }
  }

  if (!typed) {
    for (auto &node : document.nodes) {
      if ((node.factory.kind != detail::ControlFactoryKind::USER &&
           node.factory.kind != detail::ControlFactoryKind::EXISTING_BUILTIN) ||
          !detail::ResourceAccess::entry(effectiveOptions.registry, node.factory.index).protectedEditor ||
          !node.arguments.settings.placeholder) {
        continue;
      }

      auto &placeholder = *node.arguments.settings.placeholder;
      auto limits = detail::controlTextLimits(node.arguments.settings, PasswordOptions{}.limits);

      limits.max_input_bytes = std::min(limits.max_input_bytes, options.limits.maxStringBytes);

      auto value = options.resolver ? placeholder.resolve(owner.widthPolicy(), options.resolver, limits)
                                    : placeholder.resolve(owner.widthPolicy(), limits);

      if (!value) {
        return std::unexpected(textFailure(document, node, "placeholder", &value.error()));
      }

      placeholder = TextValue(value->bytes());
    }
  }

  auto references = prepareHelpReferences(document, effectiveOptions.registry);
  if (!references) {
    return std::unexpected(references.error());
  }

  auto placements = prepareGrids(document, typed);

  if (!placements) {
    return std::unexpected(placements.error());
  }

  auto resolved =
    typed ? resolveSettings(document, options, owner.widthPolicy()) : resolve(document, options, owner.widthPolicy());

  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  std::vector<std::optional<Ref<Widget>>> helpObservers(document.nodes.size());
  for (const auto &node : document.nodes) {
    if (node.helpTarget && !helpObservers[*node.helpTarget]) {
      helpObservers[*node.helpTarget].emplace();
    }
  }

  const auto existingNodes = context.nodes.size();
  std::vector<Node<Widget>> mounted(document.nodes.size());
  std::vector<Node<Widget>> layoutRoots(0);
  const auto rememberMounted = [&](std::size_t index, Node<Widget> source, Node<Widget> outer) {
    mounted[index] = source;

    if (outer == Node<Widget>{}) {
      outer = source;
    }

    if (layoutRoots.empty() && outer != source) {
      // Keep source lookup allocation-free until a real wrapper creates a distinct layout root.
      layoutRoots = mounted;
    }

    if (!layoutRoots.empty()) {
      layoutRoots[index] = outer;
    }
  };
  std::vector<bool> insideComposite(document.nodes.size());
  std::vector<std::vector<std::size_t>> children(document.nodes.size(), std::vector<std::size_t>(0));

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    const auto sourceParent = document.nodes[index].parent;

    if (sourceParent) {
      children[*sourceParent].push_back(index);

      const auto parentKind = document.nodes[*sourceParent].factory.kind;

      insideComposite[index] = insideComposite[*sourceParent] ||
                               parentKind == detail::ControlFactoryKind::DATA_STATE_FALLBACK ||
                               parentKind == detail::ControlFactoryKind::DIALOG_FALLBACK ||
                               parentKind == detail::ControlFactoryKind::MESSAGE_BINDING;
    }
  }

  std::vector<Description<Widget>> recipes(document.nodes.size());

  // Materialize source postorder, but keep every resource edge as an index. The existing
  // typed descriptions mount one at a time in the same transaction rather than recursively
  // multiplying the type-erasure adapters at every source level.
  for (auto index : document.order) {
    auto &node = document.nodes[index];

    try {
      auto recipe = [&]() -> std::expected<Description<Widget>, ManagedError> {
        if (node.factory.kind == detail::ControlFactoryKind::DATA_STATE_FALLBACK) {
          return dataStateRecipe(node, std::move(recipes[children[index].front()]), effectiveOptions.resolver,
                                 document.failure(Code::FACTORY, node.offset).sceneResource);
        }
        if (node.factory.kind == detail::ControlFactoryKind::DIALOG_FALLBACK) {
          auto body = std::move(recipes[children[index].front()]);
          const auto &entry = detail::ResourceAccess::entry(effectiveOptions.registry, node.factory.index);

          if (entry.declaredHost) {
            auto host = preparedDialogRecipe(node, ui::overlayHost(std::move(body)));

            for (std::size_t layer = 1; layer < children[index].size(); ++layer) {
              const auto child = children[index][layer];

              detail::ResourceAccess::sourceChild(
                host, std::move(recipes[child]),
                document.failure(Code::FACTORY, document.nodes[child].offset).sceneResource, true);
            }

            return host;
          }

          return preparedDialogRecipe(node, ui::dialog(*node.arguments.settings.title, std::move(body)));
        }

        if (node.factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
          return gridRecipe(node);
        }

        const auto &entry = detail::ResourceAccess::entry(effectiveOptions.registry, node.factory.index);

        if (node.helpTarget) {
          return detail::ResourceAccess::keyHintsRecipe(std::move(node.arguments), *helpObservers[*node.helpTarget]);
        }

        // Legacy adapters retain their original gap-only behavior; caller factories keep all arguments.
        if (options.format == SceneResourceFormat::CONFIGURATION &&
            node.factory.kind == detail::ControlFactoryKind::EXISTING_BUILTIN) {
          if (entry.name == "VBox") {
            return linearRecipe<Column>(node);
          }

          if (entry.name == "HBox") {
            return linearRecipe<Row>(node);
          }
        }

        auto result = entry.create(std::move(node.arguments));

        if (result && node.themeSource && detail::Access::themeCatalogRecipient(*result)) {
          auto recipient = controls;
          auto installed = detail::ControlContextAccess::installThemeCatalogSchema(recipient, true);
          if (!installed) {
            return std::unexpected(detail::controlContextError(installed.error()));
          }
          auto combined = recipient.resolve(node.name, node.themeSource->instance);
          if (!combined) {
            return std::unexpected(detail::controlContextError(combined.error()));
          }
          const auto oldCount = node.themeSource->materialized.count();
          const auto newPresence = detail::controlSettingsPresenceSet(combined->settings);
          const auto newCount = newPresence.count();
          const auto limit = controls.limits().maxMaterializedLeaves;
          if (oldCount > context.controlLeaves || context.controlLeaves - oldCount > limit ||
              newCount > limit - (context.controlLeaves - oldCount)) {
            return std::unexpected(document.failure(Code::MATERIALIZATION_LIMIT, node.offset));
          }
          auto prepared = detail::Access::messageSource(*result, combined->settings, newPresence,
                                                        detail::ControlFactoryKind::THEME_CATALOG_BINDING);
          if (!prepared) {
            return std::unexpected(prepared.error());
          }
          detail::Access::preparedControlPresence(*result, newPresence);
          node.lineage = std::move(combined->lineage);
          node.themeSource->materialized = newPresence;
          // Commit only after all fallible resolution and packet copies have succeeded.
          context.controlLeaves = context.controlLeaves - oldCount + newCount;
        }

        if (result && entry.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING) {
          if (!children[index].empty()) {
            if (children[index].size() != 1 || node.arguments.settings.body) {
              return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "body"));
            }

            auto body = detail::Access::messageBody(*result, std::move(recipes[children[index].front()]));

            if (!body) {
              return std::unexpected(body.error());
            }
          }

          detail::ResourceAccess::fallbackMessageResolver(*result, effectiveOptions.resolver,
                                                          document.failure(Code::FACTORY, node.offset).sceneResource);
        }

        if (result && (entry.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ||
                       entry.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING)) {
          detail::ResourceAccess::fallbackThemeSelectorResolver(
            *result, effectiveOptions.resolver, document.failure(Code::FACTORY, node.offset).sceneResource);
        }

        return result;
      }();

      if (!recipe) {
        return std::unexpected(attributed(document, index, recipe.error()));
      }

      if (helpObservers[index]) {
        *recipe = std::move(*recipe).reference(*helpObservers[index]);
      }

      if (typed && insideComposite[index]) {
        *recipe = std::move(*recipe).setup([identity = StyleTarget{.type = node.name, .baseTypes = node.lineage},
                                            diagnostic = document.failure(Code::FACTORY, node.offset).sceneResource](
                                             ManagedScene &scene, Node<Widget> node) {
          auto result = detail::Access::finalizeStyleIdentity(scene, node, identity);

          if (!result && !result.error().sceneResource) {
            result.error().sceneResource = diagnostic;
          }

          return result;
        });
      }

      if (typed && !insideComposite[index]) {
        *recipe = std::move(*recipe).setup([identity = StyleTarget{.type = node.name, .baseTypes = node.lineage}](
                                             ManagedScene &scene, Node<Widget> node) {
          return detail::Access::finalizeStyleIdentity(scene, node, identity);
        });
      }

      if (insideComposite[index] && node.factory.kind != detail::ControlFactoryKind::DATA_STATE_FALLBACK &&
          node.factory.kind != detail::ControlFactoryKind::DIALOG_FALLBACK &&
          node.factory.kind != detail::ControlFactoryKind::MESSAGE_BINDING) {
        for (auto child : children[index]) {
          detail::ResourceAccess::sourceChild(
            *recipe, std::move(recipes[child]),
            document.failure(Code::FACTORY, document.nodes[child].offset).sceneResource);
        }
      }

      if (insideComposite[index]) {
        detail::ResourceAccess::sourceCreate(
          *recipe,
          [&, index](auto &create, ManagedScene &scene, Node<Widget> parent,
                     detail::MountContext &mount) -> std::expected<Node<Widget>, ManagedError> {
            const auto finalizersBefore = mount.finalize.size();
            auto result = create(scene, parent, mount);

            if (!result) {
              return std::unexpected(attributed(document, index, result.error()));
            }

            const auto &sourceNode = document.nodes[index];

            if (sourceNode.helpTarget) {
              auto diagnostic =
                document
                  .propertyFailure(Code::INVALID_VALUE, sourceNode.helpId ? sourceNode.helpOffset : sourceNode.offset,
                                   sourceNode.name, "help")
                  .sceneResource;

              for (auto item = finalizersBefore; item < mount.finalize.size(); ++item) {
                auto original = std::move(mount.finalize[item]);

                mount.finalize[item] = [original = std::move(original),
                                        diagnostic]() mutable -> std::expected<void, ManagedError> {
                  auto finalized = original();

                  if (!finalized && !finalized.error().sceneResource) {
                    finalized.error().sceneResource = diagnostic;
                  }

                  return finalized;
                };
              }
            }

            rememberMounted(index, *result, mount.mountedOuter);

            auto checked = detail::Access::check(scene, *result);

            if (!checked) {
              return std::unexpected(attributed(document, index, checked.error()));
            }

            return result;
          });
      }
      recipes[index] = std::move(*recipe);
    } catch (const std::bad_alloc &) {
      return std::unexpected(attributed(document, index, detail::error({}, TreeError::NO_MEMORY)));
    } catch (...) {
      return std::unexpected(attributed(document, index, detail::error({}, TreeError::CALLBACK_FAILED)));
    }
  }

  const auto inherited = context.controls;

  struct RestoreContext {
    detail::MountContext &context;
    ControlContext inherited;

    ~RestoreContext() {
      context.controls = inherited;
    }
  } restore{context, inherited};

  context.controls = controls;

  for (std::size_t index = 0; index < recipes.size(); ++index) {
    if (insideComposite[index]) {
      continue;
    }

    const auto target = document.nodes[index].parent ? mounted[*document.nodes[index].parent] : parent;
    const auto finalizersBefore = context.finalize.size();
    auto node = detail::Access::mount(std::move(recipes[index]), owner, target, context);

    if (!node) {
      return std::unexpected(attributed(document, index, node.error()));
    }

    const auto &sourceNode = document.nodes[index];
    if (sourceNode.helpTarget) {
      auto diagnostic =
        document
          .propertyFailure(Code::INVALID_VALUE, sourceNode.helpId ? sourceNode.helpOffset : sourceNode.offset,
                           sourceNode.name, "help")
          .sceneResource;
      for (auto item = finalizersBefore; item < context.finalize.size(); ++item) {
        auto original = std::move(context.finalize[item]);
        context.finalize[item] = [original = std::move(original),
                                  diagnostic]() mutable -> std::expected<void, ManagedError> {
          auto result = original();
          if (!result && !result.error().sceneResource) {
            result.error().sceneResource = diagnostic;
          }
          return result;
        };
      }
    }

    rememberMounted(index, *node, context.mountedOuter);

    auto checked = detail::Access::check(owner, *node);

    if (!checked) {
      return std::unexpected(attributed(document, index, checked.error()));
    }
  }

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    auto &node = document.nodes[index];
    if (node.gridPlan) {
      auto configured = detail::Access::gridOptions(
        owner, *node.gridNode, boundGridOptions(document, node, layoutRoots.empty() ? mounted : layoutRoots));
      if (!configured) {
        return std::unexpected(attributed(document, index, configured.error()));
      }
    }
  }

  for (std::size_t index = existingNodes; index < context.nodes.size(); ++index) {
    auto checked = detail::Access::check(owner, context.nodes[index]);

    if (!checked) {
      return std::unexpected(attributed(document, 0, checked.error()));
    }
  }

  context.mountedOuter = layoutRoots.empty() ? mounted.front() : layoutRoots.front();

  return mounted.front();
} catch (const std::bad_alloc &) {
  auto failure = document.failure(Code::NO_MEMORY, 0);

  failure.cause = TreeError::NO_MEMORY;

  return std::unexpected(std::move(failure));
} catch (...) {
  auto failure = document.failure(Code::FACTORY, 0);

  failure.cause = TreeError::CALLBACK_FAILED;

  return std::unexpected(std::move(failure));
}
} // namespace

Description<Widget> resource(std::string_view source, SceneResourceOptions options) {
  auto input = std::make_shared<const std::string>(source);
  auto identity = std::make_shared<const std::string>("<string resource>");

  return detail::Access::deferControlValidation(detail::Access::forwardOuter(detail::Access::transaction(
    [input = std::move(input), identity = std::move(identity),
     options = std::move(options)](ManagedScene &owner, Node<Widget> parent,
                                   detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      ResourceDocument document{identity};

      try {
        if (const auto &registration = detail::ResourceAccess::registrationFailure(options); registration) {
          auto failure = document.failure(registration->first, 0, {}, false);

          failure.cause = registration->second;

          return std::unexpected(std::move(failure));
        }

        if (!valid(options.limits)) {
          return std::unexpected(document.failure(Code::INVALID_LIMIT, 0));
        }

        if (input->size() > options.limits.maxSourceBytes) {
          return std::unexpected(document.failure(Code::SOURCE_LIMIT, 0));
        }

        document.bytes = *input;

        return compile(owner, document, options, parent, context);
      } catch (const std::bad_alloc &) {
        auto failure = document.failure(Code::NO_MEMORY, 0);

        failure.cause = TreeError::NO_MEMORY;

        return std::unexpected(std::move(failure));
      }
    })));
}

Description<Widget> resource(SceneResourceFile source, SceneResourceOptions options) {
  const auto pathBytes = source.path.generic_u8string();
  auto identity =
    std::make_shared<const std::string>(reinterpret_cast<const char *>(pathBytes.data()), pathBytes.size());

  return detail::Access::deferControlValidation(detail::Access::forwardOuter(detail::Access::transaction(
    [source = std::move(source), identity = std::move(identity),
     options = std::move(options)](ManagedScene &owner, Node<Widget> parent,
                                   detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      ResourceDocument document{identity};

      try {
        if (const auto &registration = detail::ResourceAccess::registrationFailure(options); registration) {
          auto failure = document.failure(registration->first, 0, {}, false);

          failure.cause = registration->second;

          return std::unexpected(std::move(failure));
        }

        if (!valid(options.limits)) {
          return std::unexpected(document.failure(Code::INVALID_LIMIT, 0));
        }

        errno = 0;

        std::ifstream input(source.path, std::ios::binary);

        if (!input) {
          return std::unexpected(
            document.failure(Code::FILE_IO, 0, std::error_code(errno ? errno : EIO, std::generic_category())));
        }

        // Read a bounded extra byte to distinguish an exact cap from a truncated resource.
        std::array<char, 4096> buffer{};

        while (input && document.bytes.size() <= options.limits.maxSourceBytes) {
          const auto remaining = options.limits.maxSourceBytes - document.bytes.size();
          const auto count = remaining < buffer.size() ? remaining + 1 : buffer.size();

          input.read(buffer.data(), static_cast<std::streamsize>(count));
          document.bytes.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        }

        if (input.bad() || (input.fail() && !input.eof())) {
          return std::unexpected(document.failure(Code::FILE_IO, document.bytes.size(),
                                                  std::error_code(errno ? errno : EIO, std::generic_category())));
        }

        return compile(owner, document, options, parent, context);
      } catch (const std::bad_alloc &) {
        auto failure = document.failure(Code::NO_MEMORY, 0);

        failure.cause = TreeError::NO_MEMORY;

        return std::unexpected(std::move(failure));
      } catch (const std::filesystem::filesystem_error &failure) {
        return std::unexpected(document.failure(Code::FILE_IO, 0, failure.code()));
      }
    })));
}

Scene scene(std::string_view source, SceneResourceOptions resourceOptions, SceneOptions options,
            std::source_location origin) {
  return scene(resource(source, std::move(resourceOptions)), options, origin);
}

Scene scene(SceneResourceFile source, SceneResourceOptions resourceOptions, SceneOptions options,
            std::source_location origin) {
  return scene(resource(std::move(source), std::move(resourceOptions)), options, origin);
}
} // namespace portals::ui
