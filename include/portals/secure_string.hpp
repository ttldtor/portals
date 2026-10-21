// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace portals {

/** @brief Default bound for application bytes, without Unicode interpretation. */
inline constexpr std::size_t DEFAULT_MAX_SECRET_BYTES = 4096;
/** @brief Hard upper bound for an individual secret, in bytes. */
inline constexpr std::size_t MAX_SECRET_BYTES = 1048576;

/** @brief Explicit storage policy; unsupported native protection never falls back. */
enum class SecureStringStorage {
  /** @brief Dedicated native RW payload surrounded by inaccessible pages. */
  NATIVE_GUARDED,
  /** @brief Explicit weaker heap/linear-memory policy with wiping alone. */
  WIPE_ONLY
};

/** @brief Requested mandatory guarantees, established even for an empty value. */
struct SecureStringOptions {
  /** @brief Maximum application bytes, from zero through MAX_SECRET_BYTES. */
  std::size_t maxBytes{DEFAULT_MAX_SECRET_BYTES};
  /** @brief Mandatory allocation policy; no implicit platform fallback. */
  SecureStringStorage storage{SecureStringStorage::NATIVE_GUARDED};
  /** @brief Require native resident page locking before input is copied. */
  bool lockPages{};
  /** @brief Require Linux core-dump exclusion; unsupported on Windows/WASM. */
  bool excludeFromDumps{};
  /** @brief Require Windows SAME_PROCESS idle encryption; unsupported on Linux/WASM. */
  bool encryptWhileIdle{};
};

/** @brief Applied guarantees, rather than potential platform features. */
struct SecureStringCapabilities {
  /** @brief Entire accessible capacity is explicitly wiped before release. */
  bool wiping{};
  /** @brief Two inaccessible exterior pages surround the RW payload. */
  bool guardPages{};
  /** @brief Native page locking was requested and applied. */
  bool lockedPages{};
  /** @brief Linux core-dump exclusion was requested and applied. */
  bool excludedFromDumps{};
  /** @brief Windows encryption is applied between plaintext borrows. */
  bool encryptedWhileIdle{};
};

/** @brief Operation recorded without secret bytes or exception messages. */
enum class SecureStringOperation {
  /** @brief Validate options and create an owning value. */
  CREATE,
  /** @brief Replace the logical bytes of a live value. */
  ASSIGN,
  /** @brief Append application bytes to a live value. */
  APPEND,
  /** @brief Replace a live value with an empty value under the same policy. */
  CLEAR,
  /** @brief Explicitly close storage through the checked lifecycle. */
  CLOSE,
  /** @brief Admit plaintext access, capture a callable or invoke its callback. */
  BORROW,
  /** @brief Allocate payload storage and establish its exterior guards. */
  ALLOCATE,
  /** @brief Apply the requested native resident-page lock. */
  LOCK,
  /** @brief Apply the requested native core-dump exclusion. */
  DUMP,
  /** @brief Encrypt payload bytes for idle storage. */
  ENCRYPT,
  /** @brief Decrypt payload bytes before scoped access or copying. */
  DECRYPT,
  /** @brief Release an already-wiped allocation to its backend. */
  RELEASE
};

/** @brief Technical failure categories independent of UI/tree errors. */
enum class SecureStringErrorCode {
  /** @brief Options contain an invalid bound or storage-policy value. */
  INVALID_ARGUMENT,
  /** @brief Logical size or backend rounding exceeds a checked bound. */
  TOO_LARGE,
  /** @brief Metadata, callable capture or payload allocation failed. */
  NO_MEMORY,
  /** @brief A mandatory requested guarantee is unavailable for this policy or platform. */
  UNSUPPORTED,
  /** @brief Native guard, lock, dump-exclusion or crypto setup/access failed. */
  SECURITY_FAILURE,
  /** @brief The same storage already has an active plaintext borrow. */
  BUSY,
  /** @brief The handle is moved-from, closed or invalidated by a security failure. */
  UNAVAILABLE,
  /** @brief Callable capture/invocation threw or a callback returned its typed error. */
  CALLBACK_FAILED,
  /** @brief Backend release failed; zeroed allocation metadata may remain for retry. */
  RELEASE_FAILED
};

/** @brief Owning, secret-free library diagnostic; nativeCode is zero when inapplicable. */
struct SecureStringError {
  /** @brief Library operation that failed. */
  SecureStringOperation operation{};
  /** @brief Stable technical failure category. */
  SecureStringErrorCode code{};
  /** @brief Captured GetLastError/errno, or zero without a native error. */
  std::uint32_t nativeCode{};
};

/** @brief Maximum separately retained cleanup diagnostics; excess failures are counted. */
inline constexpr std::size_t MAX_SECURE_CLEANUP_ERRORS = 4;

/** @brief Allocation-free cleanup diagnostics in chronological order. */
struct SecureStringCleanupErrors {
  /** @brief First count errors, in actual cleanup order. */
  std::array<SecureStringError, MAX_SECURE_CLEANUP_ERRORS> errors{};
  /** @brief Number of valid entries in errors. */
  std::size_t count{};
  /** @brief Additional failures omitted because the fixed buffer is full. */
  std::size_t omittedCleanupCount{};

  /** @brief Construct an empty diagnostic collection. */
  SecureStringCleanupErrors() = default;

  /** @brief Construct from a possibly absent first failure.
   * @param failure First cleanup error, or no error.
   */
  SecureStringCleanupErrors(std::optional<SecureStringError> failure) noexcept {
    add(failure);
  }

  /** @brief Append an existing diagnostic without allocation; absent errors are ignored.
   * @param failure Error to retain or count after capacity is exhausted.
   */
  void add(std::optional<SecureStringError> failure) noexcept {
    if (!failure) {
      return;
    }

    if (count < errors.size()) {
      errors[count++] = *failure;
    } else {
      ++omittedCleanupCount;
    }
  }

  /** @brief Append retained failures and overflow counts from another collection.
   * @param other Valid source collection whose chronological records are copied.
   */
  void append(const SecureStringCleanupErrors &other) noexcept {
    for (std::size_t index = 0; index < other.count; ++index) {
      add(other.errors[index]);
    }

    omittedCleanupCount += other.omittedCleanupCount;
  }

  /** @return True when neither retained nor omitted cleanup failures exist. */
  [[nodiscard]] bool empty() const noexcept {
    return count == 0 && omittedCleanupCount == 0;
  }
};

/** @brief A primary failure and independently preserved bounded cleanup diagnostics. */
struct SecureStringFailure {
  /** @brief Original operation failure, never displaced by cleanup. */
  SecureStringError primary;
  /** @brief Subsequent cleanup/security failures and overflow count. */
  SecureStringCleanupErrors cleanup;
};

/** @brief Synchronous read-only byte borrow, valid only during its callback.
 *
 * There is no implicit string_view conversion. Copying the view does not extend
 * its lifetime. Escaped pointers, caller copies and external API copies remain
 * the caller's responsibility. The payload pages themselves remain writable.
 */
class SecretView {
public:
  /** @return Explicit read-only address, valid until the callback returns. */
  [[nodiscard]] const char *data() const noexcept {
    return bytes_.data();
  }

  /** @return Logical application byte count, excluding capacity/padding. */
  [[nodiscard]] std::size_t size() const noexcept {
    return bytes_.size();
  }

  /** @return Whether the logical byte count is zero. */
  [[nodiscard]] bool empty() const noexcept {
    return bytes_.empty();
  }

private:
  explicit SecretView(std::span<const char> bytes) noexcept : bytes_(bytes) {
  }

  std::span<const char> bytes_;
  friend class SecureString;
};

/** @brief Callback and cleanup diagnostics; opaque user errors are never logged.
 * @tparam E Owning, nothrow-movable error. Custom wrappers must own their dependencies.
 */
template <class E = std::monostate> struct SecureStringAccessFailure {
  /** @brief Capture/decryption/callback failure; absent when only cleanup failed. */
  std::optional<SecureStringError> primary;
  /** @brief Opaque owning callback error, never formatted by the library. */
  std::optional<E> callbackError;
  /** @brief Reprotection/retirement failures independent of the primary error. */
  SecureStringCleanupErrors cleanup;
};

/// @cond INTERNAL
namespace detail {
struct SecureStringState;
class SecureStringTestAccess;

template <class T> inline constexpr bool IS_REFERENCE_WRAPPER = false;
template <class T> inline constexpr bool IS_REFERENCE_WRAPPER<std::reference_wrapper<T>> = true;

template <class E>
concept OwningSecretError =
  !std::is_reference_v<E> && !std::is_pointer_v<E> && !IS_REFERENCE_WRAPPER<std::remove_cv_t<E>> &&
  !std::same_as<std::remove_cv_t<E>, SecretView> && !std::ranges::view<E> && !std::ranges::borrowed_range<E> &&
  std::is_nothrow_move_constructible_v<E> && std::is_nothrow_destructible_v<E>;

template <class R> struct SecretCallbackResult {
  static constexpr bool VALID = false;
  using Error = std::monostate;
};

template <> struct SecretCallbackResult<void> {
  static constexpr bool VALID = true;
  using Error = std::monostate;
};

template <class E> struct SecretCallbackResult<std::expected<void, E>> {
  static constexpr bool VALID = OwningSecretError<E>;
  using Error = E;
};

template <class F>
concept SecretCallback =
  std::constructible_from<std::decay_t<F>, F> && std::is_nothrow_destructible_v<std::decay_t<F>> &&
  std::invocable<std::decay_t<F> &, SecretView> &&
  SecretCallbackResult<std::invoke_result_t<std::decay_t<F> &, SecretView>>::VALID;
} // namespace detail

/// @endcond

/** @brief Move-only bounded secret storage with checked synchronous plaintext access.
 *
 * Native storage has adjacent inaccessible guard pages and an always-RW payload.
 * Windows can additionally lock pages and encrypt idle bytes with SAME_PROCESS;
 * Linux can lock pages and exclude them from core dumps. WIPE_ONLY uses heap
 * storage and rejects lock/dump/encryption requests. Unsupported mandatory
 * guarantees fail without a downgrade, including on WASM. Full accessible
 * capacity is wiped before release; a failed release retains zeroed metadata for
 * close() retry. A destructor retries release twice and may leak zeroed pages if
 * the OS refuses both attempts.
 *
 * Candidate allocation/capability failure preserves the old value. Ambiguous
 * crypto failure wipes the affected payload and makes that value unavailable.
 * Replacement failure after old-storage retirement never resurrects old bytes.
 *
 * This does not protect against a debugger, hostile code in the process, host,
 * keylogger, live plaintext access or external copies. Length is not concealed.
 * Operations, moves and destruction require caller synchronization. A callback
 * may move/destroy the external owner; its lease stays alive. Checked mutation
 * and nested borrowing of that same storage fail BUSY, including no-op inputs.
 */
class SecureString {
public:
  /** @brief Copying secret ownership/bytes is not supported. */
  SecureString(const SecureString &) = delete;
  /** @brief Copy assignment is not supported. */
  SecureString &operator=(const SecureString &) = delete;
  /** @brief Transfer the ownership handle and its existing busy state, without copying bytes.
   * @param other Source handle, left moved-from without invalidating active borrows.
   */
  SecureString(SecureString &&other) noexcept;
  /** @brief Detach the old destination and transfer the source handle; self-move is a no-op.
   * An active lease keeps detached destination storage alive until checked finish.
   * @param other Source handle, left moved-from unless this is self-assignment.
   * @return This same destination handle.
   */
  SecureString &operator=(SecureString &&other) noexcept;
  /** @brief Detach ownership; wipe/release immediately or after the final active lease. */
  ~SecureString() noexcept;

  /** @brief Prepare the requested policy before copying input; no input copies are retained on failure.
   * @param bytes Application bytes; embedded zeroes are preserved.
   * @param options Mandatory storage policy and finite bounds; maxBytes may be zero.
   * @return Owning value, or secret-free diagnostics. Input ownership is unchanged.
   */
  [[nodiscard]] static std::expected<SecureString, SecureStringFailure> create(std::string_view bytes = {},
                                                                               SecureStringOptions options = {});

  /** @brief Replace bytes via an exact-length candidate; preparation failure preserves the old value.
   * @param bytes Borrowed input copied synchronously; embedded zeroes are preserved.
   * @return Success or primary and cleanup failures; BUSY rejects mutation during a borrow.
   */
  [[nodiscard]] std::expected<void, SecureStringFailure> assign(std::string_view bytes);
  /** @brief Append bytes via an exact-length candidate; no exponential growth or string intermediate.
   * @param bytes Borrowed suffix copied synchronously; embedded zeroes are preserved.
   * @return Success or primary and cleanup failures; BUSY rejects even an empty suffix.
   */
  [[nodiscard]] std::expected<void, SecureStringFailure> append(std::string_view bytes);
  /** @brief Replace with a valid empty value under the same requested policy.
   * @return Success or primary and cleanup failures; unavailable and busy handles reject clearing.
   */
  [[nodiscard]] std::expected<void, SecureStringFailure> clear();
  /** @brief Wipe and release; idempotent after success, retryable after release failure, BUSY during borrow.
   * @return Success or primary and cleanup failures, without secret bytes.
   */
  [[nodiscard]] std::expected<void, SecureStringFailure> close() noexcept;
  /** @return Logical byte count; zero for closed, moved-from or unavailable values. */
  [[nodiscard]] std::size_t size() const noexcept;
  /** @return Whether size() is zero; use available() to distinguish a valid empty value. */
  [[nodiscard]] bool empty() const noexcept;
  /** @return Whether this handle has a live usable value, including a valid empty value. */
  [[nodiscard]] bool available() const noexcept;
  /** @return Current applied capabilities; all false after closing or invalidation. */
  [[nodiscard]] SecureStringCapabilities capabilities() const noexcept;

  /** @brief Invoke an owned synchronous callable, then restore protection or retire detached storage.
   * @tparam F Copyable lvalue or movable rvalue callable returning void or expected<void,E>.
   * @param callback Callable copied/moved after the lease and busy gate, before decryption.
   * @return Callback error/exception and cleanup failure are preserved independently.
   *
   * E must be owning and nothrow movable; direct pointers, reference wrappers,
   * SecretView and standard borrowed ranges/views are rejected. Custom E and
   * callable captures retain their explicit caller lifetime obligations. Capture
   * bad_alloc maps to NO_MEMORY; other exceptions map to CALLBACK_FAILED. Neither
   * exception text nor opaque E is formatted. No owner access occurs after capture.
   */
  template <class F>
    requires detail::SecretCallback<F>
  [[nodiscard]] auto withPlaintext(F &&callback)
    -> std::expected<void, SecureStringAccessFailure<typename detail::SecretCallbackResult<
                             std::invoke_result_t<std::decay_t<F> &, SecretView>>::Error>> {
    using Return = std::invoke_result_t<std::decay_t<F> &, SecretView>;
    using Failure = SecureStringAccessFailure<typename detail::SecretCallbackResult<Return>::Error>;

    auto lease = state_;
    auto entered = beginBorrow(lease);

    if (!entered) {
      return std::unexpected(Failure{entered.error(), {}, {}});
    }

    Failure failure;

    // The frame owns the callable even if invoking it deletes its originating owner.
    // Destroy it under the busy gate so destructor-driven detachment is included
    // in the checked finish path as well.
    std::optional<std::decay_t<F>> frame;

    try {
      frame.emplace(std::forward<F>(callback));
    } catch (const std::bad_alloc &) {
      failure.primary = SecureStringError{SecureStringOperation::BORROW, SecureStringErrorCode::NO_MEMORY};
    } catch (...) {
      failure.primary = SecureStringError{SecureStringOperation::BORROW, SecureStringErrorCode::CALLBACK_FAILED};
    }

    if (frame) {
      auto view = openBorrow(lease);

      if (!view) {
        failure.primary = view.error().primary;

        failure.cleanup.append(view.error().cleanup);
      } else {
        try {
          if constexpr (std::is_void_v<Return>) {
            std::invoke(*frame, *view);
          } else {
            auto result = std::invoke(*frame, *view);

            if (!result) {
              failure.primary =
                SecureStringError{SecureStringOperation::BORROW, SecureStringErrorCode::CALLBACK_FAILED};
              failure.callbackError.emplace(std::move(result.error()));
            }
          }
        } catch (...) {
          failure.primary = SecureStringError{SecureStringOperation::BORROW, SecureStringErrorCode::CALLBACK_FAILED};
        }
      }
    }

    frame.reset();

    auto cleanup = finishBorrow(lease);

    if (cleanup) {
      failure.cleanup.add(cleanup->primary);
      failure.cleanup.append(cleanup->cleanup);
    }

    if (failure.primary || !failure.cleanup.empty()) {
      return std::unexpected(std::move(failure));
    }

    return {};
  }

private:
  explicit SecureString(std::shared_ptr<detail::SecureStringState> state) noexcept;
  void detach() noexcept;
  [[nodiscard]] std::expected<void, SecureStringFailure> replace(std::string_view bytes, bool append,
                                                                 SecureStringOperation operation);
  static std::expected<void, SecureStringError>
  beginBorrow(const std::shared_ptr<detail::SecureStringState> &state) noexcept;
  static std::expected<SecretView, SecureStringFailure>
  openBorrow(const std::shared_ptr<detail::SecureStringState> &state) noexcept;
  static std::optional<SecureStringFailure>
  finishBorrow(const std::shared_ptr<detail::SecureStringState> &state) noexcept;

  std::shared_ptr<detail::SecureStringState> state_;
  friend class detail::SecureStringTestAccess;
};

} // namespace portals
