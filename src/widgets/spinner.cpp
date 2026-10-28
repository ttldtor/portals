// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

#include <algorithm>
#include <limits>

namespace portals::detail {
struct SpinnerDefinition {
  SpinnerOptions options;
  WidthPolicy width;
  std::vector<std::shared_ptr<const GraphemeText>> frames = std::vector<std::shared_ptr<const GraphemeText>>(0);
  std::optional<GraphemeText> label;
  std::int32_t frameWidth{};
  std::optional<TreeError> failure;
  std::optional<TextValueError> textFailure;
};

struct SpinnerState {
  std::shared_ptr<SpinnerDefinition> definition;
  Subscription timer;
  Node<Spinner> node;
  std::size_t frame{};
  std::uint64_t generation{};
  bool eligible{true};
  bool armed{};

  void pause() noexcept {
    if (generation != std::numeric_limits<std::uint64_t>::max()) {
      ++generation;
    }
    armed = false;
    timer = {};
  }
};
} // namespace portals::detail

namespace portals {
namespace {
bool printable(std::string_view value) {
  return std::ranges::none_of(value, [](unsigned char byte) {
    return byte < 0x20 || byte == 0x7f;
  });
}

std::shared_ptr<detail::SpinnerDefinition> compile(WidthPolicy width, SpinnerOptions options,
                                                   const SpinnerLabelResolver &resolver) {
  auto result = std::make_shared<detail::SpinnerDefinition>();
  result->options = std::move(options);
  result->width = width;
  const auto &config = result->options;
  const auto fail = [&](TreeError cause) {
    result->failure = cause;
    return result;
  };
  if (config.preset < SpinnerPreset::DOTS || config.preset > SpinnerPreset::BAR_BOUNCE ||
      config.direction < SpinnerDirection::FORWARD || config.direction > SpinnerDirection::REVERSE ||
      (config.alignment != LayoutAlign::START && config.alignment != LayoutAlign::CENTER &&
       config.alignment != LayoutAlign::END) ||
      !config.interval || !config.maxFrames || !config.maxFrameBytes || !config.maxFrameCells ||
      !config.maxPaintCells) {
    return fail(TreeError::INVALID_ARGUMENT);
  }
  std::string_view sequence = config.frames;
  if (sequence.empty()) {
    switch (config.preset) {
    case SpinnerPreset::DOTS:
      sequence = "o..\n.o.\n..o";
      break;
    case SpinnerPreset::LINE:
      sequence = "|\n/\n-\n\\";
      break;
    case SpinnerPreset::BRAILLE:
      sequence = "\xe2\xa0\x81\n\xe2\xa0\x88\n\xe2\xa0\x80\n\xe2\xa0\x82";
      break;
    case SpinnerPreset::BAR_BOUNCE:
      sequence = "o....\n.o...\n..o..\n...o.\n....o\n...o.\n..o..\n.o...";
      break;
    }
  }
  if (sequence.size() > config.maxFrameBytes) {
    return fail(TreeError::NODE_LIMIT);
  }
  TextLimits limits;
  limits.max_input_bytes = config.maxFrameBytes;
  limits.max_cells = config.maxFrameCells;
  limits.max_graphemes = config.maxFrameCells;
  for (std::size_t begin = 0;;) {
    const auto end = sequence.find('\n', begin);
    const auto bytes = sequence.substr(begin, end == std::string_view::npos ? end : end - begin);
    if (result->frames.size() == config.maxFrames) {
      return fail(TreeError::NODE_LIMIT);
    }
    if (bytes.empty() || !printable(bytes)) {
      return fail(TreeError::INVALID_ARGUMENT);
    }
    auto text = GraphemeText::from(bytes, width, limits);
    if (!text) {
      const bool exhausted =
        text.error().kind == TextErrorKind::CELL_LIMIT || text.error().kind == TextErrorKind::GRAPHEME_LIMIT;
      return fail(exhausted ? TreeError::NODE_LIMIT : TreeError::INVALID_ARGUMENT);
    }
    if (text->measure() == 0) {
      return fail(TreeError::INVALID_ARGUMENT);
    }
    if (config.frames.empty() && config.preset == SpinnerPreset::BRAILLE && text->measure() != 1) {
      text = GraphemeText::from(std::string_view("|/-\\").substr(result->frames.size(), 1), width, limits);
      if (!text) {
        return fail(TreeError::INVALID_ARGUMENT);
      }
    }
    result->frameWidth = std::max(result->frameWidth, text->measure());
    result->frames.push_back(std::make_shared<const GraphemeText>(std::move(*text)));
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  auto label = resolver ? config.label.resolve(width, resolver) : config.label.resolve(width);
  if (!label) {
    result->textFailure = label.error();
    return fail(TreeError::INVALID_ARGUMENT);
  }
  if (!printable(label->bytes())) {
    return fail(TreeError::INVALID_ARGUMENT);
  }
  result->label = std::move(*label);
  if (result->label->measure() > std::numeric_limits<std::int32_t>::max() - result->frameWidth - 1) {
    return fail(TreeError::INVALID_GEOMETRY);
  }
  return result;
}

TreeError translated(SchedulerError cause) {
  switch (cause) {
  case SchedulerError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case SchedulerError::IDENTITY_EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case SchedulerError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  case SchedulerError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case SchedulerError::CLOSED:
    return TreeError::CLOSED;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}
} // namespace

Spinner::Spinner(WidthPolicy width, SpinnerOptions options, ControlStyle style, SizePolicy policy,
                 SpinnerLabelResolver resolver)
    : SizedWidget(policy), state_(std::make_shared<detail::SpinnerState>()), style_(style) {
  state_->definition = compile(width, std::move(options), resolver);
}

Spinner::~Spinner() = default;

std::expected<void, TreeError> Spinner::validate(WidthPolicy width, SpinnerOptions options) try {
  options.label = TextValue("");
  auto prepared = compile(width, std::move(options), {});
  return prepared->failure ? std::unexpected(*prepared->failure) : std::expected<void, TreeError>{};
} catch (const std::bad_alloc &) {
  return std::unexpected(TreeError::NO_MEMORY);
}

std::expected<void, TreeError> Spinner::preparation() const noexcept {
  return state_->definition->failure ? std::unexpected(*state_->definition->failure) : std::expected<void, TreeError>{};
}

std::expected<CellSize, TreeError> Spinner::measure(CellSize) {
  const auto prepared = preparation();
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  const auto &definition = *state_->definition;
  return CellSize{definition.frameWidth + (definition.label->measure() ? definition.label->measure() + 1 : 0), 1};
}

std::expected<void, TreeError> Spinner::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());
  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  bounds_ = *bounds;
  return {};
}

std::expected<void, TreeError> Spinner::paint(PaintContext &context) {
  auto intrinsic = measure(bounds_.size());
  if (!intrinsic) {
    return std::unexpected(intrinsic.error());
  }
  const auto &definition = *state_->definition;
  const auto &config = definition.options;
  if (static_cast<std::uint64_t>(bounds_.size().width) * bounds_.size().height > config.maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  if (bounds_.empty()) {
    return {};
  }
  std::optional<ComputedStyle> manualStyle;
  if (!computedStyle()) {
    manualStyle.emplace(styleDefaults());
  }
  const auto style =
    computedStyle() ? detail::ComputedStyleView(*computedStyle()) : detail::ComputedStyleView(*manualStyle);
  const auto appearance = state_->eligible && config.active && !config.reducedMotion ? style.normal : style.disabled;
  const auto space = std::max(0, bounds_.size().width - intrinsic->width);
  const auto x = config.alignment == LayoutAlign::CENTER ? space / 2 : config.alignment == LayoutAlign::END ? space : 0;
  const auto index = config.reducedMotion ? 0 : state_->frame;
  const std::array<RenderCommand, 3> commands{
    FillCommand{bounds_, bounds_, appearance}, TextCommand{{x, 0}, *definition.frames[index], bounds_, appearance},
    TextCommand{{x + definition.frameWidth + 1, 0}, *definition.label, bounds_, style.normal}};
  return context.submit(commands);
}

ComputedStyle Spinner::styleDefaults() const {
  ComputedStyle result;
  result.normal = style_.normal;
  result.focused = style_.focused;
  result.disabled = style_.disabled;
  return result;
}

std::size_t Spinner::frameIndex() const noexcept {
  return state_->definition->options.reducedMotion ? 0 : state_->frame;
}

SpinnerOptions Spinner::options() const {
  return state_->definition->options;
}

std::expected<void, TreeError> setSpinnerActive(WidgetTree &tree, Node<Spinner> node, bool active) {
  bool changed{};
  auto checked = tree.edit(node, [&](Spinner &spinner) {
    auto &state = *spinner.state_;
    if (state.definition->options.active != active) {
      state.pause();
      state.definition->options.active = active;
      changed = true;
    }
  });
  return checked && changed ? tree.invalidate(node, Invalidation::PAINT) : checked;
}

std::expected<void, TreeError> setSpinner(WidgetTree &tree, Node<Spinner> node, const SpinnerOptions &options,
                                          SpinnerLabelResolver resolver) try {
  WidthPolicy width;
  auto checked = tree.edit(node, [&](Spinner &spinner) {
    width = spinner.state_->definition->width;
  });
  if (!checked) {
    return checked;
  }
  auto staged = compile(width, options, resolver);
  if (staged->failure) {
    return std::unexpected(*staged->failure);
  }
  checked = tree.edit(node, [&](Spinner &spinner) {
    spinner.state_->pause();
    spinner.state_->definition = std::move(staged);
    spinner.state_->frame = 0;
  });
  return checked ? tree.invalidate(node, Invalidation::LAYOUT) : checked;
} catch (const std::bad_alloc &) {
  return std::unexpected(TreeError::NO_MEMORY);
} catch (...) {
  return std::unexpected(TreeError::CALLBACK_FAILED);
}

std::expected<Node<Spinner>, ManagedError> ManagedScene::spinner(Node<Widget> parent, SpinnerOptions options,
                                                                 ControlStyle style, SizePolicy policy,
                                                                 SpinnerLabelResolver resolver) {
  auto allowed = boundary(ManagedPhase::MOUNT, parent);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  const auto lifetime = std::weak_ptr(lifetime_);
  auto created = emplace<Spinner>(parent, widthPolicy(), std::move(options), style, policy, std::move(resolver));
  if (lifetime.expired()) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::CLOSED));
  }
  if (!created) {
    return created;
  }
  std::expected<void, TreeError> prepared;
  std::shared_ptr<detail::SpinnerState> record;
  std::optional<TextValueError> textFailure;
  try {
    auto edited = app_.tree().edit(*created, [&](Spinner &spinner) {
      prepared = spinner.preparation();
      textFailure = spinner.state_->definition->textFailure;
      record = spinner.state_;
    });
    if (!edited) {
      prepared = edited;
    }
    if (prepared && spinners_.size() == spinners_.capacity()) {
      // Stage proxy/storage allocation outside the live owner; allocation hooks may retire it.
      std::vector<std::weak_ptr<detail::SpinnerState>> grown(0);
      if (lifetime.expired()) {
        return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::CLOSED));
      }
      constexpr std::size_t INITIAL_CAPACITY = 4;
      const auto capacity = spinners_.capacity();
      grown.reserve(capacity > grown.max_size() / 2 ? grown.max_size() : std::max(INITIAL_CAPACITY, capacity * 2));
      if (lifetime.expired()) {
        return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::CLOSED));
      }
      grown.insert(grown.end(), spinners_.begin(), spinners_.end());
      spinners_.swap(grown);
    }
  } catch (const std::bad_alloc &) {
    prepared = std::unexpected(TreeError::NO_MEMORY);
  }
  if (lifetime.expired()) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::CLOSED));
  }
  if (prepared) {
    prepared = app_.tree().withScope(*created, [](const UiScope &) {
    });
    if (closed_) {
      prepared = std::unexpected(TreeError::CLOSED);
    }
  }
  if (!prepared) {
    (void)remove(*created);
    auto failure = error(ManagedPhase::MOUNT, parent, prepared.error());
    if (textFailure) {
      if (const auto *cause = std::get_if<TextError>(&*textFailure)) {
        failure.text = *cause;
      } else {
        failure.resource = std::get<TextResourceError>(*textFailure);
      }
    }
    return std::unexpected(failure);
  }
  record->node = *created;
  spinners_.push_back(record); // Capacity was staged before publication; weak copies do not allocate.
  return created;
}

bool ManagedScene::spinnerEligible(Node<Spinner> node) const {
  auto admitted = app_.tree().inputEligible(node);
  auto geometry = app_.tree().observeGeometry(node);
  return admitted && *admitted && geometry && geometry->geometry.valid && !geometry->geometry.clip.empty();
}

std::expected<void, TreeError> ManagedScene::armSpinner(const std::shared_ptr<detail::SpinnerState> &record) {
  if (record->armed) {
    return {};
  }
  const auto &config = record->definition->options;
  const auto now = app_.now();
  if (record->generation == std::numeric_limits<std::uint64_t>::max() ||
      config.interval > std::numeric_limits<SchedulerTime>::max() - now) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  const auto generation = ++record->generation;
  const auto lifetime = std::weak_ptr(lifetime_);
  std::expected<Subscription, SchedulerError> reserved = std::unexpected(SchedulerError::CLOSED);
  auto checked = app_.tree().withScope(record->node, [&](const UiScope &scope) {
    reserved =
      app_.scheduler().at(scope, now + config.interval,
                          [this, weak = std::weak_ptr(record), lifetime = std::weak_ptr(lifetime_), generation] {
                            if (lifetime.expired()) {
                              return;
                            }
                            if (auto entered = weak.lock()) {
                              tickSpinner(entered, generation);
                            }
                          });
  });
  if (lifetime.expired()) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (!checked) {
    return checked;
  }
  if (!reserved) {
    return std::unexpected(translated(reserved.error()));
  }
  // Scheduler registration may allocate and reenter; never publish a stale timer.
  if (closed_ || generation != record->generation || !spinnerEligible(record->node)) {
    return {};
  }
  record->timer = std::move(*reserved);
  record->armed = true;
  return {};
}

void ManagedScene::tickSpinner(const std::shared_ptr<detail::SpinnerState> &record, std::uint64_t generation) {
  if (closed_ || generation != record->generation) {
    return;
  }
  const auto &definition = *record->definition;
  if (!record->eligible || !definition.options.active || definition.options.reducedMotion ||
      !spinnerEligible(record->node)) {
    record->eligible = false;
    record->pause();
    return;
  }
  record->armed = false;
  record->timer = {};
  const auto count = definition.frames.size();
  record->frame = definition.options.direction == SpinnerDirection::FORWARD ? (record->frame + 1) % count
                                                                            : (record->frame + count - 1) % count;
  const auto lifetime = std::weak_ptr(lifetime_);
  auto invalidated = app_.tree().invalidate(record->node, Invalidation::PAINT);
  if (invalidated) {
    invalidated = armSpinner(record);
  }
  if (!lifetime.expired() && !invalidated) {
    remember(error(ManagedPhase::PREPARE, record->node, invalidated.error()));
  }
}

void ManagedScene::stopSpinners() noexcept {
  for (const auto &weak : spinners_) {
    if (auto record = weak.lock()) {
      record->pause();
    }
  }
  spinners_.clear();
}

std::expected<void, TreeError> ManagedScene::prepareSpinners() {
  std::erase_if(spinners_, [](const auto &value) {
    return value.expired();
  });
  for (const auto &weak : spinners_) {
    auto record = weak.lock();
    if (!record) {
      continue;
    }
    const auto &config = record->definition->options;
    const auto admitted = spinnerEligible(record->node);
    record->eligible = admitted;
    if (!admitted || !config.active || config.reducedMotion || record->definition->frames.size() == 1) {
      record->pause();
      continue;
    }
    auto armed = armSpinner(record);
    if (!armed) {
      remember(error(ManagedPhase::PREPARE, record->node, armed.error()));
      return armed;
    }
  }
  return {};
}
} // namespace portals
