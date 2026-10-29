// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <portals/scroll_geometry.hpp>
#include <portals/scroll_options.hpp>

namespace {
using namespace portals;
constexpr auto MAXIMUM = std::numeric_limits<std::uint64_t>::max();
constexpr auto MAXIMUM_CELL = std::numeric_limits<std::int32_t>::max();
int failures{};

static_assert(scrollRange({MAXIMUM, 1}).maximum == MAXIMUM - 1);
static_assert(scrollRange({MAXIMUM, 1}).count == MAXIMUM);
static_assert(scrollRange({MAXIMUM, 0}).maximum == 0);
static_assert(scrollRange({1, MAXIMUM}).count == 1);
static_assert(scrollPageStep(0, MAXIMUM) == 0);
static_assert(scrollPageStep(1, MAXIMUM) == 1);
static_assert(scrollPageStep(MAXIMUM, 1) == MAXIMUM - 1);
static_assert(scrollThumb({MAXIMUM, 1}, MAXIMUM, 11)->start == 10);
static_assert(scrollThumb({0, 0}, MAXIMUM, 2)->length == 2);
static_assert(scrollThumb({100, 10}, 90, 10)->start == 9);
static_assert(*scrollOffsetForThumb(1, 2, 5) == 2);
static_assert(*scrollOffsetForThumb(1, 2, MAXIMUM) == MAXIMUM / 2);
static_assert(*scrollOffsetForThumb(MAXIMUM_CELL, MAXIMUM_CELL, MAXIMUM) == MAXIMUM);
static_assert(noexcept(detail::preciseScrollOffsetForThumb(1, 2, MAXIMUM)));
static_assert(detail::preciseScrollOffsetForThumb(1, 2, MAXIMUM).raster == MAXIMUM / 2);
static_assert(detail::preciseScrollOffsetForThumb(1, 2, MAXIMUM).fraction == 0.5);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM, MAXIMUM, MAXIMUM).raster == MAXIMUM);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM, MAXIMUM, MAXIMUM).fraction == 0);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM - 1, MAXIMUM, MAXIMUM).raster == MAXIMUM - 1);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM - 1, MAXIMUM, 1).raster == 0);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM - 1, MAXIMUM, 1).fraction == 0x1.fffffffffffffp-1);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM, MAXIMUM - 1, MAXIMUM).raster == MAXIMUM);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM, 0, MAXIMUM).raster == 0);
static_assert(detail::preciseScrollOffsetForThumb(MAXIMUM, MAXIMUM, 0).fraction == 0);
static_assert(solveScrollLayout({10, 10}, {10, 11})->horizontalBar);
static_assert(solveScrollLayout({10, 10}, {10, 11})->viewport.width == 9);
static_assert(solveScrollLayout({10, 10}, {10, 10})->viewport.height == 10);
static_assert(solveScrollLayout({2, 2}, {10, 10},
                                {ScrollBarPolicy::AUTO, ScrollBarPolicy::AUTO, ScrollPlacement::ON_FRAME, true})
                ->vertical.length == 0);

void check(bool value, const char *message) {
  if (!value) {
    if (failures < 12) {
      std::cerr << message << '\n';
    }

    ++failures;
  }
}

void fractionalThumbs() {
  for (std::uint64_t maximum = 1; maximum <= 31; ++maximum) {
    for (std::int32_t track = 0; track <= 17; ++track) {
      std::int64_t previous{};
      for (std::uint64_t eighth = 0; eighth <= maximum * 8; ++eighth) {
        const ScrollOffset offset{eighth / 8, static_cast<double>(eighth % 8) / 8};
        const auto value = scrollFractionalThumb({maximum + 1, 1}, offset, track);
        check(value.has_value(), "valid fractional geometry is admitted");
        if (!value) {
          continue;
        }
        const auto length = track == 0 ? 0 : std::max<std::int32_t>(1, track / static_cast<std::int32_t>(maximum + 1));
        const auto travel = track - length;
        const auto expected = static_cast<std::int64_t>(eighth * travel / maximum);
        check(value->startEighths == expected && value->startEighths >= previous,
              "independent small rational oracle preserves every eighth and monotonicity");
        check(value->lengthEighths == length * 8 && value->travelEighths == travel * 8,
              "fractional placement preserves integral minimum length and travel");
        previous = value->startEighths;
      }
    }
  }
  const auto nearEnd = scrollFractionalThumb({MAXIMUM, 1}, {MAXIMUM - 2, .5}, MAXIMUM_CELL);
  check(nearEnd && nearEnd->startEighths == (static_cast<std::int64_t>(MAXIMUM_CELL) - 1) * 8 - 1,
        "wide raster and fractional remainder reach the last eighth without a double round trip");
  const auto endpoint = scrollFractionalThumb({MAXIMUM, 1}, {MAXIMUM, .5}, MAXIMUM_CELL);
  check(endpoint && endpoint->startEighths == endpoint->travelEighths, "clamped uint64 endpoint is exact");
  check(!scrollFractionalThumb({10, 1}, {0, std::numeric_limits<double>::quiet_NaN()}, 10) &&
          !scrollFractionalThumb({10, 1}, {0, 1}, 10) && !scrollFractionalThumb({10, 1}, {}, -1),
        "fractional geometry rejects invalid offsets and tracks");

  struct Golden {
    std::uint64_t maximum;
    std::uint64_t raster;
    double fraction;
    std::int32_t track;
    std::int64_t expected;
  };

  // Independent Python Fraction(binary64) arithmetic supplies these exact rational results.
  constexpr std::array GOLDENS{
    Golden{3ULL, 0ULL, 0x1.999999999999ap-4, 17, 3LL},
    Golden{3ULL, 0ULL, 0x1.fffffffffffffp-4, 17, 4LL},
    Golden{3ULL, 0ULL, 0x1.0000000000000p-3, 17, 4LL},
    Golden{3ULL, 0ULL, 0x1.0000000000001p-3, 17, 4LL},
    Golden{31ULL, 7ULL, 0x1.3333333333333p-2, 19, 33LL},
    Golden{9007199254740999ULL, 9007199254740997ULL, 0x1.8000000000000p-1, MAXIMUM_CELL, 17179869167LL},
    Golden{18446744073709551614ULL, 18446744073709551613ULL, 0x1.fffffffffffffp-1, MAXIMUM_CELL, 17179869167LL},
    Golden{1ULL, 0ULL, 0x1.fffffffffffffp-1, MAXIMUM_CELL, 8589934591LL},
    Golden{1ULL, 0ULL, 0x0.0000000000001p-1022, MAXIMUM_CELL, 0LL}};
  for (const auto &golden : GOLDENS) {
    const auto value = scrollFractionalThumb({golden.maximum + 1, 1}, {golden.raster, golden.fraction}, golden.track);
    check(value && value->startEighths == golden.expected,
          "binary64 rational oracle covers carry, subnormal, near-one and wide indices");
  }
}

template <class T> T take(std::expected<T, TreeError> result) {
  if (!result) {
    std::cerr << "unexpected fixture error " << static_cast<int>(result.error()) << '\n';
    std::abort();
  }

  return *result;
}

// An independent oracle decomposes maximum by a small denominator before multiplying.
// Its remainder product is at most INT32_MAX^2 and fits uint64, even for UINT64_MAX maximum.
std::uint64_t inverseOracle(std::uint64_t position, std::uint64_t travel, std::uint64_t maximum) {
  const auto remainderProduct = (maximum % travel) * position;
  const auto remainder = remainderProduct % travel;

  return (maximum / travel) * position + remainderProduct / travel + (remainder > travel - remainder ? 1 : 0);
}

std::uint64_t forwardOracle(std::uint64_t offset, std::uint64_t travel, std::uint64_t maximum) {
  std::uint64_t result{};

  for (std::uint64_t candidate = 1; candidate <= travel; ++candidate) {
    const auto product = (maximum % travel) * candidate;
    const auto threshold = (maximum / travel) * candidate + product / travel + (product % travel != 0 ? 1 : 0);

    if (offset >= threshold) {
      result = candidate;
    }
  }

  return result;
}

void preciseThumbOffsets() {
  struct Golden {
    std::uint64_t position;
    std::uint64_t travel;
    std::uint64_t maximum;
    std::uint64_t raster;
    double fraction;
  };

  // Python arbitrary-precision divmod(position * maximum, travel), with Fraction(remainder, travel).
  // Fractions are rounded to double independently; allow two epsilons for operand conversions.
  constexpr std::array GOLDEN{
    Golden{9223372036854775931ULL, 9223372036854776795ULL, MAXIMUM, 18446744073709549887ULL, 0x1.a099fffffffffp-43},
    Golden{MAXIMUM - 1, MAXIMUM, MAXIMUM - 2, 18446744073709551612ULL, 0x1p-63},
    Golden{6148914691236517205ULL, MAXIMUM - 17, 9223372036854775809ULL, 3074457345618258605ULL, 0x1.aaaaaaaaaaaabp-1},
    Golden{9007199254740995ULL, 18014398509481991ULL, MAXIMUM, 9223372036854775295ULL, 0x1.0000000000700p-1},
    Golden{12345678901234567890ULL, 17345678901234567891ULL, 16123456789012345678ULL, 11475769926820757233ULL,
           0x1.11f487ecd1e1bp-1},
  };

  for (const auto &golden : GOLDEN) {
    const auto result = detail::preciseScrollOffsetForThumb(golden.position, golden.travel, golden.maximum);
    const auto tolerance = 2 * std::numeric_limits<double>::epsilon() * golden.fraction;

    check(result.raster == golden.raster, "precise inverse preserves the independent wide-product quotient");
    check(result.fraction >= golden.fraction - tolerance && result.fraction <= golden.fraction + tolerance,
          "precise inverse agrees with the independent rational remainder");
    check(validScrollOffset(result), "precise inverse remainder stays normalized");
  }

  for (const auto maximum : std::array<std::uint64_t, 3>{1, (1ULL << 53) + 1, MAXIMUM}) {
    for (const auto travel : std::array<std::uint64_t, 3>{1, (1ULL << 63) + 1, MAXIMUM}) {
      for (const auto position : {std::uint64_t{}, travel, MAXIMUM}) {
        const auto result = detail::preciseScrollOffsetForThumb(position, travel, maximum);

        check(result.raster == (position == 0 ? 0 : maximum) && result.fraction == 0,
              "precise inverse endpoints and clamping are exact");
      }

      const auto zeroMaximum = detail::preciseScrollOffsetForThumb(MAXIMUM, travel, 0);
      const auto zeroTravel = detail::preciseScrollOffsetForThumb(MAXIMUM, 0, maximum);

      check(zeroMaximum.raster == 0 && zeroMaximum.fraction == 0 && zeroTravel.raster == 0 && zeroTravel.fraction == 0,
            "precise inverse zero ranges never divide");
    }
  }

  const auto rounded = detail::preciseScrollOffsetForThumb(MAXIMUM - 1, MAXIMUM, 1);
  const auto wideIdentity = detail::preciseScrollOffsetForThumb(MAXIMUM - 1, MAXIMUM, MAXIMUM);

  check(rounded.raster == 0 && rounded.fraction == 0x1.fffffffffffffp-1,
        "rounded unit remainder is clamped without carrying into the exact raster");
  check(wideIdentity.raster == MAXIMUM - 1 && wideIdentity.fraction == 0,
        "wide position and travel retain an exact integral result above 2^53");
}

void thumbs() {
  for (std::uint64_t extent = 0; extent <= 40; ++extent) {
    for (std::uint64_t viewport = 0; viewport <= 40; ++viewport) {
      const auto range = scrollRange({extent, viewport});

      for (std::int32_t track = 0; track <= 20; ++track) {
        for (const auto offset : {std::uint64_t{}, range.maximum / 2, range.maximum, range.maximum + 3}) {
          const auto result = take(scrollThumb({extent, viewport}, offset, track));
          const auto length = track == 0 ? 0
                              : range.maximum == 0
                                ? track
                                : std::max<std::int32_t>(1, static_cast<std::int32_t>(track * viewport / extent));
          const auto travel = track - length;
          const auto start = range.maximum == 0 ? 0 : std::min(offset, range.maximum) * travel / range.maximum;

          check(result.length == length && result.travel == travel && result.start >= 0 &&
                  static_cast<std::uint64_t>(result.start) == start,
                "small-range thumb differs from independent exact product oracle");
        }
      }
    }
  }

  const std::array<std::uint64_t, 7> maxima{1,           2,      (1ULL << 53) - 1, (1ULL << 53) + 1, (1ULL << 63),
                                            MAXIMUM - 1, MAXIMUM};

  for (const auto maximum : maxima) {
    for (const auto travel : {1, 2, 3, 7, 31, MAXIMUM_CELL}) {
      const std::array<std::int64_t, 8> points{
        std::numeric_limits<std::int64_t>::min(), -1, 0, 1, travel / 2, travel - 1, travel,
        std::numeric_limits<std::int64_t>::max()};

      for (const auto point : points) {
        const auto clamped = static_cast<std::uint64_t>(std::clamp<std::int64_t>(point, 0, travel));
        const auto result = take(scrollOffsetForThumb(point, travel, maximum));

        check(result == inverseOracle(clamped, travel, maximum), "u64 inverse differs from quotient oracle");
      }
    }
  }

  for (const auto extent : maxima) {
    const auto maximum = scrollRange({extent, 1}).maximum;

    if (maximum == 0) {
      continue;
    }

    for (const auto track : {2, 3, 7, 31}) {
      for (const auto offset : {std::uint64_t{}, maximum / 3, maximum / 2, maximum - 1, maximum}) {
        const auto result = take(scrollThumb({extent, 1}, offset, track));

        check(static_cast<std::uint64_t>(result.start) == forwardOracle(offset, result.travel, maximum),
              "u64 forward differs from independent threshold oracle");
      }
    }
  }

  check(take(scrollThumb({100, 1}, 20, 2, 99)).length == 2, "minimum thumb caps at track");
  check(!scrollThumb({}, 0, -1) && !scrollThumb({}, 0, 1, 0), "invalid track settings rejected");
  check(!scrollOffsetForThumb(0, -1, MAXIMUM), "negative inverse travel rejected");
  check(take(scrollOffsetForThumb(100, 0, MAXIMUM)) == 0, "zero travel never divides");
}

bool trackContains(ScrollTrack track, bool horizontal, int x, int y) {
  return horizontal ? y == track.origin.y && x >= track.origin.x && x - track.origin.x < track.length
                    : x == track.origin.x && y >= track.origin.y && y - track.origin.y < track.length;
}

void placement() {
  constexpr std::array POLICIES{ScrollBarPolicy::NEVER, ScrollBarPolicy::AUTO, ScrollBarPolicy::ALWAYS};
  constexpr std::array PLACEMENTS{ScrollPlacement::RESERVED, ScrollPlacement::OVERLAY, ScrollPlacement::ON_FRAME};

  for (int width = 0; width <= 6; ++width) {
    for (int height = 0; height <= 6; ++height) {
      for (std::uint64_t extent = 0; extent <= 8; ++extent) {
        for (const auto horizontal : POLICIES) {
          for (const auto vertical : POLICIES) {
            for (const auto mode : PLACEMENTS) {
              for (const auto frame : {false, true}) {
                if (mode == ScrollPlacement::ON_FRAME && !frame) {
                  continue;
                }

                const auto result =
                  take(solveScrollLayout({width, height}, {extent, 8 - extent}, {horizontal, vertical, mode, frame}));
                const auto innerWidth = frame ? std::max(width - 2, 0) : width;
                const auto innerHeight = frame ? std::max(height - 2, 0) : height;
                int least = -1;
                int leastCount = 3;

                // Enumerate every fixed point; select the one with the fewest bars.
                for (int mask = 0; mask < 4; ++mask) {
                  const bool h = (mask & 1) != 0;
                  const bool v = (mask & 2) != 0;
                  const auto w = mode == ScrollPlacement::RESERVED ? std::max(innerWidth - (v ? 1 : 0), 0) : innerWidth;
                  const auto hgt =
                    mode == ScrollPlacement::RESERVED ? std::max(innerHeight - (h ? 1 : 0), 0) : innerHeight;
                  const bool expectedH = horizontal == ScrollBarPolicy::ALWAYS ||
                                         (horizontal == ScrollBarPolicy::AUTO && extent > static_cast<unsigned>(w));
                  const bool expectedV = vertical == ScrollBarPolicy::ALWAYS ||
                                         (vertical == ScrollBarPolicy::AUTO && 8 - extent > static_cast<unsigned>(hgt));
                  const int count = static_cast<int>(h) + static_cast<int>(v);

                  if (h == expectedH && v == expectedV && count < leastCount) {
                    least = mask;
                    leastCount = count;
                  }
                }

                check(least >= 0 && result.horizontalBar == ((least & 1) != 0) &&
                        result.verticalBar == ((least & 2) != 0),
                      "bar solve is not the least fixed point");
                check(result.viewport.width >= 0 && result.viewport.height >= 0 && result.childClipSize.width >= 0 &&
                        result.childClipSize.height >= 0,
                      "tiny viewport/clip is nonnegative");
                check(result.viewportOrigin.x + result.viewport.width <= width &&
                        result.viewportOrigin.y + result.viewport.height <= height,
                      "viewport stays in allocation");

                for (int y = 0; y < height; ++y) {
                  for (int x = 0; x < width; ++x) {
                    const bool h = trackContains(result.horizontal, true, x, y);
                    const bool v = trackContains(result.vertical, false, x, y);
                    const bool content = x >= result.viewportOrigin.x && y >= result.viewportOrigin.y &&
                                         x - result.viewportOrigin.x < result.childClipSize.width &&
                                         y - result.viewportOrigin.y < result.childClipSize.height;

                    check(!(h && v) && !(content && (h || v)), "track/content ownership is disjoint");
                    check(!result.hasCorner || x != result.corner.x || y != result.corner.y || (!h && !v && !content),
                          "corner has one decorative owner");
                  }
                }

                check(result.horizontal.length == 0 ||
                        (result.horizontal.origin.x >= 0 && result.horizontal.origin.y < height &&
                         result.horizontal.origin.x + result.horizontal.length <= width),
                      "horizontal track fits");
                check(result.vertical.length == 0 ||
                        (result.vertical.origin.y >= 0 && result.vertical.origin.x < width &&
                         result.vertical.origin.y + result.vertical.length <= height),
                      "vertical track fits");
              }
            }
          }
        }
      }
    }
  }

  const auto negative = take(solveScrollLayout({std::numeric_limits<std::int32_t>::min(), -1}, {MAXIMUM, MAXIMUM}));

  check(negative.viewport.width == 0 && negative.viewport.height == 0, "negative allocation normalizes safely");
  check(!solveScrollLayout({}, {}, {ScrollBarPolicy::AUTO, ScrollBarPolicy::AUTO, ScrollPlacement::ON_FRAME}),
        "frame placement requires a frame");
  check(!solveScrollLayout({}, {}, {static_cast<ScrollBarPolicy>(99)}), "unknown policy rejected");
  check(!solveScrollLayout({}, {}, {ScrollBarPolicy::AUTO, ScrollBarPolicy::AUTO, static_cast<ScrollPlacement>(99)}),
        "unknown placement rejected");

  const auto huge =
    take(solveScrollLayout({MAXIMUM_CELL, MAXIMUM_CELL}, {MAXIMUM, MAXIMUM},
                           {ScrollBarPolicy::AUTO, ScrollBarPolicy::AUTO, ScrollPlacement::ON_FRAME, true}));

  check(huge.vertical.origin.x == MAXIMUM_CELL - 1 && huge.horizontal.length == MAXIMUM_CELL - 2,
        "maximum allocation frame arithmetic stays representable");
}

void partialFrames() {
  constexpr std::array POLICIES{ScrollBarPolicy::NEVER, ScrollBarPolicy::AUTO, ScrollBarPolicy::ALWAYS};
  constexpr std::array PLACEMENTS{ScrollPlacement::RESERVED, ScrollPlacement::OVERLAY, ScrollPlacement::ON_FRAME};
  constexpr ScrollLayoutOptions LEGACY{ScrollBarPolicy::AUTO, ScrollBarPolicy::AUTO, ScrollPlacement::ON_FRAME, true};

  static_assert(LEGACY.frameSides.top && LEGACY.frameSides.right && LEGACY.frameSides.bottom && LEGACY.frameSides.left);
  static_assert(
    solveScrollLayout(
      {5, 1}, {20, 20},
      {ScrollBarPolicy::ALWAYS, ScrollBarPolicy::NEVER, ScrollPlacement::ON_FRAME, true, {false, false, true, false}})
      ->horizontal.length == 5);

  for (int mask = 0; mask < 16; ++mask) {
    const FrameSides sides{(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0};

    for (const int width : {0, 1, 2, 3, 9}) {
      for (const int height : {0, 1, 2, 3, 7}) {
        const CellSize size{width, height};

        for (const auto horizontal : POLICIES) {
          for (const auto vertical : POLICIES) {
            for (const auto placement : PLACEMENTS) {
              for (const bool frame : {false, true}) {
                const ScrollLayoutOptions options{horizontal, vertical, placement, frame, sides};
                const auto result = solveScrollLayout(size, {20, 30}, options);
                const bool requiresMissingEdge = placement == ScrollPlacement::ON_FRAME &&
                                                 (!frame || (horizontal != ScrollBarPolicy::NEVER && !sides.bottom) ||
                                                  (vertical != ScrollBarPolicy::NEVER && !sides.right));

                check(result.has_value() != requiresMissingEdge, "ON_FRAME admits exactly its possible bar sides");

                if (!result) {
                  continue;
                }

                const auto expectedOrigin =
                  frame ? CellPoint{std::min(size.width, sides.left ? 1 : 0), std::min(size.height, sides.top ? 1 : 0)}
                        : CellPoint{};

                check(result->viewportOrigin == expectedOrigin, "partial frame origin reserves only present sides");
                check(result->viewportOrigin.x + result->viewport.width <= size.width &&
                        result->viewportOrigin.y + result->viewport.height <= size.height,
                      "partial viewport stays in allocation");

                if (!frame) {
                  const auto plain = take(solveScrollLayout(size, {20, 30}, {horizontal, vertical, placement, false}));

                  check(result->viewport == plain.viewport && result->childClipSize == plain.childClipSize &&
                          result->horizontal.length == plain.horizontal.length &&
                          result->vertical.length == plain.vertical.length,
                        "disabled frame ignores its side descriptor");
                }

                for (int y = 0; y < size.height; ++y) {
                  for (int x = 0; x < size.width; ++x) {
                    const bool h = trackContains(result->horizontal, true, x, y);
                    const bool v = trackContains(result->vertical, false, x, y);
                    const bool content = x >= result->viewportOrigin.x && y >= result->viewportOrigin.y &&
                                         x - result->viewportOrigin.x < result->childClipSize.width &&
                                         y - result->viewportOrigin.y < result->childClipSize.height;

                    check(!(h && v) && !(content && (h || v)), "partial frame track/content ownership is disjoint");

                    if (placement == ScrollPlacement::ON_FRAME) {
                      const bool corner = ((y == 0 && sides.top) || (y == size.height - 1 && sides.bottom)) &&
                                          ((x == 0 && sides.left) || (x == size.width - 1 && sides.right));
                      const bool expectedH = result->horizontalBar && sides.bottom && y == size.height - 1 && !corner &&
                                             !(size.height == 1 && sides.top);
                      const bool expectedV = result->verticalBar && sides.right && x == size.width - 1 && !corner &&
                                             !(size.width == 1 && sides.left);

                      check(h == expectedH && v == expectedV, "ON_FRAME owns each present non-corner track cell once");
                    }
                  }
                }

                for (const auto track : {result->horizontal, result->vertical}) {
                  const auto thumb = take(scrollThumb({MAXIMUM, 1}, MAXIMUM, track.length));

                  check(thumb.length <= track.length && thumb.start + thumb.length <= track.length,
                        "partial frame thumb stays in its track");

                  if (thumb.travel != 0) {
                    check(take(scrollOffsetForThumb(thumb.travel, thumb.travel, MAXIMUM - 1)) == MAXIMUM - 1,
                          "partial frame input mapping preserves the exact logical endpoint");
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

void options() {
  ScrollAreaOptions area;
  ScrollBarOptions bar;

  check(validateScrollOptions(area).has_value() && validateScrollOptions(bar).has_value(), "defaults validate");
  check(area.horizontal == ScrollBarPolicy::AUTO && area.vertical == ScrollBarPolicy::AUTO &&
          area.placement == ScrollPlacement::RESERVED && !area.frame && area.wheelStep == 3 && area.keyStep == 1 &&
          area.pageOverlap == 1 && area.minimumThumb == 1 && area.focusable && !area.readOnly &&
          bar.wheelStep == area.wheelStep && bar.maxPaintCells == 16384,
        "reviewed defaults preserved");

  for (const auto step :
       {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    area.wheelStep = step;
    bar.wheelStep = step;

    check(!validateScrollOptions(area) && !validateScrollOptions(bar), "invalid wheel step rejected");
  }

  area = {};
  area.placement = ScrollPlacement::ON_FRAME;

  check(!validateScrollOptions(area), "ON_FRAME options require outline");

  area.frame = BorderOptions{};

  check(validateScrollOptions(area).has_value(), "existing frame options accepted");

  area.frame->maxPaintCells = 0;

  check(!validateScrollOptions(area), "invalid existing frame budget rejected");

  bar = {};
  bar.pageOverlap = MAXIMUM;
  bar.wheelStep = 0.5;

  check(validateScrollOptions(bar).has_value(), "fractional wheel and large page overlap accepted");

  bar.keyStep = 0;

  check(!validateScrollOptions(bar), "zero key step rejected");

  bar = {};
  bar.minimumThumb = 0;

  check(!validateScrollOptions(bar), "zero thumb rejected");

  bar = {};
  bar.maxPaintCells = 0;

  check(!validateScrollOptions(bar), "zero paint cap rejected");

  bar = {};
  bar.glyphs = static_cast<PrimitiveGlyphs>(99);

  check(!validateScrollOptions(bar), "unknown glyph mode rejected");
}
} // namespace

int main() {
  fractionalThumbs();
  thumbs();
  preciseThumbOffsets();
  placement();
  partialFrames();
  options();

  if (failures != 0) {
    std::cerr << failures << " scroll geometry checks failed\n";
  }

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
