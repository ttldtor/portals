// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/widget.hpp>
#include <span>

namespace portals {
class EventRouter;
}

namespace portals::detail {
/// @cond INTERNAL
// A narrow row-host publication seam. Validation precedes all allocation-free visibility
// and sibling-order changes; ownership is neither released nor transferred to callers.
struct RowTreeAccess {
  // Measure a staged row without publishing its visibility or arranging geometry.
  static std::expected<CellSize, TreeError> measure(WidgetTree &tree, Node<Widget> row, CellSize available);
  static std::expected<CellSize, TreeError> measureWidget(WidgetTree &tree, Node<Widget> node, Node<Widget> row,
                                                          CellSize available);
  static std::expected<std::uint64_t, TreeError> geometryRevision(const WidgetTree &tree);
  static bool dispatching(const EventRouter &router) noexcept;
  static std::uint64_t focusGeneration(const EventRouter &router) noexcept;
  static std::expected<void, TreeError> focusScope(EventRouter &router, Node<Widget> host, Node<Widget> row = {},
                                                   Node<Widget> chrome = {});
  static std::expected<Node<Widget>, TreeError> enterRow(EventRouter &router, Node<Widget> host, Node<Widget> row,
                                                         std::size_t column);
  static std::expected<std::size_t, TreeError> column(EventRouter &router, Node<Widget> row, Node<Widget> focused);
  static std::expected<void, TreeError> publish(WidgetTree &tree, Node<Widget> host,
                                                std::span<const Node<Widget>> visible);
};

/// @endcond
} // namespace portals::detail
