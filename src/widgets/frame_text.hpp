// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/primitives.hpp>

namespace portals::detail {
// Construction-only compilation. Semantic failures use TreeError; allocation failures propagate to the typed caller.
std::expected<PreparedFrameText, TreeError> prepareFrameText(WidthPolicy policy, const FrameOptions &options);

// All rectangles use zero-origin container-local coordinates. At most two track rectangles are accepted.
// Work is constant in the frame extent. Includes the entire submitted title's bytes and graphemes, even when clipped.
// Excluded track cells win over frame/title cells; the title uses the first largest remaining contiguous top-edge run.
std::uint64_t framePaintWork(const PreparedFrameText &text, const FrameOptions &options, CellSize size,
                             std::span<const CellRect> exclusions = {}) noexcept;
std::expected<void, TreeError> paintFrame(PaintContext &context, const PreparedFrameText &text,
                                          const FrameOptions &options, CellRect bounds, CellStyle style,
                                          std::span<const CellRect> exclusions = {}, FrameAppearance appearance = {});
} // namespace portals::detail
