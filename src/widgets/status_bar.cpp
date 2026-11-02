// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/status_bar.hpp>

#include <array>

namespace portals {

StatusBar::StatusBar(LinearOptions options, ControlStyle style, SizePolicy policy)
    : Row(options, policy), style_(style) {
}

std::expected<void, TreeError> StatusBar::arrange(CellRect local) {
  const auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;

  return {};
}

std::expected<void, TreeError> StatusBar::paint(PaintContext &context) {
  const auto &snapshot = computedStyle();
  const auto style =
    snapshot ? (enabled_ ? snapshot->normal : snapshot->disabled) : (enabled_ ? style_.normal : style_.disabled);
  const std::array<RenderCommand, 1> commands{FillCommand{bounds_, bounds_, style}};

  return context.submit(commands);
}

ComputedStyle StatusBar::styleDefaults() const {
  ComputedStyle result;

  result.normal = style_.normal;
  result.focused = style_.focused;
  result.disabled = style_.disabled;

  return result;
}

std::expected<void, TreeError> syncStatusBar(WidgetTree &tree, Node<StatusBar> node) {
  const auto eligible = tree.inputEligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  std::expected<void, TreeError> result;
  const auto edited = tree.edit(node, [&](StatusBar &bar) {
    if (bar.enabled_ == *eligible) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);

    if (result) {
      bar.enabled_ = *eligible;
    }
  });

  return edited ? result : edited;
}

} // namespace portals
