// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/text_metadata.hpp>
#include <portals/line_edit.hpp>
#include <portals/password_value.hpp>

namespace portals {
/** @brief Constant-glyph presentation of protected editor values. */
enum class PasswordDisplayMode {
  PASSWORD, ///< One ASCII '*' per grapheme; exposes the grapheme count.
  NO_ECHO   ///< No value/selection glyphs, intrinsic width one and fixed-origin caret.
};
/** @brief Explicit admission of plaintext supplied by the system clipboard/input decoder. */
enum class PasswordPastePolicy {
  DISABLED, ///< Consume paste without reading the service or inspecting supplied bytes.
  ENABLED   ///< Accept bounded external plaintext; wipe the owned service payload after copying.
};

/** @brief Owning protected-editor policy; no plaintext history, copying or cutting exists. */
struct PasswordOptions {
  TextLimits limits{4096, 4096, 4096, 8192}; ///< Whole UTF-8 value bounds; edits are never truncated.
  WidthPolicy widthPolicy{};                 ///< Unicode measurement policy; masks always use ASCII '*'.
  ControlStyle style{};                      ///< Normal, focused and disabled appearance.
  CellStyle selection{RgbColor{0, 0, 0}, RgbColor{180, 200, 255}}; ///< Selected mask appearance.
  CellStyle cursor{{}, {}, true, false, true};                     ///< Software caret appearance.
  EditMode editMode{EditMode::IMMEDIATE}; ///< Publication boundary shared by input and setValue.
  bool readOnly{};                        ///< Block user edits; explicit setValue remains available.
  PasswordPastePolicy pastePolicy{PasswordPastePolicy::DISABLED}; ///< Clipboard paste requires explicit opt-in.
  PasswordDisplayMode displayMode{PasswordDisplayMode::PASSWORD}; ///< NO_ECHO reveals no count through layout.
  std::optional<std::string> placeholder; ///< Public, nonsecret placeholder; never populated from a secret.
  SecureStringOptions securePolicy{};     ///< Mandatory native encryption and bounded protected storage.
};

/** @brief Protected editor input rules beyond UTF-8 validity. */
enum class PasswordInputError {
  LINE_SEPARATOR ///< Valid Unicode contains CR, LF, LINE SEPARATOR or PARAGRAPH SEPARATOR.
};

/** @brief Owning original causes, including independently retained callback/cleanup diagnostics. */
using PasswordFailure = std::variant<TreeError, TextError, PasswordInputError, SecureStringFailure,
                                     PasswordValueAccessFailure<SecureStringFailure>,
                                     PasswordValueAccessFailure<TextError>, PasswordValueAccessFailure<std::monostate>>;

/** @brief Outcome of a protected edit or publication boundary. */
enum class PasswordStatus {
  EDITED,            ///< Accepted local protected draft, selection change or no-op.
  COMMITTED,         ///< Successful Property::set; inspect the exact protected receipt.
  CANCELLED,         ///< Checked adoption of the latest model identity.
  CONFLICT,          ///< External model revision changed since this draft began.
  INVALID_TEXT,      ///< Invalid UTF-8 or a line separator; no partial input accepted.
  LIMIT_EXCEEDED,    ///< Whole-value Unicode or protected byte bound rejected the edit.
  VALIDATION_FAILED, ///< Semantic predicate rejected the retained protected draft.
  CLIPBOARD_FAILED,  ///< Opted-in service absent or rejected the read.
  FAILED             ///< Original technical/secure cause retained in failure.
};

/** @brief Secret-free editing outcome; a commit owns only protected immutable snapshots. */
struct PasswordResult {
  PasswordStatus status{PasswordStatus::EDITED};       ///< Editing/publication outcome.
  std::optional<PropertyUpdate<PasswordValue>> commit; ///< Exact successful receipt, including partial delivery.
  std::optional<PasswordFailure> failure; ///< Original typed technical/Unicode failure without secret messages.
  LineEditTrigger trigger{LineEditTrigger::INPUT}; ///< Routed input, fluent operation or synchronous blur.
  std::optional<TreeError> callbackError; ///< Result-delivery exception, independent of publication/primary failure.
};

/** @brief Protected single-line editor with numeric metadata and constant-mask frame data.
 * The draft and snapshots are immutable PasswordValue identities. Frames do not borrow,
 * decrypt or call application services. PASSWORD reveals grapheme count; NO_ECHO measures
 * one cell and fixes the caret at its origin. Input-event and opted-in clipboard buffers
 * are external plaintext; owned clipboard payload bytes are wiped before release.
 */
class Password final : public SizedWidget {
public:
  /** @brief Construct an unbound empty presentation.
   * @param options Owning checked presentation/security policy; invalid settings throw invalid_argument.
   * @param policy Owning layout size policy.
   * @throws std::bad_alloc On public placeholder or initial mask allocation.
   */
  explicit Password(PasswordOptions options = {}, SizePolicy policy = {});
  /** @param available Offered extent, unused for intrinsic measurement.
   * @return Mask/placeholder width plus caret, or one in NO_ECHO; checked geometry error on overflow.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Parent-relative rectangle whose extent is retained.
   * @return Success or INVALID_GEOMETRY; scrolling uses numeric grapheme metadata only.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Synchronous command sink borrowed for this frame only.
   * @return Original sink failure; commands contain only public placeholder or constant mask bytes.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Active selection endpoint as a UTF-8 byte boundary, without plaintext access. */
  std::size_t cursor() const noexcept;
  /** @return Fixed selection endpoint as a UTF-8 byte boundary. */
  std::size_t anchor() const noexcept;
  /** @return Whether a protected draft awaits publication/cancellation. */
  bool dirty() const noexcept;
  /** @return Manual control, selection and caret appearance beneath theme declarations. */
  ComputedStyle styleDefaults() const override;

private:
  friend class PasswordBinding;
  friend class ManagedScene;
  friend struct ui::detail::RefAccess;
  std::int32_t cellAt(std::size_t byte) const noexcept;
  void revealCursor() noexcept;
  PasswordOptions options_;
  PasswordValue draft_;
  detail::TextMetadata metadata_;
  std::unique_ptr<GraphemeText> masked_;
  std::unique_ptr<GraphemeText> placeholder_;
  CellRect bounds_;
  std::size_t cursor_{};
  std::size_t anchor_{};
  std::int32_t scroll_{};
  bool dirty_{};
  bool focused_{};
  bool enabled_{true};
};

/** @brief Scoped action/model ownership on the tree UI thread, outside frames.
 * Tree, router and scheduler outlive the binding. Admitted operations retain their state
 * across callback removal. Deferred drafts retain their baseline revision and never
 * overwrite external changes. No operation exposes an ordinary text draft or history.
 */
class PasswordBinding {
public:
  /** @brief Disconnect future actions/subscriptions; admitted callbacks retain owners. */
  ~PasswordBinding();
  /** @param other Binding emptied by ownership transfer on UI. */
  PasswordBinding(PasswordBinding &&other) noexcept;
  /** @param other Replacement installed before retiring reentrant old captures.
   * @return This binding; self-move preserves state.
   */
  PasswordBinding &operator=(PasswordBinding &&other) noexcept;
  PasswordBinding(const PasswordBinding &) = delete;
  PasswordBinding &operator=(const PasswordBinding &) = delete;
  /** @param focused Current router focus; empty means no focus.
   * @return Checked synchronization/focus failure, preserving typed secure causes.
   */
  std::expected<void, PasswordFailure> sync(Node<Widget> focused);
  /** @return Confirmation outcome; semantic rejection preserves the protected draft.
   * Delivers onResult once after consistency is established. Technical failures are returned and reported once.
   * If result delivery also throws, the returned FAILED result retains both failure and callbackError.
   */
  std::expected<PasswordResult, PasswordFailure> confirm();
  /** @return Outcome after checked adoption of the latest model; immediate published edits are not undone.
   * Delivers onResult once; a failed secure access can invalidate the previous shared value.
   * Combined operation/delivery failure returns an owning FAILED result with both causes.
   */
  std::expected<PasswordResult, PasswordFailure> cancel();
  /** @param value Protected candidate retained by identity, checked against this editor's policy.
   * @return Same publication/validation policy as user input; readOnly does not block this explicit operation.
   * Delivers onResult once. No-op identity replacement does not invalidate or publish.
   * Combined operation/delivery failure returns an owning FAILED result with both causes.
   */
  std::expected<PasswordResult, PasswordFailure> setValue(PasswordValue value);

private:
  struct State;
  friend class ManagedScene;
  friend struct ui::detail::RefAccess;
  friend std::expected<PasswordBinding, PasswordFailure>
  bindPassword(WidgetTree &, EventRouter &, Node<Password>, std::shared_ptr<Property<PasswordValue>>,
               std::function<void(PasswordResult)>, std::shared_ptr<Clipboard>, std::function<bool(SecretView)>);
  PasswordBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription);
  std::shared_ptr<State> state_;
  ActionBinding action_;
  Subscription subscription_;
};

/** @brief Bind and securely inspect the initial model before publishing presentation.
 * @param tree Borrowed UI owner, outliving binding and model.
 * @param router Borrowed router for tree, outliving binding.
 * @param node Live protected editor; one action binding per node is permitted.
 * @param model Shared protected model owner; external replacements are checked before input.
 * @param onResult Required owning UI callback; may remove this node or binding.
 * @param clipboard Optional shared service, called only with explicit ENABLED paste policy.
 * @param validate Optional owning semantic validator, borrowing plaintext synchronously; false retains the draft.
 * @return Scoped binding or original typed setup failure. Exceptions translate to TreeError without messages.
 */
std::expected<PasswordBinding, PasswordFailure> bindPassword(WidgetTree &tree, EventRouter &router, Node<Password> node,
                                                             std::shared_ptr<Property<PasswordValue>> model,
                                                             std::function<void(PasswordResult)> onResult,
                                                             std::shared_ptr<Clipboard> clipboard = {},
                                                             std::function<bool(SecretView)> validate = {});
} // namespace portals
