// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/log_file.hpp>
#include <portals/ui_log_view.hpp>

namespace portals {
class ManagedScene;
struct ManagedError;
} // namespace portals

namespace portals::ui::detail {
struct SceneActivity;

struct LogSourceData {
  std::shared_ptr<WindowProvider<LogLine>> provider;
  std::shared_ptr<FileLogModel> file;
  std::shared_ptr<SourceBudget> budget;
  std::optional<FileLogFailure> failure;
  SourceError error{SourceError::INVALID_ARGUMENT};
  std::optional<std::uint64_t> finiteRows;
  std::uint64_t initialRevision{1};
};

struct ExternalLogAccess {
  static LogSource make(std::shared_ptr<LogSourceData> data, SourceError error = {}) {
    return LogSource(std::move(data), error);
  }

  static const std::shared_ptr<LogSourceData> &data(const LogSource &source) {
    return source.data_;
  }

  static SourceError error(const LogSource &source) {
    return source.error_;
  }
};
} // namespace portals::ui::detail

namespace portals::detail {
struct ExternalLogAnchor {
  RowId row;
  std::size_t firstByte{};
  std::size_t lastByte{};
  std::uint64_t ordinal{};
  std::size_t recordBytes{};
  SourceCursor pageBefore;
  SourceCursor pageAfter;
};

struct ExternalLogFrame {
  // Credit precedes every owned allocation so it is released last.
  SourceCredit credit;
  std::vector<std::unique_ptr<GraphemeText>> rows = std::vector<std::unique_ptr<GraphemeText>>(0);
  std::vector<ExternalLogAnchor> anchors = std::vector<ExternalLogAnchor>(0);
  std::unique_ptr<GraphemeText> footer;
  std::unique_ptr<GraphemeText> alternateFooter;
  bool following{};
  SourceCursor before;
  SourceCursor after;
  SourceEdge edge{SourceEdge::MORE};
  SourceDirection direction{SourceDirection::FORWARD};
  std::size_t records{};
  std::size_t bytes{};
  std::int32_t width{};
  std::optional<std::uint64_t> preferredOffset;
  bool anchorResolved{};
};

struct ExternalLogState {
  std::shared_ptr<const ExternalLogFrame> frame;
  std::shared_ptr<ui::detail::SceneActivity> activity;
  CellSize viewport;
  std::uint64_t offset{};
  std::uint64_t intentSerial{};
  bool navigationChanged{};
  std::optional<std::uint64_t> requestedOffset;
  std::optional<std::uint64_t> positionRequest;
  std::optional<std::uint64_t> byteRequest;
  std::optional<std::uint64_t> finiteRows;
  bool fileSource{};
  std::uint64_t fileGeneration{};
  bool visible{};
  bool focused{};
  double wheelStep{3};
  std::uint64_t keyStep{1};
  std::uint64_t pageOverlap{1};
  bool followTail{};
  bool tailRequested{};
};

std::expected<void, ManagedError> attachExternalLogView(ManagedScene &scene, Node<LogView> node, ui::LogSource source,
                                                        LogViewConfig config);
} // namespace portals::detail
