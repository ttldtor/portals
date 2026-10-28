// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <portals/cell_buffer.hpp>
#include <portals/glyph_profile.hpp>
#include <portals/scrollbar_skin.hpp>

namespace portals {
/** @brief Resolved frame parts; absent parts inherit the ordinary control state. */
struct FrameAppearance {
  std::optional<CellStyle> line{};  ///< Perimeter glyph appearance.
  std::optional<CellStyle> title{}; ///< Title text appearance.
  std::optional<CellStyle> ends{};  ///< Prefix/suffix appearance, including their spacing.

  /** @param other Value to compare. @return Whether all parts agree. */
  bool operator==(const FrameAppearance &other) const = default;
};

/** @brief Resolved scrollbar parts; absent values inherit the ordinary control state.
 * Colors and attributes are shared by both axes; glyphs are orientation-specific.
 */
struct ScrollbarAppearance {
  std::optional<CellStyle> track{};  ///< Track cells outside the thumb.
  std::optional<CellStyle> thumb{};  ///< Moving thumb cells.
  std::optional<CellStyle> ends{};   ///< Optional decorative endpoint cells.
  std::optional<CellStyle> corner{}; ///< Intersecting horizontal and vertical gutters.
  std::optional<CellStyle> arrows{}; ///< Functional end buttons, independent of decorative ends.

  std::optional<ScrollbarPattern> trackPattern{};      ///< Resolved independent glyph source.
  std::optional<ScrollbarThumbPattern> thumbPattern{}; ///< Resolved independent glyph source.
  std::optional<ScrollbarArrowPattern> arrowPattern{}; ///< Resolved independent glyph source.

  /** @param other Value to compare. @return Whether every part agrees. */
  bool operator==(const ScrollbarAppearance &other) const = default;
};

/** @brief Complete resolved thumb segment values, shared by both axes.
 * Missing declarations inherit the common thumb while resolving this value.
 */
struct ScrollbarThumbAppearance {
  CellStyle start{};  ///< First cell of a thumb of length at least two.
  CellStyle body{};   ///< Interior cells of a thumb of length at least three.
  CellStyle end{};    ///< Last cell of a thumb of length at least two.
  CellStyle single{}; ///< Only cell of a one-cell thumb.

  /** @param other Resolved values to compare. @return Whether all segment attributes agree. */
  bool operator==(const ScrollbarThumbAppearance &other) const = default;
};

/** @brief Logical glyph overrides; absent values retain the control's owning manual map. */
struct GlyphAppearance {
  std::optional<GlyphProfile> profile{};   ///< Explicit base map, independent of colors.
  std::optional<GlyphFallback> fallback{}; ///< Explicit admission fallback.

  /** @param other Logical declarations to compare. @return Whether both choices agree. */
  constexpr bool operator==(const GlyphAppearance &other) const = default;
};

/** @brief Owning resolved presentation; no actions or model state are encoded. */
struct ComputedStyle {
  CellStyle normal;        ///< Ordinary text and fill.
  CellStyle focused;       ///< Focused control text and fill.
  CellStyle disabled;      ///< Disabled control text and fill.
  CellStyle selected;      ///< Selected list/table row or editor range.
  CellStyle header;        ///< Fixed table heading.
  CellStyle caret;         ///< Editor software caret.
  std::int32_t minWidth{}; ///< Nonnegative intrinsic minimum in cells; zero leaves measurement unchanged.
  std::array<FrameAppearance, 6> frame{};         ///< Parts in normal/focused/disabled/selected/header/caret order.
  std::array<ScrollbarAppearance, 6> scrollbar{}; ///< Parts in normal/focused/disabled/selected/header/caret order.
  /** @brief Optional complete segment values in normal/focused/disabled/selected/header/caret order.
   * Engaged only for matching segment declarations or explicit widget defaults. Inline ownership keeps
   * resolution and copies allocation-free; absence retains the common thumb and avoids segment resolution.
   */
  std::optional<std::array<ScrollbarThumbAppearance, 6>> scrollbarThumbSegments{};
  std::array<GlyphAppearance, 6> glyphs{}; ///< Logical maps in normal/focused/disabled/selected/header/caret order.
  /** @param other Owning value to compare.
   * @return True when every paint value and metric agrees.
   */
  bool operator==(const ComputedStyle &other) const = default;
};

namespace detail {
/// @cond INTERNAL
struct StyleSnapshotBlocks {
  std::shared_ptr<const std::array<FrameAppearance, 6>> frame;
  std::shared_ptr<const std::array<ScrollbarAppearance, 6>> scrollbar;
  std::shared_ptr<const std::array<ScrollbarThumbAppearance, 6>> thumb;
  std::shared_ptr<const std::array<GlyphAppearance, 6>> glyphs;
};

std::size_t styleBlockHash(const std::array<FrameAppearance, 6> &value) noexcept;
std::size_t styleBlockHash(const std::array<ScrollbarAppearance, 6> &value) noexcept;
std::size_t styleBlockHash(const std::array<ScrollbarThumbAppearance, 6> &value) noexcept;
std::size_t styleBlockHash(const std::array<GlyphAppearance, 6> &value) noexcept;
/// @endcond
} // namespace detail

/** @brief Immutable resident presentation with independently sparse extension blocks.
 * Widget bindings share equal values. Construction can allocate; all read access and flattening
 * are allocation-free. Retain the owning shared pointer to keep borrowed block references alive
 * across a style publication or destruction of the originating widget.
 */
class StyleSnapshot {
  class PreparedTag {
    friend class StyleEngine;
    friend class StyleSnapshot;
    PreparedTag() = default;

  public:
    PreparedTag(const PreparedTag &) = default;
  };

public:
  CellStyle normal;        ///< Ordinary text and fill.
  CellStyle focused;       ///< Focused control text and fill.
  CellStyle disabled;      ///< Disabled control text and fill.
  CellStyle selected;      ///< Selected row or editor range.
  CellStyle header;        ///< Fixed table heading.
  CellStyle caret;         ///< Editor software caret.
  std::int32_t minWidth{}; ///< Intrinsic minimum in cells.

  /** @brief Prepare the complete immutable value; may throw std::bad_alloc.
   * @param value Owning resolved value to copy into sparse storage.
   */
  explicit StyleSnapshot(const ComputedStyle &value);
  /// @cond INTERNAL
  StyleSnapshot(PreparedTag, const ComputedStyle &value, detail::StyleSnapshotBlocks blocks) noexcept;
  /// @endcond

  /** @return Frame states, or shared constant absent states. */
  const std::array<FrameAppearance, 6> &frame() const noexcept;
  /** @return Scrollbar states, or shared constant absent states. */
  const std::array<ScrollbarAppearance, 6> &scrollbar() const noexcept;
  /** @return Complete thumb states, or null when no segment override exists. */
  const std::array<ScrollbarThumbAppearance, 6> *scrollbarThumbSegments() const noexcept;
  /** @return Logical glyph states, or shared constant absent states. */
  const std::array<GlyphAppearance, 6> &glyphs() const noexcept;
  /** @return Whether this value owns a frame extension. */
  bool hasFrame() const noexcept;
  /** @return Whether this value owns a scrollbar extension. */
  bool hasScrollbar() const noexcept;
  /** @return Whether this value owns complete thumb segment states. */
  bool hasThumbSegments() const noexcept;
  /** @return Whether this value owns logical glyph states. */
  bool hasGlyphs() const noexcept;
  /** @return Complete owning flat value without allocation. */
  ComputedStyle flatten() const noexcept;
  /** @param other Complete owning value to compare. @return Full semantic equality. */
  bool operator==(const ComputedStyle &other) const noexcept;
  /** @param other Immutable value to compare. @return Full semantic equality. */
  bool operator==(const StyleSnapshot &other) const noexcept;
  /** @return Content hash used by bounded transaction-local sharing indexes. */
  std::size_t contentHash() const noexcept;
  /** @param value Complete owning value to hash. @return The same hash as its snapshot. */
  static std::size_t contentHash(const ComputedStyle &value) noexcept;

private:
  friend class StyleEngine;
  detail::StyleSnapshotBlocks blocks_;
};

namespace detail {
/// @cond INTERNAL
/** @brief Synchronous presentation borrow; the source must outlive this view. */
struct ComputedStyleView {
  const CellStyle &normal;
  const CellStyle &focused;
  const CellStyle &disabled;
  const CellStyle &selected;
  const CellStyle &header;
  const CellStyle &caret;
  std::int32_t minWidth;
  const std::array<FrameAppearance, 6> &frame;
  const std::array<ScrollbarAppearance, 6> &scrollbar;
  const std::array<ScrollbarThumbAppearance, 6> *scrollbarThumbSegments;
  const std::array<GlyphAppearance, 6> &glyphs;

  ComputedStyleView(const ComputedStyle &value) noexcept
      : normal(value.normal), focused(value.focused), disabled(value.disabled), selected(value.selected),
        header(value.header), caret(value.caret), minWidth(value.minWidth), frame(value.frame),
        scrollbar(value.scrollbar),
        scrollbarThumbSegments(value.scrollbarThumbSegments ? &*value.scrollbarThumbSegments : nullptr),
        glyphs(value.glyphs) {
  }

  ComputedStyleView(const StyleSnapshot &value) noexcept
      : normal(value.normal), focused(value.focused), disabled(value.disabled), selected(value.selected),
        header(value.header), caret(value.caret), minWidth(value.minWidth), frame(value.frame()),
        scrollbar(value.scrollbar()), scrollbarThumbSegments(value.scrollbarThumbSegments()), glyphs(value.glyphs()) {
  }
};

/// @endcond
} // namespace detail
} // namespace portals
