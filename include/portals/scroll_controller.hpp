// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <memory>
#include <portals/scroll_geometry.hpp>

namespace portals {
/** @brief Owning initial configuration of one exact scrolling axis. */
struct ScrollAxisConfiguration {
  ScrollAxisMetrics metrics; ///< Initial logical extent/viewport; a mounted area later owns its actual metrics.
  ScrollOffset offset;       ///< Intended offset, clamped to the initial viewport-start range.
};

/** @brief Owning pre-mount configuration; no callbacks, tree or widget references. */
struct ScrollControllerOptions {
  ScrollAxisConfiguration horizontal; ///< Initial horizontal values, defaulting to an empty range.
  ScrollAxisConfiguration vertical;   ///< Initial vertical values, defaulting to an empty range.
};

/** @brief Owning immutable observation of one axis at a controller revision. */
struct ScrollAxisSnapshot {
  ScrollAxisMetrics metrics; ///< Copied logical extent and viewport.
  ScrollOffset offset;       ///< Copied exact position and fractional remainder.
};

/** @brief Owning two-axis observation, safe to retain after the controller is destroyed.
 * Values are independent of subsequent UI changes; concurrent const reads are safe.
 */
struct ScrollControllerSnapshot {
  ScrollAxisSnapshot horizontal; ///< Horizontal values from this same revision.
  ScrollAxisSnapshot vertical;   ///< Vertical values from this same revision.
  std::uint64_t revision{};      ///< Nonzero checked revision; never wraps or reuses an exhausted identity.
};

namespace detail {
struct ScrollControllerAccess;
}

/** @brief Shared owning, UI-affine scroll values built on the existing ScrollState.
 * The controller owns no tree/widget/router references or observer runtime. Public access is
 * read-only after checked creation; mounted commands must use their checked node/tree owner.
 * Shared ownership does not permit concurrent mutation. Final destruction only releases values
 * and may occur on any thread; checked queries require the creating UI thread.
 */
class ScrollController {
public:
  /** @brief Validate and atomically create both initial axes on the current UI thread.
   * @param options Owning metrics and intended offsets; all uint64 metrics are supported.
   * @return Shared controller, INVALID_ARGUMENT for an invalid fraction, or NO_MEMORY.
   * @note Initial positions clamp to extent-viewport, with position zero for a zero viewport.
   * No tree mutation, invalidation, callbacks or terminal I/O occur.
   */
  [[nodiscard]] static std::expected<std::shared_ptr<ScrollController>, TreeError>
  create(ScrollControllerOptions options = ScrollControllerOptions{});

  /** @brief Release values; no callbacks, tree access or thread-affine cleanup occurs. */
  ~ScrollController();
  ScrollController(const ScrollController &) = delete;
  ScrollController &operator=(const ScrollController &) = delete;

  /** @brief Observe both axes atomically on the creating UI thread.
   * @return Owning values from one revision, or WRONG_THREAD without reading mutable state.
   * @note No allocation, mutation, invalidation, callbacks or borrowed result storage.
   */
  [[nodiscard]] std::expected<ScrollControllerSnapshot, TreeError> snapshot() const noexcept;

private:
  friend struct detail::ScrollControllerAccess;
  struct State;
  explicit ScrollController(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};
} // namespace portals
