// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/menu.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace portals {
struct Menu::Prepared {
  struct Row {
    std::size_t index{};
    std::unique_ptr<GraphemeText> text;
  };

  std::vector<std::shared_ptr<const ui::ActionMetadata>> metadata =
    std::vector<std::shared_ptr<const ui::ActionMetadata>>(0);
  std::vector<Row> rows = std::vector<Row>(0);
  std::int32_t width{};
  bool dirty{};
};

Menu::Menu(GraphemeText empty, const std::vector<ui::ActionRef> &actions, MenuOptions options, ControlStyle style,
           SizePolicy policy)
    : Control(std::move(empty), "", "", style, policy), actions_(actions), options_(options) {
  // Buffered input may precede the first presentation cycle; command admission needs no layout or text formatting.
  selected_ = next({}, true);
}

Menu::~Menu() = default;

std::optional<std::size_t> Menu::selectedIndex() const noexcept {
  return selected_;
}

std::size_t Menu::visibleRowCount() const noexcept {
  return prepared_ ? prepared_->rows.size() : 0;
}

std::expected<CellSize, TreeError> Menu::measure(CellSize) {
  if (options_.maxEntries == 0 ||
      options_.maxEntries > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (actions_.size() > options_.maxEntries) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  return CellSize{prepared_ ? prepared_->width : 0, static_cast<std::int32_t>(visibleRowCount())};
}

std::expected<void, TreeError> Menu::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;

  return {};
}

std::expected<void, TreeError> Menu::paint(PaintContext &context) {
  if (bounds_.empty() || !prepared_) {
    return {};
  }

  std::optional<ComputedStyle> manualStyle;
  if (!computedStyle()) {
    manualStyle.emplace(styleDefaults());
  }
  const auto resolved =
    computedStyle() ? detail::ComputedStyleView(*computedStyle()) : detail::ComputedStyleView(*manualStyle);
  const auto background = enabled() ? resolved.normal : resolved.disabled;
  const RenderCommand fill = FillCommand{bounds_, bounds_, background};
  auto submitted = context.submit(std::span(&fill, 1));

  if (!submitted) {
    return submitted;
  }

  const auto count = std::min(prepared_->rows.size(), static_cast<std::size_t>(bounds_.size().height));

  for (std::size_t row = 0; row < count; ++row) {
    const auto &entry = prepared_->rows[row];
    const auto &metadata = *prepared_->metadata[entry.index];
    const auto style = !enabled() || !metadata.enabled         ? resolved.disabled
                       : focused() && selected_ == entry.index ? resolved.focused
                                                               : resolved.normal;
    const auto rectangle = *CellRect::from({0, static_cast<std::int32_t>(row)}, {bounds_.size().width, 1});
    const std::array<RenderCommand, 2> commands{
      FillCommand{rectangle, bounds_, style},
      TextCommand{{0, static_cast<std::int32_t>(row)}, *entry.text, bounds_, style}};

    submitted = context.submit(commands);

    if (!submitted) {
      return submitted;
    }
  }

  return {};
}

bool Menu::eligible(std::size_t index) const {
  auto metadata = actions_[index].snapshot();

  return metadata && (*metadata)->enabled && (*metadata)->visible;
}

void Menu::invalidatePresentation() noexcept {
  if (prepared_) {
    prepared_->dirty = true;
  }
}

std::optional<std::size_t> Menu::next(std::optional<std::size_t> current, bool forward) const {
  if (actions_.empty()) {
    return {};
  }

  auto index = current.value_or(forward ? actions_.size() - 1 : 0);

  for (std::size_t visited = 0; visited < actions_.size(); ++visited) {
    index = forward ? (index + 1) % actions_.size() : (index ? index - 1 : actions_.size() - 1);

    if (eligible(index)) {
      return index;
    }
  }

  return {};
}

std::expected<void, TreeError> Menu::prepare(WidgetTree &tree, Node<Menu> node, WidthPolicy policy) {
  try {
    if (prepared_ && !prepared_->dirty) {
      return {};
    }

    if (prepared_) {
      bool changed{};

      for (std::size_t index = 0; index < actions_.size(); ++index) {
        auto metadata = actions_[index].snapshot();

        if (!metadata && metadata.error() != TreeError::CLOSED && metadata.error() != TreeError::STALE_NODE) {
          return std::unexpected(metadata.error());
        }

        if ((metadata ? *metadata : nullptr) != prepared_->metadata[index]) {
          changed = true;
          break;
        }
      }

      if (!changed) {
        prepared_->dirty = false;
        return {};
      }
    }

    auto incoming = std::make_unique<Prepared>();
    bool changed = !prepared_;
    bool visual = !prepared_;

    incoming->metadata.reserve(actions_.size());

    for (std::size_t index = 0; index < actions_.size(); ++index) {
      auto snapshot = actions_[index].snapshot();

      if (!snapshot && snapshot.error() != TreeError::CLOSED && snapshot.error() != TreeError::STALE_NODE) {
        return std::unexpected(snapshot.error());
      }

      const auto metadata = snapshot ? *snapshot : nullptr;
      const auto previous = prepared_ ? prepared_->metadata[index] : nullptr;

      changed = changed || metadata != previous;
      visual = visual || static_cast<bool>(metadata) != static_cast<bool>(previous);

      if (metadata && previous) {
        visual = visual || metadata->caption != previous->caption || metadata->shortcut != previous->shortcut ||
                 metadata->enabled != previous->enabled || metadata->visible != previous->visible ||
                 metadata->checked != previous->checked;
      }

      incoming->metadata.push_back(metadata);
    }

    if (!changed) {
      return {};
    }

    if (!visual) {
      prepared_->metadata.swap(incoming->metadata);
      prepared_->dirty = false;

      return {};
    }

    for (std::size_t index = 0; index < incoming->metadata.size(); ++index) {
      const auto &metadata = incoming->metadata[index];

      if (!metadata || !metadata->visible) {
        continue;
      }

      std::string bytes(metadata->checked ? "[x] " : "    ");

      bytes.append(metadata->caption);

      if (!metadata->shortcut.empty()) {
        bytes.append("  ");
        bytes.append(metadata->shortcut);
      }

      auto text = GraphemeText::from(bytes, policy);

      if (!text) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      incoming->width = std::max(incoming->width, text->measure());
      incoming->rows.push_back({index, std::make_unique<GraphemeText>(*text)});
    }

    const auto selected = selected_ && eligible(*selected_) ? selected_ : next(selected_, true);
    auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

    if (!invalidated) {
      return invalidated;
    }

    prepared_.swap(incoming);
    selected_ = selected;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<Menu::Input, TreeError> Menu::input(WidgetTree &tree, Node<Menu> node, const RoutedEvent &event,
                                                  const NodeGeometry &geometry) {
  Input result;
  auto selected = selected_;
  bool activate{};
  std::optional<Action> command;

  if (const auto *action = std::get_if<ActionEvent>(&event)) {
    if (action->action == Action::ACTIVATE && action->source == ClickSource::KEYBOARD &&
        action->keyAction != KeyAction::PRESS) {
      return result;
    }

    command = action->action;
    result.invocation = {action->source, action->modifiers};
  } else if (const auto *key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::RELEASE) {
      return result;
    }

    result.invocation = {ClickSource::KEYBOARD, key->modifiers};

    switch (key->key) {
    case InputKey::UP:
      command = Action::MOVE_UP;
      break;
    case InputKey::DOWN:
      command = Action::MOVE_DOWN;
      break;
    case InputKey::HOME:
      command = Action::MOVE_HOME;
      break;
    case InputKey::END:
      command = Action::MOVE_END;
      break;
    case InputKey::ENTER:
      if (key->action == KeyAction::PRESS) {
        command = Action::ACTIVATE;
      }
      break;
    case InputKey::CHARACTER:
      if (key->scalar == U' ' && key->action == KeyAction::PRESS) {
        command = Action::ACTIVATE;
      }
      break;
    default:
      break;
    }
  } else if (const auto *text = std::get_if<TextInputEvent>(&event); text && text->scalar == U' ') {
    command = Action::ACTIVATE;
    result.invocation = {ClickSource::KEYBOARD, text->modifiers};
  } else if (const auto *mouse = std::get_if<MouseInputEvent>(&event);
             mouse && mouse->button == MouseButton::LEFT && mouse->action == MouseAction::PRESS && prepared_ &&
             geometry.valid && geometry.clip.contains({mouse->column, mouse->row})) {
    const auto row = mouse->row - geometry.surface.origin().y;

    if (row < 0 || static_cast<std::size_t>(row) >= prepared_->rows.size()) {
      return result;
    }

    const auto index = prepared_->rows[static_cast<std::size_t>(row)].index;

    if (!eligible(index)) {
      return result;
    }

    selected = index;
    activate = true;
    result.handled = true;
    result.invocation = {ClickSource::MOUSE, mouse->modifiers};
  }

  if (command) {
    switch (*command) {
    case Action::MOVE_UP:
      selected = next(selected_, false);
      break;
    case Action::MOVE_DOWN:
      selected = next(selected_, true);
      break;
    case Action::MOVE_HOME:
      selected = next({}, true);
      break;
    case Action::MOVE_END:
      selected = next({}, false);
      break;
    case Action::ACTIVATE:
      activate = true;
      break;
    default:
      return result;
    }

    result.handled = true;
  }

  if (selected != selected_) {
    auto invalidated = tree.invalidate(node, Invalidation::PAINT);

    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }

    selected_ = selected;
  }

  if (activate && selected && eligible(*selected)) {
    result.action = actions_[*selected];
  }

  return result;
}
} // namespace portals
