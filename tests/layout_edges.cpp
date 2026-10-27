// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/detail/layout_traversal.hpp>
#include <portals/grid.hpp>
#include <portals/layout_snapshot.hpp>
#include <portals/style.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace allocation_test {
thread_local void (*hook)(){};
thread_local bool failOnce{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  if (std::exchange(allocation_test::failOnce, false)) {
    throw std::bad_alloc();
  }

  if (auto hook = std::exchange(allocation_test::hook, nullptr)) {
    hook();
  }

  if (auto *value = std::malloc(size ? size : 1)) {
    return value;
  }

  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  std::free(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  std::free(value);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

CellRect rectangle(CellPoint origin, CellSize size) {
  return take(CellRect::from(origin, size));
}

struct RegionLayout final : Widget, Layout {
  NodeId heading;
  bool invalidClip{};
  bool failPolicy{};
  bool throwPolicy{};
  bool admission{true};

  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override {
    return available;
  }

  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild>) const override {
    return CellSize{8, 6};
  }

  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override {
    try {
      std::vector<CellRect> result(0);

      for (const auto &child : children) {
        result.push_back(child.node == heading ? rectangle({}, {available.width, 1})
                                               : rectangle({0, 1}, {available.width, 5}));
      }

      return std::expected<std::vector<CellRect>, TreeError>(std::in_place, result);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    }
  }

  std::expected<LayoutChildPolicy, TreeError> childLayoutFor(NodeId child, CellSize available) const override {
    if (throwPolicy) {
      throw std::runtime_error("policy callback");
    }

    if (failPolicy) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    if (invalidClip) {
      return LayoutChildPolicy{rectangle({}, {available.width + 1, available.height})};
    }

    if (child == heading) {
      return LayoutChildPolicy{rectangle({}, {available.width, std::min(1, available.height)}),
                               RevealAxes::HORIZONTAL_ONLY};
    }

    const auto height = std::max(0, available.height - 1);

    return LayoutChildPolicy{rectangle({0, std::min(1, available.height)}, {available.width, height}), RevealAxes::BOTH,
                             !admission    ? RegionalInput::UNCHANGED
                             : height == 0 ? RegionalInput::CLOSED
                                           : RegionalInput::OPEN};
  }
};

struct Child final : Widget {
  explicit Child(std::size_t &arranged) : arranged_(arranged) {
  }

  std::expected<void, TreeError> arrange(CellRect) override {
    ++arranged_;
    return {};
  }

private:
  std::size_t &arranged_;
};

struct IntrinsicLeaf final : SizedWidget {
  using SizedWidget::SizedWidget;

  std::expected<CellSize, TreeError> measure(CellSize) override {
    return CellSize{8, 4};
  }
};

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  Node<RegionLayout> region = take(tree->emplaceChecked<RegionLayout>(root));
  std::size_t arranged{};
  Node<Child> heading = take(tree->emplaceChecked<Child>(region, arranged));
  Node<Child> body = take(tree->emplaceChecked<Child>(region, arranged));

  Fixture() {
    take(tree->edit(region, [&](RegionLayout &value) {
      value.heading = heading.id();
    }));
  }

  std::expected<void, TreeError> layout(CellRect bounds, CellRect viewport) {
    std::expected<void, TreeError> result;
    auto frame = tree->frame([&] {
      result = layoutTree(*tree, root, bounds, viewport);
    });
    return frame ? result : frame;
  }
};

struct ReentryProbe {
  WidgetTree *tree{};
  Node<Widget> owner;
  Node<Widget> child;
  CellRect bounds;
  std::array<TreeError, 5> errors{};
};

thread_local ReentryProbe *reentryProbe{};

void tryMutationFromAllocation() {
  auto &probe = *reentryProbe;
  const auto record = [&](std::size_t index, std::expected<void, TreeError> result) {
    probe.errors[index] = result ? TreeError::CALLBACK_FAILED : result.error();
  };

  record(0, probe.tree->remove(probe.owner));
  record(1, probe.tree->setVisible(probe.child, false));
  record(2, probe.tree->invalidate(probe.owner, Invalidation::LAYOUT));
  record(3, probe.tree->arrange(probe.child, probe.bounds, probe.bounds));
  record(4, probe.tree->close());
}
} // namespace

TEST_CASE("root and plain wrappers honor explicit policy while defaults fill the offer") {
  for (const auto wrapped : {false, true}) {
    for (const auto mode : {SizeMode::AUTOMATIC, SizeMode::FLEX, SizeMode::CONTENT, SizeMode::FIXED}) {
      auto scheduler = take(Scheduler::create());
      auto tree = take(WidgetTree::create(*scheduler));
      auto root = take(tree->root());
      auto parent = wrapped ? take(tree->emplaceChecked<Widget>(root)) : root;
      SizePolicy policy;

      policy.width.mode = mode;
      policy.height.mode = mode;

      if (mode == SizeMode::CONTENT) {
        policy.width.maximum = 5;
        policy.height.maximum = 3;
      }

      const auto child = take(tree->emplaceChecked<IntrinsicLeaf>(parent, policy));
      const auto viewport = rectangle({}, {12, 8});
      std::expected<void, TreeError> result;

      take(tree->frame([&] {
        result = layoutTree(*tree, root, viewport, viewport);
      }));
      REQUIRE(result.has_value());

      const auto expected = mode == SizeMode::CONTENT ? CellSize{5, 3}
                            : mode == SizeMode::FIXED ? CellSize{8, 4}
                                                      : CellSize{12, 8};

      CHECK(take(tree->inspect(child)).geometry.local.size() == expected);
      policy.width.minimum = 20;
      policy.width.maximum = 20;

      take(setSizePolicy(*tree, child, policy));
      take(tree->frame([&] {
        result = layoutTree(*tree, root, viewport, viewport);
      }));
      REQUIRE(result.has_value());
      CHECK(take(tree->inspect(child)).geometry.local.size().width == 20);
      CHECK(take(tree->inspect(child)).geometry.clip.size().width == 12);
    }
  }
}

TEST_CASE("PSS minimum is a hard allocation bound across layout solvers") {
  for (const auto kind : {0, 1, 2}) {
    auto scheduler = take(Scheduler::create());
    auto tree = take(WidgetTree::create(*scheduler));
    auto root = take(tree->root());
    Node<Widget> layout;
    Node<Grid> grid;

    if (kind == 0) {
      layout = take(tree->emplaceChecked<Row>(root));
    } else if (kind == 1) {
      layout = take(tree->emplaceChecked<Overlay>(root));
    } else {
      GridOptions options;

      options.columns = {{.preferred = 0, .mode = SizeMode::FLEX}};
      options.rows = {{.preferred = 0, .mode = SizeMode::FLEX}};
      grid = take(tree->emplaceChecked<Grid>(root, options));
      layout = grid;
    }

    const auto child = take(tree->emplaceChecked<Widget>(layout));
    StyleEngine styles(*tree);

    if (kind == 2) {
      GridOptions options;

      options.columns = {{.preferred = 0, .mode = SizeMode::FLEX}};
      options.rows = {{.preferred = 0, .mode = SizeMode::FLEX}};
      options.cells = {{child.id(), 0, 0}};

      take(setGridOptions(*tree, grid, options));
    }

    take(styles.bind(child, {.type = "Probe"}));
    take(styles.replacePss("Probe { min-width: 7; }"));

    const auto viewport = rectangle({}, {3, 2});
    std::expected<void, TreeError> result;

    take(tree->frame([&] {
      result = layoutTree(*tree, root, viewport, viewport);
    }));
    REQUIRE(result.has_value());

    const auto geometry = take(tree->inspect(child)).geometry;

    CHECK(geometry.local.size().width == 7);
    CHECK(geometry.clip.size().width == 3);
  }
}

TEST_CASE("effective style bounds intersect policy and equal policy updates retain geometry") {
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto row = take(tree->emplaceChecked<Row>(root));
  SizePolicy policy;

  policy.width = {2, INTRINSIC_EXTENT, 12, SizeMode::CONTENT};

  auto child = take(tree->emplaceChecked<SizedWidget>(row, policy));
  StyleEngine styles(*tree);

  take(styles.bind(child, {.type = "Probe"}));
  take(styles.replacePss("Probe { min-width: 7; }"));
  take(tree->read(child, [&](const Widget &widget) {
    const auto effective = take(detail::effectiveSizePolicy(widget));

    CHECK(effective.width.minimum == 7);
    CHECK(effective.width.maximum == 12);
    CHECK(effective.width.mode == SizeMode::CONTENT);
  }));

  const auto viewport = rectangle({}, {3, 2});
  std::expected<void, TreeError> result;

  take(tree->frame([&] {
    result = layoutTree(*tree, root, viewport, viewport);
  }));
  REQUIRE(result.has_value());

  const auto before = take(tree->inspect(child)).geometryEpoch;

  CHECK(take(tree->inspect(child)).geometry.local.size().width == 7);

  const auto revision = styles.revision();
  const auto conflictingStyle = styles.replacePss("Probe { min-width: 13; }");

  REQUIRE_FALSE(conflictingStyle.has_value());
  CHECK(conflictingStyle.error().cause == TreeError::INVALID_ARGUMENT);
  CHECK(styles.revision() == revision);
  CHECK(take(tree->inspect(child)).geometryEpoch == before);
  take(tree->read(child, [&](const Widget &widget) {
    CHECK(widget.computedStyle()->minWidth == 7);
  }));

  take(setSizePolicy(*tree, child, policy));
  CHECK(take(tree->inspect(child)).geometryEpoch == before);

  policy.width.maximum = 6;

  CHECK(setSizePolicy(*tree, child, policy) == std::unexpected(TreeError::INVALID_ARGUMENT));
  CHECK(take(tree->inspect(child)).geometryEpoch == before);
  take(tree->read(child, [&](const SizedWidget &widget) {
    CHECK(widget.sizePolicy().width.maximum == 12);
  }));
}

TEST_CASE("specialized edge keeps its owner clip outside the ancestor viewport") {
  Fixture fixture;
  const auto bounds = rectangle({10, 10}, {8, 6});
  const auto viewport = rectangle({}, {5, 5});

  take(fixture.layout(bounds, viewport));
  const auto child = take(fixture.tree->inspect(fixture.body));

  REQUIRE(child.geometry.valid);
  CHECK(child.geometry.clip.empty());
  REQUIRE(child.parentLayoutEdge.has_value());
  CHECK(child.parentLayoutEdge->parent == fixture.region.id());
  CHECK(child.parentLayoutEdge->child == fixture.body.id());
  CHECK(child.parentLayoutEdge->surfaceClip == rectangle({10, 11}, {8, 5}));
  CHECK(child.parentLayoutEdge->geometryEpoch == child.geometryEpoch);
  CHECK(child.parentLayoutEdge->proposedInput == RegionalInput::OPEN);
  CHECK(take(fixture.tree->inspect(fixture.heading)).parentLayoutEdge->revealAxes == RevealAxes::HORIZONTAL_ONLY);

  auto snapshot = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));
  const auto captured = take(snapshot.geometry(fixture.body));
  take(fixture.tree->invalidate(fixture.root, Invalidation::LAYOUT));

  CHECK_FALSE(take(fixture.tree->inspect(fixture.body)).parentLayoutEdge.has_value());
  CHECK_FALSE(take(fixture.tree->inspect(fixture.body)).geometryEpoch.has_value());
  CHECK(captured.parentLayoutEdge->surfaceClip == rectangle({10, 11}, {8, 5}));

  take(fixture.layout(bounds, viewport));
  CHECK(*take(fixture.tree->inspect(fixture.body)).geometryEpoch > *child.geometryEpoch);
  take(fixture.tree->remove(fixture.region));
  CHECK(take(snapshot.geometry(fixture.body)).parentLayoutEdge->child == fixture.body.id());
}

TEST_CASE("all child policies are validated before any descendant is arranged") {
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 6});

  take(fixture.tree->edit(fixture.region, [](RegionLayout &value) {
    value.invalidClip = true;
  }));
  CHECK(fixture.layout(bounds, bounds) == std::unexpected(TreeError::INVALID_GEOMETRY));
  CHECK(fixture.arranged == 0);
  CHECK_FALSE(take(fixture.tree->inspect(fixture.body)).geometry.valid);

  take(fixture.tree->edit(fixture.region, [](RegionLayout &value) {
    value.invalidClip = false;
    value.failPolicy = true;
  }));
  CHECK(fixture.layout(bounds, bounds) == std::unexpected(TreeError::NODE_LIMIT));
  CHECK(fixture.arranged == 0);

  take(fixture.tree->edit(fixture.region, [](RegionLayout &value) {
    value.failPolicy = false;
    value.throwPolicy = true;
  }));
  CHECK(fixture.layout(bounds, bounds) == std::unexpected(TreeError::CALLBACK_FAILED));
  CHECK(fixture.arranged == 0);
}

TEST_CASE("public direct arrange clears specialized policy without reviving an earlier stamp") {
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 6});

  take(fixture.layout(bounds, bounds));
  const auto before = take(fixture.tree->inspect(fixture.body));
  take(fixture.tree->frame([&] {
    take(fixture.tree->arrange(fixture.body, before.geometry.local, bounds));
  }));
  const auto after = take(fixture.tree->inspect(fixture.body));

  CHECK(after.geometry.valid);
  CHECK_FALSE(after.parentLayoutEdge.has_value());
  CHECK(*after.geometryEpoch > *before.geometryEpoch);
}

TEST_CASE("empty body viewport proposes closure without changing user flags") {
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 1});

  take(fixture.layout(bounds, bounds));
  const auto body = take(fixture.tree->inspect(fixture.body));

  CHECK(body.visible);
  CHECK(body.enabled);
  REQUIRE(body.parentLayoutEdge.has_value());
  CHECK(body.parentLayoutEdge->proposedInput == RegionalInput::CLOSED);
  CHECK(body.parentLayoutEdge->surfaceClip.empty());
}

TEST_CASE("regional admission converges independently of fresh geometry epochs") {
  using namespace portals::detail;
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 6});
  const auto apply = [&](bool open) {
    const auto snapshot = take(fixture.tree->inspect(fixture.body));
    const std::array proposals{RegionalInputProposal{fixture.body.id(), open, *snapshot.geometryEpoch}};
    const std::array owners{RegionalInputOwnerProposal{fixture.region, proposals}};
    const auto geometry = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));

    return LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, owners);
  };

  take(fixture.layout(bounds, bounds));
  CHECK(take(apply(true)));
  CHECK(take(fixture.tree->inputEligible(fixture.body)));
  take(fixture.layout(bounds, bounds));
  take(fixture.tree->consumeFrameRequest());
  CHECK_FALSE(take(apply(true)));
  CHECK(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE);

  const auto zeroBody = rectangle({}, {8, 1});

  take(fixture.layout(zeroBody, zeroBody));
  CHECK(take(apply(false)));
  CHECK_FALSE(take(fixture.tree->inputEligible(fixture.body)));
  CHECK(take(fixture.tree->inputEligible(fixture.heading)));
  const auto closed = take(fixture.tree->inspect(fixture.body));

  CHECK(closed.visible);
  CHECK(closed.enabled);
  CHECK_FALSE(closed.regionalInputOpen);

  take(fixture.layout(bounds, bounds));
  CHECK(take(apply(true)));
  CHECK(take(fixture.tree->inputEligible(fixture.body)));
  take(fixture.tree->edit(fixture.region, [](RegionLayout &value) {
    value.admission = false;
  }));
  take(fixture.layout(bounds, bounds));
  const std::array owners{RegionalInputOwnerProposal{fixture.region, {}}};
  const auto geometry = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));

  CHECK(take(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, owners)));
  CHECK(take(fixture.tree->inputEligible(fixture.body)));
}

TEST_CASE("regional publication validates every owner and freshness before changing input") {
  using namespace portals::detail;
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 1});

  take(fixture.layout(bounds, bounds));
  const auto snapshot = take(fixture.tree->inspect(fixture.body));
  const std::array proposals{RegionalInputProposal{fixture.body.id(), false, *snapshot.geometryEpoch}};
  const std::array invalidOwners{RegionalInputOwnerProposal{fixture.region, proposals},
                                 RegionalInputOwnerProposal{fixture.heading, proposals}};
  const auto geometry = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));

  CHECK(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, invalidOwners) ==
        std::unexpected(TreeError::INVALID_GEOMETRY));
  CHECK(take(fixture.tree->inputEligible(fixture.body)));
  take(fixture.layout(bounds, bounds));
  const std::array staleOwners{RegionalInputOwnerProposal{fixture.region, proposals}};
  const auto fresh = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));

  CHECK(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, fresh, staleOwners) ==
        std::unexpected(TreeError::INVALID_GEOMETRY));
  CHECK(take(fixture.tree->inputEligible(fixture.body)));
  take(fixture.tree->frame([&] {
    CHECK(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, fresh, {}) ==
          std::unexpected(TreeError::WOULD_BLOCK));
  }));
}

TEST_CASE("empty desired set cannot clear a closed region using stale or contradictory geometry") {
  using namespace portals::detail;
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 1});

  take(fixture.layout(bounds, bounds));
  const auto geometry = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));
  const auto child = take(fixture.tree->inspect(fixture.body));
  const std::array proposals{RegionalInputProposal{fixture.body.id(), false, *child.geometryEpoch}};
  const std::array owners{RegionalInputOwnerProposal{fixture.region, proposals}};
  const std::array empty{RegionalInputOwnerProposal{fixture.region, {}}};

  CHECK(take(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, owners)));
  CHECK_FALSE(take(fixture.tree->inputEligible(fixture.body)));
  CHECK(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, empty) ==
        std::unexpected(TreeError::WOULD_BLOCK));
  CHECK_FALSE(take(fixture.tree->inputEligible(fixture.body)));

  take(fixture.layout(bounds, bounds));
  const auto fresh = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));

  CHECK(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, fresh, empty) ==
        std::unexpected(TreeError::INVALID_GEOMETRY));
  CHECK_FALSE(take(fixture.tree->inputEligible(fixture.body)));
}

TEST_CASE("allocation reentry cannot retire or mutate records staged for regional publication") {
  using namespace portals::detail;
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 1});

  take(fixture.layout(bounds, bounds));
  const auto geometry = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));
  const auto child = take(fixture.tree->inspect(fixture.body));
  const std::array proposals{RegionalInputProposal{fixture.body.id(), false, *child.geometryEpoch}};
  const std::array owners{RegionalInputOwnerProposal{fixture.region, proposals}};
  ReentryProbe probe{fixture.tree.get(), fixture.region, fixture.body, bounds};

  reentryProbe = &probe;
  allocation_test::hook = tryMutationFromAllocation;
  const auto changed = LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, owners);
  allocation_test::hook = nullptr;
  reentryProbe = nullptr;

  CHECK(take(changed));

  for (const auto error : probe.errors) {
    CHECK(error == TreeError::WOULD_BLOCK);
  }

  CHECK(take(fixture.tree->inspect(fixture.body)).visible);
  CHECK_FALSE(take(fixture.tree->inputEligible(fixture.body)));
}

TEST_CASE("failed regional staging leaves admission and geometry unchanged") {
  using namespace portals::detail;
  Fixture fixture;
  const auto bounds = rectangle({}, {8, 1});

  take(fixture.layout(bounds, bounds));
  const auto geometry = take(LayoutSnapshot::capture(*fixture.tree, fixture.root, LayoutSnapshotProfile::GEOMETRY));
  const auto child = take(fixture.tree->inspect(fixture.body));
  const std::array proposals{RegionalInputProposal{fixture.body.id(), false, *child.geometryEpoch}};
  const std::array owners{RegionalInputOwnerProposal{fixture.region, proposals}};

  allocation_test::failOnce = true;
  const auto changed = LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, owners);
  allocation_test::failOnce = false;

  CHECK(changed == std::unexpected(TreeError::NO_MEMORY));
  CHECK(take(fixture.tree->inputEligible(fixture.body)));
  CHECK(take(fixture.tree->inspect(fixture.body)).geometryEpoch == child.geometryEpoch);
  CHECK(take(LayoutTraversalAccess::adoptRegionalInput(*fixture.tree, geometry, owners)));
}
