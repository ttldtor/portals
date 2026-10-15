// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/common.hpp"
#include "../gallery/themes.hpp"
#include <format>
#include <portals/input_match.hpp>
#include <portals/text_format.hpp>
#include <portals/ui.hpp>

namespace meter_example {
using namespace portals;
using example::take;
inline constexpr int MIN_LEVEL = 0;
inline constexpr int MAX_LEVEL = 100;
inline constexpr int INITIAL_LEVEL = 50;
inline constexpr int LEVEL_STEP = 5;
inline constexpr int BAR_TOP = 2;

/** @brief Final retained meter extension; state reads and formatting belong to prepare.
 * Paint consumes presentation
 * only; the managed scene controls its UI-affine lifetime.
 */
class LevelMeter final : public Widget {
public:
  /** @param value Domain value in the inclusive MIN_LEVEL..MAX_LEVEL percent range.
   * @return Whether text/value changed, or a text construction error mapped to TreeError.
   */
  std::expected<bool, TreeError> prepare(int value) {
    if (initialized_ && value_ == value) {
      return false;
    }

    auto label = formatText("Level {}% | Left/Right adjust | F2 theme | F10 quit", value);

    if (!label) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    label_ = std::move(*label);
    value_ = value;
    initialized_ = true;

    return true;
  }

  /** @param available Normalized cell constraints supplied by the layout owner.
   * @return The full available extent;
   * this example fills its scene viewport.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override {
    return available;
  }

  /** @param rect Parent-relative placement; only its size is retained.
   * @return Success after retaining the current
   * viewport extent.
   */
  std::expected<void, TreeError> arrange(CellRect rect) override {
    size_ = rect.size();

    return {};
  }

  /** @param context Borrowed synchronous sink; submitted text borrows this widget for the frame.
   * @return The
   * original sink failure or success after submitting background, bar and caption.
   * Geometry construction failures
   * throw through the finite example helper.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override {
    const auto area = take(CellRect::from({}, size_));
    const auto bar =
      take(CellRect::from({0, std::min(BAR_TOP, size_.height)},
                          {static_cast<std::int32_t>(static_cast<std::int64_t>(size_.width) * value_ / MAX_LEVEL),
                           std::max(0, size_.height - BAR_TOP)}));
    const auto styles = computedStyle().value_or(ComputedStyle{});
    const std::array<RenderCommand, 3> commands{FillCommand{area, area, styles.normal},
                                                FillCommand{bar, area, styles.focused},
                                                TextCommand{{}, label_, area, styles.normal}};

    return context.submit(commands);
  }

private:
  int value_{};
  bool initialized_{};
  CellSize size_{};
  GraphemeText label_ = example::text("");
};

} // namespace meter_example
