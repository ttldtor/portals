// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/find_bar.hpp>
#include <portals/managed_diagnostics.hpp>
#include <portals/managed_model.hpp>

#include <memory_resource>

namespace portals::detail {
/// @cond INTERNAL
bool validFindSearchOptions(const FindSearchOptions &options) noexcept;

struct ProjectionLimit {};

struct ProjectionMemory : std::pmr::memory_resource {
  virtual void chargeExternal(std::size_t bytes) = 0;
  virtual void releaseExternal(std::size_t bytes) noexcept = 0;
};

struct FindRowView {
  RowId id;
  std::uint64_t ordinal{};
  std::string_view text;
  std::shared_ptr<const std::pmr::string> owner;
};

// Only this immutable worker object crosses admission. Its UI model observer remains in FindModelSource.
struct FindWorkerSource {
  virtual ~FindWorkerSource() = default;
  virtual std::expected<void, FindSearchError> read(RowRange range, std::vector<FindRowView> &rows,
                                                    ProjectionMemory *memory = nullptr) const = 0;
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

  std::expected<void, FindSearchError> read(RowRange range, std::vector<FindRowView> &rows,
                                            ProjectionMemory *memory = nullptr) const override {
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

      if constexpr (std::same_as<std::invoke_result_t<const Projection &, const T &>, std::string>) {
        if (!memory) {
          return std::unexpected(FindSearchError::INVALID_ARGUMENT);
        }
        try {
          auto projected = std::invoke(*projection_, row.value);

          struct ExternalCharge {
            ProjectionMemory &memory;
            std::size_t bytes;

            ~ExternalCharge() {
              memory.releaseExternal(bytes);
            }
          };

          memory->chargeExternal(projected.capacity() + 1);
          ExternalCharge charge{*memory, projected.capacity() + 1};
          auto owner = std::allocate_shared<const std::pmr::string>(
            std::pmr::polymorphic_allocator<std::pmr::string>(memory), std::string_view(projected));
          const auto text = std::string_view(*owner);
          rows.push_back({row.id, range.first + rows.size(), text, std::move(owner)});
        } catch (const ProjectionLimit &) {
          return std::unexpected(FindSearchError::LIMIT_EXCEEDED);
        }
      } else {
        rows.push_back({row.id, range.first + rows.size(), std::invoke(*projection_, row.value), {}});
      }
    }

    return {};
  }

private:
  std::shared_ptr<const ModelSnapshot<T>> snapshot_;
  std::shared_ptr<const Projection> projection_;
};

/// @endcond
} // namespace portals::detail
