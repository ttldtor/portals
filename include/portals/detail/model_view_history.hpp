// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace portals::detail {
/// @cond INTERNAL
struct ModelSelectionPositions {
  ModelSelectionPositions() : positions(0) {
  }

  SourceCredit credit;
  std::vector<ModelBookmark> positions;
};

struct ModelViewCheckpoint {
  std::optional<ModelBookmark> current;
  NavigationSelection selection;
  std::optional<ModelBookmark> reading;
  std::uint64_t first{};
  PositionResolution currentResolution{PositionResolution::EMPTY};
  PositionResolution readingResolution{PositionResolution::EMPTY};
  std::shared_ptr<const ModelSelectionPositions> selectedPositions;
};

struct ModelRestoreChannels {
  bool current{true};
  bool reading{true};
  bool selection{true};
};

inline bool unresolvedPosition(PositionResolution value) noexcept {
  return value != PositionResolution::EXACT && value != PositionResolution::NEAREST &&
         value != PositionResolution::EMPTY;
}

inline bool pendingModelRestoration(const ModelViewCheckpoint &value, ModelRestoreChannels channels) noexcept {
  return (channels.current && unresolvedPosition(value.currentResolution)) ||
         (channels.reading && unresolvedPosition(value.readingResolution)) ||
         (channels.selection && unresolvedPosition(value.selection.resolution()));
}

struct ModelCanonicalRestore {
  SourceCredit credit;
  ModelViewCheckpoint value;
  ModelRestoreChannels channels;
  std::uint64_t currentRevision{};
  std::uint64_t readingRevision{};
  std::uint64_t selectionRevision{};
  std::uint64_t sequence{};
  std::uint64_t publication{};
  std::uint64_t sourceRevision{};
};

struct ModelViewHistoryEntry {
  SourceCredit credit;
  SourceCredit metadataCredit;
  std::weak_ptr<ManagedModelSource> model;
  ModelResource resource;
  bool explicitResource{};
  std::weak_ptr<const void> participant;
  std::uint64_t role{};
  std::uint64_t revision{};
  ModelViewCheckpoint value;
  std::unique_ptr<ModelViewHistoryEntry> next;
};

struct ModelViewHistoryChange {
  SourceCredit metadataCredit;
  std::unique_ptr<ModelViewHistoryEntry> added;
  ModelViewHistoryEntry *existing{};
  std::weak_ptr<ManagedModelSource> model;
  ModelResource resource;
  bool explicitResource{};
  std::weak_ptr<const void> participant;
  std::uint64_t role{};
  std::uint64_t revision{};
  ModelViewCheckpoint value;
  bool replaceMetadata{};
};

class ModelViewHistory {
public:
  static std::expected<std::shared_ptr<ModelViewHistory>, TreeError> create(std::weak_ptr<const void> lifetime,
                                                                            ModelCheckpointOptions options);
  ModelViewHistoryEntry *find(const std::shared_ptr<ManagedModelSource> &model, std::uint64_t role,
                              bool lineageFallback = true) const noexcept;
  std::expected<ModelViewHistoryChange, TreeError> stage(std::shared_ptr<ManagedModelSource> model, std::uint64_t role,
                                                         ModelViewCheckpoint value,
                                                         std::weak_ptr<const void> participant = {});
  bool valid(const ModelViewHistoryChange &change) const noexcept;
  void commit(ModelViewHistoryChange change) noexcept;
  std::expected<std::uint64_t, TreeError> role();
  std::expected<SourceCredit, TreeError> reserveState();
  std::expected<SourceCredit, TreeError> reserveRole();
  std::expected<std::shared_ptr<ModelSelectionPositions>, TreeError> selectedPositions(std::size_t count);
  std::expected<std::shared_ptr<ModelCanonicalRestore>, TreeError> canonicalRestore();
  void prune() noexcept;
  const ModelCheckpointOptions &options() const noexcept;
  std::shared_ptr<ModelCanonicalRestore> pending;

private:
  SourceCredit credit_;
  std::shared_ptr<SourceBudget> budget_;
  std::weak_ptr<const void> lifetime_;
  ModelCheckpointOptions options_;
  std::unique_ptr<ModelViewHistoryEntry> entries_;
  std::uint64_t nextRole_{1};
};

struct ModelViewRestoreState {
  SourceCredit credit;
  std::shared_ptr<ModelViewHistory> history;
  std::shared_ptr<SourceCredit> participant;
  std::weak_ptr<ManagedNavigationRecord> context;
  std::uint64_t currentRevision{};
  std::uint64_t readingRevision{};
  std::uint64_t selectionRevision{};
  ModelRestoreChannels channels;
  bool shareCurrent{};
  bool shareReading{};
  std::uint64_t role{};
  std::optional<ModelViewCheckpoint> pending;
  std::uint64_t attemptedRevision{};
  std::uint64_t attemptedProofRevision{};
  PositionResolution resolution{PositionResolution::EXACT};
  bool attempted{};
};

std::expected<ModelResource, TreeError> modelNavigationResource(ManagedModelSource &source);

struct PreparedModelViewChange {
  virtual ~PreparedModelViewChange() = default;
  virtual std::expected<void, TreeError> validate() const = 0;
  virtual std::expected<void, TreeError> invalidate() const = 0;
  virtual std::expected<void, TreeError> install() = 0;
  virtual void commitHistory() noexcept = 0;
  virtual void retire() noexcept = 0;
};

/// @endcond
} // namespace portals::detail
