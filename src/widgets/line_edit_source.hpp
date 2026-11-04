// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/line_edit.hpp>

namespace portals::detail {
// Private seam for the numeric editor. No second text Property or editor policy is retained.
struct LineEditSource {
  virtual ~LineEditSource() = default;
  virtual std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> snapshot() = 0;
  virtual std::expected<PropertyUpdate<std::string>, TreeError> set(const std::string &) = 0;
  virtual std::expected<Subscription, TreeError> observe(Node<LineEdit>, std::function<void()>) = 0;

  virtual bool hasSteps() const noexcept {
    return false;
  }

  virtual std::expected<std::optional<std::string>, TreeError> step(std::string_view, bool, int, bool) {
    return std::optional<std::string>{};
  }
};

struct LineEditSourceAccess {
  static std::expected<LineEditBinding, TreeError>
  bind(WidgetTree &, EventRouter &, Node<LineEdit>, std::shared_ptr<LineEditSource>,
       std::function<void(LineEditResult)>, std::function<bool(std::string_view)>, LineEditEvents = {},
       std::shared_ptr<Property<std::string>> legacy = {}, std::shared_ptr<Clipboard> clipboard = {});

  static LineEditOptions options(const LineEdit &edit) {
    return edit.options_;
  }

  static std::expected<LineEditResult, TreeError> set(LineEditBinding &, GraphemeText);
  static std::expected<void, TreeError> step(LineEditBinding &, int, bool = false);
};
} // namespace portals::detail
