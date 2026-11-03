// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"
#include <portals/slider.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace portals {
namespace {
GraphemeText sliderGlyph(std::string_view bytes, WidthPolicy width) {
  return GraphemeText::from(bytes, width).value();
}

std::optional<Action> sliderAction(const RoutedEvent &event) {
  if (const auto *action = std::get_if<ActionEvent>(&event)) {
    if (action->modifiers == InputModifiers{} && action->keyAction != KeyAction::RELEASE) {
      return action->action;
    }
  }

  return std::nullopt;
}
} // namespace

Slider::Slider(WidthPolicy width, SliderOptions options, ControlStyle style, SizePolicy policy)
    : Control(sliderGlyph("", width), "", "", style, policy), options_(options), track_(sliderGlyph("-", width)),
      thumb_(sliderGlyph("O", width)) {
}

std::expected<CellSize, TreeError> Slider::measure(CellSize) {
  if (options_.preferredWidth < 0 || options_.pageSteps == 0 || options_.maxPaintCells == 0 || track_.measure() != 1 ||
      thumb_.measure() != 1) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return CellSize{options_.preferredWidth, 1};
}

void Slider::placeThumb() noexcept {
  const auto width = bounds_.size().width;

  thumbColumn_ = width > 1 ? static_cast<std::int32_t>(std::round(position_ * static_cast<double>(width - 1))) : 0;
}

std::expected<void, TreeError> Slider::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;
  placeThumb();

  return {};
}

void Slider::display(NumericValue value) noexcept {
  value_ = value;
  const auto range = value.range();

  position_ = (value.value() - range.minimum) / (range.maximum - range.minimum);
  placeThumb();
}

std::expected<void, TreeError> Slider::paint(PaintContext &context) {
  const auto valid = measure({});

  if (!valid) {
    return std::unexpected(valid.error());
  }

  const auto width = bounds_.size().width;

  if (bounds_.size().height == 0 || width == 0) {
    return {};
  }

  if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(bounds_.size().height) > options_.maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  std::optional<ComputedStyle> manual;

  if (!computedStyle()) {
    manual.emplace(Control::styleDefaults());
  }

  const auto style = computedStyle() ? detail::ComputedStyleView(*computedStyle()) : detail::ComputedStyleView(*manual);
  const auto ink = !enabled() ? style.disabled : focused() ? style.focused : style.normal;
  const std::array<RenderCommand, 1> background{FillCommand{bounds_, bounds_, ink}};
  auto filled = context.submit(background);

  if (!filled) {
    return filled;
  }

  for (std::int32_t column = 0; column < width; ++column) {
    const std::array<RenderCommand, 1> command{
      TextCommand{{column, 0}, column == thumbColumn_ ? thumb_ : track_, bounds_, ink}};
    auto submitted = context.submit(command);

    if (!submitted) {
      return submitted;
    }
  }

  return {};
}

std::expected<void, TreeError> setSliderReadOnly(WidgetTree &tree, Node<Slider> node, bool readOnly) {
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](Slider &slider) {
    if (slider.options_.readOnly == readOnly) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);

    if (result) {
      slider.options_.readOnly = readOnly;
    }
  });

  return edited ? result : edited;
}

struct SliderBinding::State {
  WidgetTree &tree;
  EventRouter &router;
  Node<Slider> node;
  std::shared_ptr<Property<NumericValue>> model;
  std::function<void(SliderResult)> changed;
  std::function<void(const SliderOperationEvent &)> finished;
  PointerCapture capture;
  std::optional<SliderOperationEvent> operation;
  std::optional<std::int32_t> heldThumbColumn;
  std::uint64_t expectedRevision{};
  Publisher<SliderOperationEvent> terminal;
  Subscription terminalSubscription;
  std::shared_ptr<void> retained;
  bool retired{};

  std::expected<void, TreeError> apply(const NumericValue &value) {
    std::expected<void, TreeError> result;
    auto edited = tree.edit(node, [&](Slider &slider) {
      if (slider.value_ == value) {
        return;
      }

      result = tree.invalidate(node, Invalidation::PAINT);

      if (result) {
        slider.display(value);
      }
    });

    return edited ? result : edited;
  }

  bool live() const {
    return !retired && tree.inspect(node).has_value();
  }

  std::expected<bool, TreeError> writable() const {
    auto eligible = tree.inputEligible(node);

    if (!eligible || !*eligible) {
      return eligible;
    }

    auto geometry = tree.inspect(node);

    if (!geometry) {
      return std::unexpected(geometry.error());
    }

    if (geometry->geometry.valid && geometry->geometry.surface.empty()) {
      return false;
    }

    bool readOnly{};
    auto read = tree.read(node, [&](const Slider &slider) {
      readOnly = slider.readOnly();
    });

    return read ? std::expected<bool, TreeError>{!readOnly} : std::unexpected(read.error());
  }

  std::expected<void, TreeError> finish(SliderOperationStatus status, bool deferred) {
    if (!operation) {
      return {};
    }

    auto event = std::move(*operation);

    operation.reset();
    heldThumbColumn.reset();
    capture.disconnect();
    event.status = status;

    if (!finished || !live()) {
      return {};
    }

    if (!deferred) {
      finished(event);

      return {};
    }

    try {
      auto published = terminal.publish(std::make_shared<const SliderOperationEvent>(std::move(event)));

      if (!published) {
        return std::unexpected(published.error() == SchedulerError::WOULD_BLOCK ? TreeError::WOULD_BLOCK
                                                                                : TreeError::CLOSED);
      }

      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    }
  }

  std::expected<void, TreeError> sync() {
    auto current = model->snapshot();

    if (!current) {
      return std::unexpected(current.error());
    }

    bool readOnly{};
    auto read = tree.read(node, [&](const Slider &slider) {
      readOnly = slider.readOnly();
    });

    if (!read) {
      return read;
    }

    if (operation) {
      const auto conflict = (*current)->revision != expectedRevision;
      bool lostCapture{};

      if (operation->kind == SliderOperationKind::DRAG && !detail::PointerCaptureAccess::released(capture)) {
        auto captured = router.pointerCapture();

        if (!captured) {
          return std::unexpected(captured.error());
        }

        lostCapture = !capture.connected() || *captured != Node<Widget>{node};
      }

      if (conflict || readOnly || lostCapture) {
        auto done = finish(conflict ? SliderOperationStatus::CONFLICT : SliderOperationStatus::CANCELLED, true);

        if (!done) {
          return done;
        }
      }
    }

    return apply((*current)->value);
  }

  std::expected<bool, TreeError> reconcile() {
    if (!operation || !live()) {
      return false;
    }

    auto current = model->snapshot();

    if (!current) {
      return std::unexpected(current.error());
    }

    if ((*current)->revision != expectedRevision) {
      auto done = finish(SliderOperationStatus::CONFLICT, false);

      if (!done) {
        return std::unexpected(done.error());
      }

      return false;
    }

    return true;
  }

  std::expected<void, TreeError> commit(double candidate) {
    auto ready = reconcile();

    if (!ready || !*ready) {
      return ready ? std::expected<void, TreeError>{} : std::unexpected(ready.error());
    }

    auto allowed = writable();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (!*allowed) {
      return finish(SliderOperationStatus::CANCELLED, false);
    }

    auto current = model->snapshot();

    if (!current) {
      return std::unexpected(current.error());
    }

    auto value = (*current)->value.withValue(candidate);

    if (!value) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto result = model->set(*value);

    if (!result) {
      if (live()) {
        changed(std::unexpected(result.error()));
      }

      return finish(SliderOperationStatus::CANCELLED, false);
    }

    // Preserve the exact admitted revision before any callback/capture destructor can reenter.
    if (!live() || !operation) {
      return {};
    }

    expectedRevision = result->snapshot->revision;

    if (result->changed) {
      operation->lastUserUpdate = *result;
    }

    auto applied = apply(result->snapshot->value);

    if (!applied && applied.error() != TreeError::STALE_NODE && applied.error() != TreeError::CLOSED) {
      return applied;
    }

    if (result->changed && live()) {
      changed(std::move(result));
    }

    if (live() && operation) {
      auto stillCurrent = reconcile();

      if (!stillCurrent) {
        return std::unexpected(stillCurrent.error());
      }
    }

    return {};
  }

  std::expected<void, TreeError> begin(SliderOperationKind kind) {
    auto current = model->snapshot();

    if (!current) {
      return std::unexpected(current.error());
    }

    operation = SliderOperationEvent{kind, SliderOperationStatus::COMPLETED, *current, {}};
    heldThumbColumn.reset();
    expectedRevision = (*current)->revision;

    return {};
  }

  std::expected<double, TreeError> pointerValue(const MouseInputEvent &mouse) {
    auto geometry = tree.inspect(node);

    if (!geometry) {
      return std::unexpected(geometry.error());
    }

    if (!geometry->geometry.valid || geometry->geometry.surface.size().width < 2) {
      return model->snapshot().transform([](const auto &snapshot) {
        return snapshot->value.value();
      });
    }

    auto current = model->snapshot();

    if (!current) {
      return std::unexpected(current.error());
    }

    const auto width = geometry->geometry.surface.size().width;
    const auto column = static_cast<std::int64_t>(mouse.column) - geometry->geometry.surface.origin().x;
    const auto fraction = std::clamp(static_cast<double>(column) / static_cast<double>(width - 1), 0.0, 1.0);
    const auto range = (*current)->value.range();
    const auto candidate = fraction == 0   ? range.minimum
                           : fraction == 1 ? range.maximum
                                           : range.minimum + fraction * (range.maximum - range.minimum);

    return detail::snapNumeric(range, candidate);
  }

  std::expected<EventReply, TreeError> step(Action action, int direction, std::uint64_t steps) {
    auto done = finish(SliderOperationStatus::CANCELLED, false);

    if (!done || !live()) {
      return done ? std::expected<EventReply, TreeError>{EventReply::HANDLED} : std::unexpected(done.error());
    }

    auto started = begin(SliderOperationKind::STEP);

    if (!started) {
      return std::unexpected(started.error());
    }

    const auto &value = operation->initial->value;
    const auto candidate = action == Action::MOVE_HOME  ? value.range().minimum
                           : action == Action::MOVE_END ? value.range().maximum
                                                        : detail::nudgeNumeric(value, direction, steps);
    auto committed = commit(candidate);

    if (!committed) {
      return std::unexpected(committed.error());
    }

    done = finish(SliderOperationStatus::COMPLETED, false);

    return done ? std::expected<EventReply, TreeError>{EventReply::HANDLED} : std::unexpected(done.error());
  }

  std::expected<EventReply, TreeError> press(Slider &slider, const MouseInputEvent &mouse) {
    if (operation) {
      return EventReply::HANDLED;
    }

    auto started = begin(SliderOperationKind::DRAG);

    if (!started) {
      return std::unexpected(started.error());
    }

    auto captured = router.capturePointer(node);

    if (!captured && captured.error() != TreeError::INVALID_ARGUMENT) {
      operation.reset();

      return std::unexpected(captured.error());
    }

    const bool clickOnly = !captured;

    if (captured) {
      capture = std::move(*captured);
    } else {
      operation->kind = SliderOperationKind::STEP;
    }

    auto candidate = pointerValue(mouse);

    if (!candidate) {
      return std::unexpected(candidate.error());
    }

    auto geometry = tree.inspect(node);
    const bool pressedThumb =
      geometry && geometry->geometry.valid &&
      static_cast<std::int64_t>(mouse.column) - geometry->geometry.surface.origin().x == slider.thumbColumn_;
    if (pressedThumb) {
      heldThumbColumn = mouse.column;
    }

    auto committed = pressedThumb ? std::expected<void, TreeError>{} : commit(*candidate);

    if (!committed) {
      return std::unexpected(committed.error());
    }

    if (clickOnly) {
      auto done = finish(SliderOperationStatus::COMPLETED, false);

      if (!done) {
        return std::unexpected(done.error());
      }
    }

    return EventReply::HANDLED;
  }

  std::expected<EventReply, TreeError> continuePointer(const MouseInputEvent &mouse) {
    if (!operation) {
      return EventReply::IGNORED;
    }

    auto ready = reconcile();

    if (!ready || !*ready) {
      return ready ? std::expected<EventReply, TreeError>{EventReply::HANDLED} : std::unexpected(ready.error());
    }

    const bool admittedRelease =
      mouse.action == MouseAction::RELEASE && detail::PointerCaptureAccess::released(capture);

    if (!capture.connected() && !admittedRelease) {
      auto done = finish(SliderOperationStatus::CANCELLED, false);

      return done ? std::expected<EventReply, TreeError>{EventReply::HANDLED} : std::unexpected(done.error());
    }

    if (mouse.action == MouseAction::MOVE || mouse.action == MouseAction::RELEASE) {
      // A cell-rounded thumb must retain the source until horizontal movement begins.
      if (heldThumbColumn && *heldThumbColumn == mouse.column) {
        if (mouse.action == MouseAction::RELEASE) {
          auto done = finish(SliderOperationStatus::COMPLETED, false);

          if (!done) {
            return std::unexpected(done.error());
          }
        }

        return EventReply::HANDLED;
      }

      heldThumbColumn.reset();
      auto candidate = pointerValue(mouse);

      if (!candidate) {
        return std::unexpected(candidate.error());
      }

      auto committed = commit(*candidate);

      if (!committed) {
        return std::unexpected(committed.error());
      }

      if (mouse.action == MouseAction::RELEASE) {
        auto done = finish(SliderOperationStatus::COMPLETED, false);

        if (!done) {
          return std::unexpected(done.error());
        }
      }

      return EventReply::HANDLED;
    }

    return EventReply::IGNORED;
  }

  std::expected<EventReply, TreeError> input(Slider &slider, const RoutedEvent &event) {
    if (retired) {
      return EventReply::IGNORED;
    }

    auto allowed = writable();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (!*allowed) {
      auto done = finish(SliderOperationStatus::CANCELLED, false);

      return done ? std::expected<EventReply, TreeError>{EventReply::IGNORED} : std::unexpected(done.error());
    }

    const auto action = sliderAction(event);

    if (action == Action::CANCEL && operation) {
      auto done = finish(SliderOperationStatus::CANCELLED, false);

      return done ? std::expected<EventReply, TreeError>{EventReply::HANDLED} : std::unexpected(done.error());
    }

    const auto *key = std::get_if<KeyEvent>(&event);
    const bool page = key && key->modifiers == InputModifiers{} && key->action != KeyAction::RELEASE &&
                      (key->key == InputKey::PAGE_UP || key->key == InputKey::PAGE_DOWN);
    const bool increasing = action == Action::MOVE_RIGHT || action == Action::MOVE_UP;
    const bool decreasing = action == Action::MOVE_LEFT || action == Action::MOVE_DOWN;

    if (increasing || decreasing || action == Action::MOVE_HOME || action == Action::MOVE_END || page) {
      return step(action.value_or(Action::ACTIVATE), increasing || (page && key->key == InputKey::PAGE_UP) ? 1 : -1,
                  page ? slider.options_.pageSteps : 1);
    }

    const auto *mouse = std::get_if<MouseInputEvent>(&event);

    if (!mouse) {
      return EventReply::IGNORED;
    }

    if (mouse->action == MouseAction::PRESS) {
      return mouse->button == MouseButton::LEFT && mouse->modifiers == InputModifiers{}
               ? press(slider, *mouse)
               : std::expected<EventReply, TreeError>{EventReply::IGNORED};
    }

    if (mouse->action == MouseAction::MOVE ||
        (mouse->action == MouseAction::RELEASE && mouse->button == MouseButton::LEFT)) {
      return continuePointer(*mouse);
    }

    return EventReply::IGNORED;
  }
};

SliderBinding::SliderBinding(std::shared_ptr<State> state, ActionBinding action, Subscription subscription)
    : state_(std::move(state)), action_(std::move(action)), subscription_(std::move(subscription)) {
}

SliderBinding::~SliderBinding() {
  if (state_) {
    state_->retired = true;
    state_->operation.reset();
    state_->capture.disconnect();
  }
}

SliderBinding::SliderBinding(SliderBinding &&other) noexcept = default;

SliderBinding &SliderBinding::operator=(SliderBinding &&other) noexcept {
  if (this != &other) {
    SliderBinding retired(std::move(other));

    using std::swap;

    swap(state_, retired.state_);
    swap(action_, retired.action_);
    swap(subscription_, retired.subscription_);
  }

  return *this;
}

void SliderBinding::retain(std::shared_ptr<void> record) {
  state_->retained = std::move(record);
}

std::shared_ptr<void> SliderBinding::retained() const {
  return state_ ? state_->retained : std::shared_ptr<void>{};
}

std::expected<void, TreeError> SliderBinding::sync() {
  auto state = state_;

  return state ? state->sync() : std::unexpected(TreeError::INVALID_ARGUMENT);
}

std::expected<SliderBinding, TreeError> bindSlider(WidgetTree &tree, EventRouter &router, Node<Slider> node,
                                                   std::shared_ptr<Property<NumericValue>> model,
                                                   std::function<void(SliderResult)> changed,
                                                   std::function<void(const SliderOperationEvent &)> finished) {
  if (!model || !changed) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    auto state = std::make_shared<SliderBinding::State>(tree, router, node, std::move(model), std::move(changed),
                                                        std::move(finished));
    auto connection = tree.connect<SliderOperationEvent>(
      node, [weak = std::weak_ptr(state)](Slider &, std::shared_ptr<const SliderOperationEvent> event) {
        auto owner = weak.lock();

        if (owner && owner->live() && owner->finished) {
          owner->finished(*event);
        }
      });

    if (!connection) {
      return std::unexpected(connection.error());
    }

    state->terminal = std::move(connection->publisher);
    state->terminalSubscription = std::move(connection->subscription);

    auto subscription = state->model->observe(node, [state](Slider &, auto) {
      auto synced = state->sync();

      if (!synced) {
        throw synced.error();
      }
    });

    if (!subscription) {
      return std::unexpected(subscription.error());
    }

    auto action = router.bind(node, [state](Slider &slider, const RoutedEvent &event) {
      auto result = state->input(slider, event);

      if (!result) {
        throw result.error();
      }

      return *result;
    });

    if (!action) {
      return std::unexpected(action.error());
    }

    auto synced = state->sync();

    if (!synced) {
      return std::unexpected(synced.error());
    }

    return SliderBinding(std::move(state), std::move(*action), std::move(*subscription));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}
} // namespace portals
