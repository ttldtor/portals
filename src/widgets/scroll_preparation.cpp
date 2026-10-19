// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scroll_controls_state.hpp"
#include "scroll_preparation_state.hpp"
#include <portals/scroll_preparation.hpp>

#include <limits>

namespace portals {
namespace detail {
using detail::ScrollControllerAccess;

struct AreaPlan {
  Node<ScrollArea> node;
  ScrollAreaLayoutSnapshot layout;
  detail::ScrollControllerPlan values;
  ScrollControllerSnapshot before;
};

struct ScrollPreparationPlan {
  explicit ScrollPreparationPlan(LayoutSnapshot snapshot) : snapshot(std::move(snapshot)) {
  }

  LayoutSnapshot snapshot;
  std::vector<AreaPlan> areas = std::vector<AreaPlan>(0);
  std::vector<std::pair<Node<Widget>, LayoutGeometrySnapshot>> chain =
    std::vector<std::pair<Node<Widget>, LayoutGeometrySnapshot>>(0);
  std::optional<LayoutGeometrySnapshot> target;
  ScrollPreparationResult result;
};

namespace {

bool sameMetrics(ScrollAxisMetrics left, ScrollAxisMetrics right) {
  return left.extent == right.extent && left.viewport == right.viewport;
}

bool logicalChange(const AreaPlan &plan) {
  return plan.before.horizontal.offset.raster != plan.values.horizontal.rasterOffset() ||
         plan.before.horizontal.offset.fraction != plan.values.horizontal.fraction() ||
         plan.before.vertical.offset.raster != plan.values.vertical.rasterOffset() ||
         plan.before.vertical.offset.fraction != plan.values.vertical.fraction();
}

bool metricsChange(const AreaPlan &plan) {
  return !sameMetrics(plan.before.horizontal.metrics, plan.values.horizontalMetrics) ||
         !sameMetrics(plan.before.vertical.metrics, plan.values.verticalMetrics);
}

std::expected<std::int64_t, TreeError> add(std::int64_t value, std::int64_t delta) {
  if ((delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta)) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  return value + delta;
}

struct WideRect {
  std::int64_t left{}, top{}, right{}, bottom{};

  explicit WideRect(CellRect value)
      : left(value.origin().x), top(value.origin().y), right(value.right()), bottom(value.bottom()) {
  }

  bool empty() const {
    return left == right || top == bottom;
  }

  void intersect(CellRect clip) {
    left = std::max<std::int64_t>(left, clip.origin().x);
    top = std::max<std::int64_t>(top, clip.origin().y);
    right = std::max(left, std::min<std::int64_t>(right, clip.right()));
    bottom = std::max(top, std::min<std::int64_t>(bottom, clip.bottom()));
  }

  std::expected<void, TreeError> translate(std::int64_t x, std::int64_t y) {
    auto nextLeft = add(left, x);
    auto nextRight = add(right, x);
    auto nextTop = add(top, y);
    auto nextBottom = add(bottom, y);
    if (!nextLeft || !nextRight || !nextTop || !nextBottom) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    left = *nextLeft;
    right = *nextRight;
    top = *nextTop;
    bottom = *nextBottom;
    return {};
  }
};

std::expected<CellRect, TreeError> viewport(const ScrollAreaLayoutSnapshot &layout) {
  auto local = CellRect::from(layout.placement.viewportOrigin, layout.placement.viewport);
  if (!local) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  auto surface = local->translated(layout.geometry.surface.origin());
  return surface ? std::expected<CellRect, TreeError>(*surface) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

std::expected<std::uint64_t, TreeError> revealAxis(std::int64_t first, std::int64_t end, std::int32_t start,
                                                   std::int32_t length, std::uint64_t oldOffset,
                                                   const ScrollState &current, ScrollAxisMetrics metrics) {
  if (oldOffset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      scrollRange(metrics).maximum > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  auto contentFirst = add(first, -static_cast<std::int64_t>(start));
  auto contentEnd = add(end, -static_cast<std::int64_t>(start));
  if (!contentFirst || !contentEnd) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  contentFirst = add(*contentFirst, static_cast<std::int64_t>(oldOffset));
  contentEnd = add(*contentEnd, static_cast<std::int64_t>(oldOffset));
  if (!contentFirst || !contentEnd) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  auto intended = static_cast<std::int64_t>(current.rasterOffset());
  auto visibleEnd = add(intended, length);
  if (!visibleEnd) {
    return std::unexpected(visibleEnd.error());
  }
  if (end - first > length || *contentFirst < intended) {
    intended = *contentFirst;
  } else if (*contentEnd > *visibleEnd) {
    auto trailing = add(*contentEnd, -static_cast<std::int64_t>(length));
    if (!trailing) {
      return std::unexpected(trailing.error());
    }
    intended = *trailing;
  }
  return static_cast<std::uint64_t>(
    std::clamp<std::int64_t>(intended, 0, static_cast<std::int64_t>(scrollRange(metrics).maximum)));
}

AreaPlan *findPlan(std::vector<AreaPlan> &plans, Node<Widget> node) {
  auto found = std::ranges::lower_bound(plans, node.id().slot, {}, [](const AreaPlan &value) {
    return value.node.id().slot;
  });
  return found != plans.end() && found->node.id() == node.id() ? &*found : nullptr;
}

std::expected<void, TreeError> copyRevealChain(ScrollPreparationPlan &plan, const LayoutSnapshot &snapshot,
                                               ScrollRevealTarget request) {
  auto target = snapshot.geometry(request.target);
  if (!target) {
    return std::unexpected(target.error());
  }
  if (request.root.id().session && request.root.id().session != request.target.id().session) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }
  plan.target = *target;
  bool reached = request.root == Node<Widget>{} || request.root == request.target;
  auto current = request.root == request.target ? Node<Widget>{} : target->parent;
  while (current != Node<Widget>{}) {
    auto ancestor = snapshot.geometry(current);
    if (!ancestor) {
      if (ancestor.error() == TreeError::STALE_NODE && request.root == Node<Widget>{}) {
        break;
      }
      return std::unexpected(ancestor.error());
    }
    plan.chain.emplace_back(current, *ancestor);
    if (current == request.root) {
      reached = true;
      break;
    }
    current = ancestor->parent;
  }
  if (!reached) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  return {};
}

std::expected<bool, TreeError> planReveal(ScrollPreparationPlan &plan) {
  const auto &target = plan.target;
  auto &plans = plan.areas;
  if (!target || !target->visible || !target->geometry.valid || target->geometry.surface.empty()) {
    return false;
  }
  for (const auto &[node, ancestor] : plan.chain) {
    if (!ancestor.visible || (ancestor.scrollArea && (ancestor.scrollArea->placement.viewport.width == 0 ||
                                                      ancestor.scrollArea->placement.viewport.height == 0))) {
      return false;
    }
    if (const auto *plan = findPlan(plans, node);
        plan && (plan->layout.placement.viewport.width == 0 || plan->layout.placement.viewport.height == 0)) {
      return false;
    }
  }
  WideRect rectangle(target->geometry.surface);
  bool moved{};
  for (const auto &[node, ancestor] : plan.chain) {
    if (auto *plan = findPlan(plans, node)) {
      auto bounds = viewport(plan->layout);
      if (!bounds) {
        return std::unexpected(bounds.error());
      }
      auto horizontal =
        revealAxis(rectangle.left, rectangle.right, bounds->origin().x, bounds->size().width,
                   plan->layout.horizontalOffset.raster, plan->values.horizontal, plan->values.horizontalMetrics);
      auto vertical =
        revealAxis(rectangle.top, rectangle.bottom, bounds->origin().y, bounds->size().height,
                   plan->layout.verticalOffset.raster, plan->values.vertical, plan->values.verticalMetrics);
      if (!horizontal || !vertical) {
        return std::unexpected(horizontal ? vertical.error() : horizontal.error());
      }
      if (*horizontal != plan->values.horizontal.rasterOffset()) {
        auto changed = ScrollControllerAccess::offset(plan->values, ScrollAxis::HORIZONTAL, {*horizontal, 0});
        if (!changed) {
          return std::unexpected(changed.error());
        }
        moved = true;
      }
      if (*vertical != plan->values.vertical.rasterOffset()) {
        auto changed = ScrollControllerAccess::offset(plan->values, ScrollAxis::VERTICAL, {*vertical, 0});
        if (!changed) {
          return std::unexpected(changed.error());
        }
        moved = true;
      }
      auto translated = rectangle.translate(
        static_cast<std::int64_t>(plan->layout.horizontalOffset.raster) - static_cast<std::int64_t>(*horizontal),
        static_cast<std::int64_t>(plan->layout.verticalOffset.raster) - static_cast<std::int64_t>(*vertical));
      if (!translated) {
        return std::unexpected(translated.error());
      }
      rectangle.intersect(*bounds);
    } else if (ancestor.scrollArea) {
      auto bounds = viewport(*ancestor.scrollArea);
      if (!bounds) {
        return std::unexpected(bounds.error());
      }
      rectangle.intersect(*bounds);
    } else {
      rectangle.intersect(ancestor.descendantClip);
    }
    if (rectangle.empty()) {
      break;
    }
  }
  return moved;
}
} // namespace

std::expected<std::shared_ptr<ScrollPreparationPlan>, TreeError>
planScrollAreas(WidgetTree &tree, std::span<const Node<ScrollArea>> areas, const LayoutSnapshot &snapshot,
                std::optional<ScrollRevealTarget> reveal) {
  auto stamp = snapshot.geometryRevision();
  if (!stamp) {
    return std::unexpected(stamp.error());
  }
  try {
    auto owned = std::make_shared<ScrollPreparationPlan>(snapshot);
    auto &plans = owned->areas;
    plans.reserve(areas.size());
    for (const auto node : areas) {
      auto geometry = snapshot.geometry(node);
      if (!geometry) {
        return std::unexpected(geometry.error());
      }
      if (!geometry->scrollArea) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      const auto &layout = *geometry->scrollArea;
      plans.push_back({node, layout, {}, {}});
    }
    std::ranges::sort(plans, {}, [](const AreaPlan &plan) {
      return plan.node.id().slot;
    });
    for (std::size_t i = 1; i < plans.size(); ++i) {
      if (plans[i - 1].node == plans[i].node) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    if (reveal) {
      auto copied = copyRevealChain(*owned, snapshot, *reveal);
      if (!copied) {
        return std::unexpected(copied.error());
      }
    }
    return owned;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<ScrollSnapshotAdmission, TreeError> admitScrollPlan(WidgetTree &tree, const ScrollPreparationPlan &plan) {
  return ScrollSnapshotAccess::admit(tree, plan.snapshot);
}

std::expected<ScrollSnapshotAdmission, TreeError> refreshScrollPlans(WidgetTree &tree, ScrollPreparationPlan &owned,
                                                                     bool reveal) {
  auto admitted = admitScrollPlan(tree, owned);
  if (!admitted || *admitted == ScrollSnapshotAdmission::STALE) {
    return admitted;
  }
  owned.result = {};
  for (auto &plan : owned.areas) {
    std::expected<ScrollControllerPlan, TreeError> staged{std::unexpected(TreeError::INVALID_ARGUMENT)};
    auto read = tree.read(plan.node, [&](const ScrollArea &widget) {
      const auto &state = ScrollWidgetAccess::state(widget);
      if (!state.lease || state.retired) {
        return;
      }
      if (state.layoutRevision != plan.layout.layoutRevision) {
        staged = std::unexpected(TreeError::WOULD_BLOCK);
        return;
      }
      staged = ScrollControllerAccess::stage(*state.lease);
      if (staged && plan.layout.visible) {
        auto metrics = ScrollControllerAccess::metrics(
          *state.lease, *staged,
          {plan.layout.extent.width, static_cast<std::uint64_t>(plan.layout.placement.viewport.width)},
          {plan.layout.extent.height, static_cast<std::uint64_t>(plan.layout.placement.viewport.height)});
        if (!metrics) {
          staged = std::unexpected(metrics.error());
        }
      }
    });
    if (!read || !staged) {
      return std::unexpected(read ? staged.error() : read.error());
    }
    auto before = staged->owner->snapshot();
    if (!before) {
      return std::unexpected(before.error());
    }
    plan.values = std::move(*staged);
    plan.before = *before;
  }
  if (reveal) {
    auto planned = planReveal(owned);
    if (!planned) {
      return std::unexpected(planned.error());
    }
    owned.result.revealMoved = *planned;
  }
  return ScrollSnapshotAdmission::CURRENT;
}

std::expected<ScrollPreparationAdoption, TreeError> adoptScrollAreas(WidgetTree &tree, ScrollPreparationPlan &owned) {
  auto admitted = admitScrollPlan(tree, owned);
  if (!admitted) {
    return std::unexpected(admitted.error());
  }
  if (*admitted == ScrollSnapshotAdmission::STALE) {
    return ScrollPreparationAdoption{*admitted, {}};
  }
  // Every fallible allocation is complete. Validate all capabilities/revisions before publishing any plan.
  for (const auto &plan : owned.areas) {
    std::expected<void, TreeError> valid;
    auto read = tree.read(plan.node, [&](const ScrollArea &widget) {
      const auto &state = detail::ScrollWidgetAccess::state(widget);
      if (!state.lease || state.retired) {
        valid = std::unexpected(TreeError::STALE_NODE);
        return;
      }
      auto current = ScrollControllerAccess::stage(*state.lease);
      if (!current) {
        valid = std::unexpected(current.error());
        return;
      }
      if (current->revision != plan.values.revision || state.layoutRevision != plan.layout.layoutRevision) {
        valid = std::unexpected(TreeError::WOULD_BLOCK);
        return;
      }
      if ((logicalChange(plan) || metricsChange(plan)) &&
          current->revision == std::numeric_limits<std::uint64_t>::max()) {
        valid = std::unexpected(TreeError::IDENTITY_EXHAUSTED);
      }
    });
    if (!read || !valid) {
      return std::unexpected(read ? valid.error() : read.error());
    }
  }
  for (auto &plan : owned.areas) {
    if (!plan.layout.visible) {
      continue;
    }
    std::expected<detail::ScrollControllerChange, TreeError> changed{std::unexpected(TreeError::STALE_NODE)};
    auto edited = tree.edit(plan.node, [&](ScrollArea &widget) {
      auto &state = detail::ScrollWidgetAccess::state(widget);
      const auto &placement = plan.layout.placement;
      const bool trackChanged = !state.adoptedGeometry || !state.adoptedSurface ||
                                *state.adoptedSurface != plan.layout.geometry.surface.origin() ||
                                state.adoptedGeometry->horizontal.origin != placement.horizontal.origin ||
                                state.adoptedGeometry->horizontal.length != placement.horizontal.length ||
                                state.adoptedGeometry->vertical.origin != placement.vertical.origin ||
                                state.adoptedGeometry->vertical.length != placement.vertical.length;
      if (trackChanged) {
        state.capture.disconnect();
      }
      changed =
        detail::commitScroll(tree, plan.node, state, std::move(plan.values), detail::ScrollPublication::PREPARATION);
      if (changed) {
        state.adoptedGeometry = placement;
        state.adoptedSurface = plan.layout.geometry.surface.origin();
      }
    });
    if (!edited || !changed) {
      return std::unexpected(edited ? changed.error() : edited.error());
    }
    owned.result.layoutChanged |= changed->metricsChanged || changed->rasterChanged;
  }
  return ScrollPreparationAdoption{ScrollSnapshotAdmission::CURRENT, owned.result};
}
} // namespace detail

std::expected<ScrollPreparationResult, TreeError> prepareScrollAreas(WidgetTree &tree,
                                                                     std::span<const Node<ScrollArea>> areas,
                                                                     const LayoutSnapshot &snapshot,
                                                                     std::optional<ScrollRevealTarget> reveal) {
  auto plan = detail::planScrollAreas(tree, areas, snapshot, reveal);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  auto refreshed = detail::refreshScrollPlans(tree, **plan, true);
  if (!refreshed) {
    return std::unexpected(refreshed.error());
  }
  if (*refreshed == detail::ScrollSnapshotAdmission::STALE) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  auto adopted = detail::adoptScrollAreas(tree, **plan);
  if (!adopted) {
    return std::unexpected(adopted.error());
  }
  if (adopted->admission == detail::ScrollSnapshotAdmission::STALE) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  return adopted->result;
}
} // namespace portals
