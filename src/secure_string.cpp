// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/secure_string.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(PORTALS_SECURE_STRING_TESTING)
#  include "secure_string_test_access.hpp"
#endif

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
// Windows SDK partition definitions must precede DPAPI declarations.
#  include <windows.h>

#  include <dpapi.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#  include <cerrno>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace portals::detail {
namespace {
constexpr unsigned DESTRUCTOR_RELEASE_ATTEMPTS = 2;

using Operation = SecureStringOperation;
using Code = SecureStringErrorCode;
using Failure = SecureStringFailure;

SecureStringError error(Operation operation, Code code, std::uint32_t nativeCode = 0) noexcept {
  return {operation, code, nativeCode};
}

void wipeBytes(char *data, std::size_t bytes) noexcept {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
  SecureZeroMemory(data, bytes);
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
  explicit_bzero(data, bytes);
#else
  // Volatile accesses are observable side effects, including in WASM linear memory.
  auto *cursor = reinterpret_cast<volatile unsigned char *>(data);

  while (bytes-- != 0) {
    *cursor++ = 0;
  }
#endif
}

#if defined(PORTALS_SECURE_STRING_TESTING)
constexpr std::uint32_t TEST_NATIVE_FAILURE = 0x5052544c;

bool injected(SecureStringTestHooks *hooks, SecureStringTestStep step) noexcept {
  if (!hooks) {
    return false;
  }

  auto &remaining = hooks->failures[static_cast<std::size_t>(step)];

  if (remaining == 0) {
    return false;
  }

  --remaining;

  return true;
}
#endif

struct Mapping {
  char *base{};
  char *payload{};
  std::size_t reservationBytes{};
  std::size_t capacity{};
  bool heap{};
  bool wiped{};
#if defined(PORTALS_SECURE_STRING_TESTING)
  SecureStringTestHooks *hooks{};
#endif

  Mapping() = default;
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

  Mapping(Mapping &&other) noexcept {
    swap(other);
  }

  Mapping &operator=(Mapping &&other) noexcept {
    // Replacement happens only after checked retirement of the previous mapping.
    swap(other);

    return *this;
  }

  ~Mapping() noexcept {
    wipe();

    for (unsigned attempt = 0; base && attempt < DESTRUCTOR_RELEASE_ATTEMPTS; ++attempt) {
      (void)release();
    }
  }

  void swap(Mapping &other) noexcept {
    std::swap(base, other.base);
    std::swap(payload, other.payload);
    std::swap(reservationBytes, other.reservationBytes);
    std::swap(capacity, other.capacity);
    std::swap(heap, other.heap);
    std::swap(wiped, other.wiped);
#if defined(PORTALS_SECURE_STRING_TESTING)
    std::swap(hooks, other.hooks);
#endif
  }

  void wipe() noexcept {
    if (!payload || wiped) {
      return;
    }

    wipeBytes(payload, capacity);
    wiped = true;
#if defined(PORTALS_SECURE_STRING_TESTING)
    if (hooks) {
      ++hooks->wipes;
      hooks->lastWipedCapacity = capacity;
      hooks->lastWipeWasZero = std::all_of(payload, payload + capacity, [](char byte) {
        return byte == 0;
      });
    }
#endif
  }

  std::optional<SecureStringError> release() noexcept {
    if (!base) {
      return {};
    }

#if defined(PORTALS_SECURE_STRING_TESTING)
    if (hooks) {
      ++hooks->releases;
    }

    if (injected(hooks, SecureStringTestStep::RELEASE)) {
      return error(Operation::RELEASE, Code::RELEASE_FAILED, TEST_NATIVE_FAILURE);
    }
#endif

    if (heap) {
      delete[] base;
    } else {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
      if (!VirtualFree(base, 0, MEM_RELEASE)) {
        return error(Operation::RELEASE, Code::RELEASE_FAILED, GetLastError());
      }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
      if (munmap(base, reservationBytes) != 0) {
        return error(Operation::RELEASE, Code::RELEASE_FAILED, static_cast<std::uint32_t>(errno));
      }
#endif
    }

    base = nullptr;
    payload = nullptr;
    reservationBytes = 0;
    capacity = 0;

    return {};
  }
};

std::optional<SecureStringError> validate(const SecureStringOptions &options) noexcept {
  if (options.maxBytes > MAX_SECRET_BYTES ||
      (options.storage != SecureStringStorage::NATIVE_GUARDED && options.storage != SecureStringStorage::WIPE_ONLY)) {
    return error(Operation::CREATE, Code::INVALID_ARGUMENT);
  }

  if (options.storage == SecureStringStorage::WIPE_ONLY) {
    if (options.lockPages || options.excludeFromDumps || options.encryptWhileIdle) {
      return error(Operation::CREATE, Code::UNSUPPORTED);
    }

    return {};
  }

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
  if (options.excludeFromDumps) {
    return error(Operation::DUMP, Code::UNSUPPORTED);
  }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
  if (options.encryptWhileIdle) {
    return error(Operation::ENCRYPT, Code::UNSUPPORTED);
  }
#  if !defined(MADV_DONTDUMP)
  if (options.excludeFromDumps) {
    return error(Operation::DUMP, Code::UNSUPPORTED);
  }
#  endif
#else
  return error(Operation::CREATE, Code::UNSUPPORTED);
#endif

  return {};
}

std::expected<std::size_t, SecureStringError> pageSize() noexcept {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
  SYSTEM_INFO information{};
  GetSystemInfo(&information);

  if (information.dwPageSize != 0) {
    return information.dwPageSize;
  }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
  const auto result = sysconf(_SC_PAGESIZE);

  if (result > 0) {
    return static_cast<std::size_t>(result);
  }
#endif

  return std::unexpected(error(Operation::ALLOCATE, Code::UNSUPPORTED));
}

std::expected<void, Failure> preparationFailure(Mapping &mapping, SecureStringError primary) noexcept {
  mapping.wipe();

  return std::unexpected(Failure{primary, mapping.release()});
}

std::expected<void, Failure> allocate(Mapping &mapping, std::size_t length,
                                      const SecureStringOptions &options) noexcept {
  const auto required = std::max(length, std::size_t{1});

#if defined(PORTALS_SECURE_STRING_TESTING)
  if (mapping.hooks) {
    ++mapping.hooks->allocations;
  }

  if (injected(mapping.hooks, SecureStringTestStep::ALLOCATE)) {
    return std::unexpected(Failure{error(Operation::ALLOCATE, Code::NO_MEMORY, TEST_NATIVE_FAILURE), {}});
  }
#endif

  if (options.storage == SecureStringStorage::WIPE_ONLY) {
    mapping.base = new (std::nothrow) char[required]{};

    if (!mapping.base) {
      return std::unexpected(Failure{error(Operation::ALLOCATE, Code::NO_MEMORY), {}});
    }

    mapping.payload = mapping.base;
    mapping.capacity = required;
    mapping.reservationBytes = required;
    mapping.heap = true;

    return {};
  }

  auto page = pageSize();

  if (!page) {
    return std::unexpected(Failure{page.error(), {}});
  }

  constexpr auto SIZE_LIMIT = std::numeric_limits<std::size_t>::max();

  if (required > SIZE_LIMIT - (*page - 1)) {
    return std::unexpected(Failure{error(Operation::ALLOCATE, Code::TOO_LARGE), {}});
  }

  const auto capacity = ((required + *page - 1) / *page) * *page;

  if (*page > SIZE_LIMIT / 2 || capacity > SIZE_LIMIT - 2 * *page) {
    return std::unexpected(Failure{error(Operation::ALLOCATE, Code::TOO_LARGE), {}});
  }

  const auto reservationBytes = capacity + 2 * *page;

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
  if (options.encryptWhileIdle &&
      (capacity % CRYPTPROTECTMEMORY_BLOCK_SIZE != 0 || capacity > std::numeric_limits<DWORD>::max())) {
    return std::unexpected(Failure{error(Operation::ALLOCATE, Code::TOO_LARGE), {}});
  }

  mapping.base = static_cast<char *>(VirtualAlloc(nullptr, reservationBytes, MEM_RESERVE, PAGE_NOACCESS));

  if (!mapping.base) {
    return std::unexpected(Failure{error(Operation::ALLOCATE, Code::NO_MEMORY, GetLastError()), {}});
  }

  mapping.reservationBytes = reservationBytes;
  auto *payload = mapping.base + *page;

#  if defined(PORTALS_SECURE_STRING_TESTING)
  if (injected(mapping.hooks, SecureStringTestStep::COMMIT)) {
    return preparationFailure(mapping, error(Operation::ALLOCATE, Code::SECURITY_FAILURE, TEST_NATIVE_FAILURE));
  }
#  endif

  if (!VirtualAlloc(payload, capacity, MEM_COMMIT, PAGE_READWRITE)) {
    return preparationFailure(mapping, error(Operation::ALLOCATE, Code::SECURITY_FAILURE, GetLastError()));
  }

  mapping.payload = payload;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
  auto *reservation = mmap(nullptr, reservationBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (reservation == MAP_FAILED) {
    return std::unexpected(Failure{error(Operation::ALLOCATE, Code::NO_MEMORY, static_cast<std::uint32_t>(errno)), {}});
  }

  mapping.base = static_cast<char *>(reservation);
  mapping.payload = mapping.base + *page;
  mapping.reservationBytes = reservationBytes;
  mapping.capacity = capacity;

#  if defined(PORTALS_SECURE_STRING_TESTING)
  if (injected(mapping.hooks, SecureStringTestStep::COMMIT)) {
    return preparationFailure(mapping, error(Operation::ALLOCATE, Code::SECURITY_FAILURE, TEST_NATIVE_FAILURE));
  }
#  endif

  // Only the two exterior pages change permissions; payload remains RW until wiping.
  if (mprotect(mapping.base, *page, PROT_NONE) != 0 || mprotect(mapping.payload + capacity, *page, PROT_NONE) != 0) {
    return preparationFailure(mapping,
                              error(Operation::ALLOCATE, Code::SECURITY_FAILURE, static_cast<std::uint32_t>(errno)));
  }
#else
  return std::unexpected(Failure{error(Operation::ALLOCATE, Code::UNSUPPORTED), {}});
#endif

  mapping.capacity = capacity;

  if (options.lockPages) {
#if defined(PORTALS_SECURE_STRING_TESTING)
    if (injected(mapping.hooks, SecureStringTestStep::LOCK)) {
      return preparationFailure(mapping, error(Operation::LOCK, Code::SECURITY_FAILURE, TEST_NATIVE_FAILURE));
    }
#endif
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (!VirtualLock(mapping.payload, capacity)) {
      return preparationFailure(mapping, error(Operation::LOCK, Code::SECURITY_FAILURE, GetLastError()));
    }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    if (mlock(mapping.payload, capacity) != 0) {
      return preparationFailure(mapping,
                                error(Operation::LOCK, Code::SECURITY_FAILURE, static_cast<std::uint32_t>(errno)));
    }
#endif
  }

  if (options.excludeFromDumps) {
#if defined(PORTALS_SECURE_STRING_TESTING)
    if (injected(mapping.hooks, SecureStringTestStep::DUMP)) {
      return preparationFailure(mapping, error(Operation::DUMP, Code::SECURITY_FAILURE, TEST_NATIVE_FAILURE));
    }
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(MADV_DONTDUMP)
    if (madvise(mapping.payload, capacity, MADV_DONTDUMP) != 0) {
      return preparationFailure(mapping,
                                error(Operation::DUMP, Code::SECURITY_FAILURE, static_cast<std::uint32_t>(errno)));
    }
#endif
  }

  return {};
}

std::optional<SecureStringError> crypt(Mapping &mapping, bool encrypt) noexcept {
  const auto operation = encrypt ? Operation::ENCRYPT : Operation::DECRYPT;

#if defined(PORTALS_SECURE_STRING_TESTING)
  if (injected(mapping.hooks, encrypt ? SecureStringTestStep::ENCRYPT : SecureStringTestStep::DECRYPT)) {
    return error(operation, Code::SECURITY_FAILURE, TEST_NATIVE_FAILURE);
  }
#endif
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
  static_assert(CRYPTPROTECTMEMORY_BLOCK_SIZE == 16);

  if (mapping.capacity % CRYPTPROTECTMEMORY_BLOCK_SIZE != 0 || mapping.capacity > std::numeric_limits<DWORD>::max()) {
    return error(operation, Code::TOO_LARGE);
  }

  const auto length = static_cast<DWORD>(mapping.capacity);
  const auto success = encrypt ? CryptProtectMemory(mapping.payload, length, CRYPTPROTECTMEMORY_SAME_PROCESS)
                               : CryptUnprotectMemory(mapping.payload, length, CRYPTPROTECTMEMORY_SAME_PROCESS);

  if (!success) {
    return error(operation, Code::SECURITY_FAILURE, GetLastError());
  }

  return {};
#else
  (void)mapping;

  return error(operation, Code::UNSUPPORTED);
#endif
}
} // namespace

struct SecureStringState {
  explicit SecureStringState(SecureStringOptions requested) noexcept : options(requested) {
  }

  SecureStringOptions options;
  Mapping mapping;
  SecureStringCapabilities capabilities;
  std::size_t length{};
  bool live{};
  bool busy{};
  bool external{true};
  bool encrypted{};

  std::optional<SecureStringError> retire() noexcept {
    live = false;
    length = 0;
    encrypted = false;
    capabilities = {};
    mapping.wipe();

    return mapping.release();
  }
};

namespace {
std::expected<std::shared_ptr<SecureStringState>, Failure> prepare(std::string_view bytes,
                                                                   const SecureStringOptions &options
#if defined(PORTALS_SECURE_STRING_TESTING)
                                                                   ,
                                                                   SecureStringTestHooks *hooks = nullptr
#endif
) {
  if (auto invalid = validate(options)) {
    return std::unexpected(Failure{*invalid, {}});
  }

  if (bytes.size() > options.maxBytes) {
    return std::unexpected(Failure{error(Operation::CREATE, Code::TOO_LARGE), {}});
  }

  std::shared_ptr<SecureStringState> state;

  try {
    state = std::make_shared<SecureStringState>(options);
  } catch (const std::bad_alloc &) {
    return std::unexpected(Failure{error(Operation::CREATE, Code::NO_MEMORY), {}});
  }

#if defined(PORTALS_SECURE_STRING_TESTING)
  state->mapping.hooks = hooks;
#endif

  auto allocated = allocate(state->mapping, bytes.size(), options);

  if (!allocated) {
    return std::unexpected(allocated.error());
  }

  if (!bytes.empty()) {
    std::memcpy(state->mapping.payload, bytes.data(), bytes.size());
  }

  state->length = bytes.size();

  if (options.encryptWhileIdle) {
    if (auto failure = crypt(state->mapping, true)) {
      return std::unexpected(Failure{*failure, state->retire()});
    }

    state->encrypted = true;
  }

  state->live = true;
  state->capabilities = {true, options.storage == SecureStringStorage::NATIVE_GUARDED, options.lockPages,
                         options.excludeFromDumps, options.encryptWhileIdle};

  return state;
}

std::expected<void, Failure> checkMutable(const std::shared_ptr<SecureStringState> &state,
                                          Operation operation) noexcept {
  if (!state) {
    return std::unexpected(Failure{error(operation, Code::UNAVAILABLE), {}});
  }

  if (state->busy) {
    return std::unexpected(Failure{error(operation, Code::BUSY), {}});
  }

  if (!state->live) {
    return std::unexpected(Failure{error(operation, Code::UNAVAILABLE), {}});
  }

  return {};
}
} // namespace
} // namespace portals::detail

namespace portals {
namespace {
using namespace detail;
} // namespace

SecureString::SecureString(std::shared_ptr<detail::SecureStringState> state) noexcept : state_(std::move(state)) {
}

SecureString::SecureString(SecureString &&other) noexcept : state_(std::move(other.state_)) {
}

SecureString &SecureString::operator=(SecureString &&other) noexcept {
  if (this != &other) {
    detach();
    state_ = std::move(other.state_);
  }

  return *this;
}

SecureString::~SecureString() noexcept {
  detach();
}

void SecureString::detach() noexcept {
  if (state_) {
    state_->external = false;
    state_.reset();
  }
}

std::expected<SecureString, SecureStringFailure> SecureString::create(std::string_view bytes,
                                                                      SecureStringOptions options) {
  auto prepared = prepare(bytes, options);

  if (!prepared) {
    return std::unexpected(prepared.error());
  }

  return SecureString(std::move(*prepared));
}

std::size_t SecureString::size() const noexcept {
  return available() ? state_->length : 0;
}

bool SecureString::empty() const noexcept {
  return size() == 0;
}

bool SecureString::available() const noexcept {
  return state_ && state_->live;
}

SecureStringCapabilities SecureString::capabilities() const noexcept {
  return state_ ? state_->capabilities : SecureStringCapabilities{};
}

std::expected<void, SecureStringFailure> SecureString::close() noexcept {
  if (!state_) {
    return {};
  }

  if (state_->busy) {
    return std::unexpected(Failure{error(Operation::CLOSE, Code::BUSY), {}});
  }

  if (auto failure = state_->retire()) {
    return std::unexpected(Failure{*failure, {}});
  }

  return {};
}

std::expected<void, SecureStringFailure> SecureString::assign(std::string_view bytes) {
  return replace(bytes, false, Operation::ASSIGN);
}

std::expected<void, SecureStringFailure> SecureString::append(std::string_view bytes) {
  return replace(bytes, true, Operation::APPEND);
}

std::expected<void, SecureStringFailure> SecureString::clear() {
  return replace({}, false, Operation::CLEAR);
}

std::expected<void, SecureStringFailure> SecureString::replace(std::string_view bytes, bool appending,
                                                               SecureStringOperation operation) {
  auto checked = checkMutable(state_, operation);

  if (!checked) {
    return checked;
  }

  auto &state = *state_;

  if ((appending && bytes.empty()) || (bytes.empty() && state.length == 0)) {
    return {};
  }

  const auto prefix = appending ? state.length : 0;

  if (bytes.size() > state.options.maxBytes - prefix) {
    return std::unexpected(Failure{error(operation, Code::TOO_LARGE), {}});
  }

  // Apply mandatory native capabilities to the exact final candidate before
  // exposing/copying old plaintext; no owning ordinary string is involved.
  Mapping candidate;
#if defined(PORTALS_SECURE_STRING_TESTING)
  candidate.hooks = state.mapping.hooks;
#endif

  auto allocated = allocate(candidate, prefix + bytes.size(), state.options);

  if (!allocated) {
    return std::unexpected(allocated.error());
  }

  if (appending && state.encrypted) {
    if (auto failure = crypt(state.mapping, false)) {
      Failure result{*failure, state.retire()};
      candidate.wipe();
      result.cleanup.add(candidate.release());

      return std::unexpected(result);
    }

    state.encrypted = false;
  }

  if (prefix != 0) {
    std::memcpy(candidate.payload, state.mapping.payload, prefix);
  }

  if (!bytes.empty()) {
    std::memcpy(candidate.payload + prefix, bytes.data(), bytes.size());
  }

  if (state.options.encryptWhileIdle) {
    if (auto failure = crypt(candidate, true)) {
      candidate.wipe();

      Failure result{*failure, candidate.release()};

      // Append temporarily decrypted the old value. A failed candidate must not
      // leave it plaintext while claiming idle encryption still applies.
      if (appending) {
        if (auto restore = crypt(state.mapping, true)) {
          result.cleanup.add(restore);
          result.cleanup.add(state.retire());
        } else {
          state.encrypted = true;
        }
      }

      return std::unexpected(result);
    }
  }

  if (auto failure = state.retire()) {
    candidate.wipe();

    return std::unexpected(Failure{*failure, candidate.release()});
  }

  state.mapping = std::move(candidate);
  state.length = prefix + bytes.size();
  state.encrypted = state.options.encryptWhileIdle;
  state.live = true;
  state.capabilities = {true, state.options.storage == SecureStringStorage::NATIVE_GUARDED, state.options.lockPages,
                        state.options.excludeFromDumps, state.options.encryptWhileIdle};

  return {};
}

std::expected<void, SecureStringError>
SecureString::beginBorrow(const std::shared_ptr<detail::SecureStringState> &state) noexcept {
  auto checked = checkMutable(state, Operation::BORROW);

  if (!checked) {
    return std::unexpected(checked.error().primary);
  }

  state->busy = true;

  return {};
}

std::expected<SecretView, SecureStringFailure>
SecureString::openBorrow(const std::shared_ptr<detail::SecureStringState> &state) noexcept {
  if (state->encrypted) {
    if (auto failure = crypt(state->mapping, false)) {
      return std::unexpected(Failure{*failure, state->retire()});
    }

    state->encrypted = false;
  }

  return SecretView({state->mapping.payload, state->length});
}

std::optional<SecureStringFailure>
SecureString::finishBorrow(const std::shared_ptr<detail::SecureStringState> &state) noexcept {
  state->busy = false;

  if (!state->external) {
    if (auto failure = state->retire()) {
      return Failure{*failure, {}};
    }

    return {};
  }

  if (state->live && state->options.encryptWhileIdle && !state->encrypted) {
    if (auto failure = crypt(state->mapping, true)) {
      return Failure{*failure, state->retire()};
    }

    state->encrypted = true;
  }

  return {};
}

} // namespace portals

#if defined(PORTALS_SECURE_STRING_TESTING)
namespace portals::detail {
std::expected<SecureString, SecureStringFailure>
SecureStringTestAccess::create(std::string_view bytes, SecureStringOptions options, SecureStringTestHooks &hooks) {
  auto prepared = prepare(bytes, options, &hooks);

  if (!prepared) {
    return std::unexpected(prepared.error());
  }

  return SecureString(std::move(*prepared));
}

void SecureStringTestAccess::attach(SecureString &value, SecureStringTestHooks &hooks) noexcept {
  value.state_->mapping.hooks = &hooks;
}

SecureStringTestProbe SecureStringTestAccess::probe(const SecureString &value) noexcept {
  if (!value.state_) {
    return {};
  }

  const auto &mapping = value.state_->mapping;

  return {mapping.base, mapping.payload, mapping.reservationBytes, mapping.capacity, value.state_->encrypted};
}
} // namespace portals::detail
#endif
