// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/ui_event_access.hpp>
#include <portals/detail/ui_recipe_access.hpp>
#include <portals/ui_key_info.hpp>

namespace portals::ui {
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

  /** @return Consuming checkbox recipe selecting THREE_STATE without changing its model type.
   * Explicit bool models reject this setting at mount with INVALID_ARGUMENT.
   */
  Description &&threeState() &&
    requires std::same_as<T, CheckBox>
  {
    return std::move(*this).scalarSettings({.cycle = CheckCycle::THREE_STATE});
  }

  /** @param value Button decoration, checked before caption resolution or construction.
   * @return Consuming standard Button recipe with a sparse final override.
   */
  Description &&chrome(ButtonChrome value) &&
    requires std::same_as<T, Button>
  {
    return std::move(*this).scalarSettings({.chrome = value});
  }

  /** @param value Border/Gauge/scroll one-cell decoration mode.
   * @return Consuming native primitive recipe; absent settings remain unchanged.
   */
  Description &&glyphs(PrimitiveGlyphs value) &&
    requires(std::same_as<T, Border> || std::same_as<T, Gauge> || std::same_as<T, Spinner> ||
             std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.glyphs = value});
  }

  /** @param cells Positive allocated paint-work cap, checked before construction.
   * @return Consuming native Border/Gauge/scroll recipe with a sparse final override.
   */
  Description &&maxPaintCells(std::size_t cells) &&
    requires(std::same_as<T, Border> || std::same_as<T, Gauge> || std::same_as<T, Spinner> ||
             std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.maxPaintCells = cells});
  }

  /** @param value Finite initial value in the effective inclusive Gauge range.
   * @return Consuming native Gauge recipe with a sparse value override.
   */
  Description &&value(double value) &&
    requires std::same_as<T, Gauge>
  {
    return std::move(*this).scalarSettings({.value = value});
  }

  /** @param value Finite inclusive lower bound; validated with the receiving options and value.
   * @return Consuming native Gauge recipe with a sparse lower-bound override.
   */
  Description &&minimum(double value) &&
    requires std::same_as<T, Gauge>
  {
    return std::move(*this).scalarSettings({.minimum = value});
  }

  /** @param value Finite upper bound greater than the effective minimum, with a finite span.
   * @return Consuming native Gauge recipe with a sparse upper-bound override.
   */
  Description &&maximum(double value) &&
    requires std::same_as<T, Gauge>
  {
    return std::move(*this).scalarSettings({.maximum = value});
  }

  /** @param minimum Finite inclusive lower bound.
   * @param maximum Finite inclusive upper bound, greater than minimum with a finite span.
   * @return Consuming native Gauge recipe staging both bounds together.
   */
  Description &&range(double minimum, double maximum) &&
    requires std::same_as<T, Gauge>
  {
    return std::move(*this).scalarSettings({.minimum = minimum, .maximum = maximum});
  }

  /** @param cells Nonnegative Gauge intrinsic width, independent of fixed width policy.
   * @return Consuming native Gauge recipe with a sparse preferred-width override.
   */
  Description &&preferredWidth(std::int32_t cells) &&
    requires std::same_as<T, Gauge>
  {
    return std::move(*this).scalarSettings({.preferredWidth = cells});
  }

  /** @param cells Positive finite logical cells per wheel event.
   * @return Consuming native area/bar recipe with a sparse final input setting.
   */
  Description &&wheelStep(double cells) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.wheelStep = cells});
  }

  /** @param cells Positive exact 64-bit logical cells per arrow event.
   * @return Consuming native area/bar recipe retaining the full logical step width.
   */
  Description &&keyStep(std::uint64_t cells) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.keyStep = cells});
  }

  /** @param cells Exact 64-bit overlap between pages; zero and values above viewport are supported.
   * @return Consuming native area/bar recipe with a sparse overlap setting.
   */
  Description &&pageOverlap(std::uint64_t cells) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.pageOverlap = cells});
  }

  /** @param enabled Reserve functional end buttons on channels of at least three cells.
   * @return Consuming native scroll recipe; managed scenes automatically supply held-arrow services.
   */
  Description &&arrows(bool enabled = true) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.arrows = enabled});
  }

  /** @param cells Positive minimum thumb length, capped by the actual track.
   * @return Consuming native area/bar recipe with a checked cell setting.
   */
  Description &&minimumThumb(std::int32_t cells) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.minimumThumb = cells});
  }

  /** @param value Whether the owning area/standalone bar joins focus traversal.
   * @return Consuming native area/bar recipe; embedded bars still create no extra focus stop.
   */
  Description &&focusable(bool value = true) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.focusable = value});
  }

  /** @param value Block user mutation while preserving existing programmatic editor/scroll commands.
   * @return Consuming LineEdit or native area/bar recipe using its existing read-only policy.
   */
  Description &&readOnly(bool value = true) &&
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password> || std::same_as<T, ScrollArea> ||
             std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.readOnly = value});
  }

  /** @param enabled Allow one-edit prefix matching from three normalized query graphemes; false selects PREFIX.
   * @return Consuming choice recipe; only the strategy leaf changes, retaining configured search budgets.
   */
  Description &&fuzzy(bool enabled = true) &&
    requires std::derived_from<T, ComboBox>
  {
    return std::move(*this).scalarSettings(
      {.searchStrategy = enabled ? SearchStrategy::FUZZY_PREFIX : SearchStrategy::PREFIX});
  }

  /** @return Consuming choice recipe using exact normalized label/token prefixes and retaining search budgets. */
  Description &&prefix() &&
    requires std::derived_from<T, ComboBox>
  {
    return std::move(*this).fuzzy(false);
  }

  /** @param enabled Hide all value glyphs and use a fixed-origin caret; false restores the type's normal presentation.
   * @return Consuming editor recipe; ordinary LineEdit storage remains plaintext.
   */
  Description &&noEcho(bool enabled = true) &&
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password>)
  {
    const auto mode = enabled                     ? LineEditDisplayMode::NO_ECHO
                      : std::same_as<T, Password> ? LineEditDisplayMode::PASSWORD
                                                  : LineEditDisplayMode::NORMAL;

    return std::move(*this).scalarSettings({.displayMode = mode});
  }

  /** @param value Publication mode shared by input and fluent changes. @return Consuming editor recipe. */
  Description &&editMode(EditMode value) &&
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password>)
  {
    return std::move(*this).scalarSettings({.editMode = value});
  }

  /** @param value Public placeholder expression; never used as protected model data. @return Consuming editor recipe.
   */
  Description &&placeholder(TextValue value) &&
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password>)
  {
    return std::move(*this).scalarSettings({.placeholder = std::move(value)});
  }

  /** @param value Public placeholder bytes copied now. @return Consuming editor recipe. */
  Description &&placeholder(std::string_view value) &&
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password>)
  {
    return std::move(*this).placeholder(TextValue(value));
  }

  /** @param policy Horizontal bar presence policy.
   * @return Consuming native area recipe with a sparse horizontal policy.
   */
  Description &&horizontal(ScrollBarPolicy policy) &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).scalarSettings({.horizontal = policy});
  }

  /** @param policy Vertical bar presence policy.
   * @return Consuming native area recipe with a sparse vertical policy.
   */
  Description &&vertical(ScrollBarPolicy policy) &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).scalarSettings({.vertical = policy});
  }

  /** @param placement Existing gutter/overlay/frame placement; ON_FRAME needs an effective frame.
   * @return Consuming native area recipe validated against actual constructor options.
   */
  Description &&placement(ScrollPlacement placement) &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).scalarSettings({.placement = placement});
  }

  /** @return Consuming native area recipe placing scrollbars on its existing frame.
   * An absent effective frame remains a typed mount error, as for placement(ON_FRAME).
   */
  Description &&scrollOnFrame() &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).placement(ScrollPlacement::ON_FRAME);
  }

  /** @param pattern Compact legacy/frame/minimal/ascii preset, copied as a typed value.
   * @return Consuming scroll recipe; unknown names fail at mount before first paint.
   */
  Description &&scrollbar(std::string_view pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scrollbar(detail::scrollbarPattern(pattern));
  }

  /** @param pattern Typed preset; omitted skin leaves retain context defaults.
   * @return Consuming scroll recipe; invalid enum values fail before first paint.
   */
  Description &&scrollbar(ScrollbarPattern pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.scrollbar = {.pattern = pattern}});
  }

  /** @param pattern Independent part preset; unknown names fail before painting.
   * @return Consumed scroll recipe, preserving other parts and frame/title settings.
   */
  Description &&scrollTrack(std::string_view pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scrollTrack(detail::scrollbarPattern(pattern));
  }

  /** @param pattern Independent typed part preset. @return Consumed sparse scroll recipe. */
  Description &&scrollTrack(ScrollbarPattern pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.scrollbar = {.trackPattern = pattern}});
  }

  /** @param pattern Independent part preset; unknown names fail before painting.
   * @return Consumed scroll recipe, preserving other parts and frame/title settings.
   */
  Description &&scrollThumb(std::string_view pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scrollThumb(detail::scrollbarThumbPattern(pattern));
  }

  /** @param pattern Independent typed part preset. @return Consumed sparse scroll recipe. */
  Description &&scrollThumb(ScrollbarThumbPattern pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.scrollbar = {.thumbPattern = pattern}});
  }

  /** @param pattern Independent part preset; unknown names fail before painting.
   * @return Consumed scroll recipe, preserving other parts and frame/title settings.
   */
  Description &&scrollArrows(std::string_view pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scrollArrows(detail::scrollbarArrowPattern(pattern));
  }

  /** @param pattern Independent typed part preset. @return Consumed sparse scroll recipe. */
  Description &&scrollArrows(ScrollbarArrowPattern pattern) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.scrollbar = {.arrowPattern = pattern}});
  }

  /** @param skin Owning source copied into sparse configuration; glyphs are validated at mount.
   * @return Consuming scroll recipe. Existing geometry, offsets and input actions are preserved.
   * Construction/capture may throw on allocation, as for other owning recipe settings.
   */
  Description &&scrollbar(ScrollbarSkin skin) &&
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return std::move(*this).scalarSettings({.scrollbar = {.pattern = skin.pattern,
                                                          .ends = skin.ends,
                                                          .maxGlyphBytes = skin.maxGlyphBytes,
                                                          .horizontal = skin.horizontal,
                                                          .vertical = skin.vertical,
                                                          .corner = skin.corner,
                                                          .trackPattern = skin.trackPattern,
                                                          .thumbPattern = skin.thumbPattern,
                                                          .arrowPattern = skin.arrowPattern}});
  }

  /** @param enabled True creates a missing default one-cell frame; false removes the effective frame.
   * @return Consuming native area recipe; existing frame options survive an explicit true.
   */
  Description &&frameEnabled(bool enabled = true) &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).scalarSettings({.frame = {.enabled = enabled}});
  }

  /** @param glyphs One-cell glyph mode for an effective enabled frame.
   * @return Consuming native area recipe; an absent effective frame remains a typed error.
   */
  Description &&frameGlyphs(PrimitiveGlyphs glyphs) &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).scalarSettings({.frame = {.glyphs = glyphs}});
  }

  /** @param cells Positive paint cap for an effective enabled one-cell frame.
   * @return Consuming native area recipe; this does not enable a missing frame.
   */
  Description &&frameMaxPaintCells(std::size_t cells) &&
    requires std::same_as<T, ScrollArea>
  {
    return std::move(*this).scalarSettings({.frame = {.maxPaintCells = cells}});
  }

  /** @param axis Effective bar projection axis; standalone/shared writer ownership is unchanged.
   * @return Consuming native bar recipe with a checked sparse axis override.
   */
  Description &&axis(ScrollAxis axis) &&
    requires std::same_as<T, ScrollBar>
  {
    return std::move(*this).scalarSettings({.axis = axis});
  }

  /** @param value Positive cell rows per logical row.
   * @return This consuming exact native data-view recipe.
   */
  Description &&rowHeight(std::int32_t value) &&
    requires(detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value)
  {
    return std::move(*this).scalarSettings({.rowHeight = value});
  }

  /** @param value Extra requested rows per side, including explicit zero.
   * @return This consuming exact native data-view recipe.
   */
  Description &&overscanRows(std::size_t value) &&
    requires(detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value)
  {
    return std::move(*this).scalarSettings({.overscanRows = value});
  }

  /** @param value Positive table configured-column cap.
   * @return This consuming exact native data-view recipe.
   */
  Description &&maxColumns(std::size_t value) &&
    requires detail::IsVirtualTable<T>::value
  {
    return std::move(*this).scalarSettings({.maxColumns = value});
  }

  /** @param value Positive table visible body-cell cap.
   * @return This consuming exact native data-view recipe.
   */
  Description &&maxPreparedCells(std::size_t value) &&
    requires detail::IsVirtualTable<T>::value
  {
    return std::move(*this).scalarSettings({.maxPreparedCells = value});
  }

  /** @param value Positive table header/body UTF-8 cap.
   * @return This consuming exact native data-view recipe.
   */
  Description &&maxPreparedTextBytes(std::size_t value) &&
    requires detail::IsVirtualTable<T>::value
  {
    return std::move(*this).scalarSettings({.maxPreparedTextBytes = value});
  }

  /** @param value Existing line/candle rendering mode.
   * @return This consuming exact native chart recipe.
   */
  Description &&mode(ChartMode value) &&
    requires detail::IsChart<T>::value
  {
    return std::move(*this).scalarSettings({.mode = value});
  }

  /** @param value Chart candle glyph set, independent of primitive decorations.
   * @return This consuming exact native chart recipe.
   */
  Description &&chartGlyphSet(ChartGlyphSet value) &&
    requires detail::IsChart<T>::value
  {
    return std::move(*this).scalarSettings({.chartGlyphSet = value});
  }

  /** @param cells Nonnegative empty columns between candle samples; ignored by LINE mode.
   * @return This consuming exact native chart recipe.
   */
  Description &&candleGap(std::int32_t cells) &&
    requires detail::IsChart<T>::value
  {
    return std::move(*this).scalarSettings({.candleGap = cells});
  }

  /** @param range Finite increasing or equal explicit endpoints; nullopt restores automatic scale.
   * @return This consuming exact native chart recipe, preserving its model and reference identity.
   */
  Description &&yRange(std::optional<ChartRange> range) &&
    requires detail::IsChart<T>::value
  {
    ControlChartRangeSettings patch{.enabled = range.has_value()};

    if (range) {
      patch.minimum = range->minimum;
      patch.maximum = range->maximum;
    }

    return std::move(*this).scalarSettings({.yRange = patch});
  }

  /** @param value Whether native status labels/subtree/timer are created.
   * @return This consuming exact native DataStateView recipe.
   */
  Description &&showStatus(bool value = true) &&
    requires std::same_as<T, DataStateView>
  {
    return std::move(*this).scalarSettings({.showStatus = value});
  }

  /** @param value Positive fixed status strip height in cell rows.
   * @return This consuming exact native DataStateView recipe.
   */
  Description &&statusRows(std::int32_t value) &&
    requires std::same_as<T, DataStateView>
  {
    return std::move(*this).scalarSettings({.statusRows = value});
  }

  /** @param value Existing body visibility policy; automatic mode requires KEEP_BODY.
   * @return This consuming exact native DataStateView recipe.
   */
  Description &&contentPolicy(DataContentPolicy value) &&
    requires std::same_as<T, DataStateView>
  {
    return std::move(*this).scalarSettings({.contentPolicy = value});
  }

  /** @param value Positive uint64 monotonic milliseconds per spinner step.
   * @return This consuming exact native DataStateView recipe.
   */
  Description &&spinnerInterval(SchedulerTime value) &&
    requires std::same_as<T, DataStateView>
  {
    return std::move(*this).scalarSettings({.spinnerInterval = value});
  }

  /** @param values Owning sparse native status expressions; unused expressions remain unresolved.
   * @return This consuming exact native DataStateView recipe.
   */
  Description &&labels(ControlDataStateLabelSettings values) &&
    requires std::same_as<T, DataStateView>
  {
    return std::move(*this).scalarSettings({.labels = std::move(values)});
  }

  /** @param value Positive host registration cap, including initially closed layers.
   * @return This consuming exact native OverlayHost recipe.
   */
  Description &&maxLayers(std::size_t value) &&
    requires std::same_as<T, OverlayHost>
  {
    return std::move(*this).scalarSettings({.maxLayers = value});
  }

  /** @param value Owning caption expression; resolved by the actual native heading.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&title(TextValue value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.title = std::move(value)});
  }

  /** @param value Literal caption bytes, copied into an owning expression.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&title(std::string_view value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).title(TextValue(value));
  }

  /** @param value Native MODAL/NON_MODAL input policy.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&mode(DialogMode value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.dialog = {.mode = value}});
  }

  /** @brief Keep the main form available while this dialog is open.
   * @return This consuming exact native Dialog recipe with NON_MODAL input policy.
   * @details Equivalent to mode(DialogMode::NON_MODAL); preserves all other settings.
   * Later mode settings override this choice in declaration order.
   */
  Description &&nonModal() &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).mode(DialogMode::NON_MODAL);
  }

  /** @param value Native CENTER/BELOW_START/ABOVE_START placement.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&placement(DialogPlacement value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.dialog = {.placement = value}});
  }

  /** @param value Nonnegative viewport inset in cells.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&viewportMargin(std::int32_t value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.dialog = {.viewportMargin = value}});
  }

  /** @param value Nonnegative distance from an anchor.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&anchorGap(std::int32_t value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.dialog = {.anchorGap = value}});
  }

  /** @param value Existing Escape cancellation policy.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&closeOnEscape(bool value = true) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.dialog = {.closeOnEscape = value}});
  }

  /** @param options Top-frame close chrome; calling with defaults enables it.
   * @return This consuming Dialog recipe; tiny panels hide the affordance completely.
   */
  Description &&closeButton(DialogCloseOptions options = {}) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings(
      {.dialog = {
         .closeButton = {.enabled = options.enabled, .placement = options.placement, .focusable = options.focusable}}});
  }

  /** @param caps Positive independent width/height caps; never the full-host structural size.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&maximumPanelSize(CellSize caps) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings(
      {.dialog = {.maximumPanelSize = {.width = caps.width, .height = caps.height}}});
  }

  /** @param options Existing mandatory frame glyph/paint options; no optional enabled state.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&frame(BorderOptions options) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings(
      {.dialog = {.frame = {.glyphs = options.glyphs, .maxPaintCells = options.maxPaintCells}}});
  }

  /** @param values Sparse independent native options; unspecified actual values survive.
   * @return This consuming exact native Dialog recipe.
   */
  Description &&dialogSettings(ControlDialogSettings values) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.dialog = values});
  }

  /** @param value Owning MessageBox paragraph expression. @return This consuming message recipe.
   * Ordinary Dialog recipes reject this message-only setting during checked preparation.
   */
  Description &&body(TextValue value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).scalarSettings({.body = std::move(value)});
  }

  /** @param value Literal paragraph bytes copied immediately. @return This consuming message recipe. */
  Description &&body(std::string_view value) &&
    requires std::same_as<T, Dialog>
  {
    return std::move(*this).body(TextValue(value));
  }

  /** @param values Owning sparse native message options. @return This consuming actual MessageBox recipe.
   * An ordinary Dialog is rejected at checked preparation, including an empty explicit group.
   */
  Description &&messageSettings(ControlMessageSettings values) &&
    requires std::same_as<T, Dialog>
  {
    if (state_) {
      state_->messageSettingsRequested = true;
    }

    return std::move(*this).scalarSettings({.message = std::move(values)});
  }

  /** @param values Owning sparse constructor scalars for an actual default-catalog recipe.
   * @return Consuming recipe; external-model views reject these fields before construction.
   */
  Description &&themeCatalogSettings(ControlThemeCatalogSettings values) &&
    requires std::same_as<T, Dialog>
  {
    if (state_) {
      state_->themeCatalogSettingsRequested = true;
    }
    return std::move(*this).scalarSettings({.themeCatalog = std::move(values)});
  }

  /** @param values Sparse owning native selector view settings. @return Same actual selector recipe.
   * Ordinary Dialog and other semantic recipes reject even an explicitly empty selector stage.
   */
  Description &&themeSelectorSettings(ControlThemeSelectorSettings values) &&
    requires std::same_as<T, Dialog>
  {
    if (state_) {
      state_->themeSelectorSettingsRequested = true;
    }

    return std::move(*this).scalarSettings({.themeSelector = std::move(values)});
  }

  /** @param values Sparse immutable center options; captured resolver/body remain code-owned.
   * @return This consuming actual native notifications wrapper; ordinary Overlay rejects even an empty group.
   */
  Description &&notificationSettings(ControlNotificationSettings values) &&
    requires std::same_as<T, Overlay>
  {
    if (state_) {
      state_->notificationSettingsRequested = true;
    }

    return std::move(*this).scalarSettings({.notification = values});
  }

  /** @param opened Initial HelpView state; opening uses its existing finalizer and focus return.
   * @return This consuming native help recipe.
   */
  Description &&initiallyOpen(bool opened = true) &&
    requires std::same_as<T, HelpView>
  {
    return std::move(*this).scalarSettings({.initiallyOpen = opened});
  }

  /** @param values Owning sparse literal formatter patch, merged one leaf at a time.
   * @return This consuming native help presentation recipe.
   */
  Description &&format(ControlKeyFormatSettings values) &&
    requires(std::same_as<T, HelpView> || std::same_as<T, KeyHints>)
  {
    return std::move(*this).scalarSettings({.format = std::move(values)});
  }

  /** @param value Owning literal UTF-8 presentation text, never resolved as an expression.
   * @return This consuming applicable native help recipe.
   */
  Description &&emptyText(std::string value) &&
    requires(std::same_as<T, HelpView> || std::same_as<T, KeyHints>)
  {
    return std::move(*this).scalarSettings({.emptyText = std::move(value)});
  }

  /** @param value Owning literal UTF-8 presentation text, never resolved as an expression.
   * @return This consuming applicable native help recipe.
   */
  Description &&unavailableText(std::string value) &&
    requires(std::same_as<T, HelpView> || std::same_as<T, KeyHints>)
  {
    return std::move(*this).scalarSettings({.unavailableText = std::move(value)});
  }

  /** @param value Owning literal UTF-8 presentation text, never resolved as an expression.
   * @return This consuming applicable native help recipe.
   */
  Description &&entrySeparator(std::string value) &&
    requires std::same_as<T, KeyHints>
  {
    return std::move(*this).scalarSettings({.entrySeparator = std::move(value)});
  }

  /** @param value Owning literal UTF-8 presentation text, never resolved as an expression.
   * @return This consuming applicable native help recipe.
   */
  Description &&overflowMarker(std::string value) &&
    requires std::same_as<T, KeyHints>
  {
    return std::move(*this).scalarSettings({.overflowMarker = std::move(value)});
  }

  /** @param insets Nonnegative ordinary Overlay content insets.
   * @return Consuming exact Overlay recipe staging the shared canonical padding leaves.
   */
  Description &&padding(LayoutPadding insets) &&
    requires std::same_as<T, Overlay>
  {
    return std::move(*this).scalarSettings({.grid = {.paddingLeft = insets.left,
                                                     .paddingTop = insets.top,
                                                     .paddingRight = insets.right,
                                                     .paddingBottom = insets.bottom}});
  }

  /** @param value Owning/finite Spinner preset setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&preset(SpinnerPreset value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerPreset = value});
  }

  /** @param value Owning/finite Spinner frames setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&frames(std::string_view value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerFrames = std::string(value)});
  }

  /** @param value Owning/finite Spinner interval setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&interval(SchedulerTime value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerTick = value});
  }

  /** @param value Owning/finite Spinner direction setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&direction(SpinnerDirection value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerDirection = value});
  }

  /** @param value Owning/finite Spinner alignment setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&alignment(LayoutAlign value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerAlignment = value});
  }

  /** @param value Owning/finite Spinner active setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&active(bool value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerActive = value});
  }

  /** @param value Owning/finite Spinner reducedMotion setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&reducedMotion(bool value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerReducedMotion = value});
  }

  /** @param value Owning/finite Spinner label setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&label(std::string_view value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).label(TextValue(value));
  }

  /** @param value Owning literal/format/resource expression resolved at construction.
   * @return Consuming exact Spinner recipe with a staged label.
   */
  Description &&label(TextValue value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerLabel = std::move(value)});
  }

  /** @param value Owning/finite Spinner maxFrames setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&maxFrames(std::size_t value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerMaxFrames = value});
  }

  /** @param value Owning/finite Spinner maxFrameBytes setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&maxFrameBytes(std::size_t value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerMaxFrameBytes = value});
  }

  /** @param value Owning/finite Spinner maxFrameCells setting, checked at preparation.
   * @return Consuming exact Spinner recipe; previous unrelated settings are preserved.
   */
  Description &&maxFrameCells(std::size_t value) &&
    requires std::same_as<T, Spinner>
  {
    return std::move(*this).scalarSettings({.spinnerMaxFrameCells = value});
  }

  /** @param align Child alignment across ordinary Overlay columns.
   * @return Consuming exact Overlay recipe; containing Grid alignment stays independent.
   */
  Description &&horizontalAlign(LayoutAlign align) &&
    requires std::same_as<T, Overlay>
  {
    return std::move(*this).scalarSettings({.horizontalAlign = align});
  }

  /** @param align Child alignment across ordinary Overlay rows.
   * @return Consuming exact Overlay recipe; OverlayHost policies remain separate.
   */
  Description &&verticalAlign(LayoutAlign align) &&
    requires std::same_as<T, Overlay>
  {
    return std::move(*this).scalarSettings({.verticalAlign = align});
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

  /** @param value Class names copied now, separated only by ASCII whitespace; empty clears.
   * @return Consuming recipe; classes replace previous classes in modifier order, preserving other metadata.
   * A later style replaces the classes with its own complete target; a later classes modifier replaces only classes.
   */
  Description &&classes(std::string_view value) && {
    return std::move(*this).setup([values = detail::splitClasses(value)](ManagedScene &scene, Node<T> node) mutable {
      return scene.classes(node, values.values);
    });
  }

  /** @tparam Callback Compact owning click callback, optionally receiving ClickEvent.
   * @param callback Void, expected<Value, TreeError> or EditorCommandResult action; success values are ignored.
   * @return Consuming button recipe using one existing managed binding; duplicates fail at mount.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::same_as<T, Button> && portals::detail::ManagedAction<Callback, ClickEvent>)
  Description &&onClick(Callback callback) && {
    if constexpr (std::same_as<Callback, ActionRef>) {
      return std::move(*this).setup([declaration = detail::retainAction(callback),
                                     callback = std::move(callback)](ManagedScene &scene, Node<T> node) mutable {
        (void)declaration;

        return scene.onClick(node, std::move(callback));
      });
    }

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

  /** @param value Final local visibility; false excludes this subtree from layout, paint and input.
   * @return Consuming recipe; repeated calls replace the previous value before mount.
   */
  Description &&visible(bool value = true) && {
    if (state_) {
      state_->visible = value;
    }

    return std::move(*this);
  }

  /** @param value Final local enabled state; false preserves layout/paint and uses existing input/style rules.
   * @return Consuming recipe; ancestor flags continue to dominate this local value.
   */
  Description &&enabled(bool value = true) && {
    if (state_) {
      state_->enabled = value;
    }

    return std::move(*this);
  }

  /** @param options Owning frame pattern, sides, title and finite limits; validated during mount.
   * @return Consuming recipe retaining its original typed Ref and handlers. Width/height/flex and
   * mount-only visibility/enabled flags address the outer frame; custom setup still addresses T.
   * ScrollArea configures its own frame; ordinary sized widgets gain one passive Border ancestor.
   */
  Description &&framed(FrameOptions options = {}) &&
    requires std::derived_from<T, SizedWidget>
  {
    if (state_) {
      auto &frame = frameRecipe();

      frame.options = std::move(options);
      frame.hasOptions = true;
      frame.invalidPattern = false;
    }

    return std::move(*this);
  }

  /** @param decoration Opt-in title ends; validated at mount, with no extra height.
   * @return Consuming recipe retaining source identity and the effective frame.
   */
  Description &&titleDecoration(TitleDecoration decoration) &&
    requires std::derived_from<T, SizedWidget>
  {
    if (state_) {
      auto &frame = frameRecipe();
      frame.hasOptions = true;
      frame.options.titleDecoration = decoration;
      frame.options.customTitleDecoration.reset();
    }

    return std::move(*this);
  }

  /** @param name none/spaced/square/angle/round; unknown values fail with INVALID_ARGUMENT at mount.
   * @return Same consuming recipe as the enum overload.
   */
  Description &&titleDecoration(std::string_view name) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).titleDecoration(detail::titleDecorationPreset(name));
  }

  /** @param decoration Owning prefix/suffix, each at most64 bytes and16 one-cell printable graphemes.
   * @return Consuming recipe; invalid text fails before publication at mount.
   */
  Description &&titleDecoration(CustomTitleDecoration decoration) &&
    requires std::derived_from<T, SizedWidget>
  {
    if (state_) {
      auto &frame = frameRecipe();
      frame.hasOptions = true;
      frame.options.customTitleDecoration = decoration;
    }

    return std::move(*this);
  }

  /** @param pattern One of single/plain, rounded, double, heavy or ascii.
   * @return Consuming frame recipe; unknown names become typed frame.pattern errors at mount.
   */
  Description &&framed(std::string_view pattern) &&
    requires std::derived_from<T, SizedWidget>
  {
    if (state_) {
      auto &frame = frameRecipe();
      const auto selected = detail::framePattern(pattern);

      frame.options = {};
      frame.hasOptions = true;
      frame.invalidPattern = !selected;

      if (selected) {
        frame.options.pattern = *selected;
      }
    }

    return std::move(*this);
  }

  /** @param pattern Checked compact preset name, with the existing aliases and mount errors.
   * @param title UTF-8 top-edge title copied now and validated with the existing frame limits at mount.
   * @return Same typed recipe, retaining its original reference, handlers and outer-size behavior.
   */
  Description &&framed(std::string_view pattern, std::string_view title) &&
    requires std::derived_from<T, SizedWidget>
  {
    (void)std::move(*this).framed(pattern);

    if (state_) {
      frameRecipe().options.title = title;
    }

    return std::move(*this);
  }

  /** @param outer Empty observer published for the retained Border only after complete mount success.
   * @param options Owning advanced frame settings.
   * @return Same typed child recipe; hide/remove outer to affect both frame and content.
   * Removing or hiding the original child Ref retains the ordinary downward-only tree semantics.
   */
  Description &&framed(Ref<Border> outer, FrameOptions options = {}) &&
    requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
  {
    (void)std::move(*this).framed(std::move(options));

    if (state_) {
      frameRecipe().reference = std::move(outer);
    }

    return std::move(*this);
  }

  /** @param outer Empty observer for this recipe's new Border ancestor.
   * @param pattern Checked compact preset name.
   * @return Same typed child recipe, with the same delayed reference publication as framed(outer, options).
   */
  Description &&framed(Ref<Border> outer, std::string_view pattern) &&
    requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
  {
    (void)std::move(*this).framed(pattern);

    if (state_) {
      frameRecipe().reference = std::move(outer);
    }

    return std::move(*this);
  }

  /** @param outer Empty observer published only after complete mount success.
   * @param pattern Checked compact preset name.
   * @param title Owning top-edge title, checked at mount with the existing frame limits.
   * @return Same typed child recipe, with the existing outer Border publication contract.
   */
  Description &&framed(Ref<Border> outer, std::string_view pattern, std::string_view title) &&
    requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
  {
    (void)std::move(*this).framed(std::move(outer), pattern);

    if (state_) {
      frameRecipe().options.title = title;
    }

    return std::move(*this);
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

  /** @return Content-preferred width, shrinkable within existing bounds; checked after setup. */
  Description &&fitWidth() &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).constrainSize({detail::SizeDimension::FIT_WIDTH});
  }

  /** @return Content-preferred height, shrinkable within existing bounds; checked after setup. */
  Description &&fitHeight() &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).constrainSize({detail::SizeDimension::FIT_HEIGHT});
  }

  /** @param cells Nonnegative hard minimum width; does not change the axis mode.
   * @return Consuming recipe; final incompatible bounds fail mount with a typed error.
   */
  Description &&minWidth(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).constrainSize({detail::SizeDimension::MIN_WIDTH, cells});
  }

  /** @param cells Nonnegative hard maximum width; does not change the axis mode.
   * @return Consuming recipe; final incompatible bounds fail mount with a typed error.
   */
  Description &&maxWidth(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).constrainSize({detail::SizeDimension::MAX_WIDTH, cells});
  }

  /** @param cells Nonnegative hard minimum height; does not change the axis mode.
   * @return Consuming recipe; final incompatible bounds fail mount with a typed error.
   */
  Description &&minHeight(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).constrainSize({detail::SizeDimension::MIN_HEIGHT, cells});
  }

  /** @param cells Nonnegative hard maximum height; does not change the axis mode.
   * @return Consuming recipe; final incompatible bounds fail mount with a typed error.
   */
  Description &&maxHeight(std::int32_t cells) &&
    requires std::derived_from<T, SizedWidget>
  {
    return std::move(*this).constrainSize({detail::SizeDimension::MAX_HEIGHT, cells});
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
  friend struct detail::ResourceAccess;
  template <detail::DescriptionInput Child> friend Description<Border> border(Child &&, BorderOptions, ControlStyle);

  Description &&scalarSettings(ControlSettings patch) && {
    if (state_) {
      detail::applyFluentControlOrigins(patch, state_->controlPatch);
      state_->controlPatch = detail::Access::mergeControlSettings(std::move(state_->controlPatch), patch);
      state_->settingsRequested = true;
    }

    return std::move(*this);
  }

  template <detail::DescriptionInput Child>
  friend Description<ScrollArea> scrollArea(Child &&, ScrollAreaOptions, std::shared_ptr<ScrollController>,
                                            ControlStyle);

  template <class U, class... Args> friend Description<U> custom(Args &&...args);
  template <class U> friend Description<U> control(std::string_view, ControlStyle, SizePolicy);
  template <class U, detail::DescriptionInput... Children> friend Description<U> linear(LinearOptions, Children &&...);
  template <class U, detail::DescriptionRange R> friend Description<U> linear(LinearOptions, R &&);
  template <detail::DescriptionInput... Children>
  friend Description<Overlay> overlay(OverlayOptions, SizePolicy, Children &&...);
  template <detail::GridInput... Children> friend Description<Grid> grid(Children &&...);
  friend Description<LineEdit> lineEdit(ManagedState<std::string>, LineEditConfig);

  using State = detail::RecipeState<T>;
  using Factory = typename State::Factory;
  using Child = typename State::Child;
  using FrameRecipe = typename State::FrameRecipe;

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
    auto *state = state_.get();

    if (state && state->sizing) {
      state->sizing->stage(change, false);
    }

    // The callback lives in this State and runs synchronously before mountInto releases it.
    // It must never be exported to publish/finalize/runtime callbacks.
    return std::move(*this).setup([change, state](ManagedScene &scene, Node<T> node) {
      const Node<SizedWidget> target = state && state->frame && state->frame->mounted != Node<Border>{}
                                         ? Node<SizedWidget>(state->frame->mounted)
                                         : Node<SizedWidget>(node);

      return detail::Access::resize(scene, target, change);
    });
  }

  /** @brief Retain sparse bounds until the entire cold recipe has completed setup. */
  Description &&constrainSize(detail::SizeChange change) &&
    requires std::derived_from<T, SizedWidget>
  {
    if (state_) {
      if (!state_->sizing) {
        state_->sizing = std::make_unique<detail::RecipeSizing>();
      }

      state_->sizing->stage(change, true);
    }

    return std::move(*this);
  }

  /** @return Stable cold frame configuration, allocated only when this recipe opts in. */
  FrameRecipe &frameRecipe() {
    if (!state_->frame) {
      state_->frame = std::make_unique<FrameRecipe>();
    }

    return *state_->frame;
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

  /** @brief Validate actual semantic capabilities before merging a prepared resource packet.
   * Allocation failures propagate to the outer mount boundary; retained provenance stays owning.
   */
  static std::expected<void, ManagedError> prepareSemanticControl(State &state, Node<Widget> parent) {
    const auto kind = state.semanticConstruction ? state.semanticConstruction->kind : detail::ControlFactoryKind::USER;

    if ((state.messageSettingsRequested && kind != detail::ControlFactoryKind::MESSAGE_BINDING) ||
        (state.notificationSettingsRequested && kind != detail::ControlFactoryKind::NOTIFICATION_BINDING) ||
        (state.themeSelectorSettingsRequested && kind != detail::ControlFactoryKind::THEME_SELECTOR_BINDING &&
         kind != detail::ControlFactoryKind::THEME_CATALOG_BINDING) ||
        (state.themeCatalogSettingsRequested && kind != detail::ControlFactoryKind::THEME_CATALOG_BINDING)) {
      return std::unexpected(detail::controlContextError({ControlContextErrorCode::UNSUPPORTED_PROPERTY, {}}, parent));
    }

    if (state.semanticConstruction && state.semanticConstruction->prepared) {
      const auto &source = state.semanticConstruction->source;
      auto merged = detail::Access::mergeControlSettings(state.controlPatch, source);

      // Resource packets come from ControlContext::resolve, whose origins contain exactly
      // the present winning leaves. Keep factory provenance for every other retained value.
      merged.origin = state.controlPatch.origin;
      merged.origins = state.controlPatch.origins;

      for (const auto &origin : source.origins) {
        auto found = std::ranges::find(merged.origins, origin.path, &ControlPropertyOrigin::path);

        if (found == merged.origins.end()) {
          merged.origins.push_back(origin);
        } else {
          found->source = origin.source;
        }
      }

      state.controlPatch = std::move(merged);
      state.controlPrepared = true;
      state.materializedSettings = state.semanticConstruction->materialized;
      state.settingsRequested = true;
    }

    return {};
  }

  /** @brief Install only the concrete recipe's scoped fallback into its effective context. */
  static std::expected<void, ControlContextError> installControlSchema(const State &state, ControlContext &controls) {
    if constexpr (std::same_as<T, Border> || std::same_as<T, Gauge> || std::same_as<T, Spinner>) {
      return detail::ControlContextAccess::installPrimitiveSchema(controls, *state.controlType);
    } else if constexpr (std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar> || std::same_as<T, LogView>) {
      return detail::ControlContextAccess::installScrollSchema(controls, *state.controlType);
    } else if constexpr (std::same_as<T, DataStateView>) {
      return detail::ControlContextAccess::installDataStateSchema(controls);
    } else if constexpr (std::same_as<T, OverlayHost> || std::same_as<T, Dialog>) {
      if (!state.semanticConstruction) {
        return detail::ControlContextAccess::installDialogSchema(controls, *state.controlType);
      }

      if (state.semanticConstruction->kind == detail::ControlFactoryKind::THEME_CATALOG_BINDING) {
        return detail::ControlContextAccess::installThemeCatalogSchema(controls, true);
      }

      if (state.semanticConstruction->kind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING) {
        return detail::ControlContextAccess::installThemeSelectorSchema(controls);
      }

      return detail::ControlContextAccess::installMessageSchema(controls);
    } else if constexpr (std::same_as<T, Overlay>) {
      return state.semanticConstruction ? detail::ControlContextAccess::installNotificationSchema(controls)
                                        : detail::ControlContextAccess::installOverlaySchema(controls);
    } else if constexpr (std::same_as<T, HelpView> || std::same_as<T, KeyHints>) {
      return detail::ControlContextAccess::installKeyHelpSchema(controls, *state.controlType);
    } else if constexpr (detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value ||
                         detail::IsChart<T>::value) {
      return detail::ControlContextAccess::installVirtualViewSchema(controls, *state.controlType);
    } else {
      return {};
    }
  }

  /** @brief Check factory capability after resolving the variant's concrete base. */
  static bool matchesControlFactory(const State &state, const ResolvedControl &effective) {
    if constexpr (std::same_as<T, Border> || std::same_as<T, Gauge> || std::same_as<T, Spinner>) {
      return effective.factoryKind == detail::ControlFactoryKind::PRIMITIVE_FALLBACK;
    } else if constexpr (std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar> || std::same_as<T, LogView>) {
      return effective.factoryKind == detail::ControlFactoryKind::SCROLL_FALLBACK;
    } else if constexpr (std::same_as<T, DataStateView>) {
      return effective.factoryKind == detail::ControlFactoryKind::DATA_STATE_FALLBACK;
    } else if constexpr (std::same_as<T, OverlayHost> || std::same_as<T, Dialog>) {
      const auto expectedKind =
        state.semanticConstruction ? state.semanticConstruction->kind : detail::ControlFactoryKind::DIALOG_FALLBACK;

      return effective.factoryKind == expectedKind;
    } else if constexpr (std::same_as<T, Overlay>) {
      const auto expectedKind = state.semanticConstruction ? detail::ControlFactoryKind::NOTIFICATION_BINDING
                                                           : detail::ControlFactoryKind::OVERLAY_FALLBACK;

      return effective.factoryKind == expectedKind;
    } else if constexpr (std::same_as<T, HelpView> || std::same_as<T, KeyHints>) {
      return effective.factoryKind == detail::ControlFactoryKind::KEY_HELP_FALLBACK;
    } else if constexpr (detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value ||
                         detail::IsChart<T>::value) {
      return effective.factoryKind == detail::ControlFactoryKind::VIRTUAL_VIEW_FALLBACK;
    } else {
      return true;
    }
  }

  /** @brief Restore a non-inherited visual identity, then validate its base and factory in that order. */
  static std::expected<void, ManagedError> validateControlIdentity(const State &state, ResolvedControl &effective,
                                                                   const ControlContext &controls, Node<Widget> parent,
                                                                   bool restoreVariant) {
    if (restoreVariant && state.controlVariant) {
      const auto identity = controls.resolve(*state.controlVariant);

      if (!identity) {
        return std::unexpected(detail::controlContextError(identity.error(), parent));
      }

      effective.type = identity->type;
      effective.base = identity->base;
      effective.lineage = identity->lineage;
    }

    if (effective.base != *state.controlType) {
      return std::unexpected(detail::controlContextError({ControlContextErrorCode::INVALID_VALUE, {}}, parent));
    }

    if (!matchesControlFactory(state, effective)) {
      return std::unexpected(detail::controlContextError({ControlContextErrorCode::INVALID_VALUE, {}}, parent));
    }

    return {};
  }

  /** @brief Charge only new leaves before allocating style identity or retained resolved settings.
   * All inputs are borrowed for this synchronous preparation step; State owns the final copies.
   */
  static std::expected<void, ManagedError>
  retainControlSettings(State &state, ResolvedControl &effective,
                        const std::optional<ResolvedControl> &notificationStyle, Node<Widget> parent,
                        detail::MountContext &context) {
    const auto addedLeaves =
      state.controlPrepared
        ? (detail::controlSettingsPresenceSet(effective.settings) & ~state.materializedSettings).count()
        : effective.materializedLeaves;
    const auto limit = context.controls.limits().maxMaterializedLeaves;

    if (context.controlLeaves > limit || addedLeaves > limit - context.controlLeaves) {
      return std::unexpected(
        detail::controlContextError({ControlContextErrorCode::MATERIALIZED_LEAF_LIMIT, {}}, parent));
    }

    context.controlLeaves += addedLeaves;

    if (state.controlVariant) {
      const auto &identity = notificationStyle ? *notificationStyle : effective;

      state.intrinsicStyle =
        StyleTarget{.type = identity.type,
                    .control = std::derived_from<T, Control> || std::same_as<T, LineEdit> || std::same_as<T, Password>,
                    .baseTypes = identity.lineage};

      if constexpr (std::same_as<T, Row> || std::same_as<T, Column>) {
        constexpr std::string_view NATURAL = std::same_as<T, Row> ? "Row" : "Column";

        if (state.intrinsicStyle->type != NATURAL &&
            std::ranges::find(state.intrinsicStyle->baseTypes, NATURAL) == state.intrinsicStyle->baseTypes.end()) {
          state.intrinsicStyle->baseTypes.emplace_back(NATURAL);
        }
      }
    }

    state.resolvedControl = std::make_unique<ResolvedControl>(std::move(effective));

    return {};
  }

  /** @brief Resolve settings in a local schema snapshot, keeping notification styling independent of defaults. */
  static std::expected<void, ManagedError> prepareControlSettings(State &state, Node<Widget> parent,
                                                                  detail::MountContext &context) {
    std::optional<ResolvedControl> notificationStyle;

    if constexpr (std::same_as<T, Overlay>) {
      if (state.semanticConstruction && state.controlVariant &&
          state.semanticConstruction->kind == detail::ControlFactoryKind::NOTIFICATION_BINDING) {
        auto identity = context.controls.resolve(*state.controlVariant);

        if (!identity) {
          return std::unexpected(detail::controlContextError(identity.error(), parent));
        }

        if (identity->base == "Overlay" && identity->factoryKind == detail::ControlFactoryKind::OVERLAY_FALLBACK) {
          // Existing Overlay variants on notifications are visual identities, never inherited service/layout
          // defaults.
          notificationStyle.emplace(std::move(*identity));
        }
      }
    }

    const bool inheritControlSettings = state.inheritControlSettings && !notificationStyle;
    auto controls =
      !inheritControlSettings || (state.controlPrepared && !state.controlVariant && !state.inheritPreparedDefaults)
        ? ControlContext(context.controls.limits())
        : context.controls;
    auto installed = installControlSchema(state, controls);

    if (!installed) {
      return std::unexpected(detail::controlContextError(installed.error(), parent));
    }

    const auto type = inheritControlSettings && state.controlVariant ? std::string_view(*state.controlVariant)
                                                                     : std::string_view(*state.controlType);
    auto effective = inheritControlSettings
                       ? controls.resolve(type, detail::Access::mergeControlSettings(std::move(state.controlArguments),
                                                                                     state.controlPatch))
                       : controls.resolve(type, state.controlPatch);

    if (!effective) {
      return std::unexpected(detail::controlContextError(effective.error(), parent));
    }

    auto validated = validateControlIdentity(state, *effective, context.controls, parent,
                                             !inheritControlSettings && !notificationStyle);

    if (!validated) {
      return validated;
    }

    return retainControlSettings(state, *effective, notificationStyle, parent, context);
  }

  /** @brief Finish fallible owning configuration before entering recursive construction/children.
   * @param state Consumed recipe storage, retaining any resolved properties until setup completes.
   * @param parent Existing checked parent used for failure attribution.
   * @param context Whole-mount immutable snapshot and aggregate leaf accounting.
   * @return Typed validation/budget result; allocation exceptions stay inside the outer mount boundary.
   */
  static std::expected<void, ManagedError> prepareControl(State &state, Node<Widget> parent,
                                                          detail::MountContext &context) {
    if constexpr (std::same_as<T, Dialog> || std::same_as<T, Overlay>) {
      auto prepared = prepareSemanticControl(state, parent);

      if (!prepared) {
        return prepared;
      }
    }

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
      return prepareControlSettings(state, parent, context);
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

    auto mountParent = parent;

    if constexpr (std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>) {
      const auto &settings = resolved ? resolved->settings : state->controlArguments;
      const bool configuredFrame = detail::hasFrameSettings(settings.frame);

      if (state->frame || configuredFrame) {
        if (state->frame && state->frame->invalidPattern) {
          return std::unexpected(
            detail::frameContextError(ControlContextErrorCode::INVALID_VALUE, "frame.pattern", parent));
        }

        auto effective = detail::controlFrameOptions(
          settings, state->frame && state->frame->hasOptions ? std::optional(state->frame->options) : std::nullopt,
          scene.widthPolicy());

        if (!effective) {
          return std::unexpected(effective.error());
        }

        if (state->frame && state->frame->source) {
          effective = detail::controlFrameOptions(*state->frame->source, std::move(*effective), scene.widthPolicy());

          if (!effective) {
            return std::unexpected(effective.error());
          }
        }

        if (*effective) {
          if (!state->frame) {
            state->frame = std::make_unique<FrameRecipe>();
          }

          state->frame->options = std::move(**effective);
          state->frame->hasOptions = true;
        } else {
          state->frame.reset();
        }
      }
    }

    if (state->frame) {
      if (state->frame->invalidPattern) {
        return std::unexpected(
          detail::frameContextError(ControlContextErrorCode::INVALID_VALUE, "frame.pattern", parent));
      }

      const auto valid = Border::validateFrame(state->frame->options);

      if (!valid) {
        return std::unexpected(detail::error(parent, valid.error()));
      }

      if constexpr (std::derived_from<T, ScrollArea>) {
        // Only the native factory consumes constructor-scoped advanced frame settings.
        if (!state->controlType || (*state->controlType != "ScrollArea" && *state->controlType != "LogView")) {
          return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
        }
      } else {
        auto frame = detail::Access::framedControl(std::move(state->frame->options), {});

        if (state->frame->reference) {
          (void)std::move(frame).reference(*state->frame->reference);
        }

        auto mounted = detail::Access::mount(std::move(frame), scene, parent, context);

        if (!mounted) {
          return std::unexpected(mounted.error());
        }

        state->frame->mounted = *mounted;
        mountParent = *mounted;
      }
    }

    context.mountedOuter = {};

    auto node = [&] {
      struct Restore {
        detail::MountContext &context;
        bool previous;
        const ControlSettings *settings;
        const FrameOptions *frame;
        const ControlSettings *frameSettings;
        Description<Widget> *body;
        ~Restore() {
          context.controlPrepared = previous;
          context.controlSettings = settings;
          context.frameOptions = frame;
          context.frameSettings = frameSettings;
          context.messageBody = body;
        }
      } restore{context,
                context.controlPrepared,
                context.controlSettings,
                context.frameOptions,
                context.frameSettings,
                context.messageBody};

      context.controlPrepared = state->controlPrepared;
      context.controlSettings = resolved ? &resolved->settings : &state->controlArguments;
      context.frameOptions =
        std::derived_from<T, ScrollArea> && state->frame && state->frame->hasOptions ? &state->frame->options : nullptr;
      context.frameSettings = std::derived_from<T, ScrollArea> && state->frame ? state->frame->source.get() : nullptr;

      context.messageBody = state->semanticConstruction ? state->semanticConstruction->body.get() : nullptr;

      return state->create(scene, mountParent, context);
    }();

    if (!node) {
      return node;
    }

    const Node<Widget> outer = state->frame && state->frame->mounted != Node<Border>{}
                                 ? Node<Widget>(state->frame->mounted)
                               : state->forwardsOuter && context.mountedOuter != Node<Widget>{} ? context.mountedOuter
                                                                                                : Node<Widget>(*node);

    if (context.root == Node<Widget>{}) {
      context.root = *node;
    }

    context.nodes.push_back(*node);

    if (state->visible || state->enabled) {
      auto configured = detail::Access::instanceState(scene, outer, state->visible, state->enabled);

      if (!configured) {
        return std::unexpected(configured.error());
      }
    }

    if (state->intrinsicStyle) {
      auto styled = [&] {
        if constexpr (std::same_as<T, Dialog> || std::same_as<T, OverlayHost>) {
          // These managed factories already supplied their native structural classes.
          return detail::Access::finalizeStyleIdentity(scene, *node, *state->intrinsicStyle);
        } else if constexpr (std::same_as<T, Overlay>) {
          return state->semanticConstruction
                   ? detail::Access::finalizeStyleIdentity(scene, *node, *state->intrinsicStyle)
                   : scene.style(*node, *state->intrinsicStyle);
        } else {
          return scene.style(*node, *state->intrinsicStyle);
        }
      }();

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
        const auto *sourceSizing =
          state->semanticConstruction && state->semanticConstruction->prepared &&
              state->semanticConstruction->kind == detail::ControlFactoryKind::NOTIFICATION_BINDING
            ? &state->semanticConstruction->source
            : nullptr;

        for (const auto &change :
             {settings.width && !(sourceSizing && sourceSizing->width)
                ? std::optional(detail::SizeChange{detail::SizeDimension::WIDTH, *settings.width})
                : std::nullopt,
              settings.height && !(sourceSizing && sourceSizing->height)
                ? std::optional(detail::SizeChange{detail::SizeDimension::HEIGHT, *settings.height})
                : std::nullopt,
              settings.flex && !(sourceSizing && sourceSizing->flex)
                ? std::optional(detail::SizeChange{detail::SizeDimension::FLEX, 0, *settings.flex})
                : std::nullopt}) {
          if (change) {
            const Node<SizedWidget> target = state->frame && state->frame->mounted != Node<Border>{}
                                               ? Node<SizedWidget>(state->frame->mounted)
                                               : Node<SizedWidget>(*node);
            auto resized = detail::Access::resize(scene, target, *change);
            if (!resized) {
              return std::unexpected(resized.error());
            }
          }
        }
      }
    }

    Node<LineEdit> fieldDraft;

    if constexpr (std::same_as<T, FormField> || std::same_as<T, FindBar> || std::derived_from<T, ComboBox>) {
      auto editor = detail::NamedAccess::fieldDraft(scene, *node);

      if (!editor) {
        return std::unexpected(editor.error());
      }

      fieldDraft = *editor;
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
      context.publish.push_back([slot, value = *node, owner = &scene, lifetime = detail::Access::lifetime(scene),
                                 fieldDraft] {
        slot->node = value;
        slot->target.node = value;
        slot->target.scene = owner;
        slot->target.lifetime = lifetime;

        if constexpr (std::derived_from<T, Control>) {
          slot->target.caption = value;
        } else if constexpr (std::same_as<T, LineEdit>) {
          slot->target.draft = value;
        } else if constexpr (std::same_as<T, FormField> || std::same_as<T, FindBar> || std::derived_from<T, ComboBox>) {
          slot->target.draft = fieldDraft;
        } else if constexpr (std::same_as<T, Password>) {
          slot->target.secret = value;
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
      } else if constexpr (std::same_as<T, FormField> || std::same_as<T, FindBar> || std::derived_from<T, ComboBox>) {
        pending.draft = fieldDraft;
      } else if constexpr (std::same_as<T, Password>) {
        pending.secret = *node;
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

    if constexpr (std::same_as<T, Overlay>) {
      if (state->semanticConstruction && state->semanticConstruction->prepared &&
          state->semanticConstruction->kind == detail::ControlFactoryKind::NOTIFICATION_BINDING) {
        // Resource common sizing follows factory setup, including through erased native recipes.
        const auto &source = state->semanticConstruction->source;

        for (const auto &change :
             {source.width ? std::optional(detail::SizeChange{detail::SizeDimension::WIDTH, *source.width})
                           : std::nullopt,
              source.height ? std::optional(detail::SizeChange{detail::SizeDimension::HEIGHT, *source.height})
                            : std::nullopt,
              source.flex ? std::optional(detail::SizeChange{detail::SizeDimension::FLEX, 0, *source.flex})
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

    if constexpr (std::derived_from<T, SizedWidget>) {
      if (state->sizing || (state->frame && state->frame->mounted != Node<Border>{})) {
        const auto target = state->frame && state->frame->mounted != Node<Border>{}
                              ? Node<SizedWidget>(state->frame->mounted)
                              : Node<SizedWidget>(*node);
        auto sized = detail::Access::finishSizing(scene, *node, target, state->sizing.get());

        if (!sized) {
          return std::unexpected(sized.error());
        }
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

    context.mountedOuter = outer;

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
                          ? (controlSettingsPresenceSet(effective->settings) & ~control.materializedSettings).count()
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

/// @cond INTERNAL
template <class T> bool Access::needsChoiceHost(const Description<T> &description) {
  const auto *layout = description.state_ ? description.state_->layout.get() : nullptr;

  return layout && layout->needsChoiceHost && !layout->hasOverlayHost;
}

template <class T, class Probe> Description<T> Access::choiceProbe(Description<T> description, Probe probe) {
  description.layoutMetadata().choiceProbe = std::move(probe);
  return description;
}

template <class T>
std::expected<bool, ManagedError> Access::prepareChoiceHost(Description<T> &description, ControlContext controls) {
  auto root = description.state_ ? description.state_->layout : nullptr;
  if (!root) {
    return false;
  }
  std::vector<std::shared_ptr<RecipeLayout>> pending(0);
  pending.push_back(root);
  bool needed = root->needsChoiceHost, hosted = root->hasOverlayHost;
  for (std::size_t index = 0; index < pending.size(); ++index) {
    const auto layout = pending[index];
    if (!layout) {
      continue;
    }
    needed = needed || layout->needsChoiceHost;
    hosted = hosted || layout->hasOverlayHost;
    if (layout->choiceProbe) {
      auto result = layout->choiceProbe(controls);
      if (!result) {
        return std::unexpected(result.error());
      }
      needed = needed || result->first;
      hosted = hosted || result->second;
    }
    pending.insert(pending.end(), layout->children.begin(), layout->children.end());
  }
  return needed && !hosted;
}

template <class T> Description<T> Access::choiceHost(Description<T> description, bool explicitHost) {
  auto &layout = description.layoutMetadata();
  layout.needsChoiceHost = !explicitHost;
  layout.hasOverlayHost = explicitHost;

  return description;
}

/// @endcond

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
        // A framed source keeps its typed Ref while the Grid places its direct Border child.
        layout->mounted.push_back(context.mountedOuter.id());
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
        layout->needsChoiceHost = layout->needsChoiceHost || child.state_->layout->needsChoiceHost;
        layout->hasOverlayHost = layout->hasOverlayHost || child.state_->layout->hasOverlayHost;
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
    parent.state_->layout->needsChoiceHost =
      parent.state_->layout->needsChoiceHost || child.state_->layout->needsChoiceHost;
    parent.state_->layout->hasOverlayHost =
      parent.state_->layout->hasOverlayHost || child.state_->layout->hasOverlayHost;
  }
}

template <class T> void Access::preparedControl(Description<T> &description, std::uint32_t alreadyMaterialized) {
  preparedControlPresence(description, ControlPropertyPresence{alreadyMaterialized});
}

template <class T>
void Access::preparedControlPresence(Description<T> &description, ControlPropertyPresence alreadyMaterialized) {
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

inline void Access::preparedDialogHeading(Description<Label> &description, const TextValue &title) {
  ControlSettings materialized;

  materialized.text = title;
  preparedControlPresence(description, controlSettingsPresenceSet(materialized));

  if (description.state_) {
    description.state_->inheritPreparedDefaults = true;
  }
}

template <class T> void Access::preparedThemeSelectorCaption(Description<T> &description, const TextValue &caption) {
  static_assert(std::same_as<T, Label> || std::same_as<T, Button>);
  ControlSettings settings;

  settings.text = caption;
  preparedControlPresence(description, controlSettingsPresenceSet(settings));

  if (description.state_) {
    description.state_->inheritPreparedDefaults = true;
    description.state_->settingsRequested = true;
  }
}

inline Description<Dialog> Access::themeCatalogControl(Description<Dialog> description) {
  if (description.state_) {
    description.state_->semanticConstruction = std::make_shared<SemanticConstruction>();
    description.state_->semanticConstruction->kind = ControlFactoryKind::THEME_CATALOG_BINDING;
  }
  return configuredControl(std::move(description), "ThemeSelectorCatalog");
}

template <class T> bool Access::themeCatalogRecipient(const Description<T> &description) noexcept {
  return description.state_ && description.state_->semanticConstruction &&
         description.state_->semanticConstruction->kind == ControlFactoryKind::THEME_CATALOG_BINDING;
}

inline Description<Dialog> Access::themeSelectorControl(Description<Dialog> description) {
  if (description.state_) {
    description.state_->semanticConstruction = std::make_shared<SemanticConstruction>();
    description.state_->semanticConstruction->kind = ControlFactoryKind::THEME_SELECTOR_BINDING;
  }

  return configuredControl(std::move(description), "ThemeSelector");
}

inline Description<Dialog> Access::messageControl(Description<Dialog> description) {
  if (description.state_) {
    description.state_->semanticConstruction = std::make_shared<SemanticConstruction>();
  }

  return configuredControl(std::move(description), "MessageBox");
}

inline Description<Overlay> Access::notificationControl(Description<Overlay> description) {
  if (description.state_) {
    description.state_->semanticConstruction = std::make_shared<SemanticConstruction>();
    description.state_->semanticConstruction->kind = ControlFactoryKind::NOTIFICATION_BINDING;
  }

  return configuredControl(std::move(description), "Notifications");
}

template <class T>
std::expected<void, ManagedError> Access::messageSource(Description<T> &description, const ControlSettings &settings,
                                                        ControlPropertyPresence materialized, ControlFactoryKind kind) {
  if (!description.state_ || !description.state_->semanticConstruction ||
      (description.state_->semanticConstruction->kind != kind &&
       !(kind == ControlFactoryKind::THEME_SELECTOR_BINDING && themeCatalogRecipient(description)))) {
    return std::unexpected(controlContextError({ControlContextErrorCode::INVALID_VALUE, {}}));
  }

  auto &construction = *description.state_->semanticConstruction;

  construction.source = settings;
  construction.materialized = materialized;
  construction.prepared = true;

  return {};
}

template <class T>
std::expected<void, ManagedError> Access::messageBody(Description<T> &description, Description<Widget> body) {
  if (!description.state_ || !description.state_->semanticConstruction ||
      description.state_->semanticConstruction->kind != ControlFactoryKind::MESSAGE_BINDING) {
    return std::unexpected(error({}, TreeError::INVALID_ARGUMENT));
  }

  description.state_->semanticConstruction->body = std::make_shared<Description<Widget>>(std::move(body));

  return {};
}

inline Description<Dialog> Access::internalDialog(Description<Dialog> description, const TextValue *prechargedTitle) {
  // Only this composite's already checked options bypass ambient Dialog defaults; children still inherit normally.
  if (prechargedTitle) {
    ControlSettings settings;

    settings.title = *prechargedTitle;
    preparedControlPresence(description, controlSettingsPresenceSet(settings));
    (void)std::move(description).settings(std::move(settings));
  }

  return configuredControl(std::move(description), "Dialog", false);
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

inline Description<Widget> Access::forwardOuter(Description<Widget> description) {
  if (description.state_) {
    description.state_->forwardsOuter = true;
  }

  return description;
}

template <class T> Description<Widget> Access::erase(Description<T> description) {
  Description<Widget> erased;

  erased.state_ = std::make_unique<Description<Widget>::State>();
  erased.state_->forwardsOuter = true;
  if (description.state_) {
    erased.state_->layout = description.state_->layout;
    erased.state_->semanticConstruction = description.state_->semanticConstruction;
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

} // namespace portals::ui
