// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>
#include <portals/text_value.hpp>

#include <array>
#include <initializer_list>
#include <string_view>

namespace portals {
/** @brief Owning synchronous label lookup, invoked only while staging a configuration. */
using SpinnerLabelResolver = std::function<std::expected<std::string, TextResourceError>(std::string_view)>;
/** @brief Original single-row indeterminate animation sequences. */
enum class SpinnerPreset {
  DOTS,       ///< Three positions, moving left to right.
  LINE,       ///< Portable ASCII rotating line.
  BRAILLE,    ///< Eight one-cell braille positions; ASCII fallback for incompatible widths.
  BAR_BOUNCE, ///< Five positions traversed in both directions.
  ARC,        ///< Rotating arc.
  CIRCLE,     ///< Circle phases.
  SQUARE,     ///< Square phases.
  TRIANGLE,   ///< Triangle phases.
  ARROW,      ///< Arrow directions.
  BLOCKS,     ///< Rising and falling blocks.
  ELLIPSIS,   ///< Growing dots.
  PULSE       ///< Pulsing indicator.
};

/** @brief Canonical names in SpinnerPreset ordinal order. */
inline constexpr std::array<std::string_view, 12> SPINNER_PRESET_NAMES{"DOTS",  "LINE",   "BRAILLE",  "BAR_BOUNCE",
                                                                       "ARC",   "CIRCLE", "SQUARE",   "TRIANGLE",
                                                                       "ARROW", "BLOCKS", "ELLIPSIS", "PULSE"};

/** @param name Preset name, ASCII case-insensitive; hyphens and underscores are equivalent.
 * @return Matching preset, or an invalid value collected by ordinary configuration validation.
 */
inline SpinnerPreset spinnerPreset(std::string_view name) noexcept {
  for (std::size_t i = 0; i < SPINNER_PRESET_NAMES.size(); ++i) {
    const auto candidate = SPINNER_PRESET_NAMES[i];
    if (candidate.size() != name.size())
      continue;
    bool equal = true;
    for (std::size_t j = 0; j < name.size(); ++j) {
      char c = name[j];
      if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
      if (c == '-')
        c = '_';
      if (c != candidate[j]) {
        equal = false;
        break;
      }
    }
    if (equal)
      return static_cast<SpinnerPreset>(i);
  }
  return static_cast<SpinnerPreset>(-1);
}

namespace detail {
/** Convert an initializer list into the existing owning LF format without hiding invalid entries. */
inline std::string spinnerFrames(std::initializer_list<std::string_view> values) {
  std::string result;
  bool first = true;
  for (const auto value : values) {
    if (!first)
      result += '\n';
    first = false;
    if (value.empty() || value.find_first_of("\r\n") != std::string_view::npos)
      result += '\r';
    else
      result += value;
  }
  return result;
}
} // namespace detail

/** @brief Traversal of the finite owning frame sequence. */
enum class SpinnerDirection {
  FORWARD, ///< Increasing frame indices with wraparound.
  REVERSE  ///< Decreasing frame indices with wraparound.
};

/** @brief Owning single-row indicator configuration; no thread or timer is created by this value. */
struct SpinnerOptions {
  SpinnerPreset preset{SpinnerPreset::DOTS};             ///< Sequence used when frames is empty.
  std::string frames = std::string(0, '\0');             ///< Custom UTF-8 frames separated by LF; no empty frames.
  SchedulerTime interval{100};                           ///< Positive monotonic milliseconds between frames.
  SpinnerDirection direction{SpinnerDirection::FORWARD}; ///< Finite sequence traversal order.
  LayoutAlign alignment{LayoutAlign::START};             ///< START/CENTER/END alignment of indicator and label.
  bool active{true};                                     ///< False pauses animation without polling.
  bool reducedMotion{};                                  ///< True selects static frame zero without a timer.
  TextValue label{""};                                   ///< Owning label resolved before widget publication.
  std::size_t maxFrames{64};                             ///< Positive maximum complete frame count.
  std::size_t maxFrameBytes{4096};                       ///< Positive complete sequence source-byte budget.
  std::size_t maxFrameCells{64};                         ///< Positive maximum cells per frame.
  std::size_t maxPaintCells{16384};                      ///< Positive allocated-area paint budget.

  /** @brief Preserve fallible owning copies, including MSVC Debug iterator proxy allocations. */
  ~SpinnerOptions() = default;
};

namespace detail {
struct SpinnerState;
}
class ManagedScene;

/** @brief Retained indeterminate indicator using the receiving scene's existing scheduler.
 * Construct through ui::spinner or ManagedScene::spinner to enable animation. A standalone
 * tree.emplace remains a valid static presentation. All access is UI-affine through WidgetTree.
 * Paint borrows only widget-owned text until the enclosing frame ends.
 */
class Spinner : public SizedWidget {
public:
  /** @param width Complete receiving text-width policy.
   * @param options Owning finite sequence, label and animation configuration.
   * @param style Manual active/dim appearances beneath computed theme styles.
   * @param policy Existing owning layout constraints.
   * @param resolver Optional synchronous label localization adapter, used only during construction.
   * Construction may allocate; checked tree emplacement translates allocation failures.
   */
  explicit Spinner(WidthPolicy width = {}, SpinnerOptions options = {}, ControlStyle style = {},
                   SizePolicy policy = DEFAULT_CONTENT_POLICY, SpinnerLabelResolver resolver = {});
  /** @brief Cancel this widget's timer on UI, without retaining the scene. */
  ~Spinner() override;
  Spinner(const Spinner &) = delete;
  Spinner &operator=(const Spinner &) = delete;
  /** @param width Complete receiving text-width policy.
   * @param options Owning sequence/budget settings; label is deliberately not resolved here.
   * @return Success for valid bounded single-row frames, or NO_MEMORY/INVALID_ARGUMENT/NODE_LIMIT.
   */
  static std::expected<void, TreeError> validate(WidthPolicy width, SpinnerOptions options);
  /** @return Construction-time typed validation result; no callbacks or allocation. */
  std::expected<void, TreeError> preparation() const noexcept;
  /** @param available Offered extent; intrinsic width includes one space before a nonempty label.
   * @return Maximum sequence width plus label, one row, or construction/geometry error.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Parent-local allocation; only its extent is retained.
   * @return Success or INVALID_GEOMETRY.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Borrowed synchronous rendering sink.
   * @return Original sink error, preparation error, or WOULD_BLOCK above allocated-area budget.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual active/dim defaults beneath theme declarations. */
  ComputedStyle styleDefaults() const override;
  /** @return Current finite frame index, borrowed only during checked UI access. */
  std::size_t frameIndex() const noexcept;
  /** @return Owning configuration snapshot; copies may allocate. */
  SpinnerOptions options() const;

private:
  friend class ManagedScene;
  friend std::expected<void, TreeError> setSpinnerActive(WidgetTree &, Node<Spinner>, bool);
  friend std::expected<void, TreeError> setSpinner(WidgetTree &, Node<Spinner>, const SpinnerOptions &,
                                                   SpinnerLabelResolver);
  std::shared_ptr<detail::SpinnerState> state_;
  ControlStyle style_;
  CellRect bounds_;
};

/** @param tree Borrowed receiving UI tree.
 * @param node Checked live indicator identity.
 * @param active Whether animation should run after ordinary preparation; stopping cancels its token immediately.
 * @return Checked mutation/invalidation result; retains the bounded current frame and allocates no owning sequence.
 */
std::expected<void, TreeError> setSpinnerActive(WidgetTree &tree, Node<Spinner> node, bool active);

/** @param tree Borrowed receiving UI tree.
 * @param node Checked live indicator identity.
 * @param options Owning replacement; all text is staged before replacing state/cancelling the old timer.
 * @param resolver Optional synchronous localization adapter, not retained after validation.
 * @return Success, NO_MEMORY, INVALID_ARGUMENT, or original tree error; no partial publication on failure.
 * The next ordinary preparation starts one fresh interval, with frame zero and no elapsed-time catch-up.
 */
std::expected<void, TreeError> setSpinner(WidgetTree &tree, Node<Spinner> node, const SpinnerOptions &options,
                                          SpinnerLabelResolver resolver = {});
} // namespace portals
