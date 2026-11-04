// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/source_budget.hpp>

#include <utility>

namespace portals {
namespace {
constexpr std::size_t ROOT_METADATA_BYTES = 4096;

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
  std::lock_guard lock(mutex_);
  subtract(used_, amount);
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
