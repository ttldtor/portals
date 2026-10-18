// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <portals/text.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace portals {

/** @brief Default maximum combined undo and redo groups. */
inline constexpr std::size_t DEFAULT_TEXT_HISTORY_OPERATIONS = 128;
/** @brief Default logical record and UTF-8 capacity budget, in bytes. */
inline constexpr std::size_t DEFAULT_TEXT_HISTORY_MAX_BYTES = 1048576;

/** @brief Configurable bounds and explicit-boundary grouping for text history. */
struct TextHistoryOptions {
  /** @brief Record edits when true; disabled history accepts edits without retaining them. */
  bool enabled{true};
  /** @brief Maximum combined undo and redo groups; zero disables retention. */
  std::size_t maxOperations{DEFAULT_TEXT_HISTORY_OPERATIONS};
  /** @brief Maximum retained entry objects and UTF-8 string capacities, in bytes; zero disables retention. */
  std::size_t maxBytes{DEFAULT_TEXT_HISTORY_MAX_BYTES};
  /** @brief Combine adjacent typing edits until an explicit boundary. */
  bool groupTyping{true};
  /** @brief Combine adjacent deletions of the same direction until a boundary. */
  bool groupDeletion{true};
};

/** @brief Semantic editing categories; only the first three can form groups. */
enum class TextHistoryKind {
  /** @brief Plain insertion without replacing a selection. */
  TYPING,
  /** @brief Deletion immediately before the cursor. */
  BACKSPACE,
  /** @brief Deletion immediately after the cursor. */
  FORWARD_DELETE,
  /** @brief A separate clipboard insertion. */
  PASTE,
  /** @brief A separate clipboard removal. */
  CUT,
  /** @brief A separate selection replacement. */
  REPLACEMENT,
  /** @brief A separate recorded programmatic edit. */
  PROGRAMMATIC
};

/** @brief Owning UTF-8 bytes and byte offsets validated during preparation. */
struct TextHistorySnapshot {
  /** @brief Original UTF-8 text; no normalization is performed. */
  // Count construction propagates MSVC Debug iterator-proxy allocation failures.
  std::string bytes = std::string(0, '\0');
  /** @brief Active end of selection, at an extended grapheme boundary. */
  std::size_t cursor{};
  /** @brief Fixed end of selection, at an extended grapheme boundary. */
  std::size_t anchor{};
};

/** @brief Technical history failure; no state is changed on failure. */
enum class TextHistoryError {
  /** @brief Allocation or representational storage failure. */
  NO_MEMORY,
  /** @brief One complete operation exceeds the configured finite byte budget. */
  WOULD_BLOCK,
  /** @brief Text violates UTF-8, width policy or text limits. */
  INVALID_TEXT,
  /** @brief A selection offset is not an extended grapheme boundary. */
  NOT_BOUNDARY,
  /** @brief No operation is available in the requested direction. */
  EMPTY,
  /** @brief Prepared state belongs to another owner or is no longer current. */
  STALE,
  /** @brief The monotonically increasing preparation generation cannot advance. */
  EXHAUSTED
};

class TextHistory;

/**
 * @brief Owning, move-only staged history and already segmented restoration text.
 *
 * Destruction abandons staging. Construction performs all potentially throwing
 * value moves behind a pointer; moving this handle cannot allocate, including
 * MSVC Debug iterator bookkeeping. No callbacks or external object pointers.
 */
class PreparedTextHistory {
public:
  /** @brief Transfer a prepared value without allocating.
   * @param other Prepared value; becomes empty on success.
   */
  PreparedTextHistory(PreparedTextHistory &&other) noexcept;
  /** @brief Replace this handle without allocating.
   * @param other Prepared value; self-assignment is supported.
   * @return This handle after ownership transfer.
   */
  PreparedTextHistory &operator=(PreparedTextHistory &&other) noexcept;
  /** @brief Release abandoned staging without changing the owner. */
  ~PreparedTextHistory();

  /** @brief Report whether preparation changes recorded history.
   * @return False for a regular equal-text edit; true for reset and traversal.
   */
  [[nodiscard]] bool changed() const noexcept;
  /** @brief Return the validated target cursor.
   * @return Byte offset of the active selection end, or zero for an empty handle.
   */
  [[nodiscard]] std::size_t cursor() const noexcept;
  /** @brief Return the validated target selection anchor.
   * @return Byte offset of the fixed selection end, or zero for an empty handle.
   */
  [[nodiscard]] std::size_t anchor() const noexcept;
  /** @brief Borrow the already segmented target before publication.
   * @return Target text, or nullptr for an empty handle or after takeText; expires on transfer/destruction.
   */
  [[nodiscard]] const GraphemeText *text() const noexcept;
  /** @brief Transfer staged text into existing storage without allocating.
   * @param destination Existing text value to replace by noexcept move assignment.
   * @note Call TextHistory::checkPrepared first, then transfer and commit without
   * intervening callbacks or history mutations. A second transfer is a no-op.
   */
  void takeText(GraphemeText &destination) noexcept;

private:
  friend class TextHistory;
  struct State;
  explicit PreparedTextHistory(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

/**
 * @brief Specialized owning text history with allocation-free publication.
 *
 * Values contain no editor, model, widget, callback or external buffer references.
 * The caller synchronizes operations; editor owners normally use their UI thread.
 * All retained undo and redo groups share one count and logical memory budget.
 * Memory counts sizeof(entry) and both string capacities. Container storage,
 * shared ownership control blocks, allocator bookkeeping, the current editor
 * text and temporary staging allocations are excluded; this is not an RSS cap.
 * Staging copies only shared entry handles, never all retained strings.
 * Constructor allocations may throw; prepare methods return NO_MEMORY instead.
 */
class TextHistory {
public:
  /** @brief Create an empty history under immutable text validation settings.
   * @param options Retention and grouping configuration, captured by value.
   * @param policy Width policy applied to every prepared snapshot.
   * @param limits Text validation limits applied to every prepared snapshot.
   */
  explicit TextHistory(TextHistoryOptions options = {}, WidthPolicy policy = {}, TextLimits limits = {});
  /** @brief Release all owning history storage. */
  ~TextHistory();
  TextHistory(const TextHistory &) = delete;
  TextHistory &operator=(const TextHistory &) = delete;

  /** @brief Prepare a whole edit without changing this history.
   * @param before Current text and selection; selection changes prevent merging.
   * @param after Accepted candidate text and selection.
   * @param kind Editing category; paste, cut, replacement and programmatic edits are separate.
   * @return Owning staging or a typed failure. Equal bytes preserve redo.
   */
  [[nodiscard]] std::expected<PreparedTextHistory, TextHistoryError>
  prepareEdit(const TextHistorySnapshot &before, const TextHistorySnapshot &after, TextHistoryKind kind) const;
  /** @brief Stage one undo with restoration segmentation under captured settings.
   * @return Owning target text and staged cursor, or EMPTY/technical failure.
   */
  [[nodiscard]] std::expected<PreparedTextHistory, TextHistoryError> prepareUndo() const;
  /** @brief Stage one redo with restoration segmentation under captured settings.
   * @return Owning target text and staged cursor, or EMPTY/technical failure.
   */
  [[nodiscard]] std::expected<PreparedTextHistory, TextHistoryError> prepareRedo() const;
  /** @brief Validate a new external baseline and stage clearing every group, even for equal bytes.
   * @param baseline New current text and selection; retained editor storage remains external.
   * @return Owning target text and reset staging, or a typed failure.
   */
  [[nodiscard]] std::expected<PreparedTextHistory, TextHistoryError>
  prepareResetBaseline(const TextHistorySnapshot &baseline) const;
  /** @brief Check that a prepared value can be committed before publishing its text.
   * @param prepared Owning staged operation from this unchanged history.
   * @return Success or STALE; does not change either value or allocate.
   */
  [[nodiscard]] std::expected<void, TextHistoryError> checkPrepared(const PreparedTextHistory &prepared) const noexcept;
  /** @brief Publish prepared history without allocation or callbacks.
   * @param prepared Previously checked staging, consumed on success.
   * @return Success or STALE; failures preserve history and staging.
   */
  [[nodiscard]] std::expected<void, TextHistoryError> commit(PreparedTextHistory &&prepared) noexcept;
  /** @brief End the open editing group; invalidates outstanding staging without allocation. */
  void stopGrouping() noexcept;
  /** @brief Query local undo availability; editor eligibility belongs to the external owner.
   * @return True when at least one recorded group precedes the history cursor.
   */
  [[nodiscard]] bool canUndo() const noexcept;
  /** @brief Query local redo availability.
   * @return True when at least one recorded group follows the history cursor.
   */
  [[nodiscard]] bool canRedo() const noexcept;
  /** @brief Count all retained undo and redo groups.
   * @return Combined operation count, bounded by maxOperations.
   */
  [[nodiscard]] std::size_t operationCount() const noexcept;
  /** @brief Query retained logical memory, excluding documented overhead.
   * @return Sum of entry objects and UTF-8 capacities, in bytes.
   */
  [[nodiscard]] std::size_t retainedBytes() const noexcept;

private:
  struct State;
  [[nodiscard]] std::expected<PreparedTextHistory, TextHistoryError> prepareTraversal(bool undo) const;
  std::unique_ptr<State> state_;
  TextHistoryOptions options_;
  WidthPolicy policy_;
  TextLimits limits_;
};

} // namespace portals
