// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/source_budget.hpp>

#include <atomic>
#include <portals/scheduler.hpp>
#include <utility>

namespace portals::detail {
struct SourceCapacityState {
  SourceCredit credit;
  std::shared_ptr<SourceBudget> budget;
  Publisher<int> wake;
  std::weak_ptr<SourceCapacityState> self;
  SourceCapacityState *previous{};
  SourceCapacityState *next{};
  SourceBudgetUsage peak;
  std::atomic<bool> ready{};
  bool linked{};
  bool armed{};

  ~SourceCapacityState() {
    if (budget) {
      budget->removeObserver(*this);
    }
  }
};
} // namespace portals::detail

namespace portals {
namespace {

bool fits(SourceBudgetUsage amount, SourceBudgetUsage limit) {
  return amount.bytes <= limit.bytes && amount.pages <= limit.pages && amount.work <= limit.work &&
         amount.consumers <= limit.consumers;
}

void subtract(SourceBudgetUsage &from, SourceBudgetUsage amount) {
  from.bytes -= amount.bytes;
  from.pages -= amount.pages;
  from.work -= amount.work;
  from.consumers -= amount.consumers;
}
} // namespace

SourceCredit::SourceCredit(std::shared_ptr<SourceBudget> root, SourceBudgetUsage amount) noexcept
    : root_(std::move(root)), amount_(amount) {
}

SourceCredit::~SourceCredit() {
  if (root_) {
    root_->release(amount_);
  }
}

SourceCredit::SourceCredit(SourceCredit &&other) noexcept
    : root_(std::move(other.root_)), amount_(std::exchange(other.amount_, {})) {
}

SourceCredit &SourceCredit::operator=(SourceCredit other) noexcept {
  root_.swap(other.root_);
  std::swap(amount_, other.amount_);

  return *this;
}

std::expected<SourceCredit, SourceError> SourceCredit::split(SourceBudgetUsage amount) noexcept {
  if (!root_ || !fits(amount, amount_)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  subtract(amount_, amount);

  return SourceCredit(root_, amount);
}

SourceBudgetUsage SourceCredit::usage() const noexcept {
  return amount_;
}

SourceBudget::SourceBudget(SourceBudgetOptions options) : options_(options), used_{ROOT_METADATA_BYTES, 0, 0, 0} {
}

std::expected<std::shared_ptr<SourceBudget>, SourceError> SourceBudget::create(SourceBudgetOptions options) {
  static_assert(sizeof(SourceBudget) < ROOT_METADATA_BYTES / 2);

  if (options.maxBytes < ROOT_METADATA_BYTES || !options.maxPages || !options.maxWork || !options.maxConsumers) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  try {
    return std::shared_ptr<SourceBudget>(new SourceBudget(options));
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

std::expected<SourceCredit, SourceError> SourceBudget::reserve(SourceBudgetUsage amount) {
  std::lock_guard lock(mutex_);
  SourceBudgetUsage available{options_.maxBytes - used_.bytes, options_.maxPages - used_.pages,
                              options_.maxWork - used_.work, options_.maxConsumers - used_.consumers};

  if (!fits(amount, available)) {
    return std::unexpected(SourceError::WOULD_BLOCK);
  }

  used_.bytes += amount.bytes;
  used_.pages += amount.pages;
  used_.work += amount.work;
  used_.consumers += amount.consumers;

  return SourceCredit(shared_from_this(), amount);
}

SourceBudgetUsage SourceBudget::usage() const {
  std::lock_guard lock(mutex_);

  return used_;
}

SourceBudgetOptions SourceBudget::options() const noexcept {
  return options_;
}

void SourceBudget::release(SourceBudgetUsage amount) noexcept {
  if (!amount.bytes && !amount.pages && !amount.work && !amount.consumers) {
    return;
  }
  std::shared_ptr<detail::SourceCapacityState> wake;
  {
    std::lock_guard lock(mutex_);
    subtract(used_, amount);
    ++capacityEpoch_;
    wake = eligibleObserverLocked();
  }
  if (wake) {
    wake->wake.wake();
  }
}

SourceCapacityObserver::SourceCapacityObserver(std::shared_ptr<detail::SourceCapacityState> state) noexcept
    : state_(std::move(state)) {
}

SourceCapacityObserver::~SourceCapacityObserver() {
  if (state_) {
    state_->budget->removeObserver(*state_);
  }
}

SourceCapacityObserver::SourceCapacityObserver(SourceCapacityObserver &&) noexcept = default;

SourceCapacityObserver &SourceCapacityObserver::operator=(SourceCapacityObserver &&other) noexcept {
  if (this != &other) {
    if (state_) {
      state_->budget->removeObserver(*state_);
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

std::expected<SourceCredit, SourceError> SourceCapacityObserver::reserve(SourceBudgetUsage amount,
                                                                         SourceBudgetUsage peak) {
  return state_ ? state_->budget->reserveObserved(*state_, amount, peak) : std::unexpected(SourceError::CLOSED);
}

std::expected<void, SourceError> SourceCapacityObserver::arm(SourceBudgetUsage peak) {
  return state_ ? state_->budget->armObserver(*state_, peak) : std::unexpected(SourceError::CLOSED);
}

bool SourceCapacityObserver::takeReady() noexcept {
  return state_ && state_->ready.exchange(false, std::memory_order_acq_rel);
}

void SourceCapacityObserver::disarm() noexcept {
  if (state_) {
    state_->budget->disarmObserver(*state_);
  }
}

std::shared_ptr<detail::SourceCapacityState> SourceBudget::eligibleObserverLocked() noexcept {
  if (granted_) {
    return {};
  }
  const SourceBudgetUsage available{options_.maxBytes - used_.bytes, options_.maxPages - used_.pages,
                                    options_.maxWork - used_.work, options_.maxConsumers - used_.consumers};
  for (auto *candidate = observers_; candidate; candidate = candidate->next) {
    if (candidate->armed && fits(candidate->peak, available)) {
      if (auto owner = candidate->self.lock()) {
        granted_ = candidate;
        candidate->armed = false;
        candidate->ready.store(true, std::memory_order_release);
        return owner;
      }
    }
  }
  return {};
}

std::expected<SourceCapacityObserver, SourceError> SourceBudget::observeCapacity(Publisher<int> wake) {
  static_assert(sizeof(detail::SourceCapacityState) < CAPACITY_OBSERVER_BYTES / 2);
  auto credit = reserve({CAPACITY_OBSERVER_BYTES, 0, 0, 0});
  if (!credit) {
    return std::unexpected(credit.error());
  }
  try {
    auto state = std::make_shared<detail::SourceCapacityState>();
    state->credit = std::move(*credit);
    state->budget = shared_from_this();
    state->wake = std::move(wake);
    state->self = state;
    {
      std::lock_guard lock(mutex_);
      if (observerCount_ >= options_.maxConsumers) {
        return std::unexpected(SourceError::WOULD_BLOCK);
      }
      state->next = observers_;
      if (observers_) {
        observers_->previous = state.get();
      }
      observers_ = state.get();
      state->linked = true;
      ++observerCount_;
    }
    return SourceCapacityObserver(std::move(state));
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

std::uint64_t SourceBudget::capacityEpoch() const noexcept {
  std::lock_guard lock(mutex_);
  return capacityEpoch_;
}

void SourceBudget::removeObserver(detail::SourceCapacityState &state) noexcept {
  std::shared_ptr<detail::SourceCapacityState> wake;
  {
    std::lock_guard lock(mutex_);
    if (!state.linked) {
      return;
    }
    if (state.previous) {
      state.previous->next = state.next;
    } else {
      observers_ = state.next;
    }
    if (state.next) {
      state.next->previous = state.previous;
    }
    state.linked = false;
    --observerCount_;
    if (granted_ == &state) {
      granted_ = nullptr;
    }
    wake = eligibleObserverLocked();
  }
  if (wake) {
    wake->wake.wake();
  }
}

void SourceBudget::disarmObserver(detail::SourceCapacityState &state) noexcept {
  std::shared_ptr<detail::SourceCapacityState> wake;
  {
    std::lock_guard lock(mutex_);
    state.armed = false;
    state.ready.store(false, std::memory_order_release);
    if (granted_ == &state) {
      granted_ = nullptr;
    }
    wake = eligibleObserverLocked();
  }
  if (wake) {
    wake->wake.wake();
  }
}

std::expected<void, SourceError> SourceBudget::armObserver(detail::SourceCapacityState &state, SourceBudgetUsage peak) {
  const SourceBudgetUsage limits{options_.maxBytes, options_.maxPages, options_.maxWork, options_.maxConsumers};
  if (!fits(peak, limits)) {
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }
  std::shared_ptr<detail::SourceCapacityState> wake;
  {
    std::lock_guard lock(mutex_);
    if (granted_ == &state) {
      granted_ = nullptr;
    }
    state.peak = peak;
    state.armed = true;
    state.ready.store(false, std::memory_order_release);
    wake = eligibleObserverLocked();
  }
  if (wake) {
    wake->wake.wake();
  }
  return {};
}

std::expected<SourceCredit, SourceError>
SourceBudget::reserveObserved(detail::SourceCapacityState &state, SourceBudgetUsage amount, SourceBudgetUsage peak) {
  const SourceBudgetUsage limits{options_.maxBytes, options_.maxPages, options_.maxWork, options_.maxConsumers};
  if (!fits(amount, peak)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }
  if (!fits(peak, limits)) {
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }
  std::shared_ptr<detail::SourceCapacityState> wake;
  SourceCredit credit;
  {
    std::lock_guard lock(mutex_);
    const SourceBudgetUsage available{options_.maxBytes - used_.bytes, options_.maxPages - used_.pages,
                                      options_.maxWork - used_.work, options_.maxConsumers - used_.consumers};
    if ((granted_ && granted_ != &state) || !fits(peak, available)) {
      return std::unexpected(SourceError::WOULD_BLOCK);
    }
    used_.bytes += amount.bytes;
    used_.pages += amount.pages;
    used_.work += amount.work;
    used_.consumers += amount.consumers;
    state.armed = false;
    state.ready.store(false, std::memory_order_release);
    if (granted_ == &state) {
      granted_ = nullptr;
    }
    credit = SourceCredit(shared_from_this(), amount);
    wake = eligibleObserverLocked();
  }
  if (wake) {
    wake->wake.wake();
  }
  return credit;
}

SourceArena::SourceArena(SourceCredit credit) : credit_(std::move(credit)) {
  capacity_ = credit_.usage().bytes - METADATA_BYTES;
  storage_ = std::make_unique<std::byte[]>(capacity_);
}

SourceArena::~SourceArena() = default;

std::expected<std::shared_ptr<SourceArena>, SourceError> SourceArena::create(SourceCredit credit) {
  static_assert(sizeof(SourceArena) < METADATA_BYTES / 2);

  if (credit.usage().bytes <= METADATA_BYTES) {
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }

  try {
    return std::shared_ptr<SourceArena>(new SourceArena(std::move(credit)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

std::size_t SourceArena::used() const noexcept {
  return used_;
}

std::size_t SourceArena::capacity() const noexcept {
  return capacity_;
}

void *SourceArena::do_allocate(std::size_t bytes, std::size_t alignment) {
  void *address = storage_.get() + used_;
  auto available = capacity_ - used_;

  if (!std::align(alignment, bytes, address, available)) {
    throw std::bad_alloc();
  }

  used_ = capacity_ - available + bytes;

  return address;
}

void SourceArena::do_deallocate(void *, std::size_t, std::size_t) {
}

bool SourceArena::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
  return this == &other;
}
} // namespace portals
