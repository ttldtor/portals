// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model_position_finite.hpp>

namespace portals::detail {
/// @cond INTERNAL
class ModelViewHistory;
struct ModelViewCheckpoint;
struct PreparedModelViewChange;
struct ModelRestoreChannels;

struct ManagedNavigationRecord {
  ManagedState<NavigationPosition> current;
  ManagedState<NavigationSelection> selection;
  ManagedState<NavigationPosition> reading;
  ManagedScene *owner{};
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
  using Capture =
    std::function<std::expected<ModelBookmark, TreeError>(const std::shared_ptr<ManagedNavigationRecord> &, RowId)>;
  using Synchronize = std::function<std::expected<void, TreeError>(const std::shared_ptr<ManagedNavigationRecord> &)>;
  std::shared_ptr<Capture> capture;
  std::shared_ptr<Synchronize> synchronize;
  std::shared_ptr<ModelViewHistory> history;

  struct Publication {
    std::shared_ptr<Property<NavigationPosition>> currentOwner;
    std::shared_ptr<Property<NavigationSelection>> selectionOwner;
    std::shared_ptr<Property<NavigationPosition>> readingOwner;
    Property<NavigationPosition>::PreparedChange current;
    Property<NavigationSelection>::PreparedChange selection;
    Property<NavigationPosition>::PreparedChange reading;
  };

  std::expected<Publication, TreeError>
  preparePublication(NavigationPosition nextCurrent, NavigationSelection nextSelection, NavigationPosition nextReading);
  std::expected<void, TreeError> validatePublication(const Publication &publication) const;
  std::expected<void, TreeError> invalidatePublication(const Publication &publication) const;
  static void installPublication(const Publication &publication) noexcept;
  static std::optional<SchedulerError> publishPublication(const Publication &publication);

  std::expected<void, TreeError> setPosition(std::optional<ModelBookmark> value, PositionResolution resolution,
                                             bool readingAnchor = false);
  void cancel() noexcept;
  void close() noexcept;
};

struct ManagedNavigationBinding {
  Node<Widget> node;
  std::weak_ptr<ManagedNavigationRecord> context;
  NavigationBindingOptions options;
  using Stage = std::function<std::expected<std::unique_ptr<PreparedModelViewChange>, ManagedError>(
    const std::shared_ptr<ManagedModelSource> &, ModelRebindOptions, const std::shared_ptr<ModelViewHistory> &,
    const ModelViewCheckpoint *, const void *)>;
  Stage stage;
  std::function<void(const ModelViewCheckpoint *, const ModelRestoreChannels *)> installed;
  std::function<std::expected<void, TreeError>()> before;
  std::function<std::expected<void, TreeError>()> after;
};

struct LogCheckpointEntry {
  // Credit is released after all metadata storage owned by this entry.
  SourceCredit credit;
  SourceCredit metadataCredit;
  ModelResource resource;
  std::weak_ptr<const void> lifetime;
  LogReadingCheckpoint value;
  std::unique_ptr<LogCheckpointEntry> next;
};

struct LogCheckpointChange {
  SourceCredit metadataCredit;
  std::unique_ptr<LogCheckpointEntry> added;
  LogCheckpointEntry *existing{};
  ModelResource resource;
  std::weak_ptr<const void> lifetime;
  LogReadingCheckpoint value;
  bool replaceMetadata{};
};

struct LogNavigationSession {
  SourceCredit credit;
  std::shared_ptr<SourceBudget> budget;
  std::weak_ptr<const void> lifetime;
  std::weak_ptr<const void> binding;
  NodeId scene;
  LogCheckpointOptions options;
  LogRestoreStatus status;
  std::unique_ptr<LogCheckpointEntry> entries;
  bool closed{};
  bool implicit{};

  LogCheckpointEntry *find(ModelResource resource) const noexcept;
  void prune() noexcept;
  void reset(ModelResource resource) noexcept;
  void close() noexcept;
  std::expected<LogCheckpointChange, SourceError>
  stage(ModelResource resource, std::weak_ptr<const void> resourceLifetime, LogReadingCheckpoint value);
  void commit(LogCheckpointChange change) noexcept;
};

struct ManagedNavigationRegistry {
  std::vector<std::shared_ptr<ManagedNavigationRecord>> contexts =
    std::vector<std::shared_ptr<ManagedNavigationRecord>>(0);
  std::vector<std::shared_ptr<ManagedNavigationBinding>> bindings =
    std::vector<std::shared_ptr<ManagedNavigationBinding>>(0);
  std::vector<std::shared_ptr<LogNavigationSession>> logSessions =
    std::vector<std::shared_ptr<LogNavigationSession>>(0);
  bool refreshAfterLayout{};
};

TreeError navigationTreeError(SourceError error) noexcept;
/// @endcond
} // namespace portals::detail
