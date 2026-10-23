// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_help_state.hpp"
#include <portals/detail/control_context.hpp>
#include <portals/ui_scene_resource.hpp>

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>

namespace portals::ui {
namespace {
bool identifierStart(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool identifierPart(char value) {
  return identifierStart(value) || (value >= '0' && value <= '9');
}

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
/** @brief Internal immutable schema and checked materialization access; no registry borrow escapes prepare. */
struct ResourceAccess {
  static const auto &entries(const SceneResourceRegistry &registry) noexcept {
    return registry.entries_;
  }

  static const auto &registrationFailure(const SceneResourceOptions &options) noexcept {
    return options.registration.failure_;
  }

  /** @param registry Owning allowlist borrowed for one prepare.
   * @param name Case-sensitive source type.
   * @return Stable entry index, or no match; no factory is invoked.
   */
  static std::optional<std::size_t> find(const SceneResourceRegistry &registry, std::string_view name) {
    for (std::size_t index = 0; index < registry.entries_.size(); ++index) {
      if (registry.entries_[index].name == name) {
        return index;
      }
    }

    return {};
  }

  /** @brief Append the bounded default only after caller declarations have claimed their names. */
  static std::expected<void, ControlContextError> installGrid(SceneResourceRegistry &registry, ControlContext &controls,
                                                              bool typed) {
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

  /** @brief Install CONFIGURATION-only native primitives after caller declarations and registrations. */
  static std::expected<void, ControlContextError> installPrimitives(SceneResourceRegistry &registry,
                                                                    ControlContext &controls) {
    for (const auto name : {"Border", "Gauge"}) {
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

  /** @brief Install scoped CONFIGURATION scroll adapters without replacing caller names or writer roles. */
  static std::expected<void, ControlContextError> installScrolls(SceneResourceRegistry &registry,
                                                                 ControlContext &controls) {
    for (const auto name : {"ScrollArea", "ScrollBar"}) {
      if (find(registry, name) || ControlContextAccess::hasDeclaredType(controls, name)) {
        continue;
      }

      auto installed = ControlContextAccess::installScrollSchema(controls, name);

      if (!installed) {
        return installed;
      }

      if (std::string_view(name) == "ScrollArea") {
        const auto added = registry.add<ScrollArea>(
          name,
          [](SceneResourceArguments arguments) -> std::expected<Description<ScrollArea>, ManagedError> {
            auto options = controlScrollAreaOptions(arguments.settings, {});

            if (!options) {
              return std::unexpected(error({}, options.error()));
            }

            return Access::scrollAreaControl(*options, {}, {});
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

            return scrollBar(arguments.settings.axis.value_or(ScrollAxis::VERTICAL), *options);
          });

        if (!added) {
          return std::unexpected(ControlContextError{ControlContextErrorCode::DUPLICATE_TYPE, {}});
        }
      }

      registry.entries_.back().text = false;
      registry.entries_.back().factoryKind = ControlFactoryKind::SCROLL_FALLBACK;
    }

    return {};
  }

  /** @brief Install scoped ordinary Overlay only after caller names have claimed their registry/schema entries. */
  static std::expected<void, ControlContextError> installOverlay(SceneResourceRegistry &registry,
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

  static std::expected<void, ControlContextError> installKeyHelp(SceneResourceRegistry &registry,
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

  /** @brief CFG-only one-body native entry; public custom registrations retain their old adapters. */
  static std::expected<void, ControlContextError> installDataState(SceneResourceRegistry &registry,
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

  /** @brief CFG-only managed host/layer entries; caller names and old adapters retain priority. */
  static std::expected<void, ControlContextError> installDialogs(SceneResourceRegistry &registry,
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

  // Mutate the existing source recipe storage, without adding transaction/erasure layers per descendant.
  template <class Wrapper> static void sourceCreate(Description<Widget> &recipe, Wrapper wrapper) {
    auto original = std::move(recipe.state_->create);

    recipe.state_->create = [original = std::move(original), wrapper = std::move(wrapper)](
                              ManagedScene &scene, Node<Widget> parent, MountContext &context) mutable {
      return wrapper(original, scene, parent, context);
    };
  }

  static void sourceChild(Description<Widget> &parent, Description<Widget> child,
                          std::shared_ptr<const SceneResourceError> diagnostic, bool requireDialog = false) {
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

  static void fallbackMessageResolver(Description<Widget> &recipe, const TextResolver &resolver,
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

  static void fallbackThemeSelectorResolver(Description<Widget> &recipe, const TextResolver &resolver,
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

  static void fallbackDataStateResolver(Description<DataStateView> &recipe, const TextResolver &resolver,
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

  static std::expected<Description<Widget>, ManagedError> keyHintsRecipe(SceneResourceArguments arguments,
                                                                         Ref<Widget> help) {
    // This transient adapter owns the same cold observer as the native recipe; no resource map escapes mount.
    auto entry = SceneResourceRegistry::makeEntry<KeyHints>("KeyHints", [help](SceneResourceArguments values) {
      return KeyHelpAccess::hints(help, controlKeyHintsOptions(values.settings, {}), {});
    });
    return entry.create(std::move(arguments));
  }

  /** @param registry Owning allowlist, unchanged during prepare.
   * @param index Index previously validated by find.
   * @return Immutable schema/factory borrowed only for the current synchronous operation.
   */
  static const auto &entry(const SceneResourceRegistry &registry, std::size_t index) {
    return registry.entries_[index];
  }
};
} // namespace detail

namespace {
using Code = SceneResourceErrorCode;

bool valid(const SceneResourceLimits &limits) {
  constexpr std::size_t MAX_RESOURCE_DEPTH = 64;

  return limits.maxSourceBytes && limits.maxNodes && limits.maxDepth && limits.maxDepth <= MAX_RESOURCE_DEPTH &&
         limits.maxStringBytes && limits.maxProperties;
}

void propertyPath(std::string &destination, std::string_view type, std::string_view property) {
  destination.assign(type);

  if (!destination.empty() && !property.empty()) {
    destination.push_back('.');
  }

  destination.append(property);
}

struct ResourceNode {
  // Suppress noexcept string/vector moves on the checked MSVC standard library.
  ~ResourceNode() = default;

  struct FactoryIdentity {
    detail::ControlFactoryKind kind{detail::ControlFactoryKind::USER};
    std::size_t index{};
  } factory;

  std::size_t type{};
  GridItemOptions gridItem;
  bool itemRequested{};
  bool regions{};
  bool headerDeclared{};
  bool bodyDeclared{};
  bool directChildren{};
  std::size_t directChildCount{};
  bool stickyHeader{};
  std::optional<GridPlacementPlan> gridPlan;
  DeclarativeGridOptions gridOptions;
  std::shared_ptr<Node<Grid>> gridNode;
  std::vector<std::size_t> gridChildren = std::vector<std::size_t>(0);
  std::size_t offset{};
  std::size_t textOffset{};
  bool resourceText{};
  bool hasText{};

  struct ThemeSource {
    ~ThemeSource() = default;
    ControlSettings instance;
    detail::ControlPropertyPresence supplied;
    detail::ControlPropertyPresence materialized;
  };

  std::optional<ThemeSource> themeSource;
  SceneResourceArguments arguments;
  std::string name = std::string(0, '\0');
  std::vector<std::string> lineage = std::vector<std::string>(0);
  std::optional<std::size_t> parent;
  std::optional<std::string> helpId;
  std::size_t helpOffset{};
  std::optional<std::size_t> helpTarget;
};

// Source ownership is shared with failures, so reporting never borrows parser buffers.
struct ResourceDocument {
  std::shared_ptr<const std::string> identity;
  std::string bytes = std::string(0, '\0');
  std::vector<ResourceNode> nodes = std::vector<ResourceNode>(0);
  std::vector<std::size_t> order = std::vector<std::size_t>(0);
  std::vector<std::size_t> lines = std::vector<std::size_t>(0);

  std::shared_ptr<const ControlSourceOrigin> origin(std::size_t offset) const {
    ControlSourceOrigin result;

    result.sharedSourceIdentity = identity;
    result.byteOffset = offset;

    const auto line = std::ranges::upper_bound(lines, offset);

    result.line = static_cast<std::size_t>(line - lines.begin());
    result.column = offset - *(line - 1) + 1;

    return std::make_shared<const ControlSourceOrigin>(std::move(result));
  }

  ManagedError failure(Code code, std::size_t offset, std::error_code system = {}, bool positioned = true) const {
    auto result = detail::error({}, TreeError::INVALID_ARGUMENT);
    SceneResourceError resource{code, identity, offset, 1, 1, system};

    for (std::size_t index = 0; index < std::min(offset, bytes.size()); ++index) {
      if (bytes[index] == '\n') {
        ++resource.line;
        resource.column = 1;
      } else {
        ++resource.column;
      }
    }

    if (!positioned) {
      resource.line = 0;
      resource.column = 0;
    }

    try {
      result.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
    } catch (const std::bad_alloc &) {
      // Diagnostic context must never replace the original typed failure under memory pressure.
    }

    return result;
  }

  ManagedError propertyFailure(Code code, std::size_t offset, std::string_view type, std::string_view path) const {
    auto result = failure(code, offset);

    try {
      if (result.sceneResource) {
        auto resource = *result.sceneResource;

        std::string fullPath(0, '\0');

        propertyPath(fullPath, type, path);
        resource.propertyPath = std::make_shared<const std::string>(fullPath);
        result.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
      }
    } catch (const std::bad_alloc &) {
      // Optional provenance must not replace the original typed failure.
    }

    return result;
  }
};

class Parser {
  using NodeStatus = std::expected<std::size_t, const ManagedError *>;
  using RegionStatus = std::expected<void, const ManagedError *>;

public:
  Parser(ResourceDocument &document, SceneResourceOptions &options, ControlContext &controls, bool typed)
      : document_(document), options_(options), controls_(controls), typed_(typed) {
  }

  std::expected<void, ManagedError> parse() {
    const auto &limits = options_.limits;

    if (!valid(limits)) {
      return std::unexpected(document_.failure(Code::INVALID_LIMIT, 0));
    }

    if (document_.bytes.size() > limits.maxSourceBytes ||
        document_.bytes.size() > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
      return std::unexpected(document_.failure(Code::SOURCE_LIMIT, 0));
    }

    document_.lines.push_back(0);

    // Validate source encoding separately: even ignored comments cannot hide invalid bytes.
    for (std::size_t offset = 0; offset < document_.bytes.size();) {
      utf8proc_int32_t scalar{};
      const auto length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(document_.bytes.data() + offset),
                                           static_cast<utf8proc_ssize_t>(document_.bytes.size() - offset), &scalar);

      if (length < 0) {
        return std::unexpected(document_.failure(Code::INVALID_UTF8, offset));
      }

      if (scalar == '\n') {
        document_.lines.push_back(offset + 1);
      }

      offset += static_cast<std::size_t>(length);
    }

    std::optional<std::size_t> sceneEnd;

    if (options_.format == SceneResourceFormat::CONFIGURATION) {
      auto configured = configuration();

      if (!configured) {
        return std::unexpected(configured.error());
      }

      position_ = configured->first;
      sceneEnd = configured->second;
    }

    auto fallback = detail::ResourceAccess::installGrid(options_.registry, controls_, typed_);
    if (!fallback) {
      return std::unexpected(contextFailure(fallback.error(), 0));
    }

    if (options_.format == SceneResourceFormat::CONFIGURATION) {
      auto primitives = detail::ResourceAccess::installPrimitives(options_.registry, controls_);

      if (!primitives) {
        return std::unexpected(contextFailure(primitives.error(), 0));
      }

      auto scrolls = detail::ResourceAccess::installScrolls(options_.registry, controls_);

      if (!scrolls) {
        return std::unexpected(contextFailure(scrolls.error(), 0));
      }
      auto keyHelp = detail::ResourceAccess::installKeyHelp(options_.registry, controls_);
      if (!keyHelp) {
        return std::unexpected(contextFailure(keyHelp.error(), 0));
      }
      auto dialogs = detail::ResourceAccess::installDialogs(options_.registry, controls_);

      if (!dialogs) {
        return std::unexpected(contextFailure(dialogs.error(), 0));
      }

      auto dataState = detail::ResourceAccess::installDataState(options_.registry, controls_);
      if (!dataState) {
        return std::unexpected(contextFailure(dataState.error(), 0));
      }
      auto overlay = detail::ResourceAccess::installOverlay(options_.registry, controls_);
      if (!overlay) {
        return std::unexpected(contextFailure(overlay.error(), 0));
      }
    }

    auto prepared = controls_.prepared();

    if (!prepared) {
      return std::unexpected(contextFailure(prepared.error(), 0));
    }

    controls_ = *prepared;

    auto root = node(1);

    if (!root) {
      return std::unexpected(*root.error());
    }

    skip();

    if (position_ != sceneEnd.value_or(document_.bytes.size())) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    // Views point only into the stationary owning document. Sorting validates all ids
    // without a hash container whose debug proxy constructor can be noexcept on MSVC.
    std::ranges::sort(ids_);

    for (std::size_t index = 1; index < ids_.size(); ++index) {
      if (ids_[index - 1].first == ids_[index].first) {
        return std::unexpected(document_.failure(Code::DUPLICATE_ID, ids_[index].second));
      }
    }

    return {};
  }

private:
  ManagedError withProperty(ManagedError failure, std::string_view type, std::string_view path) const {
    try {
      if (failure.sceneResource) {
        auto resource = *failure.sceneResource;

        std::string fullPath(0, '\0');

        propertyPath(fullPath, type, path);
        resource.propertyPath = std::make_shared<const std::string>(fullPath);
        failure.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
      }
    } catch (const std::bad_alloc &) {
      // Existing tree/text/callback errors remain authoritative.
    }

    return failure;
  }

  ManagedError contextFailure(const ControlContextError &cause, std::size_t fallback) const {
    auto failure = detail::controlContextError(cause);

    if (failure.sceneResource && !failure.sceneResource->source) {
      if (cause.diagnostic) {
        const auto path = std::string_view(cause.diagnostic->path);
        const auto type = path.substr(0, path.find('.'));
        const auto found = std::ranges::find(declarations_, type, [](const auto &entry) {
          return entry.first;
        });

        if (found != declarations_.end()) {
          fallback = found->second;
        }
      }

      const auto positioned = document_.failure(failure.sceneResource->code, fallback);

      if (!positioned.sceneResource) {
        return failure;
      }

      auto resource = *positioned.sceneResource;

      resource.propertyPath = failure.sceneResource->propertyPath;
      failure.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
    }

    return failure;
  }

  char peek() const {
    return position_ < document_.bytes.size() ? document_.bytes[position_] : '\0';
  }

  void skip() {
    while (position_ < document_.bytes.size()) {
      const auto value = peek();

      if (value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == ',' || value == ';') {
        ++position_;
      } else if (value == '/' && position_ + 1 < document_.bytes.size() && document_.bytes[position_ + 1] == '/') {
        while (position_ < document_.bytes.size() && peek() != '\n') {
          ++position_;
        }
      } else {
        break;
      }
    }
  }

  bool take(char value) {
    skip();

    if (peek() != value) {
      return false;
    }

    ++position_;

    return true;
  }

  std::string_view name(bool id = false) {
    skip();

    const auto begin = position_;

    if (!identifierStart(peek())) {
      return {};
    }

    ++position_;

    while (position_ < document_.bytes.size() && (identifierPart(peek()) || (id && peek() == '-'))) {
      ++position_;
    }

    return std::string_view(document_.bytes).substr(begin, position_ - begin);
  }

  std::string_view propertyName() {
    const auto first = name();

    if (first.empty()) {
      return first;
    }

    const auto begin = position_ - first.size();

    while (peek() == '.') {
      ++position_;

      if (!identifierStart(peek())) {
        return {};
      }

      ++position_;

      while (identifierPart(peek())) {
        ++position_;
      }
    }

    return std::string_view(document_.bytes).substr(begin, position_ - begin);
  }

  std::expected<void, ManagedError> string(std::string &result) {
    skip();

    const auto begin = position_;

    if (!take('"')) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    while (position_ < document_.bytes.size()) {
      char value = document_.bytes[position_++];

      if (value == '"') {
        return {};
      }

      if (static_cast<unsigned char>(value) < 0x20) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_ - 1));
      }

      if (value == '\\') {
        if (position_ == document_.bytes.size()) {
          break;
        }

        value = document_.bytes[position_++];

        switch (value) {
        case '"':
        case '\\':
          break;
        case 'n':
          value = '\n';
          break;
        case 'r':
          value = '\r';
          break;
        case 't':
          value = '\t';
          break;
        default:
          return std::unexpected(document_.failure(Code::SYNTAX, position_ - 1));
        }
      }

      if (result.size() == options_.limits.maxStringBytes) {
        return std::unexpected(document_.failure(Code::STRING_LIMIT, position_ - 1));
      }

      result.push_back(value);
    }

    return std::unexpected(document_.failure(Code::SYNTAX, begin));
  }

  std::expected<void, ManagedError> text(ResourceNode &target) {
    skip();
    target.textOffset = position_;
    target.hasText = true;

    if (peek() != '"') {
      if (name() != "i18n" || !take('(')) {
        return std::unexpected(document_.failure(Code::SYNTAX, target.textOffset));
      }

      target.resourceText = true;
    }

    auto value = string(target.arguments.text);

    if (!value) {
      return std::unexpected(value.error());
    }

    if (target.resourceText && !take(')')) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    return {};
  }

  std::expected<std::uint32_t, ManagedError> integer(bool positive) {
    skip();

    const auto begin = position_;

    while (peek() >= '0' && peek() <= '9') {
      ++position_;
    }

    std::uint32_t value{};
    const auto *first = document_.bytes.data() + begin;
    const auto *last = document_.bytes.data() + position_;
    const auto parsed = std::from_chars(first, last, value);

    if (parsed.ec != std::errc{} || parsed.ptr != last || first == last || (positive && value == 0) ||
        (!positive && value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
    }

    // Reject numeric expressions and suffixes rather than accepting a valid prefix.
    const char next = peek();

    if (next != '\0' && next != ' ' && next != '\t' && next != '\r' && next != '\n' && next != ',' && next != ';' &&
        next != '}') {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    return value;
  }

  template <class Unsigned = std::size_t> std::expected<Unsigned, ManagedError> capacity() {
    skip();

    const auto begin = position_;

    while (peek() >= '0' && peek() <= '9') {
      ++position_;
    }

    Unsigned value{};
    const auto *first = document_.bytes.data() + begin;
    const auto *last = document_.bytes.data() + position_;
    const auto parsed = std::from_chars(first, last, value);

    if (parsed.ec != std::errc{} || parsed.ptr != last || first == last) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
    }

    const char next = peek();

    if (next != '\0' && next != ' ' && next != '\t' && next != '\r' && next != '\n' && next != ',' && next != ';' &&
        next != '}') {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    return value;
  }

  std::expected<double, ManagedError> floating() {
    skip();

    const auto begin = position_;

    while (peek() != '\0' && peek() != ' ' && peek() != '\t' && peek() != '\r' && peek() != '\n' && peek() != ',' &&
           peek() != ';' && peek() != '}') {
      ++position_;
    }

    const auto *first = document_.bytes.data() + begin;
    const auto *last = document_.bytes.data() + position_;
    double value{};
    const auto parsed = std::from_chars(first, last, value, std::chars_format::general);

    if (parsed.ec != std::errc{} || first == last || !std::isfinite(value)) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
    }

    if (parsed.ptr != last) {
      return std::unexpected(document_.failure(Code::SYNTAX, begin + static_cast<std::size_t>(parsed.ptr - first)));
    }

    return value;
  }

  const ControlPropertyDescriptor *descriptor(std::string_view path) const {
    for (const auto type : {"Password",      "LineEdit",
                            "CheckBox",      "VBox",
                            "Button",        "Border",
                            "Gauge",         "ScrollArea",
                            "ScrollBar",     "Overlay",
                            "HelpView",      "KeyHints",
                            "VirtualList",   "VirtualTable",
                            "Chart",         "DataStateView",
                            "OverlayHost",   "Dialog",
                            "MessageBox",    "Notifications",
                            "ThemeSelector", "ThemeSelectorCatalog"}) {
      const auto schema = controlPropertySchema(type);
      const auto found = std::ranges::find(schema, path, &ControlPropertyDescriptor::path);

      if (found != schema.end()) {
        return &*found;
      }
    }

    const auto grid = detail::ControlContextAccess::gridSchema();
    const auto found = std::ranges::find(grid, path, &ControlPropertyDescriptor::path);
    return found == grid.end() ? nullptr : &*found;
  }

  std::expected<void, ManagedError> compactFrame(ControlSettings &settings, std::string_view type, std::size_t offset,
                                                 std::vector<std::string_view> &properties) {
    constexpr std::array<std::string_view, 2> LEAVES{"frame.enabled", "frame.pattern"};

    for (const auto path : LEAVES) {
      if (std::ranges::find(settings.origins, path, &ControlPropertyOrigin::path) != settings.origins.end()) {
        return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, offset, type, path));
      }

      const auto *schema = descriptor(path);

      if (!schema || schema->depth > controls_.limits().maxStructDepth) {
        return std::unexpected(document_.propertyFailure(Code::DEPTH_LIMIT, offset, type, path));
      }
    }

    // The compact spelling consumes the same two canonical leaves as explicit assignments.
    if (properties.size() > options_.limits.maxProperties ||
        LEAVES.size() > options_.limits.maxProperties - properties.size()) {
      return std::unexpected(document_.propertyFailure(Code::PROPERTY_LIMIT, offset, type, "frame.pattern"));
    }

    std::string source;
    auto parsed = string(source);

    if (!parsed) {
      return std::unexpected(withProperty(parsed.error(), type, "frame.pattern"));
    }

    const auto pattern = detail::framePattern(source);

    if (!pattern) {
      return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, "frame.pattern"));
    }

    for (const auto path : LEAVES) {
      properties.push_back(path);
      settings.origins.push_back({std::string(path), document_.origin(offset)});
    }

    settings.frame.enabled = true;
    settings.frame.pattern = *pattern;

    return {};
  }

  std::expected<void, ManagedError> setting(ControlSettings &settings, std::string_view type, std::string_view path,
                                            std::size_t offset) {
    const auto *schema = descriptor(path);

    if (!schema) {
      return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
    }

    if (schema->depth > controls_.limits().maxStructDepth) {
      return std::unexpected(document_.propertyFailure(Code::DEPTH_LIMIT, offset, type, path));
    }

    if (path.starts_with("frame.") &&
        std::ranges::find(settings.origins, path, &ControlPropertyOrigin::path) != settings.origins.end()) {
      return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, offset, type, path));
    }

    settings.origins.push_back({std::string(path), document_.origin(offset)});

    if (schema->kind == ControlPropertyKind::TEXT) {
      ResourceNode temporary;
      auto parsed = text(temporary);

      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, path));
      }

      auto value = temporary.resourceText ? i18n(temporary.arguments.text) : TextValue(temporary.arguments.text);

      if (path == "text") {
        settings.text = std::move(value);
      } else if (path == "placeholder") {
        settings.placeholder = std::move(value);
      } else if (path == "title") {
        settings.title = std::move(value);
      } else if (path == "frame.title") {
        settings.frame.title = std::move(value);
      } else if (path == "body") {
        settings.body = std::move(value);
      } else if (path == "themeSelector.labels.apply") {
        settings.themeSelector.labels.apply = std::move(value);
      } else if (path == "themeSelector.labels.cancel") {
        settings.themeSelector.labels.cancel = std::move(value);
      } else if (path == "themeSelector.labels.closeKeepingPreview") {
        settings.themeSelector.labels.closeKeepingPreview = std::move(value);
      } else if (path == "themeSelector.labels.emptyStatus") {
        settings.themeSelector.labels.emptyStatus = std::move(value);
      } else if (path == "themeSelector.labels.errorStatus") {
        settings.themeSelector.labels.errorStatus = std::move(value);
      } else if (path == "themeSelector.labels.conflictStatus") {
        settings.themeSelector.labels.conflictStatus = std::move(value);
      } else if (path == "message.labels.acknowledge") {
        settings.message.labels.acknowledge = std::move(value);
      } else if (path == "message.labels.confirm") {
        settings.message.labels.confirm = std::move(value);
      } else if (path == "message.labels.save") {
        settings.message.labels.save = std::move(value);
      } else if (path == "message.labels.discard") {
        settings.message.labels.discard = std::move(value);
      } else if (path == "message.labels.yes") {
        settings.message.labels.yes = std::move(value);
      } else if (path == "message.labels.no") {
        settings.message.labels.no = std::move(value);
      } else if (path == "message.labels.cancel") {
        settings.message.labels.cancel = std::move(value);
      } else if (path == "labels.loading") {
        settings.labels.loading = std::move(value);
      } else if (path == "labels.empty") {
        settings.labels.empty = std::move(value);
      } else if (path == "labels.error") {
        settings.labels.error = std::move(value);
      } else if (path == "labels.offline") {
        settings.labels.offline = std::move(value);
      } else if (path == "labels.refreshing") {
        settings.labels.refreshing = std::move(value);
      } else if (path == "labels.retry") {
        settings.labels.retry = std::move(value);
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::LITERAL_STRING) {
      std::string value;
      auto parsed = string(value);
      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, path));
      }
      if (path == "themeCatalog.initialId.value") {
        settings.themeCatalog.initialId.value = std::move(value);
      } else if (path == "themeSelector.openingShortcut.value") {
        settings.themeSelector.openingShortcut.value = std::move(value);
      } else if (path == "format.strokeSeparator") {
        settings.format.strokeSeparator = std::move(value);
      } else if (path == "format.alternativeSeparator") {
        settings.format.alternativeSeparator = std::move(value);
      } else if (path == "format.repeatMarker") {
        settings.format.repeatMarker = std::move(value);
      } else if (path == "format.releaseMarker") {
        settings.format.releaseMarker = std::move(value);
      } else if (path == "format.unknownMarker") {
        settings.format.unknownMarker = std::move(value);
      } else if (path == "frame.customTitleDecoration.prefix") {
        settings.frame.customTitleDecoration.prefix = std::move(value);
      } else if (path == "frame.customTitleDecoration.suffix") {
        settings.frame.customTitleDecoration.suffix = std::move(value);
      } else if (path == "frame.customGlyphs.topLeft") {
        settings.frame.customGlyphs.topLeft = std::move(value);
      } else if (path == "frame.customGlyphs.topRight") {
        settings.frame.customGlyphs.topRight = std::move(value);
      } else if (path == "frame.customGlyphs.bottomLeft") {
        settings.frame.customGlyphs.bottomLeft = std::move(value);
      } else if (path == "frame.customGlyphs.bottomRight") {
        settings.frame.customGlyphs.bottomRight = std::move(value);
      } else if (path == "frame.customGlyphs.horizontal") {
        settings.frame.customGlyphs.horizontal = std::move(value);
      } else if (path == "frame.customGlyphs.vertical") {
        settings.frame.customGlyphs.vertical = std::move(value);
      } else if (path == "emptyText") {
        settings.emptyText = std::move(value);
      } else if (path == "unavailableText") {
        settings.unavailableText = std::move(value);
      } else if (path == "entrySeparator") {
        settings.entrySeparator = std::move(value);
      } else if (path == "overflowMarker") {
        settings.overflowMarker = std::move(value);
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }
      return {};
    }

    if (schema->kind == ControlPropertyKind::BOOLEAN) {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
      }

      const bool enabled = value == "true";

      if (path == "securePolicy.lockPages") {
        settings.securePolicy.lockPages = enabled;
      } else if (path == "securePolicy.excludeFromDumps") {
        settings.securePolicy.excludeFromDumps = enabled;
      } else if (path == "securePolicy.encryptWhileIdle") {
        settings.securePolicy.encryptWhileIdle = enabled;
      } else if (path == "themeCatalog.initialId.enabled") {
        settings.themeCatalog.initialId.enabled = enabled;
      } else if (path == "themeCatalog.layers.enabled") {
        settings.themeCatalog.layers.enabled = enabled;
      } else if (path == "themeSelector.openingShortcut.enabled") {
        settings.themeSelector.openingShortcut.enabled = enabled;
      } else if (path == "notification.refreshDeduplicated") {
        settings.notification.refreshDeduplicated = enabled;
      } else if (path == "notification.reducedMotion") {
        settings.notification.reducedMotion = enabled;
      } else if (path == "message.defaultChoice.enabled") {
        settings.message.defaultChoice.enabled = enabled;
      } else if (path == "message.allowDestructiveDefault") {
        settings.message.allowDestructiveDefault = enabled;
      } else if (path == "showStatus") {
        settings.showStatus = enabled;
      } else if (path == "initiallyOpen") {
        settings.initiallyOpen = enabled;
      } else if (path == "scroll") {
        settings.grid.scroll = enabled;
      } else if (path == "readOnly") {
        settings.readOnly = enabled;
      } else if (path == "focusable") {
        settings.focusable = enabled;
      } else if (path == "frame.enabled") {
        settings.frame.enabled = enabled;
      } else if (path == "frame.sides.top") {
        settings.frame.sides.top = enabled;
      } else if (path == "frame.sides.right") {
        settings.frame.sides.right = enabled;
      } else if (path == "frame.sides.bottom") {
        settings.frame.sides.bottom = enabled;
      } else if (path == "frame.sides.left") {
        settings.frame.sides.left = enabled;
      } else if (path == "frame.customTitleDecoration.enabled") {
        settings.frame.customTitleDecoration.enabled = enabled;
      } else if (path == "frame.customGlyphs.enabled") {
        settings.frame.customGlyphs.enabled = enabled;
      } else if (path == "yRange.enabled") {
        settings.yRange.enabled = enabled;
      } else if (path == "history.enabled") {
        settings.history.enabled = enabled;
      } else if (path == "history.groupTyping") {
        settings.history.groupTyping = enabled;
      } else if (path == "history.groupDeletion") {
        settings.history.groupDeletion = enabled;
      } else if (path == "dialog.closeButton.enabled") {
        settings.dialog.closeButton.enabled = enabled;
      } else if (path == "dialog.closeButton.focusable") {
        settings.dialog.closeButton.focusable = enabled;
      } else if (path == "dialog.closeOnEscape") {
        settings.dialog.closeOnEscape = enabled;
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::ENUMERATION) {
      const auto value = name();
      const auto found = std::ranges::find(schema->enumNames, value);

      if (found == schema->enumNames.end()) {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
      }

      const auto index = found - schema->enumNames.begin();

      if (path == "securePolicy.storage") {
        settings.securePolicy.storage = static_cast<SecureStringStorage>(index);
      } else if (path == "pastePolicy") {
        settings.pastePolicy = static_cast<PasswordPastePolicy>(index);
      } else if (path == "state") {
        settings.state = static_cast<CheckState>(index);
      } else if (path == "cycle") {
        settings.cycle = static_cast<CheckCycle>(index);
      } else if (path == "editMode") {
        settings.editMode = static_cast<EditMode>(index);
      } else if (path == "programmaticChange") {
        settings.programmaticChange = static_cast<ProgrammaticChange>(index);
      } else if (path == "flow") {
        settings.grid.flow = static_cast<GridFlow>(index);
      } else if (path == "mainAlign") {
        settings.linear.mainAlign = static_cast<LayoutAlign>(index);
      } else if (path == "crossAlign") {
        settings.linear.crossAlign = static_cast<LayoutAlign>(index);
      } else if (path == "overflow") {
        settings.linear.overflow = static_cast<LayoutOverflow>(index);
      } else if (path == "chrome") {
        settings.chrome = static_cast<ButtonChrome>(index);
      } else if (path == "glyphs") {
        settings.glyphs = static_cast<PrimitiveGlyphs>(index);
      } else if (path == "frame.glyphs") {
        settings.frame.glyphs = static_cast<PrimitiveGlyphs>(index);
      } else if (path == "frame.pattern") {
        constexpr std::array PATTERNS{FramePattern::SINGLE, FramePattern::SINGLE, FramePattern::ROUNDED,
                                      FramePattern::DOUBLE, FramePattern::HEAVY,  FramePattern::ASCII};

        settings.frame.pattern = PATTERNS[static_cast<std::size_t>(index)];
      } else if (path == "frame.titleDecoration") {
        settings.frame.titleDecoration = static_cast<TitleDecoration>(index);
      } else if (path == "frame.titleAlignment") {
        settings.frame.titleAlignment = static_cast<FrameTitleAlignment>(index);
      } else if (path == "horizontal") {
        settings.horizontal = static_cast<ScrollBarPolicy>(index);
      } else if (path == "vertical") {
        settings.vertical = static_cast<ScrollBarPolicy>(index);
      } else if (path == "placement") {
        settings.placement = static_cast<ScrollPlacement>(index);
      } else if (path == "axis") {
        settings.axis = static_cast<ScrollAxis>(index);
      } else if (path == "horizontalAlign") {
        settings.horizontalAlign = static_cast<LayoutAlign>(index);
      } else if (path == "verticalAlign") {
        settings.verticalAlign = static_cast<LayoutAlign>(index);
      } else if (path == "contentPolicy") {
        settings.contentPolicy = static_cast<DataContentPolicy>(index);
      } else if (path == "mode") {
        settings.mode = static_cast<ChartMode>(index);
      } else if (path == "notification.placement") {
        settings.notification.placement = static_cast<ToastPlacement>(index);
      } else if (path == "notification.overflow") {
        settings.notification.overflow = static_cast<ToastOverflowPolicy>(index);
      } else if (path == "message.buttons") {
        settings.message.buttons = static_cast<DialogButtons>(index);
      } else if (path == "message.order.first") {
        settings.message.order.first = static_cast<MessageChoice>(index);
      } else if (path == "message.order.second") {
        settings.message.order.second = static_cast<MessageChoice>(index);
      } else if (path == "message.order.third") {
        settings.message.order.third = static_cast<MessageChoice>(index);
      } else if (path == "dialog.closeButton.placement") {
        settings.dialog.closeButton.placement = static_cast<DialogClosePlacement>(index);
      } else if (path == "message.defaultChoice.value") {
        settings.message.defaultChoice.value = static_cast<MessageChoice>(index);
      } else if (path == "dialog.mode") {
        settings.dialog.mode = static_cast<DialogMode>(index);
      } else if (path == "dialog.placement") {
        settings.dialog.placement = static_cast<DialogPlacement>(index);
      } else if (path == "dialog.frame.glyphs") {
        settings.dialog.frame.glyphs = static_cast<PrimitiveGlyphs>(index);
      } else if (path == "chartGlyphSet") {
        settings.chartGlyphSet = static_cast<ChartGlyphSet>(index);
      } else if (path == "displayMode") {
        settings.displayMode = static_cast<LineEditDisplayMode>(index);
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::FLOATING_POINT) {
      auto value = floating();

      if (!value) {
        return std::unexpected(withProperty(value.error(), type, path));
      }

      if (path == "value") {
        settings.value = *value;
      } else if (path == "minimum") {
        settings.minimum = *value;
      } else if (path == "maximum") {
        settings.maximum = *value;
      } else if (path == "yRange.minimum") {
        settings.yRange.minimum = *value;
      } else if (path == "yRange.maximum") {
        settings.yRange.maximum = *value;
      } else if (path == "wheelStep") {
        settings.wheelStep = *value;
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (path == "keyStep" || path == "pageOverlap" || path == "spinnerInterval") {
      const auto value = capacity<std::uint64_t>();

      if (!value) {
        return std::unexpected(withProperty(value.error(), type, path));
      }

      if (path == "spinnerInterval") {
        settings.spinnerInterval = *value;
      } else if (path == "keyStep") {
        settings.keyStep = *value;
      } else {
        settings.pageOverlap = *value;
      }

      return {};
    }

    auto value = capacity();

    if (!value) {
      return std::unexpected(withProperty(value.error(), type, path));
    }

    if ((schema->kind == ControlPropertyKind::SIGNED_INTEGER &&
         *value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) ||
        (path == "flex" && (*value == 0 || *value > std::numeric_limits<std::uint32_t>::max()))) {
      return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
    }

    if (path == "securePolicy.maxBytes") {
      settings.securePolicy.maxBytes = *value;
    } else if (path == "maxLayers") {
      settings.maxLayers = *value;
    } else if (path == "dialog.maximumPanelSize.width") {
      settings.dialog.maximumPanelSize.width = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.maximumPanelSize.height") {
      settings.dialog.maximumPanelSize.height = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.viewportMargin") {
      settings.dialog.viewportMargin = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.anchorGap") {
      settings.dialog.anchorGap = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.frame.maxPaintCells") {
      settings.dialog.frame.maxPaintCells = *value;
    } else if (path == "width") {
      settings.width = static_cast<std::int32_t>(*value);
    } else if (path == "height") {
      settings.height = static_cast<std::int32_t>(*value);
    } else if (path == "gap") {
      settings.gap = static_cast<std::int32_t>(*value);
    } else if (path == "flex") {
      settings.flex = static_cast<std::uint32_t>(*value);
    } else if (path == "format.maxBytesPerBinding") {
      settings.format.maxBytesPerBinding = *value;
    } else if (path == "rowHeight") {
      settings.rowHeight = static_cast<std::int32_t>(*value);
    } else if (path == "statusRows") {
      settings.statusRows = static_cast<std::int32_t>(*value);
    } else if (path == "candleGap") {
      settings.candleGap = static_cast<std::int32_t>(*value);
    } else if (path == "overscanRows") {
      settings.overscanRows = *value;
    } else if (path == "maxColumns") {
      settings.maxColumns = *value;
    } else if (path == "maxPreparedCells") {
      settings.maxPreparedCells = *value;
    } else if (path == "maxPreparedTextBytes") {
      settings.maxPreparedTextBytes = *value;
    } else if (path == "notification.capacity") {
      settings.notification.capacity = *value;
    } else if (path == "notification.maxVisible") {
      settings.notification.maxVisible = *value;
    } else if (path == "notification.maximumWidth") {
      settings.notification.maximumWidth = static_cast<std::int32_t>(*value);
    } else if (path == "notification.margin") {
      settings.notification.margin = static_cast<std::int32_t>(*value);
    } else if (path == "notification.gap") {
      settings.notification.gap = static_cast<std::int32_t>(*value);
    } else if (path == "themeCatalog.maxChoices") {
      settings.themeCatalog.maxChoices = *value;
    } else if (path == "themeCatalog.maxIdBytes") {
      settings.themeCatalog.maxIdBytes = *value;
    } else if (path == "themeCatalog.maxLabelBytes") {
      settings.themeCatalog.maxLabelBytes = *value;
    } else if (path == "themeCatalog.maxDescriptionBytes") {
      settings.themeCatalog.maxDescriptionBytes = *value;
    } else if (path == "themeCatalog.maxTextBytes") {
      settings.themeCatalog.maxTextBytes = *value;
    } else if (path == "themeCatalog.textLimits.maxInputBytes") {
      settings.themeCatalog.textLimits.maxInputBytes = *value;
    } else if (path == "themeCatalog.textLimits.maxClusterBytes") {
      settings.themeCatalog.textLimits.maxClusterBytes = *value;
    } else if (path == "themeCatalog.textLimits.maxGraphemes") {
      settings.themeCatalog.textLimits.maxGraphemes = *value;
    } else if (path == "themeCatalog.textLimits.maxCells") {
      settings.themeCatalog.textLimits.maxCells = *value;
    } else if (path == "themeCatalog.layers.maxLayers") {
      settings.themeCatalog.layers.maxLayers = *value;
    } else if (path == "themeCatalog.layers.maxRules") {
      settings.themeCatalog.layers.maxRules = *value;
    } else if (path == "themeCatalog.layers.maxDeclarations") {
      settings.themeCatalog.layers.maxDeclarations = *value;
    } else if (path == "themeCatalog.layers.maxOwnedBytes") {
      settings.themeCatalog.layers.maxOwnedBytes = *value;
    } else if (path == "themeSelector.textLimits.maxInputBytes") {
      settings.themeSelector.textLimits.maxInputBytes = *value;
    } else if (path == "themeSelector.textLimits.maxClusterBytes") {
      settings.themeSelector.textLimits.maxClusterBytes = *value;
    } else if (path == "themeSelector.textLimits.maxGraphemes") {
      settings.themeSelector.textLimits.maxGraphemes = *value;
    } else if (path == "themeSelector.textLimits.maxCells") {
      settings.themeSelector.textLimits.maxCells = *value;
    } else if (path == "notification.textLimits.maxInputBytes") {
      settings.notification.textLimits.maxInputBytes = *value;
    } else if (path == "notification.textLimits.maxClusterBytes") {
      settings.notification.textLimits.maxClusterBytes = *value;
    } else if (path == "notification.textLimits.maxGraphemes") {
      settings.notification.textLimits.maxGraphemes = *value;
    } else if (path == "notification.textLimits.maxCells") {
      settings.notification.textLimits.maxCells = *value;
    } else if (path == "notification.actionLabelBytes") {
      settings.notification.actionLabelBytes = *value;
    } else if (path == "notification.dedupKeyBytes") {
      settings.notification.dedupKeyBytes = *value;
    } else if (path == "notification.maxPaintCells") {
      settings.notification.maxPaintCells = *value;
    } else if (path == "message.maxBodyLines") {
      settings.message.maxBodyLines = *value;
    } else if (path == "message.textLimits.maxInputBytes") {
      settings.message.textLimits.maxInputBytes = *value;
    } else if (path == "message.textLimits.maxClusterBytes") {
      settings.message.textLimits.maxClusterBytes = *value;
    } else if (path == "message.textLimits.maxGraphemes") {
      settings.message.textLimits.maxGraphemes = *value;
    } else if (path == "message.textLimits.maxCells") {
      settings.message.textLimits.maxCells = *value;
    } else if (path == "history.maxOperations") {
      settings.history.maxOperations = *value;
    } else if (path == "history.maxBytes") {
      settings.history.maxBytes = *value;
    } else if (path == "limits.maxInputBytes") {
      settings.limits.maxInputBytes = *value;
    } else if (path == "limits.maxClusterBytes") {
      settings.limits.maxClusterBytes = *value;
    } else if (path == "limits.maxGraphemes") {
      settings.limits.maxGraphemes = *value;
    } else if (path == "limits.maxCells") {
      settings.limits.maxCells = *value;
    } else if (path == "maxPaintCells") {
      settings.maxPaintCells = *value;
    } else if (path == "preferredWidth") {
      settings.preferredWidth = static_cast<std::int32_t>(*value);
    } else if (path == "minimumThumb") {
      settings.minimumThumb = static_cast<std::int32_t>(*value);
    } else if (path == "frame.maxPaintCells") {
      settings.frame.maxPaintCells = *value;
    } else if (path == "frame.maxTitleBytes") {
      settings.frame.maxTitleBytes = *value;
    } else if (path == "columns") {
      settings.grid.columns = *value;
    } else if (path == "rows") {
      settings.grid.rows = *value;
    } else if (path == "columnGap") {
      settings.grid.columnGap = static_cast<std::int32_t>(*value);
    } else if (path == "rowGap") {
      settings.grid.rowGap = static_cast<std::int32_t>(*value);
    } else if (path == "padding.left") {
      settings.grid.paddingLeft = static_cast<std::int32_t>(*value);
    } else if (path == "padding.top") {
      settings.grid.paddingTop = static_cast<std::int32_t>(*value);
    } else if (path == "padding.right") {
      settings.grid.paddingRight = static_cast<std::int32_t>(*value);
    } else if (path == "padding.bottom") {
      settings.grid.paddingBottom = static_cast<std::int32_t>(*value);
    } else if (path == "maxTracksPerAxis") {
      settings.grid.maxTracksPerAxis = *value;
    } else if (path == "maxPlacements") {
      settings.grid.maxPlacements = *value;
    } else if (path == "maxPlacementWork") {
      settings.grid.maxPlacementWork = *value;
    } else {
      return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
    }

    return {};
  }

  std::expected<void, ManagedError> settingsBody(ControlSettings &settings, std::string_view type,
                                                 std::string_view prefix = {}, std::size_t depth = 1) {
    if (depth > controls_.limits().maxStructDepth) {
      return std::unexpected(document_.propertyFailure(Code::DEPTH_LIMIT, position_, type, prefix));
    }

    std::vector<std::string_view> properties(0);

    while (!take('}')) {
      skip();

      const auto begin = position_;
      const auto key = propertyName();
      const auto canonicalKey = detail::ControlContextAccess::canonicalProperty(key);
      std::string path(0, '\0');

      propertyPath(path, prefix, canonicalKey);

      if (key.empty()) {
        return std::unexpected(document_.propertyFailure(Code::SYNTAX, begin, type, path));
      }

      const bool assigned = take('=') || take(':');

      skip();

      if (path == "frame" && options_.format == SceneResourceFormat::CONFIGURATION && assigned && peek() != '{') {
        auto parsed = compactFrame(settings, type, begin, properties);

        if (!parsed) {
          return parsed;
        }

        continue;
      }

      if (properties.size() == options_.limits.maxProperties) {
        return std::unexpected(document_.propertyFailure(Code::PROPERTY_LIMIT, begin, type, path));
      }

      if (std::ranges::find(properties, canonicalKey) != properties.end()) {
        return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, begin, type, path));
      }

      properties.push_back(canonicalKey);

      if (take('{')) {
        const bool configurationStruct =
          options_.format == SceneResourceFormat::CONFIGURATION &&
          (path == "securePolicy" || path == "limits" || path == "frame" || path == "frame.sides" ||
           path == "frame.customGlyphs" || path == "frame.customTitleDecoration" || path == "format" ||
           path == "yRange" || path == "labels" || path == "dialog" || path == "dialog.closeButton" ||
           path == "dialog.frame" || path == "dialog.maximumPanelSize" || path == "message" ||
           path == "message.order" || path == "message.labels" || path == "message.defaultChoice" ||
           path == "message.textLimits" || path == "notification" || path == "notification.textLimits" ||
           path == "themeSelector" || path == "themeSelector.labels" || path == "themeSelector.textLimits" ||
           path == "themeSelector.openingShortcut" || path == "themeCatalog" || path == "themeCatalog.textLimits" ||
           path == "themeCatalog.initialId" || path == "themeCatalog.layers");

        if (path != "history" && path != "padding" && !configurationStruct) {
          return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, begin, type, path));
        }

        auto parsed = settingsBody(settings, type, path, depth + 1);

        if (!parsed) {
          return parsed;
        }
      } else {
        if (!assigned) {
          return std::unexpected(document_.propertyFailure(Code::SYNTAX, begin, type, path));
        }

        auto parsed = setting(settings, type, path, begin);

        if (!parsed) {
          return parsed;
        }
      }
    }

    return {};
  }

  // Locate structural braces without decoding or allocating string values. The typed
  // property parser owns string validation and budgets, with the original property path.
  std::expected<void, ManagedError> skipQuotedRange() {
    const auto begin = position_++;

    while (position_ < document_.bytes.size()) {
      const auto value = document_.bytes[position_++];

      if (value == '"') {
        return {};
      }

      if (value == '\\' && position_ < document_.bytes.size()) {
        ++position_;
      }
    }

    return std::unexpected(document_.failure(Code::SYNTAX, begin));
  }

  std::expected<std::pair<std::size_t, std::size_t>, ManagedError> configuration() {
    struct Block {
      std::string_view kind;
      std::size_t begin;
      std::size_t end;
    };

    std::vector<Block> blocks(0);
    std::optional<std::pair<std::size_t, std::size_t>> scene;

    // Scan source ranges without rewriting bytes: inherited diagnostics keep original positions.
    while (true) {
      skip();

      if (position_ == document_.bytes.size()) {
        break;
      }

      const auto offset = position_;
      const auto kind = name();

      if ((kind != "definitions" && kind != "defaults" && kind != "scene") || !take('{')) {
        return std::unexpected(document_.failure(Code::SYNTAX, offset));
      }

      const auto begin = position_;
      std::size_t braces{1};

      while (braces) {
        skip();

        if (position_ == document_.bytes.size()) {
          return std::unexpected(document_.failure(Code::SYNTAX, position_));
        }

        if (peek() == '"') {
          auto parsed = skipQuotedRange();

          if (!parsed) {
            return std::unexpected(parsed.error());
          }
        } else {
          const auto value = document_.bytes[position_++];

          if (value == '{') {
            ++braces;
          } else if (value == '}') {
            --braces;
          }
        }
      }

      const auto end = position_ - 1;

      if (kind == "scene") {
        if (scene) {
          return std::unexpected(document_.failure(Code::DUPLICATE_PROPERTY, offset));
        }

        scene = std::pair{begin, end};
      } else {
        blocks.push_back({kind, begin, end});
      }
    }

    if (!scene) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    for (const auto &block : blocks) {
      position_ = block.begin;
      skip();

      while (position_ < block.end) {
        const auto offset = position_;
        const auto type = name();
        auto base = type;

        declarations_.emplace_back(type, offset);

        if (type.empty() || (block.kind == "definitions" && !take('='))) {
          return std::unexpected(document_.failure(Code::SYNTAX, offset));
        }

        if (block.kind == "definitions") {
          skip();

          const auto baseOffset = position_;

          base = name();
          declarations_.emplace_back(base, baseOffset);
        }

        if (base.empty() || !take('{')) {
          return std::unexpected(document_.failure(Code::SYNTAX, position_));
        }

        ControlSettings settings;

        settings.origin = document_.origin(offset);

        auto parsed = settingsBody(settings, type);

        if (!parsed) {
          return std::unexpected(parsed.error());
        }

        if (block.kind == "definitions") {
          controls_.define(type, base, settings);
        } else {
          if (std::ranges::find(defaults_, type) != defaults_.end()) {
            return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, offset, type, {}));
          }

          defaults_.push_back(type);
          controls_.overlayDefaults(type, settings);
        }

        skip();
      }

      if (position_ != block.end) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_));
      }
    }

    return *scene;
  }

  std::expected<void, ManagedError> property(ResourceNode &target, std::string_view key, std::size_t offset) {
    const auto &schema = detail::ResourceAccess::entry(options_.registry, target.type);

    if (schema.factoryKind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      return setting(target.arguments.settings, target.name, key, offset);
    }

    if (key == "text" && schema.text) {
      auto parsed = text(target);

      if (parsed && typed_) {
        target.arguments.settings.text =
          target.resourceText ? i18n(target.arguments.text) : TextValue(target.arguments.text);
      }

      return parsed;
    }

    if (key == "checked" && schema.checked) {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.failure(Code::INVALID_VALUE, offset));
      }

      target.arguments.checked = value == "true";
      target.arguments.settings.state = target.arguments.checked ? CheckState::CHECKED : CheckState::UNCHECKED;

      return {};
    }

    const bool sized = schema.sized && (key == "width" || key == "height" || key == "flex");

    if (!sized && !(key == "gap" && schema.gap)) {
      return std::unexpected(document_.failure(Code::UNKNOWN_PROPERTY, offset));
    }

    auto value = integer(key == "flex");

    if (!value) {
      return std::unexpected(value.error());
    }

    if (key == "width") {
      target.arguments.width = static_cast<std::int32_t>(*value);
      target.arguments.settings.width = target.arguments.width;
    } else if (key == "height") {
      target.arguments.height = static_cast<std::int32_t>(*value);
      target.arguments.settings.height = target.arguments.height;
    } else if (key == "flex") {
      target.arguments.flex = *value;
      target.arguments.settings.flex = target.arguments.flex;
    } else {
      target.arguments.gap = static_cast<std::int32_t>(*value);
      target.arguments.settings.gap = target.arguments.gap;
    }

    return {};
  }

  std::expected<void, ManagedError> itemProperty(ResourceNode &target, std::string_view key, std::size_t offset) {
    target.itemRequested = true;
    if (key == "horizontal" || key == "vertical") {
      constexpr std::array<std::string_view, 4> ALIGN_NAMES{"START", "CENTER", "END", "STRETCH"};
      const auto value = name();
      const auto found = std::ranges::find(ALIGN_NAMES, value);
      if (found == ALIGN_NAMES.end()) {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, target.name, key));
      }

      auto &alignment = key == "horizontal" ? target.gridItem.horizontal : target.gridItem.vertical;
      alignment = static_cast<LayoutAlign>(found - ALIGN_NAMES.begin());

      return {};
    }

    auto value = capacity();
    if (!value) {
      return std::unexpected(withProperty(value.error(), target.name, key));
    }

    if (key == "row") {
      target.gridItem.row = *value;
    } else if (key == "column") {
      target.gridItem.column = *value;
    } else if (key == "rowSpan") {
      target.gridItem.rowSpan = *value;
    } else {
      target.gridItem.columnSpan = *value;
    }

    return {};
  }

  RegionStatus region(std::size_t parent, GridRegion kind, std::size_t depth, std::size_t offset) {
    const bool declared =
      kind == GridRegion::HEADER ? document_.nodes[parent].headerDeclared : document_.nodes[parent].bodyDeclared;
    if (declared || document_.nodes[parent].directChildren) {
      return parseFailure(Code::INVALID_VALUE, offset);
    }

    if (kind == GridRegion::HEADER) {
      document_.nodes[parent].headerDeclared = true;
    } else {
      document_.nodes[parent].bodyDeclared = true;
    }

    document_.nodes[parent].regions = true;
    if (!take('{')) {
      return parseFailure(Code::SYNTAX, position_);
    }

    bool stickyDeclared{};
    while (!take('}')) {
      skip();
      const auto begin = position_;
      const auto token = name();
      const bool assigned = take(':') || take('=');
      if (assigned) {
        if (kind != GridRegion::HEADER || token != "sticky") {
          return parseFailure(Code::UNKNOWN_PROPERTY, begin, document_.nodes[parent].name, token);
        }

        if (stickyDeclared) {
          return parseFailure(Code::DUPLICATE_PROPERTY, begin, document_.nodes[parent].name, "header.sticky");
        }

        stickyDeclared = true;
        const auto value = name();
        if (value != "true" && value != "false") {
          return parseFailure(Code::INVALID_VALUE, begin, document_.nodes[parent].name, "header.sticky");
        }

        document_.nodes[parent].stickyHeader = value == "true";
      } else {
        position_ = begin;
        // Structural groups do not consume a widget-node depth or identity.
        auto child = node(depth);
        if (!child) {
          return std::unexpected(child.error());
        }

        document_.nodes[*child].parent = parent;
        document_.nodes[*child].gridItem.region = kind;
      }
    }

    return {};
  }

  std::expected<void, ManagedError> resolveType(std::string_view type, std::size_t offset,
                                                std::optional<std::size_t> &registered) {
    const auto resolved = controls_.resolve(type);

    if (!resolved) {
      return std::unexpected(contextFailure(resolved.error(), offset));
    }

    registered = detail::ResourceAccess::find(options_.registry, resolved->base);

    return {};
  }

  bool acceptsNativeProperty(const ResourceNode &target, detail::ControlFactoryKind kind, std::string_view path) const {
    if (options_.format != SceneResourceFormat::CONFIGURATION ||
        (target.factory.kind != kind && !(kind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING &&
                                          target.factory.kind == detail::ControlFactoryKind::THEME_CATALOG_BINDING))) {
      return false;
    }

    // beginNode has already resolved aliases to this concrete factory; caller-owned names never enter this branch.
    const auto &concrete = detail::ResourceAccess::entry(options_.registry, target.factory.index);
    const auto semantic =
      concrete.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING          ? "MessageBox"
      : concrete.factoryKind == detail::ControlFactoryKind::NOTIFICATION_BINDING   ? "Notifications"
      : concrete.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING  ? "ThemeSelectorCatalog"
      : concrete.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ? "ThemeSelector"
                                                                                   : std::string_view(concrete.name);
    const auto schema = controlPropertySchema(semantic);

    return std::ranges::any_of(schema, [path](const auto &property) {
      return property.path == path;
    });
  }

  bool acceptsFrameSettings(const ResourceNode &target) const {
    return options_.format == SceneResourceFormat::CONFIGURATION &&
           detail::ResourceAccess::entry(options_.registry, target.factory.index).sized;
  }

  bool acceptsChartRange(const ResourceNode &target) const {
    if (options_.format != SceneResourceFormat::CONFIGURATION) {
      return false;
    }

    const auto &concrete = detail::ResourceAccess::entry(options_.registry, target.factory.index);
    constexpr std::string_view NARROW[]{"text"};

    // Exact code-bound charts opt in through their expanded schema; old explicit allowlists stay narrow.
    return concrete.virtualView == "Chart" && !controls_.matchesRegisteredType(concrete.name, NARROW);
  }

  // Header/property error temporaries must not remain in every bounded recursive node frame.
  std::expected<void, ManagedError> nodeProperty(std::size_t index, std::string_view token, bool positional,
                                                 std::size_t begin, std::vector<std::string_view> &properties) {
    auto &target = document_.nodes[index];
    const auto type = std::string_view(target.name);
    auto key = positional ? std::string_view("text") : token;
    const bool registered = target.factory.kind == detail::ControlFactoryKind::USER ||
                            target.factory.kind == detail::ControlFactoryKind::EXISTING_BUILTIN;
    const bool protectedEditor =
      registered && detail::ResourceAccess::entry(options_.registry, target.factory.index).protectedEditor;

    if (options_.format == SceneResourceFormat::CONFIGURATION || protectedEditor) {
      key = detail::ControlContextAccess::canonicalProperty(key);
    }

    skip();

    if (key == "frame" && options_.format == SceneResourceFormat::CONFIGURATION && peek() != '{') {
      return compactFrame(target.arguments.settings, type, begin, properties);
    }

    if (properties.size() == options_.limits.maxProperties) {
      return std::unexpected(document_.failure(Code::PROPERTY_LIMIT, begin));
    }

    if (std::ranges::find(properties, key) != properties.end()) {
      return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, begin, type, key));
    }

    properties.push_back(key);

    if (positional) {
      position_ = begin;
    }

    if (protectedEditor && key == "text") {
      skip();

      // Admit only an exact empty literal, without decoding or retaining a rejected value.
      if (document_.bytes.compare(position_, 2, "\"\"") != 0) {
        return std::unexpected(document_.propertyFailure(Code::SECRET_LITERAL, begin, type, "text"));
      }
    }

    if (protectedEditor && (key == "securePolicy" || key == "limits") && take('{')) {
      return settingsBody(target.arguments.settings, type, key, 2);
    }

    if (protectedEditor && options_.format != SceneResourceFormat::CONFIGURATION && key != "text" && key != "width" &&
        key != "height" && key != "flex" && key != "classes" && key != "visible" && key != "enabled") {
      const auto schema = controlPropertySchema("Password");

      if (std::ranges::find(schema, key, &ControlPropertyDescriptor::path) == schema.end()) {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, begin, type, key));
      }

      return setting(target.arguments.settings, type, key, begin);
    }

    if (key == "help" &&
        acceptsNativeProperty(target, detail::ControlFactoryKind::KEY_HELP_FALLBACK, "entrySeparator")) {
      std::string id;
      auto parsed = string(id);
      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, key));
      }
      target.helpId = std::move(id);
      target.helpOffset = begin;
      return {};
    }

    if (key == "classes") {
      std::string value(0, '\0');
      auto parsed = string(value);

      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, key));
      }

      target.arguments.classes = std::move(value);

      return {};
    }

    if (key == "visible" || key == "enabled") {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, begin, type, key));
      }

      auto &flag = key == "visible" ? target.arguments.visible : target.arguments.enabled;

      flag = value == "true";

      return {};
    }

    if ((key == "horizontal" || key == "vertical") &&
        acceptsNativeProperty(target, detail::ControlFactoryKind::SCROLL_FALLBACK, key)) {
      return setting(target.arguments.settings, type, key, begin);
    }

    if (key == "row" || key == "column" || key == "rowSpan" || key == "columnSpan" || key == "horizontal" ||
        key == "vertical") {
      return itemProperty(target, key, begin);
    }

    if (key == "padding" && take('{')) {
      const auto firstOrigin = target.arguments.settings.origins.size();
      auto parsed = settingsBody(target.arguments.settings, type, "padding", 2);

      if (parsed && typed_ && options_.format != SceneResourceFormat::CONFIGURATION &&
          target.factory.kind == detail::ControlFactoryKind::EXISTING_BUILTIN &&
          target.arguments.settings.origins.size() != firstOrigin) {
        const auto &concrete = detail::ResourceAccess::entry(options_.registry, target.factory.index).name;

        if (concrete == "VBox" || concrete == "HBox") {
          const auto &origin = target.arguments.settings.origins[firstOrigin];

          return std::unexpected(
            document_.propertyFailure(Code::UNKNOWN_PROPERTY, origin.source->byteOffset, type, origin.path));
        }
      }

      return parsed;
    }

    if (options_.format != SceneResourceFormat::CONFIGURATION) {
      auto parsed = property(target, key, begin);

      if (parsed && typed_ && target.factory.kind != detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
        target.arguments.settings.origins.push_back(
          {std::string(key == "checked" ? "state" : key), document_.origin(begin)});
      }

      return parsed;
    }

    if ((key == "history" || key == "limits" ||
         (key == "format" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::KEY_HELP_FALLBACK, "format.maxBytesPerBinding")) ||
         (key == "frame" && acceptsFrameSettings(target)) || (key == "yRange" && acceptsChartRange(target)) ||
         (key == "labels" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::DATA_STATE_FALLBACK, "labels.loading")) ||
         (key == "themeCatalog" && acceptsNativeProperty(target, detail::ControlFactoryKind::THEME_CATALOG_BINDING,
                                                         "themeCatalog.maxChoices")) ||
         (key == "themeSelector" && acceptsNativeProperty(target, detail::ControlFactoryKind::THEME_SELECTOR_BINDING,
                                                          "themeSelector.labels.apply")) ||
         (key == "notification" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::NOTIFICATION_BINDING, "notification.capacity")) ||
         (key == "message" && acceptsNativeProperty(target, detail::ControlFactoryKind::MESSAGE_BINDING, "body")) ||
         (key == "dialog" &&
          (acceptsNativeProperty(target, detail::ControlFactoryKind::DIALOG_FALLBACK, "dialog.mode") ||
           acceptsNativeProperty(target, detail::ControlFactoryKind::MESSAGE_BINDING, "dialog.mode") ||
           acceptsNativeProperty(target, detail::ControlFactoryKind::THEME_SELECTOR_BINDING, "dialog.mode")))) &&
        take('{')) {
      return settingsBody(target.arguments.settings, type, key, 2);
    }

    return setting(target.arguments.settings, type, key, begin);
  }

  std::expected<std::size_t, ManagedError> beginNode(std::size_t depth) {
    skip();

    const auto offset = position_;

    if (depth > options_.limits.maxDepth) {
      return std::unexpected(document_.failure(Code::DEPTH_LIMIT, offset));
    }

    if (document_.nodes.size() == options_.limits.maxNodes) {
      return std::unexpected(document_.failure(Code::NODE_LIMIT, offset));
    }

    const auto type = name();
    auto registered = detail::ResourceAccess::find(options_.registry, type);

    if (typed_ && !type.empty()) {
      const auto checked = resolveType(type, offset, registered);

      if (!checked) {
        return std::unexpected(checked.error());
      }
    }

    if (type.empty() || !registered) {
      return std::unexpected(document_.failure(type.empty() ? Code::SYNTAX : Code::UNKNOWN_TYPE, offset));
    }

    const auto index = document_.nodes.size();

    document_.nodes.emplace_back();

    {
      auto &target = document_.nodes[index];

      target.type = *registered;
      target.factory = {detail::ResourceAccess::entry(options_.registry, *registered).factoryKind, *registered};
      target.offset = offset;
      target.name = type;

      if (typed_) {
        target.arguments.settings.origin = document_.origin(offset);
      }

      if (take('#')) {
        skip();
        const auto idOffset = position_;
        const auto id = name(true);

        if (id.empty()) {
          return std::unexpected(document_.failure(Code::SYNTAX, idOffset));
        }

        target.arguments.id = id;

        ids_.emplace_back(id, idOffset);
      }

      if (!take('{')) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_));
      }
    }

    return index;
  }

  // Recursive node/region frames retain only small statuses. The parser owns the
  // first full error until parse() copies it at the public boundary; no heap or
  // caller-owned pointer is required, even when a new typed cause grows.
  std::unexpected<const ManagedError *> parseFailure(const ManagedError &failure) {
    if (!parseFailure_) {
      parseFailure_.emplace(failure);
    }
    return std::unexpected<const ManagedError *>(&*parseFailure_);
  }

  std::unexpected<const ManagedError *> parseFailure(Code code, std::size_t offset) {
    return parseFailure(document_.failure(code, offset));
  }

  std::unexpected<const ManagedError *> parseFailure(Code code, std::size_t offset, std::string_view type,
                                                     std::string_view path) {
    return parseFailure(document_.propertyFailure(code, offset, type, path));
  }

  NodeStatus beginNodeStatus(std::size_t depth) {
    auto result = beginNode(depth);
    if (!result) {
      return parseFailure(result.error());
    }
    return *result;
  }

  RegionStatus nodePropertyStatus(std::size_t index, std::string_view token, bool positional, std::size_t begin,
                                  std::vector<std::string_view> &properties) {
    auto result = nodeProperty(index, token, positional, begin, properties);
    if (!result) {
      return parseFailure(result.error());
    }
    return {};
  }

  NodeStatus node(std::size_t depth) {
    auto begun = beginNodeStatus(depth);

    if (!begun) {
      return std::unexpected(begun.error());
    }

    const auto index = *begun;

    std::vector<std::string_view> properties(0);

    while (true) {
      skip();

      if (take('}')) {
        document_.order.push_back(index);

        return index;
      }

      if (position_ == document_.bytes.size()) {
        return parseFailure(Code::SYNTAX, position_);
      }

      const auto begin = position_;
      const auto token = peek() == '"' ? std::string_view{} : propertyName();
      const bool named = take(':') || take('=');
      const bool positional = !named && (token.empty() || token == "i18n");
      // A caller-owned child name wins the bare spelling; frame={...} remains an explicit native property.
      const bool frameGroup = token == "frame" && acceptsFrameSettings(document_.nodes[index]) &&
                              !detail::ResourceAccess::find(options_.registry, token);
      const bool formatGroup =
        token == "format" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::KEY_HELP_FALLBACK,
                              "format.maxBytesPerBinding") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool chartRangeGroup = token == "yRange" && acceptsChartRange(document_.nodes[index]) &&
                                   !detail::ResourceAccess::find(options_.registry, token) &&
                                   !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool labelsGroup =
        token == "labels" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::DATA_STATE_FALLBACK,
                              "labels.loading") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool dialogGroup =
        token == "dialog" &&
        (acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::DIALOG_FALLBACK, "dialog.mode") ||
         acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::MESSAGE_BINDING, "dialog.mode") ||
         acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::THEME_SELECTOR_BINDING,
                               "dialog.mode")) &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool messageGroup =
        token == "message" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::MESSAGE_BINDING, "body") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool catalogGroup =
        token == "themeCatalog" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::THEME_CATALOG_BINDING,
                              "themeCatalog.maxChoices") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool selectorGroup =
        token == "themeSelector" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::THEME_SELECTOR_BINDING,
                              "themeSelector.labels.apply") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool notificationGroup =
        token == "notification" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::NOTIFICATION_BINDING,
                              "notification.capacity") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool overlayPaddingChild =
        token == "padding" && document_.nodes[index].factory.kind == detail::ControlFactoryKind::OVERLAY_FALLBACK &&
        (detail::ResourceAccess::find(options_.registry, token) ||
         detail::ControlContextAccess::hasDeclaredType(controls_, token));
      const auto kind = document_.nodes[index].factory.kind;
      const bool protectedGroup =
        (kind == detail::ControlFactoryKind::USER || kind == detail::ControlFactoryKind::EXISTING_BUILTIN) &&
        detail::ResourceAccess::entry(options_.registry, document_.nodes[index].factory.index).protectedEditor &&
        (token == "securePolicy" || token == "limits") && !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool structured =
        (protectedGroup && peek() == '{') ||
        (options_.format == SceneResourceFormat::CONFIGURATION &&
         (token == "history" || token == "limits" || (token == "padding" && !overlayPaddingChild) || frameGroup ||
          formatGroup || chartRangeGroup || labelsGroup || dialogGroup || messageGroup || notificationGroup ||
          selectorGroup || catalogGroup) &&
         peek() == '{') ||
        (document_.nodes[index].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK &&
         token == "padding" && peek() == '{');

      if (named || positional || structured) {
        auto applied = nodePropertyStatus(index, token, positional, begin, properties);

        if (!applied) {
          return std::unexpected(applied.error());
        }
      } else {
        const bool standardGrid =
          document_.nodes[index].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK;
        if (standardGrid && (token == "header" || token == "body")) {
          auto parsed = region(index, token == "header" ? GridRegion::HEADER : GridRegion::BODY, depth + 1, begin);
          if (!parsed) {
            return std::unexpected(parsed.error());
          }
          continue;
        }
        if (standardGrid && document_.nodes[index].regions) {
          return parseFailure(Code::INVALID_VALUE, begin);
        }
        if (!detail::ResourceAccess::entry(options_.registry, document_.nodes[index].type).children &&
            document_.nodes[index].factory.kind != detail::ControlFactoryKind::MESSAGE_BINDING) {
          return parseFailure(Code::CHILDREN_FORBIDDEN, begin);
        }
        document_.nodes[index].directChildren = true;

        position_ = begin;

        auto child = node(depth + 1);

        if (!child) {
          return std::unexpected(child.error());
        }

        document_.nodes[*child].parent = index;
        ++document_.nodes[index].directChildCount;
      }
    }
  }

  std::optional<ManagedError> parseFailure_{};
  ResourceDocument &document_;
  SceneResourceOptions &options_;
  ControlContext &controls_;
  bool typed_{};
  std::size_t position_{};
  std::vector<std::pair<std::string_view, std::size_t>> ids_ = std::vector<std::pair<std::string_view, std::size_t>>(0);
  std::vector<std::pair<std::string_view, std::size_t>> declarations_ =
    std::vector<std::pair<std::string_view, std::size_t>>(0);
  std::vector<std::string_view> defaults_ = std::vector<std::string_view>(0);
};

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

      if (resolved->base == "ScrollArea") {
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

    for (const auto path : {std::string_view("text"), std::string_view("placeholder"), std::string_view("title"),
                            std::string_view("frame.title")}) {
      if (path == "title" && node.factory.kind != detail::ControlFactoryKind::DIALOG_FALLBACK) {
        continue;
      }

      auto &setting = path == "text"          ? node.arguments.settings.text
                      : path == "placeholder" ? node.arguments.settings.placeholder
                      : path == "frame.title" ? node.arguments.settings.frame.title
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

  auto parsed = Parser(document, effectiveOptions, controls, typed).parse();

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
