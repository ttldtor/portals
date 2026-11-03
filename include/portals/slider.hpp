// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>
#include <portals/numeric.hpp>

namespace portals {

/** @brief Horizontal slider presentation and user-input configuration. */
struct SliderOptions {
  std::int32_t preferredWidth{20};  ///< Nonnegative intrinsic width in cells; intrinsic height is one.
  std::uint64_t pageSteps{10};      ///< Positive increments applied by PageUp/PageDown.
  std::size_t maxPaintCells{16384}; ///< Positive bound on the allocated background area, including extra rows.
  bool readOnly{};                  ///< User mutation gate; programmatic model writes remain allowed.
};

/** @brief One completed or interrupted numeric user operation. */
enum class SliderOperationStatus {
  COMPLETED, ///< Normal matching release or one accepted keyboard action.
  CANCELLED, ///< Capture/eligibility loss or Escape; applied live values are retained.
  CONFLICT   ///< Another source revision superseded this operation; no rollback is performed.
};

/** @brief Origin of a local numeric operation, without a global command/history identity. */
enum class SliderOperationKind {
  DRAG, ///< One admitted mouse press and its captured continuation.
  STEP  ///< One keyboard press/repeat or a click on a host without capture support.
};

/** @brief Owning operation observation; model revisions and live onChange calls can be numerous. */
struct SliderOperationEvent {
  SliderOperationKind kind{SliderOperationKind::DRAG};            ///< Input sequence being completed.
  SliderOperationStatus status{SliderOperationStatus::COMPLETED}; ///< Completion or interruption cause.
  std::shared_ptr<const PropertyChange<NumericValue>> initial;    ///< Independent starting snapshot.
  std::optional<PropertyUpdate<NumericValue>> lastUserUpdate;     ///< Exact last changed user receipt, if any.
};

/** @brief Horizontal numeric view using the existing control style, layout and input runtime.
 * NumericValue remains authoritative through its binding. Paint reads only prepared presentation.
 */
class Slider : public Control {
public:
  /** @param width Complete receiving text policy used by the paired ASCII track/thumb glyphs.
   * @param options Intrinsic extent, page increment count, paint cap and user-mutation policy.
   * @param style Owning manual appearances beneath ordinary theme rules.
   * @param policy Owning layout constraints validated by the layout owner.
   * Constructor arguments and glyph construction may throw; tree emplacement translates internal failures.
   */
  explicit Slider(WidthPolicy width = {}, SliderOptions options = {}, ControlStyle style = {},
                  SizePolicy policy = DEFAULT_CONTENT_POLICY);

  /** @param available Offered extent, ignored for intrinsic width.
   * @return Configured width and height one, or INVALID_ARGUMENT for invalid options.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;

  /** @param local Parent-local allocation; only the extent is retained.
   * @return Success or INVALID_GEOMETRY; no model reads or callbacks occur.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;

  /** @param context Synchronous sink borrowing this widget's glyphs through the frame.
   * @return Success, WOULD_BLOCK above the paint cap, or the unchanged sink error.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;

  /** @return Last synchronized finite numeric value; borrow the widget only inside checked tree access. */
  double value() const noexcept {
    return value_.value();
  }

  /** @return Last synchronized validated bounds and user increment. */
  NumericRange range() const noexcept {
    return value_.range();
  }

  /** @return Whether user mutation is currently prohibited; focus remains available. */
  bool readOnly() const noexcept {
    return options_.readOnly;
  }

private:
  friend class SliderBinding;
  friend class ManagedScene;
  friend std::expected<void, TreeError> setSliderReadOnly(WidgetTree &, Node<Slider>, bool);
  void display(NumericValue value) noexcept;
  void placeThumb() noexcept;

  NumericValue value_;
  SliderOptions options_;
  GraphemeText track_;
  GraphemeText thumb_;
  CellRect bounds_;
  double position_{};
  std::int32_t thumbColumn_{};
};

/** @param tree Borrowed UI tree. @param node Live slider. @param readOnly New user-mutation policy.
 * @return Checked tree result; equal settings do not invalidate. Programmatic writes remain allowed.
 */
std::expected<void, TreeError> setSliderReadOnly(WidgetTree &tree, Node<Slider> node, bool readOnly);

/** @brief Exact numeric Property operation receipt; unexpected means no commit. */
using SliderResult = std::expected<PropertyUpdate<NumericValue>, TreeError>;

/** @brief Own one native slider binding without a second scheduler or numeric model runtime.
 * Tree/router/scheduler must outlive this UI-affine token. Admitted callbacks retain their state.
 */
class SliderBinding {
public:
  /** @brief Retire future input and data delivery on UI; executing callbacks keep their captures. */
  ~SliderBinding();
  /** @param other UI-owned token emptied by this transfer. */
  SliderBinding(SliderBinding &&other) noexcept;
  /** @param other UI-owned token installed before old captures are retired.
   * @return This token; callbacks cannot observe a partially replaced binding.
   */
  SliderBinding &operator=(SliderBinding &&other) noexcept;
  SliderBinding(const SliderBinding &) = delete;
  SliderBinding &operator=(const SliderBinding &) = delete;

  /** @brief Adopt the latest model and detect interrupted drags before a frame.
   * @return Success or typed tree/model/publication error; user callbacks are deferred to the data lane.
   */
  std::expected<void, TreeError> sync();

private:
  struct State;
  friend class ManagedScene;
  friend std::expected<SliderBinding, TreeError> bindSlider(WidgetTree &, EventRouter &, Node<Slider>,
                                                            std::shared_ptr<Property<NumericValue>>,
                                                            std::function<void(SliderResult)>,
                                                            std::function<void(const SliderOperationEvent &)>);
  SliderBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription);
  void retain(std::shared_ptr<void> record);
  std::shared_ptr<void> retained() const;

  std::shared_ptr<State> state_;
  ActionBinding action_;
  Subscription subscription_;
};

/** @param tree Borrowed UI tree, required to outlive the binding and model.
 * @param router Borrowed router for this tree, required to outlive the binding.
 * @param node Live slider receiving input and synchronized presentation.
 * @param model Required shared owner of an invariant-preserving numeric Property.
 * @param changed Required exact user-commit observation; external writes and no-ops never invoke it.
 * @param finished Optional owning operation observation, once per completed/cancelled/conflicted gesture.
 * @return Scoped native binding or existing tree/router/model admission/allocation errors.
 * Callbacks may remove their node or destroy the token. Removal suppresses subsequent delivery.
 * Callback construction before entry can throw. Notification rejection never rolls back a model commit.
 */
std::expected<SliderBinding, TreeError> bindSlider(WidgetTree &tree, EventRouter &router, Node<Slider> node,
                                                   std::shared_ptr<Property<NumericValue>> model,
                                                   std::function<void(SliderResult)> changed,
                                                   std::function<void(const SliderOperationEvent &)> finished = {});
} // namespace portals
