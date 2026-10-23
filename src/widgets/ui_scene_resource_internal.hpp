// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/control_context.hpp>
#include <portals/ui_scene_resource.hpp>

namespace portals::ui::detail {
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
  static std::optional<std::size_t> find(const SceneResourceRegistry &registry, std::string_view name);

  /** @brief Append the bounded default only after caller declarations have claimed their names. */
  static std::expected<void, ControlContextError> installGrid(SceneResourceRegistry &registry, ControlContext &controls,
                                                              bool typed);

  /** @brief Install CONFIGURATION-only native primitives after caller declarations and registrations. */
  static std::expected<void, ControlContextError> installPrimitives(SceneResourceRegistry &registry,
                                                                    ControlContext &controls);

  /** @brief Install scoped CONFIGURATION scroll adapters without replacing caller names or writer roles. */
  static std::expected<void, ControlContextError> installScrolls(SceneResourceRegistry &registry,
                                                                 ControlContext &controls);

  /** @brief Install scoped ordinary Overlay only after caller names have claimed their registry/schema entries. */
  static std::expected<void, ControlContextError> installOverlay(SceneResourceRegistry &registry,
                                                                 ControlContext &controls);

  static std::expected<void, ControlContextError> installKeyHelp(SceneResourceRegistry &registry,
                                                                 ControlContext &controls);

  /** @brief CFG-only one-body native entry; public custom registrations retain their old adapters. */
  static std::expected<void, ControlContextError> installDataState(SceneResourceRegistry &registry,
                                                                   ControlContext &controls);

  /** @brief CFG-only managed host/layer entries; caller names and old adapters retain priority. */
  static std::expected<void, ControlContextError> installDialogs(SceneResourceRegistry &registry,
                                                                 ControlContext &controls);

  // Mutate the existing source recipe storage, without adding transaction/erasure layers per descendant.
  template <class Wrapper> static void sourceCreate(Description<Widget> &recipe, Wrapper wrapper);

  static void sourceChild(Description<Widget> &parent, Description<Widget> child,
                          std::shared_ptr<const SceneResourceError> diagnostic, bool requireDialog = false);

  static void fallbackMessageResolver(Description<Widget> &recipe, const TextResolver &resolver,
                                      std::shared_ptr<const SceneResourceError> diagnostic);

  static void fallbackThemeSelectorResolver(Description<Widget> &recipe, const TextResolver &resolver,
                                            std::shared_ptr<const SceneResourceError> diagnostic);

  static void fallbackDataStateResolver(Description<DataStateView> &recipe, const TextResolver &resolver,
                                        std::shared_ptr<const SceneResourceError> diagnostic);

  static std::expected<Description<Widget>, ManagedError> keyHintsRecipe(SceneResourceArguments arguments,
                                                                         Ref<Widget> help);

  /** @param registry Owning allowlist, unchanged during prepare.
   * @param index Index previously validated by find.
   * @return Immutable schema/factory borrowed only for the current synchronous operation.
   */
  static const auto &entry(const SceneResourceRegistry &registry, std::size_t index) {
    return registry.entries_[index];
  }
};
} // namespace portals::ui::detail

namespace portals::ui::detail::resource {
using Code = SceneResourceErrorCode;

inline bool identifierStart(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

inline bool identifierPart(char value) {
  return identifierStart(value) || (value >= '0' && value <= '9');
}

inline bool valid(const SceneResourceLimits &limits) {
  constexpr std::size_t MAX_RESOURCE_DEPTH = 64;

  return limits.maxSourceBytes && limits.maxNodes && limits.maxDepth && limits.maxDepth <= MAX_RESOURCE_DEPTH &&
         limits.maxStringBytes && limits.maxProperties;
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

  std::shared_ptr<const ControlSourceOrigin> origin(std::size_t offset) const;

  ManagedError failure(Code code, std::size_t offset, std::error_code system = {}, bool positioned = true) const;

  ManagedError propertyFailure(Code code, std::size_t offset, std::string_view type, std::string_view path) const;
};

std::expected<void, ManagedError> parseResourceDocument(ResourceDocument &document, SceneResourceOptions &options,
                                                        ControlContext &controls, bool typed);

std::expected<void, ManagedError> registerSchemas(ControlContext &controls, const SceneResourceOptions &options);

std::expected<void, ManagedError> prepareNodes(ResourceDocument &document, const ControlContext &controls,
                                               detail::MountContext &context, ManagedScene &owner,
                                               const SceneResourceRegistry &registry);

std::expected<void, ManagedError> prepareHelpReferences(ResourceDocument &document,
                                                        const SceneResourceRegistry &registry);

std::expected<void, ManagedError> prepareGrids(ResourceDocument &document, bool typed);

} // namespace portals::ui::detail::resource
