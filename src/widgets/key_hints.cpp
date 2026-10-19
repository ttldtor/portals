// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_help_state.hpp"
#include <algorithm>
#include <format>

namespace portals {
namespace {
ManagedError projectionError(TreeError cause) {
  return {ManagedPhase::PREPARE, {}, cause, {}, {}};
}

const GraphemeText *caption(const detail::KeyHelpPresentation &value) {
  return value.current && value.text && value.text->caption ? &*value.text->caption : &value.unavailable;
}

std::string chordText(key::Chord chord) {
  std::string result(0, '\0');
  if (chord.modifiers.control) {
    result += "Ctrl+";
  }
  if (chord.modifiers.shift) {
    result += "Shift+";
  }
  if (chord.modifiers.alt) {
    result += "Alt+";
  }
  if (chord.modifiers.super) {
    result += "Super+";
  }
  if (chord.key == InputKey::CHARACTER) {
    auto scalar = chord.scalar;
    if (scalar >= U'a' && scalar <= U'z') {
      scalar -= U'a' - U'A';
    }
    if (scalar < 0x80) {
      result += static_cast<char>(scalar);
    } else if (scalar < 0x800) {
      result += char(0xc0 | (scalar >> 6));
      result += char(0x80 | (scalar & 63));
    } else if (scalar < 0x10000) {
      result += char(0xe0 | (scalar >> 12));
      result += char(0x80 | ((scalar >> 6) & 63));
      result += char(0x80 | (scalar & 63));
    } else {
      result += char(0xf0 | (scalar >> 18));
      result += char(0x80 | ((scalar >> 12) & 63));
      result += char(0x80 | ((scalar >> 6) & 63));
      result += char(0x80 | (scalar & 63));
    }
  } else if (chord.key >= InputKey::F1 && chord.key <= InputKey::F12) {
    result += std::format("F{}", static_cast<int>(chord.key) - static_cast<int>(InputKey::F1) + 1);
  } else {
    switch (chord.key) {
    case InputKey::ESCAPE:
      result += "Escape";
      break;
    case InputKey::ENTER:
      result += "Enter";
      break;
    case InputKey::TAB:
      result += "Tab";
      break;
    case InputKey::BACKSPACE:
      result += "Backspace";
      break;
    case InputKey::UP:
      result += "Up";
      break;
    case InputKey::DOWN:
      result += "Down";
      break;
    case InputKey::LEFT:
      result += "Left";
      break;
    case InputKey::RIGHT:
      result += "Right";
      break;
    case InputKey::HOME:
      result += "Home";
      break;
    case InputKey::END:
      result += "End";
      break;
    case InputKey::INSERT:
      result += "Insert";
      break;
    case InputKey::DELETE_KEY:
      result += "Delete";
      break;
    case InputKey::PAGE_UP:
      result += "PageUp";
      break;
    case InputKey::PAGE_DOWN:
      result += "PageDown";
      break;
    default:
      result += "?";
      break;
    }
  }
  return result;
}

std::expected<GraphemeText, ManagedError> formatRow(const KeyBindingView &row, const KeyBindingsSnapshot &snapshot,
                                                    const KeyFormatOptions &options, bool full, WidthPolicy policy) {
  if (!options.maxBytesPerBinding) {
    return std::unexpected(projectionError(TreeError::INVALID_ARGUMENT));
  }
  std::string bytes(0, '\0');
  bool limit{};
  const auto append = [&](std::string_view value) {
    if (value.size() > options.maxBytesPerBinding - bytes.size()) {
      limit = true;
      return;
    }
    bytes += value;
  };
  if (row.legacy) {
    append(chordText({row.legacy->key, row.legacy->scalar, row.legacy->modifiers}));
  } else if (row.shortcut) {
    const auto count = row.shortcut->alternatives.size() + row.shortcut->sequences.size();
    for (std::size_t alternative{}; alternative < count; ++alternative) {
      if (alternative) {
        append(options.alternativeSeparator);
      }
      const auto strokes = detail::shortcutAlternative(*row.shortcut, alternative);
      for (std::size_t stroke{}; stroke < strokes.size(); ++stroke) {
        if (stroke) {
          append(options.strokeSeparator);
        }
        append(chordText(strokes[stroke]));
      }
    }
  } else {
    append("unbound");
  }
  bool unknown{};
  for (auto phase : row.actions) {
    if (phase == KeyAction::REPEAT) {
      append(options.repeatMarker);
    }
    if (phase == KeyAction::RELEASE) {
      append(options.releaseMarker);
    }
    const auto support = phase == KeyAction::PRESS    ? snapshot.capabilities.press
                         : phase == KeyAction::REPEAT ? snapshot.capabilities.repeat
                                                      : snapshot.capabilities.release;
    unknown = unknown || support == KeySupport::UNKNOWN;
    if (support == KeySupport::UNSUPPORTED) {
      append(" [unsupported phase]");
    }
  }
  if (unknown) {
    append(options.unknownMarker);
  }
  if (!row.label.bytes().empty()) {
    append(": ");
    append(row.label.bytes());
  }
  if (full) {
    if (!row.description.bytes().empty()) {
      append(" - ");
      append(row.description.bytes());
    }
    if (row.scope.session) {
      append(" [scoped]");
    }
    switch (row.eligibility) {
    case KeyEligibility::DISABLED:
      append(" [disabled]");
      break;
    case KeyEligibility::UNBOUND:
      append(" [unbound]");
      break;
    case KeyEligibility::OWNER_INELIGIBLE:
      append(" [owner unavailable]");
      break;
    case KeyEligibility::SCOPE_INACTIVE:
      append(" [scope inactive]");
      break;
    case KeyEligibility::TERMINAL_UNSUPPORTED:
      append(" [unsupported]");
      break;
    default:
      break;
    }
    for (const auto &conflict : row.conflicts) {
      const auto peer = std::ranges::find(snapshot.bindings, conflict.otherOrdinal, &KeyBindingView::ordinal);
      if (peer == snapshot.bindings.end()) {
        continue;
      }
      append(" [overlap: ");
      if (!peer->label.bytes().empty()) {
        append(peer->label.bytes());
      } else {
        auto key = formatRow(*peer, snapshot, options, false, policy);
        if (!key) {
          return std::unexpected(key.error());
        }
        append(key->bytes());
      }
      append(conflict.kind == KeyConflictKind::EXACT             ? " (exact)]"
             : conflict.kind == KeyConflictKind::TERMINAL_PREFIX ? " (prefix)]"
                                                                 : " (shared prefix)]");
      if (limit) {
        return std::unexpected(projectionError(TreeError::NODE_LIMIT));
      }
    }
  }
  if (limit) {
    return std::unexpected(projectionError(TreeError::NODE_LIMIT));
  }
  auto text = GraphemeText::from(bytes, policy);
  if (!text) {
    auto failure = projectionError(TreeError::INVALID_ARGUMENT);
    failure.text = text.error();
    return std::unexpected(failure);
  }
  return std::move(*text);
}
} // namespace

std::expected<CellSize, TreeError> KeyHints::measure(CellSize available) {
  if (!presentation_) {
    return Button::measure(available);
  }
  return CellSize{std::max(0, available.width), available.height > 0 ? 1 : 0};
}

std::expected<void, TreeError> KeyHints::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());
  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  bounds_ = *bounds;
  return Control::arrange(local);
}

std::expected<void, TreeError> KeyHints::paint(PaintContext &context) {
  if (!presentation_) {
    return Control::paint(context);
  }
  if (bounds_.empty()) {
    return {};
  }
  const auto style = computedStyle() ? (focused() ? computedStyle()->focused : computedStyle()->normal) : CellStyle{};
  const RenderCommand command = TextCommand{{}, *caption(*presentation_), bounds_, style};
  return context.submit(std::span(&command, 1));
}

namespace detail {
std::expected<CellSize, TreeError> HelpBody::measure(CellSize) {
  if (!presentation_->current || !presentation_->text) {
    return CellSize{presentation_->unavailable.measure(), 1};
  }
  std::int32_t width{};
  for (const auto &line : presentation_->text->lines) {
    width = std::max(width, line->measure());
  }
  return CellSize{width, static_cast<std::int32_t>(presentation_->text->lines.size())};
}

std::expected<void, TreeError> HelpBody::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());
  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  bounds_ = *bounds;
  return {};
}

std::expected<void, TreeError> HelpBody::paint(PaintContext &context) {
  if (bounds_.empty()) {
    return {};
  }
  const auto style = computedStyle() ? computedStyle()->normal : CellStyle{};
  if (!presentation_->current || !presentation_->text) {
    const RenderCommand command = TextCommand{{}, presentation_->unavailable, bounds_, style};
    return context.submit(std::span(&command, 1));
  }
  for (std::size_t i{}; i < presentation_->text->lines.size(); ++i) {
    const RenderCommand command =
      TextCommand{{0, static_cast<std::int32_t>(i)}, *presentation_->text->lines[i], bounds_, style};
    auto submitted = context.submit(std::span(&command, 1));
    if (!submitted) {
      return submitted;
    }
  }
  return {};
}
} // namespace detail

std::expected<void, ManagedError>
ManagedScene::checkKeyProjectionBudget(const detail::KeyHelpRecord *replacement,
                                       const std::shared_ptr<const KeyBindingsSnapshot> &projection,
                                       std::size_t projectionBytes, std::size_t textBytes) {
  std::size_t used{};
  const auto charge = [&](std::size_t count) {
    if (count > options_.maxKeyProjectionBytes - used) {
      return false;
    }
    used += count;
    return true;
  };
  if (!charge(textBytes)) {
    return std::unexpected(projectionError(TreeError::NODE_LIMIT));
  }
  bool differentOwner{};
  for (const auto &entry : entries_) {
    const auto &other = entry->keyHelp;
    if (!other || other.get() == replacement) {
      continue;
    }
    if (!charge(other->textBytes)) {
      return std::unexpected(projectionError(TreeError::NODE_LIMIT));
    }
    differentOwner =
      differentOwner || (other->presentation->projection && other->presentation->projection != projection);
  }
  if (!differentOwner) {
    return charge(projectionBytes) ? std::expected<void, ManagedError>{}
                                   : std::unexpected(projectionError(TreeError::NODE_LIMIT));
  }
  // Last checked DTOs may outlive the common cache. Count each shared owner once, without formatting hidden views.
  std::vector<std::pair<const KeyBindingsSnapshot *, std::size_t>> retained(0);
  retained.emplace_back(projection.get(), projectionBytes);
  for (const auto &entry : entries_) {
    const auto &other = entry->keyHelp;
    if (other && other.get() != replacement && other->presentation->projection &&
        other->presentation->projection != projection) {
      retained.emplace_back(other->presentation->projection.get(), other->projectionBytes);
    }
  }
  std::ranges::sort(retained, [](const auto &a, const auto &b) {
    return std::less<const KeyBindingsSnapshot *>{}(a.first, b.first);
  });
  const KeyBindingsSnapshot *previous{};
  for (const auto &[owner, bytes] : retained) {
    if (owner != previous && !charge(bytes)) {
      return std::unexpected(projectionError(TreeError::NODE_LIMIT));
    }
    previous = owner;
  }
  return {};
}

std::expected<void, TreeError> ManagedScene::prepareKeyHelp(const std::shared_ptr<detail::KeyHelpRecord> &record,
                                                            const LayoutSnapshot &layout) {
  if (record->full && record->phase == detail::KeyHelpRecord::Phase::OPEN &&
      helpActivation(record->root) != record->activation) {
    record->phase = detail::KeyHelpRecord::Phase::CLOSED;
    if (!helpActivation(record->root)) {
      (void)app_.tree().setVisible(record->root, false);
    }
  }
  auto viewport = layout.viewport(record->root);
  if (!viewport || viewport->width <= 0 || viewport->height <= 0) {
    if (record->hints) {
      (void)app_.tree().setEnabled(record->root, false);
    }
    return {};
  }
  auto observed = observeKeyScopes();
  if (!observed) {
    // Scope observation has only allocation-free TreeError failures.
    return std::unexpected(std::get<TreeError>(observed.error().cause));
  }
  const std::array<std::uint64_t, 4> input{keyRevision_, app_.router().focusGeneration(), app_.tree().inputGeneration(),
                                           keyScopeGeneration_};
  const std::array<std::uint64_t, 6> stamp{input[0],
                                           input[1],
                                           input[2],
                                           input[3],
                                           static_cast<std::uint64_t>(viewport->width),
                                           static_cast<std::uint64_t>(viewport->height)};
  if (record->stamp == stamp && record->presentation->current) {
    return {};
  }
  if (record->stamp[4] && (record->stamp[4] != stamp[4] || record->stamp[5] != stamp[5])) {
    failedKeyProjection_ = {};
  }
  record->stamp = stamp;
  const auto failed = [&](ManagedError failure) {
    record->presentation->current = false;
    if (record->hints) {
      (void)app_.tree().setEnabled(record->root, false);
    }
    if (failedKeyProjection_ != input) {
      failedKeyProjection_ = input;
      (void)recordError(failure, {}, failure.operationOrigin.value_or(std::source_location::current()));
    }
    (void)app_.tree().invalidate(record->root, Invalidation::PAINT);
    return std::expected<void, TreeError>{};
  };
  if (failedKeyProjection_ == input) {
    record->presentation->current = false;
    if (record->hints) {
      (void)app_.tree().setEnabled(record->root, false);
    }
    return {};
  }
  auto projection = readKeyProjection(false);
  if (!projection) {
    return failed(projection.error());
  }
  try {
    auto text = std::make_shared<detail::KeyHelpText>();
    if (record->full) {
      for (const auto &row : (*projection)->bindings) {
        auto line = formatRow(row, **projection, record->full->format, true, widthPolicy());
        if (!line) {
          return failed(line.error());
        }
        std::size_t begin{};
        const auto bytes = line->bytes();
        while (begin <= bytes.size()) {
          const auto end = bytes.find_first_of("\r\n", begin);
          auto part = GraphemeText::from(
            bytes.substr(begin, end == std::string_view::npos ? bytes.size() - begin : end - begin), widthPolicy());
          if (!part) {
            auto failure = projectionError(TreeError::INVALID_ARGUMENT);
            failure.text = part.error();
            return failed(failure);
          }
          text->lines.push_back(std::move(*part));
          if (end == std::string_view::npos) {
            break;
          }
          begin = end + 1;
          if (bytes[end] == '\r' && begin < bytes.size() && bytes[begin] == '\n') {
            ++begin;
          }
        }
      }
      if (text->lines.empty()) {
        auto empty = GraphemeText::from(record->full->emptyText, widthPolicy());
        if (!empty) {
          auto error = projectionError(TreeError::INVALID_ARGUMENT);
          error.text = empty.error();
          return failed(error);
        }
        text->lines.push_back(std::move(*empty));
      }
    } else {
      std::vector<std::optional<GraphemeText>> entries(0);
      for (auto it = (*projection)->bindings.rbegin(); it != (*projection)->bindings.rend(); ++it) {
        if (!it->id || it->eligibility != KeyEligibility::ELIGIBLE) {
          continue;
        }
        auto entry = formatRow(*it, **projection, record->hints->format, false, widthPolicy());
        if (!entry) {
          return failed(entry.error());
        }
        entries.push_back(std::move(*entry));
      }
      std::string bytes(0, '\0');
      std::size_t shown{};
      for (const auto &entry : entries) {
        std::string candidate = bytes;
        if (shown) {
          if (record->hints->entrySeparator.size() > options_.maxKeyProjectionBytes - candidate.size()) {
            return failed(projectionError(TreeError::NODE_LIMIT));
          }
          candidate += record->hints->entrySeparator;
        }
        if (entry->bytes().size() > options_.maxKeyProjectionBytes - candidate.size()) {
          return failed(projectionError(TreeError::NODE_LIMIT));
        }
        candidate += entry->bytes();
        const auto remaining = entries.size() - shown - 1;
        if (record->hints->overflowMarker.size() > options_.maxKeyProjectionBytes - candidate.size()) {
          return failed(projectionError(TreeError::NODE_LIMIT));
        }
        const auto marker =
          remaining ? record->hints->overflowMarker + std::to_string(remaining) : std::string(0, '\0');
        auto measured = GraphemeText::from(candidate + marker, widthPolicy());
        if (!measured) {
          auto error = projectionError(TreeError::INVALID_ARGUMENT);
          error.text = measured.error();
          return failed(error);
        }
        if (measured->measure() > viewport->width) {
          break;
        }
        bytes.swap(candidate);
        ++shown;
      }
      text->overflow = entries.size() - shown;
      if (text->overflow) {
        auto marker = GraphemeText::from(record->hints->overflowMarker + std::to_string(text->overflow), widthPolicy());
        if (!marker) {
          auto error = projectionError(TreeError::INVALID_ARGUMENT);
          error.text = marker.error();
          return failed(error);
        }
        bytes += marker->bytes().substr(0, marker->clipPrefix(viewport->width).byte_end);
      } else if (entries.empty()) {
        bytes = record->hints->emptyText;
      }
      auto prepared = GraphemeText::from(bytes, widthPolicy());
      if (!prepared) {
        auto error = projectionError(TreeError::INVALID_ARGUMENT);
        error.text = prepared.error();
        return failed(error);
      }
      text->caption = std::move(*prepared);
    }
    std::size_t textBytes{};
    if (text->caption) {
      textBytes = text->caption->bytes().size();
    }
    for (const auto &line : text->lines) {
      if (line->bytes().size() > options_.maxKeyProjectionBytes - std::min(textBytes, options_.maxKeyProjectionBytes)) {
        return failed(projectionError(TreeError::NODE_LIMIT));
      }
      textBytes += line->bytes().size();
    }
    auto budget = checkKeyProjectionBudget(record.get(), *projection, keyProjectionBytes_, textBytes);
    if (!budget) {
      return failed(budget.error());
    }
    // Formatting can allocate. Publish the DTO and its text only while their original input remains current.
    if (!app_.tree().inputParent(record->root)) {
      return failed(projectionError(TreeError::STALE_NODE));
    }
    auto currentScopes = observeKeyScopes();
    if (!currentScopes) {
      return failed(currentScopes.error());
    }
    if (closed_ || keyProjection_ != *projection || keyRevision_ != input[0] ||
        app_.router().focusGeneration() != input[1] || app_.tree().inputGeneration() != input[2] ||
        keyScopeGeneration_ != input[3]) {
      return failed(projectionError(closed_ ? TreeError::CLOSED : TreeError::WOULD_BLOCK));
    }
    if (record->hints) {
      auto enabled = app_.tree().setEnabled(record->root, text->overflow != 0 && !text->caption->bytes().empty());
      if (!enabled) {
        return failed(projectionError(enabled.error()));
      }
    }
    record->textBytes = textBytes;
    record->projectionBytes = keyProjectionBytes_;
    record->presentation->projection = *projection;
    record->presentation->text = text;
    record->presentation->current = true;
    (void)app_.tree().invalidate(record->root, Invalidation::LAYOUT);
    return {};
  } catch (const std::bad_alloc &) {
    return failed(projectionError(TreeError::NO_MEMORY));
  } catch (...) {
    return failed(projectionError(TreeError::CALLBACK_FAILED));
  }
}
} // namespace portals
