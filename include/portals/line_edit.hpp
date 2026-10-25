// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>
#include <portals/text_history.hpp>

#include <source_location>

namespace portals {
enum class AsyncError;
enum class BackgroundError;

namespace detail {
struct LineEditValidationAccess;
struct LineEditValidationGate;
} // namespace detail

namespace ui::detail {
struct RefAccess;
}

/** @brief When an accepted editing candidate is validated and published to its model. */
enum class EditMode {
  IMMEDIATE,     ///< Publish each semantic-valid text change; invalid intermediate text remains visible.
  ON_COMPLETION, ///< Keep a draft until Enter, programmatic activation or focus loss.
  EXPLICIT       ///< Keep a draft until Enter or programmatic activation; focus loss preserves it.
};

/** @brief Immutable presentation and outbound clipboard policy; plaintext model/history remain application-owned. */
enum class LineEditDisplayMode {
  NORMAL,   ///< Render the draft using its actual grapheme widths; copying and cutting retain normal behavior.
  PASSWORD, ///< Render one ASCII '*' per grapheme, revealing its count; suppress outbound copy/cut.
  NO_ECHO   ///< Render no value or selection, with a fixed-origin caret and intrinsic width one; suppress copy/cut.
};

/** @brief History policy of an explicit programmatic text change. */
enum class ProgrammaticChange {
  RECORD,        ///< Record one separate reversible operation; equal text preserves Redo.
  RESET_BASELINE ///< Discard history after accepting this baseline, even when bytes are equal.
};

/** @brief Owning exact editor shortcut; only identities surviving router key translation are accepted. */
struct EditorChord {
  InputKey key{InputKey::CHARACTER}; ///< CHARACTER, INSERT, PAGE_UP/DOWN or F1 through F12.
  char32_t scalar{};                 ///< Valid Unicode scalar for CHARACTER; zero for other keys.
  InputModifiers modifiers{};        ///< Exact modifiers; ASCII character case is normalized.
};

/** @brief Number of independently configurable Redo aliases. */
inline constexpr std::size_t LINE_EDIT_REDO_CHORD_COUNT = 2;

/** @brief Fixed-size owning shortcuts; nullopt disables an individual binding. */
struct LineEditHistoryShortcuts {
  std::optional<EditorChord> undo{EditorChord{InputKey::CHARACTER, U'z', {.control = true}}}; ///< Default Ctrl+Z.
  std::array<std::optional<EditorChord>, LINE_EDIT_REDO_CHORD_COUNT> redo{
    EditorChord{InputKey::CHARACTER, U'y', {.control = true}},
    EditorChord{InputKey::CHARACTER, U'z', {.shift = true, .control = true}}}; ///< Ctrl+Y and Ctrl+Shift+Z.
};

/** @brief Effective history commands available without adopting a model revision or changing presentation. */
struct HistoryAvailability {
  bool canUndo{}; ///< Enabled writable live editor has an Undo operation and an unchanged external baseline.
  bool canRedo{}; ///< Enabled writable live editor has a Redo operation and an unchanged external baseline.
};

namespace detail {
/** @brief Private admission receipt retained until the managed wrapper reports its own command failure. */
struct EditorCommandFeedback {
  bool active{};   ///< One command is admitted under the existing binding reentry guard.
  bool reported{}; ///< Its exact failure was actually recorded; independent callback errors do not set this flag.
  std::string_view operation;  ///< Compiler/static-owned command label, without dynamic allocation.
  std::source_location origin; ///< Public command call site, retained through outcome delivery.
};
} // namespace detail

/** @brief Observable editing or service outcome; rejection preserves the draft. */
enum class LineEditStatus {
  EDITED,            ///< A local draft or selection changed.
  COMMITTED,         ///< Confirmation completed Property::set; inspect its exact receipt.
  CANCELLED,         ///< Escape adopted the latest model snapshot.
  COPIED,            ///< Selected bytes were accepted by the clipboard service.
  CONFLICT,          ///< The model revision changed since this draft began.
  INVALID_TEXT,      ///< Invalid UTF-8 or a line separator was rejected in full.
  LIMIT_EXCEEDED,    ///< Input, cluster, grapheme or cell limit rejected the whole edit.
  VALIDATION_FAILED, ///< The commit validator rejected the complete draft.
  CLIPBOARD_FAILED,  ///< The injected service was absent or rejected the operation.
  FAILED,            ///< A tree, allocation or callback failure; inspect error.
  PENDING            ///< The accepted draft awaits asynchronous semantic validation; no model write occurred.
};

/** @brief Entry path that produced an editing or confirmation outcome. */
enum class LineEditTrigger {
  INPUT,  ///< Routed input or programmatic activation.
  FLUENT, ///< Accepted fluent text mutation.
  BLUR,   ///< Confirmation on synchronous focus loss.
  ASYNC_VALIDATION ///< Final result of an earlier asynchronous publication intent; no synthetic input event.
};

/** @brief Exact editing or confirmation result delivered on the UI thread. */
struct LineEditResult {
  LineEditStatus status{LineEditStatus::EDITED};       ///< Meaning of this operation.
  std::optional<PropertyUpdate<std::string>> commit{}; ///< Exact successful set receipt, including notification loss.
  std::optional<TreeError> error{}; ///< Underlying tree, allocation, callback or clipboard service failure.
  LineEditTrigger trigger{LineEditTrigger::INPUT}; ///< Entry path; independent of the confirmation status.
  std::optional<AsyncError> async{};               ///< Exact asynchronous transport/worker failure, when present.
  std::optional<BackgroundError> background{};     ///< Exact background admission failure, when present.
  std::optional<TextError> text{};                 ///< Original candidate/result text error, when present.
  const char *operation{}; ///< Static library operation label; never borrows user message storage.
  bool recoverable{}; ///< Report an asynchronous gate's technical failure without stopping the managed application.
};

/** @brief Accepted user text delta, including history commands; owns the exact existing operation result. */
struct TextEditedEvent {
  LineEditResult result; ///< Original result, including semantic rejection after accepting a draft.
};

/** @brief Boundary at which the user finishes an editing operation. */
enum class EditingFinishedReason {
  ENTER,     ///< Enter or the equivalent Action::ACTIVATE command.
  FOCUS_LOST ///< Synchronous loss of this editor's focus.
};

/** @brief Outcome of the existing publication policy at an editing boundary. */
enum class EditingFinishedOutcome {
  APPLIED,             ///< Property::set changed the committed model value.
  UNCHANGED,           ///< An equal set succeeded, or a clean editor lost focus without attempting publication.
  DEFERRED,            ///< Focus loss retained a dirty draft without attempting publication in this mode.
  VALIDATION_REJECTED, ///< The existing validator rejected the retained draft.
  CONFLICT,            ///< The model revision changed before publication.
  FAILED,              ///< Technical failure; inspect the original result's error.
  PENDING              ///< This boundary retained an exact asynchronous publication intent.
};

/** @brief Owning completion observation; does not perform another validation or model update. */
struct EditingFinishedEvent {
  EditingFinishedReason reason{EditingFinishedReason::ENTER};        ///< Independent completion boundary.
  EditingFinishedOutcome outcome{EditingFinishedOutcome::UNCHANGED}; ///< Publication policy outcome.
  std::optional<LineEditResult> result; ///< Exact attempted operation, absent when blur performed no attempt.
};

/** @brief Optional owning UI callbacks delivered after the legacy result, while the node remains live.
 * Callbacks may remove the node or binding. Node removal stops subsequent delivery. Reentrant focus changes
 * publish presentation without repeating this operation's completion or commit.
 */
struct LineEditEvents {
  std::function<void(const TextEditedEvent &)> textEdited;           ///< Actual accepted user byte changes only.
  std::function<void(const EditingFinishedEvent &)> editingFinished; ///< Enter and focus-loss boundaries in every mode.
};

/** @brief Injected UI clipboard boundary; called only from input handling, never from a frame.
 * The binding shares ownership. Implementations must return promptly and may reenter UI code.
 * Bytes remain data; no terminal escapes are interpreted. Exceptions become FAILED results.
 */
class Clipboard {
public:
  /** @brief Release the service on the UI owner after admitted calls finish. */
  virtual ~Clipboard() = default;
  /** @brief Obtain an owning bounded payload.
   * @param maxBytes Maximum accepted payload bytes; the caller also checks this bound.
   * @return Owning bytes, or a service-specific TreeError; no partial success is inferred.
   */
  virtual std::expected<std::string, TreeError> read(std::size_t maxBytes) = 0;
  /** @brief Copy selected bytes into the service.
   * @param bytes UTF-8 selection borrowed only until this call returns.
   * @return Success after accepting the whole selection, or a service-specific TreeError.
   */
  virtual std::expected<void, TreeError> write(std::string_view bytes) = 0;
};

/** @brief Owning bounds and appearances for a one-line editor. */
struct LineEditOptions {
  TextLimits limits{4096, 4096, 4096, 8192};   ///< Whole draft limits, never silent truncation.
  WidthPolicy widthPolicy{};                   ///< Immutable cell-width policy for this editor.
  ControlStyle style{};                        ///< Normal, focused and disabled appearance.
  CellStyle selection{{}, {}, true, true};     ///< Whole selected grapheme appearance.
  CellStyle cursor{{}, {}, true, false, true}; ///< Software caret cell; whole wide glyph when it fits.
  EditMode editMode{EditMode::IMMEDIATE};      ///< Confirmation policy shared by input, paste and fluent text edits.
  TextHistoryOptions history{};                ///< Enabled by default: 128 combined groups, 1 MiB retained snapshots.
  bool readOnly{}; ///< User edits and history commands are blocked; explicit programmatic text changes remain allowed.
  ProgrammaticChange programmaticChange{ProgrammaticChange::RECORD}; ///< Default fluent mutation history policy.
  LineEditHistoryShortcuts historyShortcuts{}; ///< Exact owning Undo/Redo shortcuts, validated at bind.
  LineEditDisplayMode displayMode{LineEditDisplayMode::NORMAL}; ///< Remount to change this presentation policy.
  std::optional<std::string>
    placeholder{}; ///< Owned single-line UTF-8 shown only for an empty draft, never model data.
};

namespace detail {
/** @brief Already checked owning placeholder transferred by declarative construction before state publication. */
struct LineEditPlaceholder {
  std::unique_ptr<GraphemeText> text; ///< Validated under the receiving editor's width policy and limits.
};
} // namespace detail

/** @brief UI-owned Unicode draft; mutate only through its scoped binding between frames.
 * Cursor/anchor are UTF-8 byte boundaries, while scrolling and painting use terminal cells.
 * Paint borrows owning text through the enclosing tree.frame and performs no service/model calls.
 * PASSWORD caches one ASCII '*' per grapheme; NO_ECHO has fixed width one and a fixed-origin caret.
 * Protected modes suppress outbound copy/cut but permit inbound paste when writable.
 * Models, getters, commit receipts and bounded history can contain plaintext; no storage zeroization is promised.
 */
class LineEdit : public SizedWidget {
public:
  /** @brief Notify an optional weak validation gate after widget borrows drain; cancellation never joins. */
  ~LineEdit() override;
  /** @brief Preserve owning movement before attachment; attached widgets remain stationary.
   * @param other Owning editor data transferred into this unattached object.
   */
  LineEdit(LineEdit &&other) = default;
  /** @param other Owning editor data transferred before attachment.
   * @return This editor after the owning data transfer.
   */
  LineEdit &operator=(LineEdit &&other) = default;
  /** @brief Construct an empty editor; binding adopts its initial model snapshot.
   * @param options Owning immutable limits, width policy and styles.
   * @param policy Owning layout size policy validated by layout.
   * @throws std::bad_alloc If initial text storage cannot be allocated.
   * @throws std::bad_expected_access<TextError> If the width policy or placeholder text violates its bounds.
   * @throws std::invalid_argument If the display mode is unknown or the placeholder contains a line separator.
   * Declarative recipes prepare placeholder validation/storage in their checked mount boundary instead.
   */
  explicit LineEdit(LineEditOptions options = {}, SizePolicy policy = {});
  /** @brief Internal prepared-presentation construction used by checked recipes.
   * @param options Immutable editor settings, with display mode already checked.
   * @param policy Owning layout size policy.
   * @param placeholder Owning prevalidated placeholder; no placeholder parsing/allocation occurs here.
   * @throws std::bad_alloc If initial draft storage cannot be allocated.
   */
  LineEdit(LineEditOptions options, SizePolicy policy, detail::LineEditPlaceholder placeholder);
  /** @brief Measure displayed content plus one caret cell; NO_ECHO always measures one cell.
   * @param available Offered extent; does not constrain intrinsic measurement.
   * @return Intrinsic cell width and height one, or INVALID_GEOMETRY on overflow.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @brief Store local bounds and scroll by whole graphemes to expose the caret.
   * @param local Parent-relative allocated rectangle; only its extent is retained.
   * @return Success or INVALID_GEOMETRY for unrepresentable bounds.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @brief Submit bounded local commands; software caret and selection never split a grapheme.
   * @param context Synchronous sink; retained text borrows expire at enclosing tree.frame exit.
   * @return Success or the sink's error; sink exceptions propagate to WidgetTree::paint.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @brief Borrow validated draft only inside tree.read.
   * @return Reference invalidated by the next edit, synchronization or destruction.
   */
  const GraphemeText &text() const;
  /** @brief Inspect the active selection endpoint.
   * @return UTF-8 byte boundary in text(), including its end.
   */
  std::size_t cursor() const;
  /** @brief Inspect the fixed selection endpoint.
   * @return UTF-8 byte boundary; equals cursor() for an empty selection.
   */
  std::size_t anchor() const;
  /** @brief Inspect whether visible text awaits successful publication or cancellation.
   * @return True for a deferred, semantically rejected or conflicting candidate.
   */
  bool dirty() const;

  /** @return Owning manual control, selection and caret appearances beneath theme declarations. */
  ComputedStyle styleDefaults() const override;

private:
  friend class LineEditBinding;
  friend struct detail::LineEditValidationAccess;
  friend class ManagedScene;
  friend struct ui::detail::RefAccess;
  void revealCursor();
  std::int32_t cellAt(std::size_t byte) const;
  LineEditOptions options_;
  std::weak_ptr<detail::LineEditValidationGate> validationGate_;
  GraphemeText text_;
  std::unique_ptr<GraphemeText> placeholder_;
  std::unique_ptr<GraphemeText> masked_;
  CellRect bounds_;
  std::size_t cursor_{};
  std::size_t anchor_{};
  std::int32_t scroll_{};
  bool dirty_{};
  bool focused_{};
  bool enabled_{true};
};

/** @brief Scoped model/action ownership; destroy and move on UI outside frames.
 * Tree, router and scheduler outlive the binding. Call sync before each frame. Drafts survive
 * external revisions; confirmation detects conflict and Escape adopts latest. EditMode selects
 * immediate publication, confirmation on focus loss, or explicit confirmation only.
 * Admitted callbacks retain state; removal suppresses future callbacks and observer delivery.
 */
class LineEditBinding {
public:
  /** @brief Disconnect subscription/action; admitted calls retain their owners. */
  ~LineEditBinding();
  /** @brief Transfer ownership on UI.
   * @param other Binding emptied by this transfer.
   */
  LineEditBinding(LineEditBinding &&other) noexcept;
  /** @brief Install replacement before retiring old captures, which may reenter UI.
   * @param other Binding emptied by transfer; self-move preserves all state.
   * @return This binding; reentrant cleanup may have destroyed its containing owner.
   */
  LineEditBinding &operator=(LineEditBinding &&other) noexcept;
  LineEditBinding(const LineEditBinding &) = delete;
  LineEditBinding &operator=(const LineEditBinding &) = delete;
  /** @brief Synchronize latest clean model and focus presentation between frames.
   * @param focused Snapshot from EventRouter::focused; empty means no focus.
   * @return Success, INVALID_ARGUMENT for an empty binding, WOULD_BLOCK on frame/reentry,
   * NO_MEMORY, INVALID_ARGUMENT for invalid model text, or delegated tree/model error.
   * Rejected/old notifications never override a dirty draft. Invalid models preserve presentation.
   */
  std::expected<void, TreeError> sync(Node<Widget> focused);

  /** @return Success after restoring Undo and applying EditMode, or typed admission/publication failure.
   * Empty history closes grouping without callbacks or invalidation. Semantic rejection remains a visible draft.
   */

  std::expected<void, TreeError> undo();

  /** @return Success after restoring Redo and applying EditMode, or typed admission/publication failure. */

  std::expected<void, TreeError> redo();

  /** @brief Clear a writable eligible editor as one separate user edit, applying its EditMode.
   * @return Success for an empty draft or semantic rejection; technical failures retain their exact TreeError.
   * Pre-accept failures preserve draft/history; post-accept publication failure keeps a dirty undoable draft.
   * Read-only and disabled editors return WOULD_BLOCK, including when already empty.
   */
  std::expected<void, TreeError> clear();

  /** @return Success after closing grouping without redraw/notification, or typed liveness/reentry failure. */

  std::expected<void, TreeError> stopGrouping();

  /** @return Effective availability, or typed liveness/frame/reentry error; never adopts or reports. */

  std::expected<HistoryAvailability, TreeError> historyAvailability() const;

  /** @return Effective Undo availability; false on checked failure, with no error reporting. */
  bool canUndo() const;

  /** @return Effective Redo availability; false on checked failure, with no error reporting. */
  bool canRedo() const;

private:
  friend struct detail::LineEditValidationAccess;
  friend struct ui::detail::RefAccess;
  std::expected<std::uint64_t, TreeError> draftRevision() const;

  friend class ManagedScene;
  std::expected<void, TreeError> setDraft(GraphemeText text, std::uint64_t revision,

                                          std::optional<ProgrammaticChange> change = {});

  void commandFeedback(std::shared_ptr<detail::EditorCommandFeedback> feedback);
  std::pair<std::expected<void, TreeError>, bool>
  historyCommand(bool redo, std::source_location origin = std::source_location::current());
  std::pair<std::expected<void, TreeError>, bool> clearCommand(std::source_location origin);
  std::pair<std::expected<void, TreeError>, bool> editorCommand(bool redo, bool clear, std::source_location origin);
  struct State;
  friend std::expected<LineEditBinding, TreeError>
  bindLineEditWithEvents(WidgetTree &, EventRouter &, Node<LineEdit>, std::shared_ptr<Property<std::string>>,
                         std::function<void(LineEditResult)>, std::shared_ptr<Clipboard>,
                         std::function<bool(std::string_view)>, LineEditEvents);
  LineEditBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription);
  std::shared_ptr<State> state_;
  ActionBinding action_;
  Subscription subscription_;
};

/** @brief Bind a one-line editor and adopt its initial model on UI outside frames.
 * @param tree Borrowed UI owner, outliving binding and model.
 * @param router Borrowed router for this tree, outliving binding.
 * @param node Observing editor handle; one action registration is allowed per node.
 * @param model Required shared model owner. External values must satisfy editor text limits.
 * @param result Required owning callback, invoked before typed events for editing/confirmation operations; may remove
 * node/ancestors or destroy the binding. Successful commits stay committed if this callback throws.
 * @param clipboard Optional shared UI service for Ctrl+C/X/V; absence reports CLIPBOARD_FAILED.
 * @param validate Optional publication predicate borrowing an owned candidate only during its call.
 * False preserves draft with VALIDATION_FAILED; exceptions report FAILED/CALLBACK_FAILED.
 * @return Owning binding or delegated tree/router/property error; invalid initial text is INVALID_ARGUMENT.
 * Internal allocations report NO_MEMORY; by-value argument construction may throw before entry.
 * Publication uses options.editMode and requires the original revision, including after validator reentry.
 * IMMEDIATE is the default; semantic rejection retains the accepted candidate without a model set.
 * Invoke Action::ACTIVATE for the same programmatic confirmation as Enter, in every mode.
 * History owns bounded text/selection snapshots; accepted edits and restorations precede validator callbacks.
 * Default Ctrl+Z/Ctrl+Y/Ctrl+Shift+Z chords are configurable; reserved router keys and Undo/Redo overlap reject bind.
 * Read-only accepts explicit programmatic text changes but blocks user typing/deletion/cut/paste and Undo/Redo.
 * User clear is also blocked by read-only/disabled eligibility; programmatic text mutation remains unchanged.
 * PASSWORD/NO_ECHO consume Ctrl+C/X without service access or outcomes. Ctrl+V and paste remain writable inputs.
 * Escape adopts latest. Shift+arrows/Home/End select; Ctrl+A selects all. Cut changes draft only after
 * a successful service write, then applies the same publication policy as input and fluent edits.
 * Paste rejects invalid UTF-8, CR/LF/U+2028/U+2029 and whole over-limit edits.
 * Other controls remain logical data displayed as safe renderer placeholders.
 */
std::expected<LineEditBinding, TreeError> bindLineEdit(WidgetTree &tree, EventRouter &router, Node<LineEdit> node,
                                                       std::shared_ptr<Property<std::string>> model,
                                                       std::function<void(LineEditResult)> result,
                                                       std::shared_ptr<Clipboard> clipboard = {},
                                                       std::function<bool(std::string_view)> validate = {});

/** @copydoc bindLineEdit
 * @param events Typed observations delivered after the legacy result; no borrowed widget text survives callbacks.
 */
std::expected<LineEditBinding, TreeError>
bindLineEditWithEvents(WidgetTree &tree, EventRouter &router, Node<LineEdit> node,
                       std::shared_ptr<Property<std::string>> model, std::function<void(LineEditResult)> result,
                       std::shared_ptr<Clipboard> clipboard, std::function<bool(std::string_view)> validate,
                       LineEditEvents events);
} // namespace portals
