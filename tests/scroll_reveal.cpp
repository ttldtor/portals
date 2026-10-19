// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/scroll_preparation_state.hpp"
#include <portals/scroll_preparation.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {
using namespace portals;
int failures{};

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E> T take(std::expected<T, E> value) {
  if (!value) {
    std::cerr << "fixture failure\n";
    std::abort();
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Plane : Widget, Layout {
  int row;
  int height;
  bool clipEmpty{};

  Plane(int row, int height) : row(row), height(height) {
  }

  std::expected<CellSize, TreeError> childAvailable(CellSize) const override {
    return CellSize{100, 100};
  }

  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild>) const override {
    return CellSize{100, 100};
  }

  std::expected<CellRect, TreeError> childClip(CellSize size) const override {
    return CellRect::from({}, clipEmpty ? CellSize{} : size).transform_error([](auto) {
      return TreeError::INVALID_GEOMETRY;
    });
  }

  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize) const override {
    return std::vector<CellRect>(children.size(), take(CellRect::from({0, row}, {10, height})));
  }
};

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::vector<Node<ScrollArea>> areas;
  std::vector<std::shared_ptr<ScrollController>> controllers;
  Node<Widget> target;
  Node<Plane> lastPlane;

  explicit Fixture(std::size_t depth, bool reverse) {
    auto parent = root;
    ScrollAreaOptions options;
    options.horizontal = options.vertical = ScrollBarPolicy::NEVER;
    for (std::size_t level = 0; level < depth; ++level) {
      auto controller = take(ScrollController::create());
      auto area = take(tree->emplaceChecked<ScrollArea>(parent, controller, WidthPolicy{}, options));
      lastPlane = take(tree->emplaceChecked<Plane>(area, level + 1 == depth ? 95 : 50, level + 1 == depth ? 1 : 10));
      controllers.push_back(std::move(controller));
      areas.push_back(area);
      parent = lastPlane;
    }
    target = take(tree->emplaceChecked<Widget>(parent));
    if (reverse) {
      std::ranges::reverse(areas);
    }
    for (const auto area : areas) {
      take(initializeScrollArea(*tree, area));
    }
  }

  LayoutSnapshot capture() {
    std::optional<LayoutSnapshot> result;
    const auto bounds = take(CellRect::from({}, {10, 10}));
    take(tree->frame([&] {
      take(layoutTree(*tree, root, bounds, bounds));
      result.emplace(take(LayoutSnapshot::capture(*tree, root, LayoutSnapshotProfile::GEOMETRY)));
    }));
    return std::move(*result);
  }
};
} // namespace

int main() {
  for (const auto depth : {std::size_t{3}, std::size_t{4}, std::size_t{16}, std::size_t{32}, std::size_t{63}}) {
    for (const bool reverse : {false, true}) {
      Fixture fixture(depth, reverse);
      auto first = fixture.capture();
      check(take(first.geometry(fixture.target)).geometry.clip.empty(), "offviewport target has empty cumulative clip");
      auto changed =
        take(prepareScrollAreas(*fixture.tree, fixture.areas, first, ScrollRevealTarget{fixture.target, fixture.root}));
      check(changed.layoutChanged && changed.revealMoved, "first pass adopts reveal");
      for (std::size_t level = 0; level < depth; ++level) {
        check(take(fixture.controllers[level]->snapshot()).vertical.offset.raster == (level + 1 == depth ? 86u : 50u),
              "all nested offsets solved in one pass independent of registration order");
      }
      auto second = fixture.capture();
      check(!take(second.geometry(fixture.target)).geometry.clip.empty(), "target becomes visible at every depth");
      auto stable = take(
        prepareScrollAreas(*fixture.tree, fixture.areas, second, ScrollRevealTarget{fixture.target, fixture.root}));
      check(!stable.layoutChanged && !stable.revealMoved,
            "second pass stable without raising default three-pass budget");
    }
  }
  Fixture clipped(3, false);
  take(clipped.tree->edit(clipped.lastPlane, [](Plane &plane) {
    plane.clipEmpty = true;
  }));
  const auto clippedResult = take(prepareScrollAreas(*clipped.tree, clipped.areas, clipped.capture(),
                                                     ScrollRevealTarget{clipped.target, clipped.root}));
  check(!clippedResult.revealMoved, "permanent intermediate clip does not induce ancestor reveal");

  Fixture refreshed(1, false);
  const auto snapshot = refreshed.capture();
  auto plan = take(detail::planScrollAreas(*refreshed.tree, refreshed.areas, snapshot,
                                           ScrollRevealTarget{refreshed.target, refreshed.root}));
  take(prepareScrollAreas(*refreshed.tree, refreshed.areas, snapshot));
  take(setScrollOffset(*refreshed.tree, refreshed.areas.front(), ScrollAxis::VERTICAL, {20, .5}));
  take(detail::refreshScrollPlans(*refreshed.tree, *plan, false));
  take(detail::adoptScrollAreas(*refreshed.tree, *plan));
  const auto position = take(refreshed.controllers.front()->snapshot()).vertical.offset;
  check(position.raster == 20 && position.fraction == .5, "post-allocation refresh preserves newer canonical command");
  return failures ? 1 : 0;
}
