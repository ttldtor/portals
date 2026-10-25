// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/form_field.hpp>
#include <portals/ui_controls.hpp>
#include <portals/ui_layout.hpp>

#include <stop_token>

namespace portals::ui {

/** @brief Byte budgets for an asynchronous field request and its final message; no silent truncation. */
struct FieldValidationLimits {
  std::size_t maxCandidateBytes{16 * 1024}; ///< Positive UTF-8 request byte limit; editor limits remain independent.
  std::size_t maxMessageBytes{4 * 1024};    ///< Positive complete result-message byte limit.
};

/** @brief Owning worker-safe rule. Captures and their deleters must be thread-safe and transitively immutable.
 * The candidate view expires on return; only VALID/INVALID are successful final results. Do not capture UI refs,
 * mutable models, scene owners or coroutine storage. Cooperative stop callbacks must be short and nonthrowing.
 */
using AsyncFieldValidator = std::function<std::expected<FieldValidation, TreeError>(std::string_view, std::stop_token)>;

/** @brief Standard text editor configuration; validation observes the normal publication boundaries. */
struct FormFieldConfig {
  /** @brief Preserve fallible owning argument transfers before mount. */
  ~FormFieldConfig() = default;

  std::string help; ///< Explanation below the editor; empty omits its row.
  bool required{};  ///< Show a required marker and reject an empty publication candidate.
  std::function<FieldValidation(std::string_view)>
    validate; ///< Synchronous owning rule; borrowed candidate expires on return.
  std::string requiredMessage{"A value is required"}; ///< Explanation of an empty required candidate.
  LineEditConfig editor{}; ///< Optional ordinary editor configuration, including EditMode/history/display policy.
  FormFieldAppearance appearance{};    ///< Manual composition style/layout defaults; contains no validation policy.
  AsyncFieldValidator validateAsync{}; ///< Optional worker rule after synchronous checks; never called during paint.
  FieldValidationLimits validationLimits{}; ///< Independent positive worker request/result byte bounds.
};

namespace detail {
/** @param caption Owning label bytes. @param editor Erased real child recipe, consumed once.
 * @param options Owning appearances and required marker; no editor value is assumed.
 * @return Ordinary retained composition, with the same transaction and focus route as a standard field.
 */
Description<FormField> customFormField(std::string caption, Description<Widget> editor, FormFieldOptions options);

/** @param target Weak original field observation. @return Checked snapshot or exact liveness/affinity error. */
std::expected<FieldValidation, ManagedError> fieldValidation(const std::shared_ptr<RefTarget> &target);

/** @param target Weak field observation. @return Text editor observation or a slot retaining its exact checked failure.
 */
Ref<LineEdit> fieldEditor(const std::shared_ptr<RefTarget> &target);

/** @param target Weak field observation. @return True for a valid current draft; no model commit occurs. */
std::expected<bool, ManagedError> validateField(const std::shared_ptr<RefTarget> &target);

/** @param target Weak field observation. @param value Owning explicit presentation.
 * @return Checked update; no deferred completion or stale-result admission is implied.
 */
std::expected<void, ManagedError> displayField(const std::shared_ptr<RefTarget> &target, FieldValidation value);
} // namespace detail

/** @param caption UTF-8 label copied now; required adds a readable marker at mount.
 * @param initial Initial UTF-8 draft copied now, validated by the ordinary editor mount.
 * @param config Owning rules/options. Required rejects empty text before custom rules; then the editor's optional
 * bool validator runs. Rules may throw/remove the field; existing editor callback/error/lifetime policy applies.
 * Synchronous validation starts on normal editor publication or explicit Ref.validate(). For validateAsync,
 * only publication starts work; the synchronous Ref.validate() command rejects that configuration.
 * Rules never run during construction,
 * measure, paint or theme resolution. Deferred-mode dirty drafts keep the last validation message until checked.
 * @return Owning field recipe; the complete subtree rolls back on a mount failure.
 */
Description<FormField> formField(std::string_view caption, std::string_view initial = {}, FormFieldConfig config = {});

/** @param reference Existing weak field slot, published only after the whole composition mounts.
 * @param caption Owning label input. @param initial Owning initial editor text. @param config Owning rules/options.
 * @return Standard field recipe using the same Ref family as ordinary widgets.
 */
inline Description<FormField> formField(Ref<FormField> reference, std::string_view caption,
                                        std::string_view initial = {}, FormFieldConfig config = {}) {
  return formField(caption, initial, std::move(config)).reference(std::move(reference));
}

/** @tparam T Public Widget subtype supplied by the consuming editor recipe.
 * @param caption UTF-8 label copied immediately. @param editor Real custom editor root; no private core switch.
 * @param options Owning composition settings; required is descriptive for a nonstandard editor.
 * @return Retained composition with a nested focus route; custom validation is explicit via Ref.display().
 * Text-only Ref.editor()/text()/validate() reject custom non-LineEdit roots with INVALID_ARGUMENT.
 */
template <class T>
Description<FormField> formField(std::string_view caption, Description<T> editor, FormFieldOptions options = {}) {
  return detail::customFormField(std::string(caption), detail::Access::erase(std::move(editor)), std::move(options));
}

} // namespace portals::ui
