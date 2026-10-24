// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>

#include <functional>
#include <string>

namespace portals {
namespace ui::detail {
struct Access;
}

/** @brief Presentation of the most recent explicit or editor-publication validation. */
enum class FieldStatus {
  UNCHECKED, ///< No validation has been requested; required is still shown in the label.
  VALID,     ///< The checked candidate satisfied all configured synchronous rules.
  INVALID,   ///< The checked candidate was rejected; the editor retains its draft.
  PENDING    ///< Explicit presentation only; does not disable editing or schedule work.
};

/** @brief Owning validation presentation; messages remain meaningful without color. */
struct FieldValidation {
  /** @brief Keep owning transfers fallible on checked standard libraries. */
  ~FieldValidation() = default;

  FieldStatus status{FieldStatus::UNCHECKED}; ///< Last requested presentation.
  std::string message;                        ///< Optional owning UTF-8 explanation.

  /** @param message Optional explanation of acceptance. @return Owning accepted presentation. */
  static FieldValidation valid(std::string message = {});

  /** @param message Explanation of rejection; empty selects the standard error text. @return Owning rejection. */
  static FieldValidation invalid(std::string message = {});

  /** @param message Optional explanation of in-progress work. @return Explicit pending presentation. */
  static FieldValidation pending(std::string message = {});
};

/** @brief Manual style/layout defaults; semantic field rules are configured separately. */
struct FormFieldAppearance {
  ControlStyle labelStyle{};   ///< Manual label defaults beneath theme rules.
  ControlStyle helpStyle{};    ///< Manual help defaults beneath theme rules.
  ControlStyle messageStyle{}; ///< Manual validation defaults beneath theme rules.
  LinearOptions layout{};      ///< Existing vertical layout settings.
  SizePolicy policy{};         ///< Existing container size constraints.
};

/** @brief Owning composition settings for an explicitly supplied custom editor. */
struct FormFieldOptions {
  /** @brief Preserve fallible owning string transfers at the checked mount boundary. */
  ~FormFieldOptions() = default;

  std::string help; ///< Optional UTF-8 explanation below the editor.
  bool required{};  ///< Append the visible required marker; custom editors retain their own value rules.
  std::string requiredMessage{"A value is required"}; ///< Required rejection explanation for standard text fields.
  FormFieldAppearance appearance{};                   ///< Manual style and layout defaults.
};

namespace detail {
struct FormFieldRecord;
}

/** @brief Retained label/editor/help/message composition using the ordinary vertical layout.
 * The tree owns the field and its children. No model, scheduler or transaction is created by this widget.
 * Read snapshots only inside tree.read; mutating free functions are UI-only outside frames.
 */
class FormField : public Column {
public:
  /** @param options Owning layout settings and manual defaults; child recipes supply the actual captions. */
  explicit FormField(FormFieldAppearance options = {});

  /** @brief Retire any borrowed validation bridge before child ownership is reclaimed. */
  ~FormField() override;

  /** @return Most recent owning validation presentation; copying its message may allocate. */
  FieldValidation validation() const;

  /** @return Observing editor root, possibly stale after a child is independently removed. */
  Node<Widget> editor() const;

private:
  friend struct ui::detail::Access;
  friend std::expected<void, TreeError> setFormFieldValidation(WidgetTree &, Node<FormField>, FieldValidation);
  std::shared_ptr<detail::FormFieldRecord> record_;
};

/** @param tree Existing UI tree, borrowed only during this call.
 * @param node Live field identity.
 * @param value Owning explicit presentation; this is not an asynchronous completion endpoint.
 * @return Success after updating readable text, or the original tree/text/allocation failure.
 * Pending never changes editor eligibility. Invalid UTF-8 is rejected before the old presentation changes.
 */
std::expected<void, TreeError> setFormFieldValidation(WidgetTree &tree, Node<FormField> node, FieldValidation value);

} // namespace portals
