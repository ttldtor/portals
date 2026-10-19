// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "toast_state.hpp"

#include <algorithm>
#include <limits>

namespace portals::detail {
void ToastLayer::unmount() {
  // Logical retirement precedes token/capture destruction, including reentrant destructors.
  record_->retired = true;
  record_->ready = false;
  record_->timerMarker.reset();
  record_->timerDeadline.reset();
  record_->timer = {};
  for (auto &slot : record_->slots) {
    slot->picture.reset();
  }
  record_->entries.clear();
}

std::expected<CellSize, TreeError> ToastLayer::childAvailable(CellSize available) const {
  return available.normalized();
}

std::expected<CellSize, TreeError> ToastLayer::measureChildren(std::span<const MeasuredChild>) const {
  return CellSize{};
}

std::expected<std::vector<CellRect>, TreeError> ToastLayer::arrangeChildren(std::span<const MeasuredChild> children,
                                                                            CellSize available) const {
  try {
    std::vector<CellRect> result(children.size());
    if (!record_->visible || record_->retired) {
      return result;
    }
    const auto size = available.normalized();
    const auto &options = *record_->options;
    const auto mx = std::min(options.margin, std::max(0, size.width - 1) / 2);
    const auto my = std::min(options.margin, std::max(0, size.height - 1) / 2);
    const auto width = std::min(options.maximumWidth, size.width - 2 * mx);
    const auto gap = std::min(options.gap, size.height);
    std::int64_t total{};
    std::size_t count{};
    for (const auto &slot : record_->slots) {
      if (slot->picture) {
        total += slot->picture->payload->action ? 2 : 1;
        ++count;
      }
    }
    if (count) {
      total += std::int64_t(count - 1) * gap;
    }
    const bool top = options.placement == ToastPlacement::TOP_START || options.placement == ToastPlacement::TOP_END;
    const bool start =
      options.placement == ToastPlacement::TOP_START || options.placement == ToastPlacement::BOTTOM_START;
    std::int64_t y = top ? my : std::max<std::int64_t>(my, std::int64_t(size.height) - my - total);
    for (std::size_t i = 0; i != children.size(); ++i) {
      const auto found = std::ranges::find_if(record_->slots, [&](const auto &slot) {
        return slot->node.id() == children[i].node;
      });
      if (found == record_->slots.end() || !(*found)->picture) {
        continue;
      }
      const auto height = (*found)->picture->payload->action ? 2 : 1;
      if (y > std::numeric_limits<std::int32_t>::max() - height) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      auto rectangle =
        CellRect::from({start ? mx : size.width - mx - width, static_cast<std::int32_t>(y)}, {width, height});
      if (!rectangle) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      result[i] = *rectangle;
      y += height + std::int64_t(gap);
    }
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<CellSize, TreeError> ToastRow::measure(CellSize) {
  return slot_->picture ? CellSize{slot_->picture->width, slot_->picture->payload->action ? 2 : 1} : CellSize{};
}

std::expected<void, TreeError> ToastRow::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());
  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  bounds_ = *bounds;
  return {};
}

std::expected<void, TreeError> ToastRow::paint(PaintContext &context) {
  const auto picture = slot_->picture;
  if (!picture || record_->retired || !record_->visible || bounds_.empty()) {
    return {};
  }
  const auto size = bounds_.size();
  if (std::uint64_t(size.width) * std::uint64_t(size.height) > record_->options->maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  const auto severity = static_cast<std::size_t>(picture->payload->data->severity);
  const auto &glyph = picture->payload->indicator ? *picture->payload->indicator : *record_->severity[severity];
  const RenderCommand fill = FillCommand{bounds_, bounds_, picture->styles[0]};
  auto painted = context.submit(std::span(&fill, 1));
  if (!painted) {
    return painted;
  }
  const std::array<RenderCommand, 4> commands{
    TextCommand{{0, 0}, glyph, bounds_, picture->styles[picture->payload->indicator ? 4 : 0]},
    TextCommand{{2, 0}, picture->summary, bounds_, picture->styles[1]},
    TextCommand{{std::max(0, size.width - 1), 0}, *record_->dismiss, bounds_, picture->styles[3]},
    TextCommand{{0, 1}, picture->action, bounds_, picture->styles[2]}};
  return context.submit(commands);
}
} // namespace portals::detail
