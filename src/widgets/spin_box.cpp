// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "line_edit_source.hpp"
#include "spin_box_source.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <portals/spin_box.hpp>

namespace portals {
std::expected<double, NumericError> detail::parseSpinBoxText(std::string_view text) noexcept {
  if (text.empty()) {
    return std::unexpected(NumericError::INVALID_TEXT);
  }
  // from_chars deliberately excludes a leading plus; decimal input accepts it.
  if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty() || text.front() == '-' || text.front() == '+') {
      return std::unexpected(NumericError::INVALID_TEXT);
    }
  }
  if (text.empty()) {
    return std::unexpected(NumericError::INVALID_TEXT);
  }
  double value{};
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  if (parsed.ptr != text.data() + text.size()) {
    return std::unexpected(NumericError::INVALID_TEXT);
  }
  if (parsed.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
    return std::unexpected(NumericError::NOT_FINITE);
  }
  if (parsed.ec != std::errc{}) {
    return std::unexpected(NumericError::INVALID_TEXT);
  }
  return value;
}

namespace {
constexpr std::size_t NUMERIC_TEXT_CAPACITY = 128;

std::string formatNumber(double value) {
  std::array<char, NUMERIC_TEXT_CAPACITY> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    throw TreeError::INVALID_ARGUMENT;
  }
  return {buffer.data(), result.ptr};
}

// Each operation owns its carrier, even for equal sets. Upcasting shared_ptr retains
// the concrete deleter; extraction is used only for receipts produced by this source.
struct NumericCarrier : PropertyChange<std::string> {
  PropertyUpdate<NumericValue> numeric;

  explicit NumericCarrier(double value) : PropertyChange<std::string>{formatNumber(value), 0} {
  }
};

struct NumericDraftSource final : detail::LineEditSource {
  std::shared_ptr<Property<NumericValue>> model;
  std::shared_ptr<const PropertyChange<std::string>> cache;
  std::uint64_t pageSteps;

  NumericDraftSource(std::shared_ptr<Property<NumericValue>> source, std::uint64_t pages)
      : model(std::move(source)), pageSteps(pages) {
  }

  std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> snapshot() override {
    auto current = model->snapshot();
    if (!current) {
      return std::unexpected(current.error());
    }
    if (!cache || cache->revision != (*current)->revision) {
      auto next = std::make_shared<NumericCarrier>((*current)->value.value());
      next->revision = (*current)->revision;
      cache = std::move(next);
    }
    return cache;
  }

  bool valid(std::string_view text) {
    auto number = detail::parseSpinBoxText(text);
    auto current = model->snapshot();
    return number && current && (*current)->value.withValue(*number).has_value();
  }

  std::expected<PropertyUpdate<std::string>, TreeError> set(const std::string &text) override {
    auto number = detail::parseSpinBoxText(text);
    auto current = model->snapshot();
    if (!current) {
      return std::unexpected(current.error());
    }
    auto candidate = number ? (*current)->value.withValue(*number)
                            : std::expected<NumericValue, NumericError>(std::unexpected(NumericError::NOT_FINITE));
    if (!candidate) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    // Complete all text/control-block allocation before committing the numeric source.
    auto carrier = std::make_shared<NumericCarrier>(candidate->value());
    std::shared_ptr<const PropertyChange<std::string>> projection = carrier;
    auto updated = model->set(*candidate);
    if (!updated) {
      return std::unexpected(updated.error());
    }
    carrier->revision = updated->snapshot->revision;
    carrier->numeric = *updated;
    // A destructor during Property publication may reenter and observe a newer revision.
    if (!cache || cache->revision <= carrier->revision) {
      cache = projection;
    }
    return PropertyUpdate<std::string>{std::move(projection), updated->changed,   updated->accepted,
                                       updated->rejected,     updated->cancelled, updated->notification_error};
  }

  std::expected<Subscription, TreeError> observe(Node<LineEdit> node, std::function<void()> callback) override {
    return model->observe(node, [callback = std::move(callback)](LineEdit &, auto) {
      callback();
    });
  }

  bool hasSteps() const noexcept override {
    return true;
  }

  std::expected<std::optional<std::string>, TreeError> step(std::string_view text, bool dirty, int direction,
                                                            bool page) override {
    auto current = model->snapshot();
    if (!current) {
      return std::unexpected(current.error());
    }
    auto starting = (*current)->value;
    if (dirty) {
      auto parsed = detail::parseSpinBoxText(text);
      if (!parsed) {
        return std::optional<std::string>{};
      }
      auto candidate = starting.withValue(*parsed);
      if (!candidate) {
        return std::optional<std::string>{};
      }
      starting = *candidate;
    }
    return std::optional<std::string>(formatNumber(detail::nudgeNumeric(starting, direction, page ? pageSteps : 1)));
  }

  static SpinBoxResult result(LineEditResult text) {
    SpinBoxResult result{std::move(text), {}};
    if (result.editing.commit) {
      const auto carrier = std::static_pointer_cast<const NumericCarrier>(result.editing.commit->snapshot);
      result.commit = carrier->numeric;
    }
    return result;
  }
};
} // namespace

struct SpinBoxBinding::State {
  std::shared_ptr<NumericDraftSource> source;
  std::shared_ptr<LineEditBinding> editor;
  ActionBinding decrease;
  ActionBinding increase;
  std::shared_ptr<void> record;
  WidthPolicy width;
  TextLimits limits;
  std::array<Node<Widget>, 3> parts;
};

SpinBoxBinding::SpinBoxBinding(std::shared_ptr<State> state) : state_(std::move(state)) {
}

SpinBoxBinding::~SpinBoxBinding() {
  auto state = std::move(state_);
  if (state) {
    state->decrease = ActionBinding{};
    state->increase = ActionBinding{};
    // The child entry may also hold this token. Retire its registrations now;
    // admitted editor callbacks already keep their own State alive.
    LineEditBinding retired(std::move(*state->editor));
  }
}

SpinBoxBinding::SpinBoxBinding(SpinBoxBinding &&) noexcept = default;

SpinBoxBinding &SpinBoxBinding::operator=(SpinBoxBinding &&other) noexcept {
  if (this != &other) {
    SpinBoxBinding retired(std::move(other));
    state_.swap(retired.state_);
  }
  return *this;
}

std::shared_ptr<LineEditBinding> SpinBoxBinding::editorBinding() const {
  return state_ ? state_->editor : nullptr;
}

bool SpinBoxBinding::ownsPart(Node<Widget> node) const noexcept {
  auto state = state_;

  return state && std::ranges::find(state->parts, node) != state->parts.end();
}

void SpinBoxBinding::retain(std::shared_ptr<void> record) {
  state_->record = std::move(record);
}

std::shared_ptr<void> SpinBoxBinding::retained() const {
  return state_ ? state_->record : nullptr;
}

std::expected<void, TreeError> SpinBoxBinding::sync(Node<Widget> focused) {
  auto state = state_;
  return state ? state->editor->sync(focused) : std::unexpected(TreeError::INVALID_ARGUMENT);
}

std::expected<SpinBoxResult, TreeError> SpinBoxBinding::setValue(double value) {
  auto state = state_;
  if (!state) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  try {
    auto current = state->source->model->snapshot();
    if (!current) {
      return std::unexpected(current.error());
    }
    auto candidate = (*current)->value.withValue(value);
    if (!candidate) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto text = GraphemeText::from(formatNumber(value), state->width, state->limits);
    if (!text) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto receipt = detail::LineEditSourceAccess::set(*state->editor, std::move(*text));
    return receipt ? std::expected<SpinBoxResult, TreeError>(NumericDraftSource::result(std::move(*receipt)))
                   : std::unexpected(receipt.error());
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<SpinBoxBinding, TreeError> bindSpinBox(WidgetTree &tree, EventRouter &router, Node<LineEdit> editor,
                                                     Node<Button> decrease, Node<Button> increase,
                                                     std::shared_ptr<Property<NumericValue>> model,
                                                     std::uint64_t pageSteps, std::function<void(SpinBoxResult)> result,
                                                     LineEditEvents events) {
  if (!model || !result || !pageSteps) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  try {
    auto source = std::make_shared<NumericDraftSource>(std::move(model), pageSteps);
    auto bound = detail::LineEditSourceAccess::bind(
      tree, router, editor, source,
      [source, result = std::move(result)](LineEditResult receipt) {
        result(NumericDraftSource::result(std::move(receipt)));
      },
      [source](std::string_view candidate) {
        return source->valid(candidate);
      },
      std::move(events));
    if (!bound) {
      return std::unexpected(bound.error());
    }
    LineEditOptions options;
    auto read = tree.read(editor, [&](const LineEdit &edit) {
      options = detail::LineEditSourceAccess::options(edit);
    });
    if (!read) {
      return std::unexpected(read.error());
    }
    auto state = std::make_shared<SpinBoxBinding::State>(
      SpinBoxBinding::State{source,
                            std::make_shared<LineEditBinding>(std::move(*bound)),
                            {},
                            {},
                            {},
                            options.widthPolicy,
                            options.limits,
                            {editor, decrease, increase}});
    const auto arrow = [&](Node<Button> node, int direction) {
      return router.bind(
        node,
        [weak = std::weak_ptr(state), direction](Button &, const RoutedEvent &event) {
          const auto *command = std::get_if<ActionEvent>(&event);
          const auto *mouse = std::get_if<MouseInputEvent>(&event);
          const bool activated = (command && command->action == Action::ACTIVATE &&
                                  command->keyAction != KeyAction::RELEASE && command->modifiers == InputModifiers{}) ||
                                 (mouse && mouse->action == MouseAction::PRESS && mouse->button == MouseButton::LEFT &&
                                  mouse->modifiers == InputModifiers{});
          if (!activated) {
            return EventReply::IGNORED;
          }
          auto current = weak.lock();
          if (!current) {
            return EventReply::IGNORED;
          }
          auto stepped = detail::LineEditSourceAccess::step(*current->editor, direction);
          if (!stepped) {
            throw stepped.error();
          }
          return EventReply::HANDLED;
        },
        false);
    };
    auto down = arrow(decrease, -1);
    if (!down) {
      return std::unexpected(down.error());
    }
    state->decrease = std::move(*down);
    auto up = arrow(increase, 1);
    if (!up) {
      return std::unexpected(up.error());
    }
    state->increase = std::move(*up);
    return SpinBoxBinding(std::move(state));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals
