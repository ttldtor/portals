// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed.hpp>
#include <portals/ui_control_context.hpp>
#include <portals/ui_ref.hpp>

namespace portals::ui::detail {

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
