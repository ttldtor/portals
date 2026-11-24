// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/text_document.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace portals {
namespace {
bool valid(SourceStamp stamp) noexcept {
  return stamp.instance != 0 && stamp.epoch != 0;
}

bool valid(PositionAffinity affinity) noexcept {
  return affinity == PositionAffinity::FORWARD || affinity == PositionAffinity::BACKWARD;
}

TextDocumentError documentError(TextHistoryError error) noexcept {
  switch (error) {
  case TextHistoryError::NO_MEMORY:
    return TextDocumentError::NO_MEMORY;
  case TextHistoryError::WOULD_BLOCK:
    return TextDocumentError::WOULD_BLOCK;
  case TextHistoryError::INVALID_TEXT:
    return TextDocumentError::INVALID_TEXT;
  case TextHistoryError::NOT_BOUNDARY:
    return TextDocumentError::NOT_BOUNDARY;
  case TextHistoryError::EMPTY:
    return TextDocumentError::EMPTY;
  case TextHistoryError::STALE:
    return TextDocumentError::STALE;
  case TextHistoryError::EXHAUSTED:
    return TextDocumentError::EXHAUSTED;
  }

  return TextDocumentError::INVALID_ARGUMENT;
}

TextDocumentError documentError(SourceError error) noexcept {
  if (error == SourceError::NO_MEMORY) {
    return TextDocumentError::NO_MEMORY;
  }

  if (error == SourceError::IDENTITY_EXHAUSTED) {
    return TextDocumentError::EXHAUSTED;
  }

  return TextDocumentError::INVALID_ARGUMENT;
}

struct ChangeNode {
  std::uint64_t state{};
  std::uint64_t parent{};
  TextSplice change;
};

struct RevisionState {
  std::uint64_t revision{};
  std::uint64_t state{};
  std::uint64_t extent{};
};

void mapOffset(ModelPosition &position, TextSplice change, bool &nearest) noexcept {
  if (position.offset < change.offset) {
    return;
  }

  const auto end = change.offset + change.erased;

  if (position.offset > end) {
    position.offset = position.offset - change.erased + change.inserted;

    return;
  }

  if (change.erased != 0 && position.offset == end) {
    position.offset = change.offset + change.inserted;

    return;
  }

  nearest = nearest || (position.offset > change.offset && position.offset < end);
  position.offset = change.offset + (position.affinity == PositionAffinity::FORWARD ? change.inserted : 0);
}

template <class T> void appendBounded(std::vector<T> &values, T value, std::size_t capacity) noexcept {
  if (values.size() == capacity) {
    values.erase(values.begin());
  }

  values.push_back(value);
}

TextHistoryOptions anchorHistory(TextHistoryOptions options) noexcept {
  options.trackContentStates = true;

  return options;
}

std::uint64_t ordinal(const GraphemeText &text, std::size_t byteOffset) noexcept {
  const auto clusters = text.graphemes();
  const auto found =
    std::lower_bound(clusters.begin(), clusters.end(), byteOffset, [](const Grapheme &cluster, std::size_t offset) {
      return cluster.byte_begin < offset;
    });

  return static_cast<std::uint64_t>(found - clusters.begin());
}

} // namespace

struct TextChangeMap::State {
  ModelResource resource;
  TextAnchorOptions options;
  std::vector<ChangeNode> nodes = std::vector<ChangeNode>(0);
  std::vector<RevisionState> revisions = std::vector<RevisionState>(0);
  std::vector<TextSplice> forward = std::vector<TextSplice>(0);
  std::uint64_t largestState{1};

  const ChangeNode *node(std::uint64_t token) const noexcept {
    const auto found = std::ranges::find(nodes, token, &ChangeNode::state);

    return found == nodes.end() ? nullptr : &*found;
  }

  void publish(std::uint64_t token, std::uint64_t extent) noexcept {
    appendBounded(revisions, RevisionState{revisions.back().revision + 1, token, extent}, options.maxChanges + 1);
  }
};

TextChangeMap::TextChangeMap(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {
}

TextChangeMap::~TextChangeMap() = default;

std::expected<std::unique_ptr<TextChangeMap>, SourceError>
TextChangeMap::create(ModelResource resource, std::uint64_t extent, TextAnchorOptions options,
                      std::uint64_t initialRevision, std::uint64_t initialContentState) {
  if (!valid(resource.identity) || resource.lineage == 0 || options.maxChanges == 0 ||
      options.maxChanges == std::numeric_limits<std::size_t>::max() || initialRevision == 0 ||
      initialContentState == 0) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  try {
    auto state = std::make_unique<State>();

    state->resource = resource;
    state->options = options;
    state->nodes.reserve(options.maxChanges);
    state->revisions.reserve(options.maxChanges + 1);
    state->forward.reserve(options.maxChanges);
    state->revisions.push_back({initialRevision, initialContentState, extent});
    state->largestState = initialContentState;

    return std::unique_ptr<TextChangeMap>(new TextChangeMap(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

std::expected<void, SourceError> TextChangeMap::edit(TextSplice change, std::uint64_t contentState) noexcept {
  const auto current = state_->revisions.back();

  if (change.offset > current.extent || change.erased > current.extent - change.offset ||
      change.inserted > std::numeric_limits<std::uint64_t>::max() - (current.extent - change.erased) ||
      contentState <= state_->largestState) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  if (current.revision == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
  }

  appendBounded(state_->nodes, ChangeNode{contentState, current.state, change}, state_->options.maxChanges);
  state_->largestState = contentState;
  state_->publish(contentState, current.extent - change.erased + change.inserted);

  return {};
}

std::expected<void, SourceError> TextChangeMap::traverse(std::uint64_t contentState, std::uint64_t extent) noexcept {
  if (contentState == 0 || contentState > state_->largestState) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  if (revision() == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
  }

  for (const auto &known : state_->revisions) {
    if (known.state == contentState && known.extent != extent) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }
  }

  state_->publish(contentState, extent);

  return {};
}

std::expected<ModelBookmark, SourceError> TextChangeMap::bookmark(std::uint64_t offset,
                                                                  PositionAffinity affinity) const {
  if (offset > extent() || !valid(affinity)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  return ModelBookmark::create(state_->resource, {PositionKind::TEXT, {}, offset, revision(), affinity});
}

std::expected<TextMappedPosition, SourceError> TextChangeMap::map(const ModelBookmark &bookmark,
                                                                  std::stop_token stop) const {
  if (stop.stop_requested()) {
    return std::unexpected(SourceError::CANCELLED);
  }

  TextMappedPosition reply;

  if (bookmark.resource() != state_->resource) {
    reply.state = PositionResolution::INCOMPATIBLE;

    return reply;
  }

  auto position = bookmark.position();

  if (position.kind != PositionKind::TEXT) {
    reply.state = PositionResolution::UNSUPPORTED;

    return reply;
  }

  const auto origin = std::ranges::find(state_->revisions, position.revision, &RevisionState::revision);

  if (origin == state_->revisions.end()) {
    reply.state = PositionResolution::UNAVAILABLE;

    return reply;
  }

  if (position.offset > origin->extent || !valid(position.affinity)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  auto from = origin->state;
  auto to = state_->revisions.back().state;
  bool nearest{};

  state_->forward.clear();

  // State tokens strictly increase along every edit edge. Walking the larger
  // token finds the common ancestor without scanning document bytes or keeping
  // an unbounded per-bookmark registration. Undo to that ancestor cancels deletion.
  while (from != to) {
    const auto *edge = state_->node(std::max(from, to));

    if (!edge) {
      reply.state = PositionResolution::UNAVAILABLE;

      return reply;
    }

    if (from > to) {
      mapOffset(position, {edge->change.offset, edge->change.inserted, edge->change.erased}, nearest);
      from = edge->parent;
    } else {
      state_->forward.push_back(edge->change);
      to = edge->parent;
    }
  }

  for (auto change = state_->forward.rbegin(); change != state_->forward.rend(); ++change) {
    mapOffset(position, *change, nearest);
  }

  position.revision = revision();
  reply.state = nearest ? PositionResolution::NEAREST : PositionResolution::EXACT;
  reply.position = position;

  return reply;
}

std::uint64_t TextChangeMap::revision() const noexcept {
  return state_->revisions.back().revision;
}

std::uint64_t TextChangeMap::extent() const noexcept {
  return state_->revisions.back().extent;
}

struct TextSaveTicket::State {
  ModelResource resource;
  std::uint64_t sequence{};
  std::uint64_t revision{};
  std::uint64_t contentState{};
  std::string target = std::string(0, '\0');
};

std::string_view TextSaveTicket::target() const noexcept {
  return state_ ? std::string_view(state_->target) : std::string_view{};
}

std::uint64_t TextSaveTicket::revision() const noexcept {
  return state_ ? state_->revision : 0;
}

struct FiniteTextDocument::State {
  State(GraphemeText &&initial, FiniteTextDocumentOptions settings, ModelResource identity,
        std::unique_ptr<TextChangeMap> mapping)
      : options(settings), resource(identity), text(std::move(initial)),
        history(anchorHistory(settings.history), settings.widthPolicy, settings.limits), changes(std::move(mapping)) {
  }

  FiniteTextDocumentOptions options;
  ModelResource resource;
  GraphemeText text;
  TextHistory history;
  std::unique_ptr<TextChangeMap> changes;
  TextSelection selection;
  std::shared_ptr<const TextSaveTicket::State> savepoint;
  std::uint64_t nextSave{};
  bool conflict{};
};

FiniteTextDocument::FiniteTextDocument(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {
}

FiniteTextDocument::~FiniteTextDocument() = default;

std::expected<std::unique_ptr<FiniteTextDocument>, TextDocumentError>
FiniteTextDocument::create(std::string_view text, FiniteTextDocumentOptions options) {
  try {
    auto segmented = GraphemeText::from(text, options.widthPolicy, options.limits);

    if (!segmented) {
      return std::unexpected(TextDocumentError::INVALID_TEXT);
    }

    auto resource = ModelResource::create();

    if (!resource) {
      return std::unexpected(documentError(resource.error()));
    }

    auto changes = TextChangeMap::create(*resource, segmented->bytes().size(), options.anchors);

    if (!changes) {
      return std::unexpected(documentError(changes.error()));
    }

    auto state = std::make_unique<State>(std::move(*segmented), options, *resource, std::move(*changes));

    return std::unique_ptr<FiniteTextDocument>(new FiniteTextDocument(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  }
}

const GraphemeText &FiniteTextDocument::text() const noexcept {
  return state_->text;
}

ModelResource FiniteTextDocument::resource() const noexcept {
  return state_->resource;
}

SourceStamp FiniteTextDocument::session() const noexcept {
  return state_->resource.identity;
}

std::uint64_t FiniteTextDocument::revision() const noexcept {
  return state_->changes->revision();
}

TextSelection FiniteTextDocument::selection() const noexcept {
  return state_->selection;
}

std::expected<void, TextDocumentError> FiniteTextDocument::setSelection(TextSelection selection) noexcept {
  if (!state_->text.nextCursor(selection.cursor) || !state_->text.nextCursor(selection.anchor)) {
    return std::unexpected(TextDocumentError::NOT_BOUNDARY);
  }

  if (state_->selection != selection) {
    state_->history.stopGrouping();
    state_->selection = selection;
  }

  return {};
}

std::expected<void, TextDocumentError> FiniteTextDocument::replace(std::size_t begin, std::size_t end,
                                                                   std::string_view replacement, TextHistoryKind kind) {
  if (state_->conflict) {
    return std::unexpected(TextDocumentError::CONFLICT);
  }

  if (begin > end) {
    return std::unexpected(TextDocumentError::INVALID_ARGUMENT);
  }

  if (!state_->text.nextCursor(begin) || !state_->text.nextCursor(end)) {
    return std::unexpected(TextDocumentError::NOT_BOUNDARY);
  }

  try {
    TextHistorySnapshot before;
    TextHistorySnapshot after;

    before.bytes.assign(state_->text.bytes());
    before.cursor = state_->selection.cursor;
    before.anchor = state_->selection.anchor;
    after.bytes.assign(before.bytes, 0, begin);
    after.bytes.append(replacement);
    after.bytes.append(before.bytes, end, before.bytes.size() - end);

    auto segmented = GraphemeText::from(after.bytes, state_->options.widthPolicy, state_->options.limits);

    if (!segmented) {
      return std::unexpected(TextDocumentError::INVALID_TEXT);
    }

    // Inserting a combining mark or ZWJ can merge either neighboring cluster.
    // Collapse to the first actual boundary at/after the inserted byte range.
    const auto desired = begin + replacement.size();
    auto cursor = desired;

    if (!segmented->nextCursor(cursor)) {
      const auto clusters = segmented->graphemes();
      const auto cluster = std::ranges::find_if(clusters, [desired](const Grapheme &entry) {
        return entry.byte_end >= desired;
      });

      cursor = cluster->byte_end;
    }

    after.cursor = cursor;
    after.anchor = cursor;

    auto prepared = state_->history.prepareEdit(before, after, kind);

    if (!prepared) {
      return std::unexpected(documentError(prepared.error()));
    }

    const auto checked = state_->history.checkPrepared(*prepared);

    if (!checked) {
      return std::unexpected(documentError(checked.error()));
    }

    if (before.bytes != after.bytes) {
      const auto mapped = state_->changes->edit({begin, end - begin, replacement.size()}, prepared->contentState());

      if (!mapped) {
        return std::unexpected(documentError(mapped.error()));
      }
    }

    prepared->takeText(state_->text);
    state_->selection = {cursor, cursor};
    (void)state_->history.commit(std::move(*prepared));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  }
}

std::expected<void, TextDocumentError> FiniteTextDocument::traverse(bool undoDirection) {
  if (state_->conflict) {
    return std::unexpected(TextDocumentError::CONFLICT);
  }

  auto prepared = undoDirection ? state_->history.prepareUndo() : state_->history.prepareRedo();

  if (!prepared) {
    return std::unexpected(documentError(prepared.error()));
  }

  const auto checked = state_->history.checkPrepared(*prepared);

  if (!checked) {
    return std::unexpected(documentError(checked.error()));
  }

  const auto mapped = state_->changes->traverse(prepared->contentState(), prepared->text()->bytes().size());

  if (!mapped) {
    return std::unexpected(documentError(mapped.error()));
  }

  state_->selection = {prepared->cursor(), prepared->anchor()};
  prepared->takeText(state_->text);
  (void)state_->history.commit(std::move(*prepared));

  return {};
}

std::expected<void, TextDocumentError> FiniteTextDocument::undo() {
  return traverse(true);
}

std::expected<void, TextDocumentError> FiniteTextDocument::redo() {
  return traverse(false);
}

bool FiniteTextDocument::canUndo() const noexcept {
  return state_->history.canUndo();
}

bool FiniteTextDocument::canRedo() const noexcept {
  return state_->history.canRedo();
}

std::expected<ModelBookmark, SourceError> FiniteTextDocument::bookmark(std::size_t byteOffset,
                                                                       PositionAffinity affinity) const {
  if (!state_->text.nextCursor(byteOffset)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  return state_->changes->bookmark(byteOffset, affinity);
}

std::expected<PositionReply, SourceError> FiniteTextDocument::resolve(const PositionRequest &request,
                                                                      std::stop_token stop) const {
  if (request.session != session() || request.revision != revision() || request.sequence == 0) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  const auto mapped = state_->changes->map(request.bookmark, stop);

  if (!mapped) {
    return std::unexpected(mapped.error());
  }

  PositionReply reply{
    request.bookmark.resource(), request.session, request.revision, request.sequence, mapped->state, mapped->position};

  if ((reply.state == PositionResolution::EXACT || reply.state == PositionResolution::NEAREST) &&
      !state_->text.nextCursor(static_cast<std::size_t>(reply.position.offset))) {
    const auto clusters = state_->text.graphemes();
    const auto following =
      static_cast<std::size_t>(ordinal(state_->text, static_cast<std::size_t>(reply.position.offset)));
    const auto &cluster = clusters[following - 1];

    reply.position.offset =
      reply.position.affinity == PositionAffinity::FORWARD ? cluster.byte_end : cluster.byte_begin;
    reply.state = PositionResolution::NEAREST;
  }

  return reply;
}

std::expected<TextSaveTicket, TextDocumentError> FiniteTextDocument::beginSave(std::string_view target) {
  if (state_->conflict) {
    return std::unexpected(TextDocumentError::CONFLICT);
  }

  if (target.empty()) {
    return std::unexpected(TextDocumentError::INVALID_ARGUMENT);
  }

  if (state_->nextSave == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TextDocumentError::EXHAUSTED);
  }

  try {
    auto data = std::make_shared<TextSaveTicket::State>();

    data->resource = resource();
    data->sequence = state_->nextSave + 1;
    data->revision = revision();
    data->contentState = state_->history.contentState();
    data->target.assign(target);

    TextSaveTicket ticket;

    ticket.state_ = std::move(data);
    ++state_->nextSave;

    return ticket;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  }
}

std::expected<void, TextDocumentError> FiniteTextDocument::acknowledgeSave(const TextSaveTicket &ticket) noexcept {
  if (state_->conflict) {
    return std::unexpected(TextDocumentError::CONFLICT);
  }

  if (!ticket.state_ || ticket.state_->resource != resource() ||
      (state_->savepoint && ticket.state_->sequence <= state_->savepoint->sequence)) {
    return std::unexpected(TextDocumentError::STALE);
  }

  state_->savepoint = ticket.state_;

  return {};
}

bool FiniteTextDocument::dirty() const noexcept {
  const auto saved = state_->savepoint ? state_->savepoint->contentState : 1;

  return state_->conflict || state_->history.contentState() != saved;
}

std::string_view FiniteTextDocument::saveTarget() const noexcept {
  return state_->savepoint ? std::string_view(state_->savepoint->target) : std::string_view{};
}

void FiniteTextDocument::observeExternalReplacement() noexcept {
  state_->conflict = true;
}

bool FiniteTextDocument::conflicted() const noexcept {
  return state_->conflict;
}

std::expected<void, TextDocumentError> FiniteTextDocument::acceptExternalReplacement(std::string_view text) {
  if (resource().lineage == std::numeric_limits<std::uint64_t>::max() ||
      revision() == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TextDocumentError::EXHAUSTED);
  }

  try {
    TextHistorySnapshot baseline;

    baseline.bytes.assign(text);

    auto prepared = state_->history.prepareResetBaseline(baseline);

    if (!prepared) {
      return std::unexpected(documentError(prepared.error()));
    }

    auto nextResource = resource();

    ++nextResource.lineage;

    auto changes = TextChangeMap::create(nextResource, prepared->text()->bytes().size(), state_->options.anchors,
                                         revision() + 1, prepared->contentState());

    if (!changes) {
      return std::unexpected(documentError(changes.error()));
    }

    auto saved = std::make_shared<TextSaveTicket::State>();

    saved->resource = nextResource;
    saved->sequence = state_->nextSave;
    saved->contentState = prepared->contentState();
    saved->revision = (*changes)->revision();

    if (state_->savepoint) {
      saved->target = state_->savepoint->target;
    }

    const auto checked = state_->history.checkPrepared(*prepared);

    if (!checked) {
      return std::unexpected(documentError(checked.error()));
    }

    prepared->takeText(state_->text);
    (void)state_->history.commit(std::move(*prepared));
    state_->resource = nextResource;
    state_->changes = std::move(*changes);
    state_->selection = {};
    state_->savepoint = std::move(saved);
    state_->conflict = false;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TextDocumentError::NO_MEMORY);
  }
}
} // namespace portals
