// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "log_view_state.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace portals {
namespace {
constexpr std::size_t MAX_LINES = 8192;
constexpr std::size_t MAX_BYTES = 4 * 1024 * 1024;
constexpr std::size_t MAX_BATCH_BYTES = 256 * 1024;
constexpr std::size_t MAX_RECORD_BYTES = 64 * 1024;

ScrollAreaOptions logScrollOptions(LogViewConfig config) {
  ScrollAreaOptions options;

  options.horizontal = ScrollBarPolicy::NEVER;
  options.vertical = ScrollBarPolicy::AUTO;
  options.placement = ScrollPlacement::ON_FRAME;
  options.frame = BorderOptions{};
  options.readOnly = config.readOnly;

  return options;
}

void addCount(std::uint64_t &target, bool &saturated, std::size_t amount) noexcept {
  constexpr auto MAX_COUNT = std::numeric_limits<std::uint64_t>::max();

  if (amount >= MAX_COUNT - target) {
    target = MAX_COUNT;
    saturated = true;
  } else {
    target += amount;
  }
}

std::unique_ptr<GraphemeText> footerText(const LogStatus &status, WidthPolicy width) {
  std::array<char, 192> buffer{};

  if (status.followTail) {
    std::snprintf(buffer.data(), buffer.size(), "%zu lines | dropped %s%llu | following", status.retainedLines,
                  status.droppedSaturated ? ">=" : "", static_cast<unsigned long long>(status.droppedLines));
  } else {
    std::snprintf(buffer.data(), buffer.size(), "Return to tail | %zu lines | dropped %s%llu | paused +%s%llu",
                  status.retainedLines, status.droppedSaturated ? ">=" : "",
                  static_cast<unsigned long long>(status.droppedLines), status.newWhilePausedSaturated ? ">=" : "",
                  static_cast<unsigned long long>(status.newWhilePaused));
  }

  auto text = GraphemeText::from(buffer.data(), width);

  if (!text) {
    throw std::invalid_argument("invalid log width policy");
  }

  return std::make_unique<GraphemeText>(std::move(*text));
}

struct LogEdit {
  detail::LogViewState &state;

  explicit LogEdit(detail::LogViewState &value) : state(value) {
    state.editing = true;
  }

  ~LogEdit() {
    state.editing = false;
  }
};

std::expected<void, TreeError> admittedLog(const detail::LogViewState &log, const detail::ScrollWidgetState &scroll) {
  if (log.editing) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (auto valid = detail::validateLogViewState(log, scroll); !valid) {
    return valid;
  }

  if (!scroll.lease || scroll.retired || scroll.intent.expired()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return {};
}

std::uint64_t intentSerial(const detail::ScrollWidgetState &scroll) noexcept {
  const auto intent = scroll.intent.lock();

  return intent ? intent->serial : 0;
}

void anchorAt(detail::LogData &data, std::uint64_t row) noexcept {
  if (data.rows.empty()) {
    data.anchor = {};

    return;
  }

  const auto &selected = data.rows[std::min<std::uint64_t>(row, data.rows.size() - 1)];

  data.anchor = {data.records[selected.record]->id, selected.byteBegin};
}

struct LogVersion {
  std::uint64_t revision;
  std::uint64_t serial;
};

struct LogNavigation {
  LogStatus status;
  detail::LogAnchor anchor;
  std::uint64_t serial;
  LogVersion version;
};

LogNavigation navigationValues(const detail::LogData &source, const detail::ScrollWidgetState &scroll,
                               const ScrollControllerSnapshot &position) noexcept {
  const auto serial = intentSerial(scroll);
  LogNavigation candidate{source.status, source.anchor, source.intentSerial, {position.revision, serial}};

  if (serial == source.intentSerial || position.vertical.metrics.viewport == 0 || source.width <= 0) {
    return candidate;
  }

  candidate.serial = serial;
  candidate.status.followTail = position.vertical.offset.raster == scrollRange(position.vertical.metrics).maximum;

  if (candidate.status.followTail || source.status.followTail) {
    candidate.status.newWhilePaused = 0;
    candidate.status.newWhilePausedSaturated = false;
  }

  if (!candidate.status.followTail) {
    const auto row =
      std::min<std::uint64_t>(position.vertical.offset.raster, source.rows.empty() ? 0 : source.rows.size() - 1);

    if (!source.rows.empty()) {
      const auto &selected = source.rows[row];

      candidate.anchor = {source.records[selected.record]->id, selected.byteBegin, position.vertical.offset.fraction};
    }
  }

  return candidate;
}

std::expected<void, TreeError> checkLogVersion(WidgetTree &tree, Node<Widget> node,
                                               const detail::ScrollWidgetState &scroll, LogVersion version) {
  std::expected<void, TreeError> status;
  // Re-admit the owner before comparing versions, preserving exact retirement/frame errors.
  const auto checked = tree.edit(node, [&](Widget &) {
    const auto current = scroll.controller->snapshot();

    if (!current) {
      status = std::unexpected(current.error());
    } else if (current->revision != version.revision || intentSerial(scroll) != version.serial) {
      status = std::unexpected(TreeError::WOULD_BLOCK);
    }
  });

  return checked ? status : checked;
}

void adoptNavigation(detail::LogData &data, LogNavigation navigation) noexcept {
  data.status = navigation.status;
  data.anchor = navigation.anchor;
  data.intentSerial = navigation.serial;
}

std::unique_ptr<detail::LogData> copyValues(const detail::LogData &source) {
  auto result = std::make_unique<detail::LogData>();

  result->status = source.status;
  result->anchor = source.anchor;
  result->nextID = source.nextID;
  result->intentSerial = source.intentSerial;
  result->width = source.width;

  return result;
}

template <class Consume>
void visitRows(const detail::LogData &data, std::int32_t width, std::size_t firstRecord, Consume consume) {
  for (std::size_t record = firstRecord; record < data.records.size(); ++record) {
    const auto &text = data.records[record]->text;
    const auto clusters = text.graphemes();
    std::size_t begin{};

    if (clusters.empty()) {
      consume(record, 0, 0);

      continue;
    }

    while (begin < clusters.size()) {
      std::size_t end = begin;
      std::int32_t cells{};

      // A too-wide cluster is consumed once and clipped by the existing renderer.
      do {
        cells += clusters[end++].cells;
      } while (end < clusters.size() && cells + clusters[end].cells <= width);

      consume(record, clusters[begin].byte_begin, clusters[end - 1].byte_end);
      begin = end;
    }
  }
}

void wrapRows(detail::LogData &data, std::int32_t width) {
  data.width = width;

  if (width <= 0) {
    return;
  }

  std::size_t count{};

  visitRows(data, width, 0, [&](auto, auto, auto) {
    ++count;
  });
  data.rows.reserve(count);
  visitRows(data, width, 0, [&](std::size_t record, std::size_t begin, std::size_t end) {
    data.rows.push_back({record, begin, end});
  });
}

void appendRows(detail::LogData &candidate, const detail::LogData &source, std::size_t droppedLines) {
  if (source.width <= 0) {
    return;
  }

  const auto droppedRecords = std::min(droppedLines, source.records.size());
  const auto retainedRecords = source.records.size() - droppedRecords;
  const auto first = std::ranges::lower_bound(source.rows, droppedRecords, {}, &detail::LogRow::record);
  std::size_t count = static_cast<std::size_t>(source.rows.end() - first);

  // Old records retain their validated wrap at this width; only the new suffix visits graphemes.
  visitRows(candidate, source.width, retainedRecords, [&](auto, auto, auto) {
    ++count;
  });
  candidate.rows.reserve(count);

  for (auto row = first; row != source.rows.end(); ++row) {
    candidate.rows.push_back({row->record - droppedRecords, row->byteBegin, row->byteEnd});
  }

  visitRows(candidate, source.width, retainedRecords, [&](std::size_t record, std::size_t begin, std::size_t end) {
    candidate.rows.push_back({record, begin, end});
  });
}

std::uint64_t anchorRow(const detail::LogData &data, detail::LogAnchor anchor) noexcept {
  const auto record = std::ranges::find_if(data.records, [&](const auto &value) {
    return value->id == anchor.record;
  });

  if (record == data.records.end()) {
    return 0;
  }

  const auto index = static_cast<std::size_t>(record - data.records.begin());
  std::uint64_t selected{};

  for (std::size_t row = 0; row < data.rows.size(); ++row) {
    const auto &value = data.rows[row];

    if (value.record == index && value.byteBegin <= anchor.byte) {
      selected = row;
    }

    if (value.record > index || (value.record == index && value.byteEnd > anchor.byte)) {
      break;
    }
  }

  return selected;
}

std::uint64_t preparedOffset(const detail::LogData &data, LogStatus status, detail::LogAnchor anchor,
                             std::int32_t height) noexcept {
  if (height <= 0) {
    return 0;
  }

  const auto maximum = data.rows.size() > static_cast<std::size_t>(height) ? data.rows.size() - height : 0;

  return status.followTail ? maximum : std::min<std::uint64_t>(anchorRow(data, anchor), maximum);
}

std::size_t paintRowCount(const detail::LogData &data, std::uint64_t first, std::int32_t height) noexcept {
  return height > 0 && data.width > 0
           ? std::min<std::uint64_t>(height, data.rows.size() - std::min<std::uint64_t>(first, data.rows.size()))
           : 0;
}

bool cacheMatches(const detail::LogData &data, std::uint64_t first, std::int32_t height) noexcept {
  return data.paint && data.paint->first == first && data.paint->height == height &&
         data.paint->lines.size() == paintRowCount(data, first, height);
}

std::unique_ptr<detail::LogPaintCache> preparePaintRows(const detail::LogData &data, WidthPolicy policy,
                                                        std::int32_t height, LogStatus status,
                                                        detail::LogAnchor anchor) {
  auto cache = std::make_unique<detail::LogPaintCache>();

  cache->first = preparedOffset(data, status, anchor, height);
  cache->height = height;

  const auto count = paintRowCount(data, cache->first, height);

  cache->lines.reserve(count);

  for (std::size_t offset = 0; offset < count; ++offset) {
    const auto &row = data.rows[cache->first + offset];
    const auto bytes = data.records[row.record]->text.bytes().substr(row.byteBegin, row.byteEnd - row.byteBegin);
    auto text = GraphemeText::from(bytes, policy);

    // Cache only the intersecting rows, cut at complete already validated source clusters.
    if (!text) {
      throw std::logic_error("invalid prepared log row");
    }

    cache->lines.push_back(std::move(*text));
  }

  return cache;
}

std::expected<LogAppendReceipt, LogError> admitBatch(detail::LogData &candidate, const detail::LogData &source,
                                                     std::string_view bytes, LogLimits limits, WidthPolicy policy) {
  if (bytes.size() > limits.maxBatchBytes) {
    return std::unexpected(TextError{TextErrorKind::INPUT_LIMIT, 0});
  }

  // Fixed-capacity ring staging never retains a reference array larger than maxLines.
  candidate.records.resize(limits.maxLines);
  std::ranges::copy(source.records, candidate.records.begin());

  std::size_t head{};
  std::size_t count = source.records.size();
  std::size_t payload = source.status.retainedBytes;
  LogAppendReceipt receipt;
  std::size_t begin{};

  while (begin < bytes.size()) {
    const auto separator = bytes.find_first_of("\r\n", begin);
    const auto end = separator == std::string_view::npos ? bytes.size() : separator;
    const auto recordBytes = bytes.substr(begin, end - begin);

    if (recordBytes.size() > limits.maxRecordBytes) {
      return std::unexpected(TextError{TextErrorKind::INPUT_LIMIT, 0});
    }

    if (!candidate.nextID) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    TextLimits textLimits;

    textLimits.max_input_bytes = limits.maxRecordBytes;

    auto text = GraphemeText::from(recordBytes, policy, textLimits);

    if (!text) {
      auto failure = text.error();

      failure.byte_offset += begin;

      return std::unexpected(failure);
    }

    auto record = std::make_shared<detail::LogRecord>(detail::LogRecord{candidate.nextID, std::move(*text)});

    candidate.nextID = candidate.nextID == std::numeric_limits<std::uint64_t>::max() ? 0 : candidate.nextID + 1;
    ++receipt.acceptedLines;

    while (count && (count == limits.maxLines || recordBytes.size() > limits.maxBytes - payload)) {
      payload -= candidate.records[head]->text.bytes().size();
      candidate.records[head].reset();
      head = (head + 1) % limits.maxLines;
      --count;
      ++receipt.droppedLines;
    }

    candidate.records[(head + count) % limits.maxLines] = std::move(record);
    payload += recordBytes.size();
    ++count;
    begin = end;

    if (begin < bytes.size()) {
      const auto first = bytes[begin++];

      if (first == '\r' && begin < bytes.size() && bytes[begin] == '\n') {
        ++begin;
      }
    }
  }

  std::rotate(candidate.records.begin(), candidate.records.begin() + static_cast<std::ptrdiff_t>(head),
              candidate.records.end());
  candidate.records.resize(count);
  candidate.status.retainedLines = count;
  candidate.status.retainedBytes = payload;
  addCount(candidate.status.droppedLines, candidate.status.droppedSaturated, receipt.droppedLines);

  if (!candidate.status.followTail) {
    addCount(candidate.status.newWhilePaused, candidate.status.newWhilePausedSaturated, receipt.acceptedLines);
  }

  receipt.retainedLines = count;
  receipt.retainedBytes = payload;

  return receipt;
}

template <class Function>
std::expected<void, TreeError> changeLog(WidgetTree &tree, Node<Widget> node, Function function) {
  std::expected<void, TreeError> result;
  const auto edited = tree.edit(node, [&](Widget &value) {
    auto *widget = dynamic_cast<LogView *>(&value);

    if (!widget) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);

      return;
    }

    auto &state = detail::LogViewAccess::state(*widget);
    const auto &scroll = detail::ScrollWidgetAccess::state(*widget);

    result = admittedLog(state, scroll);

    if (!result) {
      return;
    }

    LogEdit guard(state);

    try {
      result = function(state, scroll);
    } catch (const std::bad_alloc &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    }
  });

  return edited ? result : edited;
}

std::expected<void, TreeError> requestTail(WidgetTree &tree, Node<Widget> node) {
  return changeLog(tree, node, [&](detail::LogViewState &state, const detail::ScrollWidgetState &scroll) {
    if (state.data->status.followTail && state.data->intentSerial == intentSerial(scroll)) {
      return std::expected<void, TreeError>{};
    }

    auto status = state.data->status;

    status.followTail = true;
    status.newWhilePaused = 0;
    status.newWhilePausedSaturated = false;

    auto footer = footerText(status, state.width);
    auto paint =
      preparePaintRows(*state.data, state.width, scroll.geometry.viewport.height, status, state.data->anchor);
    const auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

    if (invalidated) {
      state.data->footer.swap(footer);
      state.data->paint.swap(paint);
      state.data->status = status;
      state.data->intentSerial = intentSerial(scroll);
    }

    return invalidated;
  });
}

std::expected<bool, TreeError> refreshLogPaint(WidgetTree &tree, Node<Widget> node, detail::LogViewState &state,
                                               const detail::ScrollWidgetState &scroll, LogNavigation navigation,
                                               std::int32_t height) {
  const auto first = preparedOffset(*state.data, navigation.status, navigation.anchor, height);
  const bool changed = navigation.serial != state.data->intentSerial;
  const bool paintChanged = !cacheMatches(*state.data, first, height);

  if (!changed && !paintChanged) {
    return false;
  }

  auto paint =
    paintChanged ? preparePaintRows(*state.data, state.width, height, navigation.status, navigation.anchor) : nullptr;
  auto footer = changed ? footerText(navigation.status, state.width) : nullptr;
  const auto current = checkLogVersion(tree, node, scroll, navigation.version);

  if (!current) {
    return std::unexpected(current.error());
  }

  const auto invalidated = tree.invalidate(node, Invalidation::PAINT);

  if (!invalidated) {
    return std::unexpected(invalidated.error());
  }

  if (paint) {
    state.data->paint.swap(paint);
  }

  if (footer) {
    state.data->footer.swap(footer);
  }

  adoptNavigation(*state.data, navigation);

  return false;
}

std::expected<bool, TreeError> rewrapLog(WidgetTree &tree, Node<Widget> node, detail::LogViewState &state,
                                         const detail::ScrollWidgetState &scroll, LogNavigation navigation,
                                         CellSize viewport) {
  auto candidate = copyValues(*state.data);

  adoptNavigation(*candidate, navigation);
  candidate->records = state.data->records;
  wrapRows(*candidate, viewport.width);

  if (!candidate->status.followTail && !candidate->anchor.record) {
    anchorAt(*candidate, 0);
  }

  candidate->footer = footerText(candidate->status, state.width);
  candidate->paint = preparePaintRows(*candidate, state.width, viewport.height, candidate->status, candidate->anchor);

  const auto current = checkLogVersion(tree, node, scroll, navigation.version);

  if (!current) {
    return std::unexpected(current.error());
  }

  const auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

  if (!invalidated) {
    return std::unexpected(invalidated.error());
  }

  state.data.swap(candidate);

  return true;
}
} // namespace

std::expected<void, TreeError> validateLogLimits(LogLimits limits) noexcept {
  if (!limits.maxLines || limits.maxLines > MAX_LINES || !limits.maxBytes || limits.maxBytes > MAX_BYTES ||
      !limits.maxBatchBytes || limits.maxBatchBytes > MAX_BATCH_BYTES || !limits.maxRecordBytes ||
      limits.maxRecordBytes > MAX_RECORD_BYTES || limits.maxRecordBytes > limits.maxBytes ||
      limits.maxRecordBytes > limits.maxBatchBytes) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return {};
}

detail::LogViewState::LogViewState(WidthPolicy policy, LogViewConfig options)
    : width(policy), config(options), data(std::make_unique<LogData>()) {
  data->status.followTail = config.followTail;
  data->footer = footerText(data->status, width);
}

std::expected<void, TreeError> detail::validateLogViewState(const LogViewState &log, const ScrollWidgetState &scroll) {
  const auto limits = validateLogLimits(log.config.limits);

  if (!limits) {
    return limits;
  }

  const auto *frame = scrollFrameOptions(scroll);

  if (!frame || !frame->sides.right || !frame->sides.bottom) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return validateScrollWidget(scroll);
}

LogView::LogView(WidthPolicy width, LogViewConfig config, ControlStyle style, SizePolicy policy)
    : ScrollArea({}, width, logScrollOptions(config), style, policy),
      log_(std::make_unique<detail::LogViewState>(width, config)) {
}

LogView::~LogView() = default;

LogView::LogView(WidthPolicy width, LogViewConfig config, ControlStyle style, SizePolicy policy, FrameOptions frame)
    : ScrollArea({}, width, logScrollOptions(config), style, policy, std::move(frame), std::in_place),
      log_(std::make_unique<detail::LogViewState>(width, config)) {
}

std::expected<CellSize, TreeError> LogView::measure(CellSize available) {
  const auto valid = admittedLog(*log_, detail::ScrollWidgetAccess::state(*this));

  return valid ? ScrollArea::measure(available) : std::unexpected(valid.error());
}

std::expected<CellSize, TreeError> LogView::measureChildren(std::span<const MeasuredChild> children) const {
  if (!children.empty()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto &data = *log_->data;
  const auto insets = detail::scrollFrameInsets(detail::ScrollWidgetAccess::state(*this));
  const auto &scroll = detail::ScrollWidgetAccess::state(*this);

  if (data.rows.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - insets.top - insets.bottom) ||
      data.width > std::numeric_limits<std::int32_t>::max() - insets.left - insets.right) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  scroll.extent = {static_cast<std::uint64_t>(std::max(data.width, 0)), data.rows.size()};

  return CellSize{
    std::max(std::max(data.width, 0) + insets.left + insets.right, computedStyle() ? computedStyle()->minWidth : 0),
    static_cast<std::int32_t>(data.rows.size()) + insets.top + insets.bottom};
}

std::expected<LogAppendReceipt, LogError> appendLogBatch(WidgetTree &tree, Node<LogView> node, std::string_view bytes) {
  std::expected<LogAppendReceipt, LogError> result{std::unexpected(TreeError::INVALID_ARGUMENT)};
  const auto edited = tree.edit(node, [&](LogView &widget) {
    auto &state = detail::LogViewAccess::state(widget);
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);
    const auto valid = admittedLog(state, scroll);

    if (!valid) {
      result = std::unexpected(valid.error());

      return;
    }

    if (bytes.empty()) {
      result = LogAppendReceipt{0, 0, state.data->status.retainedLines, state.data->status.retainedBytes};

      return;
    }

    LogEdit guard(state);

    try {
      auto candidate = copyValues(*state.data);
      const auto position = scroll.controller->snapshot();

      if (!position) {
        result = std::unexpected(position.error());

        return;
      }

      const auto navigation = navigationValues(*state.data, scroll, *position);

      adoptNavigation(*candidate, navigation);
      result = admitBatch(*candidate, *state.data, bytes, state.config.limits, state.width);

      if (!result) {
        return;
      }

      appendRows(*candidate, *state.data, result->droppedLines);

      if (!candidate->status.followTail && std::ranges::none_of(candidate->records, [&](const auto &record) {
            return record->id == candidate->anchor.record;
          })) {
        anchorAt(*candidate, 0);
      }

      candidate->footer = footerText(candidate->status, state.width);
      candidate->paint = preparePaintRows(*candidate, state.width, scroll.geometry.viewport.height, candidate->status,
                                          candidate->anchor);

      const auto current = checkLogVersion(tree, node, scroll, navigation.version);

      if (!current) {
        result = std::unexpected(current.error());

        return;
      }

      // Invalidation validates UI/frame/liveness after all allocation and before publication.
      const auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

      if (!invalidated) {
        result = std::unexpected(invalidated.error());

        return;
      }

      state.data.swap(candidate);
    } catch (const std::bad_alloc &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    }
  });

  return edited ? result : std::unexpected(LogError{edited.error()});
}

std::expected<void, TreeError> clearLog(WidgetTree &tree, Node<LogView> node) {
  return changeLog(tree, node, [&](detail::LogViewState &state, const detail::ScrollWidgetState &scroll) {
    if (state.data->records.empty() && state.data->status.followTail && !state.data->status.droppedLines &&
        state.data->intentSerial == intentSerial(scroll)) {
      return std::expected<void, TreeError>{};
    }

    auto candidate = std::make_unique<detail::LogData>();

    candidate->nextID = state.data->nextID;
    candidate->intentSerial = intentSerial(scroll);
    candidate->width = state.data->width;
    candidate->footer = footerText(candidate->status, state.width);
    candidate->paint =
      preparePaintRows(*candidate, state.width, scroll.geometry.viewport.height, candidate->status, candidate->anchor);

    const auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

    if (invalidated) {
      state.data.swap(candidate);
    }

    return invalidated;
  });
}

std::expected<void, TreeError> followLogTail(WidgetTree &tree, Node<LogView> node) {
  return requestTail(tree, node);
}

std::expected<LogStatus, TreeError> logStatus(WidgetTree &tree, Node<LogView> node) {
  std::expected<LogStatus, TreeError> result{std::unexpected(TreeError::INVALID_ARGUMENT)};
  const auto edited = tree.edit(node, [&](LogView &widget) {
    const auto &state = detail::LogViewAccess::state(widget);
    const auto valid = admittedLog(state, detail::ScrollWidgetAccess::state(widget));

    result = valid ? std::expected<LogStatus, TreeError>(state.data->status) : std::unexpected(valid.error());
  });

  return edited ? result : std::unexpected(edited.error());
}

namespace detail {
std::expected<bool, TreeError> prepareLogView(WidgetTree &tree, Node<Widget> node, LogView &widget,
                                              const ScrollAreaLayoutSnapshot &layout) {
  auto &state = LogViewAccess::state(widget);
  const auto &scroll = ScrollWidgetAccess::state(widget);
  const auto valid = admittedLog(state, scroll);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  const auto viewport = layout.placement.viewport;

  if (!layout.visible) {
    return false;
  }

  const auto position = scroll.controller->snapshot();

  if (!position) {
    return std::unexpected(position.error());
  }

  const auto navigation = navigationValues(*state.data, scroll, *position);
  LogEdit guard(state);

  try {
    if (viewport.width <= 0) {
      // Sample pending deliberate intent before zero-viewport metrics clamp its canonical offset.
      return refreshLogPaint(tree, node, state, scroll, navigation, 0);
    }

    if (state.data->width != viewport.width) {
      return rewrapLog(tree, node, state, scroll, navigation, viewport);
    }

    return refreshLogPaint(tree, node, state, scroll, navigation, viewport.height);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> stageLogViewOffset(const LogView &widget, ScrollControllerPlan &plan,
                                                  const ScrollAreaLayoutSnapshot &layout) {
  if (!layout.visible || layout.placement.viewport.width <= 0 || layout.placement.viewport.height <= 0) {
    return {};
  }

  const auto &data = *LogViewAccess::state(widget).data;
  const auto offset = data.status.followTail ? scrollRange(plan.verticalMetrics).maximum : anchorRow(data, data.anchor);

  return ScrollControllerAccess::offset(plan, ScrollAxis::VERTICAL,
                                        {offset, data.status.followTail ? 0 : data.anchor.fraction});
}

std::expected<EventReply, TreeError> logViewInput(WidgetTree &tree, Node<Widget> node, LogView &widget,
                                                  const RoutedEvent &event) {
  const auto &scroll = ScrollWidgetAccess::state(widget);
  const auto &data = *LogViewAccess::state(widget).data;
  const auto *mouse = std::get_if<MouseInputEvent>(&event);

  if (scroll.area.readOnly || data.status.followTail || !mouse || mouse->action != MouseAction::PRESS ||
      mouse->button != MouseButton::LEFT) {
    return EventReply::IGNORED;
  }

  const auto geometry = tree.inspect(node);

  if (!geometry) {
    return std::unexpected(geometry.error());
  }

  const auto bounds = geometry->geometry.surface;

  if (!geometry->geometry.valid || mouse->row != bounds.bottom() - 1 || mouse->column <= bounds.origin().x ||
      mouse->column >= bounds.right() - 1) {
    return EventReply::IGNORED;
  }

  const auto result = requestTail(tree, node);

  return result ? std::expected<EventReply, TreeError>(EventReply::HANDLED) : std::unexpected(result.error());
}
} // namespace detail

std::expected<void, TreeError> LogView::paint(PaintContext &context) {
  const auto ready = admittedLog(*log_, detail::ScrollWidgetAccess::state(*this));

  if (!ready) {
    return ready;
  }

  const auto framed = ScrollArea::paint(context);
  const auto &scroll = detail::ScrollWidgetAccess::state(*this);

  if (!framed || !scroll.lease || scroll.retired || !scroll.arranged) {
    return framed;
  }

  const auto viewport = CellRect::from(scroll.geometry.viewportOrigin, scroll.geometry.viewport);

  if (!viewport || viewport->empty()) {
    return {};
  }

  std::optional<ComputedStyle> manualStyle;
  if (!computedStyle()) {
    manualStyle.emplace(styleDefaults());
  }
  const auto resolved =
    computedStyle() ? detail::ComputedStyleView(*computedStyle()) : detail::ComputedStyleView(*manualStyle);
  const auto style = !scroll.enabled ? resolved.disabled : scroll.focused ? resolved.focused : resolved.normal;
  const RenderCommand fill = FillCommand{*viewport, *viewport, style};
  auto painted = context.submit(std::span(&fill, 1));

  if (!painted) {
    return painted;
  }

  const auto &data = *log_->data;

  if (data.width != scroll.geometry.viewport.width ||
      !cacheMatches(data, scroll.presentation.vertical.offset.raster, scroll.geometry.viewport.height)) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  for (std::size_t row = 0; row < data.paint->lines.size(); ++row) {
    const RenderCommand command =
      TextCommand{{viewport->origin().x, viewport->origin().y + static_cast<std::int32_t>(row)},
                  data.paint->lines[row],
                  *viewport,
                  style};

    painted = context.submit(std::span(&command, 1));

    if (!painted) {
      return painted;
    }
  }

  const auto *frame = detail::scrollFrameOptions(scroll);

  if (frame && frame->sides.bottom && scroll.allocation.width > 2 && scroll.allocation.height > 1) {
    const auto footer = *CellRect::from({1, scroll.allocation.height - 1}, {scroll.allocation.width - 2, 1});
    const RenderCommand command = TextCommand{footer.origin(), *data.footer, footer, style};

    return context.submit(std::span(&command, 1));
  }

  return {};
}
} // namespace portals
