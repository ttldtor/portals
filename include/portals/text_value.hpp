// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <portals/text.hpp>
#include <portals/text_resource.hpp>

#include <concepts>
#include <format>
#include <functional>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace portals {

/** @brief Original text validation error or a distinct resource lookup error. */
using TextValueError = std::variant<TextError, TextResourceError>;

/**
 * @brief Owning literal bytes or a resource key, measured only at resolution.
 *
 * No locale, resolver, display policy or mutable global state is retained.
 * Construction copies bytes without validation, repair or normalization.
 * Resolution validates under the receiving scene's explicit policy. Re-resolve
 * after changing policy or resources; the caller owns layout/paint invalidation.
 * Independent values have no thread affinity. Concurrent reads are safe when
 * the caller's resolver is safe; mutation/destruction require synchronization.
 * Construction and copy may throw std::bad_alloc or std::length_error.
 * With the MSVC standard library and nonzero _ITERATOR_DEBUG_LEVEL, rvalue
 * construction/assignment use fallible owning copies to avoid noexcept proxy
 * allocation. Other library modes retain their default string moves.
 */
class TextValue {
public:
  /** @brief Release the independently owned expression bytes. */
  constexpr ~TextValue() = default;

  /** @brief Copy expression bytes and kind into independent storage.
   * @param other Source expression; copying may allocate and leaves it unchanged.
   */
  constexpr TextValue(const TextValue &other) = default;

  /** @brief Replace expression bytes and kind with an owning copy.
   * @param other Source expression; self-assignment is supported.
   * @return This expression; copying may allocate.
   */
  constexpr TextValue &operator=(const TextValue &other) = default;

#if defined(_MSVC_STL_VERSION) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
  /** @brief Copy on rvalue construction with the checked MSVC standard library.
   * @param other Source preserved unchanged; its debug string proxy can allocate even in a noexcept move.
   * This fallible copy keeps allocation failure catchable. Other standard-library modes use default moves.
   */
  constexpr TextValue(TextValue &&other) : TextValue(static_cast<const TextValue &>(other)) {
  }

  /** @brief Copy on rvalue assignment with the checked MSVC standard library.
   * @param other Source preserved unchanged, including self-assignment.
   * @return This expression; allocation failures remain catchable.
   */
  constexpr TextValue &operator=(TextValue &&other) {
    return *this = static_cast<const TextValue &>(other);
  }
#else
  /** @brief Transfer expression storage using the standard library's default move.
   * @param other Source expression; its moved-from value remains valid but unspecified.
   */
  constexpr TextValue(TextValue &&other) = default;

  /** @brief Transfer expression storage using the standard library's default move assignment.
   * @param other Source expression; existing storage is released.
   * @return This expression; noexcept follows the underlying string move assignment.
   */
  constexpr TextValue &operator=(TextValue &&other) = default;
#endif

  /**
   * @brief Copy a literal buffer into independent expression storage.
   * @param bytes Literal bytes borrowed only during construction, including invalid UTF-8.
   * @throws std::bad_alloc If expression storage cannot be allocated.
   * @throws std::length_error If the buffer exceeds string storage limits.
   */
  constexpr explicit TextValue(std::string_view bytes) : bytes_(bytes) {
  }

  /**
   * @brief Inspect the resource key without resolving or allocating.
   * @return Borrowed key bytes, or an empty view for a literal; an empty resource key is also permitted.
   * @note The view is invalidated by this value's assignment, move or destruction.
   */
  [[nodiscard]] constexpr std::string_view resourceKey() const noexcept {
    return resource_ ? std::string_view(bytes_) : std::string_view{};
  }

  /**
   * @brief Resolve literal bytes without requiring a resource resolver.
   * @param policy Owning display policy captured by the resulting text.
   * @param limits Resource caps applied to the entire resolved literal buffer.
   * @return Independent validated text, the original TextError, or RESOLVER_REQUIRED for a key.
   * @throws std::bad_alloc If validation storage cannot be allocated.
   * @throws std::length_error If validation storage exceeds implementation limits.
   */
  [[nodiscard]] std::expected<GraphemeText, TextValueError> resolve(WidthPolicy policy, TextLimits limits = {}) const {
    if (resource_) {
      return std::unexpected(TextValueError{TextResourceError::RESOLVER_REQUIRED});
    }

    return validate(bytes_, policy, limits);
  }

  /**
   * @brief Resolve a key synchronously through the caller, or validate a literal directly.
   * @tparam Resolver Callable accepting std::string_view and returning exactly expected<string, TextResourceError>.
   * Move-only callables are supported; the callable is not copied or retained.
   * @param policy Owning display policy captured by the resulting text.
   * @param resolver Borrowed for this call; invoked once for a key and never for a literal.
   * The key view is borrowed only for the callback. Resource acquisition must not block paint/measure.
   * @param limits Resource caps applied to the entire resolved buffer, including translated bytes.
   * @return Independent validated text, the original TextError or the resolver's resource error.
   * Non-allocation callback exceptions become RESOLVER_FAILED without retaining exception details.
   * @throws std::bad_alloc If the callback or text storage allocation fails.
   * @throws std::length_error If the callback or text storage exceeds implementation limits.
   */
  template <class Resolver>
    requires std::invocable<Resolver, std::string_view> &&
             std::same_as<std::invoke_result_t<Resolver, std::string_view>,
                          std::expected<std::string, TextResourceError>>
  [[nodiscard]] std::expected<GraphemeText, TextValueError> resolve(WidthPolicy policy, Resolver &&resolver,
                                                                    TextLimits limits = {}) const {
    if (!resource_) {
      return validate(bytes_, policy, limits);
    }

    const auto resolved = resolveResource(std::forward<Resolver>(resolver));

    if (!resolved) {
      return std::unexpected(TextValueError{resolved.error()});
    }

    return validate(*resolved, policy, limits);
  }

private:
  friend constexpr TextValue i18n(std::string_view key);

  constexpr TextValue(std::string_view key, bool resource) : bytes_(key), resource_(resource) {
  }

  static std::expected<GraphemeText, TextValueError> validate(std::string_view bytes, WidthPolicy policy,
                                                              TextLimits limits) {
    auto resolved = GraphemeText::from(bytes, policy, limits);

    if (!resolved) {
      return std::unexpected(TextValueError{resolved.error()});
    }

    return std::move(*resolved);
  }

  template <class Resolver> std::expected<std::string, TextResourceError> resolveResource(Resolver &&resolver) const {
    try {
      return std::invoke(std::forward<Resolver>(resolver), std::string_view(bytes_));
    } catch (const std::bad_alloc &) {
      throw;
    } catch (const std::length_error &) {
      throw;
    } catch (...) {
      return std::unexpected(TextResourceError::RESOLVER_FAILED);
    }
  }

  std::string bytes_;
  bool resource_{};
};

/**
 * @brief Copy literal bytes into an owning expression without interpreting braces.
 * @param bytes Literal buffer borrowed only during this call; UTF-8 validation is deferred.
 * @return Independent expression, including for empty or invalid input.
 * @throws std::bad_alloc If expression storage cannot be allocated.
 * @throws std::length_error If expression storage exceeds implementation limits.
 * @note With no format arguments, text("{}") stores the literal braces unchanged.
 */
[[nodiscard]] constexpr TextValue text(std::string_view bytes) {
  return TextValue(bytes);
}

/**
 * @brief Format immediately into owned bytes; defer validation and measurement.
 * @tparam Args One or more argument types accepted by the checked standard format string.
 * @param pattern Standard format string checked against Args at compilation.
 * @param args Values borrowed only during formatting; no references are retained.
 * @return Independent expression containing the complete formatted bytes.
 * @throws std::bad_alloc If formatting or expression storage cannot be allocated.
 * @throws std::length_error If storage exceeds implementation limits.
 * @throws std::format_error If formatting fails, including through a custom formatter.
 * @note Other exceptions from user-defined formatters also propagate unchanged.
 */
template <class... Args>
  requires(sizeof...(Args) > 0)
[[nodiscard]] TextValue text(std::format_string<Args...> pattern, Args &&...args) {
  return text(std::format(pattern, std::forward<Args>(args)...));
}

/**
 * @brief Copy a resource key for later resolution through an explicit caller.
 * @param key Opaque resource key borrowed only during this call; empty keys are permitted.
 * @return Owning expression that resolves the key anew on every resolve call.
 * @throws std::bad_alloc If key storage cannot be allocated.
 * @throws std::length_error If key storage exceeds implementation limits.
 */
[[nodiscard]] constexpr TextValue i18n(std::string_view key) {
  return TextValue(key, true);
}

} // namespace portals
