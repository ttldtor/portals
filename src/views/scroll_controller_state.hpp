// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/scroll.hpp>
#include <portals/scroll_controller.hpp>
#include <thread>

namespace portals::detail {
struct ScrollGeometryWriter {};

// Association owns values only. Each mounted projection pins it; the controller observes it weakly.
struct ScrollMountSession {
  explicit ScrollMountSession(std::uint64_t identity) : session(identity) {
  }

  std::uint64_t session;
  std::weak_ptr<ScrollGeometryWriter> writer;
};

// Internal mount owner. Move-only, no callbacks on release, and no borrowed control/tree addresses.
struct ScrollControllerLease {
  ScrollControllerLease(std::shared_ptr<ScrollController> owner, std::shared_ptr<ScrollMountSession> session,
                        std::shared_ptr<ScrollGeometryWriter> writer)
      : owner(std::move(owner)), session(std::move(session)), writer(std::move(writer)) {
  }

  ScrollControllerLease(ScrollControllerLease &&) noexcept = default;
  ScrollControllerLease &operator=(ScrollControllerLease &&) noexcept = default;
  ScrollControllerLease(const ScrollControllerLease &) = delete;
  ScrollControllerLease &operator=(const ScrollControllerLease &) = delete;

  std::shared_ptr<ScrollController> owner;
  std::shared_ptr<ScrollMountSession> session;
  std::shared_ptr<ScrollGeometryWriter> writer;
};

struct ScrollControllerChange {
  bool logicalChanged{};
  bool rasterChanged{};
  bool metricsChanged{};
};

// A plan pins its value owner but grants no mount association/writer lease. A stale plan cannot commit.
struct ScrollControllerPlan {
  std::shared_ptr<ScrollController> owner;
  std::weak_ptr<ScrollMountSession> association;
  std::weak_ptr<ScrollGeometryWriter> writer;
  std::uint64_t revision{};
  ScrollState horizontal;
  ScrollState vertical;
  ScrollAxisMetrics horizontalMetrics;
  ScrollAxisMetrics verticalMetrics;
};

// Private arithmetic boundary for future checked node commands. Callers validate their live node/tree
// before staging, then commit and request the corresponding existing tree invalidation. Nothing here
// is a public setter or a widget/router integration placeholder.
struct ScrollControllerAccess {
  static std::expected<ScrollControllerLease, TreeError> acquire(std::shared_ptr<ScrollController> owner,
                                                                 std::uint64_t session, bool geometryWriter);
  static std::expected<ScrollControllerPlan, TreeError> stage(const ScrollControllerLease &lease);
  static std::expected<void, TreeError> metrics(const ScrollControllerLease &lease, ScrollControllerPlan &plan,
                                                ScrollAxisMetrics horizontal, ScrollAxisMetrics vertical);
  static std::expected<void, TreeError> offset(ScrollControllerPlan &plan, ScrollAxis axis, ScrollOffset offset);
  static std::expected<void, TreeError> move(ScrollControllerPlan &plan, ScrollAxis axis, double cells);
  static std::expected<void, TreeError> step(ScrollControllerPlan &plan, ScrollAxis axis, std::uint64_t cells,
                                             bool towardEnd);
  static std::expected<ScrollControllerChange, TreeError> commit(const ScrollControllerLease &lease,
                                                                 ScrollControllerPlan plan);
};
} // namespace portals::detail

namespace portals {
struct ScrollController::State {
  const std::thread::id owner{std::this_thread::get_id()};
  ScrollState horizontal;
  ScrollState vertical;
  ScrollAxisMetrics horizontalMetrics;
  ScrollAxisMetrics verticalMetrics;
  std::uint64_t revision{1};
  std::weak_ptr<detail::ScrollMountSession> association;
};
} // namespace portals
