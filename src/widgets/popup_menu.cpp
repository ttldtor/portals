// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "dialog_detail.hpp"
#include "frame_text.hpp"
#include <portals/detail/layout_traversal.hpp>
#include <portals/popup_menu.hpp>
#include <portals/ui_menu.hpp>
#include <portals/ui_mount.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace portals {
namespace {
// Translate the existing zero-origin Border painter into one popup panel, without retaining commands.
class PanelPaint final : public PaintContext {
public:
  PanelPaint(PaintContext &target, CellPoint origin) : sink_(target), offset_(origin) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      auto submitted = std::visit(
        [&](const auto &value) -> std::expected<void, TreeError> {
          auto clip = value.clip.translated(offset_);

          if (!clip) {
            return std::unexpected(TreeError::INVALID_GEOMETRY);
          }

          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            const RenderCommand translated = TextCommand{
              {value.origin.x + offset_.x, value.origin.y + offset_.y}, value.text, *clip, value.style, value.z};

            return sink_.submit(std::span(&translated, 1));
          } else {
            auto area = value.area.translated(offset_);

            if (!area) {
              return std::unexpected(TreeError::INVALID_GEOMETRY);
            }

            const RenderCommand translated = FillCommand{*area, *clip, value.style, value.z};

            return sink_.submit(std::span(&translated, 1));
          }
        },
        command);

      if (!submitted) {
        return submitted;
      }
    }

    return {};
  }

private:
  PaintContext &sink_;
  CellPoint offset_;
};

char32_t folded(char32_t value) {
  return value >= U'A' && value <= U'Z' ? value + (U'a' - U'A') : value;
}

std::string scalarText(char32_t value) {
  std::string result(0, '\0');

  if (value < 0x80) {
    result.push_back(static_cast<char>(value));
  } else if (value < 0x800) {
    result.push_back(static_cast<char>(0xc0 | (value >> 6)));
    result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else if (value < 0x10000) {
    result.push_back(static_cast<char>(0xe0 | (value >> 12)));
    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else {
    result.push_back(static_cast<char>(0xf0 | (value >> 18)));
    result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
  }

  return result;
}

std::expected<void, TreeError> collect(std::span<const PopupEntry> entries, PopupMenuOptions options,
                                       std::vector<ui::ActionRef> &leaves, std::size_t &count, std::size_t depth,
                                       WidthPolicy width) {
  if (depth > options.maxDepth) {
    return std::unexpected(TreeError::DEPTH_LIMIT);
  }

  for (const auto &entry : entries) {
    if (!entry.submenu && !entry.children.empty()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (++count > options.maxEntries) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    if (entry.mnemonic) {
      const auto scalar = *entry.mnemonic;
      auto glyph = GraphemeText::from(scalarText(scalar), width);

      if (scalar < 32 || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) || !glyph ||
          glyph->measure() <= 0) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }

    if (entry.submenu) {
      if (!GraphemeText::from(entry.caption, width)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      if (!entry.children.empty()) {
        auto nested = collect(entry.children, options, leaves, count, depth + 1, width);

        if (!nested) {
          return nested;
        }
      }
    } else {
      leaves.push_back(entry.action);
    }
  }

  return {};
}
} // namespace

struct PopupMenu::State {
  struct Row {
    const PopupEntry *source{};
    std::shared_ptr<const ui::ActionMetadata> metadata;
    std::unique_ptr<GraphemeText> text;
    std::vector<std::size_t> children = std::vector<std::size_t>(0);
    bool visible{};
    bool eligible{};
  };

  struct Panel {
    std::optional<std::size_t> branch;
    std::vector<std::size_t> rows = std::vector<std::size_t>(0);
    std::optional<std::size_t> selected;
    std::size_t first{};
    CellRect bounds;
    std::int32_t width{2};
  };

  struct Prepared {
    std::vector<Row> rows = std::vector<Row>(0);
    std::vector<std::size_t> roots = std::vector<std::size_t>(0);
    std::vector<Panel> panels = std::vector<Panel>(0);
  };

  State(const std::vector<PopupEntry> &source, PopupMenuOptions limits, WidthPolicy width)
      : entries(source), options(limits), frame(*detail::prepareFrameText(width, options.frame)) {
  }

  std::vector<PopupEntry> entries;
  PopupMenuOptions options;
  std::unique_ptr<Prepared> prepared;
  detail::PreparedFrameText frame;
  CellRect bounds;
  std::optional<CellRect> anchor;
  bool dirty{true};

  static std::optional<std::size_t> next(const Prepared &prepared, const Panel &panel,
                                         std::optional<std::size_t> current, bool forward) {
    if (panel.rows.empty()) {
      return {};
    }

    auto index = current.value_or(forward ? panel.rows.size() - 1 : 0);

    for (std::size_t visited = 0; visited < panel.rows.size(); ++visited) {
      index = forward ? (index + 1) % panel.rows.size() : (index ? index - 1 : panel.rows.size() - 1);

      if (prepared.rows[panel.rows[index]].eligible) {
        return index;
      }
    }

    return {};
  }

  static Panel panel(const Prepared &prepared, std::span<const std::size_t> ids,
                     std::optional<std::size_t> branch = {}) {
    Panel result;

    result.branch = branch;

    for (auto id : ids) {
      const auto &row = prepared.rows[id];

      if (row.visible) {
        result.rows.push_back(id);
        result.width =
          std::max(result.width, static_cast<std::int32_t>(std::min<std::int64_t>(
                                   std::int64_t(row.text->measure()) + 2, std::numeric_limits<std::int32_t>::max())));
      }
    }

    result.selected = next(prepared, result, {}, true);

    return result;
  }

  std::expected<void, TreeError> append(Prepared &incoming, std::span<const PopupEntry> source,
                                        std::vector<std::size_t> &ids, WidthPolicy width) const {
    for (const auto &entry : source) {
      Row row;

      row.source = &entry;

      if (entry.submenu) {
        auto children = append(incoming, entry.children, row.children, width);

        if (!children) {
          return children;
        }

        row.visible = entry.visible;
        row.eligible = entry.enabled && std::ranges::any_of(row.children, [&](auto id) {
                         return incoming.rows[id].visible && incoming.rows[id].eligible;
                       });
      } else {
        auto snapshot = entry.action.snapshot();

        if (!snapshot && snapshot.error() != TreeError::CLOSED && snapshot.error() != TreeError::STALE_NODE) {
          return std::unexpected(snapshot.error());
        }

        row.metadata = snapshot ? *snapshot : nullptr;
        row.visible = row.metadata && row.metadata->visible;
        row.eligible = row.visible && row.metadata->enabled;
      }

      std::string bytes(row.metadata && row.metadata->checked ? "[x] " : "    ");

      bytes.append(entry.submenu ? entry.caption : row.metadata ? row.metadata->caption : "");

      if (row.metadata && !row.metadata->shortcut.empty()) {
        bytes.append("  ").append(row.metadata->shortcut);
      }

      if (entry.mnemonic) {
        bytes.append(" [").append(scalarText(*entry.mnemonic)).append("]");
      }

      if (entry.submenu) {
        bytes.append(" >");
      }

      auto text = GraphemeText::from(bytes, width);

      if (!text) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      row.text = std::make_unique<GraphemeText>(std::move(*text));
      ids.push_back(incoming.rows.size());
      incoming.rows.push_back(std::move(row));
    }

    return {};
  }

  std::expected<std::unique_ptr<Prepared>, TreeError> build(WidthPolicy width) const {
    auto incoming = std::make_unique<Prepared>();
    auto appended = append(*incoming, entries, incoming->roots, width);

    if (!appended) {
      return std::unexpected(appended.error());
    }

    incoming->panels.push_back(panel(*incoming, incoming->roots));

    if (prepared) {
      for (std::size_t index = 0; index < prepared->panels.size(); ++index) {
        if (index > 0) {
          const auto branch = prepared->panels[index].branch;

          if (!branch || *branch >= incoming->rows.size() || !incoming->rows[*branch].eligible ||
              !incoming->rows[*branch].visible) {
            break;
          }

          const auto &parent = incoming->panels.back();

          if (!parent.selected || parent.rows[*parent.selected] != *branch) {
            break;
          }

          incoming->panels.push_back(panel(*incoming, incoming->rows[*branch].children, branch));
        }

        auto &target = incoming->panels.back();
        const auto &previous = prepared->panels[index];

        if (previous.selected && *previous.selected < previous.rows.size()) {
          const auto identity = previous.rows[*previous.selected];
          const auto selected = std::ranges::find(target.rows, identity);

          if (selected != target.rows.end() && incoming->rows[identity].eligible) {
            target.selected = static_cast<std::size_t>(selected - target.rows.begin());
          }
        }

        target.first = target.selected ? std::min(previous.first, *target.selected) : 0;
      }
    }

    return incoming;
  }
};

PopupMenu::PopupMenu(GraphemeText empty, const std::vector<PopupEntry> &entries,
                     const std::vector<ui::ActionRef> &leaves, PopupMenuOptions options, Node<Dialog> layer,
                     ControlStyle style, SizePolicy policy)
    : Menu(empty, leaves, MenuOptions{options.maxEntries}, style, policy),
      state_(std::make_unique<State>(entries, options, empty.policy())), layer_(layer) {
}

PopupMenu::~PopupMenu() = default;

Node<Dialog> PopupMenu::dialogRoot() const noexcept {
  return layer_;
}

std::size_t PopupMenu::panelCount() const noexcept {
  return state_->prepared ? state_->prepared->panels.size() : 0;
}

std::optional<std::size_t> PopupMenu::selectedIndex() const noexcept {
  if (!panelCount()) {
    return {};
  }

  const auto &prepared = *state_->prepared;
  const auto &panel = prepared.panels.back();

  if (!panel.selected) {
    return {};
  }

  const auto &siblings = panel.branch ? prepared.rows[*panel.branch].source->children : state_->entries;
  const auto *selected = prepared.rows[panel.rows[*panel.selected]].source;

  return static_cast<std::size_t>(selected - siblings.data());
}

std::size_t PopupMenu::visibleRowCount() const noexcept {
  return panelCount() ? state_->prepared->panels.back().rows.size() : 0;
}

void PopupMenu::resetSelection() noexcept {
  if (state_->prepared) {
    auto &panels = state_->prepared->panels;

    panels.resize(1);
    panels.front().selected = State::next(*state_->prepared, panels.front(), {}, true);
    panels.front().first = 0;
  }

  state_->dirty = true;
}

void PopupMenu::invalidatePresentation() noexcept {
  state_->dirty = true;
}

std::expected<void, TreeError> PopupMenu::prepare(WidgetTree &tree, Node<Menu> node, WidthPolicy width) {
  std::optional<CellRect> anchor;
  auto observed = tree.read(layer_, [&](const Dialog &dialog) {
    anchor = detail::DialogAccess::anchorValue(dialog);
  });

  if (!observed) {
    return observed;
  }

  if (!state_->dirty) {
    if (anchor != state_->anchor) {
      auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

      if (!invalidated) {
        return invalidated;
      }

      state_->anchor = anchor;
    }

    return {};
  }

  try {
    auto incoming = state_->build(width);

    if (!incoming) {
      return std::unexpected(incoming.error());
    }

    auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

    if (!invalidated) {
      return invalidated;
    }

    state_->prepared = std::move(*incoming);
    state_->anchor = anchor;
    state_->dirty = false;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<CellSize, TreeError> PopupMenu::measure(CellSize available) {
  auto policy = detail::effectiveSizePolicy(*this);

  if (!policy) {
    return std::unexpected(policy.error());
  }

  const auto exceeds = [](const AxisSize &axis, std::int32_t cap) {
    return axis.minimum > cap || (axis.mode == SizeMode::FIXED && axis.preferred > cap);
  };

  if (exceeds(policy->width, state_->options.maximumPanelSize.width) ||
      exceeds(policy->height, state_->options.maximumPanelSize.height)) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  CellSize desired{2, 2};

  if (state_->prepared) {
    std::int64_t width{};

    for (const auto &panel : state_->prepared->panels) {
      width += panel.width + (width ? state_->options.panelGap : 0);
      desired.height = std::max(
        desired.height, static_cast<std::int32_t>(std::min<std::size_t>(
                          panel.rows.size() + 2, static_cast<std::size_t>(state_->options.maximumPanelSize.height))));
    }

    desired.width = static_cast<std::int32_t>(std::min<std::int64_t>(width, state_->options.maximumPanelSize.width));
  }

  desired.width = std::min({desired.width, state_->options.maximumPanelSize.width, available.width});
  desired.height = std::min({desired.height, state_->options.maximumPanelSize.height, available.height});

  return desired;
}

std::expected<void, TreeError> PopupMenu::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  state_->bounds = *bounds;

  if (!state_->prepared) {
    return {};
  }

  auto &panels = state_->prepared->panels;

  for (std::size_t index = 0; index < panels.size(); ++index) {
    auto &panel = panels[index];
    const auto width = std::min(panel.width, local.size().width);
    const auto height = static_cast<std::int32_t>(
      std::min<std::size_t>(panel.rows.size() + 2, static_cast<std::size_t>(local.size().height)));
    const auto page = static_cast<std::size_t>(std::max(0, height - 2));

    if (panel.selected && page) {
      panel.first = std::min(panel.first, *panel.selected);

      if (*panel.selected >= panel.first + page) {
        panel.first = *panel.selected - page + 1;
      }
    }

    CellPoint origin{};

    if (!index && state_->anchor) {
      const auto x = std::int64_t(state_->anchor->origin().x) - local.origin().x;

      origin.x = static_cast<std::int32_t>(std::clamp<std::int64_t>(x, 0, std::max(0, local.size().width - width)));
    }

    if (index) {
      const auto &parent = panels[index - 1];

      auto x = std::int64_t(parent.bounds.right()) + state_->options.panelGap;
      const auto y = std::int64_t(parent.bounds.origin().y) + 1 +
                     static_cast<std::int64_t>(parent.selected.value_or(0) - parent.first);

      if (x + width > local.size().width) {
        x = std::int64_t(parent.bounds.origin().x) - state_->options.panelGap - width;
      }

      origin.x = static_cast<std::int32_t>(std::clamp<std::int64_t>(x, 0, std::max(0, local.size().width - width)));
      origin.y = static_cast<std::int32_t>(std::clamp<std::int64_t>(y, 0, std::max(0, local.size().height - height)));
    }

    auto rectangle = CellRect::from(origin, {width, height});

    if (!rectangle) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    panel.bounds = *rectangle;
  }

  return {};
}

std::expected<void, TreeError> PopupMenu::paint(PaintContext &context) {
  if (!state_->prepared) {
    return {};
  }

  std::optional<ComputedStyle> manual;

  if (!computedStyle()) {
    manual.emplace(styleDefaults());
  }

  const auto style = computedStyle() ? detail::ComputedStyleView(*computedStyle()) : detail::ComputedStyleView(*manual);
  const auto normal = enabled() ? style.normal : style.disabled;
  const auto emit = [&](CellPoint origin, const GraphemeText &text, CellRect clip, CellStyle appearance) {
    const RenderCommand command = TextCommand{origin, text, clip, appearance};

    return context.submit(std::span(&command, 1));
  };

  for (const auto &panel : state_->prepared->panels) {
    const auto rectangle = panel.bounds;
    const RenderCommand fill = FillCommand{rectangle, rectangle, normal};
    auto result = context.submit(std::span(&fill, 1));

    if (!result) {
      return result;
    }

    PanelPaint frameSink(context, rectangle.origin());
    auto localFrame = CellRect::from({}, rectangle.size());

    if (!localFrame) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    result = detail::paintFrame(frameSink, state_->frame, state_->options.frame, *localFrame, normal, {},
                                style.frame[enabled() ? 0 : 2]);

    if (!result) {
      return result;
    }

    for (auto y = rectangle.origin().y + 1; y < rectangle.bottom() - 1; ++y) {
      const auto offset = panel.first + static_cast<std::size_t>(y - rectangle.origin().y - 1);

      if (offset >= panel.rows.size()) {
        break;
      }

      const auto &row = state_->prepared->rows[panel.rows[offset]];
      const auto appearance = !enabled() || !row.eligible ? style.disabled
                              : panel.selected == offset  ? style.focused
                                                          : style.normal;
      auto clip = CellRect::from({rectangle.origin().x + 1, y}, {std::max(0, rectangle.size().width - 2), 1});

      if (!clip) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }

      const RenderCommand selected = FillCommand{*clip, rectangle, appearance};

      result = context.submit(std::span(&selected, 1));

      if (!result) {
        return result;
      }

      result = emit(clip->origin(), *row.text, *clip, appearance);

      if (!result) {
        return result;
      }
    }
  }

  return {};
}

std::expected<Menu::Input, TreeError> PopupMenu::input(WidgetTree &tree, Node<Menu> node, const RoutedEvent &event,
                                                       const NodeGeometry &geometry) {
  Input result;

  // The router checks live effective eligibility. Control's paint state may still describe the closed frame.
  if (!state_->prepared) {
    return result;
  }

  try {
    auto &ready = *state_->prepared;
    auto incoming = ready.panels;
    auto &active = incoming.back();
    bool activate{}, right{}, left{}, cancel{};
    std::optional<Action> command;
    KeyEvent decodedText;
    const KeyEvent *key = std::get_if<KeyEvent>(&event);

    if (const auto *text = std::get_if<TextInputEvent>(&event)) {
      // Legacy VT emits one TextInputEvent per scalar; native extended input may emit CHARACTER instead.
      decodedText = KeyEvent{InputKey::CHARACTER, text->scalar, text->modifiers, KeyAction::PRESS};
      key = &decodedText;
    }

    if (const auto *semantic = std::get_if<ActionEvent>(&event)) {
      command = semantic->action;
      result.invocation = {semantic->source, semantic->modifiers};

      if ((semantic->action == Action::ACTIVATE || semantic->action == Action::CANCEL) &&
          semantic->source == ClickSource::KEYBOARD && semantic->keyAction != KeyAction::PRESS) {
        return result;
      }
    } else if (key) {
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
        activate = key->action == KeyAction::PRESS;
        break;
      case InputKey::RIGHT:
        right = key->action == KeyAction::PRESS;
        break;
      case InputKey::LEFT:
        left = key->action == KeyAction::PRESS;
        break;
      case InputKey::ESCAPE:
        cancel = key->action == KeyAction::PRESS;
        break;
      case InputKey::TAB:
        if (!key->modifiers.alt && !key->modifiers.control && !key->modifiers.super) {
          result.dismiss = layer_;
          result.handled = true;
        }
        break;
      case InputKey::CHARACTER:
        if (key->action == KeyAction::PRESS && !key->modifiers.control && !key->modifiers.alt &&
            !key->modifiers.super) {
          if (key->scalar == U' ') {
            activate = true;
          } else {
            std::vector<std::size_t> matches(0);

            for (std::size_t index = 0; index < active.rows.size(); ++index) {
              const auto &row = ready.rows[active.rows[index]];

              if (row.eligible && row.source->mnemonic && folded(*row.source->mnemonic) == folded(key->scalar)) {
                matches.push_back(index);
              }
            }

            if (!matches.empty()) {
              auto next = std::ranges::find_if(matches, [&](auto value) {
                return !active.selected || value > *active.selected;
              });

              active.selected = next == matches.end() ? matches.front() : *next;
              activate = matches.size() == 1;
              result.handled = true;
            }
          }
        }
        break;
      default:
        break;
      }
    } else if (const auto *mouse = std::get_if<MouseInputEvent>(&event);
               mouse && mouse->button == MouseButton::LEFT && mouse->action == MouseAction::PRESS && geometry.valid) {
      const CellPoint point{mouse->column - geometry.surface.origin().x, mouse->row - geometry.surface.origin().y};

      for (std::size_t level = incoming.size(); level-- > 0;) {
        auto &panel = incoming[level];

        if (panel.bounds.contains(point)) {
          const auto row = point.y - panel.bounds.origin().y - 1;
          const auto offset = row < 0 ? panel.rows.size() : panel.first + static_cast<std::size_t>(row);

          if (row >= 0 && point.y < panel.bounds.bottom() - 1 && point.x > panel.bounds.origin().x &&
              point.x < panel.bounds.right() - 1 && offset < panel.rows.size() &&
              ready.rows[panel.rows[offset]].eligible) {
            panel.selected = offset;
            incoming.resize(level + 1);
            activate = true;
          }

          result.handled = true;
          break;
        }
      }

      if (!result.handled) {
        result.dismiss = layer_;
      }

      result.invocation = {ClickSource::MOUSE, mouse->modifiers};
      result.handled = true;
    }

    auto &panel = incoming.back();

    if (command) {
      switch (*command) {
      case Action::MOVE_UP:
        panel.selected = State::next(ready, panel, panel.selected, false);
        break;
      case Action::MOVE_DOWN:
        panel.selected = State::next(ready, panel, panel.selected, true);
        break;
      case Action::MOVE_HOME:
        panel.selected = State::next(ready, panel, {}, true);
        break;
      case Action::MOVE_END:
        panel.selected = State::next(ready, panel, {}, false);
        break;
      case Action::ACTIVATE:
        activate = true;
        break;
      case Action::CANCEL:
        cancel = true;
        break;
      case Action::MOVE_LEFT:
        left = true;
        break;
      case Action::MOVE_RIGHT:
        right = true;
        break;
      default:
        break;
      }
    }

    if (cancel || left) {
      if (incoming.size() > 1) {
        incoming.pop_back();
      } else if (cancel) {
        result.dismiss = layer_;
      }

      result.handled = true;
    } else if ((activate || right) && panel.selected) {
      const auto id = panel.rows[*panel.selected];
      const auto &row = ready.rows[id];

      if (row.eligible) {
        if (row.source->submenu) {
          incoming.push_back(State::panel(ready, row.children, id));
        } else if (activate) {
          result.action = row.source->action;
          result.dismiss = layer_;
        }
      }

      result.handled = true;
    }

    result.handled = result.handled || command.has_value() || activate || right;

    if (result.handled && result.dismiss == Node<Dialog>{}) {
      auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);

      if (!invalidated) {
        return std::unexpected(invalidated.error());
      }

      ready.panels.swap(incoming);
    }

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}
} // namespace portals

namespace portals::ui::detail {
Description<PopupMenu> popupMenuRecipe(std::span<const PopupEntry> entries, PopupMenuOptions options,
                                       ControlStyle style, SizePolicy policy) {
  struct Cold {
    std::vector<PopupEntry> entries = std::vector<PopupEntry>(0);
    std::vector<ActionRef> leaves = std::vector<ActionRef>(0);
    std::vector<std::shared_ptr<ActionStore>> stores = std::vector<std::shared_ptr<ActionStore>>(0);
    std::optional<TreeError> failure;
  };

  auto cold = std::make_shared<Cold>();

  if (!options.maxEntries || options.maxEntries > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      !options.maxDepth || options.maxDepth > 32 || options.maximumPanelSize.width <= 0 ||
      options.maximumPanelSize.height <= 0 || options.panelGap < 0 ||
      options.panelGap > options.maximumPanelSize.width) {
    cold->failure = TreeError::INVALID_ARGUMENT;
  } else {
    std::size_t count{};
    auto valid = collect(entries, options, cold->leaves, count, 1, {});

    if (!valid) {
      cold->failure = valid.error();
    } else {
      cold->entries.assign(entries.begin(), entries.end());
    }
  }

  for (const auto &action : cold->leaves) {
    cold->stores.push_back(ActionAccess::retain(action));
  }

  return Access::cold<PopupMenu>([cold, options, style,
                                  policy](ManagedScene &scene, Node<Widget> parent,
                                          MountContext &context) -> std::expected<Node<PopupMenu>, ManagedError> {
    if (cold->failure) {
      return std::unexpected(error(parent, *cold->failure));
    }

    auto host = Access::layerHost(scene, parent);

    if (!host) {
      return std::unexpected(host.error());
    }

    auto empty = GraphemeText::from("", scene.widthPolicy());

    if (!empty) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }

    auto frame = portals::detail::prepareFrameText(scene.widthPolicy(), options.frame);

    if (!frame || !options.frame.sides.top || !options.frame.sides.bottom || !options.frame.sides.left ||
        !options.frame.sides.right) {
      return std::unexpected(error(parent, frame ? TreeError::INVALID_ARGUMENT : frame.error()));
    }

    auto layer = scene.emplace<Dialog>(*host, DialogOptions{.placement = DialogPlacement::BELOW_START,
                                                            .maximumPanelSize = options.maximumPanelSize,
                                                            .viewportMargin = 0,
                                                            .anchorGap = 0,
                                                            .closeOnEscape = false});

    if (!layer) {
      return std::unexpected(layer.error());
    }

    auto node =
      scene.emplace<PopupMenu>(*layer, std::move(*empty), cold->entries, cold->leaves, options, *layer, style, policy);

    if (!node) {
      return node;
    }

    auto attached = ActionAccess::menu(scene, *node, context);

    if (!attached) {
      return std::unexpected(attached.error());
    }

    auto registered = scene.dialog(*layer, *node, *node);

    if (!registered) {
      return std::unexpected(registered.error());
    }

    auto companion = Access::choiceCompanion(scene, *node, *layer);

    if (!companion) {
      return std::unexpected(companion.error());
    }

    ActionAccess::popupLayer(scene, *layer, *node);

    auto styled = scene.style(
      *node, StyleTarget{.type = "PopupMenu", .classes = {"popup-menu"}, .control = true, .baseTypes = {"Menu"}});

    if (!styled) {
      return std::unexpected(styled.error());
    }

    auto prepared = ActionAccess::preparePopup(scene, *node);

    return prepared ? node : std::unexpected(prepared.error());
  });
}
} // namespace portals::ui::detail
