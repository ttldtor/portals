// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/password_value.hpp>

#include <atomic>
#include <cstring>
#include <thread>

#if defined(PORTALS_SECURE_STRING_TESTING)
#  include "secure_string_test_access.hpp"
#endif

namespace portals {

struct PasswordValue::State {
  explicit State(SecureString &&value, SecureStringOptions requested) noexcept
      : storage(std::move(value)), owner(std::this_thread::get_id()), bytes(storage.size()), requested(requested),
        applied(storage.capabilities()) {
  }

  SecureString storage;
  const std::thread::id owner;
  const std::size_t bytes;
  const SecureStringOptions requested;
  const SecureStringCapabilities applied;
  std::atomic<bool> usable{true};
};

PasswordValue::PasswordValue(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {
}

PasswordValue &PasswordValue::operator=(PasswordValue &&other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
  }

  return *this;
}

std::expected<PasswordValue, SecureStringFailure> PasswordValue::create(std::string_view bytes,
                                                                        SecureStringOptions options) {
  if (options.storage != SecureStringStorage::NATIVE_GUARDED || !options.encryptWhileIdle) {
    const auto code =
      options.storage == SecureStringStorage::NATIVE_GUARDED || options.storage == SecureStringStorage::WIPE_ONLY
        ? SecureStringErrorCode::UNSUPPORTED
        : SecureStringErrorCode::INVALID_ARGUMENT;

    return std::unexpected(SecureStringFailure{{SecureStringOperation::CREATE, code}, {}});
  }

  auto value = SecureString::create(bytes, options);

  if (!value) {
    return std::unexpected(value.error());
  }

  return freeze(std::move(*value), options);
}

std::expected<PasswordValue, SecureStringFailure> PasswordValue::freeze(SecureString &&value,
                                                                        SecureStringOptions options) {
  try {
    return PasswordValue(std::make_shared<State>(std::move(value), options));
  } catch (const std::bad_alloc &) {
    SecureStringFailure failure{{SecureStringOperation::CREATE, SecureStringErrorCode::NO_MEMORY}, {}};
    auto cleanup = value.close();

    if (!cleanup) {
      failure.cleanup.add(cleanup.error().primary);
      failure.cleanup.append(cleanup.error().cleanup);
    }

    return std::unexpected(failure);
  }
}

std::size_t PasswordValue::size() const noexcept {
  return state_ ? state_->bytes : 0;
}

bool PasswordValue::empty() const noexcept {
  return size() == 0;
}

bool PasswordValue::available() const noexcept {
  return state_ && state_->usable.load(std::memory_order_relaxed);
}

SecureStringOptions PasswordValue::options() const noexcept {
  return state_ ? state_->requested : SecureStringOptions{};
}

SecureStringCapabilities PasswordValue::capabilities() const noexcept {
  return state_ ? state_->applied : SecureStringCapabilities{};
}

std::optional<TreeError> PasswordValue::checkOwner(const std::shared_ptr<State> &state) noexcept {
  if (state->owner != std::this_thread::get_id()) {
    return TreeError::WRONG_THREAD;
  }

  return {};
}

SecureString &PasswordValue::protectedStorage(const std::shared_ptr<State> &state) noexcept {
  return state->storage;
}

std::optional<SecureStringFailure> PasswordValue::finishAccess(const std::shared_ptr<State> &state) noexcept {
  std::optional<SecureStringFailure> failure;

  // The wrapper pin can outlive every external identity destroyed by the callable.
  // Retire that final storage through checked cleanup before releasing the pin.
  if (state.use_count() == 1) {
    auto closed = state->storage.close();

    if (!closed) {
      failure = closed.error();
    }
  }

  state->usable.store(state->storage.available(), std::memory_order_relaxed);

  return failure;
}

std::expected<PasswordValue, PasswordValueAccessFailure<SecureStringFailure>>
detail::PasswordValueAccess::replace(const PasswordValue &source, std::size_t first, std::size_t last,
                                     std::string_view insertion) {
  using Failure = PasswordValueAccessFailure<SecureStringFailure>;
  using AccessFailure = SecureStringAccessFailure<SecureStringFailure>;

  // Retain the source independently: callable capture/invocation can release the caller's handle.
  const auto retained = source;

  if (first > last || last > retained.size()) {
    return std::unexpected(Failure{TreeError::INVALID_ARGUMENT, {}});
  }

  const auto options = retained.options();
  std::optional<SecureString> candidate;
  bool unchanged = false;
  auto accessed = retained.withPlaintext([&](SecretView view) -> std::expected<void, SecureStringFailure> {
    const auto remaining = view.size() - (last - first);

    if (insertion.size() > options.maxBytes - remaining) {
      return std::unexpected(
        SecureStringFailure{{SecureStringOperation::CREATE, SecureStringErrorCode::TOO_LARGE}, {}});
    }

    unchanged = last - first == insertion.size() &&
                (insertion.empty() || std::memcmp(view.data() + first, insertion.data(), insertion.size()) == 0);

    if (unchanged) {
      return {};
    }

    auto prepared = SecureString::create(std::string_view(view.data(), first), options);

    if (!prepared) {
      return std::unexpected(prepared.error());
    }

    candidate.emplace(std::move(*prepared));

    auto inserted = candidate->append(insertion);

    if (!inserted) {
      return inserted;
    }

    return candidate->append(std::string_view(view.data() + last, view.size() - last));
  });

  if (!accessed) {
    // Discard explicitly so candidate cleanup failures do not disappear in a destructor.
    if (candidate) {
      auto cleanup = candidate->close();

      if (!cleanup) {
        accessed.error().access->cleanup.add(cleanup.error().primary);
        accessed.error().access->cleanup.append(cleanup.error().cleanup);
      }
    }

    return std::unexpected(std::move(accessed.error()));
  }

  if (unchanged) {
    return retained;
  }

  auto frozen = PasswordValue::freeze(std::move(*candidate), options);

  if (!frozen) {
    const auto primary = frozen.error().primary;
    const auto cleanup = frozen.error().cleanup;

    return std::unexpected(Failure{{}, AccessFailure{primary, {}, cleanup}});
  }

  return std::move(*frozen);
}

#if defined(PORTALS_SECURE_STRING_TESTING)
void detail::PasswordValueAccess::attachTestHooks(const PasswordValue &value,
                                                  detail::SecureStringTestHooks &hooks) noexcept {
  detail::SecureStringTestAccess::attach(value.state_->storage, hooks);
}
#endif

} // namespace portals
