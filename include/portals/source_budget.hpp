// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <mutex>

namespace portals {
/** @brief Source protocol and admission failures, independent of a provider's storage format. */
enum class SourceError {
  INVALID_ARGUMENT,   ///< Invalid options, enum, identity or command shape.
  WRONG_THREAD,       ///< UI ownership or worker-only load affinity was violated.
  CLOSED,             ///< Source, consumer or node delivery scope has retired.
  WOULD_BLOCK,        ///< Root, work, frame or consumer capacity rejected admission.
  NO_MEMORY,          ///< Allocation failed inside a fallible operation.
  IDENTITY_EXHAUSTED, ///< An identity or readiness revision cannot advance without reuse.
  CALLBACK_FAILED,    ///< Provider construction/preparation threw an unexpected exception.
  CONTEXT_ERROR,      ///< The tree rejected a stale/foreign context or invalidation.
  UNSUPPORTED,        ///< A required optional capability is unavailable.
  STALE_SOURCE,       ///< The supplied immutable source namespace does not match.
  INVALID_CURSOR,     ///< The provider cannot validate this opaque token.
  REVISION_CHANGED,   ///< Requested provider revision is unavailable or a notification is stale.
  POSITION_EXHAUSTED, ///< Provider position representation cannot advance without wrapping.
  ROW_NOT_FOUND,      ///< A supported stable-identity lookup did not find its row.
  ROW_TOO_LARGE,      ///< One materialized row exceeds its inline/transitive byte limit.
  PAGE_TOO_LARGE,     ///< The fixed page/workspace cannot hold the requested allocation.
  INVALID_REPLY,      ///< Result shape, identities, revision or boundaries violate the protocol.
  NO_PROGRESS,        ///< A MORE reply repeated its input gap.
  CURSOR_CYCLE,       ///< A MORE reply repeated an earlier gap of this bounded fill.
  CONTINUATION_LIMIT, ///< Demand remains after four automatic page loads; partial pin is valid.
  CANCELLED,          ///< Explicit navigation cancellation or cooperative worker cancellation.
  ALREADY_COMPLETED,  ///< This request generation already ran its single load.
  IO_ERROR            ///< Native source I/O failed; a typed adapter retains its per-operation cause.
};

/** @brief Simultaneous source reservations; bytes are requested capacities, not process RSS. */
struct SourceBudgetUsage {
  std::size_t bytes{};     ///< Reserved bytes, including accounting allowances.
  std::size_t pages{};     ///< Building, queued, cached and retained page allocations.
  std::size_t work{};      ///< Work owners, including cancelled but still live work.
  std::size_t consumers{}; ///< Live consumer storage reservations.
};

/** @brief Configurable root shared by typed pages, future text pages and replacement sessions. */
struct SourceBudgetOptions {
  std::size_t maxBytes{8 * 1024 * 1024}; ///< Aggregate reserved capacity.
  std::size_t maxPages{32};              ///< Aggregate page allocations.
  std::size_t maxWork{4};                ///< Aggregate work reservations.
  std::size_t maxConsumers{8};           ///< Aggregate consumer reservations.
};

class SourceBudget;
template <class T> class Publisher;

namespace detail {
struct SourceCapacityState;
}

/** @brief Move-only credit; backing storage must be destroyed before this final credit owner. */
class SourceCredit {
public:
  SourceCredit() = default; ///< Empty credit.
  ~SourceCredit();          ///< Return the reservation to its root.
  /** @param other Credit to transfer without allocation and leave empty. */
  SourceCredit(SourceCredit &&other) noexcept;
  /** @param other Credit replacing this reservation.
   * @return This owner; old credit is released after replacement.
   */
  SourceCredit &operator=(SourceCredit other) noexcept;
  SourceCredit(const SourceCredit &) = delete;
  /** @param amount Sub-reservation to transfer; each field must fit this credit.
   * @return Independent credit on the same root, or INVALID_ARGUMENT without mutation.
   */
  std::expected<SourceCredit, SourceError> split(SourceBudgetUsage amount) noexcept;
  /** @return This owner's reservation, not aggregate usage. */
  SourceBudgetUsage usage() const noexcept;

private:
  friend class SourceBudget;
  SourceCredit(std::shared_ptr<SourceBudget> root, SourceBudgetUsage amount) noexcept;
  std::shared_ptr<SourceBudget> root_;
  SourceBudgetUsage amount_;
};

/** @brief Move-only worker-safe capacity observation; destruction unregisters without waiting. */
class SourceCapacityObserver {
public:
  SourceCapacityObserver() = default; ///< Empty observer; admission returns CLOSED.
  ~SourceCapacityObserver();          ///< Unregister without allocation or callbacks into UI.
  /** @param other Registration to transfer and leave empty. */
  SourceCapacityObserver(SourceCapacityObserver &&other) noexcept;
  /** @param other Registration replacing this one. @return This observer after unregistering the old one. */
  SourceCapacityObserver &operator=(SourceCapacityObserver &&other) noexcept;
  SourceCapacityObserver(const SourceCapacityObserver &) = delete;
  /** @param amount Actual reservation. @param peak Complete simultaneous demand, including amount.
   * @return Charged credit after an atomic peak check, WOULD_BLOCK, or PAGE_TOO_LARGE for impossible demand.
   */
  std::expected<SourceCredit, SourceError> reserve(SourceBudgetUsage amount, SourceBudgetUsage peak);
  /** @param peak Complete next-turn demand. @return Armed one-shot notification or a typed permanent limit.
   * Capacity is rechecked atomically; a release before arming cannot be lost.
   */
  std::expected<void, SourceError> arm(SourceBudgetUsage peak);
  /** @return Whether an armed notification granted admission priority, consuming only its ready flag. */
  bool takeReady() noexcept;
  /** @brief Drop pending admission priority; no UI callback or worker join occurs. */
  void disarm() noexcept;

private:
  friend class SourceBudget;
  explicit SourceCapacityObserver(std::shared_ptr<detail::SourceCapacityState> state) noexcept;
  std::shared_ptr<detail::SourceCapacityState> state_;
};

/** @brief Thread-safe credit ledger; final page/worker release may happen on any thread. */
class SourceBudget : public std::enable_shared_from_this<SourceBudget> {
public:
  static constexpr std::size_t ROOT_METADATA_BYTES =
    4096; ///< Bootstrap ledger allowance, retained for the root lifetime.
  static constexpr std::size_t CAPACITY_OBSERVER_BYTES = 4096; ///< Precharged allowance for one capacity observer.
  /** @param options Positive capacity limits; includes a conservative bootstrap allowance.
   * @return New root or INVALID_ARGUMENT/NO_MEMORY.
   */
  static std::expected<std::shared_ptr<SourceBudget>, SourceError> create(SourceBudgetOptions options = {});
  /** @param amount Capacities to reserve atomically before allocation or I/O.
   * @return Owning credit, or WOULD_BLOCK without changing any counter.
   */
  std::expected<SourceCredit, SourceError> reserve(SourceBudgetUsage amount);
  /** @param wake Worker-safe wake-only endpoint; no payload or UI callback is invoked by release.
   * @return Precharged4KiB RAII registration, bounded by maxConsumers, or a typed admission failure.
   */
  std::expected<SourceCapacityObserver, SourceError> observeCapacity(Publisher<int> wake);
  /** @return Advisory release generation; only capacity notifications, never byte identities, use this value. */
  std::uint64_t capacityEpoch() const noexcept;
  /** @return One synchronized aggregate snapshot. */
  SourceBudgetUsage usage() const;
  /** @return Immutable root limits. */
  SourceBudgetOptions options() const noexcept;

private:
  friend class SourceCredit;
  friend class SourceCapacityObserver;
  friend struct detail::SourceCapacityState;
  std::shared_ptr<detail::SourceCapacityState> eligibleObserverLocked() noexcept;
  void removeObserver(detail::SourceCapacityState &state) noexcept;
  void disarmObserver(detail::SourceCapacityState &state) noexcept;
  std::expected<void, SourceError> armObserver(detail::SourceCapacityState &state, SourceBudgetUsage peak);
  std::expected<SourceCredit, SourceError> reserveObserved(detail::SourceCapacityState &state, SourceBudgetUsage amount,
                                                           SourceBudgetUsage peak);
  explicit SourceBudget(SourceBudgetOptions options);
  void release(SourceBudgetUsage amount) noexcept;
  SourceBudgetOptions options_;
  mutable std::mutex mutex_;
  SourceBudgetUsage used_;
  detail::SourceCapacityState *observers_{};
  detail::SourceCapacityState *granted_{};
  std::size_t observerCount_{};
  std::uint64_t capacityEpoch_{};
};

/** @brief Fixed-capacity monotonic storage shared by typed/text pages; no upstream fallback.
 * Allocate all transitively owned page payload through this resource. Deallocation is deferred
 * until the last arena owner releases it. Allocation and construction happen on one worker.
 */
class SourceArena final : public std::pmr::memory_resource {
public:
  /** @param credit Reservation containing bytes for this arena and its metadata.
   * @return Owning storage, or NO_MEMORY/PAGE_TOO_LARGE; rejected credit is released.
   */
  static std::expected<std::shared_ptr<SourceArena>, SourceError> create(SourceCredit credit);
  ~SourceArena() override; ///< Release backing before returning its credit.
  /** @return Number of occupied bytes in the fixed payload block. */
  std::size_t used() const noexcept;
  /** @return Allocatable payload capacity, excluding metadata allowance. */
  std::size_t capacity() const noexcept;
  /** @brief Conservative per-allocation envelope for arena/control-block metadata. */
  static constexpr std::size_t METADATA_BYTES = 4096;

private:
  explicit SourceArena(SourceCredit credit);
  void *do_allocate(std::size_t bytes, std::size_t alignment) override;
  void do_deallocate(void *, std::size_t, std::size_t) override;
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override;
  SourceCredit credit_;
  std::unique_ptr<std::byte[]> storage_;
  std::size_t capacity_{};
  std::size_t used_{};
};
} // namespace portals
