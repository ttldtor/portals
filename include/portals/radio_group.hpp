// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/collection.hpp>
#include <portals/controls.hpp>
#include <portals/text_value.hpp>

namespace portals {
/** @brief Owning cold caption and eligibility for ordinary radio model creation; no hand-written ID required. */
struct RadioOption {
  TextValue text;     ///< Literal or resource key, resolved with the receiving scene policy.
  bool enabled{true}; ///< User input eligibility; selection is preserved when disabled.
  bool visible{true}; ///< Whether the option occupies a row.

  /** @param caption Owning literal bytes. @param enabled Input eligibility. @param visible Row visibility. */
  RadioOption(std::string_view caption, bool enabled = true, bool visible = true)
      : text(caption), enabled(enabled), visible(visible) {
  }

  /** @param caption Owning literal/resource expression. @param enabled Eligibility. @param visible Row visibility. */
  RadioOption(TextValue caption, bool enabled = true, bool visible = true)
      : text(std::move(caption)), enabled(enabled), visible(visible) {
  }

  /** @brief Release the cold owning expression; copies retain ordinary fallible value semantics. */
  ~RadioOption() = default;
};

/** @brief One immutable, validated single-line radio caption and its input eligibility. */
struct RadioItem {
  GraphemeText text;  ///< Owning text measured under the receiving scene's policy.
  bool enabled{true}; ///< Disabled items retain their selected value but reject input.
  bool visible{true}; ///< Hidden items occupy no row and reject input.
  /** @return Whether caption, width policy and eligibility all match the unnamed other item. */
  bool operator==(const RadioItem &) const = default;
};

/** @brief Coherent immutable resident options and optional selection; copies share all row storage.
 * No mutable aliases escape. Empty construction represents no options and no selection.
 * Selection changes reuse the prepared row/index projection; retained values outlive the UI.
 */
class RadioGroupValue {
public:
  /** @brief Construct an empty value without allocating rows or selecting an item. */
  RadioGroupValue() = default;
  /** @brief Release shared rows; rvalue transfers preserve a valid shared-owner source, like RowCollection. */
  ~RadioGroupValue() = default;
  /** @param rows Owning stable IDs and validated captions with one common width policy.
   * @param selected Existing full ID, including disabled/hidden rows, or no selection.
   * @return Prepared value or INVALID_ARGUMENT/INVALID_GEOMETRY/NO_MEMORY.
   */
  static std::expected<RadioGroupValue, TreeError> from(RowCollection<RadioItem> rows,
                                                        std::optional<RowId> selected = {});
  /** @param selected Existing full ID or no selection. @return Cheap coherent replacement or INVALID_ARGUMENT. */
  std::expected<RadioGroupValue, TreeError> withSelection(std::optional<RowId> selected) const;
  /** @return Borrowed immutable rows, valid while this value remains alive. */
  std::span<const ModelRow<RadioItem>> rows() const noexcept;
  /** @return Optional stable identity, independent of position and caption. */
  std::optional<RowId> selectedId() const noexcept;
  /** @return Selected caption bytes borrowed from this value; empty when unselected. */
  std::string_view selectedText() const noexcept;
  /** @return First enabled visible ID, or no selection. */
  std::optional<RowId> firstEligible() const noexcept;
  /** @param other Independent owning value. @return Equal ordered rows and selection; shared sources compare cheaply.
   */
  bool operator==(const RadioGroupValue &other) const;

private:
  friend class RadioGroup;
  friend class ManagedScene;
  friend class RadioGroupBinding;
  struct Data;
  std::shared_ptr<const Data> data_;
  std::optional<RowId> selected_;
  bool shorthand_{};
};

/** @brief User selection publication policy; active navigation remains local to the group. */
enum class RadioSelectionMode {
  ON_ACTIVATION, ///< Arrows move active; Space/Enter selects.
  IMMEDIATE      ///< Eligible keyboard movement also selects.
};

/** @brief Ordinary one-focus-stop, model-owned resident radio group with clipped row painting. */
class RadioGroup : public SizedWidget {
public:
  /** @param style Manual normal/focused/disabled defaults. @param policy Ordinary cell sizing constraints.
   * @param readOnly Prohibit user writes while retaining focus, navigation and programmatic changes.
   * @param selectionMode Keyboard navigation publication policy.
   * @param widthPolicy Receiving scene's complete width policy; mismatched model text is rejected at sync.
   */
  explicit RadioGroup(ControlStyle style = {}, SizePolicy policy = DEFAULT_CONTENT_POLICY, bool readOnly = false,
                      WidthPolicy widthPolicy = {},
                      RadioSelectionMode selectionMode = RadioSelectionMode::ON_ACTIVATION);
  /** @brief Tree-owned widgets cannot copy their live geometry context. */
  RadioGroup(const RadioGroup &) = delete;
  /** @brief Tree-owned widgets cannot be assigned. */
  RadioGroup &operator=(const RadioGroup &) = delete;
  /** @brief A bound widget's address and tree identity remain fixed. */
  RadioGroup(RadioGroup &&) = delete;
  /** @brief A bound widget cannot transfer its tree context. */
  RadioGroup &operator=(RadioGroup &&) = delete;
  /** @param available Offered extent; intrinsic size uses prepared visible rows.
   * @return Cached natural cell size, without reading or formatting the model.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Parent-relative allocation. @return Success or INVALID_GEOMETRY before changing bounds. */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Synchronous sink borrowing commands through the enclosing tree frame.
   * @return Original geometry/sink error; no model access or user callback occurs here.
   * Bound groups visit only cumulative clipped rows; an unbound group is empty and uses its local allocation.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual defaults; selected markers inherit focused beneath theme layers. */
  ComputedStyle styleDefaults() const override;

  /** @return Actual view-only policy for orthogonal style matching. */
  bool styleReadOnly() const noexcept override;
  /** @return Last synchronized immutable presentation; the bound Property remains authoritative. */
  RadioGroupValue value() const;

private:
  friend class RadioGroupBinding;
  friend std::expected<void, TreeError> syncRadioGroup(WidgetTree &, Node<RadioGroup>, Node<Widget>);
  std::expected<NodeGeometry, TreeError> geometry(bool painting = false) const;
  WidgetTree *tree_{};
  Node<Widget> node_;
  ControlStyle style_;
  WidthPolicy widthPolicy_;
  CellRect bounds_;
  RadioGroupValue value_;
  std::optional<RowId> active_;
  bool readOnly_{};
  bool enabled_{true};
  bool focused_{};
  RadioSelectionMode selectionMode_{RadioSelectionMode::ON_ACTIVATION};
};

/** @brief Exact selection commit and observer-publication receipt, or a precommit TreeError. */
using RadioGroupResult = std::expected<PropertyUpdate<RadioGroupValue>, TreeError>;

/** @brief UI-only owning input/subscription registration; destroy before its borrowed tree/router. */
class RadioGroupBinding {
public:
  /** @brief Disconnect this registration on its creating UI thread. */
  ~RadioGroupBinding();
  /** @param other Registration transferred without changing its source or live node identity. */
  RadioGroupBinding(RadioGroupBinding &&other) noexcept;
  /** @param other Complete replacement installed before retiring old captures. @return This registration. */
  RadioGroupBinding &operator=(RadioGroupBinding &&other) noexcept;
  /** @brief Registrations have one owner. */
  RadioGroupBinding(const RadioGroupBinding &) = delete;
  /** @brief Copying cannot duplicate a live input registration. */
  RadioGroupBinding &operator=(const RadioGroupBinding &) = delete;
  /** @return Synchronize latest authoritative snapshot or original tree failure. */
  std::expected<void, TreeError> sync();
  /** @return Exact current owning model value or CLOSED/WRONG_THREAD. */
  std::expected<RadioGroupValue, TreeError> value() const;
  /** @param id Existing ID or no selection. @return Exact programmatic receipt; no user callback. */
  RadioGroupResult select(std::optional<RowId> id);

private:
  friend std::expected<RadioGroupBinding, TreeError> bindRadioGroup(WidgetTree &, EventRouter &, Node<RadioGroup>,
                                                                    std::shared_ptr<Property<RadioGroupValue>>,
                                                                    std::function<void(RadioGroupResult)>);
  struct State;
  RadioGroupBinding(std::shared_ptr<State>, ActionBinding, Subscription);
  std::shared_ptr<State> state_;
  ActionBinding action_;
  Subscription subscription_;
};

/** @param tree Borrowed live UI tree. @param router Existing router, no additional runtime.
 * @param node Live group without another binding. @param model Authoritative coherent state.
 * @param changed Required callback receiving the exact commit/delivery receipt, including failures.
 * @return Owning registration or original admission/allocation/tree error. Failed admission leaves no registration.
 */
std::expected<RadioGroupBinding, TreeError> bindRadioGroup(WidgetTree &tree, EventRouter &router, Node<RadioGroup> node,
                                                           std::shared_ptr<Property<RadioGroupValue>> model,
                                                           std::function<void(RadioGroupResult)> changed);
/** @param tree Borrowed UI owner. @param node Live group. @param focused Current router focus.
 * @return Eligibility/focus paint synchronization or original tree error.
 */
std::expected<void, TreeError> syncRadioGroup(WidgetTree &tree, Node<RadioGroup> node, Node<Widget> focused);
} // namespace portals
