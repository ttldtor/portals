// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/virtual_table.hpp>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace portals {
/** @brief Owning metadata for a typed column; validation remains part of table preparation. */
struct TableColumnOptions {
  ColumnId id;           ///< Nonzero unique value/generation pair within the table configuration.
  GraphemeText header;   ///< Owning header text using the table's width policy.
  std::int32_t width{1}; ///< Positive width in terminal cells; columns remain in configured order.
};

/** @brief An owning copyable callable that projects a borrowed immutable row.
 * @tparam T Domain record type; projection receives const T & without copying the row.
 * @tparam Projection Copy-constructible object invocable as an lvalue with const T &.
 * Reference results borrow their referent; callbacks must not retain row borrows.
 */
template <class T, class Projection>
concept ColumnProjection =
  std::is_object_v<Projection> && std::copy_constructible<Projection> && std::invocable<Projection &, const T &>;

/** @brief A copyable formatter returning exactly owning GraphemeText for a projection result.
 * @tparam T Immutable domain record type accepted by the projection.
 * @tparam Projection Object satisfying ColumnProjection; its result keeps its value category.
 * @tparam Formatter Copy-constructible object invocable as an lvalue with that result.
 * The result must be GraphemeText by value, not a reference or a convertible text type.
 */
template <class T, class Projection, class Formatter>
concept ColumnFormatter =
  ColumnProjection<T, Projection> && std::is_object_v<Formatter> && std::copy_constructible<Formatter> &&
  requires(Projection &projection, Formatter &formatter, const T &row) {
    { std::invoke(formatter, std::invoke(projection, row)) } -> std::same_as<GraphemeText>;
  };

/** @brief Compose an owning typed column from explicit projection and text formatting.
 * @tparam T Domain record type; rows are borrowed as const T & and never copied here.
 * @tparam Projection Copy-constructible projection satisfying ColumnProjection.
 * @tparam Formatter Copy-constructible formatter satisfying ColumnFormatter.
 * @param options Owning identity, header and cell width; moved into the returned column.
 * @param projection Callable owned by value; std::invoke supports member pointers and lambdas.
 * @param formatter Callable owned by value; consumes the projection's exact result category.
 * @return Existing TableColumn<T> with an owning composed callback and unchanged metadata.
 *
 * Construction may throw while copying/moving callbacks or allocating their std::function storage.
 * Neither callback is invoked during construction. References held by callbacks or returned by a
 * projection remain borrowed; callers must keep their referents alive for each invocation. A temporary
 * projection result lives through the formatter call. Callbacks must not retain row/result borrows,
 * perform I/O, mutate the tree/model, or create unbounded temporary storage. Text must use the table's
 * width policy. Table preparation retains its existing metadata/policy validation and callback exception
 * handling; direct invocation of the returned format callback propagates exceptions. Invocation follows
 * the table's UI-thread rules; this factory adds no synchronization or runtime state.
 */
template <class T, class Projection, class Formatter>
  requires ColumnFormatter<T, Projection, Formatter>
TableColumn<T> tableColumn(TableColumnOptions options, Projection projection, Formatter formatter) {
  return {options.id, std::move(options.header), options.width,
          [projection = std::move(projection), formatter = std::move(formatter)](const T &row) mutable -> GraphemeText {
            return std::invoke(formatter, std::invoke(projection, row));
          }};
}
} // namespace portals
