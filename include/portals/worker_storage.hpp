// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/background_options.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

namespace portals {
namespace detail {
struct WorkerStorageControl;
struct WorkerStorageWake;
}

/** @brief Opaque worker-owned payload. Destruction must be worker-safe and nonthrowing. */
class WorkerStorageValue {
public:
  virtual ~WorkerStorageValue() = default; ///< Released on a worker or final post-UI host teardown.
};

/** @brief Generation-fenced slot identity; carries no payload ownership. */
struct WorkerStorageId {
  std::uint64_t value{};      ///< One-based slot number.
  std::uint64_t generation{}; ///< Nonzero registration generation, never reused.
  /** @return True when both slot number and generation are equal. */
  bool operator==(const WorkerStorageId &) const noexcept = default;
};

/** @brief Light retirement capability; locking it can never acquire payload/State ownership. */
class WorkerStorageLease {
public:
  WorkerStorageLease() = default; ///< Empty lease.
  ~WorkerStorageLease();          ///< Idempotent retirement; no allocation, traversal, join or payload destruction.
  WorkerStorageLease(WorkerStorageLease &&) noexcept; ///< Transfer retirement ownership.
  /** @return Reference to this lease after retiring its previous registration and transferring ownership.
   */
  WorkerStorageLease &operator=(WorkerStorageLease &&) noexcept;
  WorkerStorageLease(const WorkerStorageLease &) = delete;
  WorkerStorageLease &operator=(const WorkerStorageLease &) = delete;
  /** @return Plain identity usable only with the live originating executor. */
  WorkerStorageId id() const noexcept { return id_; }
  /** @brief Mark retired and notify separate light wake storage; safe after host destruction. */
  void retire() noexcept;

private:
  friend class BackgroundWorkerStorage;
  WorkerStorageLease(WorkerStorageId id, std::weak_ptr<detail::WorkerStorageControl> control)
      : id_(id), control_(std::move(control)) {}
  WorkerStorageId id_;
  std::weak_ptr<detail::WorkerStorageControl> control_;
};

/** @brief Nonmovable build scope, valid only inside withBuild's worker callback. */
class WorkerStorageBuild {
public:
  WorkerStorageBuild(const WorkerStorageBuild &) = delete;
  WorkerStorageBuild &operator=(const WorkerStorageBuild &) = delete;
  WorkerStorageBuild(WorkerStorageBuild &&) = delete;
  WorkerStorageBuild &operator=(WorkerStorageBuild &&) = delete;
  /** @param value Worker-owned fully prepared value, preserved on rejection.
   * @param bytes Actual resident allocation capacity, including payload metadata.
   * @return Success after ownership transfer or LIMIT_EXCEEDED/INVALID_ARGUMENT.
   * All scratch allocations must be destroyed before the enclosing callback returns.
   */
  std::expected<void, BackgroundError> commit(std::unique_ptr<WorkerStorageValue> &value, std::size_t bytes);
  /** @return Reserved resident byte ceiling; allocations must respect transient peak too. */
  std::size_t residentCap() const noexcept { return residentCap_; }
  /** @return Reserved scratch byte ceiling, retained until callback locals have been destroyed. */
  std::size_t scratchCap() const noexcept { return scratchCap_; }

private:
  friend class BackgroundWorkerStorage;
  WorkerStorageBuild(std::size_t resident, std::size_t scratch) : residentCap_(resident), scratchCap_(scratch) {}
  std::size_t residentCap_{}, scratchCap_{}, bytes_{};
  std::unique_ptr<WorkerStorageValue> value_;
};

/** @brief Allocation-capacity observations, distinct from process RSS. */
struct WorkerStorageStats {
  std::size_t chargedBytes{}; ///< Resident, detached reclaiming, and full active build reservations.
  std::size_t peakBytes{};    ///< High-water charged bytes; never exceeds aggregate limit.
  std::size_t slots{};        ///< Registered plus retiring slots.
  std::uint64_t evictions{};  ///< Idle cache values evicted under pressure.
};

/** @brief Optional executor capability for bounded opaque worker-owned caches.
 * Direct operations borrow the live executor. Leases retain only separate light controls.
 * Payload callbacks run synchronously only inside this executor's BackgroundJob::run extent.
 * They must not retain a borrowed value/build scope, UI state, or coroutine storage.
 */
class BackgroundWorkerStorage {
public:
  ~BackgroundWorkerStorage(); ///< Final host teardown after all workers join, never scene retirement.
  BackgroundWorkerStorage(const BackgroundWorkerStorage &) = delete;
  BackgroundWorkerStorage &operator=(const BackgroundWorkerStorage &) = delete;
  /** @return Light slot registration or explicit CLOSED/WOULD_BLOCK/NO_MEMORY. */
  std::expected<WorkerStorageLease, BackgroundError> registerSlot();
  /** @return Advisory readiness generation; sample before admission to avoid missed release. */
  std::uint64_t changeEpoch() const noexcept;
  /** @return Current allocation-capacity accounting, safe while host lives. */
  WorkerStorageStats stats() const noexcept;
  /** @param id Registration identity from this executor.
   * @param read Worker callback borrowing an immutable opaque payload for this invocation only.
   * @return Original callback result or WRONG_THREAD/CLOSED/WOULD_BLOCK; exceptions translated.
   */
  std::expected<void, BackgroundError>
  withRead(WorkerStorageId id, const std::function<std::expected<void, BackgroundError>(const WorkerStorageValue &)> &read);
  /** @param id Exact registration; build excludes readers and another build on this slot.
   * @param residentCap Reserved resident bytes, including transient reallocation peak.
   * @param scratchCap Reserved temporary bytes, released only after callback returns.
   * @param build Worker callback allocating within both reservations and committing once.
   * @return Original callback result, LIMIT_EXCEEDED for an impossible own request, or bounded backpressure.
   * Idle LRU values can be reclaimed on this worker before admission. No worker waits for other work.
   */
  std::expected<void, BackgroundError>
  withBuild(WorkerStorageId id, std::size_t residentCap, std::size_t scratchCap,
            const std::function<std::expected<void, BackgroundError>(WorkerStorageBuild &)> &build);

private:
  friend class BackgroundPool;
  struct State;
  BackgroundWorkerStorage(std::size_t slots, std::size_t bytes, std::shared_ptr<detail::WorkerStorageWake> wake);
  void execute(void *context, void (*run)(void *) noexcept) noexcept;
  void drain() noexcept;
  bool retirementPending() const noexcept;
  void close() noexcept;
  std::unique_ptr<State> state_;
};
} // namespace portals
