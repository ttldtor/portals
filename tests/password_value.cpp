// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/password_value.hpp>
#include <portals/property.hpp>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#if defined(PORTALS_SECURE_STRING_TESTING)
#  include "../src/secure_string_test_access.hpp"
#endif

using namespace portals;
using Code = SecureStringErrorCode;

namespace {
template <class F>
concept AcceptedCallback =
  requires(const PasswordValue &value, F &&callback) { value.withPlaintext(std::forward<F>(callback)); };

template <class R> struct CallbackReturning {
  R operator()(SecretView);
};

struct MoveOnlyError {
  explicit MoveOnlyError(int value) noexcept : code(value) {
  }

  MoveOnlyError(const MoveOnlyError &) = delete;
  MoveOnlyError &operator=(const MoveOnlyError &) = delete;
  MoveOnlyError(MoveOnlyError &&) noexcept = default;
  MoveOnlyError &operator=(MoveOnlyError &&) = delete;
  int code;
};

template <class T>
concept MutableValue = requires(T &value) {
  value.assign("replacement");
  value.clear();
  value.close();
};

static_assert(std::equality_comparable<PasswordValue>);
static_assert(std::is_nothrow_copy_constructible_v<PasswordValue>);
static_assert(std::is_nothrow_copy_assignable_v<PasswordValue>);
static_assert(std::is_nothrow_move_constructible_v<PasswordValue>);
static_assert(std::is_nothrow_move_assignable_v<PasswordValue>);
static_assert(std::is_nothrow_destructible_v<PasswordValue>);
static_assert(!std::constructible_from<PasswordValue, SecureString &&>);
static_assert(!std::is_convertible_v<PasswordValue, std::string>);
static_assert(!std::is_convertible_v<PasswordValue, std::string_view>);
static_assert(!MutableValue<PasswordValue>);
static_assert(AcceptedCallback<CallbackReturning<void>>);
static_assert(AcceptedCallback<CallbackReturning<std::expected<void, int>>>);
static_assert(AcceptedCallback<CallbackReturning<std::expected<void, MoveOnlyError>>>);
static_assert(!AcceptedCallback<CallbackReturning<SecretView>>);
static_assert(!AcceptedCallback<CallbackReturning<std::string>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, std::string_view>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, const char *>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, SecretView>>>);

void checkBytes(const PasswordValue &value, std::string_view expected) {
  unsigned calls = 0;
  auto result = value.withPlaintext([&](SecretView view) {
    ++calls;
    CHECK(view.size() == expected.size());
    const bool matches =
      view.size() == expected.size() && (view.empty() || std::memcmp(view.data(), expected.data(), view.size()) == 0);

    CHECK(matches);
  });

  CHECK(result.has_value());
  CHECK(calls == 1);
}

struct CaptureProbe {
  unsigned *captures;
  unsigned *calls;

  CaptureProbe(unsigned &captured, unsigned &invoked) noexcept : captures(&captured), calls(&invoked) {
  }

  CaptureProbe(const CaptureProbe &other) noexcept : captures(other.captures), calls(other.calls) {
    ++*captures;
  }

  void operator()(SecretView) {
    ++*calls;
  }
};

struct DestroyOwnerOnCapture {
  std::unique_ptr<PasswordValue> *owner;
  bool *invoked;

  DestroyOwnerOnCapture(std::unique_ptr<PasswordValue> &external, bool &called) noexcept
      : owner(&external), invoked(&called) {
  }

  DestroyOwnerOnCapture(const DestroyOwnerOnCapture &other) noexcept : owner(other.owner), invoked(other.invoked) {
    owner->reset();
  }

  void operator()(SecretView view) {
    *invoked = view.size() == 3 && std::memcmp(view.data(), "pin", 3) == 0;
  }
};

struct ThrowOnCapture {
  bool allocation;

  explicit ThrowOnCapture(bool noMemory) noexcept : allocation(noMemory) {
  }

  ThrowOnCapture(const ThrowOnCapture &other) {
    if (other.allocation) {
      throw std::bad_alloc();
    }

    throw std::runtime_error("private capture sentinel");
  }

  void operator()(SecretView) {
  }
};
} // namespace

TEST_CASE("password value default identity is unavailable and does not capture callbacks") {
  const PasswordValue value;
  unsigned captures = 0;
  unsigned calls = 0;
  CaptureProbe callback(captures, calls);
  auto result = value.withPlaintext(callback);

  CHECK(value.empty());
  CHECK(value.size() == 0);
  CHECK_FALSE(value.available());
  CHECK_FALSE(value.capabilities().wiping);
  CHECK(value == PasswordValue{});
  REQUIRE_FALSE(result);
  CHECK_FALSE(result.error().ownership);
  REQUIRE(result.error().access);
  REQUIRE(result.error().access->primary);
  CHECK(result.error().access->primary->code == Code::UNAVAILABLE);
  CHECK(captures == 0);
  CHECK(calls == 0);
}

TEST_CASE("password value rejects weaker policy rather than silently downgrading") {
  SecureStringOptions options;
  options.encryptWhileIdle = false;
  auto unencrypted = PasswordValue::create("secret", options);

  REQUIRE_FALSE(unencrypted);
  CHECK(unencrypted.error().primary.code == Code::UNSUPPORTED);

  options.storage = SecureStringStorage::WIPE_ONLY;

  auto heap = PasswordValue::create("secret", options);

  REQUIRE_FALSE(heap);
  CHECK(heap.error().primary.code == Code::UNSUPPORTED);

  options.storage = static_cast<SecureStringStorage>(99);

  auto invalid = PasswordValue::create({}, options);

  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().primary.code == Code::INVALID_ARGUMENT);
}

#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
TEST_CASE("password value establishes native protection even for empty bytes") {
  auto value = PasswordValue::create();

  REQUIRE(value);
  CHECK(value->available());
  CHECK(value->empty());
  CHECK(value->capabilities().wiping);
  CHECK(value->capabilities().guardPages);
  CHECK(value->capabilities().encryptedWhileIdle);
  CHECK(value->options().storage == SecureStringStorage::NATIVE_GUARDED);
  CHECK(value->options().encryptWhileIdle);
  CHECK(value->options().maxBytes == DEFAULT_MAX_SECRET_BYTES);
  CHECK_FALSE(*value == PasswordValue{});
  checkBytes(*value, {});
}

TEST_CASE("password value preserves opaque bytes and checked finite bounds") {
  SecureStringOptions options;
  options.maxBytes = 3;
  auto value = PasswordValue::create(std::string_view("a\0b", 3), options);

  REQUIRE(value);
  CHECK(value->size() == 3);
  CHECK(value->options().maxBytes == 3);
  checkBytes(*value, std::string_view("a\0b", 3));

  auto large = PasswordValue::create("four", options);

  REQUIRE_FALSE(large);
  CHECK(large.error().primary.code == Code::TOO_LARGE);

  options.maxBytes = MAX_SECRET_BYTES + 1;

  auto invalid = PasswordValue::create({}, options);

  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().primary.code == Code::INVALID_ARGUMENT);
}

TEST_CASE("password copies retain identity and snapshots retain old protected logical values") {
  auto initial = PasswordValue::create("old");
  auto equalBytes = PasswordValue::create("old");
  auto replacement = PasswordValue::create("new");

  REQUIRE(initial);
  REQUIRE(equalBytes);
  REQUIRE(replacement);

  PasswordValue current = *initial;
  auto snapshot = std::make_shared<const PropertyChange<PasswordValue>>(PropertyChange<PasswordValue>{current, 7});

  CHECK(current == *initial);
  CHECK_FALSE(current == *equalBytes);

  current = std::move(*replacement);
  *initial = {};

  CHECK_FALSE(replacement->available());
  CHECK(replacement->size() == 0);
  CHECK(replacement->empty());
  CHECK_FALSE(replacement->capabilities().wiping);
  CHECK_FALSE(current == snapshot->value);
  CHECK(snapshot->revision == 7);
  checkBytes(snapshot->value, "old");
  checkBytes(current, "new");

  current = std::move(current);

  CHECK(current.available());
  checkBytes(current, "new");
}

TEST_CASE("password nested borrowing across copies preserves BUSY without capturing callback") {
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  auto copy = *value;
  unsigned captures = 0;
  unsigned calls = 0;
  CaptureProbe callback(captures, calls);
  auto outer = value->withPlaintext([&](SecretView) {
    auto nested = copy.withPlaintext(callback);

    REQUIRE_FALSE(nested);
    CHECK_FALSE(nested.error().ownership);
    REQUIRE(nested.error().access);
    REQUIRE(nested.error().access->primary);
    CHECK(nested.error().access->primary->code == Code::BUSY);
    CHECK(captures == 0);
    CHECK(calls == 0);
  });

  CHECK(outer);
  CHECK(value->available());
  checkBytes(copy, "secret");
}

TEST_CASE("password thread affinity precedes protected busy state and callback capture") {
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  unsigned captures = 0;
  unsigned calls = 0;
  CaptureProbe callback(captures, calls);
  bool rejected = false;
  bool metadata = false;
  auto outer = value->withPlaintext([&](SecretView) {
    std::thread worker([copy = *value, &callback, &rejected, &metadata] {
      auto result = copy.withPlaintext(callback);

      rejected = !result && result.error().ownership == TreeError::WRONG_THREAD && !result.error().access;
      metadata = copy.available() && copy.size() == 6 && copy.capabilities().encryptedWhileIdle;
    });

    worker.join();
  });

  CHECK(outer);
  CHECK(rejected);
  CHECK(metadata);
  CHECK(captures == 0);
  CHECK(calls == 0);
}

TEST_CASE("password state remains pinned when callback capture destroys its last external owner") {
  auto value = PasswordValue::create("pin");

  REQUIRE(value);

  auto owner = std::make_unique<PasswordValue>(std::move(*value));
  bool invoked = false;
  DestroyOwnerOnCapture callback(owner, invoked);
  auto result = owner->withPlaintext(callback);

  CHECK(result);
  CHECK_FALSE(owner);
  CHECK(invoked);
}

TEST_CASE("password active lease survives callback destruction or movement of external handles") {
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  auto owner = std::make_unique<PasswordValue>(std::move(*value));
  auto destroyed = owner->withPlaintext([&](SecretView view) {
    owner.reset();

    CHECK(view.size() == 6);
    CHECK(std::memcmp(view.data(), "secret", 6) == 0);
  });

  CHECK(destroyed);
  CHECK_FALSE(owner);

  auto next = PasswordValue::create("move");

  REQUIRE(next);

  PasswordValue destination;
  auto moved = next->withPlaintext([&](SecretView view) {
    destination = std::move(*next);

    CHECK_FALSE(next->available());
    CHECK(view.size() == 4);

    auto nested = destination.withPlaintext([](SecretView) {
    });

    REQUIRE_FALSE(nested);
    REQUIRE(nested.error().access);
    CHECK(nested.error().access->primary->code == Code::BUSY);
  });

  CHECK(moved);
  checkBytes(destination, "move");
}

TEST_CASE("password access retains typed errors and rejects callback exceptions without text") {
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  auto typed = value->withPlaintext([](SecretView) -> std::expected<void, int> {
    return std::unexpected(73);
  });

  REQUIRE_FALSE(typed);
  CHECK_FALSE(typed.error().ownership);
  REQUIRE(typed.error().access);
  CHECK(typed.error().access->primary->code == Code::CALLBACK_FAILED);
  REQUIRE(typed.error().access->callbackError);
  CHECK(*typed.error().access->callbackError == 73);
  CHECK(typed.error().access->cleanup.empty());

  auto moveOnly = value->withPlaintext([](SecretView) -> std::expected<void, MoveOnlyError> {
    return std::unexpected(MoveOnlyError{19});
  });

  REQUIRE_FALSE(moveOnly);
  REQUIRE(moveOnly.error().access);
  REQUIRE(moveOnly.error().access->callbackError);
  CHECK(moveOnly.error().access->callbackError->code == 19);

  auto exception = value->withPlaintext([](SecretView) {
    throw std::runtime_error("private callback sentinel");
  });

  REQUIRE_FALSE(exception);
  REQUIRE(exception.error().access);
  CHECK(exception.error().access->primary->code == Code::CALLBACK_FAILED);
  CHECK_FALSE(exception.error().access->callbackError);

  ThrowOnCapture noMemory(true);
  auto capture = value->withPlaintext(noMemory);

  REQUIRE_FALSE(capture);
  REQUIRE(capture.error().access);
  CHECK(capture.error().access->primary->code == Code::NO_MEMORY);

  ThrowOnCapture throwing(false);
  auto failedCapture = value->withPlaintext(throwing);

  REQUIRE_FALSE(failedCapture);
  REQUIRE(failedCapture.error().access);
  CHECK(failedCapture.error().access->primary->code == Code::CALLBACK_FAILED);
  CHECK(value->available());
  checkBytes(*value, "secret");
}

TEST_CASE("password protected replacement inserts replaces deletes and preserves source snapshots") {
  auto value = PasswordValue::create(std::string_view("a\0bc", 4));

  REQUIRE(value);

  auto inserted = detail::PasswordValueAccess::replace(*value, 0, 0, "pre");

  REQUIRE(inserted);
  CHECK_FALSE(*inserted == *value);
  checkBytes(*inserted, std::string_view("prea\0bc", 7));
  checkBytes(*value, std::string_view("a\0bc", 4));

  auto replaced = detail::PasswordValueAccess::replace(*value, 1, 3, std::string_view("x\0", 2));

  REQUIRE(replaced);
  checkBytes(*replaced, std::string_view("ax\0c", 4));

  auto deleted = detail::PasswordValueAccess::replace(*value, 0, 4, {});

  REQUIRE(deleted);
  CHECK(deleted->available());
  CHECK(deleted->empty());
  checkBytes(*deleted, {});

  auto appended = detail::PasswordValueAccess::replace(*deleted, 0, 0, "suffix");

  REQUIRE(appended);
  checkBytes(*appended, "suffix");
}

TEST_CASE("password no-op protected replacement retains identity after checking busy and affinity") {
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  auto empty = detail::PasswordValueAccess::replace(*value, 2, 2, {});
  auto identical = detail::PasswordValueAccess::replace(*value, 1, 4, "ecr");

  REQUIRE(empty);
  REQUIRE(identical);
  CHECK(*empty == *value);
  CHECK(*identical == *value);

  auto outer = value->withPlaintext([&](SecretView) {
    auto nested = detail::PasswordValueAccess::replace(*value, 0, 0, {});

    REQUIRE_FALSE(nested);
    REQUIRE(nested.error().access);
    CHECK(nested.error().access->primary->code == Code::BUSY);
  });

  CHECK(outer);

  bool rejected = false;
  std::thread worker([copy = *value, &rejected] {
    auto result = detail::PasswordValueAccess::replace(copy, 0, 0, {});

    rejected = !result && result.error().ownership == TreeError::WRONG_THREAD && !result.error().access;
  });

  worker.join();

  CHECK(rejected);
}

TEST_CASE("password protected replacement rejects invalid ranges and retains typed candidate failures") {
  SecureStringOptions options;
  options.maxBytes = 6;
  auto value = PasswordValue::create("secret", options);

  REQUIRE(value);

  auto reversed = detail::PasswordValueAccess::replace(*value, 4, 1, {});
  auto outside = detail::PasswordValueAccess::replace(*value, 0, 7, {});

  REQUIRE_FALSE(reversed);
  REQUIRE_FALSE(outside);
  CHECK(reversed.error().ownership == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(reversed.error().access);
  CHECK(outside.error().ownership == TreeError::INVALID_ARGUMENT);

  auto excessive = detail::PasswordValueAccess::replace(*value, 0, 0, "extra");

  REQUIRE_FALSE(excessive);
  CHECK_FALSE(excessive.error().ownership);
  REQUIRE(excessive.error().access);
  CHECK(excessive.error().access->primary->code == Code::CALLBACK_FAILED);
  REQUIRE(excessive.error().access->callbackError);
  CHECK(excessive.error().access->callbackError->primary.code == Code::TOO_LARGE);
  CHECK(excessive.error().access->cleanup.empty());
  checkBytes(*value, "secret");

  auto unavailable = detail::PasswordValueAccess::replace(PasswordValue{}, 0, 0, {});

  REQUIRE_FALSE(unavailable);
  REQUIRE(unavailable.error().access);
  CHECK(unavailable.error().access->primary->code == Code::UNAVAILABLE);
}

#  if defined(PORTALS_SECURE_STRING_TESTING)
TEST_CASE("password decryption failure invalidates all shared handles without changing cached metadata") {
  detail::SecureStringTestHooks hooks;
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  auto copy = *value;

  detail::PasswordValueAccess::attachTestHooks(*value, hooks);
  hooks.fail(detail::SecureStringTestStep::DECRYPT);

  unsigned calls = 0;
  auto failed = value->withPlaintext([&](SecretView) {
    ++calls;
  });

  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().access);
  REQUIRE(failed.error().access->primary);
  CHECK(failed.error().access->primary->operation == SecureStringOperation::DECRYPT);
  CHECK(failed.error().access->primary->code == Code::SECURITY_FAILURE);
  CHECK(calls == 0);
  CHECK_FALSE(value->available());
  CHECK_FALSE(copy.available());
  CHECK(copy.size() == 6);
  CHECK(copy.capabilities().encryptedWhileIdle);
  CHECK(copy == *value);
  CHECK(hooks.lastWipeWasZero);

  auto unavailable = copy.withPlaintext([](SecretView) {
  });

  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().access->primary->code == Code::UNAVAILABLE);
}

TEST_CASE("password typed callback failure retains reprotection and release failures independently") {
  detail::SecureStringTestHooks hooks;
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  detail::PasswordValueAccess::attachTestHooks(*value, hooks);
  hooks.fail(detail::SecureStringTestStep::ENCRYPT);
  hooks.fail(detail::SecureStringTestStep::RELEASE);

  auto failed = value->withPlaintext([](SecretView) -> std::expected<void, MoveOnlyError> {
    return std::unexpected(MoveOnlyError{23});
  });

  REQUIRE_FALSE(failed);
  CHECK_FALSE(failed.error().ownership);
  REQUIRE(failed.error().access);
  CHECK(failed.error().access->primary->code == Code::CALLBACK_FAILED);
  REQUIRE(failed.error().access->callbackError);
  CHECK(failed.error().access->callbackError->code == 23);
  REQUIRE(failed.error().access->cleanup.count >= 2);
  CHECK(failed.error().access->cleanup.errors[0].operation == SecureStringOperation::ENCRYPT);
  CHECK(failed.error().access->cleanup.errors[1].code == Code::RELEASE_FAILED);
  CHECK_FALSE(value->available());
  CHECK(hooks.lastWipeWasZero);
}

TEST_CASE("password callback destruction of last owner checks final release failure") {
  detail::SecureStringTestHooks hooks;
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  auto owner = std::make_unique<PasswordValue>(std::move(*value));

  detail::PasswordValueAccess::attachTestHooks(*owner, hooks);
  hooks.fail(detail::SecureStringTestStep::RELEASE);

  auto failed = owner->withPlaintext([&](SecretView) {
    owner.reset();
  });

  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().access);
  CHECK_FALSE(failed.error().access->primary);
  REQUIRE(failed.error().access->cleanup.count >= 1);
  CHECK(failed.error().access->cleanup.errors[0].code == Code::RELEASE_FAILED);
  CHECK_FALSE(owner);
  CHECK(hooks.lastWipeWasZero);
  CHECK(hooks.releases >= 2);
}

TEST_CASE("consumed facade snapshot reports last-owner release failure after callback removal") {
  detail::SecureStringTestHooks hooks;
  auto original = PasswordValue::create("secret");

  REQUIRE(original);
  detail::PasswordValueAccess::attachTestHooks(*original, hooks);
  auto selected = *original;
  hooks.fail(detail::SecureStringTestStep::RELEASE);

  auto failed = detail::PasswordValueAccess::borrowOwned(std::move(selected), [&](SecretView secret) {
    CHECK(secret.size() == 6);
    *original = PasswordValue{};
  });

  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().access);
  CHECK_FALSE(failed.error().access->primary);
  REQUIRE(failed.error().access->cleanup.count >= 1);
  CHECK(failed.error().access->cleanup.errors[0].code == Code::RELEASE_FAILED);
  CHECK_FALSE(selected.available());
  CHECK_FALSE(original->available());
  CHECK(hooks.lastWipeWasZero);
  CHECK(hooks.releases >= 2);
}

TEST_CASE("password protected replacement discards candidate if source reprotection fails") {
  detail::SecureStringTestHooks hooks;
  auto value = PasswordValue::create("secret");

  REQUIRE(value);

  detail::PasswordValueAccess::attachTestHooks(*value, hooks);
  hooks.fail(detail::SecureStringTestStep::ENCRYPT);

  auto failed = detail::PasswordValueAccess::replace(*value, 0, 1, "S");

  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().access);
  CHECK_FALSE(failed.error().access->primary);
  CHECK_FALSE(failed.error().access->callbackError);
  REQUIRE(failed.error().access->cleanup.count >= 1);
  CHECK(failed.error().access->cleanup.errors[0].operation == SecureStringOperation::ENCRYPT);
  CHECK_FALSE(value->available());
  CHECK(hooks.lastWipeWasZero);
}
#  endif
#else
TEST_CASE("password value reports unsupported mandatory protection on nonnative targets") {
  auto value = PasswordValue::create("secret");

  REQUIRE_FALSE(value);
  CHECK(value.error().primary.code == Code::UNSUPPORTED);
}
#endif
