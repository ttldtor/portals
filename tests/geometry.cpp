// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/geometry.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>

namespace {
using portals::CellPoint;
using portals::CellRect;
using portals::CellSize;
using portals::GeometryError;

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    if (failures < 10) {
      std::cerr << message << '\n';
    }

    ++failures;
  }
}

bool oracle_contains(CellRect rect, CellPoint point) {
  const auto origin = rect.origin();
  const auto size = rect.size();
  const auto right = std::int64_t{origin.x} + size.width;
  const auto bottom = std::int64_t{origin.y} + size.height;

  return point.x >= origin.x && point.y >= origin.y && point.x < right && point.y < bottom;
}

void boundaries() {
  constexpr auto low = std::numeric_limits<std::int32_t>::min();
  constexpr auto high = std::numeric_limits<std::int32_t>::max();

  check(CellSize{-5, 7}.normalized() == CellSize{0, 7}, "negative width normalization");
  check(CellSize{7, -5}.normalized() == CellSize{7, 0}, "negative height normalization");
  check(CellSize{low, low}.normalized() == CellSize{}, "minimum extents normalization");

  const auto negative = CellRect::from({-4, -3}, {-9, 2});
  check(negative && negative->origin() == CellPoint{-4, -3} && negative->size() == CellSize{0, 2},
        "negative available width is empty at the supplied origin");
  check(negative && negative->empty() && !negative->contains(CellPoint{-4, -3}), "empty contains no point");

  const auto full = CellRect::from({low, low}, {high, high});
  check(full && full->right() == -1 && full->bottom() == -1, "widest representable size at minimum origin");
  check(full && full->contains(CellPoint{low, low}) && !full->contains(CellPoint{-1, -1}), "half-open limits");
  const auto high_empty = CellRect::from({high, high}, {0, low});
  check(high_empty && high_empty->empty() && high_empty->right() == high && high_empty->bottom() == high,
        "empty maximum origin is valid");

  const auto overflow_x = CellRect::from({high, 0}, {1, 1});
  const auto overflow_y = CellRect::from({0, high}, {1, 1});
  check(!overflow_x && overflow_x.error() == GeometryError::coordinate_overflow, "right overflow is explicit");
  check(!overflow_y && overflow_y.error() == GeometryError::coordinate_overflow, "bottom overflow is explicit");

  const auto corner = CellRect::from({high - 1, high - 1}, {1, 1});
  check(corner && corner->contains(CellPoint{high - 1, high - 1}) && !corner->contains(CellPoint{high, high}),
        "last representable cell and exclusive end");
  check(corner && !corner->translated({1, 0}), "translation checks end as well as origin");
  check(full && !full->translated({-1, 0}), "translation checks minimum origin");
  check(high_empty && !high_empty->translated({0, 1}), "empty translation still checks origin");
  check(full && full->translated({high, high}) && full->translated({high, high})->origin() == CellPoint{-1, -1},
        "wide safe translation");

  check(!portals::translated(CellPoint{high, 0}, {1, 0}), "point positive overflow");
  check(!portals::translated(CellPoint{0, low}, {0, -1}), "point negative overflow");
  check(portals::translated(CellPoint{low, high}, {high, low}) == CellPoint{-1, -1}, "point wide addition");

  if (full && corner && high_empty) {
    const auto disjoint = full->intersection(*corner);
    check(disjoint.origin() == corner->origin() && disjoint.size() == CellSize{} && disjoint.empty(),
          "far disjoint intersection has a representable deterministic empty origin");
    check(full->contains(*high_empty) && corner->contains(*high_empty), "empty rectangle is a subset everywhere");
    check(!high_empty->contains(*corner), "empty rectangle cannot contain a nonempty rectangle");
  }

  const auto left = CellRect::from({0, 0}, {2, 3});
  const auto right = CellRect::from({2, 0}, {2, 3});

  if (left && right) {
    check(left->intersection(*right).size() == CellSize{0, 3}, "touching edges have no shared interior");
    check(!left->contains(CellPoint{2, 1}), "right edge is excluded");
  }
}

void bounded_properties() {
  constexpr std::array<std::int32_t, 3> origins{-2, 0, 2};
  constexpr std::array<std::int32_t, 4> extents{-2, 0, 1, 4};
  constexpr CellPoint offset{3, -2};
  std::vector<CellRect> rectangles;

  for (const auto x : origins) {
    for (const auto y : origins) {
      for (const auto width : extents) {
        for (const auto height : extents) {
          const auto rect = CellRect::from({x, y}, {width, height});
          check(rect.has_value(), "bounded rectangle creation");

          if (rect) {
            rectangles.push_back(*rect);
          }
        }
      }
    }
  }

  for (const auto a : rectangles) {
    check(a.intersection(a) == a, "intersection idempotence including degenerate axes");
    check(a.contains(a), "containment reflexivity");
    const auto moved_a = a.translated(offset);
    check(moved_a && moved_a->translated({-offset.x, -offset.y}) == a, "translation inverse");

    for (const auto b : rectangles) {
      const auto intersection = a.intersection(b);
      check(intersection == b.intersection(a), "intersection commutativity");
      check(a.contains(intersection) && b.contains(intersection), "intersection subset identity");
      const auto moved_b = b.translated(offset);
      check(moved_a && moved_b && intersection.translated(offset) == moved_a->intersection(*moved_b),
            "intersection commutes with translation");
      check(moved_a && moved_b && a.contains(b) == moved_a->contains(*moved_b), "containment translation identity");
      bool b_is_subset = true;

      for (std::int32_t x = -3; x <= 6; ++x) {
        for (std::int32_t y = -3; y <= 6; ++y) {
          const CellPoint point{x, y};
          const bool in_a = oracle_contains(a, point);
          const bool in_b = oracle_contains(b, point);
          check(a.contains(point) == in_a, "point containment agrees with independent interval oracle");
          check(intersection.contains(point) == (in_a && in_b), "intersection agrees with cell-set oracle");
          check(moved_a && moved_a->contains({x + offset.x, y + offset.y}) == in_a, "point translation identity");
          b_is_subset = b_is_subset && (!in_b || in_a);
        }
      }

      check(a.contains(b) == b_is_subset, "rectangle containment agrees with bounded cell-set oracle");
    }
  }
}
void extreme_properties() {
  constexpr auto low = std::numeric_limits<std::int32_t>::min();
  constexpr auto high = std::numeric_limits<std::int32_t>::max();
  constexpr std::array<std::int32_t, 5> values{low, -1, 0, 1, high};
  std::vector<CellRect> rectangles;

  for (const auto x : values) {
    for (const auto y : values) {
      for (const auto width : values) {
        for (const auto height : values) {
          const auto expected_width = width < 0 ? 0 : width;
          const auto expected_height = height < 0 ? 0 : height;
          const auto right = std::int64_t{x} + expected_width;
          const auto bottom = std::int64_t{y} + expected_height;
          const auto rect = CellRect::from({x, y}, {width, height});
          const bool representable = right <= high && bottom <= high;
          check(rect.has_value() == representable, "extreme factory agrees with independent wide arithmetic");

          if (!rect) {
            check(rect.error() == GeometryError::coordinate_overflow, "extreme factory reports overflow");

            continue;
          }

          rectangles.push_back(*rect);
          check(rect->origin() == CellPoint{x, y} && rect->size() == CellSize{expected_width, expected_height},
                "extreme factory preserves origin and normalizes independently");

          for (const auto dx : values) {
            for (const auto dy : values) {
              const auto moved_x = std::int64_t{x} + dx;
              const auto moved_y = std::int64_t{y} + dy;
              const bool can_move = moved_x >= low && moved_x <= high && moved_y >= low && moved_y <= high &&
                                    right + dx >= low && right + dx <= high && bottom + dy >= low &&
                                    bottom + dy <= high;
              const auto moved = rect->translated({dx, dy});
              check(moved.has_value() == can_move, "extreme translation checks origins and ends");

              if (moved) {
                check(moved->origin() ==
                          CellPoint{static_cast<std::int32_t>(moved_x), static_cast<std::int32_t>(moved_y)} &&
                        moved->size() == rect->size(),
                      "successful extreme translation preserves extents");
              } else {
                check(moved.error() == GeometryError::coordinate_overflow, "extreme translation reports overflow");
              }
            }
          }
        }
      }
    }
  }

  for (const auto a : rectangles) {
    check(a.intersection(a) == a, "extreme intersection idempotence");

    for (const auto b : rectangles) {
      const auto intersection = a.intersection(b);
      check(intersection == b.intersection(a), "extreme intersection commutativity");
      check(a.contains(intersection) && b.contains(intersection), "extreme intersection subset identity");

      for (const auto x : values) {
        for (const auto y : values) {
          const CellPoint point{x, y};
          check(intersection.contains(point) == (oracle_contains(a, point) && oracle_contains(b, point)),
                "extreme intersection agrees with independent interval oracle");
        }
      }
    }
  }
}

} // namespace

static_assert(!std::is_convertible_v<CellPoint, CellSize>);
static_assert(!std::is_convertible_v<CellSize, CellPoint>);
static_assert(!std::is_convertible_v<int, CellRect>);
static_assert(std::is_trivially_copyable_v<CellRect>);

int main() {
  boundaries();
  bounded_properties();
  extreme_properties();

  return failures == 0 ? 0 : 1;
}
