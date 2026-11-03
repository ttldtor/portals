// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/radio_group.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace portals {
struct RadioGroupValue::Data {
  Data(RowCollection<RadioItem> source, GraphemeText uncheckedText, GraphemeText checkedText)
      : rows(std::move(source)), visible(0), eligible(0), unchecked(std::move(uncheckedText)),
        checked(std::move(checkedText)) {
  }

  RowCollection<RadioItem> rows;
  std::vector<std::size_t> visible;
  std::vector<std::size_t> eligible;
  GraphemeText unchecked;
  GraphemeText checked;
  CellSize natural;
};

std::expected<RadioGroupValue, TreeError> RadioGroupValue::from(RowCollection<RadioItem> rows,
                                                                std::optional<RowId> selected) try {
  if (selected && rows.locate(*selected).state != RowLookupState::FOUND) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto source = rows.rows();
  const auto policy = source.empty() ? WidthPolicy{} : source.front().value.text.policy();
  auto unchecked = GraphemeText::from("( ) ", policy);
  auto checked = GraphemeText::from("(*) ", policy);

  if (!unchecked || !checked) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto data = std::make_shared<Data>(std::move(rows), std::move(*unchecked), std::move(*checked));
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto &item = source[index].value;

    if (item.text.policy() != policy) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if (!item.visible) {
      continue;
    }

    if (item.text.measure() > std::numeric_limits<std::int32_t>::max() - data->unchecked.measure() ||
        data->visible.size() == static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    data->natural.width = std::max(data->natural.width, item.text.measure() + data->unchecked.measure());
    data->visible.push_back(index);
    if (item.enabled) {
      data->eligible.push_back(index);
    }
  }

  data->natural.height = static_cast<std::int32_t>(data->visible.size());
  RadioGroupValue result;

  result.data_ = std::move(data);
  result.selected_ = selected;

  return result;
} catch (const std::bad_alloc &) {
  return std::unexpected(TreeError::NO_MEMORY);
} catch (const std::length_error &) {
  return std::unexpected(TreeError::NODE_LIMIT);
} catch (...) {
  return std::unexpected(TreeError::CALLBACK_FAILED);
}

std::expected<RadioGroupValue, TreeError> RadioGroupValue::withSelection(std::optional<RowId> selected) const {
  if (selected && (!data_ || data_->rows.locate(*selected).state != RowLookupState::FOUND)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto result = *this;

  result.selected_ = selected;

  return result;
}

std::span<const ModelRow<RadioItem>> RadioGroupValue::rows() const noexcept {
  return data_ ? data_->rows.rows() : std::span<const ModelRow<RadioItem>>{};
}

std::optional<RowId> RadioGroupValue::selectedId() const noexcept {
  return selected_;
}

std::string_view RadioGroupValue::selectedText() const noexcept {
  return selected_ ? rows()[static_cast<std::size_t>(data_->rows.locate(*selected_).index)].value.text.bytes()
                   : std::string_view{};
}

std::optional<RowId> RadioGroupValue::firstEligible() const noexcept {
  return data_ && !data_->eligible.empty() ? std::optional(rows()[data_->eligible.front()].id) : std::nullopt;
}

bool RadioGroupValue::operator==(const RadioGroupValue &other) const {
  if (selected_ != other.selected_ || shorthand_ != other.shorthand_) {
    return false;
  }

  if (data_ == other.data_) {
    return true;
  }

  return data_ && other.data_ ? data_->rows == other.data_->rows : rows().empty() && other.rows().empty();
}

RadioGroup::RadioGroup(ControlStyle style, SizePolicy policy, bool readOnly, WidthPolicy widthPolicy)
    : SizedWidget(policy), style_(style), widthPolicy_(widthPolicy), readOnly_(readOnly) {
}

std::expected<CellSize, TreeError> RadioGroup::measure(CellSize) {
  return value_.data_ ? value_.data_->natural : CellSize{};
}

std::expected<void, TreeError> RadioGroup::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;

  return {};
}

std::expected<void, TreeError> RadioGroup::paint(PaintContext &context) {
  auto clip = bounds_;
  if (tree_) {
    auto observed = geometry(true);
    if (!observed) {
      return std::unexpected(observed.error());
    }
    if (!observed->valid || observed->clip.empty()) {
      return {};
    }
    auto local = CellRect::from({observed->clip.origin().x - observed->surface.origin().x,
                                 observed->clip.origin().y - observed->surface.origin().y},
                                observed->clip.size());
    if (!local) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    clip = *local;
  }
  const auto &computed = computedStyle();
  const auto normalStyle = computed ? computed->normal : style_.normal;
  const auto focusedStyle = computed ? computed->focused : style_.focused;
  const auto disabledStyle = computed ? computed->disabled : style_.disabled;
  const auto normal = enabled_ ? normalStyle : disabledStyle;
  const std::array<RenderCommand, 1> fill{FillCommand{bounds_, clip, normal}};
  auto result = context.submit(fill);

  if (!result || !value_.data_ || bounds_.empty()) {
    return result;
  }

  const auto &data = *value_.data_;
  const auto count = std::min(data.visible.size(), static_cast<std::size_t>(clip.bottom()));

  for (auto index = static_cast<std::size_t>(clip.origin().y); index < count; ++index) {
    const auto &row = value_.rows()[data.visible[index]];
    const auto style = !enabled_ || !row.value.enabled ? disabledStyle
                       : focused_ && active_ == row.id ? focusedStyle
                                                       : normalStyle;
    const auto &chrome = value_.selected_ == row.id ? data.checked : data.unchecked;
    const auto y = static_cast<std::int32_t>(index);
    const std::array<RenderCommand, 2> commands{TextCommand{{0, y}, chrome, clip, style},
                                                TextCommand{{chrome.measure(), y}, row.value.text, clip, style}};

    result = context.submit(commands);
    if (!result) {
      return result;
    }
  }

  return {};
}

std::expected<NodeGeometry, TreeError> RadioGroup::geometry(bool painting) const {
  if (painting) {
    return tree_->paintGeometry(node_);
  }
  auto observed = tree_->observeGeometry(node_);
  return observed ? std::expected<NodeGeometry, TreeError>(observed->geometry) : std::unexpected(observed.error());
}

ComputedStyle RadioGroup::styleDefaults() const {
  ComputedStyle result;

  result.normal = style_.normal;
  result.focused = style_.focused;
  result.disabled = style_.disabled;

  return result;
}

RadioGroupValue RadioGroup::value() const {
  return value_;
}

std::expected<void, TreeError> syncRadioGroup(WidgetTree &tree, Node<RadioGroup> node, Node<Widget> focused) {
  auto eligible = tree.inputEligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](RadioGroup &group) {
    const bool hasFocus = *eligible && node.id() == focused.id();

    if (group.enabled_ == *eligible && group.focused_ == hasFocus) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);
    if (result) {
      group.enabled_ = *eligible;
      group.focused_ = hasFocus;
    }
  });

  return edited ? result : edited;
}

struct RadioGroupBinding::State {
  WidgetTree &tree;
  Node<RadioGroup> node;
  std::shared_ptr<Property<RadioGroupValue>> model;

  std::expected<void, TreeError> apply(const RadioGroupValue &value) {
    std::expected<void, TreeError> result;
    auto edited = tree.edit(node, [&](RadioGroup &group) {
      if (!value.rows().empty() && value.rows().front().value.text.policy() != group.widthPolicy_) {
        result = std::unexpected(TreeError::INVALID_ARGUMENT);
        return;
      }
      if (group.value_.data_ == value.data_ && group.value_.selected_ == value.selected_) {
        group.tree_ = &tree;
        group.node_ = node;
        return;
      }

      result = tree.invalidate(node, group.value_.data_ == value.data_ ? Invalidation::PAINT : Invalidation::LAYOUT);
      if (result) {
        // Reclaim the previous source only after the complete replacement is visible.
        // Source storage destruction may reenter UI code through the caller's allocator.
        [[maybe_unused]] const auto retired = group.value_;

        group.tree_ = &tree;
        group.node_ = node;
        group.value_ = value;
        group.active_ = value.firstEligible();
        if (value.selected_) {
          const auto &row = value.rows()[static_cast<std::size_t>(value.data_->rows.locate(*value.selected_).index)];

          if (row.value.enabled && row.value.visible) {
            group.active_ = row.id;
          }
        }
      }
    });

    return edited ? result : edited;
  }

  std::expected<void, TreeError> sync() {
    auto current = model->snapshot();

    return current ? apply((*current)->value) : std::unexpected(current.error());
  }

  RadioGroupResult select(std::optional<RowId> id) {
    auto current = model->snapshot();
    if (!current) {
      return std::unexpected(current.error());
    }

    auto candidate = (*current)->value.withSelection(id);
    if (!candidate) {
      return std::unexpected(candidate.error());
    }

    bool compatible{};
    const auto admitted = tree.read(node, [&](const RadioGroup &group) {
      compatible = candidate->rows().empty() || candidate->rows().front().value.text.policy() == group.widthPolicy_;
    });
    if (!admitted || !compatible) {
      return std::unexpected(admitted ? TreeError::INVALID_ARGUMENT : admitted.error());
    }

    auto receipt = model->set(std::move(*candidate));
    if (receipt) {
      auto applied = apply(receipt->snapshot->value);

      // Removal after a successful commit ends presentation, not the original model operation.
      if (!applied && applied.error() != TreeError::STALE_NODE && applied.error() != TreeError::CLOSED) {
        throw applied.error();
      }
    }

    return receipt;
  }

  std::optional<RowId> candidate(RadioGroup &group, const RoutedEvent &event) {
    if (group.readOnly_ || !group.value_.data_ || group.value_.data_->eligible.empty()) {
      return {};
    }

    const auto &data = *group.value_.data_;
    const auto rows = group.value_.rows();
    const auto selected = group.value_.selected_;
    const auto current = selected ? data.rows.locate(*selected) : RowLookup{};
    const auto position = current.state == RowLookupState::FOUND
                            ? std::ranges::lower_bound(data.eligible, static_cast<std::size_t>(current.index))
                            : data.eligible.end();
    auto ordinal = position != data.eligible.end() && *position == current.index
                     ? static_cast<std::size_t>(position - data.eligible.begin())
                     : std::size_t{};
    const bool hasSelection = position != data.eligible.end() && *position == current.index;

    if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
      if (mouse->button != MouseButton::LEFT || mouse->action != MouseAction::PRESS ||
          mouse->modifiers != InputModifiers{}) {
        return {};
      }

      auto geometry = group.geometry();
      if (!geometry) {
        throw geometry.error();
      }

      if (!geometry->valid) {
        return {};
      }
      const auto y = mouse->row - geometry->surface.origin().y;
      const auto x = mouse->column - geometry->surface.origin().x;

      if (x < 0 || x >= group.bounds_.size().width || y < 0 || static_cast<std::size_t>(y) >= data.visible.size()) {
        return {};
      }

      const auto &row = rows[data.visible[static_cast<std::size_t>(y)]];

      return row.value.enabled ? std::optional(row.id) : std::nullopt;
    }

    Action command;
    if (const auto *action = std::get_if<ActionEvent>(&event)) {
      if (action->modifiers != InputModifiers{} || action->keyAction == KeyAction::RELEASE) {
        return {};
      }

      command = action->action;
    } else if (const auto *text = std::get_if<TextInputEvent>(&event);
               text && text->scalar == U' ' && text->modifiers == InputModifiers{}) {
      command = Action::ACTIVATE;
    } else {
      return {};
    }

    switch (command) {
    case Action::MOVE_UP:
    case Action::MOVE_LEFT:
      ordinal = hasSelection ? (ordinal ? ordinal - 1 : data.eligible.size() - 1) : 0;
      break;
    case Action::MOVE_DOWN:
    case Action::MOVE_RIGHT:
      ordinal = hasSelection ? (ordinal + 1) % data.eligible.size() : 0;
      break;
    case Action::MOVE_HOME:
      ordinal = 0;
      break;
    case Action::MOVE_END:
      ordinal = data.eligible.size() - 1;
      break;
    case Action::ACTIVATE:
      break;
    default:
      return {};
    }

    return rows[data.eligible[ordinal]].id;
  }

  EventReply input(RadioGroup &group, const RoutedEvent &event, const std::function<void(RadioGroupResult)> &changed) {
    // Mouse coordinates describe the last arranged projection. Preserve its ID before
    // synchronizing a newer source; reinterpreting the same ordinal would select another row.
    const bool pointer = std::holds_alternative<MouseInputEvent>(event);
    const auto displayed = group.value_;
    if (!pointer) {
      auto synced = sync();
      if (!synced) {
        changed(std::unexpected(synced.error()));
        return EventReply::HANDLED;
      }
    }
    const auto id = candidate(group, event);
    if (!id) {
      return EventReply::IGNORED;
    }
    auto current = model->snapshot();
    if (!current) {
      changed(std::unexpected(current.error()));
      return EventReply::HANDLED;
    }
    const auto &value = (*current)->value;
    if (!value.data_ || (pointer && displayed.shorthand_ && displayed.data_ != value.data_)) {
      return pointer ? EventReply::HANDLED : EventReply::IGNORED;
    }
    const auto found = value.data_->rows.locate(*id);
    if (found.state != RowLookupState::FOUND) {
      return pointer ? EventReply::HANDLED : EventReply::IGNORED;
    }
    const auto &row = value.rows()[static_cast<std::size_t>(found.index)];
    if (!row.value.enabled || !row.value.visible) {
      return pointer ? EventReply::HANDLED : EventReply::IGNORED;
    }

    changed(select(id));

    return EventReply::HANDLED;
  }
};

RadioGroupBinding::RadioGroupBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription)
    : state_(std::move(state)), action_(std::move(action)), subscription_(std::move(subscription)) {
}

RadioGroupBinding::~RadioGroupBinding() = default;
RadioGroupBinding::RadioGroupBinding(RadioGroupBinding &&) noexcept = default;

RadioGroupBinding &RadioGroupBinding::operator=(RadioGroupBinding &&other) noexcept {
  if (this != &other) {
    RadioGroupBinding retired(std::move(other));

    using std::swap;

    swap(state_, retired.state_);
    swap(action_, retired.action_);
    swap(subscription_, retired.subscription_);
  }

  return *this;
}

std::expected<void, TreeError> RadioGroupBinding::sync() {
  const auto state = state_;

  return state ? state->sync() : std::unexpected(TreeError::INVALID_ARGUMENT);
}

std::expected<RadioGroupValue, TreeError> RadioGroupBinding::value() const {
  const auto state = state_;

  if (!state) {
    return std::unexpected(TreeError::CLOSED);
  }

  auto current = state->model->snapshot();

  return current ? std::expected<RadioGroupValue, TreeError>((*current)->value) : std::unexpected(current.error());
}

RadioGroupResult RadioGroupBinding::select(std::optional<RowId> id) {
  const auto state = state_;

  return state ? state->select(id) : std::unexpected(TreeError::CLOSED);
}

std::expected<RadioGroupBinding, TreeError> bindRadioGroup(WidgetTree &tree, EventRouter &router, Node<RadioGroup> node,
                                                           std::shared_ptr<Property<RadioGroupValue>> model,
                                                           std::function<void(RadioGroupResult)> changed) try {
  if (!model || !changed) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto state = std::make_shared<RadioGroupBinding::State>(RadioGroupBinding::State{tree, node, std::move(model)});
  auto subscription = state->model->observe(node, [state](RadioGroup &, auto) {
    auto result = state->sync();

    if (!result) {
      throw result.error();
    }
  });
  if (!subscription) {
    return std::unexpected(subscription.error());
  }

  auto action = router.bind(node, [state, changed = std::move(changed)](RadioGroup &group, const RoutedEvent &event) {
    return state->input(group, event, changed);
  });
  if (!action) {
    return std::unexpected(action.error());
  }

  auto synced = state->sync();
  if (!synced) {
    return std::unexpected(synced.error());
  }

  return RadioGroupBinding(std::move(state), std::move(*action), std::move(*subscription));
} catch (const std::bad_alloc &) {
  return std::unexpected(TreeError::NO_MEMORY);
}
} // namespace portals
