// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed.hpp>
#include <portals/ui_control_context.hpp>
#include <portals/ui_ref.hpp>

namespace portals::ui::detail {

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
  /** @param context Effective loader snapshot; caller-owned snapshots remain unchanged.
   * @return Success after installing the single Grid fallback, or the original typed error.
   */
  static std::expected<void, ControlContextError> installFallbackSchema(ControlContext &context) noexcept;
  /** @return Static Grid-only descriptors; this does not make Grid a global builtin. */
  static std::span<const ControlPropertyDescriptor> gridSchema() noexcept;
};

/** @param settings Sparse configuration already counted by the resource preparation pass.
 * @return Allocation-free canonical leaf presence; used to count a factory's additional overrides once.
 */
std::uint32_t controlSettingsPresence(const ControlSettings &settings) noexcept;

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
