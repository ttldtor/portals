// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <portals/secure_string.hpp>
#include <portals/widget_types.hpp>

namespace portals {

/** @brief Independent ownership and protected-access failures, without secret diagnostics.
 * @tparam E Owning, nothrow-movable callback error accepted by SecureString.
 */
template <class E = std::monostate> struct PasswordValueAccessFailure {
  /** @brief Ownership/admission rejection; the callback was neither captured nor invoked. */
  std::optional<TreeError> ownership;
  /** @brief Protected-storage, callback and cleanup diagnostics, retained independently. */
  std::optional<SecureStringAccessFailure<E>> access;
};

namespace detail {
struct PasswordValueAccess;
#if defined(PORTALS_SECURE_STRING_TESTING)
struct SecureStringTestHooks;
#endif
} // namespace detail

/** @brief Copyable identity of immutable logical bytes in protected native storage.
 *
 * Copies share one protected value; equal bytes created separately have different
 * identities. Retained snapshots can outlive a widget or model. Replacing the last
 * external handle during a callback does not end its active lease. Copy, move,
 * comparison and destruction do not allocate or invoke application/UI callbacks.
 *
 * Creation requires native guarded storage and idle encryption, without fallback.
 * Bytes are opaque, including zeroes; Unicode validation belongs to the editor.
 * Only the creating thread may borrow plaintext. Separate handles and immutable
 * metadata may be retained on other threads; simultaneous access to the same
 * handle still requires caller synchronization. A cryptographic failure can make
 * every shared identity unavailable; immutable logical content does not promise
 * immutable availability. Input bytes, callback copies and escaped views remain
 * the caller's responsibility; length is public metadata.
 */
class PasswordValue {
public:
  /** @brief Construct an unavailable identity without allocating protected storage. */
  PasswordValue() noexcept = default;
  /** @brief Retain the same identity without copying plaintext.
   * @param other Identity whose protected storage is retained.
   */
  PasswordValue(const PasswordValue &other) noexcept = default;
  /** @brief Replace this handle with the same identity as other.
   * @param other Identity whose protected storage is retained.
   * @return This handle.
   */
  PasswordValue &operator=(const PasswordValue &other) noexcept = default;
  /** @brief Transfer identity, leaving other unavailable.
   * @param other Source handle; an active borrow remains valid.
   */
  PasswordValue(PasswordValue &&other) noexcept = default;
  /** @brief Transfer identity, leaving other unavailable except on self-move.
   * @param other Source handle; an active borrow remains valid.
   * @return This handle.
   */
  PasswordValue &operator=(PasswordValue &&other) noexcept;
  /** @brief Release this identity; the last owner wipes/releases protected storage. */
  ~PasswordValue() noexcept = default;

  /** @brief Establish mandatory native protection before copying opaque input bytes.
   * @param bytes Borrowed bytes copied synchronously, including embedded zeroes.
   * @param options Mandatory policy and byte bound; weaker storage/encryption is rejected.
   * @return A new identity, including valid empty values, or secret-free failure diagnostics.
   */
  [[nodiscard]] static std::expected<PasswordValue, SecureStringFailure> create(std::string_view bytes = {},
                                                                                SecureStringOptions options = {});
  /** @return Original logical byte count, or zero for a default/moved-from handle. */
  [[nodiscard]] std::size_t size() const noexcept;
  /** @return Whether the original logical byte count is zero; this does not establish availability. */
  [[nodiscard]] bool empty() const noexcept;
  /** @return Whether the shared protected value remains usable; safe from other threads. */
  [[nodiscard]] bool available() const noexcept;
  /** @return Creation policy, or default options for a default/moved-from handle. */
  [[nodiscard]] SecureStringOptions options() const noexcept;
  /** @return Guarantees established at creation, or all false without an identity; check available() separately. */
  [[nodiscard]] SecureStringCapabilities capabilities() const noexcept;
  /** @brief Compare identities without borrowing, decrypting or comparing secret bytes.
   * @param other Identity to compare; all unavailable default/moved-from handles share a null identity.
   * @return Whether both handles retain the same identity.
   */
  [[nodiscard]] bool operator==(const PasswordValue &other) const noexcept = default;

  /** @brief Borrow read-only plaintext synchronously on the creating thread.
   * @tparam F Copyable lvalue or movable rvalue callable returning void or expected<void,E>.
   * @param callback Callable owned under the SecureString busy gate before decryption.
   * @return Independent ownership, storage, typed callback and cleanup failures.
   *
   * State is retained before callable capture or invocation, either of which may
   * destroy the external owner. Wrong-thread calls do not touch protected storage
   * or capture the callable. Nested access to the same identity returns BUSY.
   * If this frame is the last owner after invocation, it checks final retirement.
   * Other retained handles use ordinary noexcept destructor cleanup when released;
   * a concurrent release after the ownership check can also take that path.
   * Views are valid only during invocation. Callback and cleanup guarantees,
   * including owning error constraints, are those of SecureString::withPlaintext.
   */
  template <class F>
    requires detail::SecretCallback<F>
  [[nodiscard]] auto withPlaintext(F &&callback) const
    -> std::expected<void, PasswordValueAccessFailure<typename detail::SecretCallbackResult<
                             std::invoke_result_t<std::decay_t<F> &, SecretView>>::Error>> {
    return accessState(state_, std::forward<F>(callback));
  }

private:
  struct State;

  template <class F>
    requires detail::SecretCallback<F>
  static auto accessState(std::shared_ptr<State> state, F &&callback)
    -> std::expected<void, PasswordValueAccessFailure<typename detail::SecretCallbackResult<
                             std::invoke_result_t<std::decay_t<F> &, SecretView>>::Error>> {
    using Return = std::invoke_result_t<std::decay_t<F> &, SecretView>;
    using Failure = PasswordValueAccessFailure<typename detail::SecretCallbackResult<Return>::Error>;
    using AccessFailure = SecureStringAccessFailure<typename detail::SecretCallbackResult<Return>::Error>;

    if (!state) {
      return std::unexpected(Failure{
        {},
        AccessFailure{SecureStringError{SecureStringOperation::BORROW, SecureStringErrorCode::UNAVAILABLE}, {}, {}}});
    }

    if (auto error = checkOwner(state)) {
      return std::unexpected(Failure{error, {}});
    }

    auto result = protectedStorage(state).withPlaintext(std::forward<F>(callback));

    auto cleanup = finishAccess(state);

    if (cleanup) {
      auto failure = result ? AccessFailure{} : std::move(result.error());

      failure.cleanup.add(cleanup->primary);
      failure.cleanup.append(cleanup->cleanup);

      return std::unexpected(Failure{{}, std::move(failure)});
    }

    if (!result) {
      return std::unexpected(Failure{{}, std::move(result.error())});
    }

    return {};
  }

  explicit PasswordValue(std::shared_ptr<State> state) noexcept;
  static std::expected<PasswordValue, SecureStringFailure> freeze(SecureString &&value, SecureStringOptions options);
  static std::optional<TreeError> checkOwner(const std::shared_ptr<State> &state) noexcept;
  static SecureString &protectedStorage(const std::shared_ptr<State> &state) noexcept;
  static std::optional<SecureStringFailure> finishAccess(const std::shared_ptr<State> &state) noexcept;

  std::shared_ptr<State> state_;
  friend struct detail::PasswordValueAccess;
};

/// @cond INTERNAL
namespace detail {
/** @brief Internal checked editing bridge; never adopts arbitrary caller-owned storage. */
struct PasswordValueAccess {
  /** @brief Consume only the facade's snapshot before scoped admission and checked final retirement.
   * @tparam F Ordinary SecretCallback, captured by SecureString after its admission gate.
   * @param value Private owning snapshot; other caller/model aliases are never reset.
   * @param callback Forwarded original callable, not a borrowed wrapper.
   * @return Exact owner/access/callback/cleanup failure from the existing protected borrow.
   */
  template <class F>
    requires SecretCallback<F>
  static auto borrowOwned(PasswordValue value, F &&callback) {
    return PasswordValue::accessState(std::move(value.state_), std::forward<F>(callback));
  }

  /** @brief Replace a byte range using protected candidates, then publish only after the source lease finishes.
   * @param source Immutable protected value owned by its creating thread.
   * @param first First replaced byte offset, inclusive.
   * @param last Last replaced byte offset, exclusive; first <= last <= source.size() is required.
   * @param insertion Borrowed replacement bytes copied synchronously without an ordinary text intermediate.
   * @return A new protected identity, the same identity for identical bytes, or independent admission/access failures.
   * Invalid offsets return ownership INVALID_ARGUMENT. Nested/foreign-thread access
   * is rejected even for no-op replacements. Candidate errors remain typed callback
   * errors; all borrows finish before the candidate becomes a PasswordValue.
   */
  [[nodiscard]] static std::expected<PasswordValue, PasswordValueAccessFailure<SecureStringFailure>>
  replace(const PasswordValue &source, std::size_t first, std::size_t last, std::string_view insertion);
#if defined(PORTALS_SECURE_STRING_TESTING)
  static void attachTestHooks(const PasswordValue &value, SecureStringTestHooks &hooks) noexcept;
#endif
};
} // namespace detail

/// @endcond

} // namespace portals
