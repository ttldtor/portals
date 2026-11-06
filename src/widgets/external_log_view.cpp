// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "external_log_view.hpp"
#include "log_view_state.hpp"

#include <portals/ui.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace portals::ui {
std::shared_ptr<FileLogModel> LogSource::fileModel() const noexcept {
  return data_ ? data_->file : nullptr;
}

std::optional<std::uint64_t> LogSource::rowCount() const noexcept {
  return data_ ? data_->finiteRows : std::nullopt;
}

std::shared_ptr<SourceBudget> LogSource::budget() const noexcept {
  return data_ ? data_->budget : nullptr;
}

std::shared_ptr<LogBookmarkProvider> LogSource::bookmarks() const noexcept {
  return data_ ? data_->bookmarks : nullptr;
}

LogSource logSource(std::shared_ptr<WindowProvider<LogLine>> provider, std::shared_ptr<SourceBudget> budget) {
  auto bookmarks = std::dynamic_pointer_cast<LogBookmarkProvider>(provider);

  return logSource(std::move(provider), std::move(bookmarks), std::move(budget));
}

LogSource logSource(std::shared_ptr<WindowProvider<LogLine>> provider, std::shared_ptr<LogBookmarkProvider> bookmarks,
                    std::shared_ptr<SourceBudget> budget) {
  try {
    auto data = std::make_shared<detail::LogSourceData>();

    data->provider = std::move(provider);
    data->file = std::dynamic_pointer_cast<FileLogModel>(data->provider);
    data->bookmarks = std::move(bookmarks);
    data->budget = std::move(budget);

    if (data->bookmarks) {
      data->resource = data->bookmarks->resource();
    } else {
      auto resource = ModelResource::create();

      if (!resource) {
        return detail::ExternalLogAccess::make({}, resource.error());
      }

      data->resource = *resource;
    }

    return detail::ExternalLogAccess::make(std::move(data));
  } catch (const std::bad_alloc &) {
    return detail::ExternalLogAccess::make({}, SourceError::NO_MEMORY);
  }
}

LogSource logFile(std::string_view path) {
  return logFile(path, FileLogOptions{});
}

LogSource logFile(std::string_view path, const FileLogOptions &options, std::shared_ptr<SourceBudget> budget) {
  try {
    auto data = std::make_shared<detail::LogSourceData>();

    data->budget = std::move(budget);

    if (path.empty()) {
      data->failure = FileLogFailure{FileLogError::INVALID_ARGUMENT, 0, FileLogOperation::CREATE};

      return detail::ExternalLogAccess::make(std::move(data));
    }

    auto file = FileLogModel::create(
      std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size())), options);

    if (file) {
      data->file = *file;
      data->bookmarks = *file;
      data->resource = (*file)->resource();
      data->provider = std::move(*file);
    } else {
      data->failure = file.error();
      data->error =
        file.error().error == FileLogError::NO_MEMORY ? SourceError::NO_MEMORY : SourceError::INVALID_ARGUMENT;
    }

    return detail::ExternalLogAccess::make(std::move(data));
  } catch (const std::bad_alloc &) {
    return detail::ExternalLogAccess::make({}, SourceError::NO_MEMORY);
  } catch (const std::filesystem::filesystem_error &error) {
    try {
      auto data = std::make_shared<detail::LogSourceData>();

      data->failure = FileLogFailure{FileLogError::INVALID_ARGUMENT, static_cast<std::uint32_t>(error.code().value()),
                                     FileLogOperation::CREATE};

      return detail::ExternalLogAccess::make(std::move(data));
    } catch (const std::bad_alloc &) {
      return detail::ExternalLogAccess::make({}, SourceError::NO_MEMORY);
    }
  }
}

namespace detail {
LogSource failedLogSource(SourceError error) {
  return ExternalLogAccess::make({}, error);
}

LogSource finiteLogSource(std::shared_ptr<WindowProvider<LogLine>> provider, std::uint64_t rows, std::uint64_t revision,
                          std::shared_ptr<SourceBudget> budget) {
  auto source = logSource(std::move(provider), std::move(budget));
  const auto &data = ExternalLogAccess::data(source);

  if (data) {
    data->finiteRows = rows;
    data->initialRevision = revision;
  }

  return source;
}
} // namespace detail
} // namespace portals::ui

namespace portals::detail {
namespace {
using ui::detail::SceneAccess;
constexpr std::size_t OVERSCAN_ROWS = 8;
constexpr std::size_t PREPARED_METADATA_BYTES = 16 * 1024;
constexpr std::size_t GRAPHEME_ALLOCATION_FACTOR = 5;
constexpr SchedulerTime TAIL_POLL_INTERVAL = 250;
constexpr std::size_t RESTORE_ENVELOPE_BYTES = 4096;

TreeError sourceBridge(SourceError error) noexcept {
  switch (error) {
  case SourceError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case SourceError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  case SourceError::CLOSED:
    return TreeError::CLOSED;
  case SourceError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case SourceError::IDENTITY_EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case SourceError::INVALID_ARGUMENT:
    return TreeError::INVALID_ARGUMENT;
  default:
    return TreeError::CALLBACK_FAILED;
  }
}

ManagedError sourceFailure(Node<LogView> node, SourceError error) {
  ManagedError failure{ManagedPhase::PREPARE, node.id(), sourceBridge(error), {}, {}};

  failure.source = error;
  failure.operation = "LogView external source";

  return failure;
}

AsyncError schedulerAsync(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case SchedulerError::CLOSED:
    return AsyncError::CLOSED;
  default:
    return AsyncError::CALLBACK_FAILED;
  }
}

struct ExternalLogCompletion {
  SourceCredit credit;
  std::uint64_t generation{};
  std::shared_ptr<const ExternalLogFrame> frame;
  std::optional<SourceError> source;
  std::optional<FileLogFailure> file;
  std::optional<TextError> text;
  bool ready{};
  bool replacesWindow{};
  bool generationChanged{};
  bool historyUnavailable{};
  bool pending{};
  std::uint64_t readRevision{};
  std::optional<FileLogMetadata> metadata;
  bool empty{};
  SourceEdge edge{SourceEdge::MORE};
  std::size_t records{};
  std::optional<PositionRequest> restoreRequest;
  std::optional<PositionReply> restored;
  std::optional<SourceAnchor> restoreAnchor;
  LogRestoreStatus restoreStatus;
};

struct ExternalLogIntent {
  SourceAnchor anchor;
  SourceDirection direction;
  bool accumulate{};
  std::optional<ExternalLogAnchor> readingAnchor;
  std::int64_t readingDelta{};
};

class ExternalLogMetadataJob final : public BackgroundJob {
public:
  ExternalLogMetadataJob(std::shared_ptr<FileLogModel> file, Publisher<ExternalLogCompletion> publisher,
                         std::shared_ptr<ExternalLogCompletion> completion)
      : file_(std::move(file)), publisher_(std::move(publisher)), completion_(std::move(completion)) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      auto metadata = file_->metadata(stop);

      if (metadata) {
        completion_->metadata = *metadata;
      } else {
        completion_->file = metadata.error();
        completion_->source =
          metadata.error().error == FileLogError::NO_MEMORY ? SourceError::NO_MEMORY : SourceError::IO_ERROR;
      }
    } catch (const std::bad_alloc &) {
      completion_->source = SourceError::NO_MEMORY;
    } catch (...) {
      completion_->source = SourceError::CALLBACK_FAILED;
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto sent = publisher_.publish(completion_);

    return sent ? std::expected<void, AsyncError>{} : std::unexpected(schedulerAsync(sent.error()));
  }

private:
  std::shared_ptr<FileLogModel> file_;
  Publisher<ExternalLogCompletion> publisher_;
  std::shared_ptr<ExternalLogCompletion> completion_;
};

std::expected<std::shared_ptr<const ExternalLogFrame>, SourceError>
preparePage(const WindowPage<LogLine> &page, SourceEdge edge, SourceDirection direction,
            std::shared_ptr<const ExternalLogFrame> previous, std::shared_ptr<SourceBudget> budget, WidthPolicy policy,
            LogLimits limits, std::int32_t width, std::optional<TextError> &textError, std::stop_token stop,
            bool finite, bool file, std::optional<ExternalLogAnchor> anchor, std::int64_t anchorDelta,
            std::size_t maxCachedRows, bool following, const std::shared_ptr<LogBookmarkProvider> &bookmarks,
            const SourceReadCommand &command, BookmarkOptions bookmarkOptions) {
  std::size_t previousFirst{};
  std::size_t previousLast = previous ? previous->rows.size() : 0;
  auto before = previous ? previous->before : SourceCursor{};
  auto after = previous ? previous->after : SourceCursor{};

  if (previous && previousLast > maxCachedRows) {
    if (direction == SourceDirection::FORWARD) {
      previousFirst = previousLast - maxCachedRows;

      while (previousFirst &&
             ((finite || file)
                ? previous->anchors[previousFirst - 1].row == previous->anchors[previousFirst].row
                : previous->anchors[previousFirst - 1].pageBefore == previous->anchors[previousFirst].pageBefore)) {
        --previousFirst;
      }

      const auto &first = previous->anchors[previousFirst];

      before = finite || file
                 ? SourceCursor{before.source,
                                {finite ? first.ordinal : first.row.value - 1, finite ? 0 : first.row.generation}}
                 : first.pageBefore;
    } else {
      previousLast = maxCachedRows;

      while (previousLast < previous->rows.size() &&
             ((finite || file)
                ? previous->anchors[previousLast - 1].row == previous->anchors[previousLast].row
                : previous->anchors[previousLast - 1].pageAfter == previous->anchors[previousLast].pageAfter)) {
        ++previousLast;
      }

      if (previousLast < previous->rows.size()) {
        const auto &next = previous->anchors[previousLast];

        after =
          finite || file
            ? SourceCursor{after.source, {finite ? next.ordinal : next.row.value - 1, finite ? 0 : next.row.generation}}
            : previous->anchors[previousLast - 1].pageAfter;
      }
    }
  }

  std::size_t bytes{};
  std::size_t records{};
  RowId lastRecord;

  if (previous) {
    for (std::size_t index = previousFirst; index < previousLast; ++index) {
      const auto &row = previous->anchors[index];

      if (row.row != lastRecord) {
        bytes += row.recordBytes;
        ++records;
        lastRecord = row.row;
      }
    }
  }

  for (const auto &row : page.rows()) {
    if (row.value.text.size() > limits.maxRecordBytes || row.value.text.size() > limits.maxBytes - bytes ||
        records >= limits.maxLines) {
      return std::unexpected(SourceError::ROW_TOO_LARGE);
    }

    bytes += row.value.text.size();
    ++records;
  }

  // Cover retained logical/wrapped metadata plus simultaneous geometric vector reallocation
  // on the supported standard libraries. Validate this allowance with long-record allocation probes;
  // it is a capacity reservation, not an RSS measurement or a portable vector growth promise.
  const auto maxVisualRows = bytes / static_cast<std::size_t>(std::max(width / 2, 1)) + records;
  auto reservation = PREPARED_METADATA_BYTES + bytes * (GRAPHEME_ALLOCATION_FACTOR * sizeof(Grapheme) + 4) +
                     maxVisualRows * (sizeof(GraphemeText) + sizeof(ExternalLogAnchor) + sizeof(void *) + 64);

  if (bookmarks && records) {
    if (bookmarkOptions.maxBytes > (std::numeric_limits<std::size_t>::max() - reservation) / records) {
      return std::unexpected(SourceError::PAGE_TOO_LARGE);
    }

    reservation += records * bookmarkOptions.maxBytes;
  }
  auto credit = budget->reserve({reservation, 0, 0, 0});

  if (!credit) {
    return std::unexpected(credit.error());
  }

  auto frame = std::make_shared<ExternalLogFrame>();

  frame->credit = std::move(*credit);
  frame->records = records;
  frame->bytes = bytes;
  frame->width = width;
  frame->direction = direction;
  frame->edge = edge;
  frame->before = direction == SourceDirection::FORWARD && previous ? before : page.before();
  frame->after = direction == SourceDirection::BACKWARD && previous ? after : page.after();
  frame->rows.reserve(maxVisualRows);
  frame->anchors.reserve(maxVisualRows);

  auto copyPrevious = [&] {
    if (previous) {
      for (std::size_t index = previousFirst; index < previousLast; ++index) {
        frame->rows.push_back(std::make_unique<GraphemeText>(*previous->rows[index]));
      }

      frame->anchors.insert(frame->anchors.end(), previous->anchors.begin() + previousFirst,
                            previous->anchors.begin() + previousLast);
    }
  };

  if (direction == SourceDirection::FORWARD) {
    copyPrevious();
  }

  TextLimits textLimits;

  textLimits.max_input_bytes = limits.maxRecordBytes;
  textLimits.max_cluster_bytes = limits.maxRecordBytes;
  textLimits.max_graphemes = limits.maxRecordBytes;
  textLimits.max_cells = limits.maxRecordBytes * 2;

  std::size_t recordIndex{};

  for (const auto &row : page.rows()) {
    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    auto text = GraphemeText::from(row.value.text, policy, textLimits);

    if (!text) {
      textError = text.error();

      return std::unexpected(SourceError::INVALID_REPLY);
    }

    const auto clusters = text->graphemes();
    std::size_t first{};
    std::optional<ModelBookmark> bookmark;

    if (bookmarks) {
      auto captured = bookmarks->capture(command, row, bookmarkOptions, stop);

      if (captured && captured->resource() == bookmarks->resource() &&
          captured->position().kind == PositionKind::ITEM && captured->position().row == row.id &&
          captured->position().revision == command.dataRevision &&
          captured->storageBytes() <= bookmarkOptions.maxBytes) {
        bookmark = std::move(*captured);
      } else {
        frame->bookmarkError = captured ? SourceError::INVALID_REPLY : captured.error();
      }
    }

    if (anchor && anchor->row == row.id) {
      anchor->firstByte = std::min(anchor->firstByte, text->bytes().size());

      for (const auto &cluster : clusters) {
        if (cluster.byte_begin <= anchor->firstByte && anchor->firstByte < cluster.byte_end) {
          anchor->firstByte = cluster.byte_begin;

          break;
        }
      }
    }

    do {
      auto last = first;
      std::int32_t cells{};

      while (last < clusters.size() && (last == first || cells + clusters[last].cells <= width)) {
        cells += clusters[last++].cells;
      }

      const auto beginByte = first < clusters.size() ? clusters[first].byte_begin : 0;
      const auto endByte = last ? clusters[last - 1].byte_end : 0;
      auto wrapped = GraphemeText::from(text->bytes().substr(beginByte, endByte - beginByte), policy, textLimits);

      if (!wrapped) {
        textError = wrapped.error();

        return std::unexpected(SourceError::INVALID_REPLY);
      }

      frame->rows.push_back(std::make_unique<GraphemeText>(std::move(*wrapped)));
      frame->anchors.push_back({row.id, beginByte, endByte, finite ? page.before().token[0] + recordIndex : 0,
                                row.value.text.size(), page.before(), page.after(), bookmark});
      first = last;
    } while (first < clusters.size());

    ++recordIndex;
  }

  if (direction == SourceDirection::BACKWARD) {
    copyPrevious();
  }

  if (anchor) {
    const auto selected = std::ranges::find_if(frame->anchors, [&](const ExternalLogAnchor &candidate) {
      return candidate.row == anchor->row && candidate.firstByte <= anchor->firstByte &&
             (anchor->firstByte < candidate.lastByte || candidate.firstByte == candidate.lastByte);
    });

    if (selected != frame->anchors.end()) {
      const auto position = static_cast<std::int64_t>(selected - frame->anchors.begin());

      frame->preferredOffset = static_cast<std::uint64_t>(std::max<std::int64_t>(position + anchorDelta, 0));
      frame->anchorResolved =
        position + anchorDelta >= 0 && static_cast<std::uint64_t>(position + anchorDelta) < frame->rows.size();
    }
  }

  std::array<char, 160> footer{};

  std::snprintf(footer.data(), footer.size(), "Local window: %zu records | %s | %s", records,
                following ? "following tail" : "paused; Return to tail",
                edge == SourceEdge::FINAL_END ? "boundary" : "more available");

  auto caption = GraphemeText::from(footer.data(), policy);

  if (!caption) {
    textError = caption.error();

    return std::unexpected(SourceError::INVALID_REPLY);
  }

  frame->footer = std::make_unique<GraphemeText>(std::move(*caption));
  frame->following = following;

  std::snprintf(footer.data(), footer.size(), "Local window: %zu records | %s | %s", records,
                following ? "paused; Return to tail" : "following tail",
                edge == SourceEdge::FINAL_END ? "boundary" : "more available");

  auto alternate = GraphemeText::from(footer.data(), policy);

  if (!alternate) {
    textError = alternate.error();

    return std::unexpected(SourceError::INVALID_REPLY);
  }

  frame->alternateFooter = std::make_unique<GraphemeText>(std::move(*alternate));

  return frame;
}

class ExternalLogJob final : public BackgroundJob {
public:
  ExternalLogJob(WindowRequest<LogLine> request, Publisher<ExternalLogCompletion> publisher,
                 std::shared_ptr<ExternalLogCompletion> completion, std::shared_ptr<const ExternalLogFrame> previous,
                 std::shared_ptr<SourceBudget> budget, WidthPolicy policy, LogLimits limits, std::int32_t width,
                 std::uint64_t fileGeneration, bool finite, bool file, std::optional<ExternalLogAnchor> anchor,
                 std::int64_t anchorDelta, std::size_t maxCachedRows, bool following,
                 std::shared_ptr<LogBookmarkProvider> bookmarks, BookmarkOptions bookmarkOptions)
      : request_(std::move(request)), publisher_(std::move(publisher)), completion_(std::move(completion)),
        previous_(std::move(previous)), budget_(std::move(budget)), policy_(policy), limits_(limits), width_(width),
        fileGeneration_(fileGeneration), finite_(finite), file_(file), anchor_(anchor), anchorDelta_(anchorDelta),
        maxCachedRows_(maxCachedRows), following_(following), bookmarks_(std::move(bookmarks)),
        bookmarkOptions_(bookmarkOptions) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      reply_ = WindowRequestAccess::load(
        request_,
        [this](WindowProvider<LogLine> &provider, const SourceReadCommand &command, PageBuilder<LogLine> &out,
               std::stop_token token) -> std::expected<SourceReadReply<LogLine>, SourceError> {
          if (auto *file = dynamic_cast<FileLogModel *>(&provider)) {
            auto reply = file->loadFile(command, out, token);

            if (!reply) {
              completion_->file = reply.error().fileFailure;

              return std::unexpected(reply.error().sourceError);
            }

            return std::move(*reply);
          }

          return provider.load(command, out, token);
        },
        stop);

      if (!reply_) {
        completion_->source = reply_.error();

        return;
      }

      if (reply_->state != SourceReadState::READY) {
        if (reply_->state == SourceReadState::FAILED) {
          completion_->source = reply_->error;
        }
        if (reply_->state == SourceReadState::HISTORY_UNAVAILABLE && reply_->recovery && fileGeneration_ &&
            reply_->recovery->token[1] != fileGeneration_) {
          completion_->generationChanged = true;
        }

        completion_->historyUnavailable = reply_->state == SourceReadState::HISTORY_UNAVAILABLE;
        completion_->pending = reply_->state == SourceReadState::PENDING;

        return;
      }

      completion_->empty = reply_->page.rows().empty();
      completion_->edge = reply_->edge;
      completion_->records = reply_->page.rows().size();

      if (fileGeneration_ && reply_->page.before().token[1] != fileGeneration_) {
        completion_->generationChanged = true;
        reply_ = std::unexpected(SourceError::STALE_SOURCE);

        return;
      }

      auto prepared = preparePage(reply_->page, reply_->edge, request_.command().direction, previous_, budget_, policy_,
                                  limits_, width_, completion_->text, stop, finite_, file_, anchor_, anchorDelta_,
                                  maxCachedRows_, following_, bookmarks_, request_.command(), bookmarkOptions_);

      if (prepared) {
        completion_->frame = std::move(*prepared);
        completion_->ready = true;
      } else {
        completion_->source = prepared.error();
      }
    } catch (const std::bad_alloc &) {
      completion_->source = SourceError::NO_MEMORY;
    } catch (...) {
      completion_->source = SourceError::CALLBACK_FAILED;
    }
  }

  std::expected<void, AsyncError> publish() override {
    if (!sourcePublished_) {
      auto published = request_.complete(reply_);

      if (!published) {
        return std::unexpected(schedulerAsync(published.error()));
      }

      sourcePublished_ = true;
    }

    auto published = publisher_.publish(completion_);

    return published ? std::expected<void, AsyncError>{} : std::unexpected(schedulerAsync(published.error()));
  }

private:
  WindowRequest<LogLine> request_;
  Publisher<ExternalLogCompletion> publisher_;
  std::shared_ptr<ExternalLogCompletion> completion_;
  std::shared_ptr<const ExternalLogFrame> previous_;
  std::shared_ptr<SourceBudget> budget_;
  WidthPolicy policy_;
  LogLimits limits_;
  std::int32_t width_{};
  std::uint64_t fileGeneration_{};
  bool finite_{};
  bool file_{};
  std::optional<ExternalLogAnchor> anchor_;
  std::int64_t anchorDelta_{};
  std::size_t maxCachedRows_{};
  bool following_{};
  std::shared_ptr<LogBookmarkProvider> bookmarks_;
  BookmarkOptions bookmarkOptions_;
  std::expected<SourceReadReply<LogLine>, SourceError> reply_{std::unexpected(SourceError::CANCELLED)};
  bool sourcePublished_{};
};

class ExternalLogRestoreJob final : public BackgroundJob {
public:
  ExternalLogRestoreJob(std::shared_ptr<LogBookmarkProvider> provider, PositionRequest request,
                        Publisher<ExternalLogCompletion> publisher, std::shared_ptr<ExternalLogCompletion> completion,
                        WidthPolicy policy, std::int32_t width, SourceCapabilities capabilities,
                        std::weak_ptr<const WindowNotifier<LogLine>> readiness)
      : provider_(std::move(provider)), request_(std::move(request)), publisher_(std::move(publisher)),
        completion_(std::move(completion)), policy_(policy), width_(width), capabilities_(capabilities),
        readiness_(std::move(readiness)) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      auto resolved = provider_->resolveLog(request_, readiness_, stop);

      if (!resolved) {
        completion_->restoreStatus = {PositionResolution::UNAVAILABLE,
                                      resolved.error() == SourceError::INVALID_CURSOR ||
                                          resolved.error() == SourceError::ROW_NOT_FOUND
                                        ? LogRestoreReason::ANCHOR_MISMATCH
                                        : LogRestoreReason::HISTORY_UNAVAILABLE,
                                      resolved.error()};
      } else if (!positionReplyMatches(request_, *resolved)) {
        completion_->restoreStatus = {PositionResolution::UNAVAILABLE, LogRestoreReason::RESOURCE_MISMATCH,
                                      SourceError::INVALID_REPLY};
      } else {
        completion_->restored = *resolved;
        completion_->restoreStatus.resolution = resolved->state;

        switch (resolved->state) {
        case PositionResolution::EXACT:
        case PositionResolution::NEAREST: {
          auto anchor = provider_->readAnchor(*resolved, request_.session);

          if (!anchor ||
              (anchor->kind == SourceAnchorKind::ROW &&
               (!capabilities_.seekRow || !anchor->row.value || !anchor->row.generation)) ||
              anchor->kind == SourceAnchorKind::TAIL ||
              (anchor->kind == SourceAnchorKind::CURSOR && anchor->cursor.source != request_.session) ||
              resolved->position.revision != request_.revision) {
            completion_->restoreStatus = {PositionResolution::UNSUPPORTED, LogRestoreReason::UNSUPPORTED,
                                          anchor ? SourceError::UNSUPPORTED : anchor.error()};
          } else {
            completion_->restoreAnchor = *anchor;
            completion_->restoreStatus.reason = resolved->state == PositionResolution::NEAREST
                                                  ? LogRestoreReason::DELETED_SEGMENT
                                                  : LogRestoreReason::NONE;
          }

          break;
        }
        case PositionResolution::INCOMPATIBLE:
          completion_->restoreStatus.reason = LogRestoreReason::RESOURCE_MISMATCH;

          break;
        case PositionResolution::UNAVAILABLE:
          completion_->restoreStatus.reason = LogRestoreReason::HISTORY_UNAVAILABLE;

          break;
        case PositionResolution::UNSUPPORTED:
          completion_->restoreStatus.reason = LogRestoreReason::UNSUPPORTED;

          break;
        case PositionResolution::EMPTY:
        case PositionResolution::PENDING:
          break;
        default:
          completion_->restoreStatus = {PositionResolution::UNAVAILABLE, LogRestoreReason::HISTORY_UNAVAILABLE,
                                        SourceError::INVALID_REPLY};

          break;
        }
      }

      if (!completion_->restoreAnchor && !stop.stop_requested()) {
        auto credit = completion_->credit.split({PREPARED_METADATA_BYTES, 0, 0, 0});

        if (!credit) {
          completion_->restoreStatus.error = credit.error();

          return;
        }

        auto frame = std::make_shared<ExternalLogFrame>();
        const auto message = completion_->restoreStatus.resolution == PositionResolution::PENDING
                               ? "Waiting for the saved reading position"
                             : completion_->restoreStatus.resolution == PositionResolution::EMPTY
                               ? "Source is empty"
                               : "Saved reading position unavailable; navigate to resume";
        auto text = GraphemeText::from(message, policy_);

        frame->credit = std::move(*credit);
        frame->width = width_;
        frame->edge = SourceEdge::FINAL_END;
        frame->before.source = request_.session;
        frame->after.source = request_.session;

        if (text) {
          frame->alternateFooter = std::make_unique<GraphemeText>(*text);
          frame->footer = std::make_unique<GraphemeText>(std::move(*text));
        }

        completion_->frame = std::move(frame);
      }
    } catch (const std::bad_alloc &) {
      completion_->restoreStatus = {PositionResolution::UNAVAILABLE, LogRestoreReason::HISTORY_UNAVAILABLE,
                                    SourceError::NO_MEMORY};
    } catch (...) {
      completion_->restoreStatus = {PositionResolution::UNAVAILABLE, LogRestoreReason::HISTORY_UNAVAILABLE,
                                    SourceError::CALLBACK_FAILED};
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto published = publisher_.publish(completion_);

    return published ? std::expected<void, AsyncError>{} : std::unexpected(schedulerAsync(published.error()));
  }

private:
  std::shared_ptr<LogBookmarkProvider> provider_;
  PositionRequest request_;
  Publisher<ExternalLogCompletion> publisher_;
  std::shared_ptr<ExternalLogCompletion> completion_;
  WidthPolicy policy_;
  std::int32_t width_{};
  SourceCapabilities capabilities_;
  std::weak_ptr<const WindowNotifier<LogLine>> readiness_;
};

class ExternalLogActivity final : public ui::detail::SceneActivity,
                                  public std::enable_shared_from_this<ExternalLogActivity> {
public:
  ExternalLogActivity(WidgetTree &tree, Node<LogView> node, std::weak_ptr<const void> lifetime,
                      std::weak_ptr<ExternalLogState> state, WidthPolicy policy, LogViewConfig config,
                      std::shared_ptr<LogNavigationSession> navigation, std::function<BackgroundExecutor *()> executor,
                      std::function<SchedulerTime()> now, std::function<void(ManagedError)> report)
      : tree_(tree), node_(node), lifetime_(std::move(lifetime)), state_(std::move(state)), policy_(policy),
        config_(config), navigation_(std::move(navigation)), executor_(std::move(executor)), now_(std::move(now)),
        report_(std::move(report)) {
  }

  ~ExternalLogActivity() override {
    cancel();
  }

  std::expected<void, SourceError> connect(const std::shared_ptr<ui::detail::LogSourceData> &data,
                                           bool remember = true) {
    auto binding = prepareBinding(data);

    if (!binding) {
      return std::unexpected(binding.error());
    }

    auto connection = tree_.connect<ExternalLogCompletion>(
      node_, [weak = weak_from_this()](Widget &, std::shared_ptr<const ExternalLogCompletion> completion) {
        auto activity = weak.lock();

        if (activity && activity->active() && completion->generation == activity->generation_) {
          if (completion->metadata && completion->metadata->generation == activity->fileGeneration_ &&
              completion->metadata->byteExtent == activity->observedExtent_) {
            activity->busy_ = false;
            activity->armPoll();

            return;
          }

          activity->completion_ = std::move(completion);
          (void)activity->tree_.invalidate(activity->node_, Invalidation::LAYOUT);
        }
      });

    if (!connection) {
      return std::unexpected(sourceTreeError(connection.error()));
    }

    data_ = data;
    source_ = std::move(binding->source);
    consumer_ = std::move(binding->consumer);
    std::swap(readinessCredit_, binding->readinessCredit);
    readiness_.swap(binding->readiness);

    if (!data_->budget) {
      data_->budget = source_->budget();
    }

    connection_ = std::move(*connection);

    if (!navigation_->find(data->resource)) {
      auto entry = navigation_->stage(data->resource, resourceLifetime(data),
                                      {{}, 0, config_.followTail && source_->capabilities().tail});

      if (!entry) {
        return std::unexpected(entry.error());
      }

      navigation_->commit(std::move(*entry));
    }

    if (remember) {
      primeRestore(navigation_->find(data->resource)->value);
    }

    return {};
  }

  std::expected<void, SourceError> replace(const std::shared_ptr<ui::detail::LogSourceData> &data) {
    if (!active()) {
      return std::unexpected(SourceError::CLOSED);
    }

    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
    }
    if (data_ == data) {
      return {};
    }

    // Build the complete new binding before changing the current session.
    auto binding = prepareBinding(data);

    if (!binding) {
      return std::unexpected(binding.error());
    }

    auto state = state_.lock();
    std::uint64_t intentSerial{};
    const auto read = tree_.read(node_, [&](const LogView &widget) {
      const auto intent = ScrollWidgetAccess::state(widget).intent.lock();

      intentSerial = intent ? intent->serial : 0;
    });

    if (!read) {
      return std::unexpected(sourceTreeError(read.error()));
    }

    auto sampled = sampleNavigation(false);

    if (!sampled) {
      return std::unexpected(sourceTreeError(sampled.error()));
    }

    auto checkpoint = snapshotCheckpoint();

    if (!checkpoint) {
      navigation_->status = {PositionResolution::UNAVAILABLE, LogRestoreReason::CHECKPOINT_CAPACITY,
                             checkpoint.error()};

      return std::unexpected(checkpoint.error());
    }

    std::optional<LogCheckpointChange> outgoing;
    const auto *current = navigation_->find(data_->resource);

    if (!current || !sameCheckpoint(current->value, *checkpoint)) {
      auto staged = navigation_->stage(data_->resource, resourceLifetime(data_), *checkpoint);

      if (!staged) {
        navigation_->status = {PositionResolution::UNAVAILABLE, LogRestoreReason::CHECKPOINT_CAPACITY, staged.error()};

        return std::unexpected(staged.error());
      }

      outgoing = std::move(*staged);
    }

    LogReadingCheckpoint incoming{{}, 0, config_.followTail && binding->source->capabilities().tail};
    std::optional<LogCheckpointChange> destination;

    if (data->resource.identity == data_->resource.identity) {
      incoming = *checkpoint;
    } else if (const auto *saved = navigation_->find(data->resource)) {
      incoming = saved->value;
    } else {
      auto staged = navigation_->stage(data->resource, resourceLifetime(data), incoming);

      if (!staged) {
        navigation_->status = {PositionResolution::UNAVAILABLE, LogRestoreReason::CHECKPOINT_CAPACITY, staged.error()};

        return std::unexpected(staged.error());
      }

      destination = std::move(*staged);
    }

    const auto invalidated = tree_.invalidate(node_, Invalidation::LAYOUT);

    if (!invalidated) {
      return std::unexpected(sourceTreeError(invalidated.error()));
    }

    if (outgoing) {
      navigation_->commit(std::move(*outgoing));
    }

    if (destination) {
      navigation_->commit(std::move(*destination));
    }

    if (data->resource.identity == data_->resource.identity) {
      navigation_->find(data->resource)->lifetime = resourceLifetime(data);
    }

    timer_ = {};
    timerArmed_ = false;
    const auto replacingGeneration = generation_;
    const auto stillCurrent = [&]() -> std::expected<void, SourceError> {
      return checkGeneration(replacingGeneration);
    };

    if (latest_) {
      latest_->cancel();
    }

    if (auto current = stillCurrent(); !current) {
      return current;
    }

    consumer_.close();

    if (auto current = stillCurrent(); !current) {
      return current;
    }

    source_->close();

    if (auto current = stillCurrent(); !current) {
      return current;
    }

    ++generation_;
    source_ = std::move(binding->source);
    consumer_ = std::move(binding->consumer);
    std::swap(readinessCredit_, binding->readinessCredit);
    readiness_.swap(binding->readiness);
    data_ = data;

    if (!data_->budget) {
      data_->budget = source_->budget();
    }

    completion_.reset();
    fileGeneration_ = 0;
    observedExtent_ = 0;
    turnProgressRecords_ = 0;
    width_ = 0;
    busy_ = false;
    failed_ = false;
    started_ = false;
    accumulate_ = false;
    backwards_ = false;
    metadataPending_ = false;
    fillAllowed_ = true;
    lastEdge_ = SourceEdge::MORE;
    readingAnchor_.reset();
    readingDelta_ = 0;
    lastIntent_.reset();
    resumeIntent_.reset();

    ExternalLogState replacement;

    replacement.activity = state->activity;
    replacement.viewport = state->viewport;
    replacement.visible = state->visible;
    replacement.focused = state->focused;
    replacement.wheelStep = state->wheelStep;
    replacement.keyStep = state->keyStep;
    replacement.pageOverlap = state->pageOverlap;
    replacement.intentSerial = intentSerial;
    replacement.requestedOffset = 0;
    replacement.followTail = incoming.following && source_->capabilities().tail;
    replacement.finiteRows = data->finiteRows;
    replacement.fileSource = static_cast<bool>(data->file);
    replacement.navigation = state->navigation;
    *state = std::move(replacement);
    primeRestore(std::move(incoming));

    return {};
  }

  bool active() const noexcept override {
    return !closed_ && !lifetime_.expired() && !state_.expired();
  }

  void cancel() noexcept override {
    if (closed_) {
      return;
    }

    closed_ = true;
    timer_ = {};
    timerArmed_ = false;
    (void)connection_.subscription.disconnect();

    if (latest_) {
      latest_->cancel();
    }

    consumer_.close();

    if (source_) {
      source_->close();
    }

    completion_.reset();
    navigation_->binding.reset();
    readiness_.reset();
    readinessCredit_ = {};
  }

  void service() noexcept override {
    if (!active()) {
      cancel();

      return;
    }

    try {
      if (latest_) {
        auto serviced = latest_->service();

        if (!serviced) {
          failBackground(serviced.error());
        }
      }
    } catch (const std::bad_alloc &) {
      fail(SourceError::NO_MEMORY);
    } catch (...) {
      fail(SourceError::CALLBACK_FAILED);
    }
  }

  void prepared() noexcept override {
    if (!active()) {
      cancel();

      return;
    }

    try {
      prepare();
      refreshCheckpoint();
    } catch (const std::bad_alloc &) {
      fail(SourceError::NO_MEMORY);
    } catch (...) {
      fail(SourceError::CALLBACK_FAILED);
    }
  }

  std::expected<bool, TreeError> input(const InputEvent &event) override {
    auto result = inputEvent(event);

    refreshCheckpoint();

    return result;
  }

  std::expected<bool, TreeError> inputEvent(const InputEvent &event) {
    auto state = state_.lock();

    if (!active() || !state || !state->visible || config_.readOnly) {
      return false;
    }

    const auto *key = std::get_if<KeyEvent>(&event);
    const auto *mouse = std::get_if<MouseInputEvent>(&event);

    if (key || mouse) {
      const auto sampled = sampleNavigation(true);

      if (!sampled) {
        return std::unexpected(sampled.error());
      }
    }

    bool backwards{};
    bool forwards{};
    std::int64_t step =
      static_cast<std::int64_t>(std::min<std::uint64_t>(state->keyStep, std::numeric_limits<std::int32_t>::max()));

    if (key && state->focused) {
      if (key->action == KeyAction::RELEASE || key->modifiers.alt || key->modifiers.super || key->modifiers.shift ||
          (key->modifiers.control && key->key != InputKey::HOME && key->key != InputKey::END)) {
        return false;
      }

      if (key->key == InputKey::HOME) {
        state->followTail = false;
        request({}, SourceDirection::FORWARD, false);

        return true;
      }

      if (key->key == InputKey::END && source_->capabilities().tail) {
        tail();

        return true;
      }

      backwards = key->key == InputKey::UP || key->key == InputKey::PAGE_UP;
      forwards = key->key == InputKey::DOWN || key->key == InputKey::PAGE_DOWN;

      if (key->key == InputKey::PAGE_UP || key->key == InputKey::PAGE_DOWN) {
        const auto height = static_cast<std::uint64_t>(std::max(state->viewport.height, 0));

        step = static_cast<std::int64_t>(height > state->pageOverlap ? height - state->pageOverlap : 1);
      }
    }

    if (mouse && mouse->action == MouseAction::SCROLL) {
      if (mouse->modifiers.alt || mouse->modifiers.control || mouse->modifiers.super || mouse->modifiers.shift) {
        return false;
      }

      const auto movement = mouse->wheelDelta * state->wheelStep;

      if (!std::isfinite(movement) || movement <= 0 ||
          movement > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      auto geometry = tree_.inspect(node_);

      if (!geometry) {
        return std::unexpected(geometry.error());
      }

      const auto bounds = geometry->geometry.surface;

      if (!bounds.contains({mouse->column, mouse->row})) {
        return false;
      }

      backwards = mouse->button == MouseButton::WHEEL_UP;
      forwards = mouse->button == MouseButton::WHEEL_DOWN;
      step = static_cast<std::int64_t>(std::ceil(movement));
    }

    const auto frame = state->frame;

    if (!frame) {
      return false;
    }

    if ((backwards || forwards) && busy_) {
      supersede();
    }

    if (backwards) {
      state->followTail = false;

      if (state->offset < static_cast<std::uint64_t>(step) && source_->capabilities().backward) {
        const auto anchor = frame->anchors.empty()
                              ? std::optional<ExternalLogAnchor>{}
                              : frame->anchors[std::min<std::uint64_t>(state->offset, frame->anchors.size() - 1)];

        request({SourceAnchorKind::CURSOR, frame->before, {}}, SourceDirection::BACKWARD, true, anchor, -step);

        return true;
      }
    }

    if (forwards && state->offset + static_cast<std::uint64_t>(step) > frameMaximum(*state)) {
      state->followTail = false;

      if (lastEdge_ != SourceEdge::FINAL_END || frame->direction == SourceDirection::BACKWARD) {
        const auto target = state->offset + static_cast<std::uint64_t>(step);

        if ((data_->finiteRows || source_->capabilities().seekRow) && target < frame->anchors.size()) {
          const auto reading = frame->anchors[target];
          const auto anchor = data_->finiteRows
                                ? SourceAnchor{SourceAnchorKind::CURSOR, {source_->stamp(), {reading.ordinal, 0}}, {}}
                                : SourceAnchor{SourceAnchorKind::ROW, {}, reading.row};

          request(anchor, SourceDirection::FORWARD, false, reading);
        } else {
          const auto anchor = frame->anchors.empty()
                                ? std::optional<ExternalLogAnchor>{}
                                : frame->anchors[std::min<std::uint64_t>(state->offset, frame->anchors.size() - 1)];

          request({SourceAnchorKind::CURSOR, frame->after, {}}, SourceDirection::FORWARD, true, anchor, step);
        }

        return true;
      }
    }

    return false;
  }

private:
  std::expected<void, SourceError> checkGeneration(std::uint64_t expected) const {
    if (generation_ != expected) {
      return std::unexpected(SourceError::CANCELLED);
    }
    if (!active()) {
      return std::unexpected(SourceError::CLOSED);
    }

    const auto live = tree_.read(node_, [](const LogView &) {
    });

    return live ? std::expected<void, SourceError>{} : std::unexpected(SourceError::CLOSED);
  }

  static std::weak_ptr<const void> resourceLifetime(const std::shared_ptr<ui::detail::LogSourceData> &data) {
    return data->bookmarks ? std::weak_ptr<const void>(data->bookmarks) : std::weak_ptr<const void>(data);
  }

  std::expected<LogReadingCheckpoint, SourceError> snapshotCheckpoint() const {
    const auto state = state_.lock();

    if (!state || navigation_->closed) {
      return std::unexpected(SourceError::CLOSED);
    }
    if (state->byteRequest || state->positionRequest) {
      return std::unexpected(SourceError::WOULD_BLOCK);
    }

    LogReadingCheckpoint value;

    if (const auto *saved = navigation_->find(data_->resource)) {
      value = saved->value;
    }

    value.following = state->followTail;

    if (state->frame && !state->frame->anchors.empty()) {
      const auto &anchor = state->frame->anchors[std::min<std::uint64_t>(state->requestedOffset.value_or(state->offset),
                                                                         state->frame->anchors.size() - 1)];

      if (anchor.bookmark) {
        value.bookmark = anchor.bookmark;
        value.byteInRecord = anchor.firstByte;
      } else if (data_->bookmarks && state->frame->bookmarkError != SourceError::UNSUPPORTED) {
        return std::unexpected(state->frame->bookmarkError.value_or(SourceError::WOULD_BLOCK));
      }
    }

    return value;
  }

  void primeRestore(LogReadingCheckpoint checkpoint) {
    pendingRestore_.reset();
    restoreRequest_.reset();
    restoredReading_.reset();
    restoreBlocked_ = false;
    navigation_->status = {};

    if (!data_->bookmarks) {
      navigation_->status = {PositionResolution::UNSUPPORTED, LogRestoreReason::UNSUPPORTED, SourceError::UNSUPPORTED};
    }
    if (checkpoint.bookmark && !checkpoint.following) {
      pendingRestore_ = std::move(checkpoint);
      navigation_->status = {PositionResolution::PENDING, LogRestoreReason::NONE, {}};
    }
  }

  static bool sameCheckpoint(const LogReadingCheckpoint &left, const LogReadingCheckpoint &right) noexcept {
    if (left.following != right.following || left.byteInRecord != right.byteInRecord ||
        left.bookmark.has_value() != right.bookmark.has_value()) {
      return false;
    }
    if (!left.bookmark) {
      return true;
    }

    const auto a = left.bookmark->position();
    const auto b = right.bookmark->position();

    return left.bookmark->resource() == right.bookmark->resource() && a.kind == b.kind && a.row == b.row &&
           a.offset == b.offset && a.revision == b.revision && a.affinity == b.affinity &&
           left.bookmark->metadata().data() == right.bookmark->metadata().data() &&
           left.bookmark->metadata().size() == right.bookmark->metadata().size();
  }

  void refreshCheckpoint() {
    if (!active() || navigation_->closed) {
      return;
    }

    // Observing a checkpoint must not acknowledge the native intent receipt.
    // The next input/preparation pass still owns navigationChanged and intentSerial.
    const auto state = state_.lock();
    const auto sampled = tree_.read(node_, [&](const LogView &widget) {
      const auto position = ScrollWidgetAccess::state(widget).controller->snapshot();

      if (position) {
        state->offset = position->vertical.offset.raster;
      }
    });

    if (!sampled) {
      return;
    }

    auto value = snapshotCheckpoint();
    const auto *saved = navigation_->find(data_->resource);

    if (!value) {
      navigation_->status = {PositionResolution::UNAVAILABLE,
                             value.error() == SourceError::WOULD_BLOCK || value.error() == SourceError::NO_MEMORY
                               ? LogRestoreReason::CHECKPOINT_CAPACITY
                               : LogRestoreReason::HISTORY_UNAVAILABLE,
                             value.error()};

      return;
    }
    if (saved && sameCheckpoint(saved->value, *value)) {
      return;
    }

    auto staged = navigation_->stage(data_->resource, resourceLifetime(data_), std::move(*value));

    if (staged) {
      navigation_->commit(std::move(*staged));
    } else {
      navigation_->status = {PositionResolution::UNAVAILABLE, LogRestoreReason::CHECKPOINT_CAPACITY, staged.error()};
    }
  }

  std::expected<void, TreeError> sampleNavigation(bool supersedePrevious) {
    auto state = state_.lock();

    if (!state) {
      return std::unexpected(TreeError::CLOSED);
    }

    std::expected<ScrollControllerSnapshot, TreeError> position{std::unexpected(TreeError::INVALID_ARGUMENT)};
    std::uint64_t serial{};
    bool focused{};
    const auto read = tree_.read(node_, [&](const LogView &widget) {
      const auto &scroll = ScrollWidgetAccess::state(widget);
      const auto intent = scroll.intent.lock();

      position = scroll.controller->snapshot();
      serial = intent ? intent->serial : 0;
      focused = scroll.focused;
    });

    if (!read || !position) {
      return std::unexpected(read ? position.error() : read.error());
    }

    const bool changed = state->navigationChanged || serial != state->intentSerial;

    state->offset = position->vertical.offset.raster;
    state->intentSerial = serial;
    state->focused = focused;
    state->navigationChanged = false;

    if (changed) {
      state->requestedOffset.reset();

      if (supersedePrevious) {
        state->followTail = false;
        supersede();
      }
    }

    return {};
  }

  void fail(SourceError error, std::optional<FileLogFailure> file = {}, std::optional<TextError> text = {}) {
    if (!active()) {
      return;
    }

    auto failure = sourceFailure(node_, error);

    failure.logFile = file;
    failure.text = text;
    failed_ = true;
    busy_ = false;
    timer_ = {};
    timerArmed_ = false;

    if (latest_) {
      latest_->cancel();
    }

    (void)consumer_.cancel();
    report_(failure);
  }

  void failBackground(BackgroundError error) {
    auto failure =
      sourceFailure(node_, error == BackgroundError::NO_MEMORY ? SourceError::NO_MEMORY : SourceError::CONTEXT_ERROR);

    failure.background = error;
    failed_ = true;
    busy_ = false;
    timer_ = {};
    timerArmed_ = false;

    if (latest_) {
      latest_->cancel();
    }

    (void)consumer_.cancel();
    report_(failure);
  }

  void dispatch(WindowRequest<LogLine> request) {
    const auto command = request.command();

    lastIntent_ = ExternalLogIntent{command.anchor, command.direction, accumulate_, readingAnchor_, readingDelta_};
    metadataPending_ = false;

    if (!latest_) {
      auto *executor = executor_();

      if (!executor) {
        fail(SourceError::CONTEXT_ERROR);

        return;
      }

      latest_ = std::make_unique<BackgroundLatest>(*executor);
    }

    auto state = state_.lock();
    auto completion = std::make_shared<ExternalLogCompletion>();

    completion->generation = generation_;
    completion->replacesWindow = !accumulate_;
    completion->readRevision = command.dataRevision;

    auto work = std::make_unique<ExternalLogJob>(
      std::move(request), connection_.publisher, std::move(completion), accumulate_ ? state->frame : nullptr,
      source_->budget(), policy_, config_.limits, width_, data_->file ? fileGeneration_ : 0,
      data_->finiteRows.has_value(), static_cast<bool>(data_->file), readingAnchor_, readingDelta_,
      static_cast<std::size_t>(state->viewport.height) + OVERSCAN_ROWS, state->followTail, data_->bookmarks,
      navigation_->options.bookmarks);
    auto admitted = latest_->replace(std::move(work));

    if (!admitted) {
      failBackground(admitted.error());
    }
  }

  std::expected<void, SourceError> startRestore() {
    if (!pendingRestore_ || !pendingRestore_->bookmark || !data_->bookmarks) {
      return std::unexpected(SourceError::UNSUPPORTED);
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
    }

    auto revision = source_->dataRevision();

    if (!revision) {
      return std::unexpected(revision.error());
    }

    attemptedRestoreRevision_ = *revision;

    if (latest_) {
      const auto cancellingGeneration = generation_;

      latest_->cancel();
      const auto current = checkGeneration(cancellingGeneration);

      if (!current) {
        return std::unexpected(current.error());
      }
    }

    auto credit = source_->budget()->reserve({PREPARED_METADATA_BYTES + RESTORE_ENVELOPE_BYTES, 0, 1, 0});

    if (!credit) {
      return std::unexpected(credit.error());
    }

    try {
      if (!latest_) {
        auto *executor = executor_();

        if (!executor) {
          return std::unexpected(SourceError::CLOSED);
        }

        latest_ = std::make_unique<BackgroundLatest>(*executor);
      }

      auto state = state_.lock();

      PositionRequest request{source_->stamp(), *revision, generation_ + 1, *pendingRestore_->bookmark};
      auto completion = std::make_shared<ExternalLogCompletion>();

      completion->credit = std::move(*credit);
      completion->generation = request.sequence;
      completion->restoreRequest = request;
      auto work =
        std::make_unique<ExternalLogRestoreJob>(data_->bookmarks, request, connection_.publisher, completion, policy_,
                                                state->viewport.width, source_->capabilities(), readiness_);
      auto admitted = latest_->replace(std::move(work));

      if (!admitted) {
        return std::unexpected(admitted.error() == BackgroundError::NO_MEMORY ? SourceError::NO_MEMORY
                                                                              : SourceError::WOULD_BLOCK);
      }

      ++generation_;
      restoreRequest_ = std::move(request);
      navigation_->status = {PositionResolution::PENDING, LogRestoreReason::NONE, {}};
      lastIntent_.reset();
      width_ = state->viewport.width;
      busy_ = true;
      started_ = true;
      metadataPending_ = false;

      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    }
  }

  void restoreAdmissionFailed(SourceError error) {
    if (error == SourceError::CANCELLED || error == SourceError::CLOSED) {
      return;
    }

    restoreBlocked_ = true;

    if ((error == SourceError::WOULD_BLOCK || error == SourceError::NO_MEMORY) &&
        navigation_->status.resolution == PositionResolution::PENDING) {
      navigation_->status.error = error;

      return;
    }

    navigation_->status = {
      PositionResolution::UNAVAILABLE,
      error == SourceError::UNSUPPORTED ? LogRestoreReason::UNSUPPORTED : LogRestoreReason::HISTORY_UNAVAILABLE, error};
  }

  void request(SourceAnchor anchor, SourceDirection direction, bool accumulate,
               std::optional<ExternalLogAnchor> readingAnchor = {}, std::int64_t readingDelta = 0) {
    auto state = state_.lock();

    if (!state || state->viewport.width <= 0 || state->viewport.height <= 0) {
      return;
    }

    restoredReading_.reset();

    if (pendingRestore_ || restoreBlocked_) {
      pendingRestore_.reset();
      restoreRequest_.reset();
      restoreBlocked_ = false;
      navigation_->status = {};
    }

    // A new source request acknowledges native intent already observed before its admission.
    // A later preparation pass must not mistake that older receipt for a superseding command.
    const auto sampled = sampleNavigation(false);

    if (!sampled) {
      fail(sourceTreeError(sampled.error()));

      return;
    }

    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      fail(SourceError::IDENTITY_EXHAUSTED);

      return;
    }

    const auto rows =
      std::min(config_.limits.maxLines, static_cast<std::size_t>(state->viewport.height) + OVERSCAN_ROWS);
    auto work = consumer_.request(anchor, direction, rows);

    if (!work) {
      fail(work.error());

      return;
    }

    ++generation_;
    started_ = true;
    width_ = state->viewport.width;
    accumulate_ = accumulate;
    fillAllowed_ = true;
    readingAnchor_ = readingAnchor;
    readingDelta_ = readingDelta;
    turnProgressRecords_ = 0;
    backwards_ = direction == SourceDirection::BACKWARD;
    busy_ = true;
    failed_ = false;
    lastEdge_ = SourceEdge::MORE;
    completion_.reset();
    dispatch(std::move(*work));
  }

  void tail() {
    auto state = state_.lock();

    if (state) {
      state->tailRequested = false;
    }

    if (state && source_->capabilities().tail) {
      state->followTail = true;
      request({SourceAnchorKind::TAIL, {}, {}}, SourceDirection::BACKWARD, false);
    } else if (state) {
      fail(SourceError::UNSUPPORTED);
    }
  }

  void supersede() {
    if (latest_) {
      latest_->cancel();
    }

    (void)consumer_.cancel();
    completion_.reset();
    busy_ = false;
    fillAllowed_ = false;

    if (generation_ != std::numeric_limits<std::uint64_t>::max()) {
      ++generation_;
    }
  }

  void armPoll() {
    auto state = state_.lock();

    if (!active() || !data_->file || !state || !state->visible || !state->followTail || timerArmed_ || failed_) {
      return;
    }

    const auto now = now_();

    if (now > std::numeric_limits<SchedulerTime>::max() - TAIL_POLL_INTERVAL) {
      fail(SourceError::IDENTITY_EXHAUSTED);

      return;
    }

    auto timer = tree_.at(node_, now + TAIL_POLL_INTERVAL, [weak = weak_from_this()](LogView &) {
      if (auto activity = weak.lock(); activity && activity->active()) {
        activity->timerArmed_ = false;
        activity->timer_ = {};

        try {
          activity->poll();
        } catch (const std::bad_alloc &) {
          activity->fail(SourceError::NO_MEMORY);
        } catch (...) {
          activity->fail(SourceError::CALLBACK_FAILED);
        }
      }
    });

    if (!timer) {
      fail(sourceTreeError(timer.error()));

      return;
    }

    timer_ = std::move(*timer);
    timerArmed_ = true;
  }

  void poll() {
    auto state = state_.lock();

    if (!state || !state->visible || !state->followTail || busy_ || failed_ || !latest_) {
      return;
    }

    auto credit = source_->budget()->reserve({8192, 0, 1, 0});

    if (!credit) {
      fail(credit.error());

      return;
    }

    auto completion = std::make_shared<ExternalLogCompletion>();

    completion->generation = generation_;
    completion->credit = std::move(*credit);
    auto work = std::make_unique<ExternalLogMetadataJob>(data_->file, connection_.publisher, std::move(completion));
    auto admitted = latest_->replace(std::move(work));

    if (!admitted) {
      failBackground(admitted.error());
    } else {
      busy_ = true;
      metadataPending_ = true;
    }
  }

  void prepare() {
    auto state = state_.lock();

    if (!state || !state->visible || state->viewport.width <= 0 || state->viewport.height <= 0) {
      timer_ = {};
      timerArmed_ = false;
      if (busy_) {
        if (!metadataPending_) {
          resumeIntent_ = lastIntent_;
        }

        (void)consumer_.cancel();

        if (latest_) {
          latest_->cancel();
        }

        busy_ = false;
        metadataPending_ = false;
        completion_.reset();
        ++generation_;
      }

      return;
    }

    if (resumeIntent_) {
      const auto intent = *std::exchange(resumeIntent_, {});

      request(intent.anchor, intent.direction, intent.accumulate, intent.readingAnchor, intent.readingDelta);

      return;
    }

    if (std::exchange(state->navigationChanged, false)) {
      supersede();
    }

    auto completion = std::exchange(completion_, {});

    if (completion) {
      busy_ = false;

      if (completion->restoreRequest) {
        if (!restoreRequest_ || completion->restoreRequest->sequence != restoreRequest_->sequence ||
            completion->restoreRequest->session != restoreRequest_->session ||
            completion->restoreRequest->bookmark.resource() != restoreRequest_->bookmark.resource()) {
          return;
        }

        const auto revision = source_->dataRevision();

        if (revision && *revision > restoreRequest_->revision) {
          restoreRequest_.reset();
          restoreBlocked_ = false;
          const auto retried = startRestore();

          if (!retried) {
            restoreAdmissionFailed(retried.error());
          }

          return;
        }

        navigation_->status = completion->restoreStatus;

        if (completion->restored && completion->restoreAnchor &&
            positionReplyMatches(*restoreRequest_, *completion->restored)) {
          const auto saved = *pendingRestore_;
          std::optional<ExternalLogAnchor> reading;

          if (completion->restored->position.kind == PositionKind::ITEM) {
            reading.emplace();
            reading->row = completion->restored->position.row;
            reading->firstByte = saved.byteInRecord;

            if (data_->file) {
              fileGeneration_ = reading->row.generation;
              state->fileGeneration = fileGeneration_;
            }
          }

          pendingRestore_.reset();
          restoreRequest_.reset();
          request(*completion->restoreAnchor, SourceDirection::FORWARD, false, reading);

          if (active() && busy_ && !failed_) {
            restoredReading_ = saved;
          }
        } else {
          restoreRequest_.reset();
          restoreBlocked_ = true;
          started_ = true;
          state->frame = completion->frame;
          state->requestedOffset = 0;
          (void)tree_.invalidate(node_, Invalidation::LAYOUT);
        }

        return;
      }

      if (completion->generationChanged) {
        replaceSession();

        return;
      }

      if (restoredReading_) {
        const auto revision = source_->dataRevision();

        if (revision && *revision > completion->readRevision) {
          pendingRestore_ = std::exchange(restoredReading_, {});
          const auto cancellingGeneration = generation_;

          (void)consumer_.cancel();

          if (!checkGeneration(cancellingGeneration)) {
            return;
          }

          const auto restored = startRestore();

          if (!restored) {
            restoreAdmissionFailed(restored.error());
          }

          return;
        }
        if (completion->source || completion->historyUnavailable) {
          pendingRestore_ = std::exchange(restoredReading_, {});
          restoreBlocked_ = true;
          navigation_->status = {PositionResolution::UNAVAILABLE, LogRestoreReason::HISTORY_UNAVAILABLE,
                                 completion->source};
          (void)consumer_.cancel();

          return;
        }
        if (completion->pending) {
          pendingRestore_ = std::exchange(restoredReading_, {});
          restoreBlocked_ = true;
          navigation_->status = {PositionResolution::PENDING, LogRestoreReason::NONE, {}};
          (void)consumer_.cancel();

          return;
        }
        if (completion->ready) {
          restoredReading_.reset();
        }
      }

      if (completion->metadata) {
        if (completion->metadata->generation != fileGeneration_) {
          replaceSession();
        } else {
          tail();
        }

        return;
      }

      if (completion->source) {
        if (*completion->source != SourceError::CANCELLED) {
          fail(*completion->source, completion->file, completion->text);
        }

        return;
      }

      if (completion->ready) {
        turnProgressRecords_ += completion->records;
        lastEdge_ = completion->edge;

        // A fresh navigation/session owns its empty result too. Only an empty
        // continuation at a boundary preserves the existing reading window.
        if (completion->replacesWindow || !completion->empty || completion->edge == SourceEdge::MORE || !state->frame) {
          state->frame = completion->frame;
        }
        if (data_->file) {
          fileGeneration_ = state->frame->before.token[1];
          state->fileGeneration = fileGeneration_;
          if (state->followTail) {
            observedExtent_ = state->frame->after.token[0];
          }
        }
        accumulate_ = true;

        state->requestedOffset =
          completion->empty && completion->edge != SourceEdge::MORE && !completion->replacesWindow ? state->offset
          : state->followTail || (backwards_ && !readingAnchor_) ? frameMaximum(*state)
                                                                 : state->frame->preferredOffset.value_or(0);

        if (readingAnchor_ && state->frame->anchorResolved && state->frame->preferredOffset) {
          readingAnchor_ = state->frame->anchors[*state->frame->preferredOffset];
          readingDelta_ = 0;
        }

        const auto demand = static_cast<std::size_t>(state->viewport.height) + OVERSCAN_ROWS +
                            (readingAnchor_ ? static_cast<std::size_t>(state->frame->preferredOffset.value_or(0)) : 0);

        if (state->frame->rows.size() >= demand && (!readingAnchor_ || state->frame->anchorResolved)) {
          (void)consumer_.cancel();
        }

        (void)tree_.invalidate(node_, Invalidation::LAYOUT);
      }

      auto pending = consumer_.takeRequest();

      if (!pending) {
        fail(pending.error());

        return;
      }

      if (*pending) {
        busy_ = true;
        dispatch(std::move(**pending));

        return;
      }

      auto lastError = consumer_.lastError();

      if (lastError && *lastError && **lastError == SourceError::CONTINUATION_LIMIT && state->frame &&
          turnProgressRecords_ == 0) {
        fail(SourceError::CONTINUATION_LIMIT);

        return;
      }
    }

    if (state->positionRequest) {
      SourceAnchor anchor;

      anchor.kind = SourceAnchorKind::CURSOR;
      anchor.cursor = {source_->stamp(), {*std::exchange(state->positionRequest, {}), 0}};
      state->followTail = false;
      request(anchor, SourceDirection::FORWARD, false);

      return;
    }

    if (state->byteRequest && fileGeneration_) {
      const auto byte = *std::exchange(state->byteRequest, {});

      if (byte == std::numeric_limits<std::uint64_t>::max()) {
        fail(SourceError::POSITION_EXHAUSTED);

        return;
      }

      state->followTail = false;
      request({SourceAnchorKind::ROW, {}, {byte + 1, fileGeneration_}}, SourceDirection::FORWARD, false);

      return;
    }

    if (state->tailRequested) {
      started_ = true;
      tail();

      return;
    }

    if (busy_ || failed_) {
      return;
    }

    if (pendingRestore_ && restoreBlocked_ && navigation_->status.resolution == PositionResolution::PENDING) {
      const auto revision = source_->dataRevision();

      if (revision && *revision > attemptedRestoreRevision_) {
        restoreBlocked_ = false;
      }
    }

    if (pendingRestore_ && !restoreBlocked_) {
      const auto restored = startRestore();

      if (!restored) {
        restoreAdmissionFailed(restored.error());
      }

      return;
    }
    if (restoreBlocked_) {
      return;
    }

    if (state->tailRequested || (!started_ && state->followTail && source_->capabilities().tail)) {
      started_ = true;
      tail();

      return;
    }

    if (!started_) {
      started_ = true;
      request({}, SourceDirection::FORWARD, false);

      return;
    }

    const auto frame = state->frame;

    if (!frame) {
      return;
    }

    if (width_ != state->viewport.width) {
      auto anchor = SourceAnchor{SourceAnchorKind::CURSOR, frame->before, {}};
      std::optional<ExternalLogAnchor> reading;

      if (!frame->anchors.empty()) {
        reading = frame->anchors[std::min<std::uint64_t>(state->offset, frame->anchors.size() - 1)];

        if (data_->finiteRows) {
          anchor.cursor = {source_->stamp(), {reading->ordinal, 0}};
        } else if (source_->capabilities().seekRow) {
          anchor = {SourceAnchorKind::ROW, {}, reading->row};
        }
      }

      request(anchor, SourceDirection::FORWARD, false, reading);

      return;
    }

    const auto target = static_cast<std::size_t>(state->viewport.height) + OVERSCAN_ROWS +
                        (readingAnchor_ ? static_cast<std::size_t>(frame->preferredOffset.value_or(0)) : 0);

    // Source fairness limits one automatic fill to four pages. A new bounded turn continues
    // from its explicit gap, so one-row providers can still fill a twenty-row viewport.
    if (fillAllowed_ && (frame->rows.size() < target || (readingAnchor_ && !frame->anchorResolved)) &&
        lastEdge_ == SourceEdge::MORE && frame->records < config_.limits.maxLines) {
      const auto cursor = backwards_ ? frame->before : frame->after;

      request({SourceAnchorKind::CURSOR, cursor, {}}, backwards_ ? SourceDirection::BACKWARD : SourceDirection::FORWARD,
              true, readingAnchor_, readingDelta_);

      return;
    }

    armPoll();
  }

  static std::uint64_t frameMaximum(const ExternalLogState &state) {
    const auto rows = state.frame ? state.frame->rows.size() : 0;
    const auto height = static_cast<std::size_t>(std::max(state.viewport.height, 0));

    return rows > height ? rows - height : 0;
  }

  void replaceSession() {
    auto state = state_.lock();
    auto budget = source_->budget();
    auto checkpoint = snapshotCheckpoint();
    const auto replacingGeneration = generation_;

    if (latest_) {
      latest_->cancel();
    }

    if (!checkGeneration(replacingGeneration)) {
      return;
    }

    consumer_.close();

    if (!checkGeneration(replacingGeneration)) {
      return;
    }

    source_->close();

    if (!checkGeneration(replacingGeneration)) {
      return;
    }
    data_->budget = std::move(budget);
    fileGeneration_ = 0;
    auto connected = connect(data_, false);

    if (!connected) {
      fail(connected.error());

      return;
    }

    started_ = false;
    busy_ = false;
    failed_ = false;
    completion_.reset();
    ++generation_;

    if (!state->followTail && checkpoint && checkpoint->bookmark) {
      primeRestore(std::move(*checkpoint));
      state->frame.reset();
    }

    (void)tree_.invalidate(node_, Invalidation::LAYOUT);
  }

  struct Binding {
    SourceCredit readinessCredit;
    std::shared_ptr<const WindowNotifier<LogLine>> readiness;
    std::unique_ptr<WindowSource<LogLine>> source;
    WindowConsumer<LogLine> consumer;
  };

  std::expected<Binding, SourceError> prepareBinding(const std::shared_ptr<ui::detail::LogSourceData> &data) {
    WindowSourceOptions options;

    options.maxRowBytes = config_.limits.maxRecordBytes + sizeof(WindowRow<LogLine>);
    options.maxPageRows = std::min<std::size_t>(config_.limits.maxLines, 128);
    options.initialRevision = data->initialRevision;

    auto source = WindowSource<LogLine>::create(tree_, node_, data->provider, options, data->budget);

    if (!source) {
      return std::unexpected(source.error());
    }

    auto consumer = (*source)->openConsumer();

    if (!consumer) {
      return std::unexpected(consumer.error());
    }

    constexpr std::size_t READINESS_ALLOCATION_ALLOWANCE = 128;
    auto credit =
      (*source)->budget()->reserve({sizeof(WindowNotifier<LogLine>) + READINESS_ALLOCATION_ALLOWANCE, 0, 0, 0});

    if (!credit) {
      return std::unexpected(credit.error());
    }

    auto readiness = std::make_shared<const WindowNotifier<LogLine>>((*source)->notifier());

    return Binding{std::move(*credit), std::move(readiness), std::move(*source), std::move(*consumer)};
  }

  WidgetTree &tree_;
  Node<LogView> node_;
  std::weak_ptr<const void> lifetime_;
  std::weak_ptr<ExternalLogState> state_;
  WidthPolicy policy_;
  LogViewConfig config_;
  std::shared_ptr<LogNavigationSession> navigation_;
  std::optional<LogReadingCheckpoint> pendingRestore_;
  std::optional<PositionRequest> restoreRequest_;
  std::optional<LogReadingCheckpoint> restoredReading_;
  bool restoreBlocked_{};
  std::uint64_t attemptedRestoreRevision_{};
  std::shared_ptr<ui::detail::LogSourceData> data_;
  std::function<BackgroundExecutor *()> executor_;
  std::function<SchedulerTime()> now_;
  std::function<void(ManagedError)> report_;
  SourceCredit readinessCredit_;
  std::shared_ptr<const WindowNotifier<LogLine>> readiness_;
  std::unique_ptr<WindowSource<LogLine>> source_;
  WindowConsumer<LogLine> consumer_;
  SchedulerConnection<ExternalLogCompletion> connection_;
  std::unique_ptr<BackgroundLatest> latest_;
  std::shared_ptr<const ExternalLogCompletion> completion_;
  std::uint64_t generation_{};
  std::uint64_t fileGeneration_{};
  std::uint64_t observedExtent_{};
  std::size_t turnProgressRecords_{};
  Subscription timer_;
  std::int32_t width_{};
  bool closed_{};
  bool busy_{};
  bool failed_{};
  bool started_{};
  bool accumulate_{};
  bool backwards_{};
  bool timerArmed_{};
  bool metadataPending_{};
  bool fillAllowed_{true};
  SourceEdge lastEdge_{SourceEdge::MORE};
  std::optional<ExternalLogAnchor> readingAnchor_;
  std::int64_t readingDelta_{};
  std::optional<ExternalLogIntent> lastIntent_;
  std::optional<ExternalLogIntent> resumeIntent_;
};
} // namespace

std::expected<void, ManagedError> replaceExternalLogView(ManagedScene &scene, Node<LogView> node,
                                                         ui::LogSource source) {
  const auto data = ui::detail::ExternalLogAccess::data(source);

  if (!data || !data->provider) {
    auto failure = sourceFailure(node, data ? data->error : ui::detail::ExternalLogAccess::error(source));

    failure.logFile = data ? data->failure : std::nullopt;

    return std::unexpected(failure);
  }

  auto &tree = SceneAccess::application(scene).tree();
  std::shared_ptr<ExternalLogState> external;
  LogViewConfig config;
  const auto read = tree.read(node, [&](const LogView &widget) {
    const auto &state = LogViewAccess::state(widget);

    external = state.external;
    config = state.config;
  });

  if (!read) {
    return std::unexpected(sourceFailure(node, sourceTreeError(read.error())));
  }

  if (!external) {
    return attachExternalLogView(scene, node, std::move(source), config);
  }

  try {
    const auto activity = std::static_pointer_cast<ExternalLogActivity>(external->activity);
    const auto replaced = activity->replace(data);

    return replaced ? std::expected<void, ManagedError>{} : std::unexpected(sourceFailure(node, replaced.error()));
  } catch (const std::bad_alloc &) {
    return std::unexpected(sourceFailure(node, SourceError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> attachExternalLogView(ManagedScene &scene, Node<LogView> node, ui::LogSource source,
                                                        LogViewConfig config, LogNavigation navigation,
                                                        LogCheckpointOptions checkpoints) {
  auto data = ui::detail::ExternalLogAccess::data(source);

  if (!data || !data->provider) {
    auto failure = sourceFailure(node, data ? data->error : ui::detail::ExternalLogAccess::error(source));

    failure.phase = ManagedPhase::MOUNT;
    failure.logFile = data ? data->failure : std::nullopt;

    return std::unexpected(failure);
  }

  auto allowed = SceneAccess::requireBackground(scene);

  if (!allowed) {
    return allowed;
  }

  try {
    if (ui::detail::ExternalLogAccess::empty(navigation)) {
      auto created = scene.logNavigation(checkpoints);

      if (!created) {
        return std::unexpected(created.error());
      }

      navigation = *created;
      (*ui::detail::ExternalLogAccess::navigation(navigation))->implicit = true;
    }

    auto session = ui::detail::ExternalLogAccess::navigation(navigation);

    if (!session) {
      return std::unexpected(sourceFailure(node, session.error()));
    }
    if ((*session)->scene != scene.root().id()) {
      return std::unexpected(sourceFailure(node, SourceError::CONTEXT_ERROR));
    }
    if (!(*session)->binding.expired()) {
      return std::unexpected(sourceFailure(node, SourceError::WOULD_BLOCK));
    }

    auto state = std::make_shared<ExternalLogState>();
    auto empty = std::make_unique<LogData>();
    auto lifetime = ui::detail::Access::lifetime(scene);
    auto &app = SceneAccess::application(scene);
    auto activity = std::make_shared<ExternalLogActivity>(
      app.tree(), node, lifetime, state, scene.widthPolicy(), config, *session,
      [&app, lifetime] {
        return lifetime.expired() ? nullptr : app.background();
      },
      [&app, lifetime] {
        return lifetime.expired() ? SchedulerTime{} : app.now();
      },
      [&scene, lifetime](ManagedError failure) {
        if (!lifetime.expired()) {
          (void)scene.reportError(std::move(failure));
        }
      });
    auto connected = activity->connect(data);

    if (!connected) {
      return std::unexpected(sourceFailure(node, connected.error()));
    }

    const auto invalidated = app.tree().invalidate(node, Invalidation::LAYOUT);

    if (!invalidated) {
      return std::unexpected(sourceFailure(node, sourceTreeError(invalidated.error())));
    }

    auto admitted = SceneAccess::admitActivity(scene, activity);

    if (!admitted) {
      return admitted;
    }

    state->activity = std::move(activity);
    (*session)->binding = state->activity;
    const auto *saved = (*session)->find(data->resource);

    state->followTail = saved ? saved->value.following && data->provider->capabilities().tail
                              : config.followTail && data->provider->capabilities().tail;
    state->requestedOffset = 0;
    state->finiteRows = data->finiteRows;
    state->fileSource = static_cast<bool>(data->file);
    state->navigation = std::move(navigation);

    auto installed = app.tree().edit(node, [&](LogView &widget) {
      auto &log = LogViewAccess::state(widget);

      const auto intent = ScrollWidgetAccess::state(widget).intent.lock();

      state->intentSerial = intent ? intent->serial : 0;
      log.data = std::move(empty);
      log.external = std::move(state);
    });

    return installed ? std::expected<void, ManagedError>{}
                     : std::unexpected(sourceFailure(node, sourceTreeError(installed.error())));
  } catch (const std::bad_alloc &) {
    return std::unexpected(sourceFailure(node, SourceError::NO_MEMORY));
  }
}
} // namespace portals::detail

namespace portals {
std::expected<LogNavigation, SourceError> logNavigation(WidgetTree &tree, Node<LogView> node) {
  std::optional<LogNavigation> result;
  const auto read = tree.read(node, [&](const LogView &widget) {
    const auto &state = detail::LogViewAccess::state(widget);

    if (state.external) {
      result = state.external->navigation;
    }
  });

  if (!read) {
    return std::unexpected(detail::sourceTreeError(read.error()));
  }

  if (result) {
    auto session = ui::detail::ExternalLogAccess::navigation(*result);

    if (!session) {
      return std::unexpected(session.error());
    }

    (*session)->implicit = false;
  }

  return result ? std::expected<LogNavigation, SourceError>(*result) : std::unexpected(SourceError::UNSUPPORTED);
}

std::expected<void, SourceError> seekLogRow(WidgetTree &tree, Node<LogView> node, std::uint64_t ordinal) {
  std::expected<void, SourceError> result;
  const auto edited = tree.edit(node, [&](LogView &widget) {
    auto state = detail::LogViewAccess::state(widget).external;

    if (!state || !state->finiteRows) {
      result = std::unexpected(SourceError::UNSUPPORTED);
    } else if (ordinal >= *state->finiteRows) {
      result = std::unexpected(SourceError::INVALID_ARGUMENT);
    } else {
      state->positionRequest = ordinal;
    }
  });

  if (!edited) {
    return std::unexpected(detail::sourceTreeError(edited.error()));
  }

  if (!result) {
    return result;
  }

  auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

  return invalidated ? std::expected<void, SourceError>{}
                     : std::unexpected(detail::sourceTreeError(invalidated.error()));
}

std::expected<void, SourceError> seekLogByte(WidgetTree &tree, Node<LogView> node, std::uint64_t byteOffset) {
  std::expected<void, SourceError> result;
  const auto edited = tree.edit(node, [&](LogView &widget) {
    auto state = detail::LogViewAccess::state(widget).external;

    if (!state || !state->fileSource) {
      result = std::unexpected(SourceError::UNSUPPORTED);
    } else if (byteOffset == std::numeric_limits<std::uint64_t>::max()) {
      result = std::unexpected(SourceError::POSITION_EXHAUSTED);
    } else {
      state->byteRequest = byteOffset;
    }
  });

  if (!edited) {
    return std::unexpected(detail::sourceTreeError(edited.error()));
  }

  if (!result) {
    return result;
  }

  auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

  return invalidated ? std::expected<void, SourceError>{}
                     : std::unexpected(detail::sourceTreeError(invalidated.error()));
}
} // namespace portals
