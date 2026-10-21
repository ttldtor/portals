// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/secure_string.hpp>

#include "../src/secure_string_test_access.hpp"

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#  include <fstream>
#  include <sstream>
#  include <sys/mman.h>
#endif

using namespace portals;
using portals::detail::SecureStringTestAccess;
using portals::detail::SecureStringTestHooks;
using Step = portals::detail::SecureStringTestStep;
using Code = SecureStringErrorCode;
using Operation = SecureStringOperation;

namespace {
template <class F>
concept AcceptedCallback =
  requires(SecureString &value, F &&callback) { value.withPlaintext(std::forward<F>(callback)); };

template <class R> struct CallbackReturning {
  R operator()(SecretView);
};

struct TaskLike {};

static_assert(!AcceptedCallback<CallbackReturning<const char *>>);
static_assert(!AcceptedCallback<CallbackReturning<SecretView>>);
static_assert(!AcceptedCallback<CallbackReturning<TaskLike>>);
static_assert(!AcceptedCallback<CallbackReturning<std::coroutine_handle<>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, const char *>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, SecretView>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, std::string_view>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, std::span<const char>>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, std::reference_wrapper<int>>>>);
static_assert(!AcceptedCallback<CallbackReturning<std::expected<void, std::ranges::subrange<const char *>>>>);
static_assert(AcceptedCallback<CallbackReturning<void>>);
static_assert(AcceptedCallback<CallbackReturning<std::expected<void, int>>>);
static_assert(AcceptedCallback<CallbackReturning<std::expected<void, std::string>>>);
static_assert(!std::is_convertible_v<SecretView, std::string_view>);
static_assert(!std::is_convertible_v<SecureString, std::string_view>);
static_assert(!std::is_copy_constructible_v<SecureString>);
static_assert(std::is_nothrow_move_constructible_v<SecureString>);
static_assert(std::is_nothrow_move_assignable_v<SecureString>);

SecureStringOptions portableOptions() {
  SecureStringOptions options;
  options.storage = SecureStringStorage::WIPE_ONLY;

  return options;
}

void checkBytes(SecureString &value, std::string_view expected) {
  unsigned calls = 0;
  auto result = value.withPlaintext([&](SecretView view) {
    ++calls;
    CHECK(view.size() == expected.size());
    CHECK(std::string_view(view.data(), view.size()) == expected);
  });

  CHECK(result.has_value());
  CHECK(calls == 1);
}

struct ThrowOnCopy {
  bool noMemory{};

  explicit ThrowOnCopy(bool allocationFailure) : noMemory(allocationFailure) {
  }

  ThrowOnCopy(const ThrowOnCopy &other) {
    if (other.noMemory) {
      throw std::bad_alloc();
    }

    throw std::runtime_error("secret sentinel must not enter library diagnostics");
  }

  void operator()(SecretView) {
  }
};
} // namespace

TEST_CASE("secure string preserves byte counts, zero bytes, exact heap growth and tail wiping") {
  SecureStringTestHooks hooks;
  const char input[]{'a', '\0', 'b'};
  auto value = SecureStringTestAccess::create(std::string_view(input, sizeof(input)), portableOptions(), hooks);
  REQUIRE(value);
  CHECK(value->size() == 3);
  checkBytes(*value, std::string_view(input, sizeof(input)));
  CHECK(SecureStringTestAccess::probe(*value).capacity == 3);

  REQUIRE(value->append("cd"));
  CHECK(hooks.allocations == 2);
  CHECK(hooks.lastWipeWasZero);
  CHECK(hooks.lastWipedCapacity == 3);
  CHECK(SecureStringTestAccess::probe(*value).capacity == 5);
  checkBytes(*value, std::string_view("a\0bcd", 5));

  REQUIRE(value->assign("secret"));
  CHECK(value->size() == 6);
  CHECK(hooks.allocations == 3);
  REQUIRE(value->clear());
  CHECK(value->available());
  CHECK(value->empty());
  CHECK(hooks.lastWipedCapacity == 6);
  CHECK(hooks.lastWipeWasZero);
  CHECK(value->capabilities().wiping);
  REQUIRE(value->close());
  CHECK_FALSE(value->available());
  CHECK_FALSE(value->capabilities().wiping);
  CHECK(value->close());
  CHECK_FALSE(value->assign("new"));
  CHECK_FALSE(value->append({}));
  CHECK_FALSE(value->clear());
}

TEST_CASE("bounds are checked before allocations and empty creation establishes policy") {
  SecureStringTestHooks hooks;
  auto options = portableOptions();
  options.maxBytes = 3;
  auto value = SecureStringTestAccess::create("abc", options, hooks);
  REQUIRE(value);
  auto tooLarge = value->append("d");
  REQUIRE_FALSE(tooLarge);
  CHECK(tooLarge.error().primary.code == Code::TOO_LARGE);
  CHECK(hooks.allocations == 1);
  checkBytes(*value, "abc");

  options.maxBytes = 0;
  auto empty = SecureString::create({}, options);
  REQUIRE(empty);
  CHECK(empty->available());
  CHECK(empty->assign({}));
  CHECK_FALSE(empty->assign("x"));
  CHECK_FALSE(SecureString::create("x", options));

  options.maxBytes = MAX_SECRET_BYTES + 1;
  auto invalid = SecureString::create({}, options);
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().primary.code == Code::INVALID_ARGUMENT);
  options = portableOptions();
  options.lockPages = true;
  auto unsupported = SecureString::create({}, options);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().primary.code == Code::UNSUPPORTED);
}

TEST_CASE("empty no-op mutations and nested borrow reject an active lease") {
  auto value = SecureString::create("secret", portableOptions());
  REQUIRE(value);
  auto result = value->withPlaintext([&](SecretView view) {
    CHECK(view.size() == 6);
    CHECK(value->append({}).error().primary.code == Code::BUSY);
    CHECK(value->assign({}).error().primary.code == Code::BUSY);
    CHECK(value->clear().error().primary.code == Code::BUSY);
    CHECK(value->close().error().primary.code == Code::BUSY);
    auto nested = value->withPlaintext([](SecretView) {
      FAIL("nested callback must not run");
    });
    REQUIRE_FALSE(nested);
    REQUIRE(nested.error().primary);
    CHECK(nested.error().primary->code == Code::BUSY);
    CHECK(std::string_view(view.data(), view.size()) == "secret");
  });

  REQUIRE(result);
  checkBytes(*value, "secret");
}

TEST_CASE("callback errors, exceptions and throwing capture have distinct owning diagnostics") {
  auto value = SecureString::create("secret", portableOptions());
  REQUIRE(value);
  auto typed = value->withPlaintext([](SecretView) -> std::expected<void, int> {
    return std::unexpected(73);
  });
  REQUIRE_FALSE(typed);
  REQUIRE(typed.error().callbackError);
  CHECK(*typed.error().callbackError == 73);
  CHECK(typed.error().primary->code == Code::CALLBACK_FAILED);
  CHECK(typed.error().cleanup.empty());

  auto throwing = value->withPlaintext([](SecretView) {
    throw std::runtime_error("secret sentinel");
  });
  REQUIRE_FALSE(throwing);
  CHECK(throwing.error().primary->code == Code::CALLBACK_FAILED);

  ThrowOnCopy allocationFailure(true);
  auto noMemory = value->withPlaintext(allocationFailure);
  REQUIRE_FALSE(noMemory);
  CHECK(noMemory.error().primary->code == Code::NO_MEMORY);

  ThrowOnCopy captureFailure(false);
  auto capture = value->withPlaintext(captureFailure);
  REQUIRE_FALSE(capture);
  CHECK(capture.error().primary->code == Code::CALLBACK_FAILED);
  checkBytes(*value, "secret");

  auto moveOnly = [token = std::make_unique<int>(42)](SecretView view) {
    CHECK(*token == 42);
    CHECK(view.size() == 6);
  };

  CHECK(value->withPlaintext(std::move(moveOnly)));
}

TEST_CASE("owned callable and lease survive deletion of their originating owner") {
  struct Owner {
    SecureString secret;
    std::function<void(SecretView)> callback;
  };

  SecureStringTestHooks hooks;
  auto value = SecureStringTestAccess::create("secret", portableOptions(), hooks);
  REQUIRE(value);
  auto owner = std::make_unique<Owner>(Owner{std::move(*value), {}});
  bool completed = false;
  owner->callback = [&](SecretView view) {
    owner.reset();
    CHECK(std::string_view(view.data(), view.size()) == "secret");
    CHECK(hooks.wipes == 0);
    completed = true;
  };

  auto result = owner->secret.withPlaintext(owner->callback);

  CHECK(result);
  CHECK(completed);
  CHECK_FALSE(owner);
  CHECK(hooks.wipes == 1);
  CHECK(hooks.lastWipeWasZero);
  CHECK(hooks.releases == 1);
}

TEST_CASE("moves transfer the busy handle and detach an overwritten borrowed destination") {
  auto value = SecureString::create("first", portableOptions());
  auto second = SecureString::create("second", portableOptions());
  REQUIRE(value);
  REQUIRE(second);
  std::optional<SecureString> moved;

  REQUIRE(value->withPlaintext([&](SecretView view) {
    moved.emplace(std::move(*value));
    CHECK_FALSE(value->available());
    CHECK(moved->clear().error().primary.code == Code::BUSY);
    *value = std::move(*moved);
    CHECK(value->append({}).error().primary.code == Code::BUSY);
    CHECK(std::string_view(view.data(), view.size()) == "first");
  }));
  checkBytes(*value, "first");

  SecureStringTestHooks hooks;
  SecureStringTestAccess::attach(*value, hooks);
  REQUIRE(value->withPlaintext([&](SecretView view) {
    *value = std::move(*second);
    CHECK(std::string_view(view.data(), view.size()) == "first");
    CHECK(hooks.wipes == 0);
  }));
  CHECK(hooks.wipes == 1);
  CHECK(hooks.lastWipeWasZero);
  checkBytes(*value, "second");

  auto *same = &*value;
  *value = std::move(*same);
  checkBytes(*value, "second");
}

TEST_CASE("candidate allocation failure preserves the old value") {
  SecureStringTestHooks hooks;
  auto value = SecureStringTestAccess::create("old", portableOptions(), hooks);
  REQUIRE(value);
  hooks.fail(Step::ALLOCATE);
  auto assigned = value->assign("replacement");
  REQUIRE_FALSE(assigned);
  CHECK(assigned.error().primary.code == Code::NO_MEMORY);
  CHECK(hooks.wipes == 0);
  checkBytes(*value, "old");
  hooks.fail(Step::ALLOCATE);
  CHECK_FALSE(value->append("suffix"));
  checkBytes(*value, "old");
}

TEST_CASE("checked release failure retains an already wiped mapping for retry") {
  SecureStringTestHooks hooks;
  auto value = SecureStringTestAccess::create("secret", portableOptions(), hooks);
  REQUIRE(value);
  const auto before = SecureStringTestAccess::probe(*value);
  hooks.fail(Step::RELEASE);
  auto closed = value->close();
  REQUIRE_FALSE(closed);
  CHECK(closed.error().primary.code == Code::RELEASE_FAILED);
  CHECK_FALSE(value->available());
  const auto after = SecureStringTestAccess::probe(*value);
  REQUIRE(after.payload == before.payload);
  CHECK(after.capacity == before.capacity);
  CHECK(std::all_of(after.payload, after.payload + after.capacity, [](char byte) {
    return byte == 0;
  }));
  CHECK(hooks.lastWipeWasZero);
  REQUIRE(value->close());
  CHECK(SecureStringTestAccess::probe(*value).payload == nullptr);
  CHECK(hooks.wipes == 1);
  CHECK(hooks.releases == 2);
}

TEST_CASE("detached owner release failure is returned beside the callback error") {
  SecureStringTestHooks hooks;
  auto value = SecureStringTestAccess::create("secret", portableOptions(), hooks);
  REQUIRE(value);
  std::optional<SecureString> owner(std::move(*value));
  hooks.fail(Step::RELEASE);
  auto result = owner->withPlaintext([&](SecretView view) -> std::expected<void, int> {
    owner.reset();
    CHECK(view.size() == 6);
    return std::unexpected(19);
  });

  REQUIRE_FALSE(result);
  CHECK(*result.error().callbackError == 19);
  REQUIRE(result.error().cleanup.count == 1);
  CHECK(result.error().cleanup.errors[0].code == Code::RELEASE_FAILED);
  CHECK(hooks.lastWipeWasZero);
  CHECK(hooks.releases == 2); // Checked finish failed; the lease destructor retried.
}

TEST_CASE("replacement release failure does not resurrect retired bytes") {
  SecureStringTestHooks hooks;
  auto value = SecureStringTestAccess::create("old", portableOptions(), hooks);
  REQUIRE(value);
  hooks.fail(Step::RELEASE, 2);
  auto result = value->assign("new");
  REQUIRE_FALSE(result);
  CHECK(result.error().primary.code == Code::RELEASE_FAILED);
  REQUIRE(result.error().cleanup.count == 1);
  CHECK(result.error().cleanup.errors[0].code == Code::RELEASE_FAILED);
  CHECK_FALSE(value->available());
  const auto retained = SecureStringTestAccess::probe(*value);
  REQUIRE(retained.payload);
  CHECK(std::all_of(retained.payload, retained.payload + retained.capacity, [](char byte) {
    return byte == 0;
  }));
  CHECK(value->close());
}

TEST_CASE("cleanup diagnostics retain the first bounded errors and count overflow") {
  SecureStringCleanupErrors errors;

  for (std::size_t index = 0; index < MAX_SECURE_CLEANUP_ERRORS + 2; ++index) {
    errors.add(SecureStringError{Operation::RELEASE, Code::RELEASE_FAILED, static_cast<std::uint32_t>(index)});
  }

  CHECK(errors.count == MAX_SECURE_CLEANUP_ERRORS);
  CHECK(errors.omittedCleanupCount == 2);
  CHECK(errors.errors.front().nativeCode == 0);
  CHECK(errors.errors.back().nativeCode == MAX_SECURE_CLEANUP_ERRORS - 1);
}

TEST_CASE("destructor makes only two release attempts and leaves zeroes on persistent failure") {
  SecureStringTestHooks hooks;
  portals::detail::SecureStringTestProbe probe;

  {
    auto value = SecureStringTestAccess::create("secret", portableOptions(), hooks);
    REQUIRE(value);
    probe = SecureStringTestAccess::probe(*value);
    hooks.fail(Step::RELEASE, 2);
  }

  CHECK(hooks.releases == 2);
  CHECK(hooks.wipes == 1);
  CHECK(hooks.lastWipeWasZero);
  CHECK(std::all_of(probe.payload, probe.payload + probe.capacity, [](char byte) {
    return byte == 0;
  }));
  // This fixture owns the intentionally unreleased heap allocation, and cleans
  // it only after observing the destructor's bounded retry/leak contract.
  delete[] static_cast<const char *>(probe.reservation);
}

#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
TEST_CASE("native capacity is page rounded and the tail is wiped before release") {
  SecureStringTestHooks hooks;
  SecureStringOptions options;
  options.maxBytes = MAX_SECRET_BYTES;
  auto value = SecureStringTestAccess::create("secret", options, hooks);
  REQUIRE(value);
  const auto initial = SecureStringTestAccess::probe(*value);
  CHECK(value->capabilities().guardPages);
  CHECK_FALSE(value->capabilities().lockedPages);
  CHECK_FALSE(value->capabilities().excludedFromDumps);
  CHECK_FALSE(value->capabilities().encryptedWhileIdle);
  CHECK(initial.capacity >= value->size());
  CHECK(initial.reservationBytes > initial.capacity);
  std::fill(const_cast<char *>(initial.payload) + value->size(), const_cast<char *>(initial.payload) + initial.capacity,
            'x');
  REQUIRE(value->append("x"));
  CHECK(hooks.lastWipedCapacity == initial.capacity);
  CHECK(hooks.lastWipeWasZero);
  const auto grown = SecureStringTestAccess::probe(*value);
  CHECK(grown.capacity == initial.capacity);
  CHECK(hooks.allocations == 2);
  CHECK(std::all_of(grown.payload + value->size(), grown.payload + grown.capacity, [](char byte) {
    return byte == 0;
  }));

  REQUIRE(value->assign(std::string(initial.capacity + 1, 'y')));
  CHECK(SecureStringTestAccess::probe(*value).capacity == 2 * initial.capacity);
  CHECK(hooks.lastWipeWasZero);
  CHECK(hooks.allocations == 3);
}

TEST_CASE("native capability preparation and cleanup failures occur before secret input copy") {
  SecureStringTestHooks hooks;
  SecureStringOptions options;
  options.lockPages = true;
  hooks.fail(Step::LOCK);
  hooks.fail(Step::RELEASE);
  auto locked = SecureStringTestAccess::create("secret", options, hooks);
  REQUIRE_FALSE(locked);
  CHECK(locked.error().primary.operation == Operation::LOCK);
  CHECK(locked.error().primary.code == Code::SECURITY_FAILURE);
  REQUIRE(locked.error().cleanup.count == 1);
  CHECK(locked.error().cleanup.errors[0].operation == Operation::RELEASE);
  CHECK(hooks.lastWipeWasZero);

  options.lockPages = false;
  hooks.fail(Step::COMMIT);
  CHECK_FALSE(SecureStringTestAccess::create("secret", options, hooks));
}

TEST_CASE("requested native page locking either applies or returns a typed failure") {
  SecureStringOptions options;
  options.lockPages = true;
  auto value = SecureString::create("secret", options);

  if (value) {
    CHECK(value->capabilities().lockedPages);
    checkBytes(*value, "secret");
    CHECK(value->close());
  } else {
    CHECK(value.error().primary.operation == Operation::LOCK);
    CHECK(value.error().primary.code == Code::SECURITY_FAILURE);
  }
}
#endif

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
TEST_CASE("Windows native pages have two inaccessible exterior guards and an RW payload") {
  auto value = SecureString::create("secret");
  REQUIRE(value);
  const auto probe = SecureStringTestAccess::probe(*value);
  MEMORY_BASIC_INFORMATION guard{};
  MEMORY_BASIC_INFORMATION payload{};
  MEMORY_BASIC_INFORMATION trailing{};
  REQUIRE(VirtualQuery(probe.reservation, &guard, sizeof(guard)) == sizeof(guard));
  REQUIRE(VirtualQuery(probe.payload, &payload, sizeof(payload)) == sizeof(payload));
  REQUIRE(VirtualQuery(probe.payload + probe.capacity, &trailing, sizeof(trailing)) == sizeof(trailing));
  CHECK(guard.State == MEM_RESERVE);
  CHECK(trailing.State == MEM_RESERVE);
  CHECK(payload.State == MEM_COMMIT);
  CHECK(payload.Protect == PAGE_READWRITE);
  CHECK(guard.AllocationBase == payload.AllocationBase);
  CHECK(trailing.AllocationBase == payload.AllocationBase);
}

TEST_CASE("Windows idle encryption decrypts only for scoped access and wipes ambiguous failures") {
  SecureStringTestHooks hooks;
  SecureStringOptions options;
  options.encryptWhileIdle = true;
  auto value = SecureStringTestAccess::create("secret", options, hooks);
  REQUIRE(value);
  CHECK(value->capabilities().encryptedWhileIdle);
  auto probe = SecureStringTestAccess::probe(*value);
  CHECK(probe.encrypted);
  CHECK(probe.capacity % 16 == 0);
  CHECK(std::memcmp(probe.payload, "secret", 6) != 0);
  checkBytes(*value, "secret");
  REQUIRE(value->append("s"));
  checkBytes(*value, "secrets");

  hooks.fail(Step::ENCRYPT);
  hooks.fail(Step::RELEASE);
  auto result = value->withPlaintext([](SecretView) -> std::expected<void, int> {
    return std::unexpected(91);
  });
  REQUIRE_FALSE(result);
  CHECK(*result.error().callbackError == 91);
  REQUIRE(result.error().cleanup.count == 2);
  CHECK(result.error().cleanup.errors[0].operation == Operation::ENCRYPT);
  CHECK(result.error().cleanup.errors[1].operation == Operation::RELEASE);
  CHECK_FALSE(value->available());
  CHECK(hooks.lastWipeWasZero);
  REQUIRE(value->close());

  auto decrypt = SecureStringTestAccess::create("secret", options, hooks);
  REQUIRE(decrypt);
  hooks.fail(Step::DECRYPT);
  bool invoked = false;
  auto failed = decrypt->withPlaintext([&](SecretView) {
    invoked = true;
  });
  REQUIRE_FALSE(failed);
  CHECK(failed.error().primary->operation == Operation::DECRYPT);
  CHECK_FALSE(invoked);
  CHECK_FALSE(decrypt->available());
  CHECK(hooks.lastWipeWasZero);
}

TEST_CASE("Windows unsupported dump exclusion is mandatory even for empty values") {
  SecureStringOptions options;
  options.excludeFromDumps = true;
  auto result = SecureString::create({}, options);
  REQUIRE_FALSE(result);
  CHECK(result.error().primary.code == Code::UNSUPPORTED);
}

TEST_CASE("Windows candidate and restoration failures preserve ordered cleanup diagnostics") {
  SecureStringTestHooks hooks;
  SecureStringOptions options;
  options.encryptWhileIdle = true;
  hooks.fail(Step::ENCRYPT);
  auto initialFailure = SecureStringTestAccess::create("secret", options, hooks);
  REQUIRE_FALSE(initialFailure);
  CHECK(initialFailure.error().primary.operation == Operation::ENCRYPT);
  CHECK(hooks.lastWipeWasZero);

  auto value = SecureStringTestAccess::create("secret", options, hooks);
  REQUIRE(value);
  hooks.fail(Step::ENCRYPT, 2);
  hooks.fail(Step::RELEASE, 2);
  auto result = value->append("suffix");
  REQUIRE_FALSE(result);
  CHECK(result.error().primary.operation == Operation::ENCRYPT);
  REQUIRE(result.error().cleanup.count == 3);
  CHECK(result.error().cleanup.errors[0].operation == Operation::RELEASE);
  CHECK(result.error().cleanup.errors[1].operation == Operation::ENCRYPT);
  CHECK(result.error().cleanup.errors[2].operation == Operation::RELEASE);
  CHECK_FALSE(value->available());
  CHECK(hooks.lastWipeWasZero);
  CHECK(value->close());

  auto preserved = SecureStringTestAccess::create("old", options, hooks);
  REQUIRE(preserved);
  hooks.fail(Step::ENCRYPT);
  CHECK_FALSE(preserved->append("suffix"));
  CHECK(preserved->available());
  checkBytes(*preserved, "old");
}
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
TEST_CASE("Linux native permissions remain RW between inaccessible exterior guards") {
  auto value = SecureString::create("secret");
  REQUIRE(value);
  const auto probe = SecureStringTestAccess::probe(*value);
  const auto permissions = [](const void *address) {
    std::ifstream maps("/proc/self/maps");
    std::string line;

    while (std::getline(maps, line)) {
      std::istringstream fields(line);
      std::string range;
      std::string access;
      fields >> range >> access;
      const auto dash = range.find('-');
      const auto begin = std::stoull(range.substr(0, dash), nullptr, 16);
      const auto end = std::stoull(range.substr(dash + 1), nullptr, 16);
      const auto pointer = reinterpret_cast<std::uintptr_t>(address);

      if (pointer >= begin && pointer < end) {
        return access;
      }
    }

    return std::string{};
  };

  CHECK(permissions(probe.reservation).starts_with("---"));
  CHECK(permissions(probe.payload).starts_with("rw-"));
  CHECK(permissions(probe.payload + probe.capacity).starts_with("---"));
}

TEST_CASE("Linux rejects idle encryption but can apply explicit dump exclusion") {
  SecureStringOptions options;
  options.encryptWhileIdle = true;
  auto unsupported = SecureString::create({}, options);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().primary.code == Code::UNSUPPORTED);
#  if defined(MADV_DONTDUMP)
  options.encryptWhileIdle = false;
  options.excludeFromDumps = true;
  auto excluded = SecureString::create("secret", options);
  REQUIRE(excluded);
  CHECK(excluded->capabilities().excludedFromDumps);
#  endif
}
#else
TEST_CASE("unsupported platforms require explicit wipe-only including for empty creation") {
  auto unsupported = SecureString::create();
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().primary.code == Code::UNSUPPORTED);
  auto wiped = SecureString::create("secret", portableOptions());
  REQUIRE(wiped);
  CHECK(wiped->capabilities().wiping);
  CHECK_FALSE(wiped->capabilities().guardPages);
}
#endif
