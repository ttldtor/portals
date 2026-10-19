// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <limits>
#include <portals/managed.hpp>

namespace portals::detail {
/// @cond INTERNAL
// A fixed cell selects owning validated text without invoking Label's allocating LAYOUT setter per tick.
class DataStateIndicator final : public SizedWidget {
public:
  DataStateIndicator(std::array<std::shared_ptr<const GraphemeText>, 4> frames, ControlStyle style)
      : frames_(std::move(frames)), style_(style) {
  }

  std::expected<CellSize, TreeError> measure(CellSize) override {
    return CellSize{1, 1};
  }

  std::expected<void, TreeError> arrange(CellRect local) override {
    auto bounds = CellRect::from({}, local.size());
    if (!bounds) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    bounds_ = *bounds;
    return {};
  }

  std::expected<void, TreeError> paint(PaintContext &context) override {
    const auto resolved = computedStyle() ? *computedStyle() : styleDefaults();
    const auto style = enabled ? resolved.normal : resolved.disabled;
    const std::array<RenderCommand, 2> commands{FillCommand{bounds_, bounds_, style},
                                                TextCommand{{}, *frames_[frame], bounds_, style}};
    return context.submit(commands);
  }

  ComputedStyle styleDefaults() const override {
    ComputedStyle value;
    value.normal = style_.normal;
    value.focused = style_.focused;
    value.disabled = style_.disabled;
    return value;
  }

  std::size_t frame{};
  bool enabled{true};

private:
  std::array<std::shared_ptr<const GraphemeText>, 4> frames_;
  ControlStyle style_;
  CellRect bounds_;
};

/// @endcond
} // namespace portals::detail

namespace portals {
namespace {
constexpr std::array<const char *, 6> PHASE_NAMES{"ready", "loading", "empty", "error", "offline", "refreshing"};

constexpr bool retired(TreeError cause) noexcept {
  return cause == TreeError::CLOSED || cause == TreeError::STALE_NODE;
}

constexpr bool valid(DataPresentation value) noexcept {
  return static_cast<std::size_t>(value.phase) < PHASE_NAMES.size() && value.generation != 0;
}

constexpr bool retryPhase(DataPhase phase) noexcept {
  return phase == DataPhase::FAILED || phase == DataPhase::OFFLINE;
}

void stopTimer(detail::DataStateRecord &record) noexcept {
  // Never reuse a captured epoch, including during disposal/destructor reentry.
  if (record.timerGeneration != std::numeric_limits<std::uint64_t>::max()) {
    ++record.timerGeneration;
  }
  record.timerArmed = false;
  record.timer = {};
}
} // namespace

std::expected<void, TreeError> setDataStateView(WidgetTree &tree, Node<DataStateView> node, DataPresentation value) {
  std::shared_ptr<detail::DataStateRecord> record;
  auto checked = tree.edit(node, [&](DataStateView &view) {
    record = view.record_.lock();
  });
  if (!checked) {
    return checked;
  }
  if (!record || !record->mounted || record->mode != detail::DataStateMode::OWNED) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  // The owning admission keeps the callable alive through low-level removal.
  return record->display ? record->display(value) : std::unexpected(TreeError::CLOSED);
}

bool ManagedScene::dataStateMountPending() const noexcept {
  return std::ranges::any_of(dataStates_, [](const auto &record) {
    return !record->mounted && !record->retired;
  });
}

std::expected<Node<DataStateView>, ManagedError>
ManagedScene::beginDataState(Node<Widget> parent, DataPresentation initial, DataStateConfig config, bool automatic) {
  auto allowed = boundary(ManagedPhase::MOUNT, parent);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  const auto fail = [&](TreeError cause) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, cause));
  };
  if (!valid(initial) || config.statusRows <= 0 || !config.spinnerInterval ||
      (config.contentPolicy != DataContentPolicy::KEEP_BODY && config.contentPolicy != DataContentPolicy::REPLACE) ||
      (automatic && config.contentPolicy != DataContentPolicy::KEEP_BODY) || (!config.showStatus && config.retry)) {
    return fail(TreeError::INVALID_ARGUMENT);
  }
  const auto textFailure = [&](const TextValueError &issue) {
    auto failure = error(ManagedPhase::MOUNT, parent, TreeError::INVALID_ARGUMENT);
    if (const auto *cause = std::get_if<TextError>(&issue)) {
      failure.text = *cause;
      if (cause->kind == TextErrorKind::INPUT_LIMIT || cause->kind == TextErrorKind::GRAPHEME_LIMIT ||
          cause->kind == TextErrorKind::CLUSTER_LIMIT || cause->kind == TextErrorKind::CELL_LIMIT) {
        failure.cause = TreeError::NODE_LIMIT;
      }
    } else {
      failure.resource = std::get<TextResourceError>(issue);
    }
    return std::unexpected(failure);
  };
  Node<DataStateView> created;
  try {
    auto record = std::make_shared<detail::DataStateRecord>();
    record->config = std::move(config);
    record->current = initial;
    record->mode = automatic ? detail::DataStateMode::AUTOMATIC : detail::DataStateMode::OWNED;
    if (record->config.retry) {
      record->action = std::make_shared<std::function<std::expected<void, TreeError>()>>();
      record->action->swap(record->config.retry);
    }
    if (record->config.showStatus) {
      auto &labels = record->config.labels;
      const std::array<const TextValue *, 6> expressions{nullptr,       &labels.loading, &labels.empty,
                                                         &labels.error, &labels.offline, &labels.refreshing};
      for (std::size_t i = 0; i < expressions.size(); ++i) {
        TextValue empty("");
        const auto &expression = expressions[i] ? *expressions[i] : empty;
        auto text = record->config.resolver ? expression.resolve(widthPolicy(), record->config.resolver)
                                            : expression.resolve(widthPolicy());
        if (!text) {
          return textFailure(text.error());
        }
        record->messages[i] = std::make_shared<const GraphemeText>(std::move(*text));
      }
      if (record->action) {
        auto caption = record->config.resolver ? labels.retry.resolve(widthPolicy(), record->config.resolver)
                                               : labels.retry.resolve(widthPolicy());
        if (!caption) {
          return textFailure(caption.error());
        }
        record->retryText = std::make_shared<const GraphemeText>(std::move(*caption));
      }
      constexpr std::array<std::string_view, 4> GLYPHS{"|", "/", "-", "\\"};
      for (std::size_t i = 0; i < GLYPHS.size(); ++i) {
        auto glyph = GraphemeText::from(GLYPHS[i], widthPolicy());
        if (!glyph || glyph->graphemes().size() != 1 || glyph->measure() != 1) {
          return fail(TreeError::INVALID_ARGUMENT);
        }
        record->frames[i] = std::make_shared<const GraphemeText>(std::move(*glyph));
      }
    }
    record->config.resolver = {};
    // Resolver/callback copies may reenter before any composite identity exists.
    allowed = boundary(ManagedPhase::MOUNT, parent);
    if (!allowed) {
      return std::unexpected(allowed.error());
    }
    if (!dataStateOwner_) {
      auto owner = std::make_shared<detail::DataStateOwner>();
      owner->scene = this;
      owner->lifetime = lifetime_;
      dataStateOwner_ = std::move(owner);
    }
    record->owner = dataStateOwner_;
    auto node = emplace<DataStateView>(parent, record->config.policy);
    if (!node) {
      return std::unexpected(node.error());
    }
    created = *node;
    record->node = *node;
    // Reserve before publishing the record, and recheck after the allocation boundary.
    dataStates_.reserve(dataStates_.size() + 1);
    auto live = app_.tree().withScope(*node, [](const UiScope &) {
    });
    if (!live || closed_) {
      throw live ? TreeError::CLOSED : live.error();
    }
    record->display = [weak = std::weak_ptr(record)](DataPresentation value) -> std::expected<void, TreeError> {
      auto entered = weak.lock();
      auto owner = entered ? entered->owner.lock() : nullptr;
      if (!owner || owner->lifetime.expired() || !owner->scene || entered->retired) {
        return std::unexpected(TreeError::CLOSED);
      }
      return owner->scene->applyDataState(entered, value);
    };
    if (record->config.showStatus) {
      SizePolicy stripPolicy;
      stripPolicy.height = {record->config.statusRows, record->config.statusRows, record->config.statusRows,
                            SizeMode::FIXED};
      auto strip = emplace<Row>(*node, LinearOptions{}, stripPolicy);
      if (!strip) {
        throw strip.error();
      }
      record->strip = *strip;
      auto indicator = emplace<detail::DataStateIndicator>(*strip, record->frames, record->config.messageStyle);
      if (!indicator) {
        throw indicator.error();
      }
      record->indicator = *indicator;
      SizePolicy messagePolicy;
      messagePolicy.width.mode = SizeMode::FLEX;
      auto message = emplace<Label>(*strip, *record->messages[static_cast<std::size_t>(initial.phase)],
                                    record->config.messageStyle, messagePolicy);
      if (!message) {
        throw message.error();
      }
      record->message = *message;
      if (record->action) {
        auto retry = emplace<Button>(*strip, *record->retryText, record->config.retryStyle);
        if (!retry) {
          throw retry.error();
        }
        record->retryButton = *retry;
        auto connected = onClick(*retry, [weak = std::weak_ptr(record)]() -> std::expected<void, TreeError> {
          auto entered = weak.lock();
          auto owner = entered ? entered->owner.lock() : nullptr;
          if (!owner || owner->lifetime.expired() || !owner->scene || entered->retired || entered->retrying ||
              !retryPhase(entered->current.phase)) {
            return {};
          }
          auto action = entered->action;
          if (!action) {
            return {};
          }
          entered->retrying = true;
          try {
            auto result = (*action)();
            if (!result) {
              entered->retrying = false;
            }
            return result;
          } catch (...) {
            entered->retrying = false;
            throw;
          }
        });
        if (!connected) {
          throw connected.error();
        }
      }
      const std::array<std::pair<Node<Widget>, const char *>, 4> targets{{{*strip, "data-status"},
                                                                          {*indicator, "data-spinner"},
                                                                          {*message, "data-message"},
                                                                          {record->retryButton, "data-retry"}}};
      constexpr std::array<const char *, 4> TYPES{"Row", "DataStateIndicator", "Label", "Button"};
      for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto &[target, role] = targets[i];
        if (target == Node<Widget>{}) {
          continue;
        }
        StyleTarget style;
        style.type = TYPES[i];
        style.classes.emplace_back(role);
        auto styled = this->style(target, style);
        if (!styled) {
          throw styled.error();
        }
      }
      auto hidden = app_.tree().setVisible(*indicator, isDataPending(initial.phase));
      if (!hidden) {
        throw hidden.error();
      }
      if (record->retryButton != Node<Button>{}) {
        hidden = app_.tree().setVisible(record->retryButton, retryPhase(initial.phase));
        if (!hidden) {
          throw hidden.error();
        }
      }
    }
    StyleTarget rootStyle;
    rootStyle.type = "DataStateView";
    auto styled = style(*node, rootStyle);
    if (!styled) {
      throw styled.error();
    }
    // A user callback copied during child setup may have mounted another composite.
    dataStates_.reserve(dataStates_.size() + 1);
    live = app_.tree().withScope(*node, [](const UiScope &) {
    });
    if (!live || closed_) {
      throw live ? TreeError::CLOSED : live.error();
    }
    auto edited = app_.tree().edit(*node, [&](DataStateView &view) {
      view.record_ = record;
      view.presentation_ = initial;
    });
    if (!edited) {
      throw edited.error();
    }
    dataStates_.push_back(std::move(record));
    return node;
  } catch (ManagedError failure) {
    if (created != Node<DataStateView>{}) {
      auto removed = remove(created);
      if (!removed) {
        failure.cleanup = std::get<TreeError>(removed.error().cause);
      }
    }
    return std::unexpected(failure);
  } catch (TreeError cause) {
    if (created != Node<DataStateView>{}) {
      (void)remove(created);
    }
    return fail(cause);
  } catch (const std::bad_alloc &) {
    if (created != Node<DataStateView>{}) {
      (void)remove(created);
    }
    return fail(TreeError::NO_MEMORY);
  } catch (...) {
    if (created != Node<DataStateView>{}) {
      (void)remove(created);
    }
    return fail(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, TreeError> ManagedScene::applyDataState(const std::shared_ptr<detail::DataStateRecord> &record,
                                                            DataPresentation value) {
  auto live = app_.tree().withScope(record->node, [](const UiScope &) {
  });
  if (!live) {
    return std::unexpected(live.error());
  }
  if (closed_ || record->retired) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (record->applying || reporting_ || styling_ || pruningObservers_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  const auto before = record->revision;
  const std::array<Node<Widget>, 6> identities{record->node,    record->strip,       record->indicator,
                                               record->message, record->retryButton, record->body};
  const auto preflight = [&]() -> std::expected<void, TreeError> {
    for (auto node : identities) {
      if (node == Node<Widget>{}) {
        continue;
      }
      auto checked = app_.tree().withScope(node, [](const UiScope &) {
      });
      if (!checked) {
        return std::unexpected(checked.error());
      }
    }
    if (closed_ || record->retired) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (record->revision != before) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    return {};
  };
  auto complete = preflight();
  if (!complete) {
    return complete;
  }
  if (!valid(value)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (value.generation < record->current.generation || (record->presented && value == record->current)) {
    return {};
  }

  struct Applying {
    bool &value;

    ~Applying() {
      value = false;
    }
  } guard{record->applying};

  record->applying = true;
  try {
    using Staged = std::pair<std::shared_ptr<Entry>, std::shared_ptr<StyleTarget>>;
    std::vector<Staged> targets(0);
    const bool changed = value != record->current;
    const std::array<Node<Widget>, 5> styledNodes{record->node, record->strip, record->indicator, record->message,
                                                  record->retryButton};
    for (auto node : styledNodes) {
      if (node == Node<Widget>{}) {
        continue;
      }
      auto entry = find(node);
      if (!entry || !entry->target) {
        return std::unexpected(TreeError::STALE_NODE);
      }
      const auto *name = PHASE_NAMES[static_cast<std::size_t>(value.phase)];
      if (std::ranges::contains(entry->target->classes, name)) {
        continue;
      }
      auto next = std::make_shared<StyleTarget>(*entry->target);
      const auto keepClass = [](const std::string &item) {
        return !std::ranges::contains(PHASE_NAMES, item);
      };
      std::vector<std::string> classes(0);
      const auto retained = static_cast<std::size_t>(std::ranges::count_if(next->classes, keepClass));

      if (retained == classes.max_size()) {
        return std::unexpected(TreeError::NODE_LIMIT);
      }

      // MSVC Debug string moves allocate inside noexcept. Copy into empty reserved storage
      // so every injected allocation failure reaches this operation's checked boundary.
      classes.reserve(retained + 1);

      for (const auto &item : next->classes) {
        if (keepClass(item)) {
          classes.emplace_back(item);
        }
      }

      classes.emplace_back(name);
      next->classes.swap(classes);
      targets.emplace_back(entry, std::move(next));
    }

    // Label owns no decorations. Prepare both transfer destinations before the final identity check;
    // passing a GraphemeText by value to the general setter would open another allocation window.
    static_assert(std::is_nothrow_move_assignable_v<GraphemeText>);
    std::optional<GraphemeText> text;
    std::optional<GraphemeText> retiredText;
    if (record->config.showStatus) {
      text.emplace(*record->messages[static_cast<std::size_t>(value.phase)]);
      retiredText.emplace(*record->messages[static_cast<std::size_t>(DataPhase::READY)]);
    }
    if (record->revision == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    Subscription reserved;
    bool reservedTimer{};
    if (record->mounted && record->eligible && isDataPending(value.phase) && !record->timerArmed) {
      auto staged = reserveDataTimer(record, value.phase, reserved);
      if (!staged) {
        return std::unexpected(staged.error());
      }
      reservedTimer = *staged;
    }

    const auto publish = [&](DataStateView &view, Label *message) -> std::expected<void, TreeError> {
      // Both root/message borrows are active. No allocation or extension callback follows this check;
      // nested edits cannot reclaim a removed widget between individual publication steps.
      auto checked = preflight();
      if (!checked) {
        return checked;
      }
      if (message &&
          (message->text_.policy() != text->policy() || !message->prefix_.bytes().empty() ||
           !message->checked_prefix_.bytes().empty() || message->mixed_prefix_ || !message->suffix_.bytes().empty())) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      const bool textChanged = message && message->text_.bytes() != text->bytes();
      if (textChanged) {
        *retiredText = std::move(message->text_);
        message->text_ = std::move(*text);
      }
      for (auto &[entry, target] : targets) {
        entry->target.swap(target);
        entry->stylePending = true;
        stylesDirty_ = true;
      }
      // The checked identities remain live throughout these nonallocating built-in mutations.
      if (record->config.showStatus) {
        auto shown = app_.tree().setVisible(record->indicator, isDataPending(value.phase));
        if (!shown) {
          return shown;
        }
        if (record->retryButton != Node<Button>{}) {
          shown = app_.tree().setVisible(record->retryButton, retryPhase(value.phase));
          if (!shown) {
            return shown;
          }
        }
      }
      if (record->config.contentPolicy == DataContentPolicy::REPLACE) {
        auto shown =
          app_.tree().setVisible(record->body, value.phase == DataPhase::READY || value.phase == DataPhase::REFRESHING);
        if (!shown) {
          return shown;
        }
      }
      if (changed) {
        record->retrying = false;
      }
      record->current = value;
      record->presented = true;
      ++record->revision;
      record->focusDirty = record->focusDirty || changed;
      view.presentation_ = value;
      if (!isDataPending(value.phase)) {
        stopTimer(*record);
      } else if (reservedTimer) {
        auto eligible = app_.tree().inputEligible(record->node);
        if (eligible && *eligible && record->eligible && !record->retired && !closed_) {
          record->timer = std::move(reserved);
          record->timerArmed = true;
        }
      }
      if (textChanged || !targets.empty()) {
        return app_.tree().invalidate(record->node, Invalidation::LAYOUT);
      }
      return {};
    };

    std::expected<void, TreeError> committed;
    auto edited = app_.tree().edit(record->node, [&](DataStateView &view) {
      if (record->config.showStatus) {
        auto message = app_.tree().edit(record->message, [&](Label &label) {
          committed = publish(view, &label);
        });
        if (!message) {
          committed = std::unexpected(message.error());
        }
      } else {
        committed = publish(view, nullptr);
      }
    });
    return edited ? committed : edited;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<bool, TreeError> ManagedScene::reserveDataTimer(const std::shared_ptr<detail::DataStateRecord> &record,
                                                              DataPhase phase, Subscription &reserved) {
  if (!record->eligible || !isDataPending(phase) || record->timerArmed) {
    return false;
  }
  if (record->timerGeneration == std::numeric_limits<std::uint64_t>::max() ||
      app_.now() > std::numeric_limits<SchedulerTime>::max() - record->config.spinnerInterval) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  const auto revision = record->revision;
  const auto generation = ++record->timerGeneration;
  std::expected<Subscription, SchedulerError> timer = std::unexpected(SchedulerError::CLOSED);
  auto checked = app_.tree().withScope(record->node, [&](const UiScope &scope) {
    timer = app_.scheduler().at(scope, app_.now() + record->config.spinnerInterval,
                                [weak = std::weak_ptr(record), generation] {
                                  auto entered = weak.lock();
                                  auto owner = entered ? entered->owner.lock() : nullptr;
                                  if (!owner || owner->lifetime.expired() || !owner->scene || entered->retired) {
                                    return;
                                  }
                                  owner->scene->tickDataState(entered, generation);
                                });
  });
  if (!checked) {
    return std::unexpected(checked.error());
  }
  if (!timer) {
    if (timer.error() == SchedulerError::CLOSED) {
      return false;
    }
    const auto cause = timer.error() == SchedulerError::NO_MEMORY            ? TreeError::NO_MEMORY
                       : timer.error() == SchedulerError::IDENTITY_EXHAUSTED ? TreeError::IDENTITY_EXHAUSTED
                       : timer.error() == SchedulerError::WOULD_BLOCK        ? TreeError::WOULD_BLOCK
                                                                             : TreeError::INVALID_ARGUMENT;
    return std::unexpected(cause);
  }
  // Registration can allocate. Recheck generation, visibility and identity before publishing the token.
  auto eligible = app_.tree().inputEligible(record->node);
  if (!eligible || !*eligible || record->retired || closed_ || generation != record->timerGeneration ||
      revision != record->revision || !record->eligible || !isDataPending(phase)) {
    return !eligible && !retired(eligible.error()) ? std::expected<bool, TreeError>(std::unexpected(eligible.error()))
                                                   : std::expected<bool, TreeError>(false);
  }
  reserved = std::move(*timer);
  return true;
}

std::expected<void, TreeError> ManagedScene::animateDataState(const std::shared_ptr<detail::DataStateRecord> &record) {
  Subscription reserved;
  auto armed = reserveDataTimer(record, record->current.phase, reserved);
  if (!armed) {
    return std::unexpected(armed.error());
  }
  if (*armed) {
    record->timer = std::move(reserved);
    record->timerArmed = true;
  }
  return {};
}

void ManagedScene::tickDataState(const std::shared_ptr<detail::DataStateRecord> &record, std::uint64_t generation) {
  if (owner_ != std::this_thread::get_id() || closed_ || record->retired || generation != record->timerGeneration) {
    return;
  }
  auto eligible = app_.tree().inputEligible(record->node);
  auto geometry = app_.tree().observeGeometry(record->indicator);
  if (!eligible || !*eligible || !geometry || !geometry->geometry.valid || geometry->geometry.clip.size().width <= 0 ||
      geometry->geometry.clip.size().height <= 0 || !record->eligible || !isDataPending(record->current.phase)) {
    stopTimer(*record);
    return;
  }
  record->timerArmed = false;
  record->timer = {};
  const auto next = (record->frame + 1) % record->frames.size();
  auto edited = app_.tree().edit(record->indicator, [&](detail::DataStateIndicator &indicator) {
    indicator.frame = next;
  });
  if (edited) {
    edited = app_.tree().invalidate(record->node, Invalidation::PAINT);
  }
  if (edited) {
    record->frame = next;
    edited = animateDataState(record);
  }
  if (!edited && !retired(edited.error())) {
    remember(error(ManagedPhase::PREPARE, record->node, edited.error()));
  }
}

void ManagedScene::pruneDataStates() noexcept {
  // Reuse the existing capture-retirement barrier while user capture destructors run.
  const auto previous = std::exchange(pruningObservers_, true);

  struct Pruning {
    bool &flag;
    bool previous;

    ~Pruning() {
      flag = previous;
    }
  } guard{pruningObservers_, previous};

  for (const auto &record : dataStates_) {
    auto live = closed_ ? std::expected<void, TreeError>(std::unexpected(TreeError::CLOSED))
                        : app_.tree().withScope(record->node, [](const UiScope &) {
                          });
    if (!live && retired(live.error())) {
      record->retired = true;
      stopTimer(*record);
      record->action.reset();
    }
  }
  std::erase_if(dataStates_, [](const auto &record) {
    return record->retired;
  });
}

std::expected<void, TreeError> ManagedScene::prepareDataStates(const LayoutSnapshot &layout) {
  if (dataStates_.empty()) {
    return {};
  }
  try {
    pruneDataStates();
    const auto records = dataStates_;
    bool focus{};
    for (const auto &record : records) {
      if (record->retired || !record->mounted) {
        continue;
      }
      const auto fail = [&](TreeError cause) -> std::expected<void, TreeError> {
        remember(error(ManagedPhase::PREPARE, record->node, cause));
        return std::unexpected(cause);
      };
      if (record->probe) {
        auto status = record->probe();
        if (!status) {
          if (retired(status.error())) {
            record->retired = true;
            stopTimer(*record);
            continue;
          }
          return fail(status.error());
        }
        auto phase = dataPhase(*status);
        if (!phase) {
          return fail(phase.error());
        }
        auto applied = applyDataState(record, {*phase, 1});
        if (!applied) {
          return fail(applied.error());
        }
        if (!status->requestPending) {
          record->retrying = false;
        }
      }
      focus = focus || std::exchange(record->focusDirty, false);
      auto size = layout.viewport(record->node);
      auto eligible = app_.tree().inputEligible(record->node);
      record->eligible =
        record->config.showStatus && size && size->width > 0 && size->height > 0 && eligible && *eligible;
      if (record->config.showStatus) {
        auto synced = app_.tree().edit(record->indicator, [&](detail::DataStateIndicator &indicator) {
          indicator.enabled = eligible && *eligible;
        });
        if (!synced) {
          return fail(synced.error());
        }
      }
      if (record->eligible && isDataPending(record->current.phase)) {
        auto geometry = app_.tree().observeGeometry(record->indicator);
        record->eligible = geometry && geometry->geometry.valid && geometry->geometry.clip.size().width > 0 &&
                           geometry->geometry.clip.size().height > 0;
        if (record->eligible) {
          auto armed = animateDataState(record);
          if (!armed) {
            return fail(armed.error());
          }
        }
      }
      if (!record->eligible || !isDataPending(record->current.phase)) {
        stopTimer(*record);
      }
    }
    if (focus) {
      auto focused = repairDataFocus();
      if (!focused) {
        return std::unexpected(focused.error());
      }
    }
    if (stylesDirty_) {
      auto styled = flushStyles();
      if (!styled) {
        remember(styled.error());
        return std::unexpected(std::get_if<TreeError>(&styled.error().cause) ? std::get<TreeError>(styled.error().cause)
                                                                             : TreeError::INVALID_ARGUMENT);
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    remember(error(ManagedPhase::PREPARE, root_, TreeError::NO_MEMORY));
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    remember(error(ManagedPhase::PREPARE, root_, TreeError::CALLBACK_FAILED));
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals
