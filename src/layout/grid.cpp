// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../widgets/scroll_controls_state.hpp"
#include "grid_detail.hpp"
#include <array>
#include <initializer_list>
#include <new>
#include <set>

namespace portals {
namespace detail {
std::expected<void, TreeError> validateGridTrackSizes(std::span<const AxisSize> policies) noexcept {
  for (const auto policy : policies) {
    if (!grid_detail::valid(policy)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
  }

  return {};
}

struct GridChildGeometry {
  NodeId node;
  LayoutChildPolicy policy;
};

struct GridLayoutState {
  std::vector<MeasuredChild> inputs = std::vector<MeasuredChild>(0);
  std::vector<MeasuredChild> columns = std::vector<MeasuredChild>(0);
  std::vector<MeasuredChild> headerRows = std::vector<MeasuredChild>(0);
  std::vector<MeasuredChild> bodyRows = std::vector<MeasuredChild>(0);
  std::vector<CellRect> paintedRows = std::vector<CellRect>(0);
  // One fallible allocation avoids checked-STL hash-container constructors with noexcept proxy allocation.
  std::vector<GridChildGeometry> children = std::vector<GridChildGeometry>(0);
  CellSize intrinsic;
  std::int32_t headerHeight{};
  std::int32_t bodyHeight{};
  CellSize allocation;
  CellRect envelope;
  CellRect headerClip;
  CellRect bodyClip;
  CellPoint contentOrigin;
  std::int32_t horizontalOffset{};
  std::int32_t verticalOffset{};
  bool measured{};
  bool arranged{};
};
} // namespace detail

namespace {
using grid_detail::Wide;

std::expected<void, TreeError> prepareChildPolicies(detail::GridLayoutState &layout, std::size_t count) {
  constexpr std::size_t CHILD_TABLE_LOAD_FACTOR = 2;
  const auto maximum = layout.children.max_size();

  if (count > maximum / CHILD_TABLE_LOAD_FACTOR) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  const auto minimum = count * CHILD_TABLE_LOAD_FACTOR;
  std::size_t capacity = count ? 1 : 0;

  while (capacity < minimum) {
    if (capacity > maximum / CHILD_TABLE_LOAD_FACTOR) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    capacity *= CHILD_TABLE_LOAD_FACTOR;
  }

  layout.children.clear();
  layout.children.resize(capacity);

  return {};
}

detail::GridChildGeometry *childPolicy(detail::GridLayoutState &layout, NodeId node) noexcept {
  const auto count = layout.children.size();

  if (count == 0) {
    return nullptr;
  }

  auto index = static_cast<std::size_t>(node.slot) % count;

  for (std::size_t probe = 0; probe < count; ++probe) {
    auto &entry = layout.children[index];

    if (entry.node.session == 0 || entry.node == node) {
      return &entry;
    }

    index = (index + 1) % count;
  }

  return nullptr;
}

bool sameAxis(AxisSize left, AxisSize right) {
  return left.minimum == right.minimum && left.preferred == right.preferred && left.maximum == right.maximum &&
         left.mode == right.mode && left.weight == right.weight;
}

bool sameInputs(std::span<const MeasuredChild> left, std::span<const MeasuredChild> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](const auto &a, const auto &b) {
           return a.node == b.node && a.desired == b.desired && sameAxis(a.policy.width, b.policy.width) &&
                  sameAxis(a.policy.height, b.policy.height);
         });
}

bool within(std::size_t first, std::size_t count, std::size_t bound) {
  return count > 0 && first < bound && count <= bound - first;
}

std::expected<void, TreeError> validate(const GridOptions &options,
                                        GridPlacementMode mode = GridPlacementMode::EXPLICIT) {
  if (options.columns.empty() || options.rows.empty() || options.max_tracks_per_axis == 0 ||
      options.max_placements == 0 || options.column_gap < 0 || options.row_gap < 0 ||
      !grid_detail::valid(options.padding) || options.presentation.maxPaintCells == 0 ||
      (!options.regions && !options.header_rows.empty()) ||
      (mode != GridPlacementMode::EXPLICIT && mode != GridPlacementMode::DECLARATIVE)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (options.columns.size() > options.max_tracks_per_axis || options.rows.size() > options.max_tracks_per_axis ||
      options.header_rows.size() > options.max_tracks_per_axis - options.rows.size() ||
      options.cells.size() > options.max_placements) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  if (options.columns.size() > static_cast<std::size_t>(grid_detail::CELL_LIMIT) ||
      options.rows.size() > static_cast<std::size_t>(grid_detail::CELL_LIMIT)) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  for (const auto &axis : {std::span(options.columns), std::span(options.rows), std::span(options.header_rows)}) {
    for (auto track : axis) {
      if (!grid_detail::valid(track)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
  }

  for (std::size_t i = 0; i < options.cells.size(); ++i) {
    const auto &cell = options.cells[i];

    const auto rowCount = cell.region == GridRegion::HEADER ? options.header_rows.size() : options.rows.size();

    if ((cell.region != GridRegion::BODY && cell.region != GridRegion::HEADER) || cell.node.session == 0 ||
        cell.node.generation == 0 || !within(cell.row, cell.row_span, rowCount) ||
        !within(cell.column, cell.column_span, options.columns.size()) || !grid_detail::valid(cell.horizontal) ||
        !grid_detail::valid(cell.vertical)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    for (std::size_t j = 0; j < i; ++j) {
      if (cell.node == options.cells[j].node) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }

    if (mode == GridPlacementMode::DECLARATIVE) {
      for (std::size_t j = 0; j < i; ++j) {
        const auto &previous = options.cells[j];

        if (cell.region == previous.region && cell.row < previous.row + previous.row_span &&
            previous.row < cell.row + cell.row_span && cell.column < previous.column + previous.column_span &&
            previous.column < cell.column + cell.column_span) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
      }
    }
  }

  return {};
}

class PlacementSearch {
public:
  PlacementSearch(const DeclarativeGridOptions &options, std::size_t &remaining)
      : options_(options), remaining_(remaining) {
  }

  std::expected<void, TreeError> spend() {
    if (remaining_ == 0) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    --remaining_;

    return {};
  }

  std::expected<bool, TreeError> visit(GridPosition origin, const GridItemOptions &item, bool reserve) {
    for (std::size_t row = origin.row; row < origin.row + item.rowSpan; ++row) {
      for (std::size_t column = origin.column; column < origin.column + item.columnSpan; ++column) {
        auto budget = spend();

        if (!budget) {
          return std::unexpected(budget.error());
        }

        if (reserve) {
          if (!occupied_.emplace(row, column).second) {
            return false;
          }
        } else if (occupied_.contains({row, column})) {
          return false;
        }
      }
    }

    return true;
  }

  std::expected<GridPosition, TreeError> next(const GridItemOptions &item) {
    const bool rowMajor = options_.flow == GridFlow::ROW_MAJOR;
    const auto minorCount = rowMajor ? *options_.columns : *options_.rows;
    const auto majorBound = rowMajor ? options_.rows : options_.columns;
    const auto majorLimit = majorBound.value_or(options_.maxTracksPerAxis);

    while (cursor_ / minorCount < majorLimit) {
      auto budget = spend();

      if (!budget) {
        return std::unexpected(budget.error());
      }

      const auto major = cursor_ / minorCount;
      const auto minor = cursor_ % minorCount;
      const GridPosition origin = rowMajor ? GridPosition{major, minor} : GridPosition{minor, major};

      // The resolved track budget bounds both axes before any span is traversed.
      if (cursor_ == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }

      ++cursor_;

      const auto minorSpan = rowMajor ? item.columnSpan : item.rowSpan;
      const auto majorSpan = rowMajor ? item.rowSpan : item.columnSpan;

      if (minorSpan > minorCount - minor || majorSpan > majorLimit - major) {
        continue;
      }

      auto free = visit(origin, item, false);

      if (!free) {
        return std::unexpected(free.error());
      }

      if (!*free) {
        continue;
      }

      auto reserved = visit(origin, item, true);

      if (!reserved) {
        return std::unexpected(reserved.error());
      }

      return origin;
    }

    return std::unexpected(majorBound ? TreeError::INVALID_ARGUMENT : TreeError::NODE_LIMIT);
  }

private:
  const DeclarativeGridOptions &options_;
  std::size_t &remaining_;
  std::size_t cursor_{};
  std::set<std::pair<std::size_t, std::size_t>> occupied_;
};

std::expected<void, TreeError> validateItem(const DeclarativeGridOptions &options, const GridItemOptions &item) {
  if (item.row.has_value() != item.column.has_value() || item.rowSpan == 0 || item.columnSpan == 0 ||
      !grid_detail::valid(item.horizontal) || !grid_detail::valid(item.vertical) ||
      (item.region != GridRegion::BODY && item.region != GridRegion::HEADER)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto row = item.row.value_or(0);
  const auto column = item.column.value_or(0);
  const auto maximum = std::numeric_limits<std::size_t>::max();

  if (item.rowSpan > maximum - row || item.columnSpan > maximum - column) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  if ((options.rows && (row >= *options.rows || item.rowSpan > *options.rows - row)) ||
      (options.columns && (column >= *options.columns || item.columnSpan > *options.columns - column))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (row >= options.maxTracksPerAxis || column >= options.maxTracksPerAxis ||
      item.rowSpan > options.maxTracksPerAxis - row || item.columnSpan > options.maxTracksPerAxis - column) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  return {};
}

void swapOptions(GridOptions &current, GridOptions &replacement) {
  current.columns.swap(replacement.columns);
  current.rows.swap(replacement.rows);
  current.cells.swap(replacement.cells);
  current.header_rows.swap(replacement.header_rows);
  std::swap(current.column_gap, replacement.column_gap);
  std::swap(current.row_gap, replacement.row_gap);
  std::swap(current.padding, replacement.padding);
  std::swap(current.max_tracks_per_axis, replacement.max_tracks_per_axis);
  std::swap(current.max_placements, replacement.max_placements);
  std::swap(current.sticky_header, replacement.sticky_header);
  std::swap(current.regions, replacement.regions);
  std::swap(current.presentation, replacement.presentation);
}

const GridCell *placement(const GridOptions &options, NodeId node) {
  const auto found = std::ranges::find_if(options.cells, [&](const GridCell &cell) {
    return cell.node == node;
  });

  return found == options.cells.end() ? nullptr : &*found;
}

// Only intrinsic, nonfixed tracks absorb demand; explicit preferences remain authoritative.
void absorb(std::span<MeasuredChild> tracks, Wide demand, std::int32_t gap) {
  Wide total = Wide{gap} * static_cast<Wide>(tracks.size() - 1);

  for (const auto &track : tracks) {
    total += track.desired.width;
  }

  Wide deficit = std::max<Wide>(0, demand - total);

  while (deficit > 0) {
    Wide eligible{};

    for (const auto &track : tracks) {
      if (track.policy.width.mode != SizeMode::FIXED && track.policy.width.preferred == INTRINSIC_EXTENT &&
          track.desired.width < track.policy.width.maximum) {
        ++eligible;
      }
    }

    if (eligible == 0) {
      break;
    }

    const Wide quotient = deficit / eligible;
    bool saturated{};

    // Saturate capped tracks before assigning remainder cells to the remaining tracks.
    for (auto &track : tracks) {
      if (track.policy.width.mode == SizeMode::FIXED || track.policy.width.preferred != INTRINSIC_EXTENT ||
          track.desired.width == track.policy.width.maximum) {
        continue;
      }

      const Wide headroom = track.policy.width.maximum - track.desired.width;

      if (headroom <= quotient) {
        track.desired.width = track.policy.width.maximum;
        deficit -= headroom;
        saturated = true;
      }
    }

    // A capped track must not consume a remainder cell it could not accept. Restart
    // the equal-share calculation over remaining headroom before distributing extras.
    if (saturated) {
      continue;
    }

    Wide remainder = deficit % eligible;

    for (auto &track : tracks) {
      if (track.policy.width.mode == SizeMode::FIXED || track.policy.width.preferred != INTRINSIC_EXTENT ||
          track.desired.width == track.policy.width.maximum) {
        continue;
      }

      const Wide growth = quotient + (remainder > 0 ? 1 : 0);

      remainder = std::max<Wide>(0, remainder - 1);
      track.desired.width += static_cast<std::int32_t>(growth);
      deficit -= growth;
    }
  }
}

std::expected<std::vector<MeasuredChild>, TreeError> tracks(const GridOptions &options,
                                                            std::span<const MeasuredChild> children, bool horizontal,
                                                            GridRegion region = GridRegion::BODY) {
  const auto &policies = horizontal                     ? options.columns
                         : region == GridRegion::HEADER ? options.header_rows
                                                        : options.rows;
  const auto gap = horizontal ? options.column_gap : options.row_gap;
  std::vector<MeasuredChild> result(0);

  if (policies.size() > result.max_size()) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  result.reserve(policies.size());

  for (auto policy : policies) {
    MeasuredChild track;

    track.policy.width = policy;
    track.desired.width = static_cast<std::int32_t>(grid_detail::requested(policy, 0));
    result.push_back(track);
  }

  // Single-track demand is established before spanning demand, independently of child order.
  for (bool spanning : {false, true}) {
    for (const auto &child : children) {
      const auto *cell = placement(options, child.node);

      if (!cell || !grid_detail::valid(child)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      if (!horizontal && cell->region != region) {
        continue;
      }

      const auto first = horizontal ? cell->column : cell->row;
      const auto count = horizontal ? cell->column_span : cell->row_span;

      if ((count > 1) != spanning) {
        continue;
      }

      const auto policy = horizontal ? child.policy.width : child.policy.height;
      const auto desired = horizontal ? child.desired.width : child.desired.height;

      absorb(std::span(result).subspan(first, count), grid_detail::requested(policy, desired), gap);
    }
  }

  return std::expected<std::vector<MeasuredChild>, TreeError>(std::in_place, result);
}

// Reuse one-dimensional allocation for each axis. Rows use the same x/width solver
// as columns; cellRect translates its results back to two-dimensional geometry.
Row axisLayout(std::int32_t gap) {
  LinearOptions options;

  options.gap = gap;

  return Row(options);
}

bool regionalLayout(const GridOptions &options, bool scroll) {
  return scroll || options.regions || !options.header_rows.empty() || options.sticky_header ||
         options.presentation.header || options.presentation.body || options.presentation.rows.even ||
         options.presentation.rows.odd;
}

std::expected<std::unique_ptr<detail::GridLayoutState>, TreeError>
measureRegions(const GridOptions &options, std::span<const MeasuredChild> children) {
  auto columns = tracks(options, children, true);
  auto header = tracks(options, children, false, GridRegion::HEADER);
  auto body = tracks(options, children, false);

  if (!columns || !header || !body) {
    return std::unexpected(!columns ? columns.error() : !header ? header.error() : body.error());
  }

  auto width = axisLayout(options.column_gap).measureChildren(*columns);
  auto headerSize = axisLayout(options.row_gap).measureChildren(*header);
  auto bodySize = axisLayout(options.row_gap).measureChildren(*body);

  if (!width || !headerSize || !bodySize) {
    return std::unexpected(!width ? width.error() : !headerSize ? headerSize.error() : bodySize.error());
  }

  auto content = grid_detail::size(width->width, Wide{headerSize->width} + bodySize->width);

  if (!content) {
    return std::unexpected(content.error());
  }

  auto result = std::make_unique<detail::GridLayoutState>();

  result->inputs.assign(children.begin(), children.end());
  result->columns.swap(*columns);
  result->headerRows.swap(*header);
  result->bodyRows.swap(*body);
  result->intrinsic = *content;
  result->headerHeight = headerSize->width;
  result->bodyHeight = bodySize->width;
  result->measured = true;

  return result;
}

std::expected<CellRect, TreeError> localRect(CellPoint origin, CellSize size) {
  auto result = CellRect::from(origin, size);

  return result ? std::expected<CellRect, TreeError>(*result) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

ScrollOffset effectiveGridOffset(ScrollOffset offset, ScrollAxisMetrics metrics) {
  const auto maximum = scrollRange(metrics).maximum;

  offset.raster = std::min(offset.raster, maximum);

  if (offset.raster == maximum) {
    offset.fraction = 0;
  }

  return offset;
}

std::expected<ScrollAreaGeometry, TreeError> regionScrollGeometry(const GridOptions &options,
                                                                  const detail::GridLayoutState &layout,
                                                                  const detail::ScrollWidgetState &scroll,
                                                                  CellSize available) {
  bool horizontal = scroll.area.horizontal == ScrollBarPolicy::ALWAYS;
  bool vertical = scroll.area.vertical == ScrollBarPolicy::ALWAYS;
  ScrollAreaGeometry geometry;

  // At most one monotone addition per axis; preserve the existing three-pass bar fixed point.
  constexpr int MAX_BAR_PASSES = 3;

  for (int pass = 0; pass < MAX_BAR_PASSES; ++pass) {
    auto placed = solveScrollLayout(available, {},
                                    {horizontal ? ScrollBarPolicy::ALWAYS : ScrollBarPolicy::NEVER,
                                     vertical ? ScrollBarPolicy::ALWAYS : ScrollBarPolicy::NEVER, scroll.area.placement,
                                     scroll.area.frame.has_value()});

    if (!placed) {
      return std::unexpected(placed.error());
    }

    geometry = *placed;

    auto inner = grid_detail::inset(geometry.viewport, options.padding);

    if (!inner) {
      return std::unexpected(inner.error());
    }

    const auto bodyViewport = options.sticky_header ? std::max(inner->height - layout.headerHeight, 0) : inner->height;
    const auto verticalExtent = options.sticky_header ? layout.bodyHeight : layout.intrinsic.height;
    const bool nextHorizontal =
      horizontal || (scroll.area.horizontal == ScrollBarPolicy::AUTO && layout.intrinsic.width > inner->width);
    const bool nextVertical =
      vertical || (scroll.area.vertical == ScrollBarPolicy::AUTO && verticalExtent > bodyViewport);

    if (nextHorizontal == horizontal && nextVertical == vertical) {
      break;
    }

    horizontal = nextHorizontal;
    vertical = nextVertical;
  }

  return geometry;
}

std::expected<void, TreeError> arrangeRegions(const GridOptions &options, detail::GridLayoutState &layout,
                                              detail::ScrollWidgetState *scroll, CellSize available) {
  if (!layout.measured) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  available = available.normalized();

  ScrollAreaGeometry geometry;
  std::optional<ScrollControllerSnapshot> observed;

  if (scroll) {
    auto valid = detail::validateScrollWidget(*scroll);

    if (!valid) {
      return valid;
    }

    auto placed = regionScrollGeometry(options, layout, *scroll, available);
    auto snapshot = scroll->controller->snapshot();

    if (!placed || !snapshot) {
      return std::unexpected(placed ? snapshot.error() : placed.error());
    }

    if (scroll->layoutRevision == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    geometry = *placed;
    observed = *snapshot;
  } else {
    geometry.viewport = available;
    geometry.childClipSize = available;
  }

  auto viewport = grid_detail::inset(geometry.viewport, options.padding);
  auto clipSize = grid_detail::inset(geometry.childClipSize, options.padding);

  if (!viewport || !clipSize) {
    return std::unexpected(viewport ? clipSize.error() : viewport.error());
  }

  const CellPoint origin{geometry.viewportOrigin.x + std::min(options.padding.left, geometry.childClipSize.width),
                         geometry.viewportOrigin.y + std::min(options.padding.top, geometry.childClipSize.height)};
  auto envelope = localRect(origin, *clipSize);

  if (!envelope) {
    return std::unexpected(envelope.error());
  }

  const bool sticky = scroll && options.sticky_header;
  const bool reserveHeader = !scroll || sticky;
  const auto clippedHeader = std::min(layout.headerHeight, clipSize->height);
  auto headerClip = localRect(origin, {clipSize->width, reserveHeader ? clippedHeader : clipSize->height});
  auto bodyClip = localRect(
    {origin.x, origin.y + (reserveHeader ? clippedHeader : 0)},
    {clipSize->width, reserveHeader ? std::max(clipSize->height - layout.headerHeight, 0) : clipSize->height});

  if (!headerClip || !bodyClip) {
    return std::unexpected(headerClip ? bodyClip.error() : headerClip.error());
  }

  layout.allocation = available;
  layout.envelope = *envelope;
  layout.headerClip = *headerClip;
  layout.bodyClip = *bodyClip;
  layout.contentOrigin = origin;
  layout.horizontalOffset = 0;
  layout.verticalOffset = 0;
  layout.arranged = true;

  if (scroll) {
    geometry.viewportOrigin = {origin.x, origin.y + (sticky ? std::min(layout.headerHeight, viewport->height) : 0)};
    geometry.viewport = {viewport->width,
                         sticky ? std::max(viewport->height - layout.headerHeight, 0) : viewport->height};
    geometry.childClipSize = bodyClip->size();
    scroll->extent = {static_cast<std::uint64_t>(layout.intrinsic.width),
                      static_cast<std::uint64_t>(sticky ? layout.bodyHeight : layout.intrinsic.height)};
    observed->horizontal.metrics = {scroll->extent.width, static_cast<std::uint64_t>(geometry.viewport.width)};
    observed->vertical.metrics = {scroll->extent.height, static_cast<std::uint64_t>(geometry.viewport.height)};
    observed->horizontal.offset = effectiveGridOffset(observed->horizontal.offset, observed->horizontal.metrics);
    observed->vertical.offset = effectiveGridOffset(observed->vertical.offset, observed->vertical.metrics);
    layout.horizontalOffset = static_cast<std::int32_t>(observed->horizontal.offset.raster);
    layout.verticalOffset = static_cast<std::int32_t>(observed->vertical.offset.raster);
    scroll->allocation = available;
    scroll->geometry = geometry;
    scroll->presentation = *observed;
    scroll->arranged = true;
    ++scroll->layoutRevision;
  }

  return {};
}

std::expected<CellRect, TreeError> cellRect(const GridCell &cell, std::span<const CellRect> columns,
                                            std::span<const CellRect> rows, LayoutPadding padding) {
  const auto &left = columns[cell.column];
  const auto &right = columns[cell.column + cell.column_span - 1];
  const auto &top = rows[cell.row];
  const auto &bottom = rows[cell.row + cell.row_span - 1];
  auto dimensions = grid_detail::size(Wide{right.origin().x} + right.size().width - left.origin().x,
                                      Wide{bottom.origin().x} + bottom.size().width - top.origin().x);
  auto origin = grid_detail::size(Wide{left.origin().x} + padding.left, Wide{top.origin().x} + padding.top);

  if (!dimensions || !origin) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  auto result = CellRect::from({origin->width, origin->height}, *dimensions);

  if (!result) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  return *result;
}
} // namespace

namespace {
std::expected<GridPlacementPlan, TreeError> resolveGridRegion(const DeclarativeGridOptions &options,
                                                              std::span<const GridItemOptions> items,
                                                              std::size_t &remaining) {
  if ((options.flow != GridFlow::ROW_MAJOR && options.flow != GridFlow::COLUMN_MAJOR) ||
      (options.flow == GridFlow::ROW_MAJOR && !options.columns) ||
      (options.flow == GridFlow::COLUMN_MAJOR && !options.rows) || options.columns == 0 || options.rows == 0 ||
      options.columnGap < 0 || options.rowGap < 0 || !grid_detail::valid(options.padding) ||
      options.maxTracksPerAxis == 0 || options.maxPlacements == 0 || options.maxPlacementWork == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (items.size() > options.maxPlacements || options.columns > options.maxTracksPerAxis ||
      options.rows > options.maxTracksPerAxis) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  for (const auto &item : items) {
    auto checked = validateItem(options, item);

    if (!checked) {
      return std::unexpected(checked.error());
    }
  }

  try {
    GridPlacementPlan result;
    PlacementSearch search(options, remaining);

    if (items.size() > result.positions.max_size()) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    result.positions.resize(items.size());
    result.columns = options.columns.value_or(1);
    result.rows = options.rows.value_or(1);

    // Reserve future explicit entries before searching; recipe visibility is deliberately absent.
    for (std::size_t index = 0; index < items.size(); ++index) {
      const auto &item = items[index];

      if (!item.row) {
        continue;
      }

      const GridPosition position{*item.row, *item.column};
      auto reserved = search.visit(position, item, true);

      if (!reserved || !*reserved) {
        return std::unexpected(reserved ? TreeError::INVALID_ARGUMENT : reserved.error());
      }

      result.positions[index] = position;
      result.columns = std::max(result.columns, position.column + item.columnSpan);
      result.rows = std::max(result.rows, position.row + item.rowSpan);
    }

    for (std::size_t index = 0; index < items.size(); ++index) {
      const auto &item = items[index];

      if (item.row) {
        continue;
      }

      auto position = search.next(item);

      if (!position) {
        return std::unexpected(position.error());
      }

      result.positions[index] = *position;
      result.columns = std::max(result.columns, position->column + item.columnSpan);
      result.rows = std::max(result.rows, position->row + item.rowSpan);
    }

    if (result.columns > static_cast<std::size_t>(grid_detail::CELL_LIMIT) ||
        result.rows > static_cast<std::size_t>(grid_detail::CELL_LIMIT)) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    const auto minimumExtent = grid_detail::size(
      static_cast<Wide>(result.columns - 1) * options.columnGap + options.padding.left + options.padding.right,
      static_cast<Wide>(result.rows - 1) * options.rowGap + options.padding.top + options.padding.bottom);

    if (!minimumExtent) {
      return std::unexpected(minimumExtent.error());
    }

    return std::expected<GridPlacementPlan, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}
} // namespace

std::expected<GridPlacementPlan, TreeError> resolveGridPlacements(const DeclarativeGridOptions &options,
                                                                  std::span<const GridItemOptions> items) {
  std::size_t remaining = options.maxPlacementWork;

  if (!options.regions) {
    if (options.stickyHeader || std::ranges::any_of(items, [](const GridItemOptions &item) {
          return item.region != GridRegion::BODY;
        })) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    return resolveGridRegion(options, items, remaining);
  }

  if (!options.columns || items.size() > options.maxPlacements) {
    return std::unexpected(!options.columns ? TreeError::INVALID_ARGUMENT : TreeError::NODE_LIMIT);
  }

  try {
    std::vector<GridItemOptions> header(0);
    std::vector<GridItemOptions> body(0);

    header.reserve(items.size());
    body.reserve(items.size());

    for (const auto &item : items) {
      if (item.region == GridRegion::HEADER) {
        header.push_back(item);
      } else if (item.region == GridRegion::BODY) {
        body.push_back(item);
      } else {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }

    auto headerOptions = options;

    headerOptions.rows.reset();
    headerOptions.flow = GridFlow::ROW_MAJOR;

    auto headerPlan = resolveGridRegion(headerOptions, header, remaining);
    auto bodyPlan = resolveGridRegion(options, body, remaining);

    if (!headerPlan || !bodyPlan) {
      return std::unexpected(headerPlan ? bodyPlan.error() : headerPlan.error());
    }

    GridPlacementPlan result;

    result.columns = *options.columns;
    result.rows = bodyPlan->rows;
    result.headerRows = header.empty() ? 0 : headerPlan->rows;

    if (result.headerRows > options.maxTracksPerAxis - result.rows) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    const auto gapCount = result.rows - 1 + (result.headerRows ? result.headerRows - 1 : 0);
    const auto minimumExtent = grid_detail::size(
      static_cast<Wide>(result.columns - 1) * options.columnGap + options.padding.left + options.padding.right,
      static_cast<Wide>(gapCount) * options.rowGap + options.padding.top + options.padding.bottom);

    if (!minimumExtent) {
      return std::unexpected(minimumExtent.error());
    }

    result.positions.reserve(items.size());

    std::size_t headerIndex{};
    std::size_t bodyIndex{};

    for (const auto &item : items) {
      result.positions.push_back(item.region == GridRegion::HEADER ? headerPlan->positions[headerIndex++]
                                                                   : bodyPlan->positions[bodyIndex++]);
    }

    return std::expected<GridPlacementPlan, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

Grid::Grid(const GridOptions &options, SizePolicy policy) : SizedWidget(policy), options_(options) {
}

Grid::Grid(const GridOptions &options, GridPlacementMode mode, SizePolicy policy,
           std::optional<GridScrollOptions> scroll, WidthPolicy width)
    : SizedWidget(policy), options_(options), mode_(mode) {
  if (scroll) {
    scroll_ = std::make_unique<detail::ScrollWidgetState>(std::move(scroll->controller), width, scroll->area.glyphs);
    scroll_->isArea = true;
    scroll_->area = scroll->area;

    if (scroll_->area.frame) {
      scroll_->legacyFrameOptions.border = *scroll_->area.frame;

      auto prepared = detail::prepareFrameText(width, scroll_->legacyFrameOptions);

      if (prepared) {
        scroll_->legacyFrameText = std::move(*prepared);
      } else {
        scroll_->frameError = prepared.error();
      }
    }
  }
}

Grid::~Grid() = default;

void Grid::unmount() {
  if (scroll_) {
    scroll_->retired = true;
    scroll_->intent.reset();
    scroll_->capture.disconnect();
    scroll_->lease.reset();
  }
}

GridOptions Grid::options() const {
  return options_;
}

bool Grid::usesRegions() const noexcept {
  return options_.regions;
}

std::expected<CellSize, TreeError> Grid::childAvailable(CellSize available) const {
  auto checked = validate(options_, mode_);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  if (scroll_) {
    auto valid = detail::validateScrollWidget(*scroll_);

    if (!valid) {
      return std::unexpected(valid.error());
    }

    available = available.normalized();

    if (scroll_->area.frame) {
      available = {std::max(available.width - 2, 0), std::max(available.height - 2, 0)};
    }
  }

  return grid_detail::inset(available, options_.padding);
}

std::expected<CellSize, TreeError> Grid::measureChildren(std::span<const MeasuredChild> children) const {
  auto checked = validate(options_, mode_);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  if (children.size() > options_.max_placements) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  try {
    if (regionalLayout(options_, scroll_ != nullptr)) {
      auto measured = measureRegions(options_, children);

      if (!measured) {
        return std::unexpected(measured.error());
      }

      auto padded = grid_detail::padded((*measured)->intrinsic, options_.padding);

      if (!padded) {
        return std::unexpected(padded.error());
      }

      const auto frameInset = scroll_ && scroll_->area.frame ? 2 : 0;
      auto result = grid_detail::size(Wide{padded->width} + frameInset, Wide{padded->height} + frameInset);

      if (!result) {
        return std::unexpected(result.error());
      }

      layout_ = std::move(*measured);

      return *result;
    }

    auto columns = tracks(options_, children, true);
    auto rows = tracks(options_, children, false);

    if (!columns || !rows) {
      return std::unexpected(columns ? rows.error() : columns.error());
    }

    auto width = axisLayout(options_.column_gap).measureChildren(*columns);
    auto height = axisLayout(options_.row_gap).measureChildren(*rows);

    if (!width || !height) {
      return std::unexpected(width ? height.error() : width.error());
    }

    return grid_detail::padded({width->width, height->width}, options_.padding);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> Grid::arrange(CellRect local) {
  if (!regionalLayout(options_, scroll_ != nullptr)) {
    return {};
  }

  if (!layout_) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  return arrangeRegions(options_, *layout_, scroll_.get(), local.size());
}

std::expected<CellRect, TreeError> Grid::childClip(CellSize available) const {
  if (!regionalLayout(options_, scroll_ != nullptr)) {
    return Layout::childClip(available);
  }

  if (!layout_ || !layout_->arranged || layout_->allocation != available.normalized()) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  return layout_->envelope;
}

std::expected<LayoutChildPolicy, TreeError> Grid::childLayoutFor(NodeId child, CellSize available) const {
  if (!regionalLayout(options_, scroll_ != nullptr)) {
    return Layout::childLayoutFor(child, available);
  }

  if (!layout_ || !layout_->arranged || layout_->allocation != available.normalized()) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  const auto *found = childPolicy(*layout_, child);

  if (!found || found->node != child) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return found->policy;
}

std::expected<std::vector<CellRect>, TreeError> Grid::arrangeChildren(std::span<const MeasuredChild> children,
                                                                      CellSize available) const {
  auto content = childAvailable(available);

  if (!content) {
    return std::unexpected(content.error());
  }

  if (children.size() > options_.max_placements) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  try {
    if (regionalLayout(options_, scroll_ != nullptr)) {
      if (!layout_ || !layout_->arranged || layout_->allocation != available.normalized() ||
          !sameInputs(layout_->inputs, children)) {
        auto measured = measureRegions(options_, children);

        if (!measured) {
          return std::unexpected(measured.error());
        }

        auto arranged = arrangeRegions(options_, **measured, scroll_.get(), available);

        if (!arranged) {
          return std::unexpected(arranged.error());
        }

        layout_ = std::move(*measured);
      }

      struct Arrangement {
        detail::GridLayoutState &layout;
        bool complete{};

        ~Arrangement() {
          if (!complete) {
            layout.arranged = false;
          }
        }
      } arrangement{*layout_};

      const auto viewport = scroll_ ? scroll_->geometry.viewport : layout_->envelope.size();
      const auto columnSpace = scroll_ ? std::max(layout_->intrinsic.width, viewport.width) : viewport.width;
      const auto bodyViewport =
        scroll_ && options_.sticky_header ? viewport.height : std::max(viewport.height - layout_->headerHeight, 0);
      const auto bodySpace = scroll_ ? std::max(layout_->bodyHeight, bodyViewport) : bodyViewport;
      auto columns = axisLayout(options_.column_gap).arrangeChildren(layout_->columns, {columnSpace, 0});
      auto header = axisLayout(options_.row_gap).arrangeChildren(layout_->headerRows, {layout_->headerHeight, 0});
      auto body = axisLayout(options_.row_gap).arrangeChildren(layout_->bodyRows, {bodySpace, 0});

      if (!columns || !header || !body) {
        return std::unexpected(!columns ? columns.error() : !header ? header.error() : body.error());
      }

      std::vector<CellRect> result(0);
      std::vector<bool> visibleRows(body->size(), false);

      result.reserve(children.size());

      auto policies = prepareChildPolicies(*layout_, children.size());

      if (!policies) {
        return std::unexpected(policies.error());
      }

      layout_->paintedRows.clear();

      for (const auto &child : children) {
        const auto *cell = placement(options_, child.node);

        if (!cell) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }

        const bool isHeader = cell->region == GridRegion::HEADER;
        const auto &rows = isHeader ? *header : *body;
        auto slot = cellRect(*cell, *columns, rows, {});

        if (!slot) {
          return std::unexpected(slot.error());
        }

        auto aligned = grid_detail::place(child, *slot, cell->horizontal, cell->vertical);

        if (!aligned) {
          return std::unexpected(aligned.error());
        }

        const Wide y = Wide{layout_->contentOrigin.y} + (isHeader ? 0 : layout_->headerHeight) -
                       ((!isHeader || !options_.sticky_header) ? layout_->verticalOffset : 0);
        const Wide x = Wide{layout_->contentOrigin.x} - layout_->horizontalOffset;

        if (x < std::numeric_limits<std::int32_t>::min() || x > grid_detail::CELL_LIMIT ||
            y < std::numeric_limits<std::int32_t>::min() || y > grid_detail::CELL_LIMIT) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        auto placed = aligned->translated({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});

        if (!placed) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        result.push_back(*placed);

        const bool sticky = scroll_ && options_.sticky_header;
        const auto proposal = !isHeader && (scroll_ || options_.regions)
                                ? (layout_->bodyClip.empty() ? RegionalInput::CLOSED : RegionalInput::OPEN)
                                : RegionalInput::UNCHANGED;

        auto *entry = childPolicy(*layout_, child.node);

        if (!entry) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        *entry = {child.node,
                  {isHeader ? layout_->headerClip : layout_->bodyClip,
                   isHeader && sticky ? RevealAxes::HORIZONTAL_ONLY : RevealAxes::BOTH, proposal}};

        if (!isHeader) {
          for (std::size_t row = cell->row; row < cell->row + cell->row_span; ++row) {
            visibleRows[row] = true;
          }
        }
      }

      layout_->paintedRows.resize(body->size());

      for (std::size_t index = 0; index < body->size(); ++index) {
        if (!visibleRows[index]) {
          continue;
        }

        const auto &row = (*body)[index];
        const Wide y =
          Wide{layout_->contentOrigin.y} + layout_->headerHeight + row.origin().x - layout_->verticalOffset;

        if (y < std::numeric_limits<std::int32_t>::min() || y > grid_detail::CELL_LIMIT) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        auto rectangle = localRect({layout_->bodyClip.origin().x, static_cast<std::int32_t>(y)},
                                   {layout_->bodyClip.size().width, row.size().width});

        if (!rectangle) {
          return std::unexpected(rectangle.error());
        }

        layout_->paintedRows[index] = rectangle->intersection(layout_->bodyClip);
      }

      arrangement.complete = true;

      return std::expected<std::vector<CellRect>, TreeError>(std::in_place, result);
    }

    auto columns = tracks(options_, children, true);
    auto rows = tracks(options_, children, false);

    if (!columns || !rows) {
      return std::unexpected(columns ? rows.error() : columns.error());
    }

    auto horizontal = axisLayout(options_.column_gap).arrangeChildren(*columns, {content->width, 0});
    auto vertical = axisLayout(options_.row_gap).arrangeChildren(*rows, {content->height, 0});

    if (!horizontal || !vertical) {
      return std::unexpected(horizontal ? vertical.error() : horizontal.error());
    }

    std::vector<CellRect> result(0);

    if (children.size() > result.max_size()) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    result.reserve(children.size());

    for (const auto &child : children) {
      const auto &cell = *placement(options_, child.node);
      auto slot = cellRect(cell, *horizontal, *vertical, options_.padding);

      if (!slot) {
        return std::unexpected(slot.error());
      }

      auto placed = grid_detail::place(child, *slot, cell.horizontal, cell.vertical);

      if (!placed) {
        return std::unexpected(placed.error());
      }

      result.push_back(*placed);
    }

    return std::expected<std::vector<CellRect>, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

ComputedStyle Grid::styleDefaults() const {
  return scroll_ ? detail::scrollDefaults(*scroll_) : Widget::styleDefaults();
}

std::expected<void, TreeError> Grid::paint(PaintContext &context) {
  if (!regionalLayout(options_, scroll_ != nullptr)) {
    return {};
  }

  if (!layout_ || !layout_->arranged) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  class Batch final : public PaintContext {
  public:
    std::vector<RenderCommand> commands = std::vector<RenderCommand>(std::initializer_list<RenderCommand>{});

    std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
      try {
        for (const auto &value : values) {
          commands.push_back(value);
        }

        return {};
      } catch (const std::bad_alloc &) {
        return std::unexpected(TreeError::NO_MEMORY);
      }
    }
  };

  try {
    Batch batch;
    const auto background = [&](CellRect rectangle, const std::optional<CellStyle> &style) {
      if (style && !rectangle.empty()) {
        batch.commands.emplace_back(FillCommand{rectangle, rectangle, *style});
      }
    };

    auto bodyBackground = layout_->bodyClip;
    const bool nonstickyScroll = scroll_ && !options_.sticky_header;

    if (nonstickyScroll) {
      const Wide bodyTop = Wide{layout_->contentOrigin.y} + layout_->headerHeight - layout_->verticalOffset;
      const auto top =
        static_cast<std::int32_t>(std::clamp<Wide>(bodyTop, bodyBackground.origin().y, bodyBackground.bottom()));
      auto clipped = localRect({bodyBackground.origin().x, top},
                               {bodyBackground.size().width, static_cast<std::int32_t>(bodyBackground.bottom() - top)});

      if (!clipped) {
        return std::unexpected(clipped.error());
      }

      bodyBackground = *clipped;
    }

    background(bodyBackground, options_.presentation.body);

    for (std::size_t index = 0; index < layout_->paintedRows.size(); ++index) {
      background(layout_->paintedRows[index],
                 index % 2 == 0 ? options_.presentation.rows.even : options_.presentation.rows.odd);
    }

    const Wide headerTop = Wide{layout_->contentOrigin.y} - (nonstickyScroll ? layout_->verticalOffset : 0);

    if (headerTop < std::numeric_limits<std::int32_t>::min() || headerTop > grid_detail::CELL_LIMIT) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    auto headerRectangle = localRect({layout_->headerClip.origin().x, static_cast<std::int32_t>(headerTop)},
                                     {layout_->headerClip.size().width, layout_->headerHeight});

    if (!headerRectangle) {
      return std::unexpected(headerRectangle.error());
    }

    background(headerRectangle->intersection(layout_->headerClip), options_.presentation.header);

    if (scroll_) {
      auto painted = detail::paintScroll(*scroll_, computedStyle() ? *computedStyle() : styleDefaults(), batch);

      if (!painted) {
        return painted;
      }
    }

    std::uint64_t remaining = options_.presentation.maxPaintCells;

    // Validate the whole batch before the external sink sees even its first background.
    for (const auto &command : batch.commands) {
      const auto work = std::visit(
        [](const auto &value) -> std::uint64_t {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, FillCommand>) {
            const auto clipped = value.area.intersection(value.clip).size();

            return static_cast<std::uint64_t>(clipped.width) * static_cast<std::uint64_t>(clipped.height);
          } else {
            return value.text.graphemes().size();
          }
        },
        command);

      if (work > remaining) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }

      remaining -= work;
    }

    return batch.commands.empty() ? std::expected<void, TreeError>{} : context.submit(batch.commands);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> setGridOptions(WidgetTree &tree, Node<Grid> node, const GridOptions &options) {
  auto checked = validate(options);

  if (!checked) {
    return checked;
  }

  try {
    GridOptions replacement(options);
    std::expected<void, TreeError> status;
    auto result = tree.edit(node, [&](Grid &widget) {
      if (widget.mode_ != GridPlacementMode::EXPLICIT || widget.options_.regions != replacement.regions) {
        status = std::unexpected(TreeError::INVALID_ARGUMENT);

        return;
      }

      swapOptions(widget.options_, replacement);
      widget.layout_.reset();
    });

    if (!result || !status) {
      return result ? status : result;
    }

    return tree.invalidate(node, Invalidation::LAYOUT);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> setDeclarativeGridOptions(WidgetTree &tree, Node<Grid> node,
                                                         const GridOptions &options) {
  auto checked = validate(options, GridPlacementMode::DECLARATIVE);

  if (!checked) {
    return checked;
  }

  try {
    GridOptions replacement(options);
    std::expected<void, TreeError> status;
    auto result = tree.edit(node, [&](Grid &widget) {
      if (widget.mode_ != GridPlacementMode::DECLARATIVE || widget.options_.regions != replacement.regions) {
        status = std::unexpected(TreeError::INVALID_ARGUMENT);

        return;
      }

      swapOptions(widget.options_, replacement);
      widget.layout_.reset();
    });

    if (!result || !status) {
      return result ? status : result;
    }

    return tree.invalidate(node, Invalidation::LAYOUT);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> setGridPresentation(WidgetTree &tree, Node<Grid> node,
                                                   const GridPresentation &presentation) {
  if (presentation.maxPaintCells == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  bool changed{};
  bool needsLayout{};
  auto edited = tree.edit(node, [&](Grid &widget) {
    if (widget.options_.presentation == presentation) {
      return;
    }

    widget.options_.presentation = presentation;
    changed = true;
    needsLayout = regionalLayout(widget.options_, widget.scroll_ != nullptr) && !widget.layout_;
  });

  if (!edited || !changed) {
    return edited;
  }

  return tree.invalidate(node, needsLayout ? Invalidation::LAYOUT : Invalidation::PAINT);
}
} // namespace portals
