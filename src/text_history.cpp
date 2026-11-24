// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/text_history.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace portals {
namespace {
struct Entry {
  TextHistorySnapshot before;
  TextHistorySnapshot after;
  TextHistoryKind kind{};
  std::size_t retained{};
};

struct ContentStates {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries =
    std::vector<std::pair<std::uint64_t, std::uint64_t>>(0);
  std::uint64_t current{1};
};

struct OwnerToken {
  // Reuse the existing owner allocation. Preparation clones optional metadata;
  // only a fresh commit swaps it into this owner, without changing Entry layout.
  std::unique_ptr<ContentStates> content;
};

struct HistoryData {
  std::vector<std::shared_ptr<const Entry>> entries = std::vector<std::shared_ptr<const Entry>>(0);
  std::size_t cursor{};
  std::size_t retained{};
  bool groupOpen{};
};

std::expected<GraphemeText, TextHistoryError> validate(const TextHistorySnapshot &snapshot, WidthPolicy policy,
                                                       TextLimits limits) {
  auto text = GraphemeText::from(snapshot.bytes, policy, limits);

  if (!text) {
    return std::unexpected(TextHistoryError::INVALID_TEXT);
  }

  if (!text->nextCursor(snapshot.cursor) || !text->nextCursor(snapshot.anchor)) {
    return std::unexpected(TextHistoryError::NOT_BOUNDARY);
  }

  return std::move(*text);
}

bool sameSnapshot(const TextHistorySnapshot &left, const TextHistorySnapshot &right) noexcept {
  return left.bytes == right.bytes && left.cursor == right.cursor && left.anchor == right.anchor;
}

bool groupable(TextHistoryKind kind, const TextHistorySnapshot &before, const TextHistorySnapshot &after,
               TextHistoryOptions options) noexcept {
  if (before.cursor != before.anchor || after.cursor != after.anchor) {
    return false;
  }

  return (kind == TextHistoryKind::TYPING && options.groupTyping) ||
         ((kind == TextHistoryKind::BACKSPACE || kind == TextHistoryKind::FORWARD_DELETE) && options.groupDeletion);
}

std::expected<std::shared_ptr<const Entry>, TextHistoryError> makeEntry(const TextHistorySnapshot &before,
                                                                        const TextHistorySnapshot &after,
                                                                        TextHistoryKind kind, std::size_t budget,
                                                                        bool trackStates) {
  // MSVC Debug's basic_string move constructor is noexcept despite allocating
  // its iterator proxy. Construct counted empty strings, then copy-assign, so
  // a failed proxy allocation can propagate to the staging error handler.
  auto entry = std::make_shared<Entry>();

  entry->before = before;
  entry->after = after;
  entry->kind = kind;
  entry->retained = sizeof(Entry) + (trackStates ? sizeof(std::pair<std::uint64_t, std::uint64_t>) : 0);

  const auto beforeCapacity = entry->before.bytes.capacity();
  const auto afterCapacity = entry->after.bytes.capacity();

  if (entry->retained > budget || beforeCapacity > budget - entry->retained) {
    return std::unexpected(TextHistoryError::WOULD_BLOCK);
  }

  entry->retained += beforeCapacity;

  if (afterCapacity > budget - entry->retained) {
    return std::unexpected(TextHistoryError::WOULD_BLOCK);
  }

  entry->retained += afterCapacity;

  return entry;
}

void discardRedo(HistoryData &data, ContentStates *content) noexcept {
  while (data.entries.size() > data.cursor) {
    data.retained -= data.entries.back()->retained;
    data.entries.pop_back();
    if (content) {
      content->entries.pop_back();
    }
  }
}

void evictOldest(HistoryData &data, ContentStates *content) noexcept {
  data.retained -= data.entries.front()->retained;
  data.entries.erase(data.entries.begin());
  if (content) {
    content->entries.erase(content->entries.begin());
  }
  --data.cursor;
}
} // namespace

struct TextHistory::State {
  std::unique_ptr<HistoryData> data = std::make_unique<HistoryData>();
  std::shared_ptr<OwnerToken> owner = std::make_shared<OwnerToken>();
  std::uint64_t generation{};
};

struct PreparedTextHistory::State {
  State(std::unique_ptr<HistoryData> nextData, std::shared_ptr<const OwnerToken> ownerToken,
        std::uint64_t preparedGeneration, bool preparedGroupOpen, GraphemeText &&target,
        const TextHistorySnapshot &snapshot, bool changes)
      : next(std::move(nextData)), owner(std::move(ownerToken)), generation(preparedGeneration),
        groupOpen(preparedGroupOpen), text(std::move(target)), cursor(snapshot.cursor), anchor(snapshot.anchor),
        changed(changes) {
  }

  std::unique_ptr<HistoryData> next;
  std::shared_ptr<const OwnerToken> owner;
  std::uint64_t generation{};
  bool groupOpen{};
  GraphemeText text;
  std::size_t cursor{};
  std::size_t anchor{};
  bool changed{};
  bool textTaken{};
  std::uint64_t contentState{};
  std::unique_ptr<ContentStates> content;
};

PreparedTextHistory::PreparedTextHistory(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {
}

PreparedTextHistory::PreparedTextHistory(PreparedTextHistory &&other) noexcept = default;
PreparedTextHistory &PreparedTextHistory::operator=(PreparedTextHistory &&other) noexcept = default;
PreparedTextHistory::~PreparedTextHistory() = default;

bool PreparedTextHistory::changed() const noexcept {
  return state_ && state_->changed;
}

std::size_t PreparedTextHistory::cursor() const noexcept {
  return state_ ? state_->cursor : 0;
}

std::size_t PreparedTextHistory::anchor() const noexcept {
  return state_ ? state_->anchor : 0;
}

std::uint64_t PreparedTextHistory::contentState() const noexcept {
  return state_ ? state_->contentState : 0;
}

const GraphemeText *PreparedTextHistory::text() const noexcept {
  return state_ && !state_->textTaken ? &state_->text : nullptr;
}

void PreparedTextHistory::takeText(GraphemeText &destination) noexcept {
  if (state_ && !state_->textTaken) {
    destination = std::move(state_->text);
    state_->textTaken = true;
  }
}

TextHistory::TextHistory(TextHistoryOptions options, WidthPolicy policy, TextLimits limits)
    : state_(std::make_unique<State>()), options_(options), policy_(policy), limits_(limits) {
  if (options.trackContentStates) {
    state_->owner->content = std::make_unique<ContentStates>();
  }
}

TextHistory::~TextHistory() = default;

std::uint64_t TextHistory::contentState() const noexcept {
  return state_->owner->content ? state_->owner->content->current : 0;
}

std::expected<PreparedTextHistory, TextHistoryError> TextHistory::prepareEdit(const TextHistorySnapshot &before,
                                                                              const TextHistorySnapshot &after,
                                                                              TextHistoryKind kind) const {
  try {
    auto beforeText = validate(before, policy_, limits_);

    if (!beforeText) {
      return std::unexpected(beforeText.error());
    }

    auto afterText = validate(after, policy_, limits_);

    if (!afterText) {
      return std::unexpected(afterText.error());
    }

    const auto &current = *state_->data;
    const bool retaining = options_.enabled && options_.maxOperations != 0 && options_.maxBytes != 0;
    const bool changed = !retaining || before.bytes != after.bytes;

    if (changed &&
        (state_->generation == std::numeric_limits<std::uint64_t>::max() ||
         (options_.trackContentStates && state_->generation == std::numeric_limits<std::uint64_t>::max() - 1))) {
      return std::unexpected(TextHistoryError::EXHAUSTED);
    }

    std::unique_ptr<HistoryData> next;
    std::unique_ptr<ContentStates> content;

    if (changed) {
      next = std::make_unique<HistoryData>();
      if (state_->owner->content) {
        content = std::make_unique<ContentStates>();
        content->current = before.bytes == after.bytes ? state_->owner->content->current : state_->generation + 2;
      }
    }

    if (retaining && changed) {
      if (!current.entries.empty()) {
        const auto &expected =
          current.cursor == 0 ? current.entries.front()->before : current.entries[current.cursor - 1]->after;

        if (expected.bytes != before.bytes) {
          return std::unexpected(TextHistoryError::STALE);
        }
      }

      // Shared immutable handles make staging proportional to the bounded group
      // count; only the changed entry copies text.
      *next = current;
      if (content) {
        *content = *state_->owner->content;
        content->current = state_->generation + 2;
      }
      discardRedo(*next, content.get());

      const bool canGroup = groupable(kind, before, after, options_);
      const bool merge = canGroup && current.groupOpen && next->cursor != 0 && next->entries.back()->kind == kind &&
                         sameSnapshot(next->entries.back()->after, before);
      auto entry = merge ? makeEntry(next->entries.back()->before, after, kind, options_.maxBytes, content != nullptr)
                         : makeEntry(before, after, kind, options_.maxBytes, content != nullptr);
      bool merged = merge;
      auto beforeState = content ? (merge ? content->entries.back().first : state_->owner->content->current) : 0;

      if (!entry && merge && entry.error() == TextHistoryError::WOULD_BLOCK) {
        entry = makeEntry(before, after, kind, options_.maxBytes, content != nullptr);
        merged = false;
        beforeState = content ? state_->owner->content->current : 0;
      }

      if (!entry) {
        return std::unexpected(entry.error());
      }

      if (merged) {
        next->retained -= next->entries.back()->retained;
        next->entries.pop_back();
        if (content) {
          content->entries.pop_back();
        }
        --next->cursor;
      }

      // Remove whole old groups before adding the new cost to avoid overflow.
      while (!next->entries.empty() && (next->entries.size() >= options_.maxOperations ||
                                        next->retained > options_.maxBytes - (*entry)->retained)) {
        evictOldest(*next, content.get());
      }

      next->entries.push_back(std::move(*entry));
      if (content) {
        content->entries.emplace_back(beforeState, content->current);
      }
      next->retained += next->entries.back()->retained;
      ++next->cursor;
      next->groupOpen = canGroup;
    }

    auto prepared = std::make_unique<PreparedTextHistory::State>(
      std::move(next), state_->owner, state_->generation, current.groupOpen, std::move(*afterText), after, changed);

    prepared->contentState = content ? content->current : contentState();
    prepared->content = std::move(content);

    return PreparedTextHistory(std::move(prepared));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextHistoryError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextHistoryError::NO_MEMORY);
  }
}

std::expected<PreparedTextHistory, TextHistoryError> TextHistory::prepareTraversal(bool undo) const {
  const auto &current = *state_->data;

  if (undo ? !canUndo() : !canRedo()) {
    return std::unexpected(TextHistoryError::EMPTY);
  }

  if (state_->generation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TextHistoryError::EXHAUSTED);
  }

  try {
    const auto &snapshot = undo ? current.entries[current.cursor - 1]->before : current.entries[current.cursor]->after;
    auto text = validate(snapshot, policy_, limits_);

    if (!text) {
      return std::unexpected(text.error());
    }

    auto next = std::make_unique<HistoryData>(current);

    next->cursor = undo ? current.cursor - 1 : current.cursor + 1;
    next->groupOpen = false;
    std::unique_ptr<ContentStates> content;

    if (state_->owner->content) {
      content = std::make_unique<ContentStates>(*state_->owner->content);
      content->current = undo ? content->entries[current.cursor - 1].first : content->entries[current.cursor].second;
    }

    auto prepared = std::make_unique<PreparedTextHistory::State>(std::move(next), state_->owner, state_->generation,
                                                                 current.groupOpen, std::move(*text), snapshot, true);

    prepared->contentState = content ? content->current : 0;
    prepared->content = std::move(content);

    return PreparedTextHistory(std::move(prepared));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextHistoryError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextHistoryError::NO_MEMORY);
  }
}

std::expected<PreparedTextHistory, TextHistoryError> TextHistory::prepareUndo() const {
  return prepareTraversal(true);
}

std::expected<PreparedTextHistory, TextHistoryError> TextHistory::prepareRedo() const {
  return prepareTraversal(false);
}

std::expected<PreparedTextHistory, TextHistoryError>
TextHistory::prepareResetBaseline(const TextHistorySnapshot &baseline) const {
  if (state_->generation == std::numeric_limits<std::uint64_t>::max() ||
      (options_.trackContentStates && state_->generation == std::numeric_limits<std::uint64_t>::max() - 1)) {
    return std::unexpected(TextHistoryError::EXHAUSTED);
  }

  try {
    auto text = validate(baseline, policy_, limits_);

    if (!text) {
      return std::unexpected(text.error());
    }

    auto next = std::make_unique<HistoryData>();

    std::unique_ptr<ContentStates> content;

    if (state_->owner->content) {
      content = std::make_unique<ContentStates>();
      content->current = state_->generation + 2;
    }

    auto prepared = std::make_unique<PreparedTextHistory::State>(
      std::move(next), state_->owner, state_->generation, state_->data->groupOpen, std::move(*text), baseline, true);

    prepared->contentState = content ? content->current : 0;
    prepared->content = std::move(content);

    return PreparedTextHistory(std::move(prepared));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextHistoryError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextHistoryError::NO_MEMORY);
  }
}

std::expected<void, TextHistoryError> TextHistory::checkPrepared(const PreparedTextHistory &prepared) const noexcept {
  if (!prepared.state_ || prepared.state_->owner != state_->owner ||
      prepared.state_->generation != state_->generation || prepared.state_->groupOpen != state_->data->groupOpen) {
    return std::unexpected(TextHistoryError::STALE);
  }

  return {};
}

std::expected<void, TextHistoryError> TextHistory::commit(PreparedTextHistory &&prepared) noexcept {
  const auto checked = checkPrepared(prepared);

  if (!checked) {
    return checked;
  }

  if (prepared.state_->changed) {
    state_->data.swap(prepared.state_->next);
    state_->owner->content.swap(prepared.state_->content);
    ++state_->generation;
  }

  prepared.state_.reset();

  return {};
}

void TextHistory::stopGrouping() noexcept {
  state_->data->groupOpen = false;

  if (state_->generation != std::numeric_limits<std::uint64_t>::max()) {
    ++state_->generation;
  }
}

bool TextHistory::canUndo() const noexcept {
  return state_->data->cursor != 0;
}

bool TextHistory::canRedo() const noexcept {
  return state_->data->cursor < state_->data->entries.size();
}

std::size_t TextHistory::operationCount() const noexcept {
  return state_->data->entries.size();
}

std::size_t TextHistory::retainedBytes() const noexcept {
  return state_->data->retained;
}

} // namespace portals

namespace portals {
std::size_t PreparedTextHistory::retainedBytes() const noexcept {
  return state_ && state_->next ? state_->next->retained : 0;
}
} // namespace portals
