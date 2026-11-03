// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/menu.hpp>
#include <portals/popup_menu.hpp>
#include <portals/ui_construction.hpp>

#include <limits>
#include <ranges>

namespace portals::ui {
namespace detail {
/// @cond INTERNAL
Description<Menu> menuRecipe(std::span<const ActionRef>, MenuOptions, ControlStyle, SizePolicy);
Description<PopupMenu> popupMenuRecipe(std::span<const PopupEntry>, PopupMenuOptions, ControlStyle, SizePolicy);
/// @endcond
} // namespace detail

/** @param action Shared command identity. @param mnemonic Explicit scalar; ASCII matching ignores case.
 * @return Owning leaf entry; caption/state/callback stay in the shared action model.
 */
inline PopupEntry menuItem(ActionRef action, std::optional<char32_t> mnemonic = {}) {
  PopupEntry entry;

  entry.action = std::move(action);
  entry.mnemonic = mnemonic;

  return entry;
}

/** @param caption Owning literal UTF-8 branch caption. @param children Finite entries copied now.
 * @param mnemonic Explicit scalar for this branch, local to its parent panel.
 * @return Owning structural submenu, without creating another action or callback.
 */
inline PopupEntry submenu(std::string caption, std::initializer_list<PopupEntry> children,
                          std::optional<char32_t> mnemonic = {}) {
  PopupEntry entry;

  entry.caption = std::move(caption);
  entry.children.assign(children);
  entry.mnemonic = mnemonic;
  entry.submenu = true;

  return entry;
}

/** @param entries Owning finite submenu graph, copied now. @param options Immutable budgets and plane caps.
 * @param style Manual row appearance below the theme. @param policy Ordinary content/min/max/flex constraints.
 * @return Initially closed layer for ui::scene(body, layers...), with an automatically reused overlay host.
 */
inline Description<PopupMenu> popupMenu(std::initializer_list<PopupEntry> entries, PopupMenuOptions options = {},
                                       ControlStyle style = {}, SizePolicy policy = DEFAULT_MENU_POLICY) {
  return detail::popupMenuRecipe(std::span(entries.begin(), entries.size()), options, style, policy);
}

/** @param reference Observing slot published after complete mount. @param entries Owning entries copied now.
 * @param options Immutable finite budgets. @param style Manual row appearance. @param policy Panel size constraints.
 * @return The same initially closed scene layer with a unified typed Ref.
 */
inline Description<PopupMenu> popupMenu(Ref<PopupMenu> reference, std::initializer_list<PopupEntry> entries,
                                       PopupMenuOptions options = {}, ControlStyle style = {},
                                       SizePolicy policy = DEFAULT_MENU_POLICY) {
  return popupMenu(entries, options, style, policy).reference(std::move(reference));
}

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
