// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/primitives.hpp>
#include <portals/scroll_geometry.hpp>

// Keep the existing glyph defaults available under native Unicode headers, then restore their macro.
#pragma push_macro("UNICODE")
#undef UNICODE

namespace portals {
/** @brief Owning standalone bar settings, validated before mounting or replacement.
 * No callbacks, tree identity, borrowed storage, timers or terminal resources are retained.
 */
struct ScrollBarOptions {
  double wheelStep{3};          ///< Positive finite logical cells per decoded wheel event.
  std::uint64_t keyStep{1};     ///< Positive integral logical cells per arrow event.
  std::uint64_t pageOverlap{1}; ///< Shared cells between pages; values above viewport are supported.
  std::int32_t minimumThumb{1}; ///< Positive minimum thumb cells, capped by the actual track.
  PrimitiveGlyphs glyphs{PrimitiveGlyphs::UNICODE}; ///< One-cell glyph mode with existing ASCII fallback.
  std::size_t maxPaintCells{16384};                 ///< Positive total track paint-work limit.
  bool focusable{true};                             ///< Whether the standalone bar participates in Tab traversal.
  bool readOnly{}; ///< Reject user offset changes; checked programmatic commands remain possible.
};

/** @brief Owning ordinary-area settings; content is retained rather than automatically virtualized.
 * OVERLAY keeps nominal range/visibility and may obscure edge content. ON_FRAME requires frame.
 * Embedded bars share the area's input/paint owner and never add focus stops.
 */
struct ScrollAreaOptions {
  ScrollBarPolicy horizontal{ScrollBarPolicy::AUTO};    ///< Horizontal bar presence policy.
  ScrollBarPolicy vertical{ScrollBarPolicy::AUTO};      ///< Vertical bar presence policy.
  ScrollPlacement placement{ScrollPlacement::RESERVED}; ///< Explicit gutter/overlay/frame placement.
  std::optional<BorderOptions> frame;                   ///< Optional existing one-cell outline, without a second inset.
  double wheelStep{3};                                  ///< Positive finite logical cells per decoded wheel event.
  std::uint64_t keyStep{1};                             ///< Positive integral logical cells per arrow event.
  std::uint64_t pageOverlap{1}; ///< Shared cells between pages; values above viewport are supported.
  std::int32_t minimumThumb{1}; ///< Positive minimum thumb cells, capped by the actual track.
  PrimitiveGlyphs glyphs{PrimitiveGlyphs::UNICODE}; ///< Existing one-cell Unicode/ASCII glyph policy.
  std::size_t maxPaintCells{16384};                 ///< Positive total outline-plus-track paint-work limit.
  bool focusable{true};                             ///< Whether the owning area participates in Tab traversal.
  bool readOnly{}; ///< Reject user scrolling while permitting focus reveal/programmatic commands.
};

/** @brief Validate standalone bar settings without mutation, allocation or callbacks.
 * @param options Owning input settings, borrowed only for this call.
 * @return Success, or INVALID_ARGUMENT for an invalid glyph/step/minimum/budget.
 * @note Any thread; no invalidation or resource acquisition occurs.
 */
[[nodiscard]] std::expected<void, TreeError> validateScrollOptions(const ScrollBarOptions &options) noexcept;

/** @brief Validate area settings and the optional existing BorderOptions.
 * @param options Owning input settings, borrowed only for this call.
 * @return Success, or INVALID_ARGUMENT for invalid settings/placement/frame configuration.
 * @note Any thread; no allocation, callbacks, mutation or invalidation occurs.
 */
[[nodiscard]] std::expected<void, TreeError> validateScrollOptions(const ScrollAreaOptions &options) noexcept;
} // namespace portals

#pragma pop_macro("UNICODE")
