// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "scroll_controls_state.hpp"
#include <portals/log_view.hpp>

namespace portals::detail {
struct LogRecord {
  std::uint64_t id;
  GraphemeText text;
};

struct LogRow {
  std::size_t record;
  std::size_t byteBegin;
  std::size_t byteEnd;
};

struct LogPaintCache {
  std::uint64_t first{};
  std::int32_t height{};
  // Empty fallible construction must not require a default-constructible GraphemeText.
  std::vector<GraphemeText> lines = std::vector<GraphemeText>(std::initializer_list<GraphemeText>{});
};

struct LogAnchor {
  std::uint64_t record{};
  std::size_t byte{};
  double fraction{};
};

struct LogData {
  std::vector<std::shared_ptr<const LogRecord>> records = std::vector<std::shared_ptr<const LogRecord>>(0);
  std::vector<LogRow> rows = std::vector<LogRow>(0);
  std::unique_ptr<LogPaintCache> paint;
  std::unique_ptr<GraphemeText> footer;
  LogStatus status;
  LogAnchor anchor;
  std::uint64_t nextID{1};
  std::uint64_t intentSerial{};
  std::int32_t width{};
};

struct LogViewState {
  LogViewState(WidthPolicy width, LogViewConfig config);

  WidthPolicy width;
  LogViewConfig config;
  std::unique_ptr<LogData> data;
  bool editing{};
};

struct LogViewAccess {
  static LogViewState &state(LogView &widget) noexcept {
    return *widget.log_;
  }

  static const LogViewState &state(const LogView &widget) noexcept {
    return *widget.log_;
  }
};

// Both entry points borrow the initialized UI widget. They allocate only before publication.
std::expected<void, TreeError> validateLogViewState(const LogViewState &log, const ScrollWidgetState &scroll);
std::expected<bool, TreeError> prepareLogView(WidgetTree &tree, Node<Widget> node, LogView &widget,
                                              const ScrollAreaLayoutSnapshot &layout);
std::expected<void, TreeError> stageLogViewOffset(const LogView &widget, ScrollControllerPlan &plan,
                                                  const ScrollAreaLayoutSnapshot &layout);
std::expected<EventReply, TreeError> logViewInput(WidgetTree &tree, Node<Widget> node, LogView &widget,
                                                  const RoutedEvent &event);
} // namespace portals::detail
