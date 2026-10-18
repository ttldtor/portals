// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model.hpp>

namespace portals {
/** @brief Bound the row/index storage of an immutable in-memory collection; domain T payload is source-owned. */
struct CollectionLimits {
  std::size_t maxRows{4096}; ///< Positive whole-collection row cap; no silent truncation.
};

/** @brief Immutable owning rows with explicit provider identity and bounded identity lookup.
 * @tparam T Copy-constructible equality-comparable transitively owning domain value; published aliases stay immutable.
 * Copies share one const payload. Existing Model providers do not acquire these copying/equality requirements.
 */
template <class T>
  requires std::copy_constructible<T> && std::equality_comparable<T>
class RowCollection {
public:
  /** @brief Release shared row ownership; transferring this value retains a valid shared-owner source. */
  ~RowCollection() = default;

  /** @brief Copy a checked whole collection into private immutable storage.
   * @param rows Values borrowed only during construction; all RowId components must be nonzero and pairs unique.
   * @param limits Positive count cap; domain payload byte bounds remain the source's responsibility.
   * @return Independent row storage or INVALID_ARGUMENT, NODE_LIMIT, NO_MEMORY or CALLBACK_FAILED.
   */
  static std::expected<RowCollection, TreeError> from(std::span<const ModelRow<T>> rows, CollectionLimits limits = {}) {
    if (!limits.maxRows) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if (rows.size() > limits.maxRows) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    try {
      auto data = std::make_shared<Data>();

      data->rows.reserve(rows.size());
      data->index.reserve(rows.size());

      for (const auto &row : rows) {
        if (!row.id.value || !row.id.generation) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }

        data->index.emplace_back(row.id, data->rows.size());
        data->rows.emplace_back(row);
      }

      std::ranges::sort(data->index, [](const auto &left, const auto &right) {
        return less(left.first, right.first);
      });

      for (std::size_t i = 1; i < data->index.size(); ++i) {
        if (data->index[i - 1].first == data->index[i].first) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
      }

      return RowCollection(std::move(data));
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::NODE_LIMIT);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @return Borrowed immutable rows; retain this value or its snapshot throughout use. */
  std::span<const ModelRow<T>> rows() const noexcept {
    return data_->rows;
  }

  /** @param id Full provider identity to locate.
   * @return FOUND with the original ordinal, otherwise NOT_FOUND; no allocation or full row scan.
   */
  RowLookup locate(RowId id) const noexcept {
    const auto found = std::ranges::lower_bound(data_->index, id, less, [](const auto &entry) {
      return entry.first;
    });

    return found != data_->index.end() && found->first == id ? RowLookup{RowLookupState::FOUND, found->second}
                                                             : RowLookup{RowLookupState::NOT_FOUND};
  }

  /** @param other Owning rows to compare.
   * @return Equal ordered IDs and values; T comparison exceptions propagate to the receiving checked operation.
   */
  bool operator==(const RowCollection &other) const {
    if (data_ == other.data_) {
      return true;
    }

    return std::ranges::equal(data_->rows, other.data_->rows, [](const auto &left, const auto &right) {
      return left.id == right.id && left.value == right.value;
    });
  }

private:
  struct Data {
    std::vector<ModelRow<T>> rows = std::vector<ModelRow<T>>(std::initializer_list<ModelRow<T>>{});
    std::vector<std::pair<RowId, std::uint64_t>> index = std::vector<std::pair<RowId, std::uint64_t>>(0);
  };

  static bool less(RowId left, RowId right) noexcept {
    return left.value < right.value || (left.value == right.value && left.generation < right.generation);
  }

  explicit RowCollection(std::shared_ptr<const Data> data) : data_(std::move(data)) {
  }

  std::shared_ptr<const Data> data_;
};

/** @brief Policies for a scene-owned in-memory model adapter. */
struct CollectionModelOptions {
  ModelOptions model{};      ///< Existing range/consumer/work budgets.
  std::size_t maxRows{4096}; ///< Positive full collection/index cap.
};
} // namespace portals
