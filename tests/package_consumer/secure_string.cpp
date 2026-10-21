// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/secure_string.hpp>

#include <expected>
#include <optional>
#include <string_view>
#include <utility>

namespace {
enum class ConsumerError {
  CONTENT,
  BUSY_CHECK,
  EXPECTED_FAILURE
};
} // namespace

int main() {
  using namespace portals;

  auto native = SecureString::create("sample");

#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
  if (!native || !native->capabilities().guardPages || !native->capabilities().encryptedWhileIdle || !native->close()) {
    return 1;
  }
#else
  if (native || native.error().primary.code != SecureStringErrorCode::UNSUPPORTED) {
    return 1;
  }
#endif

  SecureStringOptions options;
  options.storage = SecureStringStorage::WIPE_ONLY;
  options.encryptWhileIdle = false;
  options.maxBytes = 16;

  auto secret = SecureString::create("sample", options);

  if (!secret || secret->size() != 6 || !secret->capabilities().wiping || secret->capabilities().guardPages) {
    return 2;
  }

  // Explicit input length preserves an embedded zero; no owning text conversion.
  if (!secret->assign(std::string_view("a\0b", 3)) || !secret->append("cd")) {
    return 3;
  }

  std::optional<SecureString> transferred;
  auto borrowed = secret->withPlaintext([&](SecretView view) -> std::expected<void, ConsumerError> {
    if (std::string_view(view.data(), view.size()) != std::string_view("a\0bcd", 5)) {
      return std::unexpected(ConsumerError::CONTENT);
    }

    auto nested = secret->withPlaintext([](SecretView) {
    });

    if (nested || !nested.error().primary || nested.error().primary->code != SecureStringErrorCode::BUSY) {
      return std::unexpected(ConsumerError::BUSY_CHECK);
    }

    transferred.emplace(std::move(*secret));

    auto mutation = transferred->append({});

    if (mutation || mutation.error().primary.code != SecureStringErrorCode::BUSY ||
        std::string_view(view.data(), view.size()) != std::string_view("a\0bcd", 5)) {
      return std::unexpected(ConsumerError::BUSY_CHECK);
    }

    return {};
  });

  if (!borrowed || secret->available() || !transferred || !transferred->available()) {
    return 4;
  }

  *secret = std::move(*transferred);

  auto callbackFailure = secret->withPlaintext([](SecretView) -> std::expected<void, ConsumerError> {
    return std::unexpected(ConsumerError::EXPECTED_FAILURE);
  });

  if (callbackFailure || !callbackFailure.error().primary ||
      callbackFailure.error().primary->code != SecureStringErrorCode::CALLBACK_FAILED ||
      callbackFailure.error().callbackError != ConsumerError::EXPECTED_FAILURE ||
      !callbackFailure.error().cleanup.empty()) {
    return 5;
  }

  auto oversized = secret->append("01234567890123456");

  if (oversized || oversized.error().primary.code != SecureStringErrorCode::TOO_LARGE || secret->size() != 5) {
    return 6;
  }

  if (!secret->clear() || !secret->available() || !secret->empty() || !secret->close() || secret->available() ||
      !secret->close()) {
    return 7;
  }

  auto unavailable = secret->assign("sample");

  return !unavailable && unavailable.error().primary.code == SecureStringErrorCode::UNAVAILABLE ? 0 : 8;
}
