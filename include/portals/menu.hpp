// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_action.hpp>

namespace portals {
/** @brief Natural menu extent on both axes; callers may supply an ordinary explicit SizePolicy. */
inline constexpr SizePolicy DEFAULT_MENU_POLICY{.width = {.mode = SizeMode::CONTENT},
                                                .height = {.mode = SizeMode::CONTENT}};

/** @brief Finite admission limit for an ordinary retained command list. */
struct MenuOptions {
  std::size_t maxEntries{256}; ///< Positive entry bound, at most INT32_MAX; duplicates count separately.
};

/** @brief Initially visible action-backed command list, without popup or submenu behavior.
 * Actions own callback/state; the menu owns only prepared presentation and local selection.
 * Construct through ui::menu to attach actions and routed input to the managed scene.
 */
class Menu : public Control {
public:
  /** @param empty Empty text carrying the scene's width policy.
   * @param actions Owning entry identities copied into this retained control.
   * @param options Finite admission bound, validated by the declarative factory.
   * @param style Manual normal, focused-selection and disabled row styles.
   * @param policy Ordinary layout constraints; clipping never adds scrolling.
   * Construction may allocate inside the existing checked tree emplacement boundary.
   */
  Menu(GraphemeText empty, const std::vector<ui::ActionRef> &actions, MenuOptions options = {}, ControlStyle style = {},
       SizePolicy policy = DEFAULT_MENU_POLICY);
  ~Menu() override;

  /** @param available Offered extent. @return Maximum prepared row width and visible row count. */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Allocated extent. @return Success or INVALID_GEOMETRY. */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Existing synchronous sink. @return Prepared Unicode rows or unchanged sink error. */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Original zero-based entry index, empty when no enabled visible entry exists. */
  std::optional<std::size_t> selectedIndex() const noexcept;
  /** @return Last prepared visible row count; hidden or unavailable entries occupy no row. */
  std::size_t visibleRowCount() const noexcept;

private:
  friend struct ui::detail::ActionAccess;
  friend class ManagedScene;
  struct Prepared;

  struct Input {
    bool handled{};
    std::optional<ui::ActionRef> action;
    ui::ActionInvocation invocation;
  };

  std::expected<void, TreeError> prepare(WidgetTree &, Node<Menu>, WidthPolicy);
  void invalidatePresentation() noexcept;
  std::expected<Input, TreeError> input(WidgetTree &, Node<Menu>, const RoutedEvent &, const NodeGeometry &);
  bool eligible(std::size_t) const;
  std::optional<std::size_t> next(std::optional<std::size_t>, bool forward) const;

  std::vector<ui::ActionRef> actions_;
  MenuOptions options_;
  std::unique_ptr<Prepared> prepared_;
  std::optional<std::size_t> selected_;
  CellRect bounds_;
};
} // namespace portals
