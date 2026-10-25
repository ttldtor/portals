// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/control_context.hpp>
#include <portals/detail/line_edit_text.hpp>
#include <portals/detail/move_only_function.hpp>
#include <portals/grid.hpp>
#include <portals/ui_ref.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <ranges>
#include <tuple>

namespace portals::ui {
/** @brief Single-use owning construction recipe; no mounted widget ownership.
 * @tparam T Public Widget subtype constructed by the recipe, including final types.
 */
template <class T> class Description;
class GridSection;

namespace detail {
struct ResourceAccess;

/** @brief Recognize an owning recipe after cv/ref normalization.
 * @tparam T Type tested for an exact Description
 * specialization.
 */
template <class T> struct IsDescription : std::false_type {};

/** @brief Recognize the owning recipe specialization.
 * @tparam T Widget type described by this recipe.
 */
template <class T> struct IsDescription<Description<T>> : std::true_type {};

/** @brief Recognize exact provider-bound list recipes, excluding arbitrary derived controls.
 * @tparam T Concrete widget type under test.
 */
template <class T> struct IsVirtualList : std::false_type {};

/** @brief Identify a native list specialization. @tparam T Immutable model row type. */
template <class T> struct IsVirtualList<VirtualList<T>> : std::true_type {};

/** @brief Recognize exact provider-bound table recipes, excluding arbitrary derived controls.
 * @tparam T Concrete widget type under test.
 */
template <class T> struct IsVirtualTable : std::false_type {};

/** @brief Identify a native table specialization. @tparam T Immutable model row type. */
template <class T> struct IsVirtualTable<VirtualTable<T>> : std::true_type {};

/** @brief Recognize exact native chart recipes without admitting arbitrary subclasses.
 * @tparam T Concrete widget type under test.
 */
template <class T> struct IsChart : std::false_type {};

/** @brief Identify a native chart specialization. @tparam T Immutable model row type. */
template <class T> struct IsChart<Chart<T>> : std::true_type {};

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

/** @brief Owning class names with fallible copies instead of checked-STL container moves. */
struct ClassNames {
  /** @brief Suppress implicit noexcept moves of checked containers. */
  ~ClassNames() = default;

  std::vector<std::string> values = std::vector<std::string>(0); ///< Ordered owning names, including duplicates.
};

/** @brief Copy class names using only the six ASCII whitespace separators.
 * @param value Borrowed list copied now; order and duplicate names are preserved.
 * @return Owning names; an empty or whitespace-only list clears classes. Allocation may throw.
 */
inline ClassNames splitClasses(std::string_view value) {
  ClassNames result;
  constexpr std::string_view SEPARATORS = " \t\r\n\f\v";
  auto begin = value.find_first_not_of(SEPARATORS);

  while (begin != std::string_view::npos) {
    const auto end = value.find_first_of(SEPARATORS, begin);

    result.values.emplace_back(value.substr(begin, end == std::string_view::npos ? end : end - begin));
    begin = end == std::string_view::npos ? end : value.find_first_not_of(SEPARATORS, end);
  }

  return result;
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

/** @param name Compact checked preset name; source bytes are borrowed only during this call.
 * @return Known built-in pattern, or no match; no allocation or text decoding occurs.
 */
constexpr std::optional<FramePattern> framePattern(std::string_view name) noexcept {
  if (name == "single" || name == "plain") {
    return FramePattern::SINGLE;
  }

  if (name == "rounded") {
    return FramePattern::ROUNDED;
  }

  if (name == "double") {
    return FramePattern::DOUBLE;
  }

  if (name == "heavy") {
    return FramePattern::HEAVY;
  }

  if (name == "ascii") {
    return FramePattern::ASCII;
  }

  return {};
}

/** @param name Borrowed compact skin name. @return Preset or invalid value checked at mount. */
constexpr ScrollbarPattern scrollbarPattern(std::string_view name) noexcept {
  if (name == "legacy")
    return ScrollbarPattern::LEGACY;
  if (name == "frame")
    return ScrollbarPattern::FRAME;
  if (name == "minimal")
    return ScrollbarPattern::MINIMAL;
  if (name == "ascii")
    return ScrollbarPattern::ASCII;

  return static_cast<ScrollbarPattern>(-1);
}

/** @param name Compact decoration preset. @return Known value, or an invalid value checked at mount. */
constexpr TitleDecoration titleDecorationPreset(std::string_view name) noexcept {
  if (name == "none")
    return TitleDecoration::NONE;
  if (name == "spaced")
    return TitleDecoration::SPACED;
  if (name == "square")
    return TitleDecoration::SQUARE;
  if (name == "angle")
    return TitleDecoration::ANGLE;
  if (name == "round")
    return TitleDecoration::ROUND;

  return static_cast<TitleDecoration>(-1);
}

/** @param frame Sparse frame leaves, borrowed only for the check.
 * @return Whether decoration was requested; no allocation or traversal of unrelated settings.
 */
inline bool hasFrameSettings(const ControlScrollFrameSettings &frame) noexcept {
  const auto &sides = frame.sides;
  const auto &glyphs = frame.customGlyphs;

  return frame.enabled || frame.glyphs || frame.maxPaintCells || frame.pattern || sides.top || sides.right ||
         sides.bottom || sides.left || frame.title || frame.titleAlignment || frame.maxTitleBytes || glyphs.enabled ||
         glyphs.topLeft || glyphs.topRight || glyphs.bottomLeft || glyphs.bottomRight || glyphs.horizontal ||
         glyphs.vertical || frame.titleDecoration || frame.customTitleDecoration.enabled ||
         frame.customTitleDecoration.prefix || frame.customTitleDecoration.suffix;
}

/** @param code Frame validation category. @param path Owning diagnostic path to copy.
 * @param node Existing parent used for mount attribution.
 * @return Checked frame failure; allocation failure preserves a typed NO_MEMORY fallback.
 */
inline ManagedError frameContextError(ControlContextErrorCode code, std::string_view path, Node<Widget> node = {}) {
  try {
    auto diagnostic = std::make_shared<ControlContextDiagnostic>();

    diagnostic->path = path;

    return controlContextError({code, std::move(diagnostic)}, node);
  } catch (const std::bad_alloc &) {
    return error(node, TreeError::NO_MEMORY);
  }
}

/** @brief Grid-only sparse configuration, released with its construction transaction. */
struct GridRecipeControl {
  ControlSettings arguments;                      ///< Positional grid literals retained for fallback-schema resolution.
  ControlSettings patch;                          ///< Sparse instance overrides applied after positional literals.
  std::optional<std::string> variant;             ///< Optional named fallback type checked during preflight.
  std::unique_ptr<ResolvedControl> resolved;      ///< Owning settings resolved before control construction.
  bool prepared{};                                ///< Settings were already resolved and validated.
  bool materialized{};                            ///< Resource properties already supplied materialized grid settings.
  bool settingsRequested{};                       ///< Explicit settings or variant modifiers were requested.
  ControlPropertyPresence materializedSettings{}; ///< Resource fields already counted against the mount budget.
};

/** @brief Cold layout values only; released with the single-use construction transaction. */
struct RecipeLayout {
  GridItemOptions item;   ///< Parent-child placement, never inherited from control defaults.
  bool itemRequested{};   ///< Distinguish ordinary defaults from an explicitly requested relationship.
  bool gridContainer{};   ///< True only for the standard grid recipe.
  bool validated{};       ///< This cold graph has completed its parent-aware preflight.
  bool validRecipe{true}; ///< An empty consuming child fails before any mount effects.
  bool hasDeclaredGrid{}; ///< Cold composition contains an explicitly configured standard Grid.
  bool needsChoiceHost{}; ///< A choice popup needs the existing scene-level overlay owner.
  bool hasOverlayHost{};  ///< A declared overlay owner already exists in this cold graph.
  std::function<std::expected<std::pair<bool, bool>, ManagedError>(ControlContext)>
    choiceProbe; ///< Owning cold metadata probe; never mounts nodes or invokes application factories.
  std::optional<DeclarativeGridOptions> grid; ///< Present only after declarative opt-in.
  std::unique_ptr<GridPlacementPlan> plan;    ///< Origins established before any constructor or setup.
  std::vector<std::shared_ptr<RecipeLayout>> children =
    std::vector<std::shared_ptr<RecipeLayout>>(0);      ///< Cold child metadata in insertion order.
  std::vector<NodeId> mounted = std::vector<NodeId>(0); ///< Full identities collected during child mount.
  std::size_t headerSections{};                         ///< Number of structural header sections.
  std::size_t bodySections{};                           ///< Number of structural body sections.
  std::size_t directItems{};                            ///< Number of children outside structural sections.
  bool stickyHeader{};                                  ///< Header sticky setting copied from its section.
  bool validSections{true};                             ///< Whether section placement modifiers are valid.
  GridPresentation presentation;                        ///< Owning grid background and paint-budget settings.
  std::optional<CellStyle> headerStyle;                 ///< Optional header-section background.
  std::optional<CellStyle> bodyStyle;                   ///< Optional body-section background.
  std::optional<GridRowStyles> rowStyles;               ///< Optional alternating body-row backgrounds.
  std::optional<GridScrollOptions> scroll;              ///< Optional owning scroll settings.
  std::unique_ptr<GridRecipeControl> control;           ///< Deferred grid control settings.
  std::unique_ptr<std::vector<AxisSize>> columnSizes;   ///< Cold owning shared-column policies, absent by default.
  std::unique_ptr<std::vector<AxisSize>> rowSizes;      ///< Cold owning body-row policies; header stays independent.
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

/** @brief Finite native composite capability shared only across cold recipe erasure, never mounted records. */
struct SemanticConstruction {
  ~SemanticConstruction() = default;

  ControlFactoryKind kind{
    ControlFactoryKind::MESSAGE_BINDING};    ///< Actual native producer, independent of C++ widget type.
  ControlSettings source;                    ///< Owning effective resource inputs for the actual native producer.
  ControlPropertyPresence materialized{};    ///< Leaves already reserved by resource preparation.
  bool prepared{};                           ///< Whether the source packet has been installed before construction.
  std::shared_ptr<Description<Widget>> body; ///< Optional owning resource body, consumed once by native construction.
};

/** @brief Stack-owned transaction bookkeeping, discarded after one description mount. */
struct MountContext {
  ControlContext controls{};   ///< Immutable owning defaults for this mount transaction.
  std::size_t controlLeaves{}; ///< Effective settings counted across the complete transaction.
  bool controlPrepared{};      ///< Only the current constructor received already resolved resource settings.
  const TextResolver
    *themeSelectorResolver{};               ///< Constructor-only selector fallback, cleared before internal children.
  const TextResolver *messageResolver{};    ///< Opted-in MessageBox constructor-only fallback; cleared before children.
  const TextResolver *dataStateResolver{};  ///< Native fallback constructor-only borrow; never ambient to body/setup.
  const ControlSettings *controlSettings{}; ///< Constructor-only borrowed settings; restored before setup/children.
  Description<Widget> *messageBody{};       ///< Constructor-scoped owning cold source, never retained after mount.
  const ControlSettings *frameSettings{};   ///< Current source frame patch only; never inherited by children.
  const FrameOptions *frameOptions{}; ///< Current native ScrollArea constructor only; never inherited by children.
  Node<Widget> mountedOuter;          ///< Last synchronous mount's actual layout root, including through type erasure.
  Node<Widget> root;                  ///< First created node; identifies the subtree to retire on failure.
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

} // namespace detail

} // namespace portals::ui
