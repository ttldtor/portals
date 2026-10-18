// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model.hpp>

#include <iterator>

namespace portals {
/** @brief A view has not published ready physical rows; no case denotes successful emptiness. */
enum class VisibleModelReadState {
  NOT_PREPARED, ///< No current preparation, including after scroll or configuration changes.
  PENDING,      ///< The provider has not supplied the prepared interval.
  FAILED        ///< The provider rejected the prepared interval.
};

/** @brief Owning preparation/provider failure, independent of the table and provider lifetime. */
struct VisibleModelReadError {
  VisibleModelReadState state; ///< Explicit preparation or provider failure.
  std::string message;         ///< Owning provider description for FAILED; otherwise empty.
};

template <class T> class VirtualTable;

/** @brief Copyable immutable ready values for one physically visible table interval.
 * @tparam T Immutable domain value; values are retained through the existing ModelRange pin, never copied.
 * Const iteration and copies of distinct handles may run on any thread after synchronized transfer.
 * Borrowed values/iterators require a retained snapshot owner. Final release follows ModelSnapshot's
 * payload destruction contract. Scrolling, publication, table removal and scene destruction do not change it.
 */
template <class T> class VisibleModelSnapshot {
public:
  /** @brief Standard forward iterator projecting each immutable row to its domain value. */
  class Iterator {
  public:
    using value_type = T;                                ///< Standard iterator value type.
    using difference_type = std::ptrdiff_t;              ///< Standard signed distance type.
    using iterator_concept = std::forward_iterator_tag;  ///< Multipass immutable traversal.
    using iterator_category = std::forward_iterator_tag; ///< Legacy algorithm category.

    Iterator() = default; ///< Empty iterator; do not dereference it.

    /** @return Immutable value, borrowed from the retained snapshot. */
    const T &operator*() const noexcept {
      return row_->value;
    }

    /** @return Immutable value address, borrowed from the retained snapshot. */
    const T *operator->() const noexcept {
      return std::addressof(row_->value);
    }

    /** @return This iterator after advancing one row. */
    Iterator &operator++() noexcept {
      ++row_;

      return *this;
    }

    /** @return The previous iterator, before advancing one row. */
    Iterator operator++(int) noexcept {
      auto previous = *this;

      ++*this;

      return previous;
    }

    /** @param other Iterator into the same snapshot interval.
     * @return Whether both iterators identify the same row or end.
     */
    bool operator==(const Iterator &other) const noexcept = default;

  private:
    friend class VisibleModelSnapshot;

    explicit Iterator(const ModelRow<T> *row) : row_(row) {
    }

    const ModelRow<T> *row_{};
  };

  VisibleModelSnapshot() = default;                             ///< Successful empty value with no provider storage.
  VisibleModelSnapshot(const VisibleModelSnapshot &) = default; ///< Share immutable storage without copying values.

  /** @param other Owner transferred in constant time; the source becomes an empty range. */
  VisibleModelSnapshot(VisibleModelSnapshot &&other) noexcept
      : owner_(std::move(other.owner_)), rows_(std::exchange(other.rows_, {})),
        interval_(std::exchange(other.interval_, {})) {
  }

  /** @param other Replacement value sharing or transferring immutable ownership.
   * @return This snapshot after all metadata is replaced, before old storage is released.
   */
  VisibleModelSnapshot &operator=(VisibleModelSnapshot other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(rows_, other.rows_);
    std::swap(interval_, other.interval_);

    return *this;
  }

  /** @return First immutable domain value; retain this snapshot while using iterators. */
  Iterator begin() const noexcept {
    return Iterator(rows_.data());
  }

  /** @return Sentinel past the last domain value. */
  Iterator end() const noexcept {
    return Iterator(rows_.empty() ? rows_.data() : rows_.data() + rows_.size());
  }

  /** @return Number of physically visible ready rows, excluding header and overscan. */
  std::size_t size() const noexcept {
    return rows_.size();
  }

  /** @return Whether no body row intersects the final clip. */
  bool empty() const noexcept {
    return rows_.empty();
  }

  /** @return Borrowed immutable values with their stable RowIds; retain this snapshot. */
  std::span<const ModelRow<T>> rows() const noexcept {
    return rows_;
  }

  /** @return Logical interval represented by rows(), independent of overscan. */
  RowRange range() const noexcept {
    return interval_;
  }

  /** @return Provider revision of the prepared pin; zero for an unowned hidden interval. */
  std::uint64_t dataRevision() const noexcept {
    return owner_.dataRevision();
  }

private:
  friend class VirtualTable<T>;

  VisibleModelSnapshot(ModelRange<T> owner, std::span<const ModelRow<T>> rows, RowRange interval)
      : owner_(std::move(owner)), rows_(rows), interval_(interval) {
  }

  ModelRange<T> owner_;
  std::span<const ModelRow<T>> rows_;
  RowRange interval_;
};
} // namespace portals
