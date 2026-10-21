// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

// Private fixture interface. Only tests/secure_string.cpp and its dedicated copy
// of src/secure_string.cpp are compiled with PORTALS_SECURE_STRING_TESTING.
#ifndef PORTALS_SECURE_STRING_TESTING
#  error "SecureString backend fixtures are private to the dedicated test target"
#endif

#include <portals/secure_string.hpp>

#include <array>

namespace portals::detail {

enum class SecureStringTestStep {
  ALLOCATE,
  COMMIT,
  LOCK,
  DUMP,
  ENCRYPT,
  DECRYPT,
  RELEASE,
  KEY_COMMIT,
  KEY_LOCK,
  KEY_DUMP,
  ENTROPY,
  KEY_ENTROPY,
  NONCE_ENTROPY,
  COUNT
};

struct SecureStringTestEntropyResult {
  std::ptrdiff_t bytes{};
  int error{};
};

struct SecureStringTestHooks {
  std::array<unsigned, static_cast<std::size_t>(SecureStringTestStep::COUNT)> failures{};
  std::size_t allocations{};
  std::size_t releases{};
  std::size_t wipes{};
  std::size_t lastWipedCapacity{};
  bool lastWipeWasZero{};
  std::size_t keyWipes{};
  std::size_t lastWipedKeyCapacity{};
  bool lastKeyWipeWasZero{};
  std::size_t keyLocks{};
  std::size_t payloadLocks{};
  std::size_t keyDumpExclusions{};
  std::size_t payloadDumpExclusions{};
  std::size_t entropyCalls{};
  std::size_t keyGenerations{};
  std::size_t nonceGenerations{};
  std::array<SecureStringTestEntropyResult, 64> entropyResults{};
  std::size_t entropyResultCount{};
  std::size_t entropyResultIndex{};

  void fail(SecureStringTestStep step, unsigned count = 1) noexcept {
    failures[static_cast<std::size_t>(step)] = count;
  }
};

struct SecureStringTestProbe {
  const void *reservation{};
  const char *payload{};
  std::size_t reservationBytes{};
  std::size_t capacity{};
  bool encrypted{};
  const char *keyPage{};
  std::size_t keyCapacity{};
  std::array<unsigned char, 24> nonce{};
};

class SecureStringTestAccess {
public:
  static std::expected<SecureString, SecureStringFailure> create(std::string_view bytes, SecureStringOptions options,
                                                                 SecureStringTestHooks &hooks);
  static void attach(SecureString &value, SecureStringTestHooks &hooks) noexcept;
  static SecureStringTestProbe probe(const SecureString &value) noexcept;
};

} // namespace portals::detail
