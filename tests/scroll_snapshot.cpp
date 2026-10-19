// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/scroll_preparation.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

namespace portals::detail {
// Finite injection for the existing counter's permanent zero state; no production setter exists.
struct ScrollSnapshotTestAccess {
  static void exhaustInput(LayoutSnapshot &snapshot) {
    snapshot.inputGeneration_ = 0;
  }
};
} // namespace portals::detail

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

struct Content : Widget {
  std::expected<CellSize, TreeError> measure(CellSize) override {
    return CellSize{30, 40};
  }
};

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::shared_ptr<ScrollController> controller = take(ScrollController::create());
  Node<ScrollArea> area = take(tree->emplaceChecked<ScrollArea>(root, controller));
  Node<Content> content = take(tree->emplaceChecked<Content>(area));

  Fixture() {
    take(initializeScrollArea(*tree, area));
  }

  LayoutSnapshot capture(CellSize size, LayoutSnapshotProfile profile) {
    std::optional<LayoutSnapshot> result;
    const auto bounds = take(CellRect::from({}, size));
    take(tree->frame([&] {
      take(layoutTree(*tree, root, bounds, bounds));
      result.emplace(take(LayoutSnapshot::capture(*tree, root, profile)));
    }));
    return std::move(*result);
  }
};
} // namespace

int main() {
  for (int mutation = 0; mutation < 7; ++mutation) {
    Fixture stale;
    const auto unrelated = take(stale.tree->emplaceChecked<Widget>(stale.content));
    auto snapshot = stale.capture({10, 6}, LayoutSnapshotProfile::GEOMETRY);
    if (mutation == 0) {
      take(stale.tree->setVisible(stale.area, false));
    }
    if (mutation == 1) {
      take(stale.tree->setEnabled(stale.root, false));
    }
    if (mutation == 2) {
      take(stale.tree->remove(stale.area));
    }
    if (mutation == 3) {
      take(stale.tree->setVisible(stale.area, false));
      take(stale.tree->setVisible(stale.area, true));
    }
    if (mutation == 4) {
      take(stale.tree->setEnabled(stale.area, false));
      take(stale.tree->setEnabled(stale.area, true));
    }
    if (mutation == 5) {
      take(stale.tree->setEnabled(unrelated, false));
    }
    if (mutation == 6) {
      detail::ScrollSnapshotTestAccess::exhaustInput(snapshot);
    }
    const Node<ScrollArea> areas[]{stale.area};
    const auto denied = prepareScrollAreas(*stale.tree, areas, snapshot);
    check(!denied && denied.error() == (mutation == 6 ? TreeError::IDENTITY_EXHAUSTED : TreeError::WOULD_BLOCK),
          "stale eligibility/removal/rapid-toggle/unrelated history rejects checked adoption");
    check(take(stale.controller->snapshot()).vertical.metrics.extent == 0,
          "stale or exhausted admission publishes no partial metrics");
  }
  Fixture fixture;
  auto dimensions = fixture.capture({10, 6}, LayoutSnapshotProfile::DIMENSIONS);
  check(take(dimensions.viewport(fixture.area)) == CellSize{10, 6}, "old dimensions preserved");
  check(!dimensions.geometry(fixture.area) && dimensions.geometry(fixture.area).error() == TreeError::INVALID_ARGUMENT,
        "old profile has no geometry storage");
  auto owning = fixture.capture({10, 6}, LayoutSnapshotProfile::GEOMETRY);
  const auto observed = take(owning.geometry(fixture.area));
  check(observed.visible && observed.scrollArea && observed.geometry.valid, "area owns copied observation");
  check(observed.descendantClip.size() == observed.scrollArea->placement.childClipSize, "child clip copied locally");
  const Node<ScrollArea> nodes[]{fixture.area};
  check(take(prepareScrollAreas(*fixture.tree, nodes, owning)).layoutChanged, "first metrics adopted");
  take(setScrollOffset(*fixture.tree, fixture.area, ScrollAxis::VERTICAL, {20, .5}));
  check(take(owning.geometry(fixture.area)).scrollArea->verticalOffset.raster == 0,
        "capture owns old effective transform after later controller command");
  std::thread reader([&] {
    check(take(owning.geometry(fixture.content)).parent == Node<Widget>(fixture.area), "owning const query on worker");
  });
  reader.join();
  take(fixture.tree->setVisible(fixture.area, false));
  auto hidden = fixture.capture({10, 6}, LayoutSnapshotProfile::GEOMETRY);
  check(!take(hidden.geometry(fixture.content)).visible && !take(hidden.geometry(fixture.area)).geometry.valid,
        "effective hidden differs from visible zero");
  check(take(hidden.viewport(fixture.content)) == CellSize{}, "hidden dimensions stay zero");
  check(!take(prepareScrollAreas(*fixture.tree, nodes, hidden)).layoutChanged, "hidden adoption preserves position");
  const Node<ScrollArea> duplicate[]{fixture.area, fixture.area};
  auto rejected = prepareScrollAreas(*fixture.tree, duplicate, hidden);
  check(!rejected && rejected.error() == TreeError::INVALID_ARGUMENT, "hidden duplicate rejected");
  take(fixture.tree->setVisible(fixture.area, true));
  auto zero = fixture.capture({}, LayoutSnapshotProfile::GEOMETRY);
  check(take(zero.geometry(fixture.area)).visible, "zero allocation is visible");
  take(prepareScrollAreas(*fixture.tree, nodes, zero));
  Fixture foreign;
  check(!owning.geometry(foreign.area) && owning.geometry(foreign.area).error() == TreeError::WRONG_SESSION,
        "snapshot keeps session identity");
  take(fixture.tree->remove(fixture.area));
  check(take(owning.geometry(fixture.area)).visible, "removed live node does not invalidate owning copy");
  auto replacement = take(fixture.tree->emplaceChecked<Widget>(fixture.root));
  check(!owning.geometry(replacement) && owning.geometry(replacement).error() == TreeError::STALE_NODE,
        "reused slot cannot masquerade as captured generation");
  return failures ? 1 : 0;
}
