// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "../styles/theme_receiver.hpp"
#include "dialog_state.hpp"
#include "theme_selection_state.hpp"

#include <portals/key_binding.hpp>
#include <portals/scroll_controls.hpp>
#include <portals/theme_selector.hpp>

namespace portals::detail {
enum class ThemeSelectorCommand {
  OPEN,
  APPLY,
  CANCEL,
  KEEP,
  SELECT
};

struct ThemeSelectorRecord {
  std::shared_ptr<DialogRecord> dialog;
  ui::ThemeSelectionRef model;
  std::weak_ptr<ThemeSelectionRecord> modelRecord;
  std::shared_ptr<ui::detail::ThemeSelectorTarget> target;
  std::shared_ptr<StyleThemeReceiver> receiver;
  std::shared_ptr<std::function<std::expected<void, ManagedError>(std::string_view)>> callback;
  std::optional<std::string> shortcut;
  std::optional<std::string> initialId;
  KeyBinding opening;
  std::shared_ptr<ActionCancellation> openingLifetime;
  Node<Column> rows;
  std::array<Node<Label>, 3> status;
  Node<ScrollArea> scroll;
  Node<Button> apply;
  std::vector<Node<Button>> buttons = std::vector<Node<Button>>(0);
  std::optional<std::size_t> selected;
  bool attached{};
  bool busy{};
  bool retired{};
  bool failed{};
  bool applyEnabled{};

  ThemeSelectorRecord(ui::ThemeSelectionRef model, std::shared_ptr<ui::detail::ThemeSelectorTarget> target)
      : model(std::move(model)), target(std::move(target)) {
  }
};
} // namespace portals::detail

namespace portals::ui::detail {
struct ThemeSelectorTarget {
  const std::thread::id owner{std::this_thread::get_id()};
  ManagedScene *scene{};
  std::weak_ptr<const void> lifetime;
  std::weak_ptr<portals::detail::ThemeSelectorRecord> record;
  std::shared_ptr<const ThemeApplyOutcome> last;
  bool published{};
  bool transition{};
  bool callbackReported{};
  bool closeReported{};
};

struct ThemeSelectorAccess {
  static Description<Dialog> describe(ThemeSelectorRef reference, ThemeSelectionRef model,
                                      ThemeSelectorOptions options);
  static Description<Dialog> describeDefault(ThemeSelectorRef reference, std::span<const ThemeChoice> choices,
                                             const ThemeSelectionOptions &selection,
                                             const std::optional<std::string> &initialId, ThemeSelectorOptions options);
  static std::expected<std::shared_ptr<portals::detail::ThemeSelectorRecord>, ManagedError>
  enter(const std::shared_ptr<ThemeSelectorTarget> &target, std::source_location origin);
  static std::expected<void, ManagedError> command(const std::shared_ptr<ThemeSelectorTarget> &target,
                                                   portals::detail::ThemeSelectorCommand command, std::size_t index,
                                                   std::source_location origin);
  static void interactive(const std::shared_ptr<ThemeSelectorTarget> &target,
                          portals::detail::ThemeSelectorCommand command, std::size_t index = 0);
};
} // namespace portals::ui::detail
