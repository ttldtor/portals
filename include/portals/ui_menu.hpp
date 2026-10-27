// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/menu.hpp>
#include <portals/ui_construction.hpp>

#include <limits>
#include <ranges>

namespace portals::ui {
namespace detail {
/// @cond INTERNAL
Description<Menu> menuRecipe(std::span<const ActionRef>, MenuOptions, ControlStyle, SizePolicy);
/// @endcond
} // namespace detail

/** @brief Describe an ordinary retained command list, initially visible.
 * @param actions Existing shared action identities, copied now; duplicates remain separate entries.
 * @param options Positive finite entry budget; each entry also uses its ActionSet presenter budget.
 * @param style Manual row appearances beneath the scene theme.
 * @param policy Ordinary retained layout constraints.
 * @return Single-use owning recipe retaining cold action declarations until attachment.
 * Invalid options/oversized input fail mount with INVALID_ARGUMENT/NODE_LIMIT. Shortcut/help are
 * metadata, not implicit global bindings. Only caption, checked state and shortcut are drawn.
 */
inline Description<Menu> menu(std::initializer_list<ActionRef> actions, MenuOptions options = {},
                              ControlStyle style = {}, SizePolicy policy = DEFAULT_MENU_POLICY) {
  return detail::menuRecipe(std::span(actions.begin(), actions.size()), options, style, policy);
}

/** @tparam Range Finite input range of ActionRef; identities are copied into the recipe.
 * @param actions Source range, consumed only up to the configured bound plus one rejection sentinel.
 * @param options Finite entry budget. @param style Manual row appearances. @param policy Layout constraints.
 * @return Owning single-use recipe; no range iterators or caller containers survive this call.
 */
template <std::ranges::input_range Range>
  requires std::same_as<std::ranges::range_value_t<Range>, ActionRef>
Description<Menu> menu(Range &&actions, MenuOptions options = {}, ControlStyle style = {},
                       SizePolicy policy = DEFAULT_MENU_POLICY) {
  std::vector<ActionRef> entries(0);

  if (options.maxEntries > 0 &&
      options.maxEntries <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    for (const auto &action : actions) {
      entries.push_back(action);

      if (entries.size() > options.maxEntries) {
        break;
      }
    }
  }

  return detail::menuRecipe(entries, options, style, policy);
}

/** @param reference Observing slot published after the complete mount succeeds.
 * @param actions Existing shared action identities. @param options Finite entry budget.
 * @param style Manual row appearances. @param policy Ordinary layout constraints.
 * @return Reference-first command list recipe.
 */
inline Description<Menu> menu(Ref<Menu> reference, std::initializer_list<ActionRef> actions, MenuOptions options = {},
                              ControlStyle style = {}, SizePolicy policy = DEFAULT_MENU_POLICY) {
  return menu(actions, options, style, policy).reference(std::move(reference));
}

/** @copydoc menu(Ref<Menu>, std::initializer_list<ActionRef>, MenuOptions, ControlStyle, SizePolicy) */
template <std::ranges::input_range Range>
  requires std::same_as<std::ranges::range_value_t<Range>, ActionRef>
Description<Menu> menu(Ref<Menu> reference, Range &&actions, MenuOptions options = {}, ControlStyle style = {},
                       SizePolicy policy = DEFAULT_MENU_POLICY) {
  return menu(std::forward<Range>(actions), options, style, policy).reference(std::move(reference));
}
} // namespace portals::ui
