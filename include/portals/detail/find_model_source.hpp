// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/find_bar.hpp>
#include <portals/managed_diagnostics.hpp>
#include <portals/managed_model.hpp>

namespace portals::detail {
/// @cond INTERNAL
bool validFindSearchOptions(const FindSearchOptions &options) noexcept;

struct FindRowView {
  RowId id;
  std::uint64_t ordinal{};
  std::string_view text;
};

// Only this immutable worker object crosses admission. Its UI model observer remains in FindModelSource.
struct FindWorkerSource {
  virtual ~FindWorkerSource() = default;
  virtual std::expected<void, FindSearchError> read(RowRange range, std::vector<FindRowView> &rows) const = 0;
  std::shared_ptr<const void> pin;
  std::uint64_t revision{};
  std::uint64_t rowCount{};
};

struct FindModelSource {
  virtual ~FindModelSource() = default;
  virtual std::expected<void, ManagedError> attach(ManagedScene &scene) = 0;
  virtual std::expected<std::shared_ptr<const FindWorkerSource>, TreeError> capture(ManagedScene &scene) = 0;
};

template <class T, class Projection> struct FindWorkerSourceOf final : FindWorkerSource {
  FindWorkerSourceOf(std::shared_ptr<const ModelSnapshot<T>> snapshot, std::shared_ptr<const Projection> projection)
      : snapshot_(std::move(snapshot)), projection_(std::move(projection)) {
    pin = snapshot_;
    revision = snapshot_->revision();
    rowCount = snapshot_->rowCount();
  }

  std::expected<void, FindSearchError> read(RowRange range, std::vector<FindRowView> &rows) const override {
    auto input = ModelRange<T>::read(snapshot_, range, range.count);

    if (!input) {
      return std::unexpected(input.error() == TreeError::NO_MEMORY         ? FindSearchError::NO_MEMORY
                             : input.error() == TreeError::CALLBACK_FAILED ? FindSearchError::CALLBACK_FAILED
                                                                           : FindSearchError::INVALID_ARGUMENT);
    }

    if (input->state() != RangeState::READY) {
      return std::unexpected(FindSearchError::NOT_READY);
    }

    rows.clear();

    for (const auto &row : input->rows()) {
      if (!row.id.value || !row.id.generation) {
        return std::unexpected(FindSearchError::INVALID_ARGUMENT);
      }

      rows.push_back({row.id, range.first + rows.size(), std::invoke(*projection_, row.value)});
    }

    return {};
  }

private:
  std::shared_ptr<const ModelSnapshot<T>> snapshot_;
  std::shared_ptr<const Projection> projection_;
};

/// @endcond
} // namespace portals::detail
