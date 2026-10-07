// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <portals/geometry.hpp>
#include <portals/input.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace portals {

/** @brief Absolute monotonic time used by terminal waits. */
using TerminalDeadline = std::chrono::steady_clock::time_point;

/** @brief Borrowed native descriptors supplied to open(); no Windows headers required. */
struct NativeTerminalHandles {
  std::intptr_t input{};  ///< POSIX input file descriptor.
  std::intptr_t output{}; ///< POSIX output file descriptor.
};

/** @brief Session resource bounds and cancellation/resize sampling cadence. */
struct TerminalOptions {
  std::size_t max_input_bytes{4096};          ///< Positive maximum raw bytes returned by one read.
  std::size_t max_input_events{256};          ///< Positive maximum native semantic events per read.
  std::size_t max_output_bytes{1024 * 1024};  ///< Positive maximum size of one owned output batch.
  std::chrono::milliseconds wait_cadence{10}; ///< Positive maximum individual poll interval.
};

/** @brief Terminal failure classification; native_code provides platform detail. */
enum class TerminalErrorKind {
  invalid_options, ///< An option or handle is outside its supported range.
  unsupported,     ///< Handles are not terminals, or no backend is implemented.
  system,          ///< A native system operation failed.
  closed,          ///< Session was closed or moved from.
  output_pending,  ///< The previous output batch is not complete.
  output_limit     ///< The new batch exceeds max_output_bytes.
};

/** @brief Owning error value, independent of session lifetime. */
struct TerminalError {
  TerminalErrorKind kind{}; ///< Stable failure category.
  int native_code{};        ///< errno on POSIX; zero for errors without a native code.
  int restoration_code{};   ///< First rollback errno on failed open; zero if rollback succeeded.
};

/** @brief Why an input or output operation returned. */
enum class TerminalStatus {
  progress, ///< Input/resize available, or the entire pending output batch completed.
  deadline, ///< Absolute deadline reached; pending output is preserved.
  stopped,  ///< Stop requested; pending output is preserved.
  eof,      ///< Input ended after previously readable bytes were delivered.
  error     ///< Inspect the accompanying error; no hidden output retry occurs.
};

/** @brief Bounded owning transport result; earlier results are never invalidated. */
struct TerminalRead {
  TerminalStatus status{TerminalStatus::progress}; ///< Completion reason.
  std::string bytes;                               ///< POSIX raw bytes; decode with InputDecoder.
  std::vector<InputEvent> events;                  ///< Native semantic events; empty on POSIX.
  std::optional<CellSize> resized;                 ///< Newly observed dimensions, if changed.
  std::optional<TerminalError> error;              ///< Present exactly when status is error.
};

/** @brief Owning output progress for this flush call, not a rendered-frame commit. */
struct TerminalWrite {
  TerminalStatus status{TerminalStatus::progress}; ///< Completion reason.
  std::size_t completed{};                         ///< Bytes successfully written in this call only.
  std::size_t pending{};                           ///< Unwritten bytes still owned by the session.
  std::optional<TerminalError> error;              ///< Present exactly when status is error.
};

/**
 * @brief Move-only terminal mode and I/O owner; no I/O belongs in paint/measure.
 * @details One executor owns all calls including destruction; concurrent calls are
 * unsupported. Transfer between executors requires external synchronization. POSIX
 * open duplicates both descriptors and owns those duplicates; caller handles stay
 * open. All aliases of their terminals/open-file descriptions must be exclusively
 * used by this owner until close, because termios and file status flags are shared.
 * Input/output values own storage. Allocation exceptions propagate with RAII cleanup;
 * no caller buffer/view is retained. This backend installs no signal handlers.
 * No guarantee is made for abrupt process death or externally closed/modified handles.
 */
class TerminalSession {
public:
  /** @brief Save both original states before mutation and enter raw nonblocking mode.
   * Both POSIX descriptors must be TTYs. On failure, independently restore everything
   * already changed; restoration_code reports rollback failure. Allocation can throw.
   */
  [[nodiscard]] static std::expected<TerminalSession, TerminalError> open(NativeTerminalHandles handles,
                                                                          TerminalOptions options = {});
  TerminalSession(const TerminalSession &) = delete;            ///< Exclusive state cannot be copied.
  TerminalSession &operator=(const TerminalSession &) = delete; ///< Exclusive state cannot be copied.
  TerminalSession(TerminalSession &&) noexcept;                 ///< Transfer ownership; source becomes closed.
  TerminalSession &operator=(TerminalSession &&) noexcept;      ///< Best-effort close old state, then transfer.
  ~TerminalSession() noexcept; ///< Best-effort restoration and descriptor cleanup; never flushes.

  /** @brief Read one bounded owning batch, changed size, deadline, stop, EOF or error.
   * Stop takes precedence over an expired deadline. Resize is sampled on the owner,
   * at most wait_cadence apart while waiting; changes can coalesce. No callbacks.
   */
  [[nodiscard]] TerminalRead read(TerminalDeadline deadline, std::stop_token stop = {});
  /** @brief Query current columns/rows; returns closed/system error without mutation. */
  [[nodiscard]] std::expected<CellSize, TerminalError> size() const noexcept;
  /** @brief Transfer one bounded owned byte batch; reject while output is pending.
   * No I/O. Empty batch is allowed. Rejection leaves session state unchanged.
   */
  [[nodiscard]] std::expected<void, TerminalError> submit(std::string bytes);
  /** @brief Write until batch completion, deadline, stop or error; preserve exact suffix.
   * Only successful native writes count as completed. Deadline/stop are checked
   * between nonblocking syscalls. Errors retain the suffix; caller chooses recovery.
   * Successful writes do not prove terminal interpretation or rendered-frame state.
   */
  [[nodiscard]] TerminalWrite flush(TerminalDeadline deadline, std::stop_token stop = {});
  /** @brief Number of pending owned bytes; zero for a closed/moved-from session. */
  [[nodiscard]] std::size_t pending_bytes() const noexcept;
  /** @brief Discard pending bytes and restore all changed modes/flags independently.
   * Always closes owned duplicate descriptors, even on restoration error. Returns
   * first native restoration/close error. Idempotent; destructor cannot report errors.
   * Incomplete/failed output leaves renderer belief uncertain, owned by the caller.
   */
  [[nodiscard]] std::expected<void, TerminalError> close() noexcept;

private:
  struct Impl;
  explicit TerminalSession(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace portals
