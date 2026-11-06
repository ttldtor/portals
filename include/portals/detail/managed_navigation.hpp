// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model_position_finite.hpp>

namespace portals::detail {
/// @cond INTERNAL
struct ManagedNavigationRecord {
  ManagedState<NavigationPosition> current;
  ManagedState<NavigationSelection> selection;
  ManagedState<NavigationPosition> reading;
  std::weak_ptr<const void> lifetime;
  std::weak_ptr<ManagedModelSource> model;
  NodeId scene;
  ModelResource resource;
  SourceStamp session;
  NavigationOptions options;
  std::optional<ModelBookmark> bookmark;
  std::optional<ModelBookmark> readingBookmark;
  PositionChangeMap changes;
  std::optional<PositionRequest> request;
  std::stop_source cancellation{std::nostopstate};
  std::uint64_t sequence{};
  std::uint64_t operation{};
  std::uint64_t frameRevision{};
  bool finite{};
  bool closed{};
  using Capture = std::function<std::expected<ModelBookmark, TreeError>(RowId)>;
  using Synchronize = std::function<std::expected<void, TreeError>(const std::shared_ptr<ManagedNavigationRecord> &)>;
  std::shared_ptr<Capture> capture;
  std::shared_ptr<Synchronize> synchronize;

  std::expected<void, TreeError> setPosition(std::optional<ModelBookmark> value, PositionResolution resolution,
                                             bool readingAnchor = false);
  void cancel() noexcept;
  void close() noexcept;
};

struct ManagedNavigationBinding {
  Node<Widget> node;
  std::function<std::expected<void, TreeError>()> before;
  std::function<std::expected<void, TreeError>()> after;
};

struct ManagedNavigationRegistry {
  std::vector<std::shared_ptr<ManagedNavigationRecord>> contexts =
    std::vector<std::shared_ptr<ManagedNavigationRecord>>(0);
  std::vector<std::shared_ptr<ManagedNavigationBinding>> bindings =
    std::vector<std::shared_ptr<ManagedNavigationBinding>>(0);
  bool refreshAfterLayout{};
};

TreeError navigationTreeError(SourceError error) noexcept;
/// @endcond
} // namespace portals::detail
