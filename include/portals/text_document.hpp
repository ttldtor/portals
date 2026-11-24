// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model_position.hpp>
#include <portals/text_history.hpp>

#include <string_view>

namespace portals {
/** @brief Scalar text replacement proof; does not read or retain document bytes.
 * Units are chosen by the enclosing provider; the finite adapter uses UTF-8 bytes.
 * A scalar mapping alone cannot prove that a mapped offset is a grapheme boundary.
 */
struct TextSplice {
  std::uint64_t offset{};   ///< First replaced scalar offset in the provider's text units.
  std::uint64_t erased{};   ///< Number of removed units.
  std::uint64_t inserted{}; ///< Number of inserted units.
};

/** @brief Finite metadata retention, independent of logical document size. */
struct TextAnchorOptions {
  std::size_t maxChanges{128}; ///< Positive maximum retained splice edges and revision receipts.
};

/** @brief Intermediate scalar mapping; not a boundary-validated PositionReply. */
struct TextMappedPosition {
  PositionResolution state{PositionResolution::UNAVAILABLE}; ///< Mapping proof outcome.
  ModelPosition position; ///< Scalar hint requiring the provider's final grapheme-boundary validation.
};

/** @brief Bounded metadata-only helper for a text provider; not a PositionResolver.
 * Caller serializes every operation. Creation allocates; publication and resolution
 * allocate nothing, perform no I/O or callbacks, and inspect only bounded metadata.
 * Missing proof returns UNAVAILABLE. The enclosing provider must validate/snap
 * mapped scalar hints to actual grapheme boundaries before publishing TEXT replies.
 * This avoids a whole-document grapheme index requirement. Bookmarks contain scalars only. Content-state
 * tokens are positive, provider-local values, not logical resource identities.
 */
class TextChangeMap final {
public:
  /** @param resource Existing logical resource namespace; no new issuer is introduced.
   * @param extent Initial extent in provider-defined text units; may exceed resident memory.
   * @param options Positive metadata capacity.
   * @param initialRevision Positive initial data revision, normally one.
   * @param initialContentState Positive provider-local baseline token, normally one.
   * @return Map at the supplied baseline, or a typed admission error.
   */
  static std::expected<std::unique_ptr<TextChangeMap>, SourceError> create(ModelResource resource, std::uint64_t extent,
                                                                           TextAnchorOptions options = {},
                                                                           std::uint64_t initialRevision = 1,
                                                                           std::uint64_t initialContentState = 1);
  ~TextChangeMap(); ///< Release metadata; no provider, view or text is retained.
  /** @param change Provider-proven scalar replacement.
   * @param contentState New strictly increasing provider-local content token.
   * @return Success or validation/exhaustion failure; failures preserve the map.
   */
  std::expected<void, SourceError> edit(TextSplice change, std::uint64_t contentState) noexcept;
  /** @param contentState Prior token proven by the provider's existing Undo/Redo engine.
   * @param extent Exact scalar extent in that state, even when its old proof was evicted.
   * @return Success or validation/exhaustion failure; no second undo engine is stored.
   */
  std::expected<void, SourceError> traverse(std::uint64_t contentState, std::uint64_t extent) noexcept;
  /** @param offset Current scalar offset, including end; caller must prove the grapheme boundary.
   * @param affinity FORWARD follows insertion; BACKWARD stays before insertion.
   * @return Scalar-only TEXT bookmark or INVALID_ARGUMENT.
   */
  std::expected<ModelBookmark, SourceError> bookmark(std::uint64_t offset,
                                                     PositionAffinity affinity = PositionAffinity::FORWARD) const;
  /** @param bookmark TEXT bookmark captured at a provider-validated grapheme boundary.
   * @param stop Cooperative cancellation; checked without retaining the token.
   * @return Intermediate mapped scalar hint or typed cancellation/validation failure.
   * EXACT means scalar mapping is exact; the provider may still need a NEAREST grapheme snap.
   */
  std::expected<TextMappedPosition, SourceError> map(const ModelBookmark &bookmark, std::stop_token stop = {}) const;
  /** @return Current monotonically increasing data revision. */
  std::uint64_t revision() const noexcept;
  /** @return Current logical scalar extent; no source scan. */
  std::uint64_t extent() const noexcept;

private:
  struct State;
  explicit TextChangeMap(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

/** @brief Finite document error; rejected operations preserve published state. */
enum class TextDocumentError {
  NO_MEMORY,        ///< Allocation/representation failure.
  INVALID_TEXT,     ///< Invalid UTF-8, width policy or configured text limit.
  NOT_BOUNDARY,     ///< Byte offset is not an extended grapheme boundary.
  INVALID_ARGUMENT, ///< Invalid target, range or options.
  WOULD_BLOCK,      ///< One history operation exceeds its finite retention budget.
  EMPTY,            ///< No Undo/Redo is available.
  STALE,            ///< Receipt belongs to another document or superseded save request.
  EXHAUSTED,        ///< A monotonic counter cannot advance.
  CONFLICT          ///< External content was reported; explicit replacement acceptance is required.
};

/** @brief Byte-boundary selection owned by the document session, independent of a view. */
struct TextSelection {
  std::size_t cursor{}; ///< Active byte-boundary end.
  std::size_t anchor{}; ///< Fixed byte-boundary end.
  /** @param other Selection to compare. @return Equality of both byte offsets. */
  bool operator==(const TextSelection &other) const noexcept = default;
};

/** @brief Explicit finite-buffer adapter configuration; not a windowed editor. */
struct FiniteTextDocumentOptions {
  TextHistoryOptions history; ///< Existing bounded Undo/Redo; content tokens are enabled by this adapter.
  TextAnchorOptions anchors;  ///< Metadata proof capacity independent of text/history size.
  WidthPolicy widthPolicy;    ///< Existing Unicode/display validation policy.
  TextLimits limits;          ///< Existing finite whole-buffer admission limits.
};

/** @brief Owning save acknowledgement qualification; retains target metadata, never document bytes.
 * A ticket alone does not perform I/O or grant write authority. The application
 * saves the bytes observed at its revision and acknowledges only successful writes.
 */
class TextSaveTicket {
public:
  TextSaveTicket() = default; ///< Empty invalid ticket.
  /** @return Physical destination borrowed while this ticket or a copy survives. */
  std::string_view target() const noexcept;
  /** @return Exact document revision to save, or zero for an empty ticket. */
  std::uint64_t revision() const noexcept;

private:
  friend class FiniteTextDocument;
  struct State;
  std::shared_ptr<const State> state_;
};

/** @brief Explicit owning finite UTF-8 document/session; no UI ownership or file I/O.
 * Whole-buffer segmentation and existing snapshot history have costs proportional
 * to finite admitted text. Large/windowed editors use the metadata-only map with
 * their own boundary proofs and history. Caller serializes operations; borrowed
 * text expires on successful text mutation or destruction. There are no callbacks.
 * Logical identity, selection and history survive view replacement and SaveAs.
 */
class FiniteTextDocument final : public PositionResolver {
public:
  /** @param text Initial borrowed UTF-8, copied/segmented before success.
   * @param options Explicit finite text/history and metadata bounds.
   * @return Owning session or checked admission failure.
   */
  static std::expected<std::unique_ptr<FiniteTextDocument>, TextDocumentError>
  create(std::string_view text, FiniteTextDocumentOptions options = {});
  ~FiniteTextDocument() override; ///< Release only this session's own text, history and metadata.
  /** @return Borrowed current segmented text, invalidated by text mutation/destruction. */
  const GraphemeText &text() const noexcept;
  /** @return Logical identity, unchanged by saving or physical target replacement. */
  ModelResource resource() const noexcept;
  /** @return Delivery namespace used for PositionRequest qualification. */
  SourceStamp session() const noexcept;
  /** @return Current monotonic text revision; selection/save operations do not advance it. */
  std::uint64_t revision() const noexcept;
  /** @return Current byte-boundary selection. */
  TextSelection selection() const noexcept;
  /** @param selection Current byte boundaries, including end.
   * @return Success or NOT_BOUNDARY; breaks typing/deletion grouping.
   */
  std::expected<void, TextDocumentError> setSelection(TextSelection selection) noexcept;
  /** @param begin Inclusive byte boundary.
   * @param end Exclusive byte boundary, at or after begin.
   * @param replacement Borrowed replacement UTF-8; copied before publication.
   * @param kind Existing history category controlling optional grouping.
   * @return Success or checked failure; collapses selection after inserted graphemes.
   */
  std::expected<void, TextDocumentError> replace(std::size_t begin, std::size_t end, std::string_view replacement,
                                                 TextHistoryKind kind = TextHistoryKind::PROGRAMMATIC);
  /** @return Success or EMPTY/technical failure; restores text and session selection. */
  std::expected<void, TextDocumentError> undo();
  /** @return Success or EMPTY/technical failure; restores text and session selection. */
  std::expected<void, TextDocumentError> redo();
  /** @return Whether existing bounded history has a retained Undo operation. */
  bool canUndo() const noexcept;
  /** @return Whether existing bounded history has a retained Redo operation. */
  bool canRedo() const noexcept;
  /** @param byteOffset Current byte boundary, converted by this finite adapter only.
   * @param affinity Insertion preference, retained in the scalar bookmark.
   * @return General ModelBookmark(TEXT), with UTF-8 byte offset in its captured revision.
   */
  std::expected<ModelBookmark, SourceError> bookmark(std::size_t byteOffset,
                                                     PositionAffinity affinity = PositionAffinity::FORWARD) const;
  /** @param request Exactly qualified general position request.
   * @param stop Cooperative cancellation.
   * @return General position receipt with a validated UTF-8 byte-boundary offset.
   * A disappeared boundary snaps BACKWARD before, or FORWARD after, its current cluster.
   */
  std::expected<PositionReply, SourceError> resolve(const PositionRequest &request,
                                                    std::stop_token stop = {}) const override;
  /** @param target Nonempty physical target metadata; no I/O or target change yet.
   * @return Ticket for current content; allocation failure preserves the document.
   */
  std::expected<TextSaveTicket, TextDocumentError> beginSave(std::string_view target);
  /** @param ticket Successful physical write acknowledgement from this document.
   * @return Success or STALE/CONFLICT; older acknowledgements cannot replace newer savepoints.
   * SaveAs updates only physical target/savepoint, never selection, identity or history.
   */
  std::expected<void, TextDocumentError> acknowledgeSave(const TextSaveTicket &ticket) noexcept;
  /** @return Current content differs from acknowledged content state, or external conflict is present. */
  bool dirty() const noexcept;
  /** @return Last acknowledged physical destination; borrowed until newer acknowledgement/destruction. */
  std::string_view saveTarget() const noexcept;
  /** @brief Report external replacement without accepting unrelated bytes or clearing history. */
  void observeExternalReplacement() noexcept;
  /** @return Whether an explicit external replacement awaits caller resolution. */
  bool conflicted() const noexcept;
  /** @param text Explicitly accepted new baseline, copied/validated before publication.
   * @return Success or checked failure; advances lineage and clears incompatible marks/history.
   * This starts a new compatible-history generation, keeping logical resource identity.
   */
  std::expected<void, TextDocumentError> acceptExternalReplacement(std::string_view text);

private:
  struct State;
  explicit FiniteTextDocument(std::unique_ptr<State> state) noexcept;
  std::expected<void, TextDocumentError> traverse(bool undo);
  std::unique_ptr<State> state_;
};
} // namespace portals
