// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/control_context.hpp>
#include <portals/detail/move_only_function.hpp>
#include <portals/detail/recipe_sizing.hpp>
#include <portals/ui_ref.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace portals::ui {
/** @brief Owning setup action invoked once after construction, before children.
 * @tparam T Concrete Widget type observed by the action.
 * Arguments are borrowed for the invocation. External effects and separately created scene
 * state are not rolled back; the attempted description subtree is removed on failure.
 */
template <class T>
using Setup = portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<T>)>;

namespace detail {
struct RecipeLayout;
struct SemanticConstruction;
struct MountContext;

/** @brief Unmounted recipe storage, consumed and destroyed during mount.
 * @tparam T Concrete observed widget type; callback signatures remain typed.
 */
template <class T> struct RecipeState {
  /** @brief Owning constructor adapter using the existing checked managed mount. */
  using Factory = portals::detail::MoveOnlyFunction<std::expected<Node<T>, ManagedError>(ManagedScene &, Node<Widget>,
                                                                                         detail::MountContext &)>;
  /** @brief Owning child recipe adapter sharing the parent's transaction bookkeeping. */
  using Child = portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<Widget>,
                                                                                    detail::MountContext &)>;

  /** @brief Optional cold decoration; never retained by mounted callbacks or widgets. */
  struct FrameRecipe {
    FrameOptions options;                    ///< Owning advanced pattern and finite work settings.
    std::unique_ptr<ControlSettings> source; ///< Precharged resource frame leaves and diagnostic provenance.
    bool hasOptions{};                       ///< Explicit fluent decoration, independent of an inherited sparse patch.
    std::optional<Ref<Border>> reference;    ///< Optional observer for explicit outer geometry/removal.
    Node<Border> mounted;                    ///< Mount-only resize target; destroyed with the cold State.
    bool invalidPattern{};                   ///< Unknown compact spelling, reported at mount rather than ignored.
  };

  Factory create;                                                  ///< Constructs exactly one managed node.
  std::vector<Setup<T>> setup = std::vector<Setup<T>>(0);          ///< Customization actions in modifier order.
  std::vector<Child> children = std::vector<Child>(0);             ///< Consuming child adapters in insertion order.
  std::vector<NodeRef<T>> references = std::vector<NodeRef<T>>(0); ///< Observing slots reserved for this node.
  std::shared_ptr<detail::RecipeLayout> layout; ///< Optional cold relationship metadata; never mounted state.
  std::unique_ptr<FrameRecipe> frame;           ///< Allocated only for an explicitly framed recipe.
  std::unique_ptr<RecipeSizing> sizing;         ///< Final cold bounds; absent on the legacy sizing path.
  bool forwardsOuter{};                         ///< Type-erased factory forwards its inner mount's actual layout root.
  bool focusScope{};                            ///< Register before setup/children; scene owns its token.
  bool focusDefault{};                          ///< Set the nearest registered scope's default before children.
  std::optional<bool> visible;                  ///< Final mount-only local visibility; absent keeps tree defaults.
  std::optional<bool> enabled;                  ///< Final mount-only local enabled state; no runtime state source.
  std::optional<std::string> id;                ///< Compact recipe identity preserved by later style modifiers.
  std::shared_ptr<detail::RefTarget> anchor;    ///< Cold same-attempt body anchor, without widget ownership.
  DialogPlacement placement{DialogPlacement::BELOW_START}; ///< Configured anchor preference.
  bool anchorRequested{};                    ///< Distinguish an absent modifier from an empty moved observing slot.
  std::optional<StyleTarget> intrinsicStyle; ///< Owning natural type names, finalized after ordinary setup.
  std::shared_ptr<detail::SemanticConstruction>
    semanticConstruction;                ///< Actual native capability, cold owning data only.
  bool themeCatalogSettingsRequested{};  ///< Requires the actual owning catalog constructor.
  bool themeSelectorSettingsRequested{}; ///< Explicit view stage requires the actual native selector recipe.
  bool notificationSettingsRequested{};  ///< Explicit notification group requires the actual native center recipe.
  bool messageSettingsRequested{};       ///< Explicit message group must never be ignored on ordinary Dialog.
  bool controlPrepared{};                ///< Resource settings were validated and materialized before construction.
  bool inheritPreparedDefaults{};        ///< Only derived source-title headings inherit unrelated Label defaults.
  bool deferredControlValidation{};      ///< Root resource parser validates after CONFIG, before document effects.
  detail::ControlPropertyPresence
    materializedSettings{};                  ///< Resource properties already reserved in the aggregate leaf budget.
  std::optional<std::string> controlType;    ///< Supported concrete factory; absent on arbitrary custom widgets.
  std::optional<std::string> controlVariant; ///< Requested runtime name, checked against the concrete type.
  ControlSettings controlPatch;              ///< Owning final sparse instance overrides.
  ControlSettings controlArguments;          ///< Explicit positional literals, below the final sparse patch.
  bool settingsRequested{};          ///< Reject unsupported custom recipes rather than silently ignoring a patch.
  bool inheritControlSettings{true}; ///< Explicit legacy full configuration bypasses context defaults.
  std::unique_ptr<ResolvedControl> resolvedControl; ///< Mount-only owning settings, absent on the builtin fast path.
  portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<T>,
                                                                      const ControlSettings &)>
    controlSetup; ///< Standard binding, after root tracking and before ordinary user setup.
};
} // namespace detail
} // namespace portals::ui
