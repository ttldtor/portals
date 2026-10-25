// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>
#include <portals/model.hpp>

#include <memory>
#include <string_view>

namespace portals {
class LineEdit;

namespace ui::detail {
struct Access;
}

namespace detail {
struct FindResult;
struct FindRecord;
struct FindAccess;
struct FindModelSource;
} // namespace detail

/** @brief Direction through the stored bounded prefix; both directions wrap. */
enum class FindDirection {
  NEXT,    ///< Advance, wrapping the last stored match to the first.
  PREVIOUS ///< Retreat, wrapping the first stored match to the last.
};

/** @brief Independent source/result caps; query admission uses the ordinary editor TextLimits. */
struct FindLimits {
  static constexpr std::size_t MAX_DOCUMENT_BYTES = 1024 * 1024; ///< Hard synchronous source ceiling.
  static constexpr std::size_t MAX_QUERY_BYTES = 16 * 1024;      ///< Hard synchronous query ceiling.
  static constexpr std::size_t MAX_MATCHES = 65536;              ///< Hard stored-range ceiling.
  std::size_t maxDocumentBytes{65536}; ///< Owning UTF-8 source cap, including zero for empty sources only.
  std::size_t maxMatches{4096};        ///< Positive number of retained matches; additional results set truncated.
};

/** @brief Independent worker scan and retained-result budgets for model-backed literal search. */
struct FindSearchOptions {
  std::size_t maxSourceRows{100000};               ///< Full model row ceiling, including nonmatching rows.
  std::size_t maxProjectedBytes{32 * 1024 * 1024}; ///< Sum of projected UTF-8 bytes examined per query.
  std::size_t maxRowBytes{1024 * 1024};            ///< Maximum projected bytes in one row.
  std::size_t maxQueryBytes{16 * 1024};            ///< Search ceiling in addition to ordinary editor TextLimits.
  std::size_t maxMatches{4096};                    ///< Positive retained prefix; one extra match marks truncation.
};

/** @brief Model search failures, distinct from editor admission and scheduler/tree failures. */
enum class FindSearchError {
  INVALID_ARGUMENT, ///< Invalid options, source shape, or row identity.
  LIMIT_EXCEEDED,   ///< Source rows, projected bytes, row bytes or query bytes exceeded their bound.
  INVALID_TEXT,     ///< Projected row is not admitted UTF-8 under the scene text policy.
  NOT_READY,        ///< Immutable provider range is pending or failed; no I/O was started.
  CANCELLED,        ///< Cooperative cancellation was observed.
  NO_MEMORY,        ///< Worker could not allocate bounded preparation storage.
  CALLBACK_FAILED   ///< Provider read or worker projection threw.
};

/** @brief Owning immutable selected range; retains original data, never a scene or widget.
 * Empty handles represent empty-query/no-match states. Old handles remain readable after replacement;
 * Ref.current() determines whether their result identity still belongs to the current search.
 */
class FindMatch {
public:
  /** @brief Construct an empty range without allocation. */
  FindMatch() = default;
  /** @return True for a selected stored range; no allocation or UI access. */
  explicit operator bool() const noexcept;
  /** @return Inclusive UTF-8 byte offset, or zero for an empty handle. */
  std::size_t begin() const noexcept;
  /** @return Exclusive whole-grapheme UTF-8 offset, or zero for an empty handle. */
  std::size_t end() const noexcept;
  /** @return One-based stored position, or zero for an empty handle. */
  std::size_t ordinal() const noexcept;
  /** @return Full model row identity, or zero identity for an empty/legacy document match. */
  RowId rowId() const noexcept;
  /** @return Exact provider revision, or zero for a legacy document match. */
  std::uint64_t sourceRevision() const noexcept;
  /** @return Accepted model query generation, or zero for a legacy document match. */
  std::uint64_t queryGeneration() const noexcept;
  /** @return Number of stored ranges; never an exact total when truncated() is true. */
  std::size_t storedCount() const noexcept;
  /** @return Whether at least one additional whole-grapheme match was found beyond the cap. */
  bool truncated() const noexcept;
  /** @return Original selected bytes, borrowed until this handle is assigned or destroyed. */
  std::string_view text() const noexcept;
  /** @param maxBytes Maximum whole-grapheme context bytes; defaults to256, clamped to source length.
   * @return Borrowed context including the match, or empty when no match or the match exceeds the budget.
   * Uses cached source boundaries and performs no allocation; the view expires with this handle.
   */
  std::string_view context(std::size_t maxBytes = 256) const noexcept;

private:
  friend struct detail::FindAccess;
  std::shared_ptr<const detail::FindResult> result_;
  std::size_t index_{};
};

/** @brief Manual child appearances and ordinary retained layout defaults. */
struct FindBarAppearance {
  ControlStyle navigationStyle{}; ///< Previous/Next/Clear manual defaults beneath child style rules.
  ControlStyle summaryStyle{};    ///< Count/context manual defaults beneath child style rules.
  LinearOptions layout{};         ///< Existing outer vertical layout settings.
  SizePolicy policy{};            ///< Existing outer layout constraints.
};

/** @brief Retained query/navigation/count composition over owning document or model search results.
 * The UI tree owns all children. The document recipe is synchronous and bounded; the model recipe uses
 * the existing application's background executor. Direct construction creates no asynchronous service.
 * Read low-level observations only inside tree.read; use the existing Ref commands between frames.
 */
class FindBar : public Column {
public:
  /** @param appearance Existing layout/manual style defaults, copied into the composition. */
  explicit FindBar(FindBarAppearance appearance = {});
  /** @brief Retire callback bridges before child ownership is reclaimed. */
  ~FindBar() override;
  /** @return Observing standard query editor root, possibly stale after independent child removal. */
  Node<Widget> editor() const;
  /** @return Owning selected snapshot; no source, query or range allocation. */
  FindMatch match() const;

private:
  friend struct ui::detail::Access;
  friend struct detail::FindAccess;
  std::shared_ptr<detail::FindRecord> record_;
};
} // namespace portals
