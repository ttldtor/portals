// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/line_edit_text.hpp>
#include <portals/line_edit.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace portals {
namespace {
LineEditResult failed(TreeError error) {
  return {LineEditStatus::FAILED, {}, error};
}

LineEditStatus textStatus(TextError error) {
  return error.kind == TextErrorKind::INVALID_UTF8 || error.kind == TextErrorKind::INVALID_POLICY
           ? LineEditStatus::INVALID_TEXT
           : LineEditStatus::LIMIT_EXCEEDED;
}

char32_t shortcutCharacter(const KeyEvent *key) {
  if (!key || key->key != InputKey::CHARACTER || key->action == KeyAction::RELEASE ||
      key->modifiers != InputModifiers{.control = true}) {
    return U'\0';
  }

  auto scalar = key->scalar;

  if (scalar >= U'A' && scalar <= U'Z') {
    scalar += U'a' - U'A';
  }

  return scalar == U'a' || scalar == U'c' || scalar == U'x' || scalar == U'v' ? scalar : U'\0';
}

TreeError historyError(TextHistoryError error) {
  switch (error) {
  case TextHistoryError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case TextHistoryError::INVALID_TEXT:
  case TextHistoryError::NOT_BOUNDARY:
    return TreeError::INVALID_ARGUMENT;
  case TextHistoryError::EXHAUSTED:
    return TreeError::NODE_LIMIT;
  default:
    return TreeError::WOULD_BLOCK;
  }
}

char32_t normalizedScalar(char32_t scalar) {
  return scalar >= U'A' && scalar <= U'Z' ? scalar + U'a' - U'A' : scalar;
}

bool matches(const EditorChord &chord, const KeyEvent &key) {
  return key.action != KeyAction::RELEASE && chord.key == key.key && chord.modifiers == key.modifiers &&
         normalizedScalar(chord.scalar) == normalizedScalar(key.scalar);
}

bool validChord(const EditorChord &chord) {
  switch (chord.key) {
  case InputKey::CHARACTER:
    return chord.scalar > 0 && chord.scalar <= 0x10ffff && !(chord.scalar >= 0xd800 && chord.scalar <= 0xdfff);
  case InputKey::INSERT:
  case InputKey::PAGE_UP:
  case InputKey::PAGE_DOWN:
  case InputKey::F1:
  case InputKey::F2:
  case InputKey::F3:
  case InputKey::F4:
  case InputKey::F5:
  case InputKey::F6:
  case InputKey::F7:
  case InputKey::F8:
  case InputKey::F9:
  case InputKey::F10:
  case InputKey::F11:
  case InputKey::F12:
    return chord.scalar == 0;
  default:
    return false;
  }
}

bool validShortcuts(const LineEditHistoryShortcuts &shortcuts) {
  if (shortcuts.undo && !validChord(*shortcuts.undo)) {
    return false;
  }

  for (const auto &redo : shortcuts.redo) {
    if (redo && (!validChord(*redo) ||
                 (shortcuts.undo && matches(*shortcuts.undo, KeyEvent{redo->key, redo->scalar, redo->modifiers})))) {
      return false;
    }
  }

  return true;
}

bool acceptsAction(const ActionEvent *command) {
  if (!command || command->modifiers.alt || command->modifiers.control || command->modifiers.super) {
    return false;
  }

  switch (command->action) {
  case Action::MOVE_LEFT:
  case Action::MOVE_RIGHT:
  case Action::MOVE_HOME:
  case Action::MOVE_END:
    return true;
  case Action::ACTIVATE:
  case Action::CANCEL:
  case Action::ERASE_BACKWARD:
  case Action::ERASE_FORWARD:
    return !command->modifiers.shift;
  default:
    return false;
  }
}

struct EncodedScalar {
  std::array<char, 4> bytes{};
  std::size_t size{};

  std::string_view view() const noexcept {
    return {bytes.data(), size};
  }
};

EncodedScalar encode(char32_t scalar) noexcept {
  EncodedScalar result;
  const auto append = [&](char byte) {
    result.bytes[result.size++] = byte;
  };

  if (scalar <= 0x7f) {
    append(static_cast<char>(scalar));
  } else if (scalar <= 0x7ff) {
    append(static_cast<char>(0xc0 | (scalar >> 6)));
    append(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else if (scalar <= 0xffff) {
    append(static_cast<char>(0xe0 | (scalar >> 12)));
    append(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    append(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else if (scalar <= 0x10ffff) {
    append(static_cast<char>(0xf0 | (scalar >> 18)));
    append(static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)));
    append(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    append(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else {
    append('\xff');
  }

  return result;
}
} // namespace

LineEdit::LineEdit(LineEditOptions options, SizePolicy policy)
    : SizedWidget(policy), options_(options), text_(GraphemeText::from("", options.widthPolicy).value()) {
  if (!detail::validLineEditDisplayMode(options.displayMode) ||
      (options.placeholder && detail::hasLineSeparator(*options.placeholder))) {
    throw std::invalid_argument("Invalid LineEdit presentation options");
  }

  placeholder_ = std::move(detail::prepareLineEditPlaceholder(options_).value().text);
}

LineEdit::LineEdit(LineEditOptions options, SizePolicy policy, detail::LineEditPlaceholder placeholder)
    : SizedWidget(policy), options_(std::move(options)), text_(GraphemeText::from("", options_.widthPolicy).value()),
      placeholder_(std::move(placeholder.text)) {
}

std::expected<CellSize, TreeError> LineEdit::measure(CellSize) {
  const auto width = options_.displayMode == LineEditDisplayMode::NO_ECHO ? 0
                     : text_.bytes().empty() && placeholder_              ? placeholder_->measure()
                     : masked_                                            ? masked_->measure()
                                                                          : text_.measure();
  if (width == std::numeric_limits<std::int32_t>::max()) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  return CellSize{width + 1, 1};
}

std::expected<void, TreeError> LineEdit::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;
  revealCursor();

  return {};
}

std::int32_t LineEdit::cellAt(std::size_t byte) const {
  if (options_.displayMode == LineEditDisplayMode::NO_ECHO) {
    return 0;
  }
  std::int32_t cells{};

  for (const auto &cluster : text_.graphemes()) {
    if (cluster.byte_begin >= byte) {
      break;
    }

    cells += options_.displayMode == LineEditDisplayMode::PASSWORD ? 1 : cluster.cells;
  }

  return cells;
}

void LineEdit::revealCursor() {
  if (options_.displayMode == LineEditDisplayMode::NO_ECHO) {
    scroll_ = 0;
    return;
  }
  const auto width = bounds_.size().width;
  const auto caret = cellAt(cursor_);

  const auto displayWidth = masked_ ? masked_->measure() : text_.measure();
  if (static_cast<std::int64_t>(displayWidth) + 1 <= width) {
    scroll_ = 0;

    return;
  }
  auto caretWidth = 1;
  const auto atCursor = std::ranges::find(text_.graphemes(), cursor_, &Grapheme::byte_begin);

  if (options_.displayMode == LineEditDisplayMode::NORMAL && atCursor != text_.graphemes().end() &&
      atCursor->cells <= width) {
    caretWidth = atCursor->cells;
  }

  // Scroll origins are always complete grapheme boundaries. A viewport narrower than
  // the current glyph shows a styled blank caret, never half of a wide glyph.
  if (caret < scroll_) {
    scroll_ = caret;
  }

  std::int32_t origin{};

  for (const auto &cluster : text_.graphemes()) {
    if (width <= 0 || static_cast<std::int64_t>(caret) + caretWidth - scroll_ <= width) {
      break;
    }

    origin += options_.displayMode == LineEditDisplayMode::PASSWORD ? 1 : cluster.cells;

    if (origin > scroll_) {
      scroll_ = origin;
    }
  }
}

std::expected<void, TreeError> LineEdit::paint(PaintContext &context) {
  const auto resolved = computedStyle() ? *computedStyle() : LineEdit::styleDefaults();
  const auto style = !enabled_ ? resolved.disabled : focused_ ? resolved.focused : resolved.normal;
  const auto selectionBegin = cellAt(std::min(cursor_, anchor_)) - scroll_;
  const auto selectionEnd = cellAt(std::max(cursor_, anchor_)) - scroll_;
  const auto selection = CellRect::from({selectionBegin, 0}, {selectionEnd - selectionBegin, 1}).value();
  const auto *shown = text_.bytes().empty()                                  ? placeholder_.get()
                      : options_.displayMode == LineEditDisplayMode::NO_ECHO ? nullptr
                      : masked_                                              ? masked_.get()
                                                                             : &text_;
  const std::array<RenderCommand, 1> background{FillCommand{bounds_, bounds_, style}};
  auto submitted = context.submit(background);

  if (submitted && shown) {
    const std::array<RenderCommand, 2> value{
      TextCommand{{-scroll_, 0}, *shown, bounds_, style},
      TextCommand{{-scroll_, 0}, *shown, selection.intersection(bounds_), enabled_ ? resolved.selected : style}};
    submitted = context.submit(value);
  }

  if (!submitted || !focused_ || !enabled_ || bounds_.empty()) {
    return submitted;
  }

  const auto caret = cellAt(cursor_) - scroll_;
  const auto cluster = std::ranges::find(text_.graphemes(), cursor_, &Grapheme::byte_begin);
  const auto width = options_.displayMode == LineEditDisplayMode::NORMAL && cluster != text_.graphemes().end() &&
                         cluster->cells <= bounds_.size().width
                       ? cluster->cells
                       : 1;
  const auto area = CellRect::from({caret, 0}, {width, 1}).value();
  const std::array<RenderCommand, 1> cursor{FillCommand{area, bounds_, resolved.caret}};
  submitted = context.submit(cursor);
  if (submitted && shown) {
    const std::array<RenderCommand, 1> glyph{
      TextCommand{{-scroll_, 0}, *shown, area.intersection(bounds_), resolved.caret}};
    submitted = context.submit(glyph);
  }
  return submitted;
}

ComputedStyle LineEdit::styleDefaults() const {
  ComputedStyle result;

  result.normal = options_.style.normal;
  result.focused = options_.style.focused;
  result.disabled = options_.style.disabled;
  result.selected = options_.selection;
  result.caret = options_.cursor;

  return result;
}

const GraphemeText &LineEdit::text() const {
  return text_;
}

std::size_t LineEdit::cursor() const {
  return cursor_;
}

std::size_t LineEdit::anchor() const {
  return anchor_;
}

bool LineEdit::dirty() const {
  return dirty_;
}

struct LineEditBinding::State {
  WidgetTree &tree;
  Node<LineEdit> node;
  std::shared_ptr<Property<std::string>> model;
  std::shared_ptr<Clipboard> clipboard;
  std::function<bool(std::string_view)> validate;
  std::function<void(LineEditResult)> onResult;
  std::unique_ptr<TextHistory> history;
  std::shared_ptr<detail::EditorCommandFeedback> feedback{};
  std::uint64_t revision{};
  bool initialized{};
  bool busy{};
  LineEditEvents events{};
  bool userTextChanged{};
  bool delivering{};
  bool completionDelivery{};

  static EditingFinishedOutcome finishOutcome(const LineEditResult &result) {
    switch (result.status) {
    case LineEditStatus::COMMITTED:
      return result.commit && result.commit->changed ? EditingFinishedOutcome::APPLIED
                                                     : EditingFinishedOutcome::UNCHANGED;
    case LineEditStatus::VALIDATION_FAILED:
      return EditingFinishedOutcome::VALIDATION_REJECTED;
    case LineEditStatus::CONFLICT:
      return EditingFinishedOutcome::CONFLICT;
    default:
      return EditingFinishedOutcome::FAILED;
    }
  }

  void deliver(const LineEditResult &result, bool finish = false) {
    BusyGuard delivery(delivering);
    BusyGuard completion(completionDelivery, finish);

    // Preserve the legacy callback boundary and never borrow the widget across extension code.
    onResult(result);

    if (userTextChanged && events.textEdited && alive()) {
      events.textEdited(TextEditedEvent{result});
    }

    if (finish && events.editingFinished && alive()) {
      events.editingFinished({EditingFinishedReason::ENTER, finishOutcome(result), result});
    }
  }

  struct BusyGuard {
    bool &active;
    bool previous;

    explicit BusyGuard(bool &value, bool next = true) : active(value), previous(value) {
      active = next;
    }

    ~BusyGuard() {
      active = previous;
    }
  };

  static LineEditOptions options(const LineEdit &edit) {
    return edit.options_;
  }

  static bool validMode(const LineEdit &edit) {
    return detail::validLineEditDisplayMode(edit.options_.displayMode) &&
           (edit.options_.editMode == EditMode::IMMEDIATE || edit.options_.editMode == EditMode::ON_COMPLETION ||
            edit.options_.editMode == EditMode::EXPLICIT);
  }

  std::expected<void, TreeError> alive() {
    return tree.withScope(node, [](const UiScope &) {
    });
  }

  TextHistorySnapshot snapshot(const LineEdit &edit) const {
    return {std::string(edit.text_.bytes()), edit.cursor_, edit.anchor_};
  }

  std::expected<void, TreeError> accept(LineEdit &edit, PreparedTextHistory &prepared, bool changed) {
    // Build the complete mask before invalidation or local/history publication. Pointer transfer below cannot allocate.
    std::unique_ptr<GraphemeText> mask;
    if (edit.options_.displayMode == LineEditDisplayMode::PASSWORD) {
      const auto *target = prepared.text();
      if (!target) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto masked = GraphemeText::from(std::string(target->graphemes().size(), '*'), edit.options_.widthPolicy,
                                       edit.options_.limits);
      if (!masked) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      mask = std::make_unique<GraphemeText>(std::move(*masked));
    }
    auto checked = history->checkPrepared(prepared);

    if (!checked) {
      return std::unexpected(historyError(checked.error()));
    }

    const bool moved = edit.cursor_ != prepared.cursor() || edit.anchor_ != prepared.anchor();

    if (changed || moved) {
      auto invalidated = tree.invalidate(node, changed ? Invalidation::LAYOUT : Invalidation::PAINT);

      if (!invalidated) {
        return invalidated;
      }
    }

    // No allocation, extension callback or history mutation occurs between this check and commit.

    const auto cursor = prepared.cursor();
    const auto anchor = prepared.anchor();

    prepared.takeText(edit.text_);
    edit.masked_.swap(mask);

    auto committed = history->commit(std::move(prepared));

    if (!committed) {
      return std::unexpected(historyError(committed.error()));
    }

    edit.cursor_ = cursor;
    edit.anchor_ = anchor;
    edit.scroll_ = 0;
    edit.revealCursor();

    return {};
  }

  std::expected<void, TreeError> adopt(LineEdit &edit, const PropertyChange<std::string> &snapshot) {
    if (detail::hasLineSeparator(snapshot.value)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto prepared =
      history->prepareResetBaseline({std::string(snapshot.value), snapshot.value.size(), snapshot.value.size()});

    if (!prepared) {
      return std::unexpected(historyError(prepared.error()));
    }

    auto applied = accept(edit, *prepared, edit.text_.bytes() != snapshot.value);

    if (!applied) {
      return applied;
    }

    edit.dirty_ = false;
    revision = snapshot.revision;
    initialized = true;

    return {};
  }

  std::expected<void, TreeError> syncModel(LineEdit &edit) {
    auto snapshot = model->snapshot();

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    if (!edit.dirty_ && (!initialized || revision != (*snapshot)->revision)) {
      return adopt(edit, **snapshot);
    }

    return {};
  }

  std::expected<void, TreeError> sync(std::optional<Node<Widget>> focused) {
    auto allowed = alive();

    if (!allowed) {
      return allowed;
    }
    if (busy) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    std::expected<void, TreeError> result;
    auto edited = tree.edit(node, [&](LineEdit &edit) {
      try {
        result = syncModel(edit);

        if (!result || !focused) {
          return;
        }

        auto eligible = tree.inputEligible(node);

        if (!eligible) {
          result = std::unexpected(eligible.error());

          return;
        }

        const bool hasFocus = *eligible && focused->id() == node.id();

        if (edit.focused_ != hasFocus || edit.enabled_ != *eligible) {
          result = tree.invalidate(node, Invalidation::PAINT);

          if (result) {
            edit.focused_ = hasFocus;
            edit.enabled_ = *eligible;
          }
        }
      } catch (const std::bad_alloc &) {
        result = std::unexpected(TreeError::NO_MEMORY);
      }
    });

    return edited ? result : edited;
  }

  LineEditResult replace(LineEdit &edit, std::size_t begin, std::size_t end, std::string_view inserted,
                         TextHistoryKind kind) {
    const auto old = edit.text_.bytes();
    const auto remaining = old.size() - (end - begin);

    if (remaining > edit.options_.limits.max_input_bytes ||
        inserted.size() > edit.options_.limits.max_input_bytes - remaining) {
      return {LineEditStatus::LIMIT_EXCEEDED};
    }

    if (detail::hasLineSeparator(inserted)) {
      return {LineEditStatus::INVALID_TEXT};
    }

    std::string candidate(old.substr(0, begin));

    candidate.append(inserted);
    candidate.append(old.substr(end));

    auto text = GraphemeText::from(candidate, edit.options_.widthPolicy, edit.options_.limits);

    if (!text) {
      return {textStatus(text.error())};
    }

    // Inserted scalars can merge with either adjacent cluster (combining marks, ZWJ,
    // regional indicators). Re-segment first and choose the next legal byte boundary.
    const auto requested = begin + inserted.size();
    auto cursor = requested;

    for (const auto &cluster : text->graphemes()) {
      if (cluster.byte_begin < requested && requested < cluster.byte_end) {
        cursor = cluster.byte_end;

        break;
      }
    }

    const bool changed = edit.text_.bytes() != text->bytes();

    if (!changed && edit.cursor_ == cursor && edit.anchor_ == cursor) {
      return {LineEditStatus::EDITED};
    }

    auto prepared = history->prepareEdit(snapshot(edit), {std::string(text->bytes()), cursor, cursor}, kind);

    if (!prepared) {
      return prepared.error() == TextHistoryError::WOULD_BLOCK ? LineEditResult{LineEditStatus::LIMIT_EXCEEDED}
                                                               : failed(historyError(prepared.error()));
    }

    auto applied = accept(edit, *prepared, changed);

    if (!applied) {
      return failed(applied.error());
    }

    edit.dirty_ = changed || edit.dirty_;
    userTextChanged = changed || userTextChanged;

    return changed && edit.options_.editMode == EditMode::IMMEDIATE ? commit(edit, false)
                                                                    : LineEditResult{LineEditStatus::EDITED};
  }

  LineEditResult insert(LineEdit &edit, std::string_view bytes, TextHistoryKind kind = TextHistoryKind::TYPING) {
    if (kind == TextHistoryKind::TYPING && edit.cursor_ != edit.anchor_) {
      kind = TextHistoryKind::REPLACEMENT;
    }

    return replace(edit, std::min(edit.cursor_, edit.anchor_), std::max(edit.cursor_, edit.anchor_), bytes, kind);
  }

  LineEditResult commit(LineEdit &edit, bool closeGrouping = true) {
    if (closeGrouping) {
      history->stopGrouping();
    }

    auto snapshot = model->snapshot();

    if (!snapshot) {
      return failed(snapshot.error());
    }

    if (revision != (*snapshot)->revision) {
      return {LineEditStatus::CONFLICT};
    }

    // The predicate may reenter or remove the node. Own the candidate and revalidate
    // both node and model before committing; never borrow widget text across user code.
    std::string candidate(edit.text_.bytes());

    if (validate && !validate(candidate)) {
      return {LineEditStatus::VALIDATION_FAILED};
    }

    auto allowed = alive();

    if (!allowed) {
      return failed(allowed.error());
    }

    snapshot = model->snapshot();

    if (!snapshot) {
      return failed(snapshot.error());
    }

    if (revision != (*snapshot)->revision) {
      return {LineEditStatus::CONFLICT};
    }

    auto updated = model->set(candidate);

    if (!updated) {
      return failed(updated.error());
    }

    // Property pruning may release reentrant user captures and remove the node.
    // Preserve the exact successful receipt even when presentation no longer exists.
    auto applied = tree.edit(node, [&](LineEdit &current) {
      current.dirty_ = false;
      revision = updated->snapshot->revision;
    });
    LineEditResult result{LineEditStatus::COMMITTED, std::move(*updated)};

    if (!applied && applied.error() != TreeError::STALE_NODE && applied.error() != TreeError::CLOSED) {
      result.error = applied.error();
    }

    return result;
  }

  LineEditResult action(LineEdit &edit, ActionEvent event) {
    if (event.action == Action::ACTIVATE) {
      return commit(edit);
    }

    if (event.action == Action::CANCEL) {
      auto snapshot = model->snapshot();
      const bool changed = snapshot && edit.text_.bytes() != (*snapshot)->value;
      auto status = snapshot ? adopt(edit, **snapshot) : std::unexpected(snapshot.error());
      userTextChanged = status && changed;

      return status ? LineEditResult{LineEditStatus::CANCELLED} : failed(status.error());
    }

    auto begin = std::min(edit.cursor_, edit.anchor_);
    auto end = std::max(edit.cursor_, edit.anchor_);

    if (event.action == Action::ERASE_BACKWARD || event.action == Action::ERASE_FORWARD) {
      if (begin == end) {
        if (event.action == Action::ERASE_BACKWARD) {
          begin = edit.text_.previousCursor(begin).value();
        } else {
          end = edit.text_.nextCursor(end).value();
        }
      }

      const auto kind = edit.cursor_ != edit.anchor_             ? TextHistoryKind::REPLACEMENT
                        : event.action == Action::ERASE_BACKWARD ? TextHistoryKind::BACKSPACE
                                                                 : TextHistoryKind::FORWARD_DELETE;

      return replace(edit, begin, end, "", kind);
    }

    history->stopGrouping();

    auto cursor = edit.cursor_;

    switch (event.action) {
    case Action::MOVE_LEFT:
      cursor = !event.modifiers.shift && begin != end ? begin : edit.text_.previousCursor(cursor).value();

      break;
    case Action::MOVE_RIGHT:
      cursor = !event.modifiers.shift && begin != end ? end : edit.text_.nextCursor(cursor).value();

      break;
    case Action::MOVE_HOME:
      cursor = 0;

      break;
    case Action::MOVE_END:
      cursor = edit.text_.bytes().size();

      break;
    default:
      break;
    }

    if (cursor == edit.cursor_ && (event.modifiers.shift || edit.anchor_ == cursor)) {
      return {LineEditStatus::EDITED};
    }

    auto invalidated = tree.invalidate(node, Invalidation::PAINT);

    if (!invalidated) {
      return failed(invalidated.error());
    }

    edit.cursor_ = cursor;

    if (!event.modifiers.shift) {
      edit.anchor_ = cursor;
    }

    edit.revealCursor();

    return {LineEditStatus::EDITED};
  }

  LineEditResult shortcut(LineEdit &edit, char32_t key) {
    if (key == U'a') {
      history->stopGrouping();

      auto invalidated = tree.invalidate(node, Invalidation::PAINT);

      if (!invalidated) {
        return failed(invalidated.error());
      }

      edit.anchor_ = 0;
      edit.cursor_ = edit.text_.bytes().size();
      edit.revealCursor();

      return {LineEditStatus::EDITED};
    }

    if (!clipboard) {
      return {LineEditStatus::CLIPBOARD_FAILED};
    }

    if (key == U'v') {
      auto bytes = clipboard->read(edit.options_.limits.max_input_bytes);

      if (!bytes) {
        return {LineEditStatus::CLIPBOARD_FAILED, {}, bytes.error()};
      }

      auto allowed = alive();

      return allowed ? insert(edit, *bytes, TextHistoryKind::PASTE) : failed(allowed.error());
    }

    const auto begin = std::min(edit.cursor_, edit.anchor_);
    const auto end = std::max(edit.cursor_, edit.anchor_);
    const std::string selected(edit.text_.bytes().substr(begin, end - begin));
    auto written = clipboard->write(selected);

    if (!written) {
      return {LineEditStatus::CLIPBOARD_FAILED, {}, written.error()};
    }

    auto allowed = alive();

    if (!allowed) {
      return failed(allowed.error());
    }

    return key == U'x' ? replace(edit, begin, end, "", TextHistoryKind::CUT) : LineEditResult{LineEditStatus::COPIED};
  }

  std::optional<bool> historyKey(const LineEdit &edit, const KeyEvent *key) const {
    if (!key) {
      return {};
    }

    const auto &shortcuts = edit.options_.historyShortcuts;

    if (shortcuts.undo && matches(*shortcuts.undo, *key)) {
      return false;
    }

    for (const auto &chord : shortcuts.redo) {
      if (chord && matches(*chord, *key)) {
        return true;
      }
    }

    return {};
  }

  bool readOnlyBlocks(const LineEdit &edit, const RoutedEvent &event) const {
    if (!edit.options_.readOnly) {
      return false;
    }

    if (std::get_if<TextInputEvent>(&event) || std::get_if<PasteInputEvent>(&event)) {
      return true;
    }

    if (const auto *action = std::get_if<ActionEvent>(&event)) {
      return action->action == Action::ERASE_BACKWARD || action->action == Action::ERASE_FORWARD;
    }

    const auto scalar = shortcutCharacter(std::get_if<KeyEvent>(&event));

    return scalar == U'x' || scalar == U'v';
  }

  struct HistoryOperation {
    std::expected<void, TreeError> result;
    std::optional<LineEditResult> outcome;
  };

  HistoryOperation clear(LineEdit &edit) {
    auto eligible = tree.inputEligible(node);
    if (!eligible) {
      return {std::unexpected(eligible.error()), {}};
    }
    if (!*eligible || edit.options_.readOnly) {
      return {std::unexpected(TreeError::WOULD_BLOCK), {}};
    }
    if (edit.text_.bytes().empty()) {
      return {{}, {}};
    }

    // PROGRAMMATIC stages a separate group; never close the live typing group before fallible preparation.
    auto prepared = history->prepareEdit(snapshot(edit), {std::string(0, '\0'), 0, 0}, TextHistoryKind::PROGRAMMATIC);
    if (!prepared) {
      const auto error = historyError(prepared.error());
      return {std::unexpected(error), failed(error)};
    }
    auto applied = accept(edit, *prepared, true);
    if (!applied) {
      return {applied, failed(applied.error())};
    }

    edit.dirty_ = true;
    userTextChanged = true;
    auto outcome =
      edit.options_.editMode == EditMode::IMMEDIATE ? commit(edit, false) : LineEditResult{LineEditStatus::EDITED};
    return {outcome.error ? std::expected<void, TreeError>(std::unexpected(*outcome.error))
                          : std::expected<void, TreeError>{},
            std::move(outcome)};
  }

  HistoryOperation restore(LineEdit &edit, bool redo) {
    auto eligible = tree.inputEligible(node);

    if (!eligible) {
      return {std::unexpected(eligible.error()), {}};
    }

    if (!*eligible || edit.options_.readOnly) {
      return {std::unexpected(TreeError::WOULD_BLOCK), {}};
    }

    auto latest = model->snapshot();

    if (!latest) {
      return {std::unexpected(latest.error()), {}};
    }

    if (revision != (*latest)->revision) {
      if (edit.dirty_) {
        return {std::unexpected(TreeError::WOULD_BLOCK), LineEditResult{LineEditStatus::CONFLICT}};
      }

      return {adopt(edit, **latest), {}};
    }

    auto prepared = redo ? history->prepareRedo() : history->prepareUndo();

    if (!prepared) {
      if (prepared.error() == TextHistoryError::EMPTY) {
        history->stopGrouping();

        return {{}, {}};
      }

      return {std::unexpected(historyError(prepared.error())), {}};
    }

    const bool changed = prepared->text() && edit.text_.bytes() != prepared->text()->bytes();
    auto applied = accept(edit, *prepared, changed);

    if (!applied) {
      return {applied, {}};
    }

    edit.dirty_ = true;
    userTextChanged = changed;
    auto outcome =
      edit.options_.editMode == EditMode::IMMEDIATE ? commit(edit, false) : LineEditResult{LineEditStatus::EDITED};

    return {outcome.error ? std::expected<void, TreeError>(std::unexpected(*outcome.error))
                          : std::expected<void, TreeError>{},

            std::move(outcome)};
  }

  std::optional<LineEditResult> compute(LineEdit &edit, const RoutedEvent &event) {
    const auto *command = std::get_if<ActionEvent>(&event);
    const auto *text = std::get_if<TextInputEvent>(&event);
    const auto *paste = std::get_if<PasteInputEvent>(&event);
    const auto *key = std::get_if<KeyEvent>(&event);
    const auto shortcutScalar = shortcutCharacter(key);
    const bool shortcutKey = shortcutScalar != U'\0';
    const bool commandKey = acceptsAction(command);
    const auto historyShortcut = historyKey(edit, key);

    if (!commandKey && !(text && !text->modifiers.alt && !text->modifiers.control && !text->modifiers.super) &&
        !paste && !shortcutKey && !historyShortcut) {
      return {};
    }

    try {
      if (historyShortcut) {
        auto operation = restore(edit, *historyShortcut);

        return operation.outcome   ? operation.outcome
               : !operation.result ? std::optional(failed(operation.result.error()))
                                   : std::nullopt;
      }

      auto synced = syncModel(edit);

      if (!synced) {
        return failed(synced.error());
      }

      if (commandKey) {
        return action(edit, *command);
      }

      if (shortcutKey) {
        return shortcut(edit, shortcutScalar);
      }

      if (paste) {
        return insert(edit, paste->bytes, TextHistoryKind::PASTE);
      }

      const auto encoded = encode(text->scalar);

      return insert(edit, encoded.view());
    } catch (const std::bad_alloc &) {
      return failed(TreeError::NO_MEMORY);
    } catch (...) {
      return failed(TreeError::CALLBACK_FAILED);
    }
  }

  EventReply handle(LineEdit &edit, const RoutedEvent &event) {
    if (busy) {
      return EventReply::HANDLED;
    }

    BusyGuard guard(busy);
    userTextChanged = false;

    const auto shortcut = shortcutCharacter(std::get_if<KeyEvent>(&event));
    if (edit.options_.displayMode != LineEditDisplayMode::NORMAL && (shortcut == U'c' || shortcut == U'x')) {
      return EventReply::HANDLED;
    }

    if (readOnlyBlocks(edit, event)) {
      return EventReply::HANDLED;
    }

    const bool historyShortcut = historyKey(edit, std::get_if<KeyEvent>(&event)).has_value();
    auto outcome = compute(edit, event);

    if (outcome) {
      // Last widget access occurred in compute; result may remove the node or binding.
      const auto *command = std::get_if<ActionEvent>(&event);

      deliver(*outcome, command && command->action == Action::ACTIVATE);
    }

    return outcome || historyShortcut ? EventReply::HANDLED : EventReply::IGNORED;
  }

  std::expected<void, TreeError> focus(bool focused) {
    if (busy && (!delivering || completionDelivery)) {
      // Focus publication can reenter from an admitted callback. Keep its presentation in sync,
      // but do not turn the same operation into another commit or completion observation.
      return tree.edit(node, [&](LineEdit &edit) {
        edit.focused_ = focused;
      });
    }

    BusyGuard guard(busy);

    if (!focused) {
      history->stopGrouping();
    }

    std::optional<LineEditResult> outcome;
    EditingFinishedOutcome completion = EditingFinishedOutcome::UNCHANGED;
    auto edited = tree.edit(node, [&](LineEdit &edit) {
      edit.focused_ = focused;

      if (!focused && edit.dirty_) {
        completion = EditingFinishedOutcome::DEFERRED;
      }

      if (!focused && edit.dirty_ && edit.options_.editMode == EditMode::ON_COMPLETION) {
        try {
          outcome = commit(edit);
        } catch (const std::bad_alloc &) {
          outcome = failed(TreeError::NO_MEMORY);
        } catch (...) {
          outcome = failed(TreeError::CALLBACK_FAILED);
        }
      }
    });

    if (!edited) {
      return edited;
    }

    if (outcome) {
      outcome->trigger = LineEditTrigger::BLUR;
      BusyGuard delivery(delivering);
      BusyGuard completionGuard(completionDelivery);

      onResult(*outcome);
      completion = finishOutcome(*outcome);
    }

    if (!focused && events.editingFinished && alive()) {
      BusyGuard delivery(delivering);
      BusyGuard completionGuard(completionDelivery);

      events.editingFinished({EditingFinishedReason::FOCUS_LOST, completion, outcome});
    }

    return outcome && outcome->error ? std::expected<void, TreeError>(std::unexpected(*outcome->error))
                                     : std::expected<void, TreeError>{};
  }
};

LineEditBinding::LineEditBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription)
    : state_(std::move(state)), action_(std::move(action)), subscription_(std::move(subscription)) {
}

LineEditBinding::~LineEditBinding() = default;
LineEditBinding::LineEditBinding(LineEditBinding &&) noexcept = default;

LineEditBinding &LineEditBinding::operator=(LineEditBinding &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  LineEditBinding retired(std::move(other));

  using std::swap;

  swap(state_, retired.state_);
  swap(action_, retired.action_);
  swap(subscription_, retired.subscription_);

  return *this;
}

std::expected<void, TreeError> LineEditBinding::sync(Node<Widget> focused) {
  // A retained state also protects against reentrant destruction during synchronization.
  auto state = state_;

  if (!state) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return state->sync(focused);
}

std::expected<std::uint64_t, TreeError> LineEditBinding::draftRevision() const {
  auto state = state_;

  if (!state) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto allowed = state->alive();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state->busy) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  return state->revision;
}

std::expected<void, TreeError> LineEditBinding::setDraft(GraphemeText text, std::uint64_t revision,
                                                         std::optional<ProgrammaticChange> change) {
  auto state = state_;

  if (!state) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto allowed = state->alive();

  if (!allowed) {
    return allowed;
  }

  if (state->busy) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  State::BusyGuard guard(state->busy);
  state->userTextChanged = false;
  std::expected<void, TreeError> updated;
  std::optional<LineEditResult> outcome;

  bool accepted = false;
  auto edited = state->tree.edit(state->node, [&](LineEdit &edit) {
    try {
      const auto policy = change.value_or(edit.options_.programmaticChange);

      if (policy != ProgrammaticChange::RECORD && policy != ProgrammaticChange::RESET_BASELINE) {
        updated = std::unexpected(TreeError::INVALID_ARGUMENT);

        return;
      }

      auto latest = state->model->snapshot();

      if (!latest) {
        updated = std::unexpected(latest.error());

        return;
      }

      const bool changed = edit.text_.bytes() != text.bytes() || edit.text_.policy() != text.policy();

      if (!changed && policy == ProgrammaticChange::RECORD && state->revision == revision) {
        return;
      }

      TextHistorySnapshot after{std::string(text.bytes()), changed ? text.bytes().size() : edit.cursor_,
                                changed ? text.bytes().size() : edit.anchor_};
      auto prepared = policy == ProgrammaticChange::RESET_BASELINE
                        ? state->history->prepareResetBaseline(after)
                        : state->history->prepareEdit(state->snapshot(edit), after, TextHistoryKind::PROGRAMMATIC);

      if (!prepared) {
        updated = std::unexpected(historyError(prepared.error()));

        return;
      }

      updated = state->accept(edit, *prepared, changed);

      if (!updated) {
        return;
      }

      accepted = true;

      const bool baselineChanged = state->revision != revision;

      state->revision = policy == ProgrammaticChange::RESET_BASELINE ? (*latest)->revision : revision;

      if (changed || baselineChanged) {
        edit.dirty_ = true;
        outcome = edit.options_.editMode == EditMode::IMMEDIATE ? state->commit(edit, false)
                                                                : LineEditResult{LineEditStatus::EDITED};
      }
    } catch (const std::bad_alloc &) {
      if (accepted) {
        outcome = failed(TreeError::NO_MEMORY);
      } else {
        updated = std::unexpected(TreeError::NO_MEMORY);
      }
    } catch (...) {
      if (accepted) {
        outcome = failed(TreeError::CALLBACK_FAILED);
      } else {
        updated = std::unexpected(TreeError::CALLBACK_FAILED);
      }
    }
  });

  if (edited && updated && outcome) {
    outcome->trigger = LineEditTrigger::FLUENT;
    state->onResult(*outcome);
  }

  return edited ? updated : edited;
}

void LineEditBinding::commandFeedback(std::shared_ptr<detail::EditorCommandFeedback> feedback) {
  state_->feedback = std::move(feedback);
}

std::pair<std::expected<void, TreeError>, bool> LineEditBinding::historyCommand(bool redo,
                                                                                std::source_location origin) {
  return editorCommand(redo, false, origin);
}

std::pair<std::expected<void, TreeError>, bool> LineEditBinding::clearCommand(std::source_location origin) {
  return editorCommand(false, true, origin);
}

std::pair<std::expected<void, TreeError>, bool> LineEditBinding::editorCommand(bool redo, bool clear,
                                                                               std::source_location origin) {
  auto state = state_;

  if (!state) {
    return {std::unexpected(TreeError::INVALID_ARGUMENT), false};
  }

  auto allowed = state->alive();

  if (!allowed) {
    return {allowed, false};
  }

  if (state->busy) {
    return {std::unexpected(TreeError::WOULD_BLOCK), false};
  }

  State::BusyGuard guard(state->busy);
  state->userTextChanged = false;
  auto feedback = state->feedback;

  if (feedback) {
    feedback->active = true;
    feedback->reported = false;
    feedback->operation = clear ? "lineEdit.clear" : redo ? "lineEdit.redo" : "lineEdit.undo";
    feedback->origin = origin;
  }

  struct FeedbackGuard {
    std::shared_ptr<detail::EditorCommandFeedback> feedback;

    ~FeedbackGuard() {
      if (feedback) {
        feedback->active = false;
      }
    }
  } feedbackGuard{feedback};

  State::HistoryOperation operation;
  auto edited = state->tree.edit(state->node, [&](LineEdit &edit) {
    try {
      operation = clear ? state->clear(edit) : state->restore(edit, redo);
    } catch (const std::bad_alloc &) {
      operation = {std::unexpected(TreeError::NO_MEMORY), failed(TreeError::NO_MEMORY)};
    } catch (...) {
      operation = {std::unexpected(TreeError::CALLBACK_FAILED), failed(TreeError::CALLBACK_FAILED)};
    }
  });

  if (!edited) {
    return {edited, false};
  }

  if (operation.outcome) {
    operation.outcome->trigger = LineEditTrigger::FLUENT;
    try {
      state->deliver(*operation.outcome);
    } catch (const std::bad_alloc &) {
      return {std::unexpected(TreeError::NO_MEMORY), false};
    } catch (...) {
      return {std::unexpected(TreeError::CALLBACK_FAILED), false};
    }
  }

  return {operation.result, feedback && feedback->reported};
}

std::expected<void, TreeError> LineEditBinding::undo() {
  return historyCommand(false).first;
}

std::expected<void, TreeError> LineEditBinding::redo() {
  return historyCommand(true).first;
}

std::expected<void, TreeError> LineEditBinding::clear() {
  return clearCommand(std::source_location::current()).first;
}

std::expected<void, TreeError> LineEditBinding::stopGrouping() {
  auto state = state_;

  if (!state) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto allowed = state->alive();

  if (!allowed) {
    return allowed;
  }

  if (state->busy) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  state->history->stopGrouping();

  return {};
}

std::expected<HistoryAvailability, TreeError> LineEditBinding::historyAvailability() const {
  auto state = state_;

  if (!state) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto allowed = state->alive();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state->busy) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto eligible = state->tree.inputEligible(state->node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  auto latest = state->model->snapshot();

  if (!latest) {
    return std::unexpected(latest.error());
  }

  HistoryAvailability result;

  auto inspected = state->tree.read(state->node, [&](const LineEdit &edit) {
    if (*eligible && !edit.options_.readOnly && state->revision == (*latest)->revision) {
      result = {state->history->canUndo(), state->history->canRedo()};
    }
  });

  return inspected ? std::expected<HistoryAvailability, TreeError>(result) : std::unexpected(inspected.error());
}

bool LineEditBinding::canUndo() const {
  auto value = historyAvailability();

  return value && value->canUndo;
}

bool LineEditBinding::canRedo() const {
  auto value = historyAvailability();

  return value && value->canRedo;
}

std::expected<LineEditBinding, TreeError> bindLineEdit(WidgetTree &tree, EventRouter &router, Node<LineEdit> node,
                                                       std::shared_ptr<Property<std::string>> model,
                                                       std::function<void(LineEditResult)> result,
                                                       std::shared_ptr<Clipboard> clipboard,
                                                       std::function<bool(std::string_view)> validate) {
  return bindLineEditWithEvents(tree, router, node, std::move(model), std::move(result), std::move(clipboard),
                                std::move(validate), {});
}

std::expected<LineEditBinding, TreeError>
bindLineEditWithEvents(WidgetTree &tree, EventRouter &router, Node<LineEdit> node,
                       std::shared_ptr<Property<std::string>> model, std::function<void(LineEditResult)> result,
                       std::shared_ptr<Clipboard> clipboard, std::function<bool(std::string_view)> validate,
                       LineEditEvents events) {
  if (!model || !result) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    bool validMode = false;
    LineEditOptions options;
    auto inspected = tree.read(node, [&](const LineEdit &edit) {
      options = LineEditBinding::State::options(edit);
      validMode = LineEditBinding::State::validMode(edit) && validShortcuts(options.historyShortcuts) &&
                  (options.programmaticChange == ProgrammaticChange::RECORD ||
                   options.programmaticChange == ProgrammaticChange::RESET_BASELINE);
    });

    if (!inspected || !validMode) {
      return std::unexpected(inspected ? TreeError::INVALID_ARGUMENT : inspected.error());
    }

    auto state = std::make_shared<LineEditBinding::State>(
      LineEditBinding::State{tree, node, std::move(model), std::move(clipboard), std::move(validate), std::move(result),

                             std::make_unique<TextHistory>(options.history, options.widthPolicy, options.limits)});
    state->events = std::move(events);

    auto subscription = state->model->observe(node, [state](LineEdit &, auto) {
      // Read latest rather than replaying a potentially obsolete delivered snapshot.
      auto synced = state->sync({});

      if (!synced) {
        throw synced.error();
      }
    });

    if (!subscription) {
      return std::unexpected(subscription.error());
    }

    auto action = router.bind(
      node,
      [state](LineEdit &edit, const RoutedEvent &event) {
        return state->handle(edit, event);
      },
      [state](LineEdit &, bool focused) {
        return state->focus(focused);
      });

    if (!action) {
      return std::unexpected(action.error());
    }

    auto synced = state->sync({});

    if (!synced) {
      return std::unexpected(synced.error());
    }

    return LineEditBinding(std::move(state), std::move(*action), std::move(*subscription));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals
