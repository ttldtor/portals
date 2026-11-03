// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/menu.hpp>

namespace portals {
struct PopupEntry;

namespace detail {
/// @cond INTERNAL
std::vector<PopupEntry> emptyPopupEntries();
/// @endcond
} // namespace detail

/** @brief Owning finite popup entry; callbacks and leaf presentation remain in ActionSet. */
struct PopupEntry {
  ui::ActionRef action;                                           ///< Shared leaf identity; ignored for a submenu.
  std::string caption = std::string(0, '\0');                     ///< Owning submenu caption, literal UTF-8.
  std::vector<PopupEntry> children = detail::emptyPopupEntries(); ///< Owned submenu entries.
  std::optional<char32_t> mnemonic;                               ///< Explicit scalar; ASCII comparison ignores case.
  bool submenu{};     ///< True for a structural branch, including an empty branch.
  bool enabled{true}; ///< Structural branch admission; leaf admission comes from its action.
  bool visible{true}; ///< Structural branch visibility; hidden branches consume no row.
};

namespace detail {
/// @cond INTERNAL
inline std::vector<PopupEntry> emptyPopupEntries() {
  // The complete type permits the count constructor, which keeps debug proxy allocation fallible.
  return std::vector<PopupEntry>(0);
}

/// @endcond
} // namespace detail

/** @brief Immutable finite popup construction options; cell extents include panel borders. */
struct PopupMenuOptions {
  std::size_t maxEntries{256};       ///< Total entries across all branches; duplicates count separately.
  std::size_t maxDepth{8};           ///< Positive tree depth including the root, at most 32.
  CellSize maximumPanelSize{60, 20}; ///< Positive viewport-independent caps on the popup plane.
  std::int32_t panelGap{1};          ///< Nonnegative transparent cell gap between adjacent panels.
  FrameOptions frame{};              ///< Existing frame pattern/glyphs, title chrome and finite per-panel paint budget.
};

/** @brief Sized action-backed popup plane, owned by one private managed Dialog activation.
 * Use ui::popupMenu as an initially closed scene layer. One Ref observes this real Menu subtype;
 * removal through ManagedScene removes its private root as well. No command store or scheduler is added.
 */
class PopupMenu final : public Menu {
public:
  /** @brief Internal construction path after checked popup admission.
   * @pre Entries, leaf mapping and frame options were validated under the empty text width policy.
   * Use ui::popupMenu for checked user-facing construction; allocation failures propagate.
   * @param empty Valid empty text using the scene width policy.
   * @param entries Owning immutable tree, copied before publication.
   * @param leaves All leaf occurrences used by the existing shared presenter admission.
   * @param options Positive finite construction/paint geometry budgets.
   * @param layer Private existing Dialog root carrying input confinement and focus return.
   * @param style Manual row styles below the scene theme.
   * @param policy Ordinary content/min/max/flex policy on the transparent popup plane.
   */
  PopupMenu(GraphemeText empty, const std::vector<PopupEntry> &entries, const std::vector<ui::ActionRef> &leaves,
            PopupMenuOptions options, Node<Dialog> layer, ControlStyle style = {},
            SizePolicy policy = DEFAULT_MENU_POLICY);
  ~PopupMenu() override;

  /** @param available Offered plane extent. @return Prepared content extent capped by available space. */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Allocated plane rectangle. @return Checked panel placement without tree access. */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Synchronous frame sink. @return Separately painted opaque panels with transparent gaps. */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Private structural layer observer, without ownership or implicit activation. */
  Node<Dialog> dialogRoot() const noexcept;
  /** @return Prepared panels in the current navigation path, regardless of Dialog visibility. */
  std::size_t panelCount() const noexcept;
  /** @return Selection in the active panel's original entry order, or empty. */
  std::optional<std::size_t> selectedIndex() const noexcept override;
  /** @return Number of visible rows in the active panel, before viewport clipping. */
  std::size_t visibleRowCount() const noexcept override;
  /** @brief Reset local selection before a new managed activation; no callback or tree access. */
  void resetSelection() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
  Node<Dialog> layer_;

  std::expected<void, TreeError> prepare(WidgetTree &, Node<Menu>, WidthPolicy) override;
  void invalidatePresentation() noexcept override;
  std::expected<Input, TreeError> input(WidgetTree &, Node<Menu>, const RoutedEvent &, const NodeGeometry &) override;
};
} // namespace portals
