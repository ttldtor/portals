// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/control_context.hpp>
#include <portals/detail/line_edit_text.hpp>
#include <portals/detail/move_only_function.hpp>
#include <portals/grid.hpp>
#include <portals/key_hints.hpp>
#include <portals/managed.hpp>
#include <portals/message_box.hpp>
#include <portals/theme_selection.hpp>
#include <portals/theme_selector.hpp>
#include <portals/ui_ref.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <ranges>
#include <tuple>
#include <typeinfo>

/// @brief Owning construction descriptions mounted through the existing ManagedScene lifecycle.
namespace portals::ui {
/** @brief Single-use owning construction recipe; no mounted widget ownership.
 * @tparam T Public Widget subtype constructed by the recipe, including final types.
 */
template <class T> class Description;
class GridSection;

namespace detail {
/** @brief Recognize an owning recipe after cv/ref normalization.
 * @tparam T Type tested for an exact Description
 * specialization.
 */
template <class T> struct IsDescription : std::false_type {};

/** @brief Recognize the owning recipe specialization.
 * @tparam T Widget type described by this recipe.
 */
template <class T> struct IsDescription<Description<T>> : std::true_type {};

/** @brief Admit only consuming recipe arguments, rejecting implicit moves from lvalues.
 * @tparam T
 * Forwarding-reference argument type before normalization.
 */
template <class T>
concept DescriptionInput = IsDescription<std::remove_cvref_t<T>>::value && !std::is_lvalue_reference_v<T>;

/** @brief Consuming real grid child or semantic section. @tparam T Forwarded construction argument. */
template <class T>
concept GridInput =
  DescriptionInput<T> || (std::same_as<std::remove_cvref_t<T>, GridSection> && !std::is_lvalue_reference_v<T>);

/** @brief Admit a declared dialog layer or an erased recipe checked after its existing factory runs.
 * @tparam T Consuming Description<Dialog> or Description<Widget>; other typed widgets are ordinary body content.
 */
template <class T>
concept SceneLayer = DescriptionInput<T> && (std::same_as<std::remove_cvref_t<T>, Description<Dialog>> ||
                                             std::same_as<std::remove_cvref_t<T>, Description<Widget>>);

/** @brief Admit immediately consumable recipes without draining borrowed lvalue elements.
 * @tparam R Input range producing Description values, explicit rvalue references, or an rvalue owning container.
 */
template <class R>
concept DescriptionRange =
  std::ranges::input_range<R> && IsDescription<std::ranges::range_value_t<R>>::value &&
  ((DescriptionInput<std::ranges::range_reference_t<R>> &&
    std::constructible_from<std::ranges::range_value_t<R>, std::ranges::range_reference_t<R>>) ||
   (!std::is_lvalue_reference_v<R> && !std::ranges::view<std::remove_cvref_t<R>> &&
    !std::ranges::enable_borrowed_range<std::remove_cvref_t<R>> &&
    std::same_as<std::ranges::range_reference_t<R>, std::ranges::range_value_t<R> &>));

/** @brief Preserve a construction failure without formatting its diagnostic.
 * @param node Observing target identity,
 * possibly empty before allocation.
 * @param cause Original tree error from the failed construction step.
 * @return
 * Mount-phase error with independent diagnostic fields initially empty.
 */
inline ManagedError error(Node<Widget> node, TreeError cause) {
  return {ManagedPhase::MOUNT, node.id(), cause, {}, {}};
}

/** @brief Preserve the standard text-control error mapping and diagnostic payload.
 * @param node Observing parent before text-control allocation.
 * @param cause Original semantic or resource-limit text error.
 * @return MOUNT error: NODE_LIMIT for text resource caps, otherwise INVALID_ARGUMENT, with unchanged text payload.
 */
inline ManagedError textError(Node<Widget> node, TextError cause) {
  const auto kind = cause.kind;
  const bool limited = kind == TextErrorKind::INPUT_LIMIT || kind == TextErrorKind::CLUSTER_LIMIT ||
                       kind == TextErrorKind::GRAPHEME_LIMIT || kind == TextErrorKind::CELL_LIMIT;
  auto failure = error(node, limited ? TreeError::NODE_LIMIT : TreeError::INVALID_ARGUMENT);

  failure.text = cause;

  return failure;
}

/** @param options Editor options under the receiving scene width policy; owned placeholder bytes are consumed.
 * @param parent Receiving parent identity retained on preparation failure.
 * @return Prepared presentation or exact checked mount error, before internal state publication.
 */
inline std::expected<portals::detail::LineEditPlaceholder, ManagedError> lineEditPlaceholder(LineEditOptions &options,
                                                                                             Node<Widget> parent) {
  if (!portals::detail::validLineEditDisplayMode(options.displayMode) ||
      (options.placeholder && portals::detail::hasLineSeparator(*options.placeholder))) {
    return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
  }
  auto prepared = portals::detail::prepareLineEditPlaceholder(options);
  if (!prepared) {
    return std::unexpected(textError(parent, prepared.error()));
  }
  return std::move(*prepared);
}

// Checked MSVC strings allocate iterator proxies in otherwise noexcept moves.
#if defined(_MSVC_STL_VERSION) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
inline constexpr bool CHECKED_STRING_MOVES = true; ///< Use stationary string captures only in the affected STL mode.
#else
inline constexpr bool CHECKED_STRING_MOVES = false; ///< Ordinary string moves retain their original construction cost.
#endif

/** @param value Text copied during capture; no borrowed view survives.
 * @return Stationary shared bytes only for checked MSVC, otherwise the ordinary owning string.
 */
inline auto captureText(std::string_view value) {
  if constexpr (CHECKED_STRING_MOVES) {
    return std::make_shared<const std::string>(value);
  } else {
    return std::string(value);
  }
}

/** @tparam T Exact owning string or shared string storage returned by captureText.
 * @param value Storage borrowed only during one synchronous recipe operation.
 * @return Bytes borrowed until the capture is replaced or destroyed.
 */
template <class T> std::string_view captionView(const T &value) {
  if constexpr (std::same_as<T, std::string>) {
    return value;
  } else {
    return *value;
  }
}

/** @brief One existing size-policy update, retained until checked mount. */
enum class SizeDimension {
  WIDTH,
  HEIGHT,
  FLEX_X,
  FLEX_Y,
  FLEX
};

/** @brief Owning compact size operation; validation remains in setSizePolicy. */
struct SizeChange {
  SizeDimension dimension; ///< Axis or axes receiving the change.
  std::int32_t extent{};   ///< Fixed cells for WIDTH/HEIGHT.
  std::uint32_t weight{1}; ///< Positive flex weight for FLEX operations.
};

/** @brief Grid-only sparse configuration, released with its construction transaction. */
struct GridRecipeControl {
  ControlSettings arguments; ///< Positional grid literals retained for fallback-schema resolution.
  ControlSettings patch;     ///< Sparse instance overrides applied after positional literals.
  std::optional<std::string> variant; ///< Optional named fallback type checked during preflight.
  std::unique_ptr<ResolvedControl> resolved; ///< Owning settings resolved before control construction.
  bool prepared{};           ///< Settings were already resolved and validated.
  bool materialized{};       ///< Resource properties already supplied materialized grid settings.
  bool settingsRequested{};  ///< Explicit settings or variant modifiers were requested.
  std::uint32_t materializedSettings{}; ///< Resource fields already counted against the mount budget.
};

/** @brief Cold layout values only; released with the single-use construction transaction. */
struct RecipeLayout {
  GridItemOptions item;   ///< Parent-child placement, never inherited from control defaults.
  bool itemRequested{};   ///< Distinguish ordinary defaults from an explicitly requested relationship.
  bool gridContainer{};   ///< True only for the standard grid recipe.
  bool validated{};       ///< This cold graph has completed its parent-aware preflight.
  bool validRecipe{true}; ///< An empty consuming child fails before any mount effects.
  bool hasDeclaredGrid{}; ///< Cold composition contains an explicitly configured standard Grid.
  std::optional<DeclarativeGridOptions> grid; ///< Present only after declarative opt-in.
  std::unique_ptr<GridPlacementPlan> plan;    ///< Origins established before any constructor or setup.
  std::vector<std::shared_ptr<RecipeLayout>> children = std::vector<std::shared_ptr<RecipeLayout>>(0); ///< Cold child metadata in insertion order.
  std::vector<NodeId> mounted = std::vector<NodeId>(0); ///< Full identities collected during child mount.
  std::size_t headerSections{}; ///< Number of structural header sections.
  std::size_t bodySections{};   ///< Number of structural body sections.
  std::size_t directItems{};    ///< Number of children outside structural sections.
  bool stickyHeader{};          ///< Header sticky setting copied from its section.
  bool validSections{true};     ///< Whether section placement modifiers are valid.
  GridPresentation presentation; ///< Owning grid background and paint-budget settings.
  std::optional<CellStyle> headerStyle; ///< Optional header-section background.
  std::optional<CellStyle> bodyStyle;   ///< Optional body-section background.
  std::optional<GridRowStyles> rowStyles; ///< Optional alternating body-row backgrounds.
  std::optional<GridScrollOptions> scroll; ///< Optional owning scroll settings.
  std::unique_ptr<GridRecipeControl> control; ///< Deferred grid control settings.
  std::unique_ptr<std::vector<AxisSize>> columnSizes; ///< Cold owning shared-column policies, absent by default.
  std::unique_ptr<std::vector<AxisSize>> rowSizes;    ///< Cold owning body-row policies; header stays independent.
};

/** @param root Cold metadata borrowed for this synchronous validation.
 * @return Complete parent-aware placement validation, without factories or tree access.
 */
inline std::expected<void, TreeError> preflightLayout(const std::shared_ptr<RecipeLayout> &root) {
  if (!root || root->validated) {
    return {};
  }

  struct Visit {
    std::shared_ptr<RecipeLayout> layout;
    bool gridChild{};
  };

  std::vector<Visit> pending(0);
  std::vector<std::shared_ptr<RecipeLayout>> checked(0);

  pending.push_back({root, false});

  while (!pending.empty()) {
    auto visit = std::move(pending.back());

    pending.pop_back();

    auto &layout = *visit.layout;

    if (!layout.validRecipe || (layout.itemRequested && !visit.gridChild) || (layout.grid && !layout.gridContainer)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if (layout.gridContainer && layout.presentation.maxPaintCells == 0) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    const bool declarative = layout.gridContainer && layout.grid.has_value();

    if (declarative) {
      if (!layout.validSections || layout.headerSections > 1 || layout.bodySections > 1 ||
          ((layout.grid->regions || layout.headerSections || layout.bodySections) && layout.directItems != 0)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      if (layout.headerSections || layout.bodySections) {
        layout.grid->regions = true;
        layout.grid->stickyHeader = layout.stickyHeader;
      }

      if (layout.scroll) {
        auto valid = validateScrollOptions(layout.scroll->area);

        if (!valid) {
          return std::unexpected(valid.error());
        }
      }

      std::vector<GridItemOptions> items(0);

      items.reserve(layout.children.size());

      for (const auto &child : layout.children) {
        items.push_back(child->item);
      }

      auto resolved = resolveGridPlacements(*layout.grid, items);

      if (!resolved) {
        return std::unexpected(resolved.error());
      }

      for (const auto &[policies, count] :
           {std::pair{layout.columnSizes.get(), resolved->columns}, std::pair{layout.rowSizes.get(), resolved->rows}}) {
        if (!policies) {
          continue;
        }

        if (policies->size() != count) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }

        auto valid = portals::detail::validateGridTrackSizes(*policies);

        if (!valid) {
          return std::unexpected(valid.error());
        }
      }

      layout.plan = std::make_unique<GridPlacementPlan>(*resolved);
      layout.mounted.reserve(items.size());
    }

    for (const auto &child : layout.children) {
      pending.push_back({child, declarative});
    }

    checked.push_back(std::move(visit.layout));
  }

  for (const auto &layout : checked) {
    layout->validated = true;
  }

  return {};
}

/** @param layout Validated cold grid metadata, borrowed only for this owning materialization.
 * @return Resolved options, with identities only after all children have mounted.
 */
inline GridOptions resolvedGridOptions(const RecipeLayout &layout) {
  GridOptions result;
  const auto &settings = *layout.grid;
  const auto &plan = *layout.plan;

  result.columns.resize(plan.columns);
  result.rows.resize(plan.rows);
  result.header_rows.resize(plan.headerRows);

  if (layout.columnSizes) {
    result.columns = *layout.columnSizes;
  }

  if (layout.rowSizes) {
    result.rows = *layout.rowSizes;
  }
  result.regions = settings.regions;
  result.sticky_header = settings.stickyHeader;
  result.presentation = layout.presentation;

  if (layout.headerStyle) {
    result.presentation.header = layout.headerStyle;
  }

  if (layout.bodyStyle) {
    result.presentation.body = layout.bodyStyle;
  }

  if (layout.rowStyles) {
    result.presentation.rows = *layout.rowStyles;
  }
  result.column_gap = settings.columnGap;
  result.row_gap = settings.rowGap;
  result.padding = settings.padding;
  result.max_tracks_per_axis = settings.maxTracksPerAxis;
  result.max_placements = settings.maxPlacements;
  result.cells.reserve(layout.mounted.size());

  for (std::size_t index = 0; index < layout.mounted.size(); ++index) {
    const auto &item = layout.children[index]->item;
    const auto position = plan.positions[index];

    result.cells.push_back({layout.mounted[index], position.row, position.column, item.rowSpan, item.columnSpan,
                            item.horizontal, item.vertical, item.region});
  }

  return GridOptions(result);
}

/** @brief Stack-owned transaction bookkeeping, discarded after one description mount. */
struct MountContext {
  ControlContext controls{};   ///< Immutable owning defaults for this mount transaction.
  std::size_t controlLeaves{}; ///< Effective settings counted across the complete transaction.
  bool controlPrepared{};      ///< Only the current constructor received already resolved resource settings.
  const ControlSettings *controlSettings{}; ///< Constructor-only borrowed settings; restored before setup/children.
  Node<Widget> root;                        ///< First created node; identifies the subtree to retire on failure.
  std::vector<Node<Widget>> nodes =
    std::vector<Node<Widget>>(0); ///< Created observers checked for liveness before publication.
  std::vector<portals::detail::MoveOnlyFunction<void()>> publish =
    std::vector<portals::detail::MoveOnlyFunction<void()>>(0); ///< Nonthrowing assignments publishing reserved refs.
  std::vector<portals::detail::MoveOnlyFunction<void()>> release =
    std::vector<portals::detail::MoveOnlyFunction<void()>>(0); ///< Nonthrowing releases of temporary ref reservations.

  /** @brief Same-attempt Ref reservation used by cold graph finalization before publication. */
  struct PendingRef {
    const RefTarget *identity{}; ///< Original unpublished slot identity; never dereferenced from another thread.
    RefTarget target;            ///< Checked private identity used only by cold finalization.
  };

  std::vector<PendingRef> pending = std::vector<PendingRef>(0); ///< Private same-attempt reservations.

  /** @brief Transaction-local dependency; it never survives mount or schedules runtime work. */
  struct CaptionFinalizer {
    Node<Control> receiver;            ///< Newly created caption receiving this transaction's initial value.
    std::shared_ptr<RefTarget> source; ///< Original weak observation, resolved after every sibling has mounted.
    bool completed{}; ///< True after source-first initial synchronization and observation registration succeed.
    portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>()> complete; ///< Owning mount-only action.
  };

  std::vector<CaptionFinalizer> captions =
    std::vector<CaptionFinalizer>(0); ///< Fallible checked-STL construction stays inside mount's error boundary.
  std::vector<portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>()>> finalize =
    std::vector<portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>()>>(
      0); ///< Pure cold graph completion.
  std::vector<portals::detail::MoveOnlyFunction<void()>> rollback =
    std::vector<portals::detail::MoveOnlyFunction<void()>>(
      0);           ///< Explicit attempt-owned resources, never numeric checkpoints.
  bool committed{}; ///< True only after every finalizer and node check succeeds.

  /** @brief Release attempt-owned resources and ref reservations after publication or rollback. */
  ~MountContext() {
    if (!committed) {
      for (auto &retire : rollback) {
        retire();
      }
    }
    for (auto &reservation : release) {
      reservation();
    }
  }
};

/** @brief Internal bridge to existing managed validation and private consuming recipes. */
/** @param root Cold layout metadata. @param context Whole transaction accounting and immutable controls.
 * @param parent Checked error attribution parent. @return Grid-only preflight with owning typed diagnostics.
 */
std::expected<void, ManagedError> prepareGridLayouts(const std::shared_ptr<RecipeLayout> &root, MountContext &context,
                                                     Node<Widget> parent);

/** @brief Internal bridge for checked access to managed scene and recipe state. */
struct Access {
  /** @param scene Owner of the nested mount transaction entered on this thread. */
  static void beginMount(ManagedScene &scene) noexcept {
    if (scene.owner_ == std::this_thread::get_id()) {
      ++scene.mountDepth_;
    }
  }

  /** @param scene Owner whose complete transaction, including Ref publication, has ended. */
  static void endMount(ManagedScene &scene) noexcept {
    if (scene.owner_ == std::this_thread::get_id()) {
      --scene.mountDepth_;
    }
  }

  /** @param scene Borrowed managed owner; observed only during UI-affine publication.
   * @return Weak token that expires before scene destruction releases registrations.
   */
  static std::weak_ptr<const void> lifetime(ManagedScene &scene) {
    return scene.lifetime_;
  }

  /// @cond INTERNAL
  static std::expected<Node<Widget>, ManagedError> dialogPanel(ManagedScene &scene, Node<Dialog> node,
                                                               BorderOptions options) {
    return scene.makeDialogPanel(node, options);
  }

  /// @endcond

  /// @cond INTERNAL
  static std::expected<void, ManagedError> notifications(ManagedScene &scene, Node<Widget> wrapper, Node<Widget> body,
                                                         const NotificationCenter &center,
                                                         const NotificationOptions &options, MountContext &context) {
    auto reserved = NotificationAccess::reserve(center);
    if (!reserved) {
      return reserved;
    }
    try {
      context.release.emplace_back([center] {
        NotificationAccess::release(center);
      });
    } catch (...) {
      NotificationAccess::release(center);
      throw;
    }
    auto made = scene.makeNotifications(wrapper, body, options, context.nodes);
    if (!made) {
      return std::unexpected(made.error());
    }
    context.finalize.emplace_back([&scene, record = *made] {
      return scene.finalizeNotifications(record);
    });
    context.publish.emplace_back([center, record = *made] {
      NotificationAccess::publish(center, record);
    });
    return {};
  }

  /// @endcond

  /** @brief Mount a root or child inside the same transaction.
   * @tparam T Public Widget subtype constructed by the
   * recipe.
   * @param description Owning recipe consumed by this call, including on failure.
   * @param scene
   * Existing UI-affine owner of all mounted nodes.
   * @param parent Live managed parent receiving the new child.
   *
   * @param context Borrowed transaction retaining observations and deferred ref publication.
   * @return Typed
   * observing node or the unchanged creation/setup/child error.
   */
  template <class T>
  static std::expected<Node<T>, ManagedError> mount(Description<T> &&description, ManagedScene &scene,
                                                    Node<Widget> parent, MountContext &context);
  /** @tparam T Root widget type. @param description Unmounted root borrowed for bounded opt-in detection.
   * @return Whether an explicitly configured standard Grid needs the local fallback schema.
   */
  template <class T> static bool needsGridContext(const Description<T> &description);

  /** @param description Root resource transaction validating its effective graph after local declarations.
   * @return Consuming description; native ancestor callbacks keep their existing ordering.
   */
  static Description<Widget> deferControlValidation(Description<Widget> description);
  /** @param description Root recipe borrowed before any factory effects.
   * @return Whether the resource parser owns complete effective-context validation before its effects.
   */
  template <class T> static bool defersControlValidation(const Description<T> &description);

  /** @brief Wrap a checked managed factory without exposing recipe construction publicly.
   * @tparam T Widget subtype returned by the factory.
   * @tparam Factory Owning callable returning expected<Node<T>, ManagedError> for scene and parent.
   * @param factory Capture retained until the consuming mount; construction may throw.
   * @return Single-use description with the existing transaction semantics.
   */
  template <class T, class Factory> static Description<T> describe(Factory factory);

  /** @tparam T Concrete widget created by the recipe.
   * @param description Owning recipe receiving intrinsic style names.
   * @param identity Owning natural type and required ancestors, finalized after setup modifiers.
   * @return Consuming recipe retaining the existing mount/ref transaction.
   */
  template <class T> static Description<T> intrinsicStyle(Description<T> description, StyleTarget identity);
  /** @brief Mark resource-resolved settings without budgeting the same fields twice.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Owning recipe updated before mounting.
   * @param alreadyMaterialized Presence mask already included in the resource mount budget.
   */
  template <class T> static void preparedControl(Description<T> &description, std::uint32_t alreadyMaterialized = 0);
  /** @brief Attach a builtin configuration identity to an owning recipe.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Single-use recipe receiving configuration metadata.
   * @param type Canonical builtin name copied into the recipe.
   * @param inherit Whether omitted constructor properties inherit context defaults.
   * @return Recipe retaining its existing construction and cleanup behavior.
   */
  template <class T>
  static Description<T> configuredControl(Description<T> description, std::string_view type, bool inherit = true);
  /** @brief Retain explicit constructor properties above inherited defaults.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Single-use recipe receiving constructor metadata.
   * @param arguments Owning sparse properties supplied by the factory call.
   * @return Recipe whose later instance settings retain highest priority.
   */
  template <class T> static Description<T> controlArguments(Description<T> description, ControlSettings arguments);
  /** @tparam Parent Parent widget type. @tparam Child Child widget type.
   * @param parent Receiving cold recipe. @param child Consumed child recipe, inspected before transfer.
   * @param structural Retain every child relationship, including default placements.
   */
  template <class Parent, class Child>
  static void layoutChild(Description<Parent> &parent, Description<Child> &child, bool structural = false);
  /** @tparam Children Consuming description types inspected before their constructor captures are transferred.
   * @param children Ordinary child recipes, without invoking their factories.
   * @return Optional owning layout-only metadata for a composition whose children mount inside its factory.
   */
  template <class... Children> static std::shared_ptr<RecipeLayout> captureLayouts(Children &...children);
  /** @tparam T Parent widget type. @param description Cold composition retaining normal transaction behavior.
   * @param layout Previously captured owning layout metadata.
   * @return Same consuming composition with its children included in preflight.
   */
  template <class T> static Description<T> withLayout(Description<T> description, std::shared_ptr<RecipeLayout> layout);
  /** @tparam Child Consuming real child recipe. @param parent Receiving standard grid recipe.
   * @param child Single-use real child. @param region Present only for a structural section.
   */
  template <class Child>
  static void gridChild(Description<Grid> &parent, Child &&child, std::optional<GridRegion> region = {});
  /** @param parent Receiving standard grid recipe. @param section Consumed header/body group, without a Widget. */
  static void gridSection(Description<Grid> &parent, GridSection section);

  /** @param scene Live managed owner. @param node Declarative grid receiving complete identities.
   * @param options Resolved settings borrowed during checked replacement.
   * @return Existing typed configuration result, attributed to this grid.
   */
  static std::expected<void, ManagedError> gridOptions(ManagedScene &scene, Node<Grid> node,
                                                       const GridOptions &options) {
    auto result = setDeclarativeGridOptions(scene.app_.tree(), node, options);

    return result ? std::expected<void, ManagedError>{} : std::unexpected(error(node, result.error()));
  }
  /** @brief Attach binding setup after node creation within the mount transaction.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @tparam Callback Synchronous checked setup callable for scene, node and settings.
   * @param description Single-use recipe retaining normal failure cleanup.
   * @param callback Owning setup callable retained until mount.
   * @return Recipe publishing references only after setup and finalization succeed.
   */
  template <class T, class Callback> static Description<T> controlSetup(Description<T> description, Callback callback);

  /** @brief Identify builtin recipe types without allocating runtime metadata.
   * @tparam T Concrete widget subtype to classify.
   * @return Canonical builtin configuration name, or an empty view for custom types.
   */
  template <class T> static constexpr std::string_view controlType() {
    if constexpr (std::same_as<T, Label>) {
      return "Label";
    } else if constexpr (std::same_as<T, Button>) {
      return "Button";
    } else if constexpr (std::same_as<T, CheckBox>) {
      return "CheckBox";
    } else if constexpr (std::same_as<T, ThreeState>) {
      return "ThreeState";
    } else if constexpr (std::same_as<T, LineEdit>) {
      return "LineEdit";
    } else if constexpr (std::same_as<T, Row>) {
      return "HBox";
    } else if constexpr (std::same_as<T, Column>) {
      return "VBox";
    } else
      return {};
  }

  /** @param base Owning lower-priority constructor arguments.
   * @param patch Final explicit sparse instance properties.
   * @return Owning structural merge, retaining explicit false/zero and winning property provenance.
   */
  static ControlSettings mergeControlSettings(ControlSettings base, const ControlSettings &patch) {
    const auto apply = [](auto &target, const auto &source) {
      if (source) {
        target = source;
      }
    };
    apply(base.text, patch.text);
    apply(base.width, patch.width);
    apply(base.height, patch.height);
    apply(base.flex, patch.flex);
    apply(base.gap, patch.gap);
    apply(base.state, patch.state);
    apply(base.cycle, patch.cycle);
    apply(base.editMode, patch.editMode);
    apply(base.readOnly, patch.readOnly);
    apply(base.programmaticChange, patch.programmaticChange);
    apply(base.displayMode, patch.displayMode);
    apply(base.placeholder, patch.placeholder);
    apply(base.history.enabled, patch.history.enabled);
    apply(base.history.maxOperations, patch.history.maxOperations);
    apply(base.history.maxBytes, patch.history.maxBytes);
    apply(base.history.groupTyping, patch.history.groupTyping);
    apply(base.history.groupDeletion, patch.history.groupDeletion);
    apply(base.grid.columns, patch.grid.columns);
    apply(base.grid.rows, patch.grid.rows);
    apply(base.grid.flow, patch.grid.flow);
    apply(base.grid.columnGap, patch.grid.columnGap);
    apply(base.grid.rowGap, patch.grid.rowGap);
    apply(base.grid.paddingLeft, patch.grid.paddingLeft);
    apply(base.grid.paddingTop, patch.grid.paddingTop);
    apply(base.grid.paddingRight, patch.grid.paddingRight);
    apply(base.grid.paddingBottom, patch.grid.paddingBottom);
    apply(base.grid.maxTracksPerAxis, patch.grid.maxTracksPerAxis);
    apply(base.grid.maxPlacements, patch.grid.maxPlacements);
    apply(base.grid.maxPlacementWork, patch.grid.maxPlacementWork);
    apply(base.grid.scroll, patch.grid.scroll);
    base.origin = patch.origin;
    base.origins = patch.origins;
    return base;
  }

  /** @param scene Existing UI owner; checked before reading mutable metadata.
   * @param node Live managed node whose latest target is retained during preparation.
   * @param identity Owned natural type/ancestors retained by the mounting recipe.
   * @return Checked style admission; metadata copies and new ancestors are prepared before publication.
   */
  static std::expected<void, ManagedError> finalizeStyleIdentity(ManagedScene &scene, Node<Widget> node,
                                                                 const StyleTarget &identity) {
    auto allowed = scene.boundary(ManagedPhase::STYLE, node);

    if (!allowed) {
      return allowed;
    }

    auto entry = scene.find(node);

    if (!entry) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    try {
      // Retain the exact metadata owner across allocation and possible nested mutation.
      const auto current = entry->target;

      if (current && current->baseTypes.size() > MAX_STYLE_BASE_TYPES) {
        return std::unexpected(
          ManagedError{ManagedPhase::STYLE, node.id(), validateStyleTarget(*current).error(), {}, {}});
      }

      if (current && !current->type.empty() && (!identity.control || current->control)) {
        const auto containsName = [&](const std::string &name) {
          return name == current->type || std::ranges::find(current->baseTypes, name) != current->baseTypes.end();
        };

        if (containsName(identity.type) && std::ranges::all_of(identity.baseTypes, containsName)) {
          auto valid = validateStyleTarget(*current);

          if (!valid) {
            return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), valid.error(), {}, {}});
          }

          return {};
        }
      }

      auto target = current ? *current : StyleTarget{};

      if (target.type.empty()) {
        target.type = identity.type;
      }

      const auto retainName = [&](const std::string &name) {
        if (name != target.type && std::ranges::find(target.baseTypes, name) == target.baseTypes.end()) {
          target.baseTypes.push_back(name);
        }
      };

      retainName(identity.type);

      for (const auto &name : identity.baseTypes) {
        retainName(name);
      }

      target.control = target.control || identity.control;

      auto valid = validateStyleTarget(target);

      if (!valid) {
        return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), valid.error(), {}, {}});
      }

      return scene.style(node, std::move(target));
    } catch (const std::bad_alloc &) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), TreeError::NO_MEMORY, {}, {}});
    }
  }

  /// @cond INTERNAL
  template <class T, class Factory> static Description<T> cold(Factory factory);

  static void beginCold(ManagedScene &scene) noexcept {
    ++scene.coldMountPending_;
  }

  struct ColdGraph {
    ManagedScene *scene{};
    std::shared_ptr<ManagedScene::LiveTextRecord> live;
    std::shared_ptr<void> output;
    std::shared_ptr<ManagedScene::ObserverRecord> observer;
    std::shared_ptr<portals::detail::ManagedModelSource> model;
    bool pending{};

    void retire() noexcept {
      // The complete logical mark precedes disconnect/destructor reentry.
      if (pending) {
        --scene->coldMountPending_;
        pending = false;
      }
      if (observer) {
        observer->retired = true;
      }
      if (observer) {
        observer->subscription = {};
      }
      if (model) {
        std::erase(scene->models_, model);
      }
      if (observer) {
        std::erase(scene->observers_, observer);
      }
      if (output) {
        std::erase(scene->properties_, output);
        std::erase_if(scene->originProperties_, [&](const auto &record) {
          return record.property.lock() == output;
        });
      }
      if (live) {
        live->origin->retired = true;
        scene->pruneLiveText();
      }
    }
  };

  static std::shared_ptr<RefTarget> captionIdentity(const TextSource &source) {
    return source.target_;
  }

  static std::shared_ptr<RefTarget> captionTarget(std::shared_ptr<RefTarget> target, MountContext &context) {
    for (const auto &pending : context.pending) {
      if (pending.identity == target.get()) {
        return std::make_shared<RefTarget>(pending.target);
      }
    }
    return target;
  }

  static std::expected<void, ManagedError> finishCaptions(MountContext &context) {
    if (std::ranges::all_of(context.captions, &MountContext::CaptionFinalizer::completed)) {
      return {};
    }
    // Resolve dependencies once; iterative completion never consumes one stack frame per control.
    std::vector<std::optional<std::size_t>> dependencies(context.captions.size());
    for (std::size_t index = 0; index < context.captions.size(); ++index) {
      auto target = captionTarget(context.captions[index].source, context);
      if (!target) {
        return std::unexpected(error(context.captions[index].receiver, TreeError::CLOSED));
      }
      if (target->owner != std::this_thread::get_id()) {
        return std::unexpected(error(context.captions[index].receiver, TreeError::WRONG_THREAD));
      }
      for (std::size_t source = 0; source < context.captions.size(); ++source) {
        if (context.captions[source].receiver == target->node) {
          dependencies[index] = source;
          break;
        }
      }
    }
    auto remaining = std::ranges::count(context.captions, false, &MountContext::CaptionFinalizer::completed);
    while (remaining) {
      bool progressed{};
      for (std::size_t index = 0; index < context.captions.size(); ++index) {
        auto &caption = context.captions[index];
        if (caption.completed || (dependencies[index] && !context.captions[*dependencies[index]].completed)) {
          continue;
        }
        auto completed = caption.complete();
        if (!completed) {
          return completed;
        }
        caption.completed = true;
        --remaining;
        progressed = true;
      }
      if (!progressed) {
        const auto cycle = std::ranges::find(context.captions, false, &MountContext::CaptionFinalizer::completed);
        return std::unexpected(error(cycle->receiver, TreeError::INVALID_ARGUMENT));
      }
    }
    return {};
  }

  static std::expected<void, ManagedError> completeCaption(ManagedScene &scene, Node<Control> node,
                                                           const TextSource &source, MountContext &context,
                                                           const std::shared_ptr<ColdGraph> &graph) {
    auto target = captionTarget(source.target_, context);
    if (!target) {
      return std::unexpected(error(node, TreeError::CLOSED));
    }
    if (target->owner != std::this_thread::get_id()) {
      return std::unexpected(error(node, TreeError::WRONG_THREAD));
    }
    auto owner = eventScene(target);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (*owner != &scene) {
      return std::unexpected(error(node, TreeError::INVALID_ARGUMENT));
    }
    const bool existed = std::ranges::any_of(scene.liveText_, [&](const auto &record) {
      return record->origin->node == target->node && record->kind == static_cast<int>(source.kind_);
    });
    auto live =
      scene.attachLiveText(TextSource(target, source.kind_), PropertyOptions{}, std::source_location::current(), false);
    if (!live) {
      return std::unexpected(live.error());
    }
    if (!existed) {
      const auto created = std::ranges::find_if(scene.liveText_, [&](const auto &record) {
        return record->origin == live->state_.origin_;
      });
      if (created != scene.liveText_.end()) {
        graph->live = *created;
      }
    }
    // A shared capture can precede this mount; read the original identity for a fresh initial caption.
    auto initial = TextSource(target, source.kind_).snapshot();
    if (!initial) {
      return std::unexpected(initial.error());
    }
    auto applied = setControlText(scene.app_.tree(), node, std::move(*initial));
    if (!applied) {
      return std::unexpected(error(node, applied.error()));
    }
    auto observed = scene.observe(node, *live, [&scene, node](const GraphemeText &text) {
      return setControlText(scene.app_.tree(), node, text);
    });
    if (!observed) {
      return observed;
    }
    graph->observer = scene.observers_.back();
    graph->observer->operation = "text.caption";
    if (graph->pending) {
      --scene.coldMountPending_;
      graph->pending = false;
    }
    return {};
  }

  static std::expected<void, ManagedError> completeTextList(ManagedScene &scene, Node<VirtualList<TextSegment>> node,
                                                            const TextSource &source, bool splitWords,
                                                            TextSplitOptions options, MountContext &context,
                                                            const std::shared_ptr<ColdGraph> &graph,
                                                            ManagedViewHandler<TextSegment> prepared) {
    auto target = source.target_;
    for (const auto &pending : context.pending) {
      if (pending.identity == target.get()) {
        target = std::make_shared<RefTarget>(pending.target);
        break;
      }
    }
    if (target && target->owner != std::this_thread::get_id()) {
      return std::unexpected(error({}, TreeError::WRONG_THREAD));
    }
    if (!target) {
      return std::unexpected(error({}, TreeError::CLOSED));
    }
    const bool existed = std::ranges::any_of(scene.liveText_, [&](const auto &record) {
      return record->origin->node == target->node && record->kind == static_cast<int>(source.kind_);
    });
    auto resolved = TextSource(target, source.kind_);
    auto live = scene.attachLiveText(resolved, PropertyOptions{}, std::source_location::current(), false);
    if (!live) {
      return std::unexpected(live.error());
    }
    const auto existing = std::ranges::find_if(scene.liveText_, [&](const auto &record) {
      return record->origin == live->state_.origin_;
    });
    // Ownership is assigned by the finalizer that created the bridge, not by every shared consumer.
    if (!existed && existing != scene.liveText_.end()) {
      graph->live = *existing;
    }
    auto initialText = live->snapshot();
    if (!initialText) {
      return std::unexpected(error(target->node, initialText.error()));
    }
    if ((*initialText)->value.policy() != scene.widthPolicy()) {
      return std::unexpected(error(target->node, TreeError::INVALID_ARGUMENT));
    }
    auto admitted = scene.observerAdmission(node);
    if (!admitted) {
      return std::unexpected(admitted.error());
    }
    auto textFailure = std::make_shared<std::optional<TextError>>();
    auto derived = scene.derive(*live, [splitWords, options, textFailure](const GraphemeText &text) {
      *textFailure = portals::detail::textSplitLimit(text, options.limits);
      return splitWords ? portals::words(text, options) : portals::graphemes(text, options);
    });
    if (!derived) {
      auto failure = derived.error();
      failure.node = target->node.id();
      failure.text = *textFailure;
      return std::unexpected(failure);
    }
    graph->output = *derived->state_.lock();
    graph->observer = scene.observers_.back();
    graph->observer->textFailure = std::move(textFailure);
    graph->observer->operation = splitWords ? "text.words" : "text.graphemes";
    graph->observer->node = node;
    scene.originProperties_.push_back({live->state_.origin_, graph->output, node});
    // Private cold edges share origin retirement; ordinary public derived outputs remain independent.
    derived->state_.origin_ = live->state_.origin_;
    auto model =
      scene.collectionModel<TextSegment>(derived->state_, CollectionModelOptions{}, portals::detail::segmentRows);
    if (!model) {
      return std::unexpected(model.error());
    }
    auto owned = model->lock();
    if (!owned) {
      return std::unexpected(error(node, owned.error()));
    }
    graph->model = *owned;
    graph->model->receiver = node;
    auto range = (*owned)->model->openRange();
    if (!range) {
      return std::unexpected(error(node, range.error()));
    }
    auto installed = scene.app_.tree().edit(node, [&](VirtualList<TextSegment> &view) {
      view.consumer_ = std::move(*range);
    });
    if (!installed) {
      return std::unexpected(error(node, installed.error()));
    }
    auto attached = scene.attachView(
      node, *owned,
      [&scene, node](const ModelFrame<TextSegment> &frame, const LayoutSnapshot &layout) {
        return prepareVirtualList(scene.app_.tree(), node, frame, layout);
      },
      std::move(prepared));
    if (attached && graph->pending) {
      --scene.coldMountPending_;
      graph->pending = false;
    }
    return attached ? std::expected<void, ManagedError>{} : std::unexpected(attached.error());
  }

  /// @endcond

  /** @brief Build the shared checked data-presentation recipe for public overloads.
   * @tparam Content Retained body widget type.
   * @tparam Initial Owning presentation value or same-scene managed source.
   * @param initial Initial value or source retained by the consuming recipe.
   * @param content Single-use body recipe mounted in the same transaction.
   * @param config Owning construction-only labels, styles and retry action.
   * @param automatic Read the virtual body's prepared status when true.
   * @return Single-use composition preserving mount rollback and deferred reference publication.
   */
  template <class Content, class Initial>
  static Description<DataStateView> dataState(Initial initial, Description<Content> content, DataStateConfig config,
                                              bool automatic);

  /** @tparam Factory Owning checked callable accepting scene, parent and the shared mount context.
   * @param factory Internal adapter performing a complete existing recipe transaction.
   * @return Single-use erased recipe; no extra widget or lifecycle owner is created.
   */
  template <class Factory> static Description<Widget> transaction(Factory factory);

  /** @brief Erase only the observer type while preserving one mount transaction.
   * @tparam T Concrete public widget type produced by the existing recipe.
   * @param description Owning typed recipe consumed once.
   * @return Single-use recipe sharing rollback and deferred reference publication.
   */
  template <class T> static Description<Widget> erase(Description<T> description);

  /** @param scene Existing managed owner receiving a checked policy update.
   * @param node Live sized target created by this mount transaction.
   * @param change Owning operation; other axes and unrelated bounds are preserved.
   * @return Original managed boundary/tree/validation error, or success.
   */
  static std::expected<void, ManagedError> resize(ManagedScene &scene, Node<SizedWidget> node, SizeChange change) {
    const auto allowed = scene.boundary(ManagedPhase::MOUNT, node);

    if (!allowed) {
      return allowed;
    }

    SizePolicy policy;
    auto &tree = scene.app_.tree();
    const auto read = tree.read(node, [&](const SizedWidget &widget) {
      policy = widget.sizePolicy();
    });

    if (!read) {
      return std::unexpected(error(node, read.error()));
    }

    if (change.dimension == SizeDimension::WIDTH) {
      policy.width = {change.extent, change.extent, change.extent, SizeMode::FIXED, 1};
    } else if (change.dimension == SizeDimension::HEIGHT) {
      policy.height = {change.extent, change.extent, change.extent, SizeMode::FIXED, 1};
    } else {
      if (change.dimension != SizeDimension::FLEX_Y) {
        policy.width.mode = SizeMode::FLEX;
        policy.width.preferred = 0;
        policy.width.weight = change.weight;
      }

      if (change.dimension != SizeDimension::FLEX_X) {
        policy.height.mode = SizeMode::FLEX;
        policy.height.preferred = 0;
        policy.height.weight = change.weight;
      }
    }

    const auto changed = setSizePolicy(tree, node, policy);

    return changed ? std::expected<void, ManagedError>{} : std::unexpected(error(node, changed.error()));
  }

  /** @brief Verify a created node remains accessible before publishing references.
   * @param scene Existing managed
   * owner, borrowed for this check.
   * @param node Observing identity whose tree scope is validated.
   * @return
   * Success or the original managed boundary/liveness error.
   */
  static std::expected<void, ManagedError> check(ManagedScene &scene, Node<Widget> node) {
    return scene.boundary(ManagedPhase::MOUNT, node);
  }

  /** @param scene Existing owner with a completely mounted explicit or automatic host.
   * @param root Exact recipe root which must be that owner's registered host.
   * @return The existing typed host or INVALID_ARGUMENT; never creates a second host.
   */
  static std::expected<Node<OverlayHost>, ManagedError> layerHost(ManagedScene &scene, Node<Widget> root);

  /** @param scene Existing owner of the selected host and unpublished layer.
   * @param node Mounted recipe root; erased recipes must produce an exact bound Dialog.
   * @return Success for an existing registered Dialog or typed INVALID_ARGUMENT.
   */
  static std::expected<void, ManagedError> checkLayer(ManagedScene &scene, Node<Widget> node);
};
} // namespace detail

/** @tparam Child Consuming Description type, including layouts and final custom widgets.
 * @param child Owning single child transferred into this same mount transaction.
 * @param options Owning glyph selection and finite allocated-perimeter paint cap.
 * @param style Owning manual appearances beneath theme declarations.
 * @return Owning frame recipe; errors roll back the entire subtree without partial ref publication.
 */
template <detail::DescriptionInput Child>
Description<Border> border(Child &&child, BorderOptions options = {}, ControlStyle style = {});

/** @tparam Child Consuming ordinary content recipe.
 * @param child Single content root, mounted in the same rollback transaction.
 * @param options Owning placement/input options.
 * @param controller Shared values; empty creates default values automatically at mount.
 * @param style Manual appearance defaults beneath themes.
 * @return Automatically initialized, bound and prepared ordinary scroll owner.
 */
template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Child &&child, ScrollAreaOptions options = {},
                                   std::shared_ptr<ScrollController> controller = {}, ControlStyle style = {});

/** @brief Shared observing slot published only after a complete successful description mount.
 * @tparam T Observed public Widget subtype; no widget or scene ownership is retained.
 * Read/write the slot only on the UI thread. Copies share the slot. A published identity is
 * never automatically reset; checked tree access detects later retirement. Use a new slot
 * for another description. Construction can throw std::bad_alloc.
 */
template <class T> class NodeRef {
public:
  /** @brief Allocate an empty observing slot, with no widget ownership. */
  NodeRef() : slot_(std::make_shared<Slot>()) {
  }

  /** @return Empty before successful mount, otherwise the observing identity, possibly stale. */
  Node<T> node() const noexcept {
    return slot_ ? slot_->node : Node<T>{};
  }

private:
  friend struct detail::NamedAccess;
  friend class Description<T>;
  template <class U> friend class Ref;

  /** @brief Shared identity storage; never owns a widget or its scene. */
  struct Slot {
    Node<T> node;             ///< Published identity, empty until the complete mount succeeds.
    bool pending{};           ///< Reserved by an in-progress mount, including across reentrant calls.
    detail::RefTarget target; ///< Weak scene context for the additive fluent reference.
    bool fluent{};            ///< Only fluent references enforce their immutable construction-thread identity.
  };

  std::shared_ptr<Slot> slot_;
};

/** @brief Typed Widget observation; explicit composite specializations reuse their existing weak facades.
 * @tparam T Public Widget subtype, or a supported composite facade specialization for MessageDialog/ThemeSelector.
 * This primary template provides Widget capabilities; text() is available for Control subtypes and exactly LineEdit.
 * Construct on the receiving UI thread. Publish with Description::reference only after successful mount.
 * Copies share the observation and never own the scene/widget. Copy/destruction may occur on other
 * threads after publication, but operations reject them before reading mutable scene/slot state.
 * Synchronize transfer; publication must not race foreign-thread use. The low-level node() remains UI-only.
 * Construction allocates and can throw; moved-from references produce an empty text observation.
 */
template <class T> class Ref : public NodeRef<T> {
  friend struct detail::MessageAccess;

public:
  /** @brief Allocate an empty observing slot on the UI thread; may throw std::bad_alloc. */
  Ref() {
    this->slot_->fluent = true;
  }

  /** @param origin Operation site. @return Pure checked scope entry; already open is a no-op. */
  std::expected<void, ManagedError> open(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, HelpView>
  {
    return detail::helpCommand(target(), true, origin);
  }

  /** @param origin Operation site. @return Pure checked focus return; already closed is a no-op. */
  std::expected<void, ManagedError> close(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, HelpView>
  {
    return detail::helpCommand(target(), false, origin);
  }

  /** @param axis Selected logical axis.
   * @param offset Exact logical offset, clamped to the current range.
   * @param origin Command site retained by the scene collector.
   * @return Checked programmatic command or reported affinity/liveness/lease error.
   */
  std::expected<void, ManagedError> setOffset(ScrollAxis axis, ScrollOffset offset,
                                              std::source_location origin = std::source_location::current()) const
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return detail::scrollOffset(target(), axis, offset, origin);
  }

  /** @param metrics Exact standalone extent and viewport.
   * @param origin Command site retained by the scene collector.
   * @return Checked metrics command; an area projection is not a metrics writer.
   */
  std::expected<void, ManagedError> setMetrics(ScrollAxisMetrics metrics,
                                               std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, ScrollBar>
  {
    const auto observation = target();
    auto owner = detail::eventScene(observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    auto result = (*owner)->setScrollMetrics(this->node(), metrics);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result;
  }

  /** @param origin Request site retained by the scene collector.
   * @return Checked one-shot reveal admission; final rendering remains asynchronous.
   */
  std::expected<void, ManagedError> ensureVisible(std::source_location origin = std::source_location::current()) const {
    return detail::ensureVisible(target(), origin);
  }

  /** @return Owning final local/surface/clip geometry, or checked affinity/liveness/reentry failure.
   * Before final layout, while hidden, or after invalidation/rearrangement returns INVALID_GEOMETRY.
   * Does not perform layout or report errors; copies never borrow widgets or the scene.
   */
  std::expected<NodeGeometry, ManagedError> geometry() const {
    return detail::geometry(target());
  }

  /** @return Owning final local size or the exact geometry() error; never performs layout. */
  std::expected<CellSize, ManagedError> size() const {
    return geometry().transform([](const NodeGeometry &value) {
      return value.local.size();
    });
  }

  /** @tparam Callback Owning compact action receiving const NodeGeometry& or no arguments.
   * @param callback Void, checked TreeError or EditorCommandResult action retained by the scene.
   * @param origin Registration site retained in diagnostics.
   * @return Scene-owned registration or checked admission/budget/allocation error.
   * Initial valid and changed local/surface/clip bounds arrive on a subsequent UI dispatch.
   * Removal/close retires captures; an executing capture survives self-removal.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, NodeGeometry>
  std::expected<void, ManagedError> onLayout(Callback callback,
                                             std::source_location origin = std::source_location::current()) const {
    auto observation = target();
    auto owner = detail::eventScene(observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        return (*owner)->onLayout(this->node(), std::move(callback), origin);
      } catch (const std::bad_alloc &) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::NO_MEMORY, {}, {}});
      } catch (...) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::CALLBACK_FAILED, {}, {}});
      }
    }();
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result;
  }

  /** @tparam Callback Owning compact click action, optionally taking const ClickEvent&.
   * @param action Void, expected<Value, TreeError> or exact EditorCommandResult callback; success values are ignored.
   * @param origin Registration call site retained in diagnostics.
   * @return Checked registration on a mounted live button; CLOSED before publication/destruction,
   * WRONG_THREAD, STALE_NODE, duplicate INVALID_ARGUMENT or allocation error. Scene owns captures/token.
   * This adds one binding; it never replaces an existing one. Admitted calls survive self-removal.
   * Registration failures are also recorded in a live UI collector, without stopping the scene.
   * Internal callback transfers are checked; constructing the by-value argument before entry may throw.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::same_as<T, Button> && portals::detail::ManagedAction<Callback, ClickEvent>)
  std::expected<void, ManagedError> onClick(Callback action,
                                            std::source_location origin = std::source_location::current()) const {
    detail::commandOrigin(action, origin);
    auto observation = target();
    auto scene = detail::eventScene(observation);

    if (!scene) {
      return std::unexpected(scene.error());
    }

    // The forwarding move into the checked scene API is still part of this boundary.
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        return (*scene)->onClick(this->node(), std::move(action));
      } catch (const std::bad_alloc &) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::NO_MEMORY, {}, {}});
      } catch (...) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::CALLBACK_FAILED, {}, {}});
      }
    }();

    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }

    return result;
  }

  /** @param value Owning explicit presentation for an own-value composite; older generations are ignored.
   * @param origin Public call site for the existing short diagnostic collector.
   * @return Original checked TreeError; bound/automatic composites reject writes with INVALID_ARGUMENT.
   */
  std::expected<void, TreeError> display(DataPresentation value,
                                         std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, DataStateView>
  {
    return detail::displayDataState(target(), value, origin);
  }

  /** @param origin Focus request call site retained in managed diagnostics.
   * @return Checked focus/entry result; CLOSED before publication/after scene destruction,
   * WRONG_THREAD, STALE_NODE, typed eligibility or callback failure. No lifetime is extended.
   * Programmatic focus preserves the same editor blur and validation policies as keyboard focus.
   */
  std::expected<void, ManagedError> focus(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::focus, origin);
  }

  /** @param origin Activation call site retained by the existing callback error collector.
   * @return Checked activation; ignored failures in an admitted short callback remain reported once.
   */
  DialogCommandResult open(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Dialog>
  {
    return detail::dialogCommand(target(), true, origin);
  }

  /** @param origin Dismissal call site retained by the existing callback error collector.
   * @return Checked dismissal; a closed layer is a no-op and admitted callbacks never replay.
   */
  DialogCommandResult dismiss(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Dialog>
  {
    return detail::dialogCommand(target(), false, origin);
  }

  /** @return Effective activation, or exact affinity/lifetime/identity failure without reporting. */
  std::expected<bool, ManagedError> opened() const
    requires std::same_as<T, Dialog>
  {
    auto observation = target();
    auto owner = detail::eventScene(observation);
    return owner ? (*owner)->dialogOpened(observation->node) : std::unexpected(owner.error());
  }

  /** @param origin Focus-entry call site retained in managed diagnostics.
   * @return Checked focus-only scope entry, saving a return record; no input trap is installed.
   * Published callback errors retain entry and can be followed by leaveFocusScope().
   */
  std::expected<void, ManagedError>
  enterFocusScope(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::enterFocusScope, origin);
  }

  /** @param origin Focus-return call site retained in managed diagnostics.
   * @return Checked top-scope return; stale/unpublished/foreign-thread errors follow focus().
   * Owned failures are reported once, allowing use in ordinary void button callbacks.
   */
  std::expected<void, ManagedError>
  leaveFocusScope(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::leaveFocusScope, origin);
  }

  /** @param resolver Optional owning synchronous adapter used only for i18n expressions.
   * @param origin Property acquisition call site used by assignment/+= diagnostics.
   * @return Owning observing proxy, valid after Ref destruction; never extends scene/widget lifetime.
   * Resolver capture/storage allocation may throw before a mutation starts.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const
    requires(std::derived_from<T, Control> || std::same_as<T, LineEdit>)
  {
    auto target = this->slot_ ? std::shared_ptr<detail::RefTarget>(this->slot_, &this->slot_->target) : nullptr;
    constexpr std::string_view TYPE = std::same_as<T, LineEdit>     ? "LineEdit"
                                      : std::same_as<T, Label>      ? "Label"
                                      : std::same_as<T, Button>     ? "Button"
                                      : std::same_as<T, CheckBox>   ? "CheckBox"
                                      : std::same_as<T, ThreeState> ? "ThreeState"
                                                                    : "Control";

    return TextProperty(std::move(target), std::move(resolver), TYPE, origin);
  }

  /** @param change Owning policy override for every mutation of this proxy; does not change editor options.
   * @param resolver Optional owning synchronous resource adapter.
   * @param origin Property acquisition call site retained in diagnostics.
   * @return Observing editor proxy; RESET_BASELINE also clears equal-text history without redraw.
   */
  TextProperty text(ProgrammaticChange change, TextResolver resolver = {},
                    std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return TextProperty(target(), std::move(resolver), "LineEdit", origin, change);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked Undo result, with exact reporting provenance for managed button adapters.
   */
  EditorCommandResult undo(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, false, origin);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked Redo result; empty history is a no-op without callbacks or redraw.
   */
  EditorCommandResult redo(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), true, false, origin);
  }

  /** @brief Clear the visible draft as one user operation, applying the configured EditMode.
   * @param origin Command call site retained in diagnostics.
   * @return Checked technical result with reporting provenance; semantic validation/conflict uses LineEditResult.
   * Empty writable drafts are no-ops. Read-only/disabled editors return WOULD_BLOCK. Return the complete
   * EditorCommandResult from managed callbacks to preserve report-once behavior.
   */
  EditorCommandResult clear(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, false, origin, true);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked grouping boundary; does not change text or presentation.
   */
  EditorCommandResult stopGrouping(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, true, origin);
  }

  /** @return Read-only visible caption source; acquisition never reads or subscribes. */
  TextSource captionText() const
    requires std::derived_from<T, Control>
  {
    return TextSource(target(), TextSource::Kind::CAPTION);
  }

  /** @return Deferred physical body-row source; acquisition never reads or subscribes.
   * Retains the original observing slot, including when acquired before mount, without owning the scene.
   */
  auto visibleModel() const
    requires requires { typename detail::TableRowType<T>::Type; }
  {
    using Row = typename detail::TableRowType<T>::Type;
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;

    return VisibleModelSource<Row>(target(), std::move(node));
  }

  /** @return Read-only visible draft source, including uncommitted or semantically rejected text. */
  TextSource draftText() const
    requires std::same_as<T, LineEdit>
  {
    return TextSource(target(), TextSource::Kind::DRAFT);
  }

  /** @return Read-only latest accepted model source, independent of the editor's visible draft. */
  TextSource committedText() const
    requires std::same_as<T, LineEdit>
  {
    return TextSource(target(), TextSource::Kind::COMMITTED);
  }

  /** @return Effective checked availability without adoption, invalidation or error reporting. */
  std::expected<HistoryAvailability, TreeError> historyAvailability() const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorAvailability(target());
  }

  /** @return Effective Undo availability; false on checked errors without reporting. */
  bool canUndo() const
    requires std::same_as<T, LineEdit>
  {
    const auto value = historyAvailability();

    return value && value->canUndo;
  }

  /** @return Effective Redo availability; false on checked errors without reporting. */
  bool canRedo() const
    requires std::same_as<T, LineEdit>
  {
    const auto value = historyAvailability();

    return value && value->canRedo;
  }

private:
  friend struct detail::ClickAccess;
  friend struct detail::EditorEventAccess;
  friend class KeyInfo;
  friend struct detail::KeyHelpAccess;
  friend struct detail::LayoutAccess;
  friend struct detail::NamedAccess;
  friend class DynamicRef;
  template <class U> friend class Description;

  std::expected<void, ManagedError>
  focusOperation(std::expected<void, ManagedError> (ManagedScene::*operation)(Node<Widget>),
                 std::source_location origin) const {
    auto observation = target();
    auto owner = detail::eventScene(observation);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    auto result = ((*owner)->*operation)(this->node());

    if (!result) {
      detail::reportFocusError(observation, result.error(), origin);
    }

    return result;
  }

  std::shared_ptr<detail::RefTarget> target() const {
    return this->slot_ ? std::shared_ptr<detail::RefTarget>(this->slot_, &this->slot_->target) : nullptr;
  }
};

/** @brief Consuming metadata for one canonical annotated registration; refs resolve after complete mount. */
class KeyInfo {
public:
  /** @param label Owning required label. @param resolver Local resource resolver, never retained by a row. */
  explicit KeyInfo(TextValue label, TextResolver resolver = {}) : value_(std::move(label)) {
    value_.resolver = std::move(resolver);
  }

  /** @param text Owning explanation. @return This consuming metadata. */
  KeyInfo &&description(TextValue text) && {
    value_.description = std::move(text);
    return std::move(*this);
  }

  /** @param reference Deferred command slot. @return This consuming metadata. */
  KeyInfo &&reference(KeyBindingRef reference) && {
    value_.publication_ = std::move(reference.slot_);
    return std::move(*this);
  }

  /** @tparam T Widget type. @param reference Original owner observation. @return This consuming metadata. */
  template <class T>
    requires std::derived_from<T, Widget>
  KeyInfo &&owner(const Ref<T> &reference) && {
    owner_ = reference.target();
    return std::move(*this);
  }

  /** @tparam T Widget type. @param reference Declared scope observation. @return This consuming metadata. */
  template <class T>
    requires std::derived_from<T, Widget>
  KeyInfo &&scope(const Ref<T> &reference) && {
    scope_ = reference.target();
    return std::move(*this);
  }

  /** @param value New admission state. @return This consuming metadata. */
  KeyInfo &&enabled(bool value) && {
    value_.enabled = value;
    return std::move(*this);
  }

  /** @param value New visibility/admission state. @return This consuming metadata. */
  KeyInfo &&visible(bool value) && {
    value_.visible = value;
    return std::move(*this);
  }

private:
  friend class Scene;

  std::expected<portals::KeyInfo, ManagedError> resolve(ManagedScene &scene) {
    for (const auto &target : {owner_, scope_}) {
      if (!target) {
        continue;
      }
      auto owner = detail::eventScene(target);
      if (!owner) {
        return std::unexpected(owner.error());
      }
      if (*owner != &scene) {
        return std::unexpected(
          ManagedError{ManagedPhase::BIND, target->node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }
    }
    if (owner_) {
      value_.owner = owner_->node;
    }
    if (scope_) {
      value_.scope = scope_->node;
    }
    return std::move(value_);
  }

  portals::KeyInfo value_;
  std::shared_ptr<detail::RefTarget> owner_, scope_;
};

/** @param label Literal bytes copied now. @return Owning consuming metadata. */
inline KeyInfo keyInfo(std::string_view label) {
  return KeyInfo(TextValue(label));
}

/** @param label Owning expression. @param resolver Local resolver. @return Owning consuming metadata. */
inline KeyInfo keyInfo(TextValue label, TextResolver resolver = {}) {
  return KeyInfo(std::move(label), std::move(resolver));
}

namespace detail {
/** @brief Internal checked named publication bridge; no widget or scene ownership escapes. */
struct NamedAccess {
  /** @param stage Existing owning style payload.
   * @param target Weak exact-node observation.
   * @return Independent scoped stage using the same existing style implementation.
   */
  static StyleStage scopeStyle(const StyleStage &stage, std::shared_ptr<RefTarget> target);

  /** @param reference Dynamic observation.
   * @return Shared weak target with no scene ownership.
   */
  static std::shared_ptr<RefTarget> target(const DynamicRef &reference);

  /** @param scene Receiving managed owner.
   * @param target Shared observation, checked before reading mutable fields.
   * @return Exact node scope or typed thread/owner/liveness failure.
   */
  static std::expected<NodeId, ManagedError> scope(ManagedScene &scene, const std::shared_ptr<RefTarget> &target) {
    auto owner = eventScene(target);

    if (!owner) {
      auto failure = owner.error();
      failure.phase = ManagedPhase::STYLE;

      return std::unexpected(failure);
    }

    if (*owner != &scene) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, target->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }

    auto live = scene.boundary(ManagedPhase::STYLE, target->node);

    return live ? std::expected<NodeId, ManagedError>(target->node.id()) : std::unexpected(live.error());
  }

  /** @param scene Receiving managed owner.
   * @param source Original dynamic observation, never resolved again by name.
   * @return Original live identity, or exact prior lookup/type/liveness/owner error.
   */
  static std::expected<Node<Widget>, ManagedError> observe(ManagedScene &scene,
                                                           const std::shared_ptr<RefTarget> &source) {
    auto owner = eventScene(source);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, source->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }

    auto allowed = scene.boundary(ManagedPhase::BIND, source->node);

    return allowed ? std::expected<Node<Widget>, ManagedError>(source->node) : std::unexpected(allowed.error());
  }

  /** @param scene Borrowed live managed owner.
   * @param name Case-sensitive ID; empty, absent and duplicate names reject.
   * @return Unique live managed identity, or exact boundary/name error.
   */
  static std::expected<Node<Widget>, ManagedError> find(ManagedScene &scene, std::string_view name) {
    auto allowed = scene.boundary(ManagedPhase::BIND, scene.root_);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    Node<Widget> found;

    for (const auto &entry : scene.entries_) {
      if (entry->removed || !entry->target || entry->target->id != name || name.empty()) {
        continue;
      }

      auto live = scene.app_.tree().read(entry->node, [](const Widget &) {
      });

      if (!live && (live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED)) {
        continue;
      }

      if (!live) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, entry->node.id(), live.error(), {}, {}});
      }

      if (found != Node<Widget>{}) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, entry->node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }

      found = entry->node;
    }

    if (found == Node<Widget>{}) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    }

    return found;
  }

  /** @tparam T Public Widget subtype checked with RTTI before constructing its observer.
   * @param reference Empty shared observing slot to publish.
   * @param scene Existing owner, borrowed only during checked validation.
   * @param node Unique name result; no unchecked downcast is exposed.
   * @return Success after one publication or INVALID_ARGUMENT for a wrong type.
   */
  template <class T>
  static std::expected<void, ManagedError> publish(const Ref<T> &reference, ManagedScene &scene, Node<Widget> node) {
    auto &slot = *reference.slot_;
    bool compatible{};
    Node<Control> caption;
    Node<LineEdit> draft;
    auto checked = scene.app_.tree().read(node, [&](const Widget &widget) {
      if constexpr (std::same_as<T, Widget>) {
        compatible = true;
      } else {
        compatible = dynamic_cast<const T *>(&widget) != nullptr;
      }

      if (dynamic_cast<const Control *>(&widget)) {
        caption = Node<Control>(node.id());
      }

      if (dynamic_cast<const LineEdit *>(&widget)) {
        draft = Node<LineEdit>(node.id());
      }
    });

    if (!checked || !compatible) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, node.id(), checked ? TreeError::INVALID_ARGUMENT : checked.error(), {}, {}});
    }

    slot.node = Node<T>(node.id());
    slot.target.scene = &scene;
    slot.target.lifetime = scene.lifetime_;
    slot.target.node = node;
    slot.target.caption = caption;
    slot.target.draft = draft;

    return {};
  }

  /** @tparam T Observed public widget type.
   * @param reference Shared slot retaining this request's original failure.
   * @param failure Exact failure copied without widget access.
   */
  template <class T> static void reject(const Ref<T> &reference, const ManagedError &failure) {
    reference.slot_->target.failure = std::make_shared<const ManagedError>(failure);
  }

  /** @param scene Existing collector.
   * @param failure Exact cause stamped with short-error provenance.
   * @param source Owned name and static expected-type diagnostic.
   */
  static void report(ManagedScene &scene, ManagedError &failure, const LookupSource &source) {
    scene.reportShortError(failure, "scene.get", source.name, source.expectedType, source.origin);
  }

  /** @tparam T Observed widget subtype.
   * @param reference Existing typed observation.
   * @return Shared weak target, never scene or widget ownership.
   */
  template <class T> static std::shared_ptr<RefTarget> target(const Ref<T> &reference) {
    return reference.target();
  }

  /** @tparam T Public Widget subtype required by this request.
   * @param request Name and weak facade identity, copied through fallible owning transfers.
   * @return Typed slot with deferred publication or its retained admission failure.
   */
  template <class T> static Ref<T> request(const LookupSource &request) {
    Ref<T> reference;
    LookupSource source(request);

    source.expectedType = typeid(T).name();

    auto result =
      requestLookup(source, [reference](ManagedScene &scene, std::expected<Node<Widget>, ManagedError> node) {
        auto published =
          node ? publish(reference, scene, *node) : std::expected<void, ManagedError>(std::unexpected(node.error()));

        if (!published) {
          reject(reference, published.error());
        }

        return published;
      });

    if (!result) {
      reject(reference, result.error());
    }

    return reference;
  }
};
} // namespace detail

/** @brief Dynamic named observation with checked caption/editor and Button capabilities.
 * Copies own the request name and observing slot; they never retain a scene/widget.
 * Converting to Ref<T> checks the same published NodeId, or waits for this slot before mount.
 */
class DynamicRef {
public:
  /** @tparam T Public Widget subtype tested during deferred/immediate name resolution.
   * @return Typed weak slot; wrong type records INVALID_ARGUMENT before startup or in the live collector.
   * Conversion construction may allocate. A successful slot never retargets on later ID reuse.
   */
  template <class T>
    requires std::derived_from<T, Widget>
  operator Ref<T>() const {
    auto source = source_;

    source.observation = detail::NamedAccess::target(reference_);

    return detail::NamedAccess::request<T>(source);
  }

  /** @param resolver Optional owning synchronous localization adapter.
   * @param origin Property acquisition call site retained for short mutations.
   * @return Existing checked text proxy; non-text widgets return INVALID_ARGUMENT.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const {
    return TextProperty(reference_.target(), std::move(resolver), "Widget", origin);
  }

  /** @tparam Callback Owning compact Button action accepted by Ref<Button>::onClick.
   * @param callback Action transferred into the existing managed binding.
   * @param origin Registration call site.
   * @return Checked Button registration result; use a pipe stage for registration before preparation.
   */
  template <class Callback = detail::QuitCommand>
    requires portals::detail::ManagedAction<Callback, ClickEvent>
  std::expected<void, ManagedError> onClick(Callback callback,
                                            std::source_location origin = std::source_location::current()) const {
    Ref<Button> typed = *this;

    return typed.onClick(std::move(callback), origin);
  }

  /** @return Observing published identity, empty before mount; UI-thread only. */
  Node<Widget> node() const noexcept {
    return reference_.node();
  }

private:
  friend class Scene;
  friend struct detail::NamedAccess;

  explicit DynamicRef(const detail::LookupSource &source)
      : source_(source), reference_(detail::NamedAccess::request<Widget>(source_)) {
  }

  detail::LookupSource source_;
  Ref<Widget> reference_;
};

inline std::shared_ptr<detail::RefTarget> detail::NamedAccess::target(const DynamicRef &reference) {
  return reference.reference_.target();
}

// The public contract is documented on Scene::get in ui_scene.hpp.
template <class T>
  requires std::derived_from<T, Widget>
Ref<T> Scene::get(std::string_view name, std::source_location origin) {
  return detail::NamedAccess::request<T>({ref().anchor_, owner_, std::string(name), origin});
}

namespace detail {
/** @brief Checked adapter to managed-owned layout observations; never owns a widget or scene. */
struct LayoutAccess {
  /** @tparam T Observed widget type.
   * @tparam Callback Owning compact geometry action.
   * @param scene Existing receiving managed owner.
   * @param reference Immutable-affinity checked observing reference.
   * @param callback Action transferred after scene identity admission.
   * @param origin Original registration site.
   * @return Success or exact checked admission/registration failure.
   */
  template <class T, class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<T> &reference, Callback callback,
                                                  std::source_location origin) {
    auto owner = eventScene(reference.target());
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }
    return scene.onLayout(reference.node(), std::move(callback), origin);
  }
};

/** @brief Checked adapter to the managed-owned click chain; never owns a widget or scene. */
struct ClickAccess {
  /** @tparam Callback Owning no-argument or ClickParams callback with a managed action result.
   * @param scene Existing owner in which the reference must have been published.
   * @param reference Observing button identity, read only after immutable affinity admission.
   * @param callback Owning callable transferred into the bounded managed registry.
   * @param origin Original registration site retained for callback failures.
   * @return Success or exact affinity, identity, registration or allocation error.
   */
  template <class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<Button> &reference, Callback callback,
                                                  std::source_location origin) {
    commandOrigin(callback, origin);
    auto owner = eventScene(reference.target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    try {
      return scene.composeButtonClick(reference.node(), scene.clickAdapter(std::move(callback), origin), origin);
    } catch (const std::bad_alloc &) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::NO_MEMORY, {}, {}});
    } catch (...) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::CALLBACK_FAILED, {}, {}});
    }
  }
};

/** @brief Checked registration bridge to the editor's existing managed-owned binding. */
struct EditorEventAccess {
  /** @tparam Event TextEditedEvent or EditingFinishedEvent.
   * @tparam Callback Owning managed action receiving the event or no arguments.
   * @param scene Existing scene in which the editor reference was published.
   * @param reference Observing identity; no widget or scene ownership is retained.
   * @param callback Owning callable transferred to the combined finite editor registry.
   * @param origin Original registration site retained in failure diagnostics.
   * @return Exact affinity, identity, budget or checked allocation error.
   */
  template <class Event, class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<LineEdit> &reference,
                                                  Callback callback, std::source_location origin) {
    commandOrigin(callback, origin);
    auto owner = eventScene(reference.target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    if constexpr (std::same_as<Event, TextEditedEvent>) {
      return scene.onTextEdited(reference.node(), std::move(callback), origin);
    } else {
      return scene.onEditingFinished(reference.node(), std::move(callback), origin);
    }
  }
};

/** @brief Admit callbacks borrowing the mutable chain context, or ordinary no-argument callbacks.
 * @tparam Callback Copyable callable whose result is void, checked TreeError or an editor receipt.
 */
template <class Callback>
concept ClickCallback = std::copy_constructible<Callback> && (requires(Callback &callback, ClickParams &params) {
                          { callback(params) } -> portals::detail::ManagedActionResult;
                        } || (!std::invocable<Callback &, ClickParams &> && requires(Callback &callback) {
                                                                { callback() } -> portals::detail::ManagedActionResult;
                                                              }));
} // namespace detail

/** @brief Owning setup action invoked once after construction, before children.
 * @tparam T Concrete Widget type observed by the action.
 * Arguments are borrowed for the invocation. External effects and separately created scene
 * state are not rolled back; the attempted description subtree is removed on failure.
 */
template <class T>
using Setup = portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<T>)>;

/** @brief Owning optional editor customization and confirmation policy. */
struct LineEditConfig {
  std::function<std::expected<void, TreeError>(LineEditResult)> result{}; ///< Optional callback after standard editing.
  LineEditOptions options{}; ///< Editor limits and appearances; scene width policy overrides options.widthPolicy.
  SizePolicy policy{};       ///< Owning layout constraints validated by the existing layout engine.
  std::shared_ptr<Clipboard> clipboard{};           ///< Optional UI-owned clipboard adapter retained by the binding.
  std::function<bool(std::string_view)> validate{}; ///< Optional publication validator; false retains visible draft.
};

/** @brief Move-only recipe whose strings, arguments, children and callbacks survive their sources.
 * @tparam T Public Widget subtype. Factories enforce constructor compatibility when instantiated.
 * Factory/modifier allocations and capture construction may throw before mount begins. Mount
 * translates internal exceptions, consumes the entire recipe even on failure, and transfers
 * registered callbacks to the existing ManagedScene. Destruction never changes mounted UI.
 */
template <class T> class Description {
public:
  /** @brief Construct an empty recipe; mounting it returns INVALID_ARGUMENT. */
  Description() = default;

  /** @brief Transfer the unnamed recipe; its source becomes empty. */
  Description(Description &&) noexcept = default;

  /** @brief Replace this recipe with the unnamed source, which becomes empty.
   * @return This recipe. Destroying a replaced recipe has no mounted UI effect.
   */
  Description &operator=(Description &&) noexcept = default;

  Description(const Description &) = delete;

  Description &operator=(const Description &) = delete;

  /** @param patch Owning sparse instance overrides; false, zero and builtin-equal values remain explicit.
   * @return This consuming recipe. A later settings call replaces the previous instance patch.
   * Supported standard factories validate the patch before their constructor or resolver runs.
   */
  Description &&settings(ControlSettings patch) && {
    if (state_) {
      state_->controlPatch = std::move(patch);
      state_->settingsRequested = true;

      if constexpr (std::same_as<T, Grid>) {
        if (state_->layout && state_->layout->gridContainer) {
          gridControl().patch = state_->controlPatch;
          gridControl().settingsRequested = true;
          declarativeOptions();
        }
      }
    }
    return std::move(*this);
  }

  /** @param type Owning name resolved from the explicitly supplied immutable mount context.
   * @return This consuming typed recipe; a different concrete base rejects mount before construction.
   * Runtime names preserve the existing C++ type and do not synthesize a new widget class.
   */
  Description &&variant(std::string_view type) && {
    if (state_) {
      state_->controlVariant.emplace(type);

      if constexpr (std::same_as<T, Grid>) {
        if (state_->layout && state_->layout->gridContainer) {
          gridControl().variant.emplace(type);
          declarativeOptions();
        }
      }
    }
    return std::move(*this);
  }

  /** @param reference Empty observing slot; duplicate slots in one mount are rejected.
   * @return This consuming recipe for chaining. The slot remains empty on mount failure.
   */
  Description &&reference(NodeRef<T> reference) && {
    return std::move(*this).storeReference(std::move(reference));
  }

  /** @tparam U Public widget type of the ordinary-body anchor.
   * @param reference Weak observing slot resolved after the entire cold mount, before publication.
   * @param placement Preferred ABOVE_START/BELOW_START position; CENTER retains centered placement.
   * @return Consuming dialog recipe; foreign/dialog-owned/stale anchors fail without partial publication.
   */
  template <class U>
  Description &&anchor(const Ref<U> &reference, DialogPlacement placement = DialogPlacement::BELOW_START) &&
    requires std::same_as<T, Dialog>
  {
    if (state_) {
      state_->anchor = reference.target();
      state_->placement = placement;
      state_->anchorRequested = true;
    }
    return std::move(*this);
  }

  /** @tparam Callback Copyable void or expected<void,TreeError> callback taking DialogDismissReason.
   * @param callback Owning action admitted once after published dismissal; teardown invokes no action.
   * @return Consuming dialog recipe using the scene-owned callback lifetime.
   */
  template <class Callback>
  Description &&onDismiss(Callback callback) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onDismiss(node, std::move(callback));
    });
  }

  /** @param target Owning style identity, staged through ManagedScene::style.
   * @return This consuming recipe for chaining.
   */
  Description &&style(StyleTarget target) && {
    if (state_ && state_->id) {
      if (target.id.empty()) {
        target.id = *state_->id;
      } else if (!target.id.empty()) {
        state_->id = target.id;
      }
    }

    return std::move(*this).setup([target = std::move(target)](ManagedScene &scene, Node<T> node) mutable {
      return scene.style(node, std::move(target));
    });
  }

  /** @param value Case-sensitive style id copied into this recipe; type/classes are preserved.
   * @return Consuming recipe; id is assigned at mount in modifier order, before later styles.
   * A later style with an empty id preserves it; an explicit nonempty style id replaces it.
   */
  Description &&id(std::string_view value) && {
    if (state_) {
      state_->id.emplace(value);
    }

    return std::move(*this).setup([value = detail::captureText(value)](ManagedScene &scene, Node<T> node) {
      return scene.id(node, detail::captionView(value));
    });
  }

  /** @tparam Callback Compact owning click callback, optionally receiving ClickEvent.
   * @param callback Void, expected<Value, TreeError> or EditorCommandResult action; success values are ignored.
   * @return Consuming button recipe using one existing managed binding; duplicates fail at mount.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::same_as<T, Button> && portals::detail::ManagedAction<Callback, ClickEvent>)
  Description &&onClick(Callback callback) && {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onClick(node, std::move(callback));
    });
  }

  /** @tparam Value Owning equality-comparable managed state value.
   * @tparam Callback Copyable presentation callback taking const Value&, returning void or expected<void, TreeError>.
   * @param model State owned by the receiving scene.
   * @param callback Owning action, first delivered after the entire lazy mount and reference publication.
   * @return Consuming recipe; the scene owns the subscription after successful mount.
   */
  template <std::equality_comparable Value, class Callback>
    requires std::copy_constructible<Callback> &&
             (std::same_as<std::invoke_result_t<Callback &, const Value &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const Value &>, std::expected<void, TreeError>>)
  Description &&observe(ManagedState<Value> model, Callback callback) && {
    return std::move(*this).setup([model, callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.observe(node, model, std::move(callback));
    });
  }

  /** @tparam Value Owning derived source value.
   * @tparam Callback Copyable presentation callback taking const Value& and returning void or expected<void,
   * TreeError>.
   * @param source Read-only source owned by the receiving scene.
   * @param callback Owning action first delivered after complete mount and Ref publication.
   * @return Consuming recipe with the existing scene-owned subscription and checked failures.
   */
  template <std::equality_comparable Value, class Callback>
    requires std::copy_constructible<Callback> &&
             (std::same_as<std::invoke_result_t<Callback &, const Value &>, void> ||
              std::same_as<std::invoke_result_t<Callback &, const Value &>, std::expected<void, TreeError>>)
  Description &&observe(ManagedSource<Value> source, Callback callback) && {
    return std::move(*this).setup([source, callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.observe(node, source, std::move(callback));
    });
  }

  /** @param callback Existing managed custom event adapter; standard bindings reject duplicates.
   * @return This consuming recipe for chaining; the scene owns the callback after mounting.
   */
  Description &&onEvent(ManagedEventHandler<T> callback) && {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onEvent(node, std::move(callback));
    });
  }

  /** @param callback Existing managed dirty-frame adapter; no new polling or runtime is introduced.
   * @return This consuming recipe for chaining.
   */
  Description &&onPrepare(ManagedPrepareHandler<T> callback) && {
    return std::move(*this).setup([callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
      return scene.onPrepare(node, std::move(callback));
    });
  }

  /** @tparam Callback Owning copyable callback returning expected<Value, TreeError> with non-void Value.
   * @param callback Existing dirty-frame work; its success value is ignored inside the entered scene scope.
   * @return This consuming recipe; errors are unchanged and no additional invalidation is requested.
   */
  template <class Callback>
    requires std::copy_constructible<Callback> &&
             portals::detail::ManagedTreeResult<std::invoke_result_t<Callback &, T &, Node<Widget>>> &&
             (!std::same_as<std::invoke_result_t<Callback &, T &, Node<Widget>>, std::expected<void, TreeError>>)
  Description &&onPrepare(Callback callback) && {
    ManagedPrepareHandler<T> normalized;
    bool present = true;

    if constexpr (std::is_pointer_v<Callback> || requires { callback.operator bool(); }) {
      present = static_cast<bool>(callback);
    }

    if (present) {
      normalized = [callback = std::move(callback)](T &widget, Node<Widget> focused) mutable {
        return portals::detail::invokeIgnoringValue(callback, widget, focused);
      };
    }

    return std::move(*this).onPrepare(std::move(normalized));
  }

  /** @param cells Nonnegative fixed width; replaces only the horizontal bounds/mode.
   * @return Consuming recipe; invalid cells fail at mount without publishing refs.
   */
  Description &&width(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::WIDTH, cells});
  }

  /** @param cells Nonnegative fixed height; replaces only the vertical bounds/mode.
   * @return Consuming recipe; invalid cells fail at mount without publishing refs.
   */
  Description &&height(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::HEIGHT, cells});
  }

  /** @param count Positive fixed column count; row-major flow grows rows within the configured budget.
   * @return Consuming standard grid recipe; invalid relationships fail before mount effects.
   */
  Description &&columns(std::size_t count) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions().columns = count;
      gridControl().arguments.grid.columns = count;
    }

    return std::move(*this);
  }

  /** @param count Positive fixed row count; column-major flow grows columns within the configured budget.
   * @return Consuming standard grid recipe; explicit trailing rows remain present.
   */
  Description &&rows(std::size_t count) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions().rows = count;
      gridControl().arguments.grid.rows = count;
    }

    return std::move(*this);
  }

  /** @param value Forward placement direction; COLUMN_MAJOR requires rows.
   * @return Consuming standard grid recipe; unknown directions fail during preflight.
   */
  Description &&flow(GridFlow value) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions().flow = value;
      gridControl().arguments.grid.flow = value;
    }

    return std::move(*this);
  }

  /** @param cells Nonnegative terminal cells between columns.
   * @return Consuming standard grid recipe; a column or row count is still required.
   */
  Description &&columnGap(std::int32_t cells) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions().columnGap = cells;
      gridControl().arguments.grid.columnGap = cells;
    }

    return std::move(*this);
  }

  /** @param cells Nonnegative terminal cells between rows.
   * @return Consuming standard grid recipe, with spanning gaps included inside its cells.
   */
  Description &&rowGap(std::int32_t cells) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions().rowGap = cells;
      gridControl().arguments.grid.rowGap = cells;
    }

    return std::move(*this);
  }

  /** @param options Complete owning declaration, including finite work budgets; replaces prior grid modifiers.
   * @return Consuming standard grid recipe; relationships remain independent from control defaults.
   */
  Description &&gridOptions(DeclarativeGridOptions options) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions() = options;
      auto &arguments = gridControl().arguments.grid;

      arguments.columns = options.columns;
      arguments.rows = options.rows;
      arguments.flow = options.flow;
      arguments.columnGap = options.columnGap;
      arguments.rowGap = options.rowGap;
      arguments.paddingLeft = options.padding.left;
      arguments.paddingTop = options.padding.top;
      arguments.paddingRight = options.padding.right;
      arguments.paddingBottom = options.padding.bottom;
      arguments.maxTracksPerAxis = options.maxTracksPerAxis;
      arguments.maxPlacements = options.maxPlacements;
      arguments.maxPlacementWork = options.maxPlacementWork;
    }

    return std::move(*this);
  }

  /** @param presentation Owning region/row backgrounds and finite paint budget.
   * @return Consuming standard grid recipe; backgrounds never inherit into child control state styles.
   */
  Description &&presentation(GridPresentation presentation) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      layoutMetadata().presentation = presentation;
    }

    return std::move(*this);
  }

  /** @param policies Track policies copied now; length must equal the resolved shared column count.
   * @return Consuming declarative grid recipe; invalid policies/counts fail before mount effects.
   */
  Description &&columnSizes(const std::vector<AxisSize> &policies) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions();
      layoutMetadata().columnSizes = std::make_unique<std::vector<AxisSize>>(policies);
    }

    return std::move(*this);
  }

  /** @param policies Track policies copied now; length must equal the resolved BODY row count.
   * @return Consuming declarative recipe. Header rows keep independent inferred policies.
   * Invalid policies/counts fail before mount effects; this method does not set the row count.
   */
  Description &&rowSizes(const std::vector<AxisSize> &policies) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions();
      layoutMetadata().rowSizes = std::make_unique<std::vector<AxisSize>>(policies);
    }

    return std::move(*this);
  }

  /** @param controller Shared owning values; empty uses an independent default controller.
   * @return Consuming standard grid, enabling one scroll owner before mount without a new Widget or Tab stop.
   */
  Description &&scroll(std::shared_ptr<ScrollController> controller = {}) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions();
      auto &layout = layoutMetadata();

      if (!layout.scroll) {
        layout.scroll.emplace();
      }

      if (controller) {
        layout.scroll->controller = std::move(controller);
      }

      gridControl().arguments.grid.scroll = true;
    }

    return std::move(*this);
  }

  /** @param options Complete existing ScrollArea settings; focusable=true explicitly adds a focus stop.
   * @return Consuming standard grid with scrolling enabled before mount; retains a supplied controller.
   */
  Description &&scrollOptions(ScrollAreaOptions options) &&
    requires std::same_as<T, Grid>
  {
    if (state_) {
      declarativeOptions();
      auto &layout = layoutMetadata();

      if (!layout.scroll) {
        layout.scroll.emplace();
      }

      layout.scroll->area = options;
      gridControl().arguments.grid.scroll = true;
    }

    return std::move(*this);
  }

  /** @param options Complete parent-child placement; replaces prior relationship modifiers.
   * @return Consuming recipe; only an immediate declarative Grid parent accepts this relationship.
   */
  Description &&gridItem(GridItemOptions options) && {
    if (state_) {
      layoutMetadata().item = options;
      state_->layout->itemRequested = true;
    }

    return std::move(*this);
  }

  /** @param index Zero-based local row; column must also be explicit.
   * @return Consuming child recipe; a partial coordinate fails before mount effects.
   */
  Description &&row(std::size_t index) && {
    if (state_) {
      layoutMetadata().item.row = index;
      state_->layout->itemRequested = true;
    }

    return std::move(*this);
  }

  /** @param index Zero-based local column; row must also be explicit.
   * @return Consuming child recipe; this does not alter an ordinary row container.
   */
  Description &&column(std::size_t index) && {
    if (state_) {
      layoutMetadata().item.column = index;
      state_->layout->itemRequested = true;
    }

    return std::move(*this);
  }

  /** @param count Positive number of occupied rows, including intervening gaps.
   * @return Consuming child recipe; hidden children retain the same structural span.
   */
  Description &&rowSpan(std::size_t count) && {
    if (state_) {
      layoutMetadata().item.rowSpan = count;
      state_->layout->itemRequested = true;
    }

    return std::move(*this);
  }

  /** @param count Positive number of occupied columns, including intervening gaps.
   * @return Consuming child recipe; overlap is rejected in declarative Grid mode.
   */
  Description &&columnSpan(std::size_t count) && {
    if (state_) {
      layoutMetadata().item.columnSpan = count;
      state_->layout->itemRequested = true;
    }

    return std::move(*this);
  }

  /** @param weight Positive horizontal flex weight; zero preferred basis, existing bounds and vertical axis are
   * preserved.
   * @return Consuming recipe; zero weight fails at checked mount.
   */
  Description &&flexX(std::uint32_t weight = 1) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::FLEX_X, 0, weight});
  }

  /** @param weight Positive vertical flex weight; zero preferred basis, existing bounds and horizontal axis are
   * preserved.
   * @return Consuming recipe; zero weight fails at checked mount.
   */
  Description &&flexY(std::uint32_t weight = 1) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::FLEX_Y, 0, weight});
  }

  /** @param weight Positive flex weight for both axes, with zero preferred bases and existing bounds preserved.
   * @return Consuming recipe; zero weight fails at checked mount. A prior fixed bound still caps flex growth.
   */
  Description &&flex(std::uint32_t weight = 1) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).resize({detail::SizeDimension::FLEX, 0, weight});
  }

  /** @param callback Owning customization action, run in modifier order before children.
   * Capture the existing Application explicitly when a low-level tree operation is needed.
   * @return This consuming recipe for chaining. Empty callbacks fail at mount.
   */
  Description &&setup(Setup<T> callback) && {
    if (state_) {
      state_->setup.push_back(std::move(callback));
    }

    return std::move(*this);
  }

  /** @brief Declare this subtree as a nested focus scope, with scene-owned lifetime.
   * @return This consuming recipe; mount registers before child defaults and rolls back on failure.
   */
  Description &&focusScope() && {
    if (state_) {
      state_->focusScope = true;
    }

    return std::move(*this);
  }

  /** @brief Prefer this node when entering its nearest scope without eligible history.
   * @return This consuming recipe; mount fails with INVALID_ARGUMENT without an enclosing scope.
   * With multiple defaults in one scope, the last declared preorder default wins.
   */
  Description &&focusDefault() && {
    if (state_) {
      state_->focusDefault = true;
    }

    return std::move(*this);
  }

private:
  friend struct detail::Access;
  template <detail::DescriptionInput Child> friend Description<Border> border(Child &&, BorderOptions, ControlStyle);
  template <detail::DescriptionInput Child>
  friend Description<ScrollArea> scrollArea(Child &&, ScrollAreaOptions, std::shared_ptr<ScrollController>,
                                            ControlStyle);

  template <class U, class... Args> friend Description<U> custom(Args &&...args);
  template <class U> friend Description<U> control(std::string_view, ControlStyle, SizePolicy);
  template <class U, detail::DescriptionInput... Children> friend Description<U> linear(LinearOptions, Children &&...);
  template <class U, detail::DescriptionRange R> friend Description<U> linear(LinearOptions, R &&);
  template <detail::GridInput... Children> friend Description<Grid> grid(Children &&...);
  friend Description<LineEdit> lineEdit(ManagedState<std::string>, LineEditConfig);

  /** @brief Owning constructor adapter using the existing checked managed mount. */
  using Factory = portals::detail::MoveOnlyFunction<std::expected<Node<T>, ManagedError>(ManagedScene &, Node<Widget>,
                                                                                         detail::MountContext &)>;
  /** @brief Owning child recipe adapter sharing the parent's transaction bookkeeping. */
  using Child = portals::detail::MoveOnlyFunction<std::expected<void, ManagedError>(ManagedScene &, Node<Widget>,
                                                                                    detail::MountContext &)>;

  /** @brief Unmounted recipe storage, consumed and destroyed during mount. */
  struct State {
    Factory create;                                                  ///< Constructs exactly one managed node.
    std::vector<Setup<T>> setup = std::vector<Setup<T>>(0);          ///< Customization actions in modifier order.
    std::vector<Child> children = std::vector<Child>(0);             ///< Consuming child adapters in insertion order.
    std::vector<NodeRef<T>> references = std::vector<NodeRef<T>>(0); ///< Observing slots reserved for this node.
    std::shared_ptr<detail::RecipeLayout> layout; ///< Optional cold relationship metadata; never mounted state.
    bool focusScope{};                            ///< Register before setup/children; scene owns its token.
    bool focusDefault{};                          ///< Set the nearest registered scope's default before children.
    std::optional<std::string> id;                ///< Compact recipe identity preserved by later style modifiers.
    std::shared_ptr<detail::RefTarget> anchor;    ///< Cold same-attempt body anchor, without widget ownership.
    DialogPlacement placement{DialogPlacement::BELOW_START}; ///< Configured anchor preference.
    bool anchorRequested{};                    ///< Distinguish an absent modifier from an empty moved observing slot.
    std::optional<StyleTarget> intrinsicStyle; ///< Owning natural type names, finalized after ordinary setup.
    bool controlPrepared{};                 ///< Resource settings were validated and materialized before construction.
    bool deferredControlValidation{};       ///< Root resource parser validates after CONFIG, before document effects.
    std::uint32_t materializedSettings{};   ///< Resource properties already reserved in the aggregate leaf budget.
    std::optional<std::string> controlType; ///< Supported concrete factory; absent on arbitrary custom widgets.
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

  /** @brief Allocate recipe storage and take ownership of its constructor adapter.
   * @tparam Create Owning callable returning a checked typed node for ManagedScene and parent.
   * @param create Owning checked
   * constructor callback, invoked only at mount.
   */
  // Reject unrelated by-value conversions before checked-result constructor probes recurse.
  template <class Create>
    requires std::invocable<Create &, ManagedScene &, Node<Widget> &>
  explicit Description(Create create) : state_(std::make_unique<State>()) {
    state_->create = [create = std::move(create)](ManagedScene &scene, Node<Widget> parent,
                                                  detail::MountContext &) mutable {
      return create(scene, parent);
    };
  }

  detail::RecipeLayout &layoutMetadata() {
    if (!state_->layout) {
      state_->layout = std::make_shared<detail::RecipeLayout>();
    }

    return *state_->layout;
  }

  detail::GridRecipeControl &gridControl()
    requires std::same_as<T, Grid>
  {
    auto &layout = layoutMetadata();

    if (!layout.control) {
      layout.control = std::make_unique<detail::GridRecipeControl>();
    }

    return *layout.control;
  }

  DeclarativeGridOptions &declarativeOptions()
    requires std::same_as<T, Grid>
  {
    auto &layout = layoutMetadata();

    if (!layout.grid) {
      layout.grid.emplace();
    }

    layout.hasDeclaredGrid = true;

    return *layout.grid;
  }

  /** @param change Owning policy operation applied in setup/modifier order.
   * @return This consuming recipe; existing checked policy errors propagate at mount.
   */
  Description &&resize(detail::SizeChange change) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).setup([change](ManagedScene &scene, Node<T> node) {
      return detail::Access::resize(scene, node, change);
    });
  }

  /** @brief Retain a shared observing slot without publishing it yet.
   * @param reference Slot to reserve when this
   * recipe is mounted.
   * @return This consuming recipe for modifier chaining.
   */
  Description &&storeReference(NodeRef<T> reference) && {
    if (state_) {
      state_->references.push_back(std::move(reference));
    }

    return std::move(*this);
  }

  /** @brief Finish fallible owning configuration before entering recursive construction/children.
   * @param state Consumed recipe storage, retaining any resolved properties until setup completes.
   * @param parent Existing checked parent used for failure attribution.
   * @param context Whole-mount immutable snapshot and aggregate leaf accounting.
   * @return Typed validation/budget result; allocation exceptions stay inside the outer mount boundary.
   */
  static std::expected<void, ManagedError> prepareControl(State &state, Node<Widget> parent,
                                                          detail::MountContext &context) {
    if constexpr (std::same_as<T, Grid>) {
      if (state.layout && state.layout->gridContainer && state.layout->control && state.layout->control->prepared) {
        state.resolvedControl = std::move(state.layout->control->resolved);

        if (state.controlVariant && state.resolvedControl) {
          state.intrinsicStyle =
            StyleTarget{.type = state.resolvedControl->type, .baseTypes = state.resolvedControl->lineage};
        }

        return {};
      }
    }

    if ((state.settingsRequested || state.controlVariant) && !state.controlType) {
      return std::unexpected(detail::controlContextError({ControlContextErrorCode::UNSUPPORTED_PROPERTY, {}}, parent));
    }
    const bool builtinOnly = context.controls.empty() && !state.settingsRequested && !state.controlVariant;
    if (state.controlType && (!state.controlPrepared || state.settingsRequested || state.controlVariant) &&
        !builtinOnly) {
      const auto controls = !state.inheritControlSettings || (state.controlPrepared && !state.controlVariant)
                              ? ControlContext(context.controls.limits())
                              : context.controls;
      const auto type = state.inheritControlSettings && state.controlVariant ? std::string_view(*state.controlVariant)
                                                                             : std::string_view(*state.controlType);
      auto effective = state.inheritControlSettings
                         ? controls.resolve(type, detail::Access::mergeControlSettings(
                                                    std::move(state.controlArguments), state.controlPatch))
                         : controls.resolve(type, state.controlPatch);
      if (!effective) {
        return std::unexpected(detail::controlContextError(effective.error(), parent));
      }
      if (!state.inheritControlSettings && state.controlVariant) {
        const auto identity = context.controls.resolve(*state.controlVariant);

        if (!identity) {
          return std::unexpected(detail::controlContextError(identity.error(), parent));
        }

        effective->type = identity->type;
        effective->base = identity->base;
        effective->lineage = identity->lineage;
      }
      if (effective->base != *state.controlType) {
        return std::unexpected(detail::controlContextError({ControlContextErrorCode::INVALID_VALUE, {}}, parent));
      }
      const auto addedLeaves =
        state.controlPrepared ? static_cast<std::size_t>(std::popcount(
                                  detail::controlSettingsPresence(effective->settings) & ~state.materializedSettings))
                              : effective->materializedLeaves;
      const auto limit = context.controls.limits().maxMaterializedLeaves;
      if (context.controlLeaves > limit || addedLeaves > limit - context.controlLeaves) {
        return std::unexpected(
          detail::controlContextError({ControlContextErrorCode::MATERIALIZED_LEAF_LIMIT, {}}, parent));
      }
      context.controlLeaves += addedLeaves;
      if (state.controlVariant) {
        state.intrinsicStyle = StyleTarget{.type = effective->type,
                                           .control = std::derived_from<T, Control> || std::same_as<T, LineEdit>,
                                           .baseTypes = effective->lineage};
        if constexpr (std::same_as<T, Row> || std::same_as<T, Column>) {
          constexpr std::string_view NATURAL = std::same_as<T, Row> ? "Row" : "Column";
          if (state.intrinsicStyle->type != NATURAL &&
              std::ranges::find(state.intrinsicStyle->baseTypes, NATURAL) == state.intrinsicStyle->baseTypes.end()) {
            state.intrinsicStyle->baseTypes.emplace_back(NATURAL);
          }
        }
      }
      state.resolvedControl = std::make_unique<ResolvedControl>(std::move(*effective));
    }

    return {};
  }

  /** @brief Consume this recipe into one existing managed transaction.
   * @param scene Borrowed UI owner receiving
   * the new subtree.
   * @param parent Live managed parent receiving this recipe's root.
   * @param context Borrowed
   * observations and ref reservations for the complete mount.
   * @return Created observer or unchanged error; the
   * outer mount owns subtree rollback.
   */
  std::expected<Node<T>, ManagedError> mountInto(ManagedScene &scene, Node<Widget> parent,
                                                 detail::MountContext &context) && {
    auto state = std::move(state_);

    if (!state || !state->create) {
      return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
    }

    auto gridPrepared = detail::prepareGridLayouts(state->layout, context, parent);

    if (!gridPrepared) {
      return std::unexpected(gridPrepared.error());
    }

    auto layoutChecked = detail::preflightLayout(state->layout);

    if (!layoutChecked) {
      return std::unexpected(detail::error(parent, layoutChecked.error()));
    }

    auto prepared = prepareControl(*state, parent, context);
    if (!prepared) {
      return std::unexpected(prepared.error());
    }
    const auto *resolved = state->resolvedControl.get();

    auto node = [&] {
      struct Restore {
        detail::MountContext &context;
        bool previous;
        const ControlSettings *settings;
        ~Restore() {
          context.controlPrepared = previous;
          context.controlSettings = settings;
        }
      } restore{context, context.controlPrepared, context.controlSettings};

      context.controlPrepared = state->controlPrepared;
      context.controlSettings = resolved ? &resolved->settings : &state->controlArguments;

      return state->create(scene, parent, context);
    }();

    if (!node) {
      return node;
    }

    if (context.root == Node<Widget>{}) {
      context.root = *node;
    }

    context.nodes.push_back(*node);

    if (state->intrinsicStyle) {
      auto styled = scene.style(*node, *state->intrinsicStyle);
      if (!styled) {
        return std::unexpected(styled.error());
      }
    }

    if (state->controlSetup) {
      auto configured = state->controlSetup(scene, *node, resolved ? resolved->settings : state->controlArguments);
      if (!configured) {
        return std::unexpected(configured.error());
      }
    }

    if constexpr (std::derived_from<T, SizedWidget>) {
      if (resolved) {
        const auto &settings = resolved->settings;
        for (const auto &change :
             {settings.width ? std::optional(detail::SizeChange{detail::SizeDimension::WIDTH, *settings.width})
                             : std::nullopt,
              settings.height ? std::optional(detail::SizeChange{detail::SizeDimension::HEIGHT, *settings.height})
                              : std::nullopt,
              settings.flex ? std::optional(detail::SizeChange{detail::SizeDimension::FLEX, 0, *settings.flex})
                            : std::nullopt}) {
          if (change) {
            auto resized = detail::Access::resize(scene, *node, *change);
            if (!resized) {
              return std::unexpected(resized.error());
            }
          }
        }
      }
    }

    for (auto &reference : state->references) {
      auto slot = reference.slot_;

      if (slot && slot->fluent && slot->target.owner != std::this_thread::get_id()) {
        return std::unexpected(detail::error(*node, TreeError::WRONG_THREAD));
      }

      if (!slot || slot->node != Node<T>{} || slot->pending) {
        return std::unexpected(detail::error(*node, TreeError::INVALID_ARGUMENT));
      }

      // Reserve across reentrant mount calls too, but never publish a partial identity.
      // Allocate both callbacks before setting pending so exceptions cannot strand the slot.
      context.release.push_back([slot] {
        slot->pending = false;
      });
      context.publish.push_back([slot, value = *node, owner = &scene, lifetime = detail::Access::lifetime(scene)] {
        slot->node = value;
        slot->target.node = value;
        slot->target.scene = owner;
        slot->target.lifetime = lifetime;

        if constexpr (std::derived_from<T, Control>) {
          slot->target.caption = value;
        } else if constexpr (std::same_as<T, LineEdit>) {
          slot->target.draft = value;
        }
      });
      detail::RefTarget pending;
      pending.node = *node;
      pending.scene = &scene;
      pending.lifetime = detail::Access::lifetime(scene);
      if constexpr (std::derived_from<T, Control>) {
        pending.caption = *node;
      } else if constexpr (std::same_as<T, LineEdit>) {
        pending.draft = *node;
      }
      context.pending.push_back({&slot->target, std::move(pending)});
      slot->pending = true;
    }

    if (state->focusScope) {
      auto registered = scene.focusScope(*node);

      if (!registered) {
        return std::unexpected(registered.error());
      }
    }

    if (state->focusDefault) {
      auto configured = scene.focusDefault(*node);

      if (!configured) {
        return std::unexpected(configured.error());
      }
    }

    for (auto &setup : state->setup) {
      if (!setup) {
        return std::unexpected(detail::error(*node, TreeError::INVALID_ARGUMENT));
      }

      auto configured = setup(scene, *node);

      if (!configured) {
        return std::unexpected(configured.error());
      }
    }

    if (state->intrinsicStyle) {
      auto styled = detail::Access::finalizeStyleIdentity(scene, *node, *state->intrinsicStyle);

      if (!styled) {
        return std::unexpected(styled.error());
      }
    }

    for (auto &child : state->children) {
      auto mounted = child(scene, *node, context);

      if (!mounted) {
        return std::unexpected(mounted.error());
      }
    }

    if constexpr (std::same_as<T, Grid>) {
      if (state->layout && state->layout->grid) {
        if (state->layout->mounted.size() != state->layout->children.size()) {
          return std::unexpected(detail::error(*node, TreeError::INVALID_ARGUMENT));
        }

        auto configured = detail::Access::gridOptions(scene, *node, detail::resolvedGridOptions(*state->layout));

        if (!configured) {
          return std::unexpected(configured.error());
        }
      }
    }

    if constexpr (std::same_as<T, Dialog>) {
      if (state->anchorRequested) {
        context.finalize.push_back([&scene, &context, node = *node, target = state->anchor,
                                    placement = state->placement]() -> std::expected<void, ManagedError> {
          const auto *resolved = target.get();
          if (!resolved) {
            return std::unexpected(detail::error(node, TreeError::CLOSED));
          }
          for (const auto &pending : context.pending) {
            if (pending.identity == target.get()) {
              resolved = &pending.target;
              break;
            }
          }
          if (resolved->owner != std::this_thread::get_id()) {
            return std::unexpected(detail::error(node, TreeError::WRONG_THREAD));
          }
          if (resolved->failure) {
            return std::unexpected(*resolved->failure);
          }
          if (resolved->lifetime.expired()) {
            return std::unexpected(detail::error(node, TreeError::CLOSED));
          }
          return scene.dialogAnchor(node, resolved->node, placement);
        });
      }
    }

    return node;
  }

  std::unique_ptr<State> state_;
};

/** @brief Single-use semantic Grid section; owns recipes and never mounts a placeholder Widget. */
class GridSection {
public:
  /** @param other Owning source consumed into this group; checked-STL transfers may allocate. */
  GridSection(GridSection &&other)
      : region_(other.region_), sticky_(other.sticky_), valid_(other.valid_), style_(other.style_), rows_(other.rows_) {
    children_.swap(other.children_);
    other.valid_ = false;
  }

  /** @param other Owning source replacing these unmounted children. @return This section. */
  GridSection &operator=(GridSection &&other) {
    if (this == &other) {
      return *this;
    }

    children_.swap(other.children_);
    region_ = other.region_;
    sticky_ = other.sticky_;
    valid_ = other.valid_;
    style_ = other.style_;
    rows_ = other.rows_;
    other.valid_ = false;

    return *this;
  }

  GridSection(const GridSection &) = delete;
  GridSection &operator=(const GridSection &) = delete;

  /** @param value Whether this header remains fixed vertically when Grid owns scrolling.
   * @return Consuming header; using sticky on body fails preflight.
   */
  GridSection &&sticky(bool value = true) && {
    sticky_ = value;
    valid_ = valid_ && region_ == GridRegion::HEADER;

    return std::move(*this);
  }

  /** @param value Owning region background; does not inherit into child controls.
   * @return Consuming section with this regional background.
   */
  GridSection &&style(CellStyle value) && {
    style_ = value;

    return std::move(*this);
  }

  /** @param value Owning logical odd/even body backgrounds, below real controls.
   * @return Consuming body; row styles on header fail preflight.
   */
  GridSection &&rowStyles(GridRowStyles value) && {
    rows_ = value;
    valid_ = valid_ && region_ == GridRegion::BODY;

    return std::move(*this);
  }

private:
  friend struct detail::Access;
  template <detail::DescriptionInput... Children> friend GridSection header(Children &&...);
  template <detail::DescriptionInput... Children> friend GridSection body(Children &&...);

  explicit GridSection(GridRegion region) : region_(region) {
  }

  GridRegion region_;
  bool sticky_{};
  bool valid_{true};
  std::optional<CellStyle> style_;
  std::optional<GridRowStyles> rows_;
  std::vector<Description<Widget>> children_ = std::vector<Description<Widget>>(0);
};

namespace detail {
inline std::expected<void, ManagedError> prepareGridLayouts(const std::shared_ptr<RecipeLayout> &root,
                                                            MountContext &context, Node<Widget> parent) {
  if (!root || root->validated) {
    return {};
  }

  std::vector<std::shared_ptr<RecipeLayout>> pending(0);
  std::size_t addedLeaves{};
  const auto limit = context.controls.limits().maxMaterializedLeaves;

  pending.push_back(root);

  while (!pending.empty()) {
    auto layout = std::move(pending.back());

    pending.pop_back();

    for (const auto &child : layout->children) {
      pending.push_back(child);
    }

    if (!layout->gridContainer || !layout->grid) {
      continue;
    }

    if (!layout->control) {
      layout->control = std::make_unique<GridRecipeControl>();
    }

    auto &control = *layout->control;

    if (control.prepared) {
      continue;
    }

    auto identityControls = context.controls;
    auto installed = ControlContextAccess::installFallbackSchema(identityControls);

    if (!installed) {
      return std::unexpected(controlContextError(installed.error(), parent));
    }

    auto identity = identityControls.resolve(control.variant ? std::string_view(*control.variant) : "Grid");

    if (!identity) {
      return std::unexpected(controlContextError(identity.error(), parent));
    }

    if (identity->factoryKind != ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      return std::unexpected(controlContextError({ControlContextErrorCode::INVALID_VALUE, {}}, parent));
    }

    if (control.materialized && !control.settingsRequested && !control.variant) {
      control.prepared = true;

      continue;
    }

    auto controls =
      control.materialized && !control.variant ? ControlContext(context.controls.limits()) : identityControls;
    installed = ControlContextAccess::installFallbackSchema(controls);

    if (!installed) {
      return std::unexpected(controlContextError(installed.error(), parent));
    }

    auto effective = controls.resolve(control.variant ? std::string_view(*control.variant) : "Grid",
                                      Access::mergeControlSettings(control.arguments, control.patch));

    if (!effective) {
      return std::unexpected(controlContextError(effective.error(), parent));
    }

    const auto leaves = control.materialized
                          ? static_cast<std::size_t>(std::popcount(controlSettingsPresence(effective->settings) &
                                                                   ~control.materializedSettings))
                          : effective->materializedLeaves;

    if (context.controlLeaves > limit || addedLeaves > limit - context.controlLeaves ||
        leaves > limit - context.controlLeaves - addedLeaves) {
      return std::unexpected(controlContextError({ControlContextErrorCode::MATERIALIZED_LEAF_LIMIT, {}}, parent));
    }

    addedLeaves += leaves;

    const auto &grid = effective->settings.grid;
    auto &options = *layout->grid;
    const auto apply = [](auto &target, const auto &source) {
      if (source) {
        target = *source;
      }
    };

    apply(options.columns, grid.columns);
    apply(options.rows, grid.rows);
    apply(options.flow, grid.flow);
    apply(options.columnGap, grid.columnGap);
    apply(options.rowGap, grid.rowGap);
    apply(options.padding.left, grid.paddingLeft);
    apply(options.padding.top, grid.paddingTop);
    apply(options.padding.right, grid.paddingRight);
    apply(options.padding.bottom, grid.paddingBottom);
    apply(options.maxTracksPerAxis, grid.maxTracksPerAxis);
    apply(options.maxPlacements, grid.maxPlacements);
    apply(options.maxPlacementWork, grid.maxPlacementWork);

    if (grid.scroll) {
      if (*grid.scroll && !layout->scroll) {
        layout->scroll.emplace();
      } else if (!*grid.scroll) {
        layout->scroll.reset();
      }
    }

    control.resolved = std::make_unique<ResolvedControl>(std::move(*effective));
    control.prepared = true;
  }

  context.controlLeaves += addedLeaves;

  return {};
}

template <class T> bool Access::needsGridContext(const Description<T> &description) {
  return description.state_ && description.state_->layout && description.state_->layout->hasDeclaredGrid;
}

inline Description<Widget> Access::deferControlValidation(Description<Widget> description) {
  if (description.state_) {
    description.state_->deferredControlValidation = true;
  }

  return description;
}

template <class T> bool Access::defersControlValidation(const Description<T> &description) {
  return description.state_ && description.state_->deferredControlValidation;
}

template <class Child>
void Access::gridChild(Description<Grid> &parent, Child &&child, std::optional<GridRegion> region) {
  layoutChild(parent, child, true);

  if (region && child.state_) {
    child.state_->layout->item.region = *region;
  }

  auto layout = parent.state_->layout;

  parent.state_->children.push_back(
    [layout, child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> owner,
                                                 MountContext &context) mutable -> std::expected<void, ManagedError> {
      auto mounted = Access::mount(std::move(child), scene, owner, context);

      if (!mounted) {
        return std::unexpected(mounted.error());
      }

      if (layout->grid) {
        layout->mounted.push_back(mounted->id());
      }

      return {};
    });
}

inline void Access::gridSection(Description<Grid> &parent, GridSection section) {
  auto &layout = parent.layoutMetadata();

  parent.declarativeOptions();
  layout.validSections = layout.validSections && section.valid_;

  if (section.region_ == GridRegion::HEADER) {
    ++layout.headerSections;
    layout.stickyHeader = section.sticky_;
    layout.headerStyle = section.style_;
  } else {
    ++layout.bodySections;
    layout.bodyStyle = section.style_;
    layout.rowStyles = section.rows_;
  }

  for (auto &child : section.children_) {
    gridChild(parent, std::move(child), section.region_);
  }
}

template <class... Children> std::shared_ptr<RecipeLayout> Access::captureLayouts(Children &...children) {
  std::shared_ptr<RecipeLayout> layout;
  const auto append = [&](auto &child) {
    if (!child.state_ || child.state_->layout) {
      if (!layout) {
        layout = std::make_shared<RecipeLayout>();
      }

      if (child.state_) {
        layout->children.push_back(child.state_->layout);
        layout->hasDeclaredGrid = layout->hasDeclaredGrid || child.state_->layout->hasDeclaredGrid;
      } else {
        auto empty = std::make_shared<RecipeLayout>();

        empty->validRecipe = false;
        layout->children.push_back(std::move(empty));
      }
    }
  };

  (append(children), ...);

  return layout;
}

template <class T> Description<T> Access::withLayout(Description<T> description, std::shared_ptr<RecipeLayout> layout) {
  if (description.state_) {
    description.state_->layout = std::move(layout);
  }

  return description;
}

template <class Parent, class Child>
void Access::layoutChild(Description<Parent> &parent, Description<Child> &child, bool structural) {
  if (!parent.state_) {
    return;
  }

  if (!child.state_) {
    auto empty = std::make_shared<RecipeLayout>();

    empty->validRecipe = false;
    parent.layoutMetadata().children.push_back(std::move(empty));

    return;
  }

  if (structural) {
    child.layoutMetadata();
  }

  if (child.state_->layout) {
    parent.layoutMetadata().children.push_back(child.state_->layout);
    parent.state_->layout->hasDeclaredGrid =
      parent.state_->layout->hasDeclaredGrid || child.state_->layout->hasDeclaredGrid;
  }
}

template <class T> void Access::preparedControl(Description<T> &description, std::uint32_t alreadyMaterialized) {
  if (description.state_) {
    description.state_->controlPrepared = true;
    description.state_->materializedSettings = alreadyMaterialized;

    if constexpr (std::same_as<T, Grid>) {
      if (description.state_->layout && description.state_->layout->gridContainer && description.state_->layout->grid) {
        auto &control = description.gridControl();

        control.materialized = true;
        control.materializedSettings = alreadyMaterialized;
      }
    }
  }
}

template <class T>
Description<T> Access::configuredControl(Description<T> description, std::string_view type, bool inherit) {
  if (description.state_ && !type.empty()) {
    description.state_->controlType.emplace(type);
    description.state_->inheritControlSettings = inherit;
  }
  return description;
}

template <class T> Description<T> Access::controlArguments(Description<T> description, ControlSettings arguments) {
  if (description.state_) {
    description.state_->controlArguments = std::move(arguments);
  }
  return description;
}

template <class T, class Callback> Description<T> Access::controlSetup(Description<T> description, Callback callback) {
  if (description.state_) {
    description.state_->controlSetup = std::move(callback);
  }
  return description;
}

template <class T, class Factory> Description<T> Access::describe(Factory factory) {
  return Description<T>(std::move(factory));
}

template <class T> Description<T> Access::intrinsicStyle(Description<T> description, StyleTarget identity) {
  if (description.state_) {
    description.state_->intrinsicStyle.emplace(std::move(identity));
  }

  return description;
}

/// @cond INTERNAL
template <class T, class Factory> Description<T> Access::cold(Factory factory) {
  Description<T> description;
  description.state_ = std::make_unique<typename Description<T>::State>();
  description.state_->create = std::move(factory);
  return description;
}

/// @endcond

template <class Factory> Description<Widget> Access::transaction(Factory factory) {
  Description<Widget> description;

  description.state_ = std::make_unique<Description<Widget>::State>();
  description.state_->create = std::move(factory);

  return description;
}

template <class T> Description<Widget> Access::erase(Description<T> description) {
  Description<Widget> erased;

  erased.state_ = std::make_unique<Description<Widget>::State>();
  if (description.state_) {
    erased.state_->layout = description.state_->layout;
  }
  erased.state_->create =
    [description = std::move(description)](ManagedScene &scene, Node<Widget> parent,
                                           MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
    auto result = mount(std::move(description), scene, parent, context);

    if (!result) {
      return std::unexpected(result.error());
    }

    return Node<Widget>(*result);
  };

  return erased;
}

template <class T>
std::expected<Node<T>, ManagedError> Access::mount(Description<T> &&description, ManagedScene &scene,
                                                   Node<Widget> parent, MountContext &context) {
  return std::move(description).mountInto(scene, parent, context);
}

/** @brief Materialize string-like inputs and otherwise retain a decayed argument value.
 * @tparam T Forwarded
 * constructor argument type; strings must convert to std::string_view.
 * @param value Source copied or moved into
 * recipe storage; nonstring borrows remain explicit.
 * @return Owning std::string for string-like inputs, otherwise a
 * decayed argument value.
 */
template <class T> auto own(T &&value) {
  if constexpr (std::convertible_to<T, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return std::decay_t<T>(std::forward<T>(value));
  }
}

/** @brief Borrow string bytes only while constructing the final owning argument tuple.
 * @tparam T Forwarded constructor argument; string-like values convert to string_view.
 * @param value Input living through the immediate tuple constructor.
 * @return A temporary string view, or the unchanged nonstring forwarding reference.
 */
template <class T> decltype(auto) constructionArgument(T &&value) {
  if constexpr (std::convertible_to<T, std::string_view>) {
    return std::string_view(value);
  } else {
    return std::forward<T>(value);
  }
}
} // namespace detail

/** @brief Capture constructor arguments without requiring inheritance from a DSL widget.
 * @tparam T Public Widget subtype, including final types, constructible from the owned arguments.
 * @tparam Args Decayed owning argument types; string-like inputs are materialized as std::string.
 * @param args Values copied/moved now and moved into T at mount. Other pointers/reference_wrapper
 * remain explicit borrows: their referents must outlive the widget. No deep copy is inferred.
 * @return A single-use recipe. T must own any string_view it retains beyond its constructor.
 */
template <class T, class... Args> Description<T> custom(Args &&...args) {
  using OwnedArguments = std::tuple<decltype(detail::own(std::forward<Args>(args)))...>;
  constexpr bool HAS_TEXT_ARGUMENT = (std::convertible_to<Args, std::string_view> || ...);

  // Construct strings directly in stationary tuple storage. Moving a string or tuple<string>
  // can allocate a checked MSVC proxy despite noexcept; a string_view constructor stays fallible.
  // Explicit element types also preserve tuple/pair constructor arguments without CTAD flattening.
  auto owned = [&] {
    if constexpr (HAS_TEXT_ARGUMENT && detail::CHECKED_STRING_MOVES) {
      return std::make_unique<OwnedArguments>(detail::constructionArgument(std::forward<Args>(args))...);
    } else {
      return OwnedArguments(detail::own(std::forward<Args>(args))...);
    }
  }();

  return Description<T>([owned = std::move(owned)](ManagedScene &scene, Node<Widget> parent) mutable {
    auto &arguments = [&]() -> auto & {
      if constexpr (HAS_TEXT_ARGUMENT && detail::CHECKED_STRING_MOVES) {
        return *owned;
      } else {
        return owned;
      }
    }();

    return std::apply(
      [&](auto &...values) {
        return scene.emplace<T>(parent, std::move(values)...);
      },
      arguments);
  });
}

template <class Content, class Initial>
Description<DataStateView> detail::Access::dataState(Initial initial, Description<Content> content,
                                                     DataStateConfig config, bool automatic) {
  auto result = describe<DataStateView>(
    [initial, config = std::move(config), automatic](ManagedScene &scene, Node<Widget> parent) mutable {
      if constexpr (std::same_as<Initial, DataPresentation>) {
        return scene.beginDataState(parent, initial, std::move(config), automatic);
      } else {
        return scene.beginBoundDataState(parent, initial, std::move(config));
      }
    });
  layoutChild(result, content);
  result.state_->children.push_back(
    [content = std::move(content), automatic](ManagedScene &scene, Node<Widget> parent,
                                              MountContext &context) mutable -> std::expected<void, ManagedError> {
      const auto finalizers = context.finalize.size();

      auto child = mount(std::move(content), scene, parent, context);

      if (!child) {
        return std::unexpected(child.error());
      }

      if (automatic && context.finalize.size() != finalizers) {
        // Cold virtual bodies attach their model during the existing mount finalization.
        // Publish the relationship afterward, before any reference or preparation escapes.
        context.finalize.push_back([&scene, parent, body = *child] {
          return scene.finishDataState(parent, body, true);
        });

        return {};
      }

      return scene.finishDataState(parent, *child, automatic);
    });
  return result;
}

/** @tparam Content Retained widget type mounted as the one body subtree.
 * @param initial Owning explicit state, with nonzero producer generation.
 * @param content Consuming body recipe; independent work is required when REPLACE hides it.
 * @param config Construction-only labels, fixed strip, styles and optional retry.
 * @return Single-use composition; validation/allocation errors preserve ordinary mount rollback.
 */
template <class Content>
Description<DataStateView> dataStateView(DataPresentation initial, Description<Content> content,
                                         DataStateConfig config = {}) {
  return detail::Access::dataState(initial, std::move(content), std::move(config), false);
}

/** @tparam Source Exactly ManagedState<DataPresentation> or readonly ManagedSource<DataPresentation>.
 * @tparam Content Retained body widget type.
 * @param source Weak same-scene one-way observation; no writable alias escapes.
 * @param content Consuming body recipe. @param config Owning immutable presentation settings.
 * @return Single-use bound composition; initial snapshot is validated before publication.
 */
template <class Source, class Content>
  requires(std::same_as<Source, ManagedState<DataPresentation>> ||
           std::same_as<Source, ManagedSource<DataPresentation>>)
Description<DataStateView> dataStateView(Source source, Description<Content> content, DataStateConfig config = {}) {
  return detail::Access::dataState(source, std::move(content), std::move(config), false);
}

/** @tparam View VirtualList/VirtualTable type exposing existing prepared owning facts.
 * @param content Consuming managed virtual view; its provider callback retains ordinary work ownership.
 * @param config Stable strip and optional explicit retry; REPLACE is rejected.
 * @return Automatic composition using the body's successful Prepared without repinning or I/O.
 */
template <class View>
  requires requires(const View &view) {
    { view.dataStatus() } -> std::same_as<DataRangeStatus>;
  }
Description<DataStateView> dataStateView(Description<View> content, DataStateConfig config = {}) {
  return detail::Access::dataState(DataPresentation{DataPhase::LOADING, 1}, std::move(content), std::move(config),
                                   true);
}

/** @brief Construct a standard text-control recipe with the scene's width policy.
 * @tparam T Label, Button or CheckBox-compatible text constructor.
 * @param text UTF-8 bytes copied now; invalid text returns INVALID_ARGUMENT at mount.
 * @param style Owning manual control appearances.
 * @param policy Owning layout constraints.
 * @return Single-use owning recipe; allocation while copying may throw.
 */
template <class T> Description<T> control(std::string_view text, ControlStyle style, SizePolicy policy) {
  auto description =
    detail::Access::cold<T>([style, policy](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) {
      auto value = context.controlSettings->text->resolve(scene.widthPolicy());

      if (!value) {
        auto failure = detail::error(parent, TreeError::INVALID_ARGUMENT);
        if (const auto *textError = std::get_if<TextError>(&value.error())) {
          failure = detail::textError(parent, *textError);
        } else {
          failure.resource = std::get<TextResourceError>(value.error());
        }

        return std::expected<Node<T>, ManagedError>(std::unexpected(failure));
      }

      return scene.emplace<T>(parent, std::move(*value), style, policy);
    });
  return detail::Access::controlArguments(
    detail::Access::configuredControl(std::move(description), detail::Access::controlType<T>()),
    ControlSettings{.text = TextValue(text)});
}

/** @param text UTF-8 copied immediately, validated at mount under the scene's width policy.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use label recipe.
 */
inline Description<Label> label(std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Label>(text, style, policy);
}

namespace detail {
/** @brief Resolve an owning caption expression once inside the existing mount transaction.
 * @tparam T Existing text control constructed with text, style and size policy.
 * @param text Owning literal, format result or localization key.
 * @param style Manual appearances copied into the recipe.
 * @param policy Owning layout constraints.
 * @param resolver Owning explicit localization adapter; no locale subscription is created.
 * @return Single-use caption recipe with original text/resource errors.
 */
template <class T>
Description<T> textControl(TextValue text, ControlStyle style, SizePolicy policy, TextResolver resolver) {
  auto arguments = ControlSettings{.text = std::move(text)};
  auto description = Access::cold<T>(
    [style, policy, resolver = std::move(resolver)](ManagedScene &scene, Node<Widget> parent,
                                                    MountContext &context) -> std::expected<Node<T>, ManagedError> {
      const auto &caption = *context.controlSettings->text;
      auto value = resolver ? caption.resolve(scene.widthPolicy(), resolver) : caption.resolve(scene.widthPolicy());

      if (!value) {
        if (const auto *failure = std::get_if<TextError>(&value.error())) {
          return std::unexpected(textError(parent, *failure));
        }

        auto failure = error(parent, TreeError::INVALID_ARGUMENT);

        failure.resource = std::get<TextResourceError>(value.error());

        return std::unexpected(failure);
      }

      return scene.emplace<T>(parent, std::move(*value), style, policy);
    });
  return Access::controlArguments(Access::configuredControl(std::move(description), Access::controlType<T>()),
                                  std::move(arguments));
}
/** @brief Exact deferred caption inputs; literals and bare braces keep their existing overloads. */
template <class T>
concept CaptionSource =
  std::same_as<std::remove_cvref_t<T>, TextSource> || std::same_as<std::remove_cvref_t<T>, TextProperty>;

template <class T> Description<T> liveCaption(TextSource source, ControlStyle style, SizePolicy policy) {
  auto description = Access::cold<T>(
    [source = std::move(source), style, policy](ManagedScene &scene, Node<Widget> parent,
                                                MountContext &context) -> std::expected<Node<T>, ManagedError> {
      auto admitted = Access::check(scene, parent);
      if (!admitted) {
        return std::unexpected(admitted.error());
      }
      auto graph = std::make_shared<Access::ColdGraph>();
      graph->scene = &scene;
      context.rollback.push_back([graph] {
        graph->retire();
      });
      graph->pending = true;
      Access::beginCold(scene);
      auto empty = GraphemeText::from("", scene.widthPolicy());
      if (!empty) {
        return std::unexpected(textError(parent, empty.error()));
      }
      auto node = scene.emplace<T>(parent, std::move(*empty), style, policy);
      if (!node) {
        return node;
      }
      if (context.root == Node<Widget>{}) {
        context.root = *node;
      }
      context.captions.push_back(
        {*node, Access::captionIdentity(source), false, [&scene, &context, node = *node, source, graph] {
           return Access::completeCaption(scene, node, source, context, graph);
         }});
      context.finalize.push_back([&context] {
        return Access::finishCaptions(context);
      });
      return node;
    });
  return Access::configuredControl(std::move(description), Access::controlType<T>());
}

template <CaptionSource Source> TextSource captionSource(Source &&source) {
  if constexpr (std::same_as<std::remove_cvref_t<Source>, TextProperty>) {
    return source.source();
  } else {
    return std::forward<Source>(source);
  }
}
} // namespace detail

/** @tparam Source Exact TextSource or TextProperty observation, read after the complete mount.
 * @param source Visible caption/draft or accepted model identity; never retains a widget.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Live one-way label recipe; cycles reject mount and removed sources never retarget.
 */
template <detail::CaptionSource Source>
Description<Label> label(Source &&source, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::liveCaption<Label>(detail::captionSource(std::forward<Source>(source)), style, policy);
}

/** @tparam Source Exact TextSource or TextProperty, including an unpublished sibling reference.
 * @param source Visible caption/draft or accepted model identity.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Live one-way button caption; action attachment remains independent.
 */
template <detail::CaptionSource Source>
Description<Button> button(Source &&source, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::liveCaption<Button>(detail::captionSource(std::forward<Source>(source)), style, policy);
}

/** @param text Owning literal, format result or localization key, resolved once at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param resolver Explicit owning localization adapter; borrowed services must outlive mount.
 * @return Independent label recipe; expression/resource failures retain their typed payload.
 */
inline Description<Label> label(TextValue text, ControlStyle style = {}, SizePolicy policy = {},
                                TextResolver resolver = {}) {
  return detail::textControl<Label>(std::move(text), style, policy, std::move(resolver));
}

/** @tparam State Exact ManagedState<string> or read-only ManagedSource<string>; bare braces retain literal overload.
 * @param model Scene-owned accepted text state; must belong to the receiving scene.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return One-way caption recipe; changed revisions synchronize before dirty frames without caller tokens.
 */
template <class State>
  requires(std::same_as<std::remove_cvref_t<State>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<State>, ManagedSource<std::string>>)
inline Description<Label> label(State &&model, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Label>("", style, policy).setup([model](ManagedScene &scene, Node<Label> node) {
    return scene.bindText(node, model);
  });
}

/** @param text Owning literal, format result or localization key, resolved once at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param resolver Explicit owning localization adapter; borrowed services must outlive mount.
 * @return Unbound button recipe with an independent caption; onClick can be composed separately.
 */
inline Description<Button> button(TextValue text, ControlStyle style = {}, SizePolicy policy = {},
                                  TextResolver resolver = {}) {
  return detail::textControl<Button>(std::move(text), style, policy, std::move(resolver));
}

/** @tparam State Exact ManagedState<string> or read-only ManagedSource<string>; bare braces retain literal overload.
 * @param model Scene-owned accepted text state; must belong to the receiving scene.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return One-way caption recipe with independent action attachment and existing dirty-frame recovery.
 */
template <class State>
  requires(std::same_as<std::remove_cvref_t<State>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<State>, ManagedSource<std::string>>)
inline Description<Button> button(State &&model, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Button>("", style, policy).setup([model](ManagedScene &scene, Node<Button> node) {
    return scene.bindText(node, model);
  });
}

/** @tparam Caption Owning TextValue, ManagedState<string> or read-only ManagedSource<string> observation.
 * @tparam Callback Owning compact managed action, optionally borrowing ClickEvent.
 * @param text Caption expression or explicit accepted text state.
 * @param action Independent activation callback with existing checked result handling.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Button recipe with scene-owned caption/action lifetimes and transactional mount cleanup.
 */
template <class Caption, class Callback>
  requires((std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>) &&
           portals::detail::ManagedAction<Callback, ClickEvent>)
inline Description<Button> button(Caption &&text, Callback action, ControlStyle style = {}, SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), style, policy).onClick(std::move(action));
}

/** @param text UTF-8 caption copied immediately.
 * @param action Standard managed activation callback; must be nonempty.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use button recipe retaining its callback until mount.
 */
inline Description<Button> button(std::string_view text, std::function<std::expected<void, TreeError>()> action,
                                  ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Button>(text, style, policy)
    .setup([action = std::move(action)](ManagedScene &scene, Node<Button> node) mutable {
      return scene.onClick(node, std::move(action));
    });
}

/** @tparam Callback Owning compact action returning void, expected<void, TreeError> or EditorCommandResult.
 * @param text UTF-8 caption copied immediately.
 * @param action Action optionally borrowing ClickEvent; return field.undo/redo directly to preserve its receipt.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use button recipe with scene-owned callback and exact command provenance reporting.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
inline Description<Button> button(std::string_view text, Callback action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return control<Button>(text, style, policy)
    .setup([action = std::move(action)](ManagedScene &scene, Node<Button> node) mutable {
      return scene.onClick(node, std::move(action));
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param model Observing managed state from the scene receiving this recipe.
 * @param changed Optional callback after the standard commit; cannot undo the commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use bound checkbox recipe.
 */
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<bool> model,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                          {.text = TextValue(text), .cycle = CheckCycle::TWO_STATE})
    .setup([model, changed = std::move(changed)](ManagedScene &scene, Node<CheckBox> node) mutable {
      return scene.bind(node, model, std::move(changed));
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param initial Initial checked value, owned by the receiving scene after mount.
 * @param changed Optional callback after the standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Checkbox recipe with internal managed state; no binding token escapes.
 */
inline Description<CheckBox> checkbox(std::string_view text, bool initial,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                     {.text = TextValue(text),
                                      .state = initial ? CheckState::CHECKED : CheckState::UNCHECKED,
                                      .cycle = CheckCycle::TWO_STATE}),
    [initial, changed = std::move(changed)](ManagedScene &scene, Node<CheckBox> node,
                                            const ControlSettings &settings) mutable {
      if (settings.state == CheckState::MIXED || settings.cycle == CheckCycle::THREE_STATE) {
        return std::expected<void, ManagedError>(std::unexpected(detail::error(node, TreeError::INVALID_ARGUMENT)));
      }
      return scene
        .state(settings.state.value_or(initial ? CheckState::CHECKED : CheckState::UNCHECKED) == CheckState::CHECKED)
        .and_then([&scene, node, &changed](ManagedState<bool> model) {
          return scene.bind(node, model, std::move(changed));
        });
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param model Observing managed state from the scene receiving this recipe.
 * @param changed Optional callback after the standard commit; cannot undo the commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @param options Typed user activation cycle.
 * @return Single-use bound checkbox recipe.
 */
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<CheckState> model,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return detail::Access::controlSetup(
    control<CheckBox>(text, style, policy),
    [model, changed = std::move(changed), options](ManagedScene &scene, Node<CheckBox> node,
                                                   const ControlSettings &settings) mutable {
      options.cycle = settings.cycle.value_or(options.cycle);
      return scene.bind(node, model, std::move(changed), options);
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param initial Initial checked value, owned by the receiving scene after mount.
 * @param changed Optional callback after the standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @param options Typed user activation cycle.
 * @return Checkbox recipe with internal managed state; no binding token escapes.
 */
inline Description<CheckBox> checkbox(std::string_view text, CheckState initial,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                     {.text = TextValue(text), .state = initial}),
    [initial, changed = std::move(changed), options](ManagedScene &scene, Node<CheckBox> node,
                                                     const ControlSettings &settings) mutable {
      options.cycle = settings.cycle.value_or(options.cycle);
      return scene.state(settings.state.value_or(initial))
        .and_then([&scene, node, &changed, options](ManagedState<CheckState> model) {
          return scene.bind(node, model, std::move(changed), options);
        });
    });
}

/** @brief Construct a named checkbox with the fixed three-state activation cycle.
 * @param text UTF-8 caption copied now and validated under the receiving scene's width policy.
 * @param model Observing explicit state owned by the receiving scene.
 * @param changed Optional owning callback after the standard commit; cannot undo that commit.
 * @param style Owning manual appearances below selector declarations.
 * @param policy Owning layout constraints.
 * @return Single-use ThreeState recipe using the existing CheckBox binding and checked errors.
 */
inline Description<ThreeState> threeState(std::string_view text, ManagedState<CheckState> model,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::intrinsicStyle(control<ThreeState>(text, style, policy),
                                        StyleTarget{.type = "ThreeState", .control = true, .baseTypes = {"CheckBox"}})
    .setup([model, changed = std::move(changed)](ManagedScene &scene, Node<ThreeState> node) mutable {
      return scene.bind(Node<CheckBox>{node}, model, std::move(changed),
                        CheckBoxOptions{.cycle = CheckCycle::THREE_STATE});
    });
}

/** @brief Construct a ThreeState with scene-owned state and inherited checkbox presentation.
 * @param text UTF-8 caption copied now and validated at mount.
 * @param initial Initial explicit state, UNCHECKED by default; invalid values fail at binding.
 * @param changed Optional owning callback after each standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use recipe with natural ThreeState/CheckBox style names and the fixed three-state cycle.
 */
inline Description<ThreeState> threeState(std::string_view text, CheckState initial,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(
      detail::Access::intrinsicStyle(control<ThreeState>(text, style, policy),
                                     StyleTarget{.type = "ThreeState", .control = true, .baseTypes = {"CheckBox"}}),
      {.text = TextValue(text), .state = initial}),
    [initial, changed = std::move(changed)](ManagedScene &scene, Node<ThreeState> node,
                                            const ControlSettings &settings) mutable {
      return scene.state(settings.state.value_or(initial))
        .and_then([&scene, node, &changed](ManagedState<CheckState> model) {
          return scene.bind(Node<CheckBox>{node}, model, std::move(changed),
                            CheckBoxOptions{.cycle = CheckCycle::THREE_STATE});
        });
    });
}

/** @param text Explicit caption. @return Checkbox with context-provided initial state and cycle. */
inline Description<CheckBox> checkbox(std::string_view text) {
  return detail::Access::controlArguments(checkbox(text, CheckState::UNCHECKED), {.text = TextValue(text)});
}

/** @param text Explicit caption. @return ThreeState with context-provided initial state and fixed cycle. */
inline Description<ThreeState> threeState(std::string_view text) {
  return detail::Access::controlArguments(threeState(text, CheckState::UNCHECKED), {.text = TextValue(text)});
}

/** @brief Configure an explicit checkbox value without positional callback/style placeholders.
 * @tparam Options Exactly CheckBoxOptions after cv/ref removal; bare braces do not deduce this overload.
 * @param text UTF-8 caption copied immediately.
 * @param initial Initial state owned by the receiving scene after mount.
 * @param options Owning activation policy copied now; invalid values fail through the existing bind boundary.
 * @param changed Optional exact commit receipt callback; cannot undo a committed value.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return The existing checkbox recipe, preserving its ownership, errors and synchronization behavior.
 */
template <class Options>
  requires std::same_as<std::remove_cvref_t<Options>, CheckBoxOptions>
inline Description<CheckBox> checkbox(std::string_view text, CheckState initial, Options &&options,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlArguments(
    checkbox(text, initial, std::move(changed), style, policy, std::forward<Options>(options)),
    {.text = TextValue(text), .state = initial, .cycle = options.cycle});
}

/** @brief Configure shared checkbox state without positional callback/style placeholders.
 * @tparam Options Exactly CheckBoxOptions after cv/ref removal; implicit conversions are not admitted.
 * @param text UTF-8 caption copied immediately.
 * @param model Observing managed explicit state from the receiving scene.
 * @param options Owning activation policy copied now, including from a temporary or const lvalue.
 * @param changed Optional exact commit receipt callback.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return The existing shared-state checkbox recipe; captures and cleanup follow the original factory.
 */
template <class Options>
  requires std::same_as<std::remove_cvref_t<Options>, CheckBoxOptions>
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<CheckState> model, Options &&options,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlArguments(
    checkbox(text, model, std::move(changed), style, policy, std::forward<Options>(options)),
    {.text = TextValue(text), .cycle = options.cycle});
}

namespace detail {
/** @brief One editor constructor path; context settings never replace an explicit shared model.
 * @param external Explicit observing model, or absent for independent internal state.
 * @param initial Owned fallback text; an explicit positional literal is also an instance override.
 * @param config Legacy full options/services, retained by value until mount.
 * @param inherit Whether omitted options inherit the immutable context.
 * @param resolver Owning synchronous text adapter; no callback survives its normal binding lifetime.
 * @return Existing editor/binding recipe with constructor-only settings access.
 */
inline Description<LineEdit> lineEditRecipe(std::optional<ManagedState<std::string>> external, TextValue initial,
                                            LineEditConfig config, bool inherit, TextResolver resolver = {}) {
  auto model = std::make_shared<ManagedState<std::string>>(external.value_or(ManagedState<std::string>{}));
  auto description =
    Access::cold<LineEdit>([initial = std::move(initial), options = config.options, policy = config.policy,
                            resolver = std::move(resolver), model, external = external.has_value()](
                             ManagedScene &scene, Node<Widget> parent,
                             MountContext &context) mutable -> std::expected<Node<LineEdit>, ManagedError> {
      const ControlSettings empty;
      const auto &settings = context.controlSettings ? *context.controlSettings : empty;
      auto effective = controlLineEditOptions(settings, options, scene.widthPolicy(), resolver, parent);
      if (!effective) {
        return std::unexpected(effective.error());
      }
      auto placeholder = lineEditPlaceholder(*effective, parent);
      if (!placeholder) {
        return std::unexpected(placeholder.error());
      }
      if (!external) {
        const auto &text = settings.text ? *settings.text : initial;
        auto resolved = resolver ? text.resolve(scene.widthPolicy(), resolver) : text.resolve(scene.widthPolicy());
        if (!resolved) {
          auto failure = error(parent, TreeError::INVALID_ARGUMENT);
          if (const auto *textError = std::get_if<TextError>(&resolved.error())) {
            failure.text = *textError;
          } else {
            failure.resource = std::get<TextResourceError>(resolved.error());
          }
          return std::unexpected(failure);
        }
        auto state = scene.state(std::string(resolved->bytes()));
        if (!state) {
          return std::unexpected(state.error());
        }
        *model = *state;
      }
      return scene.emplace<LineEdit>(parent, *effective, policy, std::move(*placeholder));
    });
  return Access::controlSetup(
    Access::configuredControl(std::move(description), "LineEdit", inherit),
    [model, result = std::move(config.result), clipboard = std::move(config.clipboard),
     validate = std::move(config.validate)](ManagedScene &scene, Node<LineEdit> node, const ControlSettings &) mutable {
      return scene.bind(node, *model, std::move(result), std::move(clipboard), std::move(validate));
    });
}
} // namespace detail

/** @param model Explicit scene-owned model. @param config Full legacy options, bypassing context defaults.
 * @return Bound editor; an additional settings modifier explicitly overrides those options.
 */
inline Description<LineEdit> lineEdit(ManagedState<std::string> model, LineEditConfig config) {
  return detail::lineEditRecipe(model, TextValue(""), std::move(config), false);
}

/** @param model Explicit scene-owned model. @return Editor inheriting sparse context options. */
inline Description<LineEdit> lineEdit(ManagedState<std::string> model) {
  return detail::lineEditRecipe(model, TextValue(""), {}, true);
}

/** @param initial Owned explicit initial text. @param config Full legacy editor options.
 * @param resolver Optional synchronous text adapter. @return Independent editor preserving full-config semantics.
 */
inline Description<LineEdit> lineEdit(TextValue initial, LineEditConfig config, TextResolver resolver = {}) {
  return detail::lineEditRecipe(std::nullopt, std::move(initial), std::move(config), false, std::move(resolver));
}

/** @param initial Explicit initial expression. @return Independent editor inheriting context options. */
inline Description<LineEdit> lineEdit(TextValue initial) {
  auto arguments = ControlSettings{.text = initial};
  return detail::Access::controlArguments(detail::lineEditRecipe(std::nullopt, std::move(initial), {}, true),
                                          std::move(arguments));
}

/** @param initial UTF-8 copied immediately. @param config Full legacy editor options.
 * @return Independent editor preserving the explicit full configuration.
 */
inline Description<LineEdit> lineEdit(std::string_view initial, LineEditConfig config) {
  return lineEdit(TextValue(initial), std::move(config));
}

/** @param initial UTF-8 copied immediately. @return Independent editor inheriting context options. */
inline Description<LineEdit> lineEdit(std::string_view initial) {
  return lineEdit(TextValue(initial));
}

/** @return Independent editor whose initial text and options may come from context defaults. */
inline Description<LineEdit> lineEdit() {
  return detail::lineEditRecipe(std::nullopt, TextValue(""), {}, true);
}

/** @tparam Settings Exactly ControlSettings; bare braces keep the legacy full-config overload.
 * @tparam Initial Explicit text expression, string or scene-owned model accepted by lineEdit.
 * @param initial Explicit initial value/model. @param patch Owning sparse final overrides.
 * @return Context-aware editor with only present fields overridden.
 */
template <class Initial, class Settings>
  requires std::same_as<std::remove_cvref_t<Settings>, ControlSettings> &&
           requires(Initial &&initial) { lineEdit(std::forward<Initial>(initial)); }
inline Description<LineEdit> lineEdit(Initial &&initial, Settings &&patch) {
  return lineEdit(std::forward<Initial>(initial)).settings(std::forward<Settings>(patch));
}

/** @tparam Settings Exactly ControlSettings. @param initial Owning explicit expression.
 * @param patch Sparse final overrides. @param resolver Synchronous text adapter.
 * @return Context-aware editor supporting localized text and placeholders.
 */
template <class Settings>
  requires std::same_as<std::remove_cvref_t<Settings>, ControlSettings>
inline Description<LineEdit> lineEdit(TextValue initial, Settings &&patch, TextResolver resolver) {
  auto arguments = ControlSettings{.text = initial};
  return detail::Access::controlArguments(
           detail::lineEditRecipe(std::nullopt, std::move(initial), {}, true, std::move(resolver)),
           std::move(arguments))
    .settings(std::forward<Settings>(patch));
}

/** @param reference Empty observing slot. @param initial Explicit text expression.
 * @param config Full legacy options. @param resolver Optional synchronous text adapter.
 * @return Independent editor with deferred Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, TextValue initial, LineEditConfig config,
                                      TextResolver resolver = {}) {
  return lineEdit(std::move(initial), std::move(config), std::move(resolver)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param initial Explicit text expression.
 * @return Context-aware editor with deferred Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, TextValue initial) {
  return lineEdit(std::move(initial)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param initial UTF-8 copied now. @param config Full legacy options.
 * @return Independent editor preserving the complete explicit configuration.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, std::string_view initial, LineEditConfig config) {
  return lineEdit(initial, std::move(config)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param initial UTF-8 copied now.
 * @return Context-aware editor with deferred Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, std::string_view initial) {
  return lineEdit(initial).reference(std::move(reference));
}

/** @param reference Empty observing slot. @return Editor inheriting initial text and options from context. */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference) {
  return lineEdit().reference(std::move(reference));
}

/** @tparam Settings Exactly ControlSettings. @tparam Initial Existing text/model initial input.
 * @param reference Empty observing slot. @param initial Explicit value/model. @param patch Sparse overrides.
 * @return Context-aware typed editor with deferred publication.
 */
template <class Initial, class Settings>
  requires std::same_as<std::remove_cvref_t<Settings>, ControlSettings> &&
           requires(Initial &&initial) { lineEdit(std::forward<Initial>(initial)); }
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, Initial &&initial, Settings &&patch) {
  return lineEdit(std::forward<Initial>(initial), std::forward<Settings>(patch)).reference(std::move(reference));
}

/// @brief Describe a managed virtual table without opening a range until mount.
/// @tparam T Immutable model row accepted by the column formatters; deduced from model.
/// @param model Weak UI-affine handle belonging to the scene receiving this recipe.
/// @param columns Descriptions copied now, including owning headers and formatter objects.
/// @param options Existing table settings, validated by managed preparation.
/// @param policy Existing layout constraints for the table.
/// @param prepared Owning provider callback after preparation, including retries.
/// Empty is valid for ready snapshots; pending work without it returns INVALID_ARGUMENT.
/// @return Single-use recipe with standard input, styles and automatic preparation.
/// Construction may throw while copying columns or callbacks. Mount preserves typed errors
/// and subtree rollback. The scene owns the model; reference captures remain borrowed.
template <class T>
Description<VirtualTable<T>> virtualTable(ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          VirtualTableOptions options = {}, SizePolicy policy = {},
                                          std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  // A recipe may outlive the caller's array or vector. Copy before retaining a mount callback.
  auto ownedColumns = std::vector<TableColumn<T>>(columns.begin(), columns.end());

  return detail::Access::describe<VirtualTable<T>>(
    [model, columns = std::move(ownedColumns), options, policy,
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent) mutable {
      return scene.virtualTable(parent, model, columns, options, policy, std::move(prepared));
    });
}

/// @brief Describe a managed virtual list without opening a range until mount.
/// @tparam T Immutable model row accepted by the formatter; deduced from model.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param delegate Owning formatter invoked for visible rows during preparation.
/// @param options Existing list settings, validated by managed preparation.
/// @param policy Existing layout constraints for the list.
/// @param prepared Owning provider callback after preparation. Empty rejects pending work
/// with INVALID_ARGUMENT. Captured references must remain valid until retirement.
/// @return Single-use recipe retaining callbacks and standard managed input/style behavior.
/// Capture construction may throw; mount preserves typed errors and subtree rollback.
template <class T>
Description<VirtualList<T>> virtualList(ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        VirtualListOptions options = {}, SizePolicy policy = {},
                                        std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return detail::Access::describe<VirtualList<T>>(
    [model, delegate = std::move(delegate), options, policy,
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent) mutable {
      return scene.virtualList(parent, model, std::move(delegate), options, policy, std::move(prepared));
    });
}

/// @brief Describe a managed chart using the existing frame preparation order.
/// @tparam T Immutable model row projected into optional OHLC values; deduced from model.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param accessor Owning projection; nullopt represents a gap, as in the managed chart API.
/// @param options Existing chart mode, width and glyph configuration.
/// @param first Owning origin callback, invoked in composition registration order during
/// preparation. Empty means row zero. NodeRef observations are already published then.
/// @param prepared Owning provider callback after preparation, including retries. Empty
/// rejects pending work with INVALID_ARGUMENT. Explicit reference captures remain borrowed.
/// @return Single-use recipe with automatic style/preparation and no input binding.
/// Capture construction may throw; mount preserves typed errors and subtree rollback.
template <class T>
Description<Chart<T>> chart(ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                            ChartOptions options = {}, ManagedChartOrigin first = {},
                            std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return detail::Access::describe<Chart<T>>(
    [model, accessor = std::move(accessor), options, first = std::move(first),
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent) mutable {
      return scene.chart(parent, model, std::move(accessor), options, std::move(first), std::move(prepared));
    });
}

/// @brief Named owning settings for a table recipe; defaults match the positional factory.
/// @tparam T Immutable model row type, matching the receiving ManagedModel.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct VirtualTableConfig {
  VirtualTableOptions options{};    ///< Existing dimensions, width policy, budgets and styles.
  SizePolicy policy{};              ///< Layout constraints in terminal cells.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/// @brief Named owning settings for a list recipe; defaults match the positional factory.
/// @tparam T Immutable model row type, matching the receiving ManagedModel.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct VirtualListConfig {
  VirtualListOptions options{};     ///< Existing row height, overscan, width policy and styles.
  SizePolicy policy{};              ///< Layout constraints in terminal cells.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/// @brief Named owning settings for a chart recipe; defaults match the positional factory.
/// @tparam T Immutable model row type projected by the chart accessor.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct ChartConfig {
  ChartOptions options{};           ///< Existing plot mode, width, glyphs and styles.
  ManagedChartOrigin first{};       ///< Origin evaluated during preparation; empty starts at row zero.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/** @brief Owning cold text collection recipe; attaching happens only during a complete mount. */
struct TextCollectionRecipe {
  TextSource source;        ///< Weak direct caption/draft/committed text identity, including same-attempt pending Ref.
  TextSplitOptions options; ///< Positive token and whole-input limits.
  bool splitWords{};        ///< True for whitespace-delimited tokens; false for extended graphemes.
};

/** @param source Read-only text identity; copying does not read or attach.
 * @param options Whole-input limits and positive token cap.
 * @return Cold whitespace-token recipe; punctuation is preserved, not UAX #29 word-break.
 */
inline TextCollectionRecipe words(TextSource source, TextSplitOptions options = {}) {
  return {std::move(source), options, true};
}

/** @param source Read-only text identity, including a direct Ref reserved in this mount.
 * @param options Whole-input limits and positive token cap.
 * @return Cold recipe with one token per existing extended grapheme, including whitespace.
 */
inline TextCollectionRecipe graphemes(TextSource source, TextSplitOptions options = {}) {
  return {std::move(source), options, false};
}

/** @param recipe Cold text transform retaining owning segments with reset-on-change row IDs.
 * @param config Existing list options and sizing; the default formatter displays each complete token.
 * @return Single-use list recipe; pure initial finalization precedes all public Ref publication.
 * The temporary internal shell cannot prepare or paint before its real model and consumer are installed.
 */
inline Description<VirtualList<TextSegment>> virtualList(TextCollectionRecipe recipe,
                                                         VirtualListConfig<TextSegment> config = {}) {
  return detail::Access::cold<VirtualList<TextSegment>>(
    [recipe = std::move(recipe), config = std::move(config)](
      ManagedScene &scene, Node<Widget> parent,
      detail::MountContext &context) mutable -> std::expected<Node<VirtualList<TextSegment>>, ManagedError> {
      auto graph = std::make_shared<detail::Access::ColdGraph>();
      graph->scene = &scene;
      context.rollback.push_back([graph] {
        graph->retire();
      });
      graph->pending = true;
      detail::Access::beginCold(scene);
      config.options.width_policy = scene.widthPolicy();
      auto node = scene.emplace<VirtualList<TextSegment>>(
        parent, ModelConsumer<TextSegment>{}, ManagedListFormatter<TextSegment>([](const ModelRow<TextSegment> &row) {
          return row.value.text();
        }),
        config.options, config.policy);
      if (!node) {
        return node;
      }
      context.finalize.push_back([&scene, &context, graph, node = *node, recipe = std::move(recipe),
                                  prepared = std::move(config.prepared)]() mutable {
        return detail::Access::completeTextList(scene, node, recipe.source, recipe.splitWords, recipe.options, context,
                                                graph, std::move(prepared));
      });
      return node;
    });
}

/// @brief Describe a table using named settings and the same owning mount transaction.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly VirtualTableConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param columns Definitions copied now, including headers and formatter captures.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe; mount and preparation preserve the positional factory's typed errors.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy table options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualTableConfig<T>>
Description<VirtualTable<T>>
virtualTable(ManagedModel<T> model, std::type_identity_t<std::span<const TableColumn<T>>> columns, Config &&config) {
  return virtualTable(model, columns, config.options, config.policy, std::forward<Config>(config).prepared);
}

/// @brief Describe a list using named settings without changing its managed lifecycle.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly VirtualListConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param delegate Owning visible-row formatter; reference captures remain borrowed.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe preserving positional mount, preparation and error semantics.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy list options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualListConfig<T>>
Description<VirtualList<T>> virtualList(ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        Config &&config) {
  return virtualList(model, std::move(delegate), config.options, config.policy, std::forward<Config>(config).prepared);
}

/// @brief Describe a chart using named settings without changing preparation order.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly ChartConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param accessor Owning projection to optional OHLC; nullopt represents a gap.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe preserving positional mount, preparation and error semantics.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy chart options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, ChartConfig<T>>
Description<Chart<T>> chart(ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                            Config &&config) {
  return chart(model, std::move(accessor), config.options, std::forward<Config>(config).first,
               std::forward<Config>(config).prepared);
}

/** @brief Compose a retained linear layout with consuming child recipes.
 * @tparam T Existing Row or Column subtype
 * accepting LinearOptions.
 * @tparam Children Description types, passed as rvalues.
 * @param options Owning layout settings, validated by the existing layout engine.
 * @param children Owning recipes, mounted in argument order after the parent setup.
 * @return Single-use parent recipe; no second runtime or retained description tree is created.
 */
template <class T, detail::DescriptionInput... Children>
Description<T> linear(LinearOptions options, Children &&...children) {
  auto description = detail::Access::configuredControl(
    detail::Access::cold<T>([options](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) mutable {
      if (context.controlSettings && context.controlSettings->gap) {
        options.gap = *context.controlSettings->gap;
      }
      return scene.emplace<T>(parent, options);
    }),
    detail::Access::controlType<T>());

  (detail::Access::layoutChild(description, children), ...);

  (description.state_->children.push_back(
     [child =
        std::forward<Children>(children)](ManagedScene &scene, Node<Widget> parent,
                                          detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
       auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

       return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
     }),
   ...);

  return detail::Access::controlArguments(std::move(description), {.gap = options.gap});
}

/** @brief Immediately consume a range into the existing linear-layout mount transaction.
 * @tparam T Existing Row or Column subtype accepting LinearOptions.
 * @tparam R Input range of one Description type; elements must be rvalues, or R an rvalue non-view, non-borrowed
 * container.
 * @param options Owning layout settings, validated by the existing layout engine.
 * @param children Recipes consumed once, in iteration order, before this function returns.
 * @return Single-use parent owning every recipe; an empty range creates an empty layout.
 * Use views::as_rvalue for explicit transfer from lvalue containers. Borrowed lvalue elements,
 * including those exposed by temporary views, are rejected. No iterator or view is retained.
 * Custom non-view, non-borrowed ranges must own their elements; that ownership cannot be inferred.
 * Construction/iteration may throw; already transferred source elements are not restored.
 * Mount preserves the original child error, subtree rollback and deferred reference publication.
 */
template <class T, detail::DescriptionRange R> Description<T> linear(LinearOptions options, R &&children) {
  auto description = detail::Access::configuredControl(
    detail::Access::cold<T>([options](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) mutable {
      if (context.controlSettings && context.controlSettings->gap) {
        options.gap = *context.controlSettings->gap;
      }
      return scene.emplace<T>(parent, options);
    }),
    detail::Access::controlType<T>());

  for (auto &&child : children) {
    detail::Access::layoutChild(description, child);
    description.state_->children.push_back(
      [child = std::move(child)](ManagedScene &scene, Node<Widget> parent,
                                 detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
        auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      });
  }

  return detail::Access::controlArguments(std::move(description), {.gap = options.gap});
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param options Owning horizontal layout settings.
 * @param children Recipes transferred now, in left-to-right order; lvalue elements require explicit transfer.
 * @return Owning row recipe with transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Row> row(LinearOptions options, R &&children) {
  return linear<Row>(options, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param children Recipes transferred now, in left-to-right order; lvalue elements require explicit transfer.
 * @return Owning row recipe with default settings and transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Row> row(R &&children) {
  return detail::Access::controlArguments(linear<Row>({}, std::forward<R>(children)), {});
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param options Owning vertical layout settings.
 * @param children Recipes transferred now, in top-to-bottom order; lvalue elements require explicit transfer.
 * @return Owning column recipe with transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Column> column(LinearOptions options, R &&children) {
  return linear<Column>(options, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param children Recipes transferred now, in top-to-bottom order; lvalue elements require explicit transfer.
 * @return Owning column recipe with default settings and transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Column> column(R &&children) {
  return detail::Access::controlArguments(linear<Column>({}, std::forward<R>(children)), {});
}

/** @tparam Children Rvalue Description types.
 * @param options Owning horizontal layout settings.
 * @param children Owning child recipes in left-to-right order.
 * @return Single-use row recipe.
 */
template <detail::DescriptionInput... Children> Description<Row> row(LinearOptions options, Children &&...children) {
  return linear<Row>(options, std::forward<Children>(children)...);
}

/** @brief Describe a semantic header with independent local rows and shared grid columns.
 * @tparam Children Consuming real child descriptions. @param children Real controls in structural order.
 * @return Owning section; no Widget, focus stop or separate tree is introduced.
 */
template <detail::DescriptionInput... Children> GridSection header(Children &&...children) {
  GridSection section(GridRegion::HEADER);

  (section.children_.push_back(detail::Access::erase(std::forward<Children>(children))), ...);

  return section;
}

/** @brief Describe semantic body rows, sharing their parent's columns with its header.
 * @tparam Children Consuming real child descriptions. @param children Real controls in body structural order.
 * @return Owning section; spans cannot cross the header/body boundary.
 */
template <detail::DescriptionInput... Children> GridSection body(Children &&...children) {
  GridSection section(GridRegion::BODY);

  (section.children_.push_back(detail::Access::erase(std::forward<Children>(children))), ...);

  return section;
}

/** @brief Compose a grid using the existing managed mount transaction.
 * @tparam Children Consuming child Description types, including custom final widgets.
 * @param children Owning recipes mounted in sibling order; no placement is inferred.
 * @return Single-use grid recipe; failed child mount rolls back the whole subtree.
 * columns/rows opt into automatic placement, checked before factories or setup. Without
 * these modifiers, setGridOptions retains the existing explicit full-identity contract.
 * Construction may allocate; mounting retains the existing typed error and cleanup rules.
 */
template <detail::GridInput... Children> Description<Grid> grid(Children &&...children) {
  auto layout = std::make_shared<detail::RecipeLayout>();

  layout->gridContainer = true;

  auto description =
    detail::Access::cold<Grid>([layout](ManagedScene &scene, Node<Widget> parent, detail::MountContext &) {
      if (layout->grid) {
        return scene.emplace<Grid>(parent, detail::resolvedGridOptions(*layout), GridPlacementMode::DECLARATIVE,
                                   SizePolicy{}, layout->scroll, scene.widthPolicy());
      }

      GridOptions options;

      options.presentation = layout->presentation;

      return scene.emplace<Grid>(parent, options);
    });

  description.state_->layout = layout;
  const auto append = [&](auto &&child) {
    if constexpr (std::same_as<std::remove_cvref_t<decltype(child)>, GridSection>) {
      detail::Access::gridSection(description, std::forward<decltype(child)>(child));
    } else {
      ++layout->directItems;
      detail::Access::gridChild(description, std::forward<decltype(child)>(child));
    }
  };

  (append(std::forward<Children>(children)), ...);

  return description;
}

/** @tparam Children Rvalue Description types.
 * @param children Owning child recipes in left-to-right order.
 * @return Single-use row recipe with default layout settings.
 */
template <detail::DescriptionInput... Children> Description<Row> row(Children &&...children) {
  return detail::Access::controlArguments(linear<Row>({}, std::forward<Children>(children)...), {});
}

/** @tparam Children Rvalue Description types.
 * @param options Owning vertical layout settings.
 * @param children Owning child recipes in top-to-bottom order.
 * @return Single-use column recipe.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(LinearOptions options, Children &&...children) {
  return linear<Column>(options, std::forward<Children>(children)...);
}

/** @tparam Children Rvalue Description types.
 * @param children Owning child recipes in top-to-bottom order.
 * @return Single-use column recipe with default layout settings.
 */
template <detail::DescriptionInput... Children> Description<Column> column(Children &&...children) {
  return detail::Access::controlArguments(linear<Column>({}, std::forward<Children>(children)...), {});
}

/** @param value Semantic non-character key; raw input is matched before router normalization.
 * @param modifiers Exact modifiers required by ManagedScene::onKey.
 * @return Owning chord value; PRESS and REPEAT are admitted by onKey.
 */
inline KeyEvent key(InputKey value, InputModifiers modifiers = {}) {
  return {value, {}, modifiers, KeyAction::PRESS};
}

/** @param scalar Character scalar, including existing control-key representations.
 * @param modifiers Exact modifiers required by ManagedScene::onKey.
 * @return Owning CHARACTER chord; no keyboard layout or scan code is inferred.
 */
inline KeyEvent character(char32_t scalar, InputModifiers modifiers = {}) {
  return {InputKey::CHARACTER, scalar, modifiers, KeyAction::PRESS};
}

/** @param text Caption copied into an unbound button recipe.
 * @return Consuming recipe; add onClick before mount or Ref.onClick after successful mount.
 */
inline Description<Button> button(std::string_view text) {
  return control<Button>(text, {}, {});
}

/** @param reference Shared empty slot, published after the whole mount succeeds.
 * @param text Caption copied now.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Label recipe sharing the existing reference transaction.
 */
inline Description<Label> label(Ref<Label> reference, std::string_view text, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return label(text, style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text Caption copied now.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Unbound button recipe for late Ref.onClick registration before run.
 */
inline Description<Button> button(Ref<Button> reference, std::string_view text, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return control<Button>(text, style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @param reference Empty observing slot published only after the complete mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Label recipe retaining the original transactional reference publication.
 */
template <class Caption>
  requires(std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>)
inline Description<Label> label(Ref<Label> reference, Caption &&text, ControlStyle style = {}, SizePolicy policy = {}) {
  return label(std::forward<Caption>(text), style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @param reference Empty observing slot published only after the complete mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Button caption recipe; action registration remains independent.
 */
template <class Caption>
  requires(std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>)
inline Description<Button> button(Ref<Button> reference, Caption &&text, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @tparam Callback Owning compact managed action, optionally borrowing ClickEvent.
 * @param reference Empty observing slot published after the whole mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param action Independent checked activation callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Bound button recipe preserving existing callback and rollback contracts.
 */
template <class Caption, class Callback>
  requires((std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>) &&
           portals::detail::ManagedAction<Callback, ClickEvent>)
inline Description<Button> button(Ref<Button> reference, Caption &&text, Callback action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), std::move(action), style, policy).reference(std::move(reference));
}

/** @tparam Callback Owning compact action, optionally taking ClickEvent; void/expected/command result.
 * @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param action Owning callback; captures transfer to the scene at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Bound button recipe; registration errors participate in subtree rollback.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
Description<Button> button(Ref<Button> reference, std::string_view text, Callback action, ControlStyle style = {},
                           SizePolicy policy = {}) {
  return button(text, std::move(action), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param model Existing managed bool state.
 * @param changed Optional typed receipt callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Checkbox recipe retaining its original commit semantics.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text, ManagedState<bool> model,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return checkbox(text, model, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param model Existing managed explicit checkbox state.
 * @param changed Optional typed receipt callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param options Typed user activation cycle.
 * @return Checkbox recipe retaining its original commit semantics.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text, ManagedState<CheckState> model,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return checkbox(text, model, std::move(changed), style, policy, options).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text UTF-8 caption copied now.
 * @param initial Initial scene-owned state, UNCHECKED by default.
 * @param changed Optional owning receipt callback after standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Named three-state recipe with checked reference publication and fixed activation cycle.
 */
inline Description<ThreeState> threeState(Ref<ThreeState> reference, std::string_view text, CheckState initial,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return threeState(text, initial, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param text Explicit caption.
 * @return ThreeState inheriting its initial state from the immutable context.
 */
inline Description<ThreeState> threeState(Ref<ThreeState> reference, std::string_view text) {
  return threeState(text).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param text Explicit caption.
 * @return Checkbox inheriting its initial state and activation cycle from context.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text) {
  return checkbox(text).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text UTF-8 caption copied now.
 * @param model Explicit state owned by the receiving scene.
 * @param changed Optional owning receipt callback after standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Named recipe reusing the original checkbox binding and typed failures.
 */
inline Description<ThreeState> threeState(Ref<ThreeState> reference, std::string_view text,
                                          ManagedState<CheckState> model,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return threeState(text, model, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing editor slot published after complete mount.
 * @param model Existing managed text state.
 * @param config Owning editor callbacks/options; existing EditMode/history behavior is retained.
 * @return Bound editor recipe using the original factory and reference transaction.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, ManagedState<std::string> model, LineEditConfig config) {
  return lineEdit(model, std::move(config)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param model Explicit scene-owned model.
 * @return Editor inheriting context options while preserving the explicit shared model.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, ManagedState<std::string> model) {
  return lineEdit(model).reference(std::move(reference));
}

/** @tparam T Public widget subtype compatible with the forwarded constructor arguments.
 * @tparam Args Owning constructor argument types, following the existing custom factory contract.
 * @param reference Empty observing slot published after complete mount.
 * @param args Constructor arguments forwarded into owning recipe storage.
 * @return Existing custom recipe with deferred reference publication.
 */
template <class T, class... Args> Description<T> custom(Ref<T> reference, Args &&...args) {
  return custom<T>(std::forward<Args>(args)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty layout slot published after complete mount.
 * @param children Owning child recipes.
 * @return Existing column recipe with one additional observation.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(Ref<Column> reference, Children &&...children) {
  return column(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty layout slot published after complete mount.
 * @param children Owning child recipes.
 * @return Existing row recipe with one additional observation.
 */
template <detail::DescriptionInput... Children> Description<Row> row(Ref<Row> reference, Children &&...children) {
  return row(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty grid slot published after complete mount.
 * @param children Owning child recipes; placement remains explicit after mount.
 * @return Existing grid recipe with one additional observation.
 */
template <detail::GridInput... Children> Description<Grid> grid(Ref<Grid> reference, Children &&...children) {
  return grid(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes.
 * @param reference Empty row observation published after complete mount.
 * @param options Existing owning linear layout options.
 * @param children Owning child recipes, mounted in argument order.
 * @return Existing row recipe retaining its layout configuration.
 */
template <detail::DescriptionInput... Children>
Description<Row> row(Ref<Row> reference, LinearOptions options, Children &&...children) {
  return row(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes.
 * @param reference Empty column observation published after complete mount.
 * @param options Existing owning linear layout options.
 * @param children Owning child recipes, mounted in argument order.
 * @return Existing column recipe retaining its layout configuration.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(Ref<Column> reference, LinearOptions options, Children &&...children) {
  return column(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam T Control subtype compatible with the existing caption constructor.
 * @param reference Empty control observation published after complete mount.
 * @param text UTF-8 caption copied into owning storage.
 * @param style Existing manual appearances.
 * @param policy Existing layout constraints.
 * @return Existing control recipe with additive reference publication.
 */
template <class T>
Description<T> control(Ref<T> reference, std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<T>(text, style, policy).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty view observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param columns Column descriptions copied by the original factory.
 * @param options Existing table configuration.
 * @param policy Existing layout constraints.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing table recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<VirtualTable<T>> virtualTable(Ref<VirtualTable<T>> reference, ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          VirtualTableOptions options = {}, SizePolicy policy = {},
                                          std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return virtualTable(model, columns, options, policy, std::move(prepared)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty view observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param delegate Owning row formatter retained by the original recipe.
 * @param options Existing list configuration.
 * @param policy Existing layout constraints.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing list recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<VirtualList<T>> virtualList(Ref<VirtualList<T>> reference, ManagedModel<T> model,
                                        std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        VirtualListOptions options = {}, SizePolicy policy = {},
                                        std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return virtualList(model, std::move(delegate), options, policy, std::move(prepared)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty chart observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param accessor Owning OHLC projection retained by the original recipe.
 * @param options Existing chart configuration.
 * @param first Owning origin callback; empty starts at row zero.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing chart recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<Chart<T>> chart(Ref<Chart<T>> reference, ManagedModel<T> model,
                            std::type_identity_t<ManagedChartAccessor<T>> accessor, ChartOptions options = {},
                            ManagedChartOrigin first = {}, std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return chart(model, std::move(accessor), options, std::move(first), std::move(prepared))
    .reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original VirtualTableConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param columns Owning column descriptions copied by the original factory.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named table recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualTableConfig<T>>
Description<VirtualTable<T>> virtualTable(Ref<VirtualTable<T>> reference, ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          Config &&config) {
  return virtualTable(model, columns, std::forward<Config>(config)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original VirtualListConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param delegate Owning row formatter retained by the original recipe.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named list recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualListConfig<T>>
Description<VirtualList<T>> virtualList(Ref<VirtualList<T>> reference, ManagedModel<T> model,
                                        std::type_identity_t<ManagedListFormatter<T>> delegate, Config &&config) {
  return virtualList(model, std::move(delegate), std::forward<Config>(config)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original ChartConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param accessor Owning OHLC projection retained by the original recipe.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named chart recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, ChartConfig<T>>
Description<Chart<T>> chart(Ref<Chart<T>> reference, ManagedModel<T> model,
                            std::type_identity_t<ManagedChartAccessor<T>> accessor, Config &&config) {
  return chart(model, std::move(accessor), std::forward<Config>(config)).reference(std::move(reference));
}

/** @brief Consume a recipe into the existing managed subtree, with failure cleanup.
 * @tparam T Concrete root Widget type constructed by the recipe.
 * @param scene Existing UI-affine owner, outliving all mounted widgets.
 * @param description Owning rvalue recipe, consumed even when mounting fails.
 * @param controls Owning immutable defaults snapshot validated before construction.
 * @return Observing root or original typed failure plus independent cleanup error. Earlier
 * scene nodes and states survive. Reusing an empty/moved/consumed recipe returns INVALID_ARGUMENT.
 * References publish only after all nodes remain live. External callback effects are not undone.
 */
template <class T>
std::expected<Node<T>, ManagedError> mount(ManagedScene &scene, Description<T> &&description,
                                           ControlContext controls) try {
  auto admitted = detail::Access::check(scene, scene.root());
  if (!admitted) {
    auto consumed = std::move(description);
    return std::unexpected(admitted.error());
  }

  if (detail::Access::needsGridContext(description)) {
    auto installed = detail::ControlContextAccess::installFallbackSchema(controls);

    if (!installed) {
      auto consumed = std::move(description);
      return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
    }
  }

  auto prepared = detail::Access::defersControlValidation(description)
                    ? std::expected<ControlContext, ControlContextError>(controls)
                    : controls.prepared();
  if (!prepared) {
    auto consumed = std::move(description);
    return std::unexpected(detail::controlContextError(prepared.error(), scene.root()));
  }

  struct Mounting {
    ManagedScene &scene;

    ~Mounting() {
      detail::Access::endMount(scene);
    }
  } guard{scene};

  detail::Access::beginMount(scene);
  detail::MountContext context;
  context.controls = std::move(*prepared);
  std::expected<Node<T>, ManagedError> result =
    std::unexpected(detail::error(scene.root(), TreeError::INVALID_ARGUMENT));

  try {
    result = detail::Access::mount(std::move(description), scene, scene.root(), context);

    if (result) {
      for (auto &finalize : context.finalize) {
        auto completed = finalize();
        if (!completed) {
          result = std::unexpected(completed.error());
          break;
        }
      }
    }
    if (result) {
      for (auto node : context.nodes) {
        auto live = detail::Access::check(scene, node);

        if (!live) {
          result = std::unexpected(live.error());

          break;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    result = std::unexpected(detail::error(context.root, TreeError::NO_MEMORY));
  } catch (...) {
    result = std::unexpected(detail::error(context.root, TreeError::CALLBACK_FAILED));
  }

  if (!result && context.root != Node<Widget>{}) {
    auto removed = scene.remove(context.root);

    if (!removed) {
      if (const auto *cause = std::get_if<TreeError>(&removed.error().cause)) {
        if (*cause != TreeError::STALE_NODE && *cause != TreeError::CLOSED) {
          result.error().cleanup = *cause;
        }
      }
    }
  }

  if (result) {
    context.committed = true;
    for (auto &publish : context.publish) {
      publish();
    }
  }

  return result;
} catch (const std::bad_alloc &) {
  // Transaction storage is constructed before any node exists; preserve the checked allocation boundary too.
  auto consumed = std::move(description);

  return std::unexpected(detail::error(scene.root(), TreeError::NO_MEMORY));
}

/** @param scene Existing UI owner. @param description Consuming recipe using builtin settings only.
 * @return Existing checked mount result; explicit contexts use the three-argument overload.
 */
template <class T> std::expected<Node<T>, ManagedError> mount(ManagedScene &scene, Description<T> &&description) {
  return mount(scene, std::move(description), ControlContext{});
}

/** @tparam Args Arguments accepted by an existing row overload, including references and layout options.
 * @param args Owning child recipes and optional row configuration, forwarded unchanged.
 * @return The existing horizontal row recipe; ownership, checked failures and layout semantics are unchanged.
 */
template <class... Args>
  requires requires(Args &&...args) { row(std::forward<Args>(args)...); }
auto hbox(Args &&...args) {
  return row(std::forward<Args>(args)...);
}

/** @tparam Args Arguments accepted by an existing column overload, including references and layout options.
 * @param args Owning child recipes and optional column configuration, forwarded unchanged.
 * @return The existing vertical column recipe; ownership, checked failures and layout semantics are unchanged.
 */
template <class... Args>
  requires requires(Args &&...args) { column(std::forward<Args>(args)...); }
auto vbox(Args &&...args) {
  return column(std::forward<Args>(args)...);
}

/** @brief Compose an application-owned nonmodal notification plane inside ordinary body content.
 * @tparam Body Consuming body description; only this body contributes intrinsic size.
 * @param center Copyable UI-affine observer published only after the complete mount succeeds.
 * @param body Ordinary retained content; dialogs belong outside this composition in overlayHost.
 * @param options Owning immutable host budgets and explicit text resolver.
 * @return Consuming overlay recipe with one private zero-intrinsic notification layer.
 */
template <detail::DescriptionInput Body>
Description<Overlay> notifications(NotificationCenter center, Body &&body, NotificationOptions options = {}) {
  auto layout = detail::Access::captureLayouts(body);
  auto description = detail::Access::cold<Overlay>(
    [center = std::move(center), body = std::forward<Body>(body),
     options](ManagedScene &scene, Node<Widget> parent,
              detail::MountContext &context) mutable -> std::expected<Node<Overlay>, ManagedError> {
      auto wrapper = scene.emplace<Overlay>(parent);
      if (!wrapper) {
        return std::unexpected(wrapper.error());
      }
      if (context.root == Node<Widget>{}) {
        context.root = *wrapper;
      }
      auto content = detail::Access::mount(std::move(body), scene, *wrapper, context);
      if (!content) {
        return std::unexpected(content.error());
      }
      auto bound = detail::Access::notifications(scene, *wrapper, *content, center, options, context);
      return bound ? wrapper : std::unexpected(bound.error());
    });

  return detail::Access::withLayout(std::move(description), std::move(layout));
}

/** @brief Observe the real notification-plane Overlay while keeping its service handle separate.
 * @tparam Body Consuming ordinary-body description, following the existing notification composition contract.
 * @param reference Empty visual observing slot, published only after the complete mount succeeds.
 * @param center Existing weak notification service; post() still returns its exact ToastPostReceipt.
 * @param body Ordinary retained content contributing the host's intrinsic size.
 * @param options Existing owning notification budgets and localization settings.
 * @return The existing notification recipe with one visual reference; no new runtime or service is created.
 */
template <detail::DescriptionInput Body>
Description<Overlay> notifications(Ref<Overlay> reference, NotificationCenter center, Body &&body,
                                   NotificationOptions options = {}) {
  return notifications(std::move(center), std::forward<Body>(body), std::move(options)).reference(std::move(reference));
}

/** @brief Compose a retained host with one ordinary body and initially closed dialog layers.
 * @tparam Body Consuming ordinary-body description.
 * @tparam Layers Consuming dialog descriptions, registered in declaration order.
 * @param options Positive owning layer budget, including closed layers.
 * @param body Ordinary content retained across every open/dismiss cycle.
 * @param layers Dialog recipes mounted in the same complete transaction.
 * @return Single-use host recipe; any child/finalization failure retires the whole unpublished subtree.
 */
template <detail::DescriptionInput Body, detail::DescriptionInput... Layers>
  requires(std::same_as<std::remove_cvref_t<Layers>, Description<Dialog>> && ...)
Description<OverlayHost> overlayHost(OverlayHostOptions options, Body &&body, Layers &&...layers) {
  auto layout = detail::Access::captureLayouts(body, layers...);
  auto description = detail::Access::cold<OverlayHost>(
    [options, body = std::forward<Body>(body), layers = std::tuple(std::forward<Layers>(layers)...)](
      ManagedScene &scene, Node<Widget> parent,
      detail::MountContext &context) mutable -> std::expected<Node<OverlayHost>, ManagedError> {
      auto host = scene.emplace<OverlayHost>(parent, options);
      if (!host) {
        return std::unexpected(host.error());
      }
      if (context.root == Node<Widget>{}) {
        context.root = *host;
      }
      auto content = detail::Access::mount(std::move(body), scene, *host, context);
      if (!content) {
        return std::unexpected(content.error());
      }
      auto bound = scene.overlayHost(*host, *content);
      if (!bound) {
        return std::unexpected(bound.error());
      }
      auto styled = scene.style(*host, StyleTarget{.type = "OverlayHost", .classes = {"overlay-host"}});
      if (!styled) {
        return std::unexpected(styled.error());
      }
      std::expected<void, ManagedError> result;
      std::apply(
        [&](auto &...layer) {
          const auto mount = [&](auto &recipe) {
            if (result) {
              auto child = detail::Access::mount(std::move(recipe), scene, *host, context);
              if (!child) {
                result = std::unexpected(child.error());
              }
            }
          };
          (mount(layer), ...);
        },
        layers);
      return result ? host : std::unexpected(result.error());
    });

  return detail::Access::withLayout(std::move(description), std::move(layout));
}

/** @tparam Body Consuming ordinary-body recipe.
 * @tparam Layers Consuming dialog recipes.
 * @param body Retained ordinary content.
 * @param layers Initially closed layers.
 * @return Host with the default maximum of 32 registered layers.
 */
template <detail::DescriptionInput Body, detail::DescriptionInput... Layers>
  requires(std::same_as<std::remove_cvref_t<Layers>, Description<Dialog>> && ...)
Description<OverlayHost> overlayHost(Body &&body, Layers &&...layers) {
  return overlayHost(OverlayHostOptions{}, std::forward<Body>(body), std::forward<Layers>(layers)...);
}

/** @tparam Body Consuming ordinary-body recipe.
 * @tparam Layers Consuming dialog recipes.
 * @param reference Empty host observation published with the complete mount.
 * @param body Retained ordinary content.
 * @param layers Initially closed layers.
 * @return Default host recipe publishing the optional observation.
 */
template <detail::DescriptionInput Body, detail::DescriptionInput... Layers>
  requires(std::same_as<std::remove_cvref_t<Layers>, Description<Dialog>> && ...)
Description<OverlayHost> overlayHost(Ref<OverlayHost> reference, Body &&body, Layers &&...layers) {
  return overlayHost(std::forward<Body>(body), std::forward<Layers>(layers)...).reference(std::move(reference));
}

/** @brief Compose a reusable initially closed dialog with a standard title and opaque panel.
 * @tparam Body Consuming application content recipe.
 * @param title Owning text expression resolved once with the scene's complete width policy.
 * @param body Retained content; opening never remounts it.
 * @param options Immutable panel geometry/input settings.
 * @return Single-use layer recipe, requiring an enclosing declared overlayHost.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(TextValue title, Body &&body, DialogOptions options = {}) {
  auto heading = label(std::move(title)).style(StyleTarget{.type = "Label", .classes = {"dialog-title"}});
  auto content = column(std::forward<Body>(body)).style(StyleTarget{.type = "Column", .classes = {"dialog-body"}});
  auto layout = detail::Access::captureLayouts(heading, content);
  auto description =
    detail::Access::cold<Dialog>([options, heading = std::move(heading), content = std::move(content)](
                                   ManagedScene &scene, Node<Widget> parent,
                                   detail::MountContext &context) mutable -> std::expected<Node<Dialog>, ManagedError> {
      auto valid = Dialog::validate(options);
      if (!valid) {
        return std::unexpected(detail::error(parent, valid.error()));
      }
      auto root = scene.emplace<Dialog>(parent, options);
      if (!root) {
        return std::unexpected(root.error());
      }
      if (context.root == Node<Widget>{}) {
        context.root = *root;
      }
      auto panel = detail::Access::dialogPanel(scene, *root, options.frame);
      if (!panel) {
        return std::unexpected(panel.error());
      }
      context.nodes.push_back(*panel);
      auto column = scene.emplace<Column>(*panel);
      if (!column) {
        return std::unexpected(column.error());
      }
      context.nodes.push_back(*column);
      auto titleNode = detail::Access::mount(std::move(heading), scene, *column, context);
      if (!titleNode) {
        return std::unexpected(titleNode.error());
      }
      auto mounted = detail::Access::mount(std::move(content), scene, *column, context);
      if (!mounted) {
        return std::unexpected(mounted.error());
      }
      auto bound = scene.dialog(*root, *panel, *mounted);
      if (!bound) {
        return std::unexpected(bound.error());
      }
      auto styled = scene.style(
        *root,
        StyleTarget{.type = "Dialog", .classes = {"dialog", options.mode == DialogMode::MODAL ? "modal" : "nonmodal"}});
      return styled ? root : std::unexpected(styled.error());
    });

  return detail::Access::withLayout(std::move(description), std::move(layout));
}

/** @tparam Body Consuming application content recipe.
 * @param reference Empty weak observation published after complete mount and anchor finalization.
 * @param title Owning title expression.
 * @param body Retained panel content.
 * @param options Immutable panel/input settings.
 * @return Standard reusable dialog recipe with its optional Ref.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(Ref<Dialog> reference, TextValue title, Body &&body, DialogOptions options = {}) {
  return dialog(std::move(title), std::forward<Body>(body), options).reference(std::move(reference));
}

/** @tparam Body Consuming application content recipe.
 * @param title Literal UTF-8 title copied into an owning expression immediately.
 * @param body Retained content inside the panel.
 * @param options Immutable geometry/input settings.
 * @return Same standard initially closed dialog recipe.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(std::string_view title, Body &&body, DialogOptions options = {}) {
  return dialog(TextValue(title), std::forward<Body>(body), options);
}

/** @tparam Body Consuming application content recipe.
 * @param reference Weak observation published only with a complete mount.
 * @param title Literal UTF-8 title copied immediately.
 * @param body Retained content.
 * @param options Immutable panel/input settings.
 * @return Same standard dialog recipe with its optional observation.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(Ref<Dialog> reference, std::string_view title, Body &&body, DialogOptions options = {}) {
  return dialog(std::move(reference), TextValue(title), std::forward<Body>(body), options);
}

/// @cond INTERNAL
namespace detail {
Description<Dialog> defaultThemeSelector(ThemeSelectorRef reference, std::span<const ThemeChoice> choices,
                                         const ThemeSelectionOptions &selection,
                                         const std::optional<std::string> &initialId, ThemeSelectorOptions options);
} // namespace detail

template <std::size_t N>
  requires(N > 0)
Description<Dialog> themeSelector(ThemeSelectorRef reference, const ThemeChoice (&choices)[N],
                                  ThemeSelectorOptions options) {
  return detail::defaultThemeSelector(std::move(reference), choices, ThemeSelectionOptions{}, {}, std::move(options));
}

template <class Catalog>
  requires std::same_as<std::remove_cvref_t<Catalog>, ThemeSelectorCatalog>
Description<Dialog> themeSelector(ThemeSelectorRef reference, Catalog &&catalog, ThemeSelectorOptions options) {
  return detail::defaultThemeSelector(std::move(reference), catalog.choices, catalog.selection, catalog.initialId,
                                      std::move(options));
}

/// @endcond

/** @brief Owning compact size stage; uses the same methods and existing solver as ordinary composition. */
struct SizeModifier {
  detail::SizeChange change; ///< Owning checked policy operation; no scene/widget borrow.

  /** @tparam T SizedWidget subtype described by the consuming input.
   * @param description Owning recipe consumed by this stage.
   * @param modifier Owning axis/weight update, applied in pipe order at mount.
   * @return Updated owning recipe; invalid values preserve transactional mount failure semantics.
   */
  template <std::derived_from<SizedWidget> T>
  friend Description<T> operator|(Description<T> &&description, SizeModifier modifier) {
    switch (modifier.change.dimension) {
    case detail::SizeDimension::WIDTH:
      return std::move(description).width(modifier.change.extent);
    case detail::SizeDimension::HEIGHT:
      return std::move(description).height(modifier.change.extent);
    case detail::SizeDimension::FLEX_X:
      return std::move(description).flexX(modifier.change.weight);
    case detail::SizeDimension::FLEX_Y:
      return std::move(description).flexY(modifier.change.weight);
    case detail::SizeDimension::FLEX:
      return std::move(description).flex(modifier.change.weight);
    }

    return std::move(description).flex(0);
  }
};

/** @param cells Nonnegative fixed horizontal extent; checked at mount.
 * @return Owning size stage replacing only the horizontal policy.
 */
constexpr SizeModifier width(std::int32_t cells) {
  return {{detail::SizeDimension::WIDTH, cells}};
}

/** @param cells Nonnegative fixed vertical extent; checked at mount.
 * @return Owning size stage replacing only the vertical policy.
 */
constexpr SizeModifier height(std::int32_t cells) {
  return {{detail::SizeDimension::HEIGHT, cells}};
}

/** @param weight Positive horizontal flex weight; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds and vertical policy.
 */
constexpr SizeModifier flexX(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX_X, 0, weight}};
}

/** @param weight Positive vertical flex weight; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds and horizontal policy.
 */
constexpr SizeModifier flexY(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX_Y, 0, weight}};
}

/** @param weight Positive weight for both axes; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds.
 */
constexpr SizeModifier flex(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX, 0, weight}};
}

template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Child &&child, ScrollAreaOptions options,
                                   std::shared_ptr<ScrollController> controller, ControlStyle style) {
  auto description = detail::Access::describe<ScrollArea>(
    [options, controller = std::move(controller), style](ManagedScene &scene, Node<Widget> parent) {
      return scene.emplace<ScrollArea>(parent, controller, scene.widthPolicy(), options, style);
    });
  detail::Access::layoutChild(description, child);
  description.state_->children.push_back(
    [child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> parent,
                                         detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
      auto mounted = detail::Access::mount(std::move(child), scene, parent, context);
      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    });
  return description;
}

/** @tparam Child Consuming ordinary content recipe.
 * @param reference Empty area slot, published only after the complete subtree mounts.
 * @param child Single ordinary content root.
 * @param options Owning placement/input settings.
 * @param controller Optional owning shared values.
 * @param style Manual appearance defaults.
 * @return Ref-first recipe with automatic lifecycle and checked rollback.
 */
template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Ref<ScrollArea> reference, Child &&child, ScrollAreaOptions options = {},
                                   std::shared_ptr<ScrollController> controller = {}, ControlStyle style = {}) {
  return scrollArea(std::forward<Child>(child), options, std::move(controller), style).reference(std::move(reference));
}

/** @param axis Projected axis.
 * @param options Owning input/paint settings.
 * @param style Manual defaults beneath themes.
 * @return Standalone bar with automatic default controller and sole metrics-writer lease.
 */
inline Description<ScrollBar> scrollBar(ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return detail::Access::describe<ScrollBar>([axis, options, style](ManagedScene &scene, Node<Widget> parent) {
    return scene.scrollBar(parent, {}, axis, options, style);
  });
}

/** @param controller Owning values shared with an area's metrics writer.
 * @param axis Projected axis.
 * @param options Owning input/paint settings.
 * @param style Manual defaults beneath themes.
 * @return Read projection; a missing controller is rejected rather than creating orphan metrics.
 */
inline Description<ScrollBar> scrollBar(std::shared_ptr<ScrollController> controller,
                                        ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return detail::Access::describe<ScrollBar>(
    [controller = std::move(controller), axis, options,
     style](ManagedScene &scene, Node<Widget> parent) -> std::expected<Node<ScrollBar>, ManagedError> {
      if (!controller) {
        return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
      }
      return scene.scrollBar(parent, controller, axis, options, style, false);
    });
}

/** @param reference Empty bar observer published after successful mount.
 * @param axis Selected standalone axis.
 * @param options Owning bar settings.
 * @param style Manual defaults.
 * @return Ref-first standalone writer recipe.
 */
inline Description<ScrollBar> scrollBar(Ref<ScrollBar> reference, ScrollAxis axis = ScrollAxis::VERTICAL,
                                        ScrollBarOptions options = {}, ControlStyle style = {}) {
  return scrollBar(axis, options, style).reference(std::move(reference));
}

/** @param reference Empty observer published after successful mount.
 * @param controller Owning area-shared values.
 * @param axis Selected projection axis.
 * @param options Owning bar settings.
 * @param style Manual defaults.
 * @return Ref-first area projection recipe with automatic binding and cleanup.
 */
inline Description<ScrollBar> scrollBar(Ref<ScrollBar> reference, std::shared_ptr<ScrollController> controller,
                                        ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return scrollBar(std::move(controller), axis, options, style).reference(std::move(reference));
}

template <detail::DescriptionInput Child>
Description<Border> border(Child &&child, BorderOptions options, ControlStyle style) {
  auto description = detail::Access::describe<Border>([options, style](ManagedScene &scene, Node<Widget> parent) {
    const auto valid = Border::validate(options);

    if (!valid) {
      return std::expected<Node<Border>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
    }

    return scene.emplace<Border>(parent, scene.widthPolicy(), options, style);
  });

  detail::Access::layoutChild(description, child);
  description.state_->children.push_back(
    [child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> parent,
                                         detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
      const auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    });

  return description;
}

/** @tparam Child Consuming Description type.
 * @param child Owning child mounted within the frame transaction.
 * @param options Owning frame glyph/paint-work policy.
 * @param style Owning manual appearances beneath themes.
 * @return Same owning frame recipe as border, with identical layout and failure semantics.
 */
template <detail::DescriptionInput Child>
Description<Border> frame(Child &&child, BorderOptions options = {}, ControlStyle style = {}) {
  return border(std::forward<Child>(child), options, style);
}

/** @brief Owning frame pipe stage over the same transactional border factory. */
struct BorderModifier {
  BorderOptions options; ///< Owning glyph/work settings.
  ControlStyle style;    ///< Owning manual appearances beneath themes.

  /** @tparam T Widget subtype described by the consuming input.
   * @param description Owning child recipe consumed by the frame.
   * @param modifier Owning glyph, style and finite work settings.
   * @return Owning frame containing the input in the same mount transaction.
   */
  template <class T> friend Description<Border> operator|(Description<T> &&description, BorderModifier modifier) {
    return border(std::move(description), modifier.options, modifier.style);
  }
};

/** @param options Owning frame glyph selection and finite paint cap.
 * @param style Owning manual appearances beneath themes.
 * @return Frame stage for a consuming description; no view/widget borrow is retained.
 */
inline BorderModifier border(BorderOptions options = {}, ControlStyle style = {}) {
  return {options, style};
}

/** @param options Owning frame glyph selection and finite paint cap.
 * @param style Owning manual appearances beneath themes.
 * @return The same frame stage as border(options, style).
 */
inline BorderModifier frame(BorderOptions options = {}, ControlStyle style = {}) {
  return {options, style};
}

/** @param value Finite value in the inclusive configured range; checked at mount.
 * @param options Owning finite bounds, intrinsic width, glyph mode and allocated-area paint cap.
 * @param style Owning manual appearances beneath theme declarations.
 * @param policy Owning layout constraints, validated by the existing solver.
 * @return Single-use determinate gauge recipe; invalid values/settings preserve typed mount errors.
 */
inline Description<Gauge> gauge(double value, GaugeOptions options = {}, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return detail::Access::describe<Gauge>([value, options, style, policy](ManagedScene &scene, Node<Widget> parent) {
    const auto valid = Gauge::validate(value, options);

    if (!valid) {
      return std::expected<Node<Gauge>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
    }

    return scene.emplace<Gauge>(parent, value, scene.widthPolicy(), options, style, policy);
  });
}

/** @param text UTF-8 caption copied now and checked at mount under the receiving WidthPolicy.
 * @param style Owning manual normal/focused/disabled appearances beneath themes.
 * @param policy Owning layout constraints, checked by the existing solver.
 * @return Existing Button recipe without brackets; activation still uses existing bindings/router.
 */
inline Description<Button> plainButton(std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::describe<Button>(
    [text = std::string(text), style, policy](ManagedScene &scene, Node<Widget> parent) {
      auto caption = GraphemeText::from(text, scene.widthPolicy());

      if (!caption) {
        auto failure = detail::textError(parent, caption.error());

        return std::expected<Node<Button>, ManagedError>(std::unexpected(failure));
      }

      return scene.emplace<Button>(parent, std::move(*caption), style, policy, ButtonChrome::NONE);
    });
}

/** @tparam Callback Existing compact action accepting optionally const ClickEvent&.
 * @param text UTF-8 caption copied immediately.
 * @param action Owning action transferred to the existing managed click binding at mount.
 * @param style Owning manual appearances beneath themes.
 * @param policy Owning layout constraints.
 * @return Existing unbracketed Button recipe; action errors preserve their checked boundary/provenance.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
Description<Button> plainButton(std::string_view text, Callback action, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return plainButton(text, style, policy).onClick(std::move(action));
}

/** @param reference Empty shared observing slot, published only after complete mount.
 * @param text UTF-8 caption copied immediately.
 * @param style Owning manual appearances beneath themes.
 * @param policy Owning layout constraints.
 * @return Existing unbracketed Button recipe sharing normal transactional reference publication.
 */
inline Description<Button> plainButton(Ref<Button> reference, std::string_view text, ControlStyle style = {},
                                       SizePolicy policy = {}) {
  return plainButton(text, style, policy).reference(std::move(reference));
}

namespace detail {
/** @brief Admit only owning descriptions or their exact managed checked result.
 * @tparam T Factory result after cv/ref normalization.
 */
template <class T> struct IsSceneRecipe : IsDescription<T> {};

/** @brief Admit a factory preserving an original managed error.
 * @tparam T Concrete widget constructed by the returned description.
 */
template <class T> struct IsSceneRecipe<std::expected<Description<T>, ManagedError>> : std::true_type {};

/** @brief Admit owning app results without widening managed root-recipe factories.
 * @tparam T Exact normalized no-argument factory result.
 */
template <class T> struct IsAppResult : IsSceneRecipe<T> {};

/** @brief An already configured owning facade is a complete app result. */
template <> struct IsAppResult<Scene> : std::true_type {};

/** @brief Preserve an exact managed failure before an owning facade exists. */
template <> struct IsAppResult<std::expected<Scene, ManagedError>> : std::true_type {};

/** @brief Admit only no-argument factories returning owning supported app results.
 * @tparam Factory Callable borrowed only through the synchronous upper runner boundary.
 */
template <class Factory>
concept AppFactory = std::invocable<Factory &> && !std::is_reference_v<std::invoke_result_t<Factory &>> &&
                     IsAppResult<std::remove_cvref_t<std::invoke_result_t<Factory &>>>::value;

/** @brief Deliver a completed pre-owner report without recursively invoking the sink.
 * @param sink Borrowed final sink, available before factory invocation.
 * @param report Owning diagnostic retained through delivery and stderr fallback.
 */
inline void deliverFactoryReport(const SceneReportHandler &sink, const SceneReport &report) noexcept {
  FactoryCleanupScope factoryMask;
  SetupFailureScope reportingScope({}, nullptr, nullptr);
  CurrentSceneScope currentMask;

  try {
    if (sink) {
      sink(report);
    } else {
      reportToStderr(report);
    }
  } catch (...) {
    reportToStderr(report);
  }
}

/** @brief Select the legacy factory signature before considering an Application borrow.
 * @tparam Factory Owning scene factory returning an owning description or exact checked description.
 */
template <class Factory>
concept SceneFactory =
  (std::invocable<Factory &, ManagedScene &> && !std::is_reference_v<std::invoke_result_t<Factory &, ManagedScene &>> &&
   IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &, ManagedScene &>>>::value) ||
  (!std::invocable<Factory &, ManagedScene &> && std::invocable<Factory &, ManagedScene &, Application &> &&
   !std::is_reference_v<std::invoke_result_t<Factory &, ManagedScene &, Application &>> &&
   IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &, ManagedScene &, Application &>>>::value);

/** @brief Compose declared layers through the existing host and one ordinary mount transaction.
 * @tparam Body Consuming ordinary body or exact explicit OverlayHost recipe.
 * @tparam Layers Consuming typed dialogs or erased recipes checked before publication.
 * @param body Existing ordinary content; an exact typed host takes priority over automatic wrapping.
 * @param layers Initially closed layers, appended in declaration order to that same host.
 * @return Owning erased recipe; every node/reference remains subject to the original transaction rollback.
 */
template <DescriptionInput Body, SceneLayer... Layers> Description<Widget> layered(Body &&body, Layers &&...layers) {
  auto hosted = [&] {
    if constexpr (std::same_as<std::remove_cvref_t<Body>, Description<OverlayHost>>) {
      return std::forward<Body>(body);
    } else {
      return overlayHost(std::forward<Body>(body));
    }
  }();

  auto layout = Access::captureLayouts(hosted, layers...);
  auto description =
    Access::transaction([hosted = std::move(hosted), layers = std::tuple(std::forward<Layers>(layers)...)](
                          ManagedScene &owner, Node<Widget> parent,
                          MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      auto root = Access::mount(std::move(hosted), owner, parent, context);

      if (!root) {
        return std::unexpected(root.error());
      }

      auto host = Access::layerHost(owner, *root);

      if (!host) {
        return std::unexpected(host.error());
      }

      std::expected<void, ManagedError> result;

      std::apply(
        [&](auto &...layer) {
          const auto mountLayer = [&](auto &recipe) {
            if (!result) {
              return;
            }

            auto mounted = Access::mount(std::move(recipe), owner, *host, context);

            result = mounted ? Access::checkLayer(owner, *mounted) : std::unexpected(mounted.error());
          };

          (mountLayer(layer), ...);
        },
        layers);

      return result ? std::expected<Node<Widget>, ManagedError>(*root) : std::unexpected(result.error());
    });

  return Access::withLayout(std::move(description), std::move(layout));
}

/** @brief Build one owning final factory diagnostic without allocating.
 * @param failure Original typed cause, independent of terminal/widget lifetime.
 * @param origin Factory/setup invocation site.
 * @param cleanup Bounded diagnostics from actual abandoned factory-local destructors, merged without replacing primary.
 * @return Stamped primary and independent cleanup failure with exact bounded overflow.
 */
inline SceneReport factoryFailureReport(ManagedError failure, std::source_location origin,
                                        const FactoryCleanup &cleanup = {}) noexcept {
  SceneReport report;

  if (!failure.operation) {
    failure.operation = "scene.factory";
    failure.operationOrigin = origin;
  }

  failure.execution = "scene.factory";

  report.failure = std::move(failure);
  report.origin = report.failure->operationOrigin.value_or(origin);
  report.droppedErrors = cleanup.attach(*report.failure);

  return report;
}

/** @brief Deliver a pre-owner factory failure through the already available final sink.
 * @param sink Pinned handler borrowed for this synchronous call; empty selects stderr.
 * @param failure Original typed cause, independent of terminal/widget lifetime.
 * @param origin Factory/setup invocation site.
 * @param cleanup Bounded actual destructor cleanup diagnostics.
 */
inline void reportFactoryFailure(const SceneReportHandler &sink, ManagedError failure, std::source_location origin,
                                 const FactoryCleanup &cleanup = {}) noexcept {
  const auto report = factoryFailureReport(std::move(failure), origin, cleanup);

  deliverFactoryReport(sink, report);
}

/** @brief Admit exact stationary setup results.
 * @tparam Setup Callable taking Scene& and returning void or exact expected<void,ManagedError>.
 */
template <class Setup>
concept AppSetup = requires(Setup &setup, Scene &owner) {
  { setup(owner) } -> std::same_as<void>;
} || requires(Setup &setup, Scene &owner) {
  { setup(owner) } -> std::same_as<std::expected<void, ManagedError>>;
};

} // namespace detail

/** @tparam T Concrete Widget type described by the consuming recipe.
 * @param description Owning root recipe consumed once by prepare; creation/capture allocation may throw.
 * @param options Existing runtime policies and bounded pending-configuration budget.
 * @param origin Construction call site retained across lazy mount.
 * @return UI-affine lazy facade; neither mount nor terminal I/O occurs during construction.
 */
template <class T>
Scene scene(Description<T> &&description, SceneOptions options = {},
            std::source_location origin = std::source_location::current()) {
  return detail::SceneAccess::create(
    [description = std::move(description),
     controls = options.controls](ManagedScene &owner) mutable -> std::expected<void, ManagedError> {
      auto mounted = mount(owner, std::move(description), controls);

      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    },
    options, origin);
}

/** @tparam Factory Owning callable returning Description<T> or expected<Description<T>, ManagedError>.
 * @param factory Invoked once at prepare with ManagedScene&, optionally followed by Application&;
 * the one-argument form has priority. May create states/models and application-backed services.
 * Explicitly borrowed services must outlive their use; construction/capture allocation may throw.
 * Application remains UI-only and facade-owned. It supplies UI services; run this owner through Scene,
 * never
 * Application::run directly from factory, configuration or shutdown. Stop external work with onStop before managed
 * teardown, and release borrowed services before the facade is destroyed.
 * @param options Existing runtime policies and finite facade limits.
 * @param origin Factory registration call site retained in mount diagnostics.
 * @return UI-affine lazy facade; the factory does not execute during construction.
 */
template <detail::SceneFactory Factory>
Scene scene(Factory factory, SceneOptions options = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneAccess::create(
    [factory = std::move(factory),
     controls = options.controls](ManagedScene &owner) mutable -> std::expected<void, ManagedError> {
      auto effectiveControls = controls;
      auto installed = detail::ControlContextAccess::installFallbackSchema(effectiveControls);

      if (!installed) {
        return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
      }

      auto prepared = effectiveControls.prepared();
      if (!prepared) {
        return std::unexpected(detail::controlContextError(prepared.error(), owner.root()));
      }
      auto recipe = [&] {
        if constexpr (std::invocable<Factory &, ManagedScene &>) {
          return std::invoke(factory, owner);
        } else {
          return std::invoke(factory, owner, detail::SceneAccess::application(owner));
        }
      }();

      if constexpr (detail::IsDescription<decltype(recipe)>::value) {
        auto mounted = mount(owner, std::move(recipe), *prepared);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      } else {
        if (!recipe) {
          return std::unexpected(recipe.error());
        }

        auto mounted = mount(owner, std::move(*recipe), *prepared);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      }
    },
    options, origin);
}

/** @brief Declare scene layers without manually composing an OverlayHost.
 * @tparam Body Consuming body recipe, or the existing owning SceneFactory signature.
 * @tparam First First typed Dialog or erased resource layer; at least one layer is required.
 * @tparam Layers Further consuming layers of the same admitted kinds.
 * @param options Existing scene/runtime policies; ordinary scene(body,options,origin) remains unchanged.
 * @param origin Explicit construction site retained in mount diagnostics.
 * @param body Ordinary content, or its factory; an exact typed root OverlayHost is reused.
 * @param first Initially closed layer; erased roots must produce an exact bound Dialog.
 * @param layers Further initially closed layers, appended in declaration order.
 * @return Lazy owner of one existing host and one mount transaction. Failed composition publishes no refs.
 * Dialogs inside ordinary layout children are not hoisted. An erased body hiding a host is not a typed override.
 * Registered resource factory effects retain their existing contract and are not rolled back.
 */
template <class Body, detail::SceneLayer First, detail::SceneLayer... Layers>
  requires(detail::DescriptionInput<Body> || detail::SceneFactory<std::remove_cvref_t<Body>>)
Scene scene(SceneOptions options, std::source_location origin, Body &&body, First &&first, Layers &&...layers) {
  if constexpr (detail::DescriptionInput<Body>) {
    return scene(detail::layered(std::forward<Body>(body), std::forward<First>(first), std::forward<Layers>(layers)...),
                 options, origin);
  } else {
    return scene(
      [factory = std::forward<Body>(body),
       layers = std::tuple(std::forward<First>(first), std::forward<Layers>(layers)...)](
        ManagedScene &owner) mutable -> std::expected<Description<Widget>, ManagedError> {
        auto recipe = [&] {
          if constexpr (std::invocable<decltype(factory) &, ManagedScene &>) {
            return std::invoke(factory, owner);
          } else {
            return std::invoke(factory, owner, detail::SceneAccess::application(owner));
          }
        }();

        const auto compose = [&](auto &&content) {
          return std::apply(
            [&](auto &...layer) {
              return detail::layered(std::move(content), std::move(layer)...);
            },
            layers);
        };

        if constexpr (detail::IsDescription<decltype(recipe)>::value) {
          return compose(std::move(recipe));
        } else {
          if (!recipe) {
            return std::unexpected(recipe.error());
          }

          return compose(std::move(*recipe));
        }
      },
      options, origin);
  }
}

/** @brief Declare layers with explicit existing scene policies.
 * @tparam Body Consuming body recipe or existing SceneFactory.
 * @tparam First Consuming typed Dialog or erased recipe checked as an exact bound Dialog.
 * @tparam Layers Further consuming dialog layers.
 * @param options Existing scene/runtime policies.
 * @param body Ordinary content or its factory; a typed root OverlayHost is reused.
 * @param first First initially closed layer.
 * @param layers Further initially closed layers in declaration order.
 * @return The same lazy scene composition; use the explicit-origin overload for diagnostic attribution.
 */
template <class Body, detail::SceneLayer First, detail::SceneLayer... Layers>
  requires(detail::DescriptionInput<Body> || detail::SceneFactory<std::remove_cvref_t<Body>>)
Scene scene(SceneOptions options, Body &&body, First &&first, Layers &&...layers) {
  return scene(options, std::source_location::current(), std::forward<Body>(body), std::forward<First>(first),
               std::forward<Layers>(layers)...);
}

/** @brief Declare initially closed layers with the existing default scene policies.
 * @tparam Body Consuming body recipe or existing SceneFactory.
 * @tparam First Consuming typed Dialog or erased recipe checked as an exact bound Dialog.
 * @tparam Layers Further consuming dialog layers.
 * @param body Ordinary content or its factory; a typed root OverlayHost is reused.
 * @param first First initially closed layer.
 * @param layers Further initially closed layers in declaration order.
 * @return The same lazy scene composition; a scene without declared layers retains its original path.
 */
template <class Body, detail::SceneLayer First, detail::SceneLayer... Layers>
  requires(detail::DescriptionInput<Body> || detail::SceneFactory<std::remove_cvref_t<Body>>)
Scene scene(Body &&body, First &&first, Layers &&...layers) {
  return scene(SceneOptions{}, std::source_location::current(), std::forward<Body>(body), std::forward<First>(first),
               std::forward<Layers>(layers)...);
}

namespace detail {
/** @brief Owning minimal pipe adapter forwarding to the same facade method.
 * @tparam Action Callable accepting Scene&; no separate configuration implementation.
 */
template <class Action> struct SceneStage {
  Action action; ///< Owning method adapter; explicitly borrowed services remain borrowed.

  /** @param owner Stationary facade; no ownership move occurs.
   * @param stage Borrowed owning adapter.
   * @return The same owner reference after exactly one dispatch.
   */
  friend Scene &operator|(Scene &owner, SceneStage &stage) {
    stage.action(owner);

    return owner;
  }

  /** @param owner Stationary facade.
   * @param stage Temporary owning adapter.
   * @return The same owner reference after exactly one dispatch.
   */
  friend Scene &operator|(Scene &owner, SceneStage &&stage) {
    stage.action(owner);

    return owner;
  }

  /** @param input Consumed idle facade; move admission precedes action dispatch.
   * @param stage Borrowed owning adapter.
   * @return Owning facade preserving the existing early move guard.
   */
  friend Scene operator|(Scene &&input, SceneStage &stage) {
    Scene owner(std::move(input));

    stage.action(owner);

    return owner;
  }

  /** @param input Consumed idle facade; move admission precedes action dispatch.
   * @param stage Temporary owning adapter.
   * @return Owning facade preserving the existing early move guard.
   */
  friend Scene operator|(Scene &&input, SceneStage &&stage) {
    Scene owner(std::move(input));

    stage.action(owner);

    return owner;
  }
};

/** @brief Terminal pipe adapter; the borrowed transport is optional for native run. */
struct RunStage {
  ApplicationTerminal *terminal; ///< Borrowed transport, or null for the native run overload.
  std::stop_token stop;          ///< Existing runner cancellation.
  std::source_location origin;   ///< Pipe registration call site.

  /** @param owner Consumed idle facade.
   * @param stage Runner selection and call site.
   * @return Same terminal checked result as Scene::run, including explicit cleanup.
   */
  friend std::expected<ApplicationStats, ManagedError> operator|(Scene owner, RunStage stage) {
    return stage.terminal ? owner.run(*stage.terminal, stage.stop, stage.origin) : owner.run(stage.stop, stage.origin);
  }
};

/** @brief Final int stage over the same runApp boundary; transport remains borrowed. */
struct RunAppStage {
  ApplicationTerminal *terminal; ///< Borrowed transport, or null for native terminal ownership.
  std::stop_token stop;          ///< Optional worker-safe cancellation.
  std::source_location origin;   ///< Final stage call site.
  RunAppOptions options{};       ///< Upper factory policies; callable construction precedes consumption.

  /** @param owner Borrowed lvalue scene or live temporary; it is never moved by this stage.
   * @param stage Transport/cancellation selection.
   * @return Same zero/one status and final diagnostic delivery as Scene::runApp.
   */
  friend int operator|(Scene &owner, RunAppStage stage) {
    if (stage.options.report) {
      owner.reportOnError(std::move(stage.options.report), stage.origin);
    }

    return stage.terminal ? owner.runApp(*stage.terminal, stage.stop, stage.origin)
                          : owner.runApp(stage.stop, stage.origin);
  }

  /** @param owner Live temporary scene; its lifetime covers the entire run and reporting.
   * @param stage Final runner selection.
   * @return Same status as the lvalue overload, without relocating callback self-references.
   */
  friend int operator|(Scene &&owner, RunAppStage stage) {
    return owner | stage;
  }
};

/** @brief Lazy command-line preflight followed by the existing final runner boundary. */
struct CommandLineRunStage {
  int argc{};                      ///< Input argument count, including program name.
  const char *const *argv{};       ///< Borrowed input array, copied at consumption.
  CommandLineParser parser;        ///< Owning parser adapter; bound options remain caller-owned.
  ApplicationTerminal *terminal{}; ///< Optional borrowed custom transport.
  std::stop_token stop;            ///< Optional worker-safe cancellation.
  std::source_location origin;     ///< Final stage call site.
  RunAppOptions options{};         ///< Pre-factory policies; returned Scene policies remain its own.

  /** @param owner Existing UI-affine lazy scene.
   * @param stage Single-use command-line stage.
   * @return Zero for success/help, two for launch errors, one for runtime failure.
   */
  friend int operator|(Scene &owner, CommandLineRunStage stage) {
    if (stage.options.report) {
      owner.reportOnError(std::move(stage.options.report), stage.origin);
    }

    return stage.terminal
             ? owner.runApp(*stage.terminal, stage.argc, stage.argv, std::move(stage.parser), stage.stop, stage.origin)
             : owner.runApp(stage.argc, stage.argv, std::move(stage.parser), stage.stop, stage.origin);
  }

  /** @param owner Live temporary scene, kept in place throughout preflight and run.
   * @param stage Owning parser and borrowed transport selection.
   * @return Same status as the lvalue overload, without moving the scene.
   */
  friend int operator|(Scene &&owner, CommandLineRunStage stage) {
    return owner | std::move(stage);
  }
};

} // namespace detail

/** @param origin Preparation stage call site.
 * @return Owning stage forwarding to Scene::prepare without terminal I/O.
 */
inline auto prepare(std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[origin](Scene &owner) {
    owner.prepare(origin);
  }};
}

/** @param sheet Owning replacement sheet captured before entry; construction may throw.
 * @param origin Theme stage registration call site.
 * @return Consuming stage forwarding to Scene::theme.
 */
inline auto theme(StyleSheet sheet, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[sheet = std::move(sheet), origin](Scene &owner) mutable {
    detail::SceneAccess::theme(owner, sheet, origin);
  }};
}

/** @param options Aggregate logical layer budgets, including permanent base slots.
 * @param origin Configuration call site retained on failure.
 * @return Consuming opt-in stage over the existing managed style engine; no second runtime.
 */
inline auto styleLayers(StyleLayerOptions options = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[options, origin](Scene &owner) {
    owner.styleLayers(options, origin);
  }};
}

/** @param tier APPLICATION or USER override tier; enable styleLayers before this stage.
 * @param name Owning tier-unique name retained until removal or scene cleanup.
 * @param sheet Owning rules, independent of theme replacement.
 * @param origin Configuration call site retained on typed failure.
 * @return Consuming stage; the scene owns the layer without client token storage.
 * Constructing stage captures may throw before the checked facade entry.
 */
inline auto styleLayer(StyleTier tier, std::string name, StyleSheet sheet,
                       std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[tier, name = std::move(name), sheet = std::move(sheet), origin](Scene &owner) {
    detail::SceneAccess::styleLayer(owner, tier, name, sheet, origin);
  }};
}

/** @param observer Owning observer, never a veto; capture construction may throw.
 * @param origin Observer stage registration call site.
 * @return Consuming stage forwarding to Scene::onError.
 */
inline auto onError(ManagedErrorHandler observer, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[observer = std::move(observer), origin](Scene &owner) mutable {
    owner.onError(std::move(observer), origin);
  }};
}

/** @param sink Owning copyable callable receiving const SceneReport&, or empty for default stderr.
 * @param origin Reporting stage registration call site.
 * @return Consuming stage forwarding to Scene::reportOnError; independent of onError order.
 */
inline auto reportOnError(SceneReportHandler sink = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[sink = std::move(sink), origin](Scene &owner) mutable {
    owner.reportOnError(std::move(sink), origin);
  }};
}

/** @tparam Callback Owning void/expected<void, ManagedError> configuration.
 * @param callback Existing facade configuration action; capture construction may throw.
 * @param origin Stage registration call site.
 * @return Consuming stage forwarding to Scene::configure.
 */
template <detail::SceneConfiguration Callback>
auto configure(Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[callback = std::move(callback), origin](Scene &owner) mutable {
    detail::SceneAccess::configure(owner, callback, origin);
  }};
}

/** @tparam Callback Existing compact key action preserving typed command receipts.
 * @param chord Exact key/scalar/modifiers admitted for PRESS and REPEAT.
 * @param callback Owning action, optionally receiving KeyEvent; capture construction may throw.
 * @param origin Stage registration call site.
 * @return Consuming stage forwarding to Scene::onKey and its existing bounded composition.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeyEvent chord, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[chord, callback = std::move(callback), origin](Scene &owner) mutable {
    detail::SceneAccess::onKey(owner, chord, callback, origin);
  }};
}

/** @tparam Source String-view-convertible text, chord or owning Shortcut normalized during this call.
 * @param source
 * Borrowed input copied or moved into immutable owned alternatives before return.
 * @param actions Nonempty action
 * set; duplicates are idempotent, invalid values retain their index.
 * @param options Positive finite normalization
 * budgets.
 * @return Owning selection or deferred ShortcutError; onKey supplies the registration origin.
 * The action set applies independently to every sequence stroke without synthesizing phases.
 */
template <portals::detail::ShortcutSource Source>
KeySelection keyActions(Source &&source, std::initializer_list<KeyAction> actions, ShortcutOptions options = {}) {
  return portals::detail::KeySelectionAccess::make(
    portals::detail::prepareShortcut(std::forward<Source>(source), options), actions, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param actions Nonempty action set,
 * with duplicates admitted once.
 * @param options Positive finite normalization budgets.
 * @return Immutable owning
 * selection or deferred typed failure.
 */
inline KeySelection keyActions(std::initializer_list<key::Chord> source, std::initializer_list<KeyAction> actions,
                               ShortcutOptions options = {}) {
  return portals::detail::KeySelectionAccess::make(portals::detail::prepareShortcut(source, options), actions, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting PRESS only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source> KeySelection press(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::PRESS}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting PRESS only, or deferred typed failure.
 */
inline KeySelection press(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::PRESS}, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting REPEAT only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source> KeySelection repeat(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::REPEAT}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting REPEAT only, or deferred typed failure.
 */
inline KeySelection repeat(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::REPEAT}, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting RELEASE only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source> KeySelection release(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::RELEASE}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting RELEASE only, or deferred typed failure.
 */
inline KeySelection release(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::RELEASE}, options);
}

/** @tparam Source Text, typed chord or owning Shortcut accepted by checked normalization.
 * @param source Input
 * normalized and owned before this helper returns.
 * @param options Positive finite normalization budgets.
 * @return
 * Selection admitting PRESS or REPEAT only, or deferred typed failure.
 */
template <portals::detail::ShortcutSource Source>
KeySelection pressRepeat(Source &&source, ShortcutOptions options = {}) {
  return keyActions(std::forward<Source>(source), {KeyAction::PRESS, KeyAction::REPEAT}, options);
}

/** @param source Alternative chords copied before the temporary list expires.
 * @param options Positive finite
 * normalization budgets.
 * @return Selection admitting PRESS or REPEAT only, or deferred typed failure.
 */
inline KeySelection pressRepeat(std::initializer_list<key::Chord> source, ShortcutOptions options = {}) {
  return keyActions(source, {KeyAction::PRESS, KeyAction::REPEAT}, options);
}

/** @tparam Callback Existing owning compact key action, optionally taking const KeyEvent&.
 * @param selection
 * Immutable owning alternatives and exact selected native phases.
 * @param info Owning metadata resolved once after complete mount.
 * @param callback Action stored until checked stage
 * consumption; caller construction/moves may throw.
 * @param origin Registration call site, independent of helper
 * construction.
 * @return Consuming stage using the existing bounded newest-first registration.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeySelection selection, KeyInfo info, Callback callback,
           std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[selection = std::move(selection), info = std::move(info), callback = std::move(callback),
                             origin](Scene &owner) mutable {
    owner.onKey(std::move(selection), std::move(info), std::move(callback), origin);
  }};
}

/** @tparam Source Existing shortcut source. @tparam Callback Exact managed action.
 * @param source Owning normalization input. @param info Metadata registered once with the action.
 * @param callback Action. @param origin Original registration site. @return Consuming annotated stage. */
template <portals::detail::ShortcutSource Source, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(Source &&source, KeyInfo info, Callback callback,
           std::source_location origin = std::source_location::current()) {
  return onKey(press(std::forward<Source>(source)), std::move(info), std::move(callback), origin);
}

/** @tparam Callback Existing compact key action preserving old callback result forms.
 * @param selection Owning alternatives and exact native phases.
 * @param callback Owning action. @param origin Original registration site.
 * @return Consuming unannotated stage with existing routing behavior. */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(KeySelection selection, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{
    [selection = std::move(selection), callback = std::move(callback), origin](Scene &owner) mutable {
      detail::SceneAccess::onSelection(owner, selection, callback, origin);
    }};
}

/** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
 * @tparam Callback
 * Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Source normalized during stage
 * construction; caller text is never retained.
 * @param callback Action stored until checked stage consumption; caller
 * callback construction/moves may throw.
 * @param origin Registration site preserved across deferred configuration.
 *
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <portals::detail::ShortcutSource Source, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(Source &&source, Callback callback, std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), {}));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, {}, {}, origin);
    }};
}

/** @tparam Source Constexpr chord, owning alternatives or string-view-convertible runtime text.
 * @tparam Callback Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Source normalized during stage construction; caller text is never retained.
 * @param callback Action stored until checked stage consumption; caller callback construction/moves may throw.
 * @param behavior PRESS-only default; repeat requires explicit opt-in.
 * @param options Positive finite parser budgets.
 * @param origin Registration site preserved across deferred configuration.
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <portals::detail::ShortcutSource Source, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(Source &&source, Callback callback, ShortcutBehavior behavior, ShortcutOptions options = {},
           std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), options));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), behavior, options, origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, behavior, options, origin);
    }};
}

/** @tparam Callback Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Alternatives copied during stage construction.
 * @param callback Action stored until checked stage consumption; caller callback construction/moves may throw.
 * @param origin Registration site preserved across deferred configuration.
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(std::initializer_list<key::Chord> source, Callback callback,
           std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), {}));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, {}, {}, origin);
    }};
}

/** @tparam Callback Owning compact checked action optionally taking a semantic KeyEvent.
 * @param source Alternatives copied during stage construction.
 * @param callback Action stored until checked stage consumption; caller callback construction/moves may throw.
 * @param behavior PRESS-only default; repeat requires explicit opt-in.
 * @param options Positive finite parser budgets.
 * @param origin Registration site preserved across deferred configuration.
 * @return Consuming stage registering one atomic owning alternative set in the existing input slot.
 */
template <class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, KeyEvent>
auto onKey(std::initializer_list<key::Chord> source, Callback callback, ShortcutBehavior behavior,
           ShortcutOptions options = {}, std::source_location origin = std::source_location::current()) {
  auto shortcut =
    portals::detail::retainShortcut(portals::detail::prepareShortcut(std::forward<decltype(source)>(source), options));

  return detail::SceneStage{
    [shortcut = std::move(shortcut), callback = std::move(callback), behavior, options, origin](Scene &owner) mutable {
      detail::SceneAccess::onShortcut(owner, shortcut, callback, behavior, options, origin);
    }};
}

/** @tparam Callback Owning no-argument or ClickParams callback returning void, checked TreeError or editor receipt.
 * @param reference Button observation published by this scene's recipe; no scene/widget ownership is retained.
 * @param callback Owning callback invoked FIFO with other pipe callbacks, including modified activations.
 * @param origin Registration site retained through deferred mount and input diagnostics.
 * @return Owning pipe stage over one existing managed/router binding. stop/stopPropagation end the remaining
 * chain; the button already consumes ancestor propagation. Failure stops the chain without rollback.
 * Callback parameters are borrowed synchronously; copies retain an owning event snapshot only.
 * Runtime append affects the next event; a new binding during dispatch returns existing WOULD_BLOCK.
 * UI affinity, finite ManagedOptions limits and sticky facade failures apply. Capture construction may throw.
 */
template <class Callback = detail::QuitCommand>
  requires detail::ClickCallback<Callback>
auto onClick(Ref<Button> reference, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::ClickAccess::attach(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onClick(owner, registration, origin);
  }};
}

/** @tparam Callback Owning managed action taking TextEditedEvent or no arguments.
 * @param reference Editor published by this scene's recipe.
 * @param callback FIFO callback for actual accepted user byte changes, including Undo/Redo and clear.
 * @param origin Registration site retained through deferred mounting and input diagnostics.
 * @return Pipe stage over the existing editor binding, with no extra subscription or manual token.
 * Cursor/selection changes, fluent writes, model synchronization and failed edits without a delta do not notify.
 * The legacy result runs first; node/scene removal suppresses subsequent typed delivery. Callback errors stop
 * this event's remaining chain and use existing diagnostics. Appending during delivery affects the next event.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, TextEditedEvent>
auto onTextEdited(Ref<LineEdit> reference, Callback callback,
                  std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::EditorEventAccess::attach<TextEditedEvent>(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onEditing(owner, registration, origin);
  }};
}

/** @tparam Callback Owning managed action taking EditingFinishedEvent or no arguments.
 * @param reference Editor published by this scene's recipe.
 * @param callback FIFO observation of Enter or focus loss in every edit mode, including rejected attempts.
 * @param origin Registration site retained through deferred mounting and input diagnostics.
 * @return Pipe stage reusing the existing commit result; no second validator or model update runs.
 * Deferred/clean blur has no result receipt. Events own their values; the callback borrows them synchronously.
 * Legacy-result removal suppresses delivery; reentrant focus from that attempt does not duplicate completion.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, EditingFinishedEvent>
auto onEditingFinished(Ref<LineEdit> reference, Callback callback,
                       std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::EditorEventAccess::attach<EditingFinishedEvent>(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onEditing(owner, registration, origin);
  }};
}

/** @tparam T Observed widget type.
 * @tparam Callback Owning compact action accepting geometry or no arguments.
 * @param reference Ref published by this scene's description.
 * @param callback Void, checked TreeError or EditorCommandResult action retained by the scene.
 * @param origin Registration site retained in diagnostics.
 * @return Scene pipe stage registering deferred final local/surface/clip observations.
 * Callbacks run on UI outside tree frames. Equal bounds do not notify or request redraw.
 */
template <class T, class Callback>
  requires portals::detail::ManagedAction<Callback, NodeGeometry>
auto onLayout(Ref<T> reference, Callback callback, std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    return detail::LayoutAccess::attach(scene, reference, std::move(callback), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onLayout(owner, registration, origin);
  }};
}

namespace detail {
/** @param sink Optional upper callable copied before any user factory/parser executes.
 * @return Pinned owning sink, empty for default delivery, or exact allocation/copy failure.
 */
inline std::expected<std::shared_ptr<SceneReportHandler>, ManagedError> pinAppSink(const SceneReportHandler &sink) {
  try {
    return sink ? std::make_shared<SceneReportHandler>(sink) : nullptr;
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED, {}, {}});
  }
}

/** @tparam Factory Owning-result factory admitted by the public upper boundary.
 * @tparam Setup Stationary setup callback, returning void or exact managed expected.
 * @param factory Synchronously invoked factory.
 * @param setup Callback running against one stationary facade.
 * @param options Borrowed Description construction and runner policies.
 * @param finalSink Optional owning upper sink, already pinned before user code executes.
 * @param origin Upper invocation site.
 * @return Existing zero/one runtime status with final diagnostics after cleanup.
 */
template <AppFactory Factory, AppSetup Setup>
int runAppFactory(Factory &&factory, Setup &&setup, const RunAppOptions &options,
                  std::shared_ptr<SceneReportHandler> finalSink, std::source_location origin) {
  FactoryCleanupScope factoryMask;
  SetupFailureScope invocationScope({}, nullptr, nullptr);
  CurrentSceneScope currentMask;
  SceneReportHandler fallbackSink;
  FactoryCleanup cleanup;

  try {
    auto description = [&] {
      FactoryCleanupScope factoryScope(&cleanup);

      return std::invoke(factory);
    }();

    using Result = std::remove_cvref_t<decltype(description)>;
    constexpr bool DIRECT_RESULT = IsDescription<Result>::value || std::same_as<Result, Scene>;

    auto create = [&]() {
      if constexpr (std::same_as<Result, Scene>) {
        return std::move(description);
      } else if constexpr (std::same_as<Result, std::expected<Scene, ManagedError>>) {
        return std::move(*description);
      } else if constexpr (IsDescription<Result>::value) {
        return scene(std::move(description), options.scene, origin);
      } else {
        return scene(std::move(*description), options.scene, origin);
      }
    };

    if constexpr (!DIRECT_RESULT) {
      if (!description) {
        reportFactoryFailure(finalSink ? *finalSink : fallbackSink, description.error(), origin, cleanup);

        return 1;
      }
    }

    // This owner stays stationary during setup and run; callbacks observe stable State through SceneRef.
    auto owner = create();
    auto admitted = SceneAccess::factoryResultChecked(owner);

    if (!admitted) {
      auto failure = admitted.error();
      const auto closed = owner.close(origin);

      if (!closed && !failure.cleanup && closed.error().cleanup) {
        failure.cleanup = closed.error().cleanup;
      }

      const auto report = factoryFailureReport(std::move(failure), origin, cleanup);

      if (!SceneAccess::deliverFactoryReport(owner, report, finalSink)) {
        deliverFactoryReport(finalSink ? *finalSink : fallbackSink, report);
      }

      return 1;
    }

    const auto droppedErrors = SceneAccess::applyFactoryCleanup(owner, cleanup, origin);

    if (owner.checked()) {
      std::optional<ManagedError> setupFailure;

      {
        auto setupScope = SceneAccess::setupScope(owner);
        auto currentScope = SceneAccess::currentScope(owner);

        try {
          if constexpr (std::same_as<std::invoke_result_t<Setup &, Scene &>, void>) {
            std::invoke(setup, owner);
          } else {
            auto configured = std::invoke(setup, owner);

            if (!configured) {
              setupFailure = configured.error();
            }
          }
        } catch (const std::bad_alloc &) {
          setupFailure = ManagedError{ManagedPhase::BIND, {}, TreeError::NO_MEMORY, {}, {}};
        } catch (...) {
          setupFailure = ManagedError{ManagedPhase::BIND, {}, TreeError::CALLBACK_FAILED, {}, {}};
        }
      }

      // Finalize the short-error scope before cleanup can revoke its weak delivery publication.
      if (setupFailure) {
        SceneAccess::setupFailure(owner, *setupFailure, origin);
      }
    }

    return SceneAccess::runApp(owner, options, std::move(finalSink), origin, droppedErrors);
  } catch (const std::bad_alloc &) {
    reportFactoryFailure(finalSink ? *finalSink : fallbackSink,
                         ManagedError{ManagedPhase::MOUNT, {}, TreeError::NO_MEMORY, {}, {}}, origin, cleanup);
  } catch (...) {
    reportFactoryFailure(finalSink ? *finalSink : fallbackSink,
                         ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED, {}, {}}, origin, cleanup);
  }

  return 1;
}
} // namespace detail

/** @tparam Factory No-argument callable returning owning Scene, Description<T>, or their exact expected with
 * ManagedError.
 * @tparam Setup Callable taking the stationary Scene&, returning void or exact expected<void,ManagedError>.
 * @param factory Invoked inside the checked construction boundary, after final sink availability.
 * @param setup Invoked on one stationary facade; capture SceneRef for later callbacks across facade moves.
 * @param options Description construction policies, borrowed terminal, upper final sink and cancellation.
 * A returned Scene retains its own policies and final sink unless options.report is nonempty.
 * The explicit sink is pinned before invoking the factory; if pinning fails, stderr receives the typed failure.
 * @param origin Upper runner invocation site retained in typed failures.
 * @return Zero after successful run, one for factory/construction/setup/run failure after cleanup.
 * Callable and argument construction before actual invocation can throw outside this boundary.
 * Factory/setup exceptions map to NO_MEMORY or CALLBACK_FAILED. A factory returning Scene must retain
 * callback data by ownership or SceneRef; references to its local facade do not survive the return move.
 * Cold and prepared, unrun scenes are admitted; completed, closed and moved-from results fail before terminal access.
 * Abandoned local cleanup retains its first typed error; further failures increment report.droppedErrors.
 * A rejected short mutation inside setup latches its original typed cause before observers; the current void
 * callback finishes, while later setup/run is skipped. Unbound targets can be attributed to this entered scope.
 */
template <detail::AppFactory Factory, detail::AppSetup Setup>
int runApp(Factory &&factory, Setup &&setup, const RunAppOptions &options = {},
           std::source_location origin = std::source_location::current()) {
  detail::FactoryCleanupScope factoryMask;
  detail::SetupFailureScope invocationScope({}, nullptr, nullptr);
  detail::CurrentSceneScope currentMask;
  auto sink = detail::pinAppSink(options.report);

  if (!sink) {
    detail::reportFactoryFailure({}, sink.error(), origin);

    return 1;
  }

  return detail::runAppFactory(std::forward<Factory>(factory), std::forward<Setup>(setup), options, std::move(*sink),
                               origin);
}

/** @tparam Factory No-argument factory returning owning Scene, Description<T>, or their exact managed expected.
 * @param factory Invoked inside the checked upper construction boundary.
 * @param options Existing runtime policies and final sink, available before factory invocation.
 * @param origin Upper invocation site.
 * @return Same zero/one status and cleanup as the stationary setup overload with no setup action.
 */
template <detail::AppFactory Factory>
int runApp(Factory &&factory, const RunAppOptions &options = {},
           std::source_location origin = std::source_location::current()) {
  return runApp(
    std::forward<Factory>(factory),
    [](Scene &) {
    },
    options, origin);
}

/** @tparam Factory No-argument owning Scene/Description factory, optionally returning exact managed expected.
 * @param factory Invoked once only after successful command-line preflight; captures must outlive this call.
 * @param argc Input argument count, including the program name.
 * @param argv Borrowed argument array, copied before parser invocation.
 * @param parser Owning parser, retired before factory invocation, including on help/failure.
 * @param options Description policies, optional explicit final sink, borrowed terminal and cancellation.
 * A returned Scene keeps its policies and configured sink unless the upper sink is nonempty.
 * @param origin Upper launch call site retained in diagnostics.
 * @return Zero on success/help, two on command-line failure, one on factory/runtime failure after cleanup.
 * Callable/argument construction before invocation remains outside this checked boundary.
 */
template <detail::AppFactory Factory>
int runApp(Factory &&factory, int argc, const char *const *argv, CommandLineParser parser,
           const RunAppOptions &options = {}, std::source_location origin = std::source_location::current()) {
  detail::FactoryCleanupScope factoryMask;
  detail::SetupFailureScope invocationScope({}, nullptr, nullptr);
  detail::CurrentSceneScope currentMask;
  auto sink = detail::pinAppSink(options.report);

  if (!sink) {
    parser = nullptr;
    detail::reportFactoryFailure({}, sink.error(), origin);

    return 1;
  }

  auto action = detail::commandLinePreflight(argc, argv, parser);

  if (!action) {
    SceneReport report;
    SceneReportHandler fallbackSink;

    report.origin = origin;
    report.commandLineFailure = std::move(action.error());
    detail::deliverFactoryReport(*sink ? **sink : fallbackSink, report);

    return 2;
  }

  if (action->disposition == LaunchDisposition::FINISH) {
    if (!action->output.empty()) {
      std::fwrite(action->output.data(), 1, action->output.size(), stdout);
    }

    return 0;
  }

  return detail::runAppFactory(
    std::forward<Factory>(factory),
    [](Scene &) {
    },
    options, std::move(*sink), origin);
}

namespace detail {
/** @tparam Factory Owning-result factory borrowed through synchronous stage consumption.
 * @param factory Callable evaluated inside the existing checked upper boundary.
 * @param stage Upper policies, final sink and optional borrowed transport.
 * @return Same zero/one result as the function form.
 */
template <AppFactory Factory> int operator|(Factory &&factory, RunAppStage stage) {
  stage.options.terminal = stage.terminal;
  stage.options.stop = stage.stop;

  return ui::runApp(std::forward<Factory>(factory), stage.options, stage.origin);
}

/** @tparam Factory Owning-result factory borrowed through synchronous command-line launch.
 * @param factory Callable evaluated only after successful preflight.
 * @param stage Owning parser, upper sink and optional borrowed transport.
 * @return Same zero/two/one result as the command-line function form.
 */
template <AppFactory Factory> int operator|(Factory &&factory, CommandLineRunStage stage) {
  stage.options.terminal = stage.terminal;
  stage.options.stop = stage.stop;

  return ui::runApp(std::forward<Factory>(factory), stage.argc, stage.argv, std::move(stage.parser), stage.options,
                    stage.origin);
}
} // namespace detail

/** @param stop Optional existing runner cancellation.
 * @param origin Stage registration call site.
 * @return Terminal stage opening the native terminal only when consumed.
 */
inline auto run(std::stop_token stop = {}, std::source_location origin = std::source_location::current()) {
  return detail::RunStage{nullptr, stop, origin};
}

/** @param terminal Borrowed existing transport, outliving stage consumption.
 * @param stop Optional cancellation.
 * @param origin Stage registration call site.
 * @return Terminal checked stage using the same Scene::run implementation.
 */
inline auto run(ApplicationTerminal &terminal, std::stop_token stop = {},
                std::source_location origin = std::source_location::current()) {
  return detail::RunStage{&terminal, stop, origin};
}

/** @param stop Optional existing runner cancellation.
 * @param origin Final stage registration call site.
 * @return Native final stage, equivalent to ui::runApp(scene), returning zero or one.
 */
inline auto runApp(std::stop_token stop = {}, std::source_location origin = std::source_location::current()) {
  return detail::RunAppStage{nullptr, stop, origin};
}

/** @param terminal Borrowed existing custom/replay transport, outliving consumption.
 * @param stop Optional cancellation.
 * @param origin Final stage registration call site.
 * @return Same final reporting stage with custom transport; construction before evaluation is outside its boundary.
 */
inline auto runApp(ApplicationTerminal &terminal, std::stop_token stop = {},
                   std::source_location origin = std::source_location::current()) {
  return detail::RunAppStage{&terminal, stop, origin};
}

/** @tparam AppOptions Exact RunAppOptions type; deduction keeps legacy braced stop-token calls unambiguous.
 * @param options Upper factory policies and explicit final sink, copied into the consuming stage.
 * The optional terminal remains borrowed through synchronous consumption; Scene policies remain its own.
 * @param origin Final stage registration site.
 * @return Checked final stage accepting an owning-result factory or an existing Scene.
 */
template <class AppOptions>
  requires std::same_as<std::remove_cvref_t<AppOptions>, RunAppOptions>
inline auto runApp(AppOptions &&options, std::source_location origin = std::source_location::current()) {
  return detail::RunAppStage{options.terminal, options.stop, origin, options};
}

/** @param callback Owning settings factory, evaluated after CLI success and before Application creation.
 * @param origin Registration call site.
 * @return Lazy application policy modifier; borrowed option storage must outlive run.
 */
inline auto applicationOptions(std::function<ApplicationOptions()> callback,
                               std::source_location origin = std::source_location::current()) {
  return detail::SceneStage{[callback = std::move(callback), origin](Scene &owner) mutable {
    owner.applicationOptions(std::move(callback), origin);
  }};
}

/** @param argc Input argument count, including program name.
 * @param argv Borrowed input strings, copied when the final stage is consumed.
 * @param parser Owning parser adapter, invoked before mount or terminal acquisition.
 * @param origin Final stage call site.
 * @return Single-use launch stage; help/version and failures never open a terminal.
 */
inline auto runApp(int argc, const char *const *argv, CommandLineParser parser,
                   std::source_location origin = std::source_location::current()) {
  return detail::CommandLineRunStage{argc, argv, std::move(parser), nullptr, {}, origin};
}

/** @param terminal Borrowed custom transport, untouched on preflight help/failure.
 * @param argc Input argument count.
 * @param argv Borrowed input array.
 * @param parser Owning generic command-line adapter.
 * @param origin Final stage call site.
 * @return The same checked launch stage with an explicit test/embedding transport.
 */
inline auto runApp(ApplicationTerminal &terminal, int argc, const char *const *argv, CommandLineParser parser,
                   std::source_location origin = std::source_location::current()) {
  return detail::CommandLineRunStage{argc, argv, std::move(parser), &terminal, {}, origin};
}

/** @tparam AppOptions Exact RunAppOptions type; braced legacy origin arguments do not deduce this overload.
 * @param argc Input argument count, including the program name.
 * @param argv Borrowed input array copied at consumption.
 * @param parser Owning parser retired before invoking a factory or opening a terminal.
 * @param options Upper factory policies, explicit final sink, borrowed terminal and cancellation.
 * @param origin Final launch stage registration site.
 * @return Command-line stage with the same zero/two/one result as the function form.
 */
template <class AppOptions>
  requires std::same_as<std::remove_cvref_t<AppOptions>, RunAppOptions>
inline auto runApp(int argc, const char *const *argv, CommandLineParser parser, AppOptions &&options,
                   std::source_location origin = std::source_location::current()) {
  return detail::CommandLineRunStage{argc, argv, std::move(parser), options.terminal, options.stop, origin, options};
}
} // namespace portals::ui

#include <portals/ui_scene_resource.hpp>
