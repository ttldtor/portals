// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/password.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace portals {
namespace {
bool validOptions(const PasswordOptions &options) noexcept {
  return (options.editMode == EditMode::IMMEDIATE || options.editMode == EditMode::ON_COMPLETION ||
          options.editMode == EditMode::EXPLICIT) &&
         (options.displayMode == PasswordDisplayMode::PASSWORD ||
          options.displayMode == PasswordDisplayMode::NO_ECHO) &&
         (options.pastePolicy == PasswordPastePolicy::DISABLED ||
          options.pastePolicy == PasswordPastePolicy::ENABLED) &&
         options.securePolicy.storage == SecureStringStorage::NATIVE_GUARDED && options.securePolicy.encryptWhileIdle &&
         options.securePolicy.maxBytes <= MAX_SECRET_BYTES;
}

PasswordResult failed(PasswordFailure failure) {
  return {PasswordStatus::FAILED, {}, std::move(failure)};
}

PasswordStatus textStatus(TextError error) noexcept {
  return error.kind == TextErrorKind::INVALID_UTF8 || error.kind == TextErrorKind::INVALID_POLICY
           ? PasswordStatus::INVALID_TEXT
           : PasswordStatus::LIMIT_EXCEEDED;
}

PasswordResult preparationFailure(PasswordFailure failure) {
  auto result = failed(std::move(failure));

  if (const auto *text = std::get_if<TextError>(&*result.failure)) {
    result.status = textStatus(*text);
  } else if (std::holds_alternative<PasswordInputError>(*result.failure)) {
    result.status = PasswordStatus::INVALID_TEXT;
  } else if (const auto *access = std::get_if<PasswordValueAccessFailure<TextError>>(&*result.failure)) {
    if (access->access && access->access->callbackError) {
      result.status = textStatus(*access->access->callbackError);
    }
  } else if (const auto *secure = std::get_if<SecureStringFailure>(&*result.failure)) {
    if (secure->primary.code == SecureStringErrorCode::TOO_LARGE) {
      result.status = PasswordStatus::LIMIT_EXCEEDED;
    }
  }

  return result;
}

// Volatile stores are observable side effects and cannot be removed as dead stores.
// This only covers owned temporary bytes, never decoder/system/clipboard service storage.
void wipe(std::span<char> bytes) noexcept {
  auto *cursor = static_cast<volatile char *>(bytes.data());

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    cursor[index] = 0;
  }
}

struct EncodedScalar {
  std::array<char, 4> bytes{};
  std::size_t size{};

  ~EncodedScalar() {
    wipe(bytes);
  }

  std::string_view view() const noexcept {
    return {bytes.data(), size};
  }
};

void encode(char32_t scalar, EncodedScalar &encoded) noexcept {
  const auto append = [&](char byte) {
    encoded.bytes[encoded.size++] = byte;
  };

  if (scalar <= 0x7f) {
    append(static_cast<char>(scalar));
  } else if (scalar <= 0x7ff) {
    append(static_cast<char>(0xc0 | (scalar >> 6)));
    append(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else if (scalar <= 0xffff && !(scalar >= 0xd800 && scalar <= 0xdfff)) {
    append(static_cast<char>(0xe0 | (scalar >> 12)));
    append(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    append(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else if (scalar <= 0x10ffff && scalar > 0xffff) {
    append(static_cast<char>(0xf0 | (scalar >> 18)));
    append(static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)));
    append(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    append(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else {
    append('\xff');
  }
}

char32_t shortcut(const KeyEvent *key) noexcept {
  if (!key || key->key != InputKey::CHARACTER || key->action == KeyAction::RELEASE ||
      key->modifiers != InputModifiers{.control = true}) {
    return 0;
  }

  auto scalar = key->scalar;

  if (scalar >= U'A' && scalar <= U'Z') {
    scalar += U'a' - U'A';
  }

  return scalar == U'a' || scalar == U'c' || scalar == U'x' || scalar == U'v' ? scalar : 0;
}

bool accepts(const ActionEvent *event) noexcept {
  if (!event || event->modifiers.alt || event->modifiers.super) {
    return false;
  }

  switch (event->action) {
  case Action::MOVE_LEFT:
  case Action::MOVE_RIGHT:
    return true;
  case Action::MOVE_HOME:
  case Action::MOVE_END:
    return !event->modifiers.control;
  case Action::ACTIVATE:
  case Action::CANCEL:
  case Action::ERASE_BACKWARD:
  case Action::ERASE_FORWARD:
    return !event->modifiers.shift && !event->modifiers.control;
  default:
    return false;
  }
}
} // namespace

Password::Password(PasswordOptions options, SizePolicy policy) : SizedWidget(policy), options_(std::move(options)) {
  if (!validOptions(options_)) {
    throw std::invalid_argument("Invalid Password options");
  }

  // This validation touches only explicitly public presentation, never a secret.
  auto empty = GraphemeText::from({}, options_.widthPolicy, options_.limits);

  if (!empty) {
    throw std::bad_expected_access(empty.error());
  }

  masked_ = std::make_unique<GraphemeText>(std::move(*empty));

  if (options_.placeholder && !options_.placeholder->empty()) {
    auto metadata = detail::scanTextMetadata(*options_.placeholder, options_.widthPolicy, options_.limits);

    if (!metadata) {
      throw std::bad_expected_access(metadata.error());
    }

    if (metadata->lineSeparator) {
      throw std::invalid_argument("Invalid Password placeholder");
    }

    placeholder_ = std::make_unique<GraphemeText>(
      GraphemeText::from(*options_.placeholder, options_.widthPolicy, options_.limits).value());
  }

  options_.placeholder.reset();
}

std::expected<CellSize, TreeError> Password::measure(CellSize) {
  const auto width = options_.displayMode == PasswordDisplayMode::NO_ECHO ? 0
                     : metadata_.graphemes.empty() && placeholder_        ? placeholder_->measure()
                     : masked_                                            ? masked_->measure()
                                                                          : 0;

  if (width == std::numeric_limits<std::int32_t>::max()) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  return CellSize{width + 1, 1};
}

std::expected<void, TreeError> Password::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;
  revealCursor();

  return {};
}

std::int32_t Password::cellAt(std::size_t byte) const noexcept {
  if (options_.displayMode == PasswordDisplayMode::NO_ECHO) {
    return 0;
  }

  const auto found = std::ranges::lower_bound(metadata_.graphemes, byte, {}, &Grapheme::byte_begin);

  return static_cast<std::int32_t>(found - metadata_.graphemes.begin());
}

void Password::revealCursor() noexcept {
  if (options_.displayMode == PasswordDisplayMode::NO_ECHO) {
    scroll_ = 0;

    return;
  }

  const auto caret = cellAt(cursor_);
  const auto width = bounds_.size().width;

  if (static_cast<std::int64_t>(metadata_.graphemes.size()) + 1 <= width) {
    scroll_ = 0;
  } else if (caret < scroll_) {
    scroll_ = caret;
  } else if (width > 0 && caret - scroll_ >= width) {
    scroll_ = caret - width + 1;
  }
}

std::expected<void, TreeError> Password::paint(PaintContext &context) {
  std::optional<ComputedStyle> manualStyle;
  if (!computedStyle()) {
    manualStyle.emplace(styleDefaults());
  }
  const auto resolved =
    computedStyle() ? detail::ComputedStyleView(*computedStyle()) : detail::ComputedStyleView(*manualStyle);
  const auto style = !enabled_ ? resolved.disabled : focused_ ? resolved.focused : resolved.normal;
  const auto selectionBegin = cellAt(std::min(cursor_, anchor_)) - scroll_;
  const auto selectionEnd = cellAt(std::max(cursor_, anchor_)) - scroll_;
  const auto selection = CellRect::from({selectionBegin, 0}, {selectionEnd - selectionBegin, 1}).value();
  const auto *shown = options_.displayMode == PasswordDisplayMode::NO_ECHO ? nullptr
                      : metadata_.graphemes.empty()                        ? placeholder_.get()
                                                                           : masked_.get();
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

  const auto area = CellRect::from({cellAt(cursor_) - scroll_, 0}, {1, 1}).value();
  const std::array<RenderCommand, 1> caret{FillCommand{area, bounds_, resolved.caret}};
  submitted = context.submit(caret);

  if (submitted && shown) {
    const std::array<RenderCommand, 1> glyph{
      TextCommand{{-scroll_, 0}, *shown, area.intersection(bounds_), resolved.caret}};
    submitted = context.submit(glyph);
  }

  return submitted;
}

std::size_t Password::cursor() const noexcept {
  return cursor_;
}

std::size_t Password::anchor() const noexcept {
  return anchor_;
}

bool Password::dirty() const noexcept {
  return dirty_;
}

ComputedStyle Password::styleDefaults() const {
  ComputedStyle style;

  style.normal = options_.style.normal;
  style.focused = options_.style.focused;
  style.disabled = options_.style.disabled;
  style.selected = options_.selection;
  style.caret = options_.cursor;

  return style;
}

struct PasswordBinding::State {
  WidgetTree &tree;
  Node<Password> node;
  std::shared_ptr<Property<PasswordValue>> model;
  std::shared_ptr<Clipboard> clipboard;
  std::function<bool(SecretView)> validate;
  std::function<void(PasswordResult)> onResult;
  std::uint64_t revision{};
  bool initialized{};
  bool busy{};

  struct BusyGuard {
    bool &busy;

    explicit BusyGuard(bool &value) : busy(value) {
      busy = true;
    }

    ~BusyGuard() {
      busy = false;
    }
  };

  struct Prepared {
    PasswordValue value;
    detail::TextMetadata metadata;
    std::unique_ptr<GraphemeText> mask;
    std::size_t cursor{};
  };

  std::expected<void, PasswordFailure> alive() const {
    auto result = tree.withScope(node, [](const UiScope &) {
    });

    return result ? std::expected<void, PasswordFailure>{} : std::unexpected(PasswordFailure{result.error()});
  }

  std::expected<Prepared, PasswordFailure> prepare(PasswordValue value, const PasswordOptions &options,
                                                   std::size_t requested) {
    const auto capabilities = value.capabilities();
    const auto actualPolicy = value.options();

    if (!value.available() || actualPolicy.storage != SecureStringStorage::NATIVE_GUARDED ||
        !capabilities.encryptedWhileIdle || !capabilities.guardPages || !capabilities.wiping ||
        (options.securePolicy.lockPages && !capabilities.lockedPages) ||
        (options.securePolicy.excludeFromDumps && !capabilities.excludedFromDumps)) {
      return std::unexpected(
        PasswordFailure{SecureStringFailure{{SecureStringOperation::BORROW, SecureStringErrorCode::UNAVAILABLE}, {}}});
    }

    if (value.size() > options.securePolicy.maxBytes) {
      return std::unexpected(
        PasswordFailure{SecureStringFailure{{SecureStringOperation::BORROW, SecureStringErrorCode::TOO_LARGE}, {}}});
    }

    Prepared prepared;
    auto accessed = value.withPlaintext([&](SecretView bytes) -> std::expected<void, TextError> {
      auto metadata = detail::scanTextMetadata({bytes.data(), bytes.size()}, options.widthPolicy, options.limits, true);

      if (!metadata) {
        return std::unexpected(metadata.error());
      }

      prepared.metadata.graphemes.swap(metadata->graphemes);
      prepared.metadata.wordStarts.swap(metadata->wordStarts);
      prepared.metadata.cells = metadata->cells;
      prepared.metadata.lineSeparator = metadata->lineSeparator;

      return {};
    });

    if (!accessed) {
      return std::unexpected(PasswordFailure{std::move(accessed.error())});
    }

    if (prepared.metadata.lineSeparator) {
      return std::unexpected(PasswordFailure{PasswordInputError::LINE_SEPARATOR});
    }

    if (options.displayMode == PasswordDisplayMode::PASSWORD) {
      // Only constant public mask bytes enter GraphemeText; never pass the secret view.
      auto mask =
        GraphemeText::from(std::string(prepared.metadata.graphemes.size(), '*'), options.widthPolicy, options.limits);

      if (!mask) {
        return std::unexpected(PasswordFailure{mask.error()});
      }

      prepared.mask = std::make_unique<GraphemeText>(std::move(*mask));
    }

    prepared.cursor = std::min(requested, value.size());

    for (const auto &cluster : prepared.metadata.graphemes) {
      if (cluster.byte_begin < prepared.cursor && prepared.cursor < cluster.byte_end) {
        prepared.cursor = cluster.byte_end;

        break;
      }
    }

    prepared.value = std::move(value);

    return prepared;
  }

  static void install(Password &edit, Prepared &prepared, bool dirty) noexcept {
    edit.draft_ = std::move(prepared.value);
    edit.metadata_.graphemes.swap(prepared.metadata.graphemes);
    edit.metadata_.wordStarts.swap(prepared.metadata.wordStarts);
    edit.metadata_.cells = prepared.metadata.cells;
    edit.masked_.swap(prepared.mask);
    edit.cursor_ = prepared.cursor;
    edit.anchor_ = prepared.cursor;
    edit.dirty_ = dirty;
    edit.revealCursor();
  }

  // A failed decrypt/reprotect can invalidate an already shared draft. Its old mask
  // cannot keep claiming availability, even when an ordinary transaction rolls back.
  void clearUnavailable() {
    (void)tree.edit(node, [&](Password &edit) {
      if (edit.draft_.available()) {
        return;
      }

      edit.metadata_.graphemes.clear();
      edit.metadata_.wordStarts.clear();
      edit.metadata_.cells = 0;
      edit.masked_.reset();
      edit.cursor_ = edit.anchor_ = 0;
      edit.scroll_ = 0;
      (void)tree.invalidate(node, Invalidation::LAYOUT);
    });
  }

  std::expected<void, PasswordFailure> adopt(Password &edit, const PropertyChange<PasswordValue> &snapshot) {
    auto prepared = prepare(snapshot.value, edit.options_, snapshot.value.size());

    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }

    const bool changed =
      edit.draft_ != snapshot.value || edit.cursor_ != prepared->cursor || edit.anchor_ != prepared->cursor;

    if (changed) {
      auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

      if (!invalidated) {
        return std::unexpected(PasswordFailure{invalidated.error()});
      }
    }

    install(edit, *prepared, false);
    revision = snapshot.revision;
    initialized = true;

    return {};
  }

  std::expected<void, PasswordFailure> syncModel(Password &edit) {
    auto snapshot = model->snapshot();

    if (!snapshot) {
      return std::unexpected(PasswordFailure{snapshot.error()});
    }

    if (!edit.dirty_ && (!initialized || revision != (*snapshot)->revision)) {
      return adopt(edit, **snapshot);
    }

    if (!edit.draft_.available()) {
      return std::unexpected(
        PasswordFailure{SecureStringFailure{{SecureStringOperation::BORROW, SecureStringErrorCode::UNAVAILABLE}, {}}});
    }

    return {};
  }

  std::expected<void, PasswordFailure> syncImpl(std::optional<Node<Widget>> focused) {
    auto allowed = alive();

    if (!allowed) {
      return allowed;
    }

    if (busy) {
      return std::unexpected(PasswordFailure{TreeError::WOULD_BLOCK});
    }

    BusyGuard guard(busy);
    std::expected<void, PasswordFailure> result;
    auto edited = tree.edit(node, [&](Password &edit) {
      result = syncModel(edit);

      if (!result || !focused) {
        return;
      }

      auto eligible = tree.inputEligible(node);

      if (!eligible) {
        result = std::unexpected(PasswordFailure{eligible.error()});

        return;
      }

      const bool hasFocus = *eligible && focused->id() == node.id();

      if (edit.focused_ != hasFocus || edit.enabled_ != *eligible) {
        auto invalidated = tree.invalidate(node, Invalidation::PAINT);

        if (!invalidated) {
          result = std::unexpected(PasswordFailure{invalidated.error()});

          return;
        }

        edit.focused_ = hasFocus;
        edit.enabled_ = *eligible;
      }
    });

    if (!edited) {
      return std::unexpected(PasswordFailure{edited.error()});
    }

    if (!result) {
      clearUnavailable();
    }

    return result;
  }

  std::expected<void, PasswordFailure> sync(std::optional<Node<Widget>> focused) {
    try {
      return syncImpl(focused);
    } catch (const std::bad_alloc &) {
      clearUnavailable();

      return std::unexpected(PasswordFailure{TreeError::NO_MEMORY});
    } catch (...) {
      clearUnavailable();

      return std::unexpected(PasswordFailure{TreeError::CALLBACK_FAILED});
    }
  }

  PasswordResult publish(PasswordValue candidate, std::uint64_t baseline, bool final) {
    auto snapshot = model->snapshot();

    if (!snapshot) {
      return failed(snapshot.error());
    }

    if (baseline != (*snapshot)->revision) {
      return {PasswordStatus::CONFLICT};
    }

    bool accepted = true;

    if (validate) {
      auto checked = candidate.withPlaintext([&](SecretView bytes) {
        accepted = validate(bytes);
      });

      if (!checked) {
        return failed(std::move(checked.error()));
      }
    }

    auto live = alive();

    if (!live) {
      return failed(std::move(live.error()));
    }

    if (!accepted) {
      return {PasswordStatus::VALIDATION_FAILED};
    }

    snapshot = model->snapshot();

    if (!snapshot) {
      return failed(snapshot.error());
    }

    if (baseline != (*snapshot)->revision) {
      return {PasswordStatus::CONFLICT};
    }

    if (!final) {
      return {PasswordStatus::EDITED};
    }

    auto updated = model->set(std::move(candidate));

    if (!updated) {
      return failed(updated.error());
    }

    revision = updated->snapshot->revision;

    return {PasswordStatus::COMMITTED, std::move(*updated)};
  }

  PasswordResult accept(Password &edit, Prepared prepared) {
    if (prepared.value == edit.draft_) {
      if (prepared.cursor != edit.cursor_ || edit.anchor_ != prepared.cursor) {
        auto invalidated = tree.invalidate(node, Invalidation::PAINT);

        if (!invalidated) {
          return failed(invalidated.error());
        }

        edit.cursor_ = edit.anchor_ = prepared.cursor;
        edit.revealCursor();
      }

      return {PasswordStatus::EDITED};
    }

    // Everything read from the widget is captured before the validator/property
    // boundary. Either can remove this widget or an ancestor synchronously.
    const auto mode = edit.options_.editMode;
    const auto baseline = revision;
    auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

    if (!invalidated) {
      return failed(invalidated.error());
    }

    auto result = publish(prepared.value, baseline, mode == EditMode::IMMEDIATE);

    if (result.status == PasswordStatus::FAILED) {
      clearUnavailable();

      return result;
    }

    auto installed = tree.edit(node, [&](Password &current) {
      install(current, prepared, result.status != PasswordStatus::COMMITTED);
    });

    if (!installed && installed.error() != TreeError::STALE_NODE && installed.error() != TreeError::CLOSED) {
      result.failure = installed.error();
    }

    return result;
  }

  PasswordResult replace(Password &edit, std::size_t first, std::size_t last, std::string_view insertion) {
    const auto remaining = edit.draft_.size() - (last - first);

    const auto maximum = std::min(edit.options_.limits.max_input_bytes, edit.options_.securePolicy.maxBytes);

    if (remaining > maximum || insertion.size() > maximum - remaining) {
      return {PasswordStatus::LIMIT_EXCEEDED, {}, TextError{TextErrorKind::INPUT_LIMIT, 0}};
    }

    auto candidate = detail::PasswordValueAccess::replace(edit.draft_, first, last, insertion);

    if (!candidate) {
      return failed(std::move(candidate.error()));
    }

    auto prepared = prepare(std::move(*candidate), edit.options_, first + insertion.size());

    if (!prepared) {
      return preparationFailure(std::move(prepared.error()));
    }

    return accept(edit, std::move(*prepared));
  }

  PasswordResult confirm(Password &edit) {
    const auto candidate = edit.draft_;
    const auto baseline = revision;
    auto result = publish(candidate, baseline, true);

    if (result.status == PasswordStatus::COMMITTED) {
      (void)tree.edit(node, [](Password &current) {
        current.dirty_ = false;
      });
    } else if (result.status == PasswordStatus::FAILED) {
      clearUnavailable();
    }

    return result;
  }

  PasswordResult cancel(Password &edit) {
    auto snapshot = model->snapshot();

    if (!snapshot) {
      return failed(snapshot.error());
    }

    auto adopted = adopt(edit, **snapshot);

    return adopted ? PasswordResult{PasswordStatus::CANCELLED} : failed(std::move(adopted.error()));
  }

  std::size_t previous(const Password &edit, std::size_t cursor) const noexcept {
    const auto found = std::ranges::lower_bound(edit.metadata_.graphemes, cursor, {}, &Grapheme::byte_end);

    return cursor == 0 || found == edit.metadata_.graphemes.end() ? cursor : found->byte_begin;
  }

  std::size_t next(const Password &edit, std::size_t cursor) const noexcept {
    const auto found = std::ranges::lower_bound(edit.metadata_.graphemes, cursor, {}, &Grapheme::byte_begin);

    return found == edit.metadata_.graphemes.end() ? cursor : found->byte_end;
  }

  PasswordResult action(Password &edit, const ActionEvent &event) {
    if (event.action == Action::ACTIVATE) {
      return confirm(edit);
    }

    if (event.action == Action::CANCEL) {
      return cancel(edit);
    }

    auto first = std::min(edit.cursor_, edit.anchor_);
    auto last = std::max(edit.cursor_, edit.anchor_);

    if (event.action == Action::ERASE_BACKWARD || event.action == Action::ERASE_FORWARD) {
      if (first == last) {
        if (event.action == Action::ERASE_BACKWARD) {
          first = previous(edit, first);
        } else {
          last = next(edit, last);
        }
      }

      return replace(edit, first, last, {});
    }

    auto cursor = edit.cursor_;

    switch (event.action) {
    case Action::MOVE_LEFT:
      cursor = !event.modifiers.shift && first != last ? first : previous(edit, cursor);

      if (event.modifiers.control && !edit.metadata_.wordStarts.empty()) {
        const auto found = std::ranges::lower_bound(edit.metadata_.wordStarts, edit.cursor_);
        cursor = found == edit.metadata_.wordStarts.begin() ? 0 : *std::prev(found);
      }

      break;
    case Action::MOVE_RIGHT:
      cursor = !event.modifiers.shift && first != last ? last : next(edit, cursor);

      if (event.modifiers.control) {
        const auto found = std::ranges::upper_bound(edit.metadata_.wordStarts, edit.cursor_);
        cursor = found == edit.metadata_.wordStarts.end() ? edit.draft_.size() : *found;
      }

      break;
    case Action::MOVE_HOME:
      cursor = 0;

      break;
    case Action::MOVE_END:
      cursor = edit.draft_.size();

      break;
    default:
      break;
    }

    if (cursor == edit.cursor_ && (event.modifiers.shift || edit.anchor_ == cursor)) {
      return {PasswordStatus::EDITED};
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

    return {PasswordStatus::EDITED};
  }

  PasswordResult paste(Password &edit) {
    if (!clipboard) {
      return {PasswordStatus::CLIPBOARD_FAILED};
    }

    // The service may destroy the node. Capture only public bounds before it runs,
    // then acquire the current widget again; never reuse edit after read().
    const auto maximum = std::min(edit.options_.limits.max_input_bytes, edit.options_.securePolicy.maxBytes);
    auto bytes = clipboard->read(maximum);

    if (!bytes) {
      return {PasswordStatus::CLIPBOARD_FAILED, {}, bytes.error()};
    }

    struct WipePayload {
      std::string &bytes;

      ~WipePayload() {
        wipe({bytes.data(), bytes.size()});
      }
    } cleanup{*bytes};

    PasswordResult result;
    auto edited = tree.edit(node, [&](Password &current) {
      auto synced = syncModel(current);

      if (!synced) {
        result = failed(std::move(synced.error()));

        return;
      }

      if (current.options_.readOnly || current.options_.pastePolicy != PasswordPastePolicy::ENABLED) {
        return;
      }

      result = replace(current, std::min(current.cursor_, current.anchor_), std::max(current.cursor_, current.anchor_),
                       *bytes);
    });

    return edited ? result : failed(edited.error());
  }

  std::optional<PasswordResult> compute(Password &edit, const RoutedEvent &event) {
    const auto *command = std::get_if<ActionEvent>(&event);
    const auto *text = std::get_if<TextInputEvent>(&event);
    const auto *pasted = std::get_if<PasteInputEvent>(&event);
    const auto key = shortcut(std::get_if<KeyEvent>(&event));
    const bool textAccepted = text && !text->modifiers.alt && !text->modifiers.control && !text->modifiers.super;

    if (!accepts(command) && !textAccepted && !pasted && !key) {
      return {};
    }

    if (key == U'c' || key == U'x' ||
        ((key == U'v' || pasted) && edit.options_.pastePolicy == PasswordPastePolicy::DISABLED)) {
      return PasswordResult{};
    }

    const bool erases =
      command && (command->action == Action::ERASE_BACKWARD || command->action == Action::ERASE_FORWARD);

    if (edit.options_.readOnly && (textAccepted || pasted || erases || key == U'v')) {
      return PasswordResult{};
    }

    // Escape is the recovery path even when the previous protected identity has
    // become unavailable. It checks the latest model independently below.
    if (command && command->action == Action::CANCEL) {
      return cancel(edit);
    }

    auto synced = syncModel(edit);

    if (!synced) {
      return failed(std::move(synced.error()));
    }

    if (accepts(command)) {
      return action(edit, *command);
    }

    if (key == U'v') {
      return paste(edit);
    }

    if (key == U'a') {
      if (edit.anchor_ != 0 || edit.cursor_ != edit.draft_.size()) {
        auto invalidated = tree.invalidate(node, Invalidation::PAINT);

        if (!invalidated) {
          return failed(invalidated.error());
        }

        edit.anchor_ = 0;
        edit.cursor_ = edit.draft_.size();
        edit.revealCursor();
      }

      return PasswordResult{};
    }

    if (pasted) {
      return replace(edit, std::min(edit.cursor_, edit.anchor_), std::max(edit.cursor_, edit.anchor_), pasted->bytes);
    }

    EncodedScalar encoded;
    encode(text->scalar, encoded);

    return replace(edit, std::min(edit.cursor_, edit.anchor_), std::max(edit.cursor_, edit.anchor_), encoded.view());
  }

  void deliver(PasswordResult &result) {
    try {
      onResult(result);
    } catch (const std::bad_alloc &) {
      result.callbackError = TreeError::NO_MEMORY;
    } catch (...) {
      result.callbackError = TreeError::CALLBACK_FAILED;
    }
  }

  EventReply handle(Password &edit, const RoutedEvent &event) {
    if (busy) {
      return EventReply::HANDLED;
    }

    BusyGuard guard(busy);
    std::optional<PasswordResult> result;

    try {
      result = compute(edit, event);
    } catch (const std::bad_alloc &) {
      result = failed(TreeError::NO_MEMORY);
    } catch (...) {
      result = failed(TreeError::CALLBACK_FAILED);
    }

    if (!result) {
      return EventReply::IGNORED;
    }

    clearUnavailable();
    deliver(*result);

    if (result->callbackError) {
      throw *result->callbackError;
    }

    return EventReply::HANDLED;
  }

  std::expected<void, TreeError> focus(bool focused) {
    if (busy) {
      return tree.edit(node, [&](Password &edit) {
        edit.focused_ = focused;
      });
    }

    BusyGuard guard(busy);
    std::optional<PasswordResult> result;
    auto edited = tree.edit(node, [&](Password &edit) {
      edit.focused_ = focused;

      if (!focused && edit.dirty_ && edit.options_.editMode == EditMode::ON_COMPLETION) {
        result = confirm(edit);
      }
    });

    if (!edited) {
      return edited;
    }

    if (result) {
      result->trigger = LineEditTrigger::BLUR;
      clearUnavailable();
      deliver(*result);

      if (result->callbackError) {
        return std::unexpected(*result->callbackError);
      }

      if (result->failure) {
        if (const auto *error = std::get_if<TreeError>(&*result->failure)) {
          return std::unexpected(*error);
        }

        return std::unexpected(TreeError::CALLBACK_FAILED);
      }
    }

    return {};
  }

  enum class Command {
    CONFIRM,
    CANCEL,
    SET_VALUE
  };

  std::expected<PasswordResult, PasswordFailure> command(Command command, PasswordValue value = {}) {
    auto allowed = alive();

    if (!allowed) {
      return std::unexpected(std::move(allowed.error()));
    }

    if (busy) {
      return std::unexpected(PasswordFailure{TreeError::WOULD_BLOCK});
    }

    BusyGuard guard(busy);
    PasswordResult result;

    try {
      auto edited = tree.edit(node, [&](Password &edit) {
        if (command == Command::CANCEL) {
          result = cancel(edit);

          return;
        }

        auto synced = syncModel(edit);

        if (!synced) {
          result = failed(std::move(synced.error()));

          return;
        }

        if (command == Command::CONFIRM) {
          result = confirm(edit);
        } else {
          if (value == edit.draft_) {
            return;
          }

          auto prepared = prepare(value, edit.options_, value.size());
          result = prepared ? accept(edit, std::move(*prepared)) : preparationFailure(std::move(prepared.error()));
        }
      });

      if (!edited) {
        result = failed(edited.error());
      }
    } catch (const std::bad_alloc &) {
      result = failed(TreeError::NO_MEMORY);
    } catch (...) {
      result = failed(TreeError::CALLBACK_FAILED);
    }

    result.trigger = LineEditTrigger::FLUENT;
    clearUnavailable();
    deliver(result);

    if (result.status == PasswordStatus::FAILED && result.failure && !result.callbackError) {
      return std::unexpected(std::move(*result.failure));
    }

    return result;
  }
};

PasswordBinding::PasswordBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription)
    : state_(std::move(state)), action_(std::move(action)), subscription_(std::move(subscription)) {
}

PasswordBinding::~PasswordBinding() = default;
PasswordBinding::PasswordBinding(PasswordBinding &&) noexcept = default;

PasswordBinding &PasswordBinding::operator=(PasswordBinding &&other) noexcept {
  if (this != &other) {
    PasswordBinding retired(std::move(*this));

    state_ = std::move(other.state_);
    action_ = std::move(other.action_);
    subscription_ = std::move(other.subscription_);
  }

  return *this;
}

std::expected<void, PasswordFailure> PasswordBinding::sync(Node<Widget> focused) {
  auto state = state_;

  if (!state) {
    return std::unexpected(PasswordFailure{TreeError::INVALID_ARGUMENT});
  }

  try {
    return state->sync(focused);
  } catch (const std::bad_alloc &) {
    state->clearUnavailable();

    return std::unexpected(PasswordFailure{TreeError::NO_MEMORY});
  } catch (...) {
    state->clearUnavailable();

    return std::unexpected(PasswordFailure{TreeError::CALLBACK_FAILED});
  }
}

std::expected<PasswordResult, PasswordFailure> PasswordBinding::confirm() {
  auto state = state_;

  return state ? state->command(State::Command::CONFIRM)
               : std::unexpected(PasswordFailure{TreeError::INVALID_ARGUMENT});
}

std::expected<PasswordResult, PasswordFailure> PasswordBinding::cancel() {
  auto state = state_;

  return state ? state->command(State::Command::CANCEL) : std::unexpected(PasswordFailure{TreeError::INVALID_ARGUMENT});
}

std::expected<PasswordResult, PasswordFailure> PasswordBinding::setValue(PasswordValue value) {
  auto state = state_;

  return state ? state->command(State::Command::SET_VALUE, std::move(value))
               : std::unexpected(PasswordFailure{TreeError::INVALID_ARGUMENT});
}

std::expected<PasswordBinding, PasswordFailure> bindPassword(WidgetTree &tree, EventRouter &router, Node<Password> node,
                                                             std::shared_ptr<Property<PasswordValue>> model,
                                                             std::function<void(PasswordResult)> onResult,
                                                             std::shared_ptr<Clipboard> clipboard,
                                                             std::function<bool(SecretView)> validate) {
  if (!model || !onResult) {
    return std::unexpected(PasswordFailure{TreeError::INVALID_ARGUMENT});
  }

  try {
    auto state = std::make_shared<PasswordBinding::State>(PasswordBinding::State{
      tree, node, std::move(model), std::move(clipboard), std::move(validate), std::move(onResult)});
    auto subscription = state->model->observe(node, [state](Password &, auto) {
      auto synced = state->sync({});

      if (!synced) {
        auto failure = failed(std::move(synced.error()));

        state->deliver(failure);

        if (failure.callbackError) {
          throw *failure.callbackError;
        }
      }
    });

    if (!subscription) {
      return std::unexpected(PasswordFailure{subscription.error()});
    }

    auto action = router.bind(
      node,
      [state](Password &edit, const RoutedEvent &event) {
        return state->handle(edit, event);
      },
      [state](Password &, bool focused) {
        return state->focus(focused);
      });

    if (!action) {
      return std::unexpected(PasswordFailure{action.error()});
    }

    auto synced = state->sync({});

    if (!synced) {
      return std::unexpected(std::move(synced.error()));
    }

    return PasswordBinding(std::move(state), std::move(*action), std::move(*subscription));
  } catch (const std::bad_alloc &) {
    return std::unexpected(PasswordFailure{TreeError::NO_MEMORY});
  } catch (...) {
    return std::unexpected(PasswordFailure{TreeError::CALLBACK_FAILED});
  }
}
} // namespace portals
