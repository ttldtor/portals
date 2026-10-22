// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/chart.hpp>
#include <portals/data_presentation.hpp>
#include <portals/dialog.hpp>
#include <portals/grid.hpp>
#include <portals/line_edit.hpp>
#include <portals/message_box.hpp>
#include <portals/password.hpp>
#include <portals/primitives.hpp>
#include <portals/scroll_options.hpp>
#include <portals/text_value.hpp>
#include <portals/theme_selector.hpp>
#include <portals/toast.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace portals::ui {
namespace detail {
struct ControlContextAccess;

/** @brief Terminal factory provenance retained through named aliases. */
enum class ControlFactoryKind {
  USER,                      ///< Explicit caller schema and registry binding.
  EXISTING_BUILTIN,          ///< One of the seven existing global controls.
  DECLARATIVE_GRID_FALLBACK, ///< Loader-local standard Grid selected after caller declarations.
  PRIMITIVE_FALLBACK,        ///< Scoped native Border/Gauge selected only when callers have not declared the name.
  SCROLL_FALLBACK,           ///< Scoped native ScrollArea/ScrollBar, preserving caller declarations and writer roles.
  OVERLAY_FALLBACK,          ///< Scoped ordinary Overlay, preserving caller declarations and OverlayHost policy.
  KEY_HELP_FALLBACK,         ///< Scoped native HelpView/KeyHints, preserving caller declarations.
  VIRTUAL_VIEW_FALLBACK,  ///< Scoped native provider-bound list/table/chart schema; no resource provider is supplied.
  DATA_STATE_FALLBACK,    ///< Scoped native one-body DataStateView; explicit READY with generation1.
  DIALOG_FALLBACK,        ///< Scoped native OverlayHost/Dialog, preserving managed body/layer registration.
  MESSAGE_BINDING,        ///< Explicit code-bound native MessageBox semantic schema.
  NOTIFICATION_BINDING,   ///< Explicit code-bound native notifications semantic schema.
  THEME_SELECTOR_BINDING, ///< Explicit code-bound native ThemeSelector view schema.
  THEME_CATALOG_BINDING   ///< Explicit owning default-catalog constructor schema.
};
} // namespace detail

/** @brief Owning source coordinates; offsets and columns are UTF-8 bytes, lines/columns are one-based. */
struct ControlSourceOrigin {
  ~ControlSourceOrigin() = default;

  std::string sourceIdentity = std::string(0, '\0'); ///< Caller-defined file/resource identity.
  std::size_t byteOffset{};                          ///< Zero-based byte offset in that source.
  std::size_t line{1};                               ///< One-based source line.
  std::size_t column{1};                             ///< One-based byte column.
  std::shared_ptr<const std::string>
    sharedSourceIdentity{}; ///< Shared identity, preferred over sourceIdentity when present.
};

/** @brief Provenance of one sparse property; shared storage is immutable and owned. */
struct ControlPropertyOrigin {
  ~ControlPropertyOrigin() = default;

  std::string path = std::string(0, '\0');             ///< Canonical relative path, e.g. history.maxBytes.
  std::shared_ptr<const ControlSourceOrigin> source{}; ///< Original declaration location.
};

/** @brief Sparse editor history settings; explicit false and zero remain present. */
struct ControlHistorySettings {
  std::optional<bool> enabled{};              ///< Whether this instance retains undo history.
  std::optional<std::size_t> maxOperations{}; ///< Maximum retained operations; zero disables retention.
  std::optional<std::size_t> maxBytes{};      ///< Maximum retained payload bytes; zero disables retention.
  std::optional<bool> groupTyping{};          ///< Group adjacent ordinary typing operations.
  std::optional<bool> groupDeletion{};        ///< Group adjacent ordinary deletion operations.
};

/** @brief Sparse container literals; padding is shared by Grid/VBox/HBox/Overlay, other fields are Grid-only. */
struct ControlGridSettings {
  std::optional<std::size_t> columns{};        ///< Positive horizontal track count.
  std::optional<std::size_t> rows{};           ///< Positive vertical track count.
  std::optional<GridFlow> flow{};              ///< Automatic placement order.
  std::optional<std::int32_t> columnGap{};     ///< Nonnegative horizontal cell gap.
  std::optional<std::int32_t> rowGap{};        ///< Nonnegative vertical cell gap.
  std::optional<std::int32_t> paddingLeft{};   ///< Canonical padding.left; nonnegative Grid/VBox/HBox/Overlay inset.
  std::optional<std::int32_t> paddingTop{};    ///< Canonical padding.top; nonnegative Grid/VBox/HBox/Overlay inset.
  std::optional<std::int32_t> paddingRight{};  ///< Canonical padding.right; nonnegative Grid/VBox/HBox/Overlay inset.
  std::optional<std::int32_t> paddingBottom{}; ///< Canonical padding.bottom; nonnegative Grid/VBox/HBox/Overlay inset.
  std::optional<std::size_t> maxTracksPerAxis{}; ///< Positive track budget.
  std::optional<std::size_t> maxPlacements{};    ///< Positive structural child budget.
  std::optional<std::size_t> maxPlacementWork{}; ///< Positive occupancy work budget.
  std::optional<bool> scroll{};                  ///< Construct with Grid scrolling; explicit false overrides defaults.
};

/** @brief Sparse VBox/HBox alignment and overflow options; canonical padding lives in ControlGridSettings. */
struct ControlLinearSettings {
  std::optional<LayoutAlign> mainAlign{};   ///< Group alignment; STRETCH acts as START on the main axis.
  std::optional<LayoutAlign> crossAlign{};  ///< Child alignment across the container's main axis.
  std::optional<LayoutOverflow> overflow{}; ///< Shortage policy; only CLIP is supported.
};

/** @brief Sparse LineEdit text bounds; explicit zero retains the low-level TextLimits semantics. */
struct ControlTextLimitSettings {
  std::optional<std::size_t> maxInputBytes{};   ///< Maximum UTF-8 input bytes, including initial text and placeholder.
  std::optional<std::size_t> maxClusterBytes{}; ///< Maximum UTF-8 bytes in one grapheme cluster.
  std::optional<std::size_t> maxGraphemes{};    ///< Maximum number of grapheme clusters.
  std::optional<std::size_t> maxCells{};        ///< Maximum display width in terminal cells.
};

/** @brief Sparse owning custom frame source; omitted glyphs preserve the receiving source. */
struct ControlFrameGlyphSettings {
  /** @brief Keep owning transfers fallible on checked standard-library implementations. */
  ~ControlFrameGlyphSettings() = default;

  std::optional<bool> enabled{};            ///< True creates a source; false removes it.
  std::optional<std::string> topLeft{};     ///< One-cell top-left corner, at most 64 UTF-8 bytes.
  std::optional<std::string> topRight{};    ///< One-cell top-right corner, at most 64 UTF-8 bytes.
  std::optional<std::string> bottomLeft{};  ///< One-cell bottom-left corner, at most 64 UTF-8 bytes.
  std::optional<std::string> bottomRight{}; ///< One-cell bottom-right corner, at most 64 UTF-8 bytes.
  std::optional<std::string> horizontal{};  ///< One-cell horizontal edge, at most 64 UTF-8 bytes.
  std::optional<std::string> vertical{};    ///< One-cell vertical edge, at most 64 UTF-8 bytes.
};

/** @brief Sparse frame settings; sized controls gain a Border, ScrollArea uses its native frame. */
struct ControlScrollFrameSettings {
  /** @brief Keep owning transfers fallible; copies are translated at the existing checked boundary. */
  ~ControlScrollFrameSettings() = default;

  std::optional<bool> enabled{}; ///< Explicit true creates missing default FrameOptions; false removes the frame.
  std::optional<PrimitiveGlyphs> glyphs{};    ///< Glyph selection; requires an effective enabled frame.
  std::optional<std::size_t> maxPaintCells{}; ///< Positive existing frame paint cap; requires an effective frame.
  std::optional<FramePattern> pattern{};      ///< Built-in line pattern.

  /** @brief Sparse edge presence, preserving omitted receiving insets. */
  struct Sides {
    std::optional<bool> top{};    ///< Present top edge.
    std::optional<bool> right{};  ///< Present right edge.
    std::optional<bool> bottom{}; ///< Present bottom edge.
    std::optional<bool> left{};   ///< Present left edge.
  } sides{};                      ///< Sparse one-cell edge insets.

  std::optional<TextValue> title{};                    ///< Owning title expression, resolved before construction.
  std::optional<FrameTitleAlignment> titleAlignment{}; ///< Alignment of the fitting title prefix.
  std::optional<std::size_t> maxTitleBytes{};          ///< Positive complete title byte limit; default 4096.
  ControlFrameGlyphSettings customGlyphs{};            ///< Optional owning six-glyph source.
};

/** @brief Sparse owning literal formatter settings; omitted strings preserve the receiving options. */
struct ControlKeyFormatSettings {
  ~ControlKeyFormatSettings() = default;

  std::optional<std::size_t> maxBytesPerBinding{};   ///< Positive whole formatted row cap.
  std::optional<std::string> strokeSeparator{};      ///< Literal text between sequence strokes.
  std::optional<std::string> alternativeSeparator{}; ///< Literal text between alternatives.
  std::optional<std::string> repeatMarker{};         ///< Literal repeat-phase marker.
  std::optional<std::string> releaseMarker{};        ///< Literal release-phase marker.
  std::optional<std::string> unknownMarker{};        ///< Literal unknown-support marker.
};

/** @brief Sparse explicit chart scale; disabling leaves inherited bounds inactive. */
struct ControlChartRangeSettings {
  std::optional<bool> enabled{};   ///< Create/preserve an explicit range, or clear it with false.
  std::optional<double> minimum{}; ///< Finite lower endpoint; requires an effective explicit range.
  std::optional<double> maximum{}; ///< Finite upper endpoint; active ranges must not be reversed.
};

/** @brief Sparse owning status expressions; unused labels remain native construction concerns. */
struct ControlDataStateLabelSettings {
  ~ControlDataStateLabelSettings() = default;

  std::optional<TextValue> loading{};    ///< Initial/range pending message.
  std::optional<TextValue> empty{};      ///< Successful empty presentation.
  std::optional<TextValue> error{};      ///< Domain failure message.
  std::optional<TextValue> offline{};    ///< Explicit offline message.
  std::optional<TextValue> refreshing{}; ///< Pending work with last-good content.
  std::optional<TextValue> retry{};      ///< Used only when status and an explicit retry action are enabled.
};

/** @brief Sparse independent panel caps; these never size the full-host Dialog root. */
struct ControlDialogSizeSettings {
  std::optional<std::int32_t> width{};  ///< Positive maximum panel width in cells.
  std::optional<std::int32_t> height{}; ///< Positive maximum panel height in cells.
};

/** @brief Sparse mandatory dialog border; independent from optional ScrollArea frames. */
struct ControlDialogFrameSettings {
  std::optional<PrimitiveGlyphs> glyphs{};    ///< Existing UNICODE/ASCII border glyph domain.
  std::optional<std::size_t> maxPaintCells{}; ///< Positive allocated opaque-panel paint cap.
};

/** @brief Sparse active-action captions; unused expressions retain native lazy resolution. */
struct ControlMessageLabelSettings {
  ~ControlMessageLabelSettings() = default;

  std::optional<TextValue> acknowledge{}; ///< Information acknowledgement.
  std::optional<TextValue> confirm{};     ///< Confirmation action.
  std::optional<TextValue> save{};        ///< Save request.
  std::optional<TextValue> discard{};     ///< Destructive discard request.
  std::optional<TextValue> cancel{};      ///< Safe cancellation action.
  std::optional<TextValue> yes{};         ///< Affirmative action.
  std::optional<TextValue> no{};          ///< Negative action.
};

/** @brief Sparse optional initial choice; a supplied value alone enables it. */
struct ControlMessageChoiceSettings {
  std::optional<bool> enabled{}; ///< False clears a captured choice; true preserves it or selects the safe choice.
  std::optional<MessageChoice> value{}; ///< Always a valid enum, including while disabled.
};

/** @brief Sparse bounded ordering of the active preset actions. */
struct ControlMessageOrderSettings {
  std::optional<MessageChoice> first{};  ///< Leading active action.
  std::optional<MessageChoice> second{}; ///< Second active action, when present.
  std::optional<MessageChoice> third{};  ///< Third active action, when present.
};

/** @brief Sparse native MessageBox options, preserving captured values and kind-specific checks. */
struct ControlMessageSettings {
  ControlMessageLabelSettings labels{};          ///< Owning active-action expressions.
  ControlMessageChoiceSettings defaultChoice{};  ///< Optional initial-choice overlay.
  std::optional<bool> allowDestructiveDefault{}; ///< Explicit permission for CONFIRM/DISCARD initial focus.
  ControlTextLimitSettings textLimits{};     ///< Positive per-expression limits, unlike low-level editor zero limits.
  std::optional<std::size_t> maxBodyLines{}; ///< Positive wrapped paragraph line cap.
  std::optional<DialogButtons> buttons{};    ///< Standard action preset.
  ControlMessageOrderSettings order{};       ///< Sparse bounded active-action permutation.
};

/** @brief Owning ThemeSelector captions, resolved only during native view construction. */
struct ControlThemeSelectorLabelSettings {
  ~ControlThemeSelectorLabelSettings() = default;

  std::optional<TextValue> apply{};               ///< Sparse native apply caption.
  std::optional<TextValue> cancel{};              ///< Sparse native cancel caption.
  std::optional<TextValue> closeKeepingPreview{}; ///< Sparse native closeKeepingPreview caption.
  std::optional<TextValue> emptyStatus{};         ///< Sparse native emptyStatus caption.
  std::optional<TextValue> errorStatus{};         ///< Sparse native errorStatus caption.
  std::optional<TextValue> conflictStatus{};      ///< Sparse native conflictStatus caption.
};

/** @brief Sparse optional opening shortcut; supplied values are always syntax checked. */
struct ControlThemeSelectorShortcutSettings {
  ~ControlThemeSelectorShortcutSettings() = default;

  std::optional<bool> enabled{};      ///< False clears; true preserves captured or enables the native default.
  std::optional<std::string> value{}; ///< Owning shortcut syntax; a value alone enables it.
};

/** @brief Sparse owning initial catalog selection; absence preserves captured input. */
struct ControlThemeCatalogInitialSettings {
  ~ControlThemeCatalogInitialSettings() = default;
  std::optional<bool> enabled;      ///< False clears the captured initial ID.
  std::optional<std::string> value; ///< Literal stable choice ID, never localized.
};

/** @brief Sparse admission budgets for the existing style-layer engine. */
struct ControlThemeCatalogLayerSettings {
  std::optional<bool> enabled;                ///< False clears explicit layer options.
  std::optional<std::size_t> maxLayers;       ///< At least two layers when supplied.
  std::optional<std::size_t> maxRules;        ///< Positive rule budget.
  std::optional<std::size_t> maxDeclarations; ///< Positive declaration budget.
  std::optional<std::size_t> maxOwnedBytes;   ///< Positive logical owned-byte budget.
};

/** @brief Constructor-only scalars for a real owning default catalog recipe. */
struct ControlThemeCatalogSettings {
  std::optional<std::size_t> maxChoices;          ///< Positive choice admission budget.
  std::optional<std::size_t> maxIdBytes;          ///< Positive ID byte budget.
  std::optional<std::size_t> maxLabelBytes;       ///< Positive label byte budget.
  std::optional<std::size_t> maxDescriptionBytes; ///< Positive description byte budget.
  std::optional<std::size_t> maxTextBytes;        ///< Positive combined text budget.
  ControlTextLimitSettings textLimits;            ///< Positive selection-model text limits.
  ControlThemeCatalogInitialSettings initialId;   ///< Optional startup preview/commit.
  ControlThemeCatalogLayerSettings layers;        ///< Optional existing engine admission options.
};

/** @brief Native ThemeSelector view settings, independent of model/catalog construction. */
struct ControlThemeSelectorSettings {
  ControlThemeSelectorLabelSettings labels{};             ///< Native action and status captions.
  ControlTextLimitSettings textLimits{};                  ///< Positive limits for each resolved view expression.
  ControlThemeSelectorShortcutSettings openingShortcut{}; ///< Optional canonical opening binding.
};

/** @brief Sparse immutable notification-center construction options; runtime posts stay code-owned. */
struct ControlNotificationSettings {
  std::optional<std::size_t> capacity{};         ///< Positive total retained-record cap.
  std::optional<std::size_t> maxVisible{};       ///< Positive selected-record cap, no greater than effective capacity.
  std::optional<ToastPlacement> placement{};     ///< Existing corner of the ordinary body viewport.
  std::optional<std::int32_t> maximumWidth{};    ///< Positive card width cap in cells.
  std::optional<std::int32_t> margin{};          ///< Nonnegative viewport inset in cells.
  std::optional<std::int32_t> gap{};             ///< Nonnegative gap between cards in cells.
  std::optional<ToastOverflowPolicy> overflow{}; ///< Existing bounded queue admission policy.
  std::optional<bool> refreshDeduplicated{};     ///< Refresh presented transient deadlines on deduplication.
  std::optional<bool> reducedMotion{};           ///< Disable alternating indicators without changing static content.
  ControlTextLimitSettings textLimits{};         ///< Positive full resolved runtime message bounds.
  std::optional<std::size_t> actionLabelBytes{}; ///< Positive runtime action-caption byte cap.
  std::optional<std::size_t> dedupKeyBytes{};    ///< Positive exact runtime dedup-key byte cap.
  std::optional<std::size_t> maxPaintCells{};    ///< Positive allocated card paint-area cap.
};

/** @brief Sparse close affordance settings. */
struct ControlDialogCloseSettings {
  std::optional<bool> enabled{};                   ///< Enable or hide close chrome.
  std::optional<DialogClosePlacement> placement{}; ///< START or END top-frame segment.
  std::optional<bool> focusable{};                 ///< Include chrome in focus traversal.
};

/** @brief Sparse native dialog geometry/input options, merged per leaf. */
struct ControlDialogSettings {
  std::optional<DialogMode> mode{};             ///< MODAL or NON_MODAL input policy.
  std::optional<DialogPlacement> placement{};   ///< CENTER/BELOW_START/ABOVE_START positioning.
  ControlDialogSizeSettings maximumPanelSize{}; ///< Independent positive panel caps.
  std::optional<std::int32_t> viewportMargin{}; ///< Nonnegative viewport inset in cells.
  std::optional<std::int32_t> anchorGap{};      ///< Nonnegative distance from the body anchor.
  std::optional<bool> closeOnEscape{};          ///< Existing managed cancellation policy.
  ControlDialogFrameSettings frame{};           ///< Mandatory one-cell opaque panel border.
  ControlDialogCloseSettings closeButton{};     ///< Opt-in top-frame close affordance.
};

/** @brief Sparse mandatory protected-editor policy; unsupported guarantees never downgrade. */
struct ControlSecureSettings {
  std::optional<std::size_t> maxBytes{};        ///< Maximum secret bytes, including explicit zero.
  std::optional<SecureStringStorage> storage{}; ///< Password accepts only NATIVE_GUARDED.
  std::optional<bool> lockPages{};              ///< Require native page locking before any secret is copied.
  std::optional<bool> excludeFromDumps{};       ///< Require native dump exclusion where supported.
  std::optional<bool> encryptWhileIdle{};       ///< Password requires true; explicit false is rejected.
};

/** @brief Owning sparse literals only; never contains IDs, models, handlers or live history. */
struct ControlSettings {
  ~ControlSettings() = default;

  std::optional<TextValue> text{};                        ///< Caption/value expression, resolved by receiving scene.
  std::optional<std::int32_t> width{};                    ///< Nonnegative fixed width in cells.
  std::optional<std::int32_t> height{};                   ///< Nonnegative fixed height in cells.
  std::optional<std::uint32_t> flex{};                    ///< Positive common flex weight.
  std::optional<std::int32_t> gap{};                      ///< Nonnegative VBox/HBox gap.
  std::optional<CheckState> state{};                      ///< CheckBox/ThreeState initial state.
  std::optional<CheckCycle> cycle{};                      ///< CheckBox cycle; ThreeState is fixed THREE_STATE.
  std::optional<EditMode> editMode{};                     ///< LineEdit publication mode.
  std::optional<bool> readOnly{};                         ///< LineEdit/scroll user-mutation policy.
  std::optional<ProgrammaticChange> programmaticChange{}; ///< LineEdit initial programmatic history policy.
  std::optional<LineEditDisplayMode> displayMode{};       ///< LineEdit immutable presentation.
  std::optional<TextValue> placeholder{};                 ///< LineEdit placeholder expression.
  ControlHistorySettings history{};                       ///< Structural merge, never a whole-history reset.
  std::shared_ptr<const ControlSourceOrigin> origin{};    ///< Fallback location for present properties.
  std::vector<ControlPropertyOrigin> origins = std::vector<ControlPropertyOrigin>(0); ///< Per-property overrides.
  ControlGridSettings grid{};                 ///< Sparse container settings, supported only by a matching schema.
  ControlLinearSettings linear{};             ///< Sparse VBox/HBox alignment and overflow settings.
  ControlTextLimitSettings limits{};          ///< Sparse LineEdit limits, merged one bound at a time.
  std::optional<ButtonChrome> chrome{};       ///< Button decoration; absence preserves the receiving factory's chrome.
  std::optional<PrimitiveGlyphs> glyphs{};    ///< Border/Gauge/scroll one-cell decoration selection.
  std::optional<std::size_t> maxPaintCells{}; ///< Positive Border/Gauge/scroll paint-work cap in allocated cells.
  std::optional<double> value{};   ///< Finite Gauge initial value, checked against its effective range at construction.
  std::optional<double> minimum{}; ///< Finite Gauge lower bound; effective maximum must be greater.
  std::optional<double> maximum{}; ///< Finite Gauge upper bound with a finite positive span.
  std::optional<std::int32_t> preferredWidth{}; ///< Nonnegative Gauge intrinsic width in terminal cells.
  std::optional<double> wheelStep{};            ///< Positive finite logical scroll cells per decoded wheel event.
  std::optional<std::uint64_t> keyStep{}; ///< Positive integral logical cells per arrow event, retaining all 64 bits.
  std::optional<std::uint64_t> pageOverlap{}; ///< Shared logical cells between pages; explicit zero is supported.
  std::optional<std::int32_t> minimumThumb{}; ///< Positive minimum thumb cells, capped by the actual track.
  std::optional<bool> focusable{}; ///< Whether the owning area/standalone bar participates in focus traversal.
  std::optional<ScrollBarPolicy> horizontal{}; ///< ScrollArea horizontal bar presence policy.
  std::optional<ScrollBarPolicy> vertical{};   ///< ScrollArea vertical bar presence policy.
  std::optional<ScrollPlacement> placement{};  ///< ScrollArea gutter/overlay/frame relationship.
  std::optional<ScrollAxis> axis{};            ///< ScrollBar projection axis, applied without changing its writer role.
  ControlScrollFrameSettings frame{};          ///< Sparse optional frame configuration for sized controls.
  std::optional<LayoutAlign> horizontalAlign{}; ///< Ordinary Overlay child alignment across columns.
  std::optional<LayoutAlign> verticalAlign{};   ///< Ordinary Overlay child alignment across rows.
  std::optional<bool> initiallyOpen{};    ///< HelpView initial open state, using its existing focus-return lifecycle.
  ControlKeyFormatSettings format{};      ///< Sparse literal formatter shared by HelpView and KeyHints.
  std::optional<std::string> emptyText{}; ///< Literal empty-projection presentation.
  std::optional<std::string> unavailableText{};  ///< Literal failed-projection presentation.
  std::optional<std::string> entrySeparator{};   ///< KeyHints separator between complete entries.
  std::optional<std::string> overflowMarker{};   ///< Nonempty KeyHints overflow marker.
  std::optional<std::int32_t> rowHeight{};       ///< Positive VirtualList/VirtualTable cell rows per logical row.
  std::optional<std::size_t> overscanRows{};     ///< Additional requested rows per side; explicit zero is supported.
  std::optional<std::size_t> maxColumns{};       ///< Positive VirtualTable configured-column cap.
  std::optional<std::size_t> maxPreparedCells{}; ///< Positive VirtualTable visible body-cell cap.
  std::optional<std::size_t> maxPreparedTextBytes{}; ///< Positive VirtualTable header/body UTF-8 cap.
  std::optional<ChartMode> mode{};                   ///< Chart line/candle presentation.
  std::optional<ChartGlyphSet> chartGlyphSet{};      ///< Chart-only candle glyph domain.
  std::optional<std::int32_t> candleGap{};           ///< Nonnegative empty columns between candles.
  ControlChartRangeSettings yRange{};                ///< Sparse optional chart scale, merged per leaf.
  std::optional<bool> showStatus{};                  ///< Native status subtree visibility at construction.
  std::optional<std::int32_t> statusRows{};          ///< Positive fixed status strip height in cell rows.
  std::optional<DataContentPolicy> contentPolicy{};  ///< Existing KEEP_BODY/REPLACE behavior.
  std::optional<SchedulerTime> spinnerInterval{};    ///< Positive uint64 monotonic milliseconds.
  ControlDataStateLabelSettings labels{};            ///< Owning native expressions, merged one label at a time.
  std::optional<std::size_t> maxLayers{}; ///< Positive OverlayHost layer-registration cap, including closed layers.
  std::optional<TextValue> title{};       ///< Owning Dialog caption expression; unspecified keeps the actual caption.
  ControlDialogSettings dialog{};         ///< Native Dialog-only options, merged per leaf.
  std::optional<TextValue> body{};        ///< Owning native MessageBox paragraph expression.
  ControlMessageSettings message{};       ///< Native MessageBox-only sparse options.
  ControlNotificationSettings notification{};   ///< Explicit code-bound native notifications options.
  ControlThemeSelectorSettings themeSelector{}; ///< Explicit code-bound native selector view settings.
  ControlThemeCatalogSettings themeCatalog{};   ///< Constructor-only real owning catalog scalars.
  ControlSecureSettings securePolicy{}; ///< Password native security policy; appended for aggregate compatibility.
  std::optional<PasswordPastePolicy> pastePolicy{}; ///< Password explicit inbound paste admission.
};

/** @brief Finite context bounds, checked before publication/materialization beyond each bound. */
struct ControlContextLimits {
  std::size_t maxDefinitions{64};           ///< User definitions; builtin types do not consume this budget.
  std::size_t maxInheritance{32};           ///< Ancestor identities, excluding the requested type; at most 32.
  std::size_t maxStructDepth{8};            ///< Property depth: scalar one, history/padding/limits member two.
  std::size_t maxMaterializedLeaves{65536}; ///< Leaves per resolve; mount must also aggregate across all nodes.
};

/** @brief Configuration-only failure, before resolvers, factories or mounting. */
enum class ControlContextErrorCode {
  NO_MEMORY,               ///< Allocation failed; diagnostic storage may be absent.
  CONFIGURATION_FAILED,    ///< Unexpected internal preparation exception.
  INVALID_LIMIT,           ///< At least one finite bound is zero.
  INVALID_NAME,            ///< Type name is not an ASCII identifier.
  DUPLICATE_TYPE,          ///< A type definition/schema already exists.
  BUILTIN_SHADOW,          ///< A user type attempts to replace a builtin.
  UNKNOWN_TYPE,            ///< A requested type or definition base is missing.
  INHERITANCE_CYCLE,       ///< Definition bases contain a cycle.
  INHERITANCE_LIMIT,       ///< Effective ancestor count exceeds its bound.
  DEFINITION_LIMIT,        ///< User definitions and custom schemas exceed their shared bound.
  STRUCT_DEPTH_LIMIT,      ///< A present property's structural depth exceeds its bound.
  MATERIALIZED_LEAF_LIMIT, ///< Effective sparse scalar leaf count exceeds its bound.
  UNKNOWN_PROPERTY,        ///< A schema/origin path is outside the canonical vocabulary.
  DUPLICATE_PROPERTY,      ///< Duplicate defaults block, schema path or property provenance.
  UNSUPPORTED_PROPERTY,    ///< Known property is not supported by the concrete base.
  INVALID_VALUE,           ///< Scalar range or enum is invalid.
  FIXED_CYCLE              ///< ThreeState lineage attempts a cycle other than THREE_STATE.
};

/** @brief Independently owned diagnostic; copying errors never copies strings. */
struct ControlContextDiagnostic {
  ~ControlContextDiagnostic() = default;

  std::string path = std::string(0, '\0');             ///< Fully qualified type.property, or offending type.
  std::shared_ptr<const ControlSourceOrigin> origin{}; ///< Original inherited declaration, when supplied.
};

/** @brief Allocation-free typed fallback remains usable if diagnostic construction fails. */
struct ControlContextError {
  ControlContextErrorCode code{ControlContextErrorCode::CONFIGURATION_FAILED}; ///< Typed failure.
  std::shared_ptr<const ControlContextDiagnostic> diagnostic{};                ///< Optional owning details.
};

/** @brief Scalar schema kind, shared by C++ validation and resource decoding. */
enum class ControlPropertyKind {
  TEXT,             ///< Owning TextValue expression.
  BOOLEAN,          ///< Explicit true or false.
  SIGNED_INTEGER,   ///< Signed cell extent.
  UNSIGNED_INTEGER, ///< Nonnegative capacity or positive weight.
  ENUMERATION,      ///< One of the descriptor's symbolic enum names.
  FLOATING_POINT,   ///< Finite decimal/exponent scalar, without expressions or units.
  LITERAL_STRING    ///< Owning UTF-8 string; never evaluated or resolved as TextValue.
};

/** @brief Static canonical property descriptor; returned views have process lifetime. */
struct ControlPropertyDescriptor {
  std::string_view path;                       ///< Canonical scalar path, including nested history members.
  ControlPropertyKind kind;                    ///< Literal kind; enum names follow the existing public enumerators.
  std::size_t depth;                           ///< Structural path depth.
  std::span<const std::string_view> enumNames; ///< Allowed symbolic enum names, empty for other kinds.
  /** @brief Accepted alternative spellings; canonical schema paths remain unchanged. */
  std::span<const std::string_view> aliases{};
};

/** @brief Return a static concrete factory schema; unknown types have an empty schema.
 * @param builtin VBox/HBox/Label/Button/CheckBox/LineEdit/ThreeState or scoped
 * Border/Gauge/ScrollArea/ScrollBar/Overlay.
 * @return Static descriptors only; named definitions are resolved to their concrete base first.
 * Returning a native primitive schema does not register or reserve that name in a context.
 */
std::span<const ControlPropertyDescriptor> controlPropertySchema(std::string_view builtin) noexcept;

/** @brief Owning effective sparse configuration and type identity, with no scene-affine state. */
struct ResolvedControl {
  ~ResolvedControl() = default;

  std::string type = std::string(0, '\0'); ///< Immediate requested named/builtin type.
  std::string base = std::string(0, '\0'); ///< Concrete builtin factory type, preserving ThreeState.
  std::vector<std::string> lineage = std::vector<std::string>(0); ///< Immediate-to-distant ancestors; excludes type.
  ControlSettings settings;                                       ///< Merged sparse settings with winning origins.
  std::size_t materializedLeaves{}; ///< Present scalar leaves; caller aggregates per mount.
  detail::ControlFactoryKind factoryKind{detail::ControlFactoryKind::USER}; ///< Actual terminal schema provenance.
};

/** @brief Cheap owning immutable snapshot of named definitions and defaults.
 * Const snapshot reads are concurrent-safe. Mutating the same handle requires external synchronization.
 * Copies preserve the snapshot and sticky error; later fluent mutations publish a new version only.
 * Construction/copy of caller arguments may throw; fluent operations catch internal allocation failures.
 * No callback, resolver, factory, locale, runtime or mutable cache is retained or invoked.
 */
class ControlContext {
public:
  /** @param limits Positive finite limits; invalid limits become a sticky configuration error. */
  explicit ControlContext(ControlContextLimits limits = {}) noexcept;

  /** @brief Register a callback-free custom factory schema from canonical scalar property paths.
   * @param name Unique non-builtin type identity, shared with the caller's explicit factory allowlist.
   * @param properties Allowed paths from the builtin descriptor vocabulary; copied now, no borrowed span survives.
   * @return This same handle; custom types and named definitions share maxDefinitions. Unknown paths fail.
   */
  ControlContext &registerType(std::string_view name, std::span<const std::string_view> properties) & noexcept;

  /** @param name Custom schema name. @param properties Copied canonical paths. @return This temporary handle. */
  ControlContext &&registerType(std::string_view name, std::span<const std::string_view> properties) && noexcept {
    registerType(name, properties);

    return std::move(*this);
  }

  /** @brief Add one unique named definition; base may be declared later.
   * @param name Case-sensitive ASCII identifier, excluding builtin names.
   * @param base One builtin or named definition; checked by validate/resolve.
   * @param settings Owning sparse initial settings copied during this call.
   * @return This same handle. The first typed failure stops later fluent mutations.
   */
  ControlContext &define(std::string_view name, std::string_view base, const ControlSettings &settings = {}) & noexcept;

  /** @param name Variant name. @param base Ancestor identity. @param settings Sparse patch. @return This temporary. */
  ControlContext &&define(std::string_view name, std::string_view base,
                          const ControlSettings &settings = {}) && noexcept {
    define(name, base, settings);

    return std::move(*this);
  }

  /** @brief Register the sole defaults block for one builtin/named type.
   * @param type Case-sensitive type identity; forward definitions are permitted.
   * @param settings Sparse patch; a second defaults block for this type is a typed duplicate.
   * @return This same handle; internal exceptions become a sticky typed error.
   */
  ControlContext &defaults(std::string_view type, const ControlSettings &settings) & noexcept;

  /** @param type Defaults target. @param settings Sparse patch. @return This temporary handle. */
  ControlContext &&defaults(std::string_view type, const ControlSettings &settings) && noexcept {
    defaults(type, settings);

    return std::move(*this);
  }

  /** @brief Apply sparse defaults in a nested configuration scope without changing earlier snapshots.
   * @param type Existing or forward-declared type receiving local defaults.
   * @param settings Present leaves override inherited defaults, including false, zero and their provenance.
   * @return This handle with a new immutable version; a resource loader separately rejects duplicate local blocks.
   */
  ControlContext &overlayDefaults(std::string_view type, const ControlSettings &settings) & noexcept;

  /** @param type Local defaults target. @param settings Sparse patch. @return This temporary handle. */
  ControlContext &&overlayDefaults(std::string_view type, const ControlSettings &settings) && noexcept {
    overlayDefaults(type, settings);

    return std::move(*this);
  }

  /** @brief Validate every definition/default before any application effect.
   * @return First sticky, graph, schema or bounded-materialization failure, otherwise success.
   */
  std::expected<void, ControlContextError> validate() const noexcept;

  /** @brief Validate once and return an immutable version with reusable graph qualification.
   * @return Checked snapshot; resolve still validates each instance patch. Later fluent mutation
   * invalidates qualification only on the mutated handle, leaving earlier copies unchanged.
   */
  std::expected<ControlContext, ControlContextError> prepared() const noexcept;

  /** @return Whether this is the unconfigured context with ordinary default budgets. */
  bool empty() const noexcept;

  /** @param name Exact custom schema name, excluding builtins and named definitions.
   * @return Whether this snapshot contains the explicit callback-free schema.
   */
  bool hasRegisteredType(std::string_view name) const noexcept;

  /** @param name Exact custom schema name.
   * @param properties Canonical allowed paths, compared as an unordered set without allocation.
   * @return Exact schema equality; false for missing types, unknown paths or duplicate paths.
   */
  bool matchesRegisteredType(std::string_view name, std::span<const std::string_view> properties) const noexcept;

  /** @brief Validate the whole snapshot, then resolve builtin/defaults/definition/defaults/instance precedence.
   * @param name Requested builtin or named type.
   * @param instancePatch Final sparse overrides, copied without resolving TextValue expressions.
   * @return Owning effective sparse settings or a typed failure with original declaration provenance.
   */
  std::expected<ResolvedControl, ControlContextError> resolve(std::string_view name,
                                                              const ControlSettings &instancePatch = {}) const noexcept;

  /** @return Immutable bounds captured by this snapshot. */
  ControlContextLimits limits() const noexcept;

private:
  friend struct detail::ControlContextAccess;
  struct Data;
  std::shared_ptr<const Data> data_{};
  ControlContextLimits limits_;
  std::optional<ControlContextError> failure_{};
  bool prepared_{};
};

} // namespace portals::ui
