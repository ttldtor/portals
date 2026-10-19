// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/theme_selection.hpp>

#include <thread>

namespace portals::detail {
struct ThemeSelectorRecord;
enum class ThemeSelectionCommand {
  PREVIEW,
  COMMIT,
  CANCEL
};

struct ThemeSelectionChoice {
  std::shared_ptr<const ui::ThemeChoiceData> data;
  std::function<std::expected<StyleSheet, StyleError>()> factory;
};

struct ThemeSelectionRecord {
  std::weak_ptr<ThemeSelectorRecord> selector;
  std::vector<ThemeSelectionChoice> choices = std::vector<ThemeSelectionChoice>(0);
  std::shared_ptr<const ui::ThemeSelectionSnapshot> snapshot;

  std::optional<StyleLayerOptions> layers;
  StyleLayerHandle preview;
  bool busy{};

  // Every caller checks this before owning staging and again before the model guard is acquired.
  std::expected<void, ManagedError> checkTransition(NodeId owner, std::source_location origin) const;
};
} // namespace portals::detail

namespace portals::ui::detail {
struct ThemeSelectionTarget {
  const std::thread::id owner{std::this_thread::get_id()};
  ManagedScene *scene{};
  std::weak_ptr<portals::detail::ThemeSelectionRecord> record;
  std::weak_ptr<const void> lifetime;
  NodeId root{};
  bool published{};
};

struct ThemeSelectionDescription {
  ThemeSelectionRef reference;
  std::vector<ThemeChoice> choices;
  ThemeSelectionOptions options;

  ThemeSelectionDescription(ThemeSelectionRef reference, std::initializer_list<ThemeChoice> choices,
                            const ThemeSelectionOptions &options)
      : reference(std::move(reference)), choices(choices), options(options) {
  }
};
} // namespace portals::ui::detail
