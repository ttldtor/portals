// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/line_edit.hpp>
#include <portals/numeric.hpp>

namespace portals {
/** @brief Numeric editor with one ordinary LineEdit and two nonfocusable step buttons.
 * The containing tree owns all three child nodes. NumericValue is the only committed source.
 */
class SpinBox : public Row {
public:
  /** @param policy Root sizing constraints; the ordinary editor prefers eight cells plus two arrows. */
  explicit SpinBox(SizePolicy policy = DEFAULT_CONTENT_POLICY) : Row({}, policy) {
  }

  /** @return The one editor identity; empty until managed binding succeeds. */
  Node<LineEdit> editor() const noexcept {
    return editor_;
  }

private:
  friend class ManagedScene;
  Node<LineEdit> editor_;
};

/** @brief Existing text editing outcome and its exact numeric receipt, if publication occurred.
 * EDITED means a draft was accepted; it does not imply a numeric commit. Accepted spellings are preserved.
 */
struct SpinBoxResult {
  LineEditResult editing; ///< Owning text/history outcome, including validation/conflict and technical errors.
  std::optional<PropertyUpdate<NumericValue>> commit; ///< Exact numeric set receipt; never a later snapshot.
};

/** @brief UI-affine owner of the editor source and nonfocusable arrow actions. */
class SpinBoxBinding {
public:
  ~SpinBoxBinding(); ///< Disconnect future input/data delivery; admitted callbacks retain their owners.
  /** @param other Token transferred on UI outside frames. */
  SpinBoxBinding(SpinBoxBinding &&other) noexcept;
  /** @param other Token replacing this binding on UI outside frames. @return This token. */
  SpinBoxBinding &operator=(SpinBoxBinding &&other) noexcept;
  SpinBoxBinding(const SpinBoxBinding &) = delete;
  SpinBoxBinding &operator=(const SpinBoxBinding &) = delete;
  /** @param focused Current router focus. @return Existing checked editor synchronization result. */
  std::expected<void, TreeError> sync(Node<Widget> focused);
  /** @param value Finite in-range replacement, following the editor's EditMode.
   * @return Exact draft/commit observation or a pre-accept error. Read-only permits programmatic changes.
   */
  std::expected<SpinBoxResult, TreeError> setValue(double value);

private:
  friend class ManagedScene;
  struct State;
  friend std::expected<SpinBoxBinding, TreeError> bindSpinBox(WidgetTree &, EventRouter &, Node<LineEdit>, Node<Button>,
                                                              Node<Button>, std::shared_ptr<Property<NumericValue>>,
                                                              std::uint64_t, std::function<void(SpinBoxResult)>,
                                                              LineEditEvents);
  explicit SpinBoxBinding(std::shared_ptr<State> state);
  std::shared_ptr<LineEditBinding> editorBinding() const;
  bool ownsPart(Node<Widget> node) const noexcept;
  void retain(std::shared_ptr<void> record);
  std::shared_ptr<void> retained() const;
  std::shared_ptr<State> state_;
};

/** @brief Bind pre-mounted editor/decrement/increment children of one SpinBox.
 * @param tree Borrowed UI tree outliving the binding. @param router Borrowed matching router.
 * @param editor The single ordinary editor. @param decrease Nonfocusable plain decrement button.
 * @param increase Nonfocusable plain increment button. @param model Required authoritative numeric source.
 * @param pageSteps Positive number of increments per PageUp/PageDown.
 * @param result Required owning result observer; callbacks may remove nodes or destroy the binding.
 * @param events Existing typed text and confirmation observations.
 * @return Scoped binding or the original typed admission/allocation error.
 */
std::expected<SpinBoxBinding, TreeError> bindSpinBox(WidgetTree &tree, EventRouter &router, Node<LineEdit> editor,
                                                     Node<Button> decrease, Node<Button> increase,
                                                     std::shared_ptr<Property<NumericValue>> model,
                                                     std::uint64_t pageSteps, std::function<void(SpinBoxResult)> result,
                                                     LineEditEvents events = {});
} // namespace portals
