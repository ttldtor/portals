// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/key_hints.hpp>
#include <portals/managed.hpp>
#include <portals/ui_control_context.hpp>
#include <portals/ui_ref.hpp>

#include <bitset>

namespace portals::ui::detail {

inline constexpr std::size_t CONTROL_PROPERTY_COUNT = 180; ///< Complete canonical scalar vocabulary size.
inline constexpr std::size_t CONTROL_PRESENCE_BITS =
  CONTROL_PROPERTY_COUNT < 32 ? 32 : CONTROL_PROPERTY_COUNT;        ///< Retain all legacy mask bits.
using ControlPropertyPresence = std::bitset<CONTROL_PRESENCE_BITS>; ///< Allocation-free internal leaf presence.

/** @brief Loader-only bounded schema installation in an effective context copy. */
struct ControlContextAccess {
  /** @param context Immutable snapshot borrowed for this check.
   * @param name Exact schema or definition name.
   * @return Whether a user declaration owns the name, without resolving its base.
   */
  static bool hasDeclaredType(const ControlContext &context, std::string_view name) noexcept;
  /** @param context Immutable snapshot borrowed for the legacy loader's mode selection.
   * @return Whether definitions, defaults, schemas, nondefault limits or a sticky failure require typed
   * validation. The single loader-internal fallback is excluded; public empty() keeps its meaning.
   */
  static bool hasUserConfiguration(const ControlContext &context) noexcept;
  /** @param property Exact case-sensitive canonical property path or alias.
   * @return Static canonical path for known names, or the borrowed input unchanged.
   */
  static std::string_view canonicalProperty(std::string_view property) noexcept;
  /** @param context Effective loader snapshot; caller-owned snapshots remain unchanged.
   * @return Success after installing the single Grid fallback, or the original typed error.
   */
  static std::expected<void, ControlContextError> installFallbackSchema(ControlContext &context) noexcept;
  /** @param context Effective native/loader snapshot; caller declarations take precedence.
   * @param name Exactly Border or Gauge; this does not reserve a process-global control name.
   * @return Success after installing the scoped fallback, or the original typed preparation error.
   */
  static std::expected<void, ControlContextError> installPrimitiveSchema(ControlContext &context,
                                                                         std::string_view name) noexcept;
  /** @param context Effective native/loader snapshot; caller declarations retain their priority.
   * @param name Exactly ScrollArea or ScrollBar; neither name is globally reserved.
   * @return Success after installing the scoped schema, or the original typed preparation error.
   */
  static std::expected<void, ControlContextError> installScrollSchema(ControlContext &context,
                                                                      std::string_view name) noexcept;
  /** @param context Effective native/loader snapshot; caller Overlay declarations retain priority.
   * @return Success after installing the scoped ordinary Overlay schema, or the original typed error.
   */
  static std::expected<void, ControlContextError> installOverlaySchema(ControlContext &context) noexcept;
  /** @param context Effective native/loader snapshot; explicit caller declarations retain priority.
   * @param name Exactly HelpView or KeyHints; neither name is globally reserved.
   * @return Success after installing the scoped native schema, or the original typed error.
   */
  static std::expected<void, ControlContextError> installKeyHelpSchema(ControlContext &context,
                                                                       std::string_view name) noexcept;
  /** @param context Effective native context; explicit caller declarations retain priority.
   * @param name Exactly VirtualList, VirtualTable or Chart; this never installs a model/resource factory.
   * @return Success after installing the scoped scalar schema, or the original typed failure.
   */
  static std::expected<void, ControlContextError> installVirtualViewSchema(ControlContext &context,
                                                                           std::string_view name) noexcept;
  /** @param context Effective native/loader snapshot; caller declarations retain priority.
   * @return Success after installing scoped DataStateView settings, or the original typed error.
   */
  static std::expected<void, ControlContextError> installDataStateSchema(ControlContext &context) noexcept;
  /** @param context Effective native snapshot; explicit caller names retain priority.
   * @return Scoped code-bound Notifications schema, without a resource factory or runtime resolver.
   */
  static std::expected<void, ControlContextError> installNotificationSchema(ControlContext &context) noexcept;

  /** @param context Effective native snapshot; caller declarations retain priority.
   * @return Scoped ThemeSelector view schema without creating a model or catalog.
   */
  static std::expected<void, ControlContextError> installThemeSelectorSchema(ControlContext &context) noexcept;

  /** @param context Effective context; caller declarations retain priority.
   * @param recipient Enable the private catalog-to-view ancestry only for an actual owning recipe.
   * @return Scoped canonical catalog schema, or the original typed failure.
   */
  static std::expected<void, ControlContextError> installThemeCatalogSchema(ControlContext &context,
                                                                            bool recipient = false) noexcept;
  /** @param context Effective registry context, never the caller's immutable snapshot.
   * @param name Exact checked semantic registration name.
   * @param kind View or catalog capability selected by canonical metadata.
   * @return Success after recording the finite family, or a typed allocation failure.
   */
  static std::expected<void, ControlContextError> themeFamilySchema(ControlContext &context, std::string_view name,
                                                                    ControlFactoryKind kind) noexcept;

  /** @param context Effective owning defaults snapshot receiving the native message fallback.
   * @return Scoped canonical schema, or the original typed configuration/allocation failure.
   */
  static std::expected<void, ControlContextError> installMessageSchema(ControlContext &context) noexcept;

  /** @param context Effective native/loader snapshot; explicit caller names retain priority.
   * @param name Exact OverlayHost or Dialog schema name.
   * @return Scoped native schema or the original typed configuration failure.
   */
  static std::expected<void, ControlContextError> installDialogSchema(ControlContext &context,
                                                                      std::string_view name) noexcept;
  /** @return Static Grid-only descriptors; this does not make Grid a global builtin. */
  static std::span<const ControlPropertyDescriptor> gridSchema() noexcept;
};

/** @param settings Sparse configuration already counted by the resource preparation pass.
 * @return Safe low 32-bit compatibility view; higher canonical leaves are omitted.
 */
std::uint32_t controlSettingsPresence(const ControlSettings &settings) noexcept;

/** @param settings Sparse configuration already counted by the resource preparation pass.
 * @return Complete allocation-free canonical presence for internal leaf accounting.
 */
ControlPropertyPresence controlSettingsPresenceSet(const ControlSettings &settings) noexcept;

/** @brief Overlay only present linear settings, including the shared canonical padding fields.
 * @param settings Validated sparse settings for a VBox/HBox concrete schema.
 * @param options Existing options whose absent leaves remain unchanged.
 * @return Owning effective options; no callback or retained borrow is introduced.
 */
LinearOptions controlLinearOptions(const ControlSettings &settings, LinearOptions options) noexcept;

/** @param settings Sparse ordinary Overlay literals with shared canonical padding.
 * @param options Actual receiving constructor options; absent leaves retain their values.
 * @return Effective options, or INVALID_ARGUMENT for negative padding/unknown alignment without side effects.
 */
std::expected<OverlayOptions, TreeError> controlOverlayOptions(const ControlSettings &settings,
                                                               OverlayOptions options) noexcept;

/** @brief Overlay only present LineEdit text bounds, preserving explicit zero.
 * @param settings Validated sparse settings for a LineEdit concrete schema.
 * @param limits Existing text limits whose absent leaves remain unchanged.
 * @return Owning effective text limits.
 */
TextLimits controlTextLimits(const ControlSettings &settings, TextLimits limits) noexcept;

/** @param settings Sparse frame patch; omitted leaves preserve the receiving values.
 * @param base Owning effective frame, or empty for a control without a frame.
 * @param policy Complete width policy used to resolve the owning title.
 * @return Effective optional frame, or typed structural/text/allocation failure. No runtime state is created.
 */
std::expected<std::optional<FrameOptions>, ManagedError>
controlFrameOptions(const ControlSettings &settings, std::optional<FrameOptions> base, WidthPolicy policy);

/** @param settings Validated sparse Border settings.
 * @param options Actual receiving constructor options; absent leaves retain these values.
 * @return Owning effective options, checked by Border::validate before construction.
 */
BorderOptions controlBorderOptions(const ControlSettings &settings, BorderOptions options) noexcept;

/** @param settings Validated sparse Gauge settings.
 * @param options Actual receiving constructor options; absent leaves retain these values.
 * @return Owning effective options, checked together with the effective value by Gauge::validate.
 */
GaugeOptions controlGaugeOptions(const ControlSettings &settings, GaugeOptions options) noexcept;

/** @param settings Validated sparse area literals, including optional one-cell frame leaves.
 * @param options Actual receiving constructor options; absent leaves retain their values.
 * @return Effective options checked by validateScrollOptions, or INVALID_ARGUMENT without a controller effect.
 */
std::expected<ScrollAreaOptions, TreeError> controlScrollAreaOptions(const ControlSettings &settings,
                                                                     ScrollAreaOptions options) noexcept;

/** @param settings Validated sparse standalone/projection bar literals.
 * @param options Actual receiving constructor options; absent leaves retain their values.
 * @return Effective options checked by validateScrollOptions; writer role and controller are not modified.
 */
std::expected<ScrollBarOptions, TreeError> controlScrollBarOptions(const ControlSettings &settings,
                                                                   ScrollBarOptions options) noexcept;

/** @param value Borrowed literal bytes, never normalized or retained.
 * @return Whether the complete value is valid UTF-8, without imposing a presentation byte cap.
 */
bool validControlLiteral(std::string_view value) noexcept;

/** @param settings Validated sparse native help literals.
 * @param options Actual receiving options; absent leaves retain these values.
 * @return Owning effective HelpView options; string transfers may throw before retained allocation.
 */
HelpViewOptions controlHelpViewOptions(const ControlSettings &settings, HelpViewOptions options);

/** @param settings Validated sparse native hints literals.
 * @param options Actual receiving options; absent leaves retain these values.
 * @return Owning effective KeyHints options; string transfers may throw before retained allocation.
 */
KeyHintsOptions controlKeyHintsOptions(const ControlSettings &settings, KeyHintsOptions options);

/** @param settings Validated sparse list literals.
 * @param options Actual receiving options, including width policy and styles.
 * @return Effective options; raw option validation stays in native preparation.
 */
VirtualListOptions controlVirtualListOptions(const ControlSettings &settings, VirtualListOptions options) noexcept;

/** @param settings Validated sparse table literals.
 * @param options Actual receiving options, including width policy and styles.
 * @return Effective options; model/column-dependent validation retains its native phase.
 */
VirtualTableOptions controlVirtualTableOptions(const ControlSettings &settings, VirtualTableOptions options) noexcept;

/** @param settings Sparse chart literals; absent leaves retain the actual receiving options.
 * @param options Actual native options, including optional scale, styles and width policy.
 * @return Effective options, or INVALID_ARGUMENT for a patched range missing its prerequisite or reversed.
 * Raw options without chart patches retain their native preparation validation phase.
 */
std::expected<ChartOptions, TreeError> controlChartOptions(const ControlSettings &settings,
                                                           ChartOptions options) noexcept;

/** @param settings Sparse DataStateView settings; label expressions remain unresolved.
 * @param config Actual receiving config, including resolver/retry/styles and layout policy.
 * @return Owning effective config; native construction retains scalar validation and unused-label gates.
 */
DataStateConfig controlDataStateConfig(const ControlSettings &settings, DataStateConfig config);

/** @param settings Sparse host layer cap. @param options Actual receiving host options.
 * @return Native options with only the present cap changed; validation stays native.
 */
OverlayHostOptions controlOverlayHostOptions(const ControlSettings &settings, OverlayHostOptions options) noexcept;

/** @param settings Sparse independent Dialog options. @param options Actual receiving native options.
 * @return Native options with present leaves overlaid; title remains an owning recipe concern.
 */
DialogOptions controlDialogOptions(const ControlSettings &settings, DialogOptions options) noexcept;

/** @param settings Sparse message literals. @param options Actual captured native options.
 * @param kind Existing native kind: information, confirmation, or unsaved changes.
 * @return Owning effective options; active labels and kind safety remain native checks.
 */
MessageOptions controlMessageOptions(const ControlSettings &settings, MessageOptions options, int kind);

/** @param settings Sparse immutable notification literals. @param options Actual captured options and own resolver.
 * @return Effective options; native construction retains queue validation and resolver ownership.
 */
NotificationOptions controlNotificationOptions(const ControlSettings &settings, NotificationOptions options);

/** @param settings Validated sparse view literals. @param options Actual native captured options.
 * @return Owning effective view options, preserving code-owned resolver/model callbacks.
 */
ThemeSelectorOptions controlThemeSelectorOptions(const ControlSettings &settings, ThemeSelectorOptions options);

/** @param settings Validated constructor-only sparse patch.
 * @param options Owning captured selection options, updated before model installation.
 * @param initialId Owning captured initial ID, updated without touching the captured catalog.
 * @return INVALID_ARGUMENT when enabled requires a missing captured ID; no model is installed.
 */
std::expected<void, TreeError> controlThemeCatalogOptions(const ControlSettings &settings,
                                                          ThemeSelectionOptions &options,
                                                          std::optional<std::string> &initialId);

/** @brief Translate an owning configuration failure at the existing managed boundary.
 * @param failure Configuration error whose optional diagnostic owns its strings and source.
 * @param node Existing parent identity; no widget is retained or accessed.
 * @return Typed mount error; diagnostic allocation failure becomes NO_MEMORY.
 */
ManagedError controlContextError(const ControlContextError &failure, Node<Widget> node = {});

/** @brief Apply present editor settings after context precedence has been resolved.
 * @param settings Effective sparse settings; text is handled by the recipe separately.
 * @param options Existing constructor options, copied inside the checked boundary.
 * @param policy Receiving scene's immutable width policy.
 * @param resolver Optional synchronous resource adapter for the placeholder only.
 * @param node Existing parent for typed failure attribution.
 * @return Owning options, or original text/resource error without creating a widget or history.
 */
std::expected<LineEditOptions, ManagedError> controlLineEditOptions(const ControlSettings &settings,
                                                                    const LineEditOptions &options, WidthPolicy policy,
                                                                    const TextResolver &resolver,
                                                                    Node<Widget> node = {});

} // namespace portals::ui::detail
