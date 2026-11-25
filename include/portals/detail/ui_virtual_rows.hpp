// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace portals::ui::detail {
/// @cond INTERNAL

template <class T, class Source, class Factory>
struct RowsController : VirtualRowsState,
                        SceneActivity,
                        portals::detail::VirtualRowsModelBinding<T>,
                        portals::detail::EditNavigationParticipant,
                        std::enable_shared_from_this<RowsController<T, Source, Factory>> {
  RowsController(ManagedScene &owner, Source descriptor, Factory create, ControlContext defaults)
      : scene(owner), source(std::move(descriptor)), factory(std::move(create)), controls(std::move(defaults)),
        lifetime(Access::lifetime(owner)) {
  }

  ~RowsController() override {
    retire();
  }

  ManagedScene &scene;
  Source source;
  Factory factory;
  ControlContext controls;
  std::weak_ptr<const void> lifetime;
  Node<VirtualRows<T>> node;
  ManagedModel<T> initialModel;
  std::shared_ptr<const void> layoutLease;
  std::optional<ActionBinding> inputBinding;
  std::vector<std::shared_ptr<RowAssignment<T>>> rows = std::vector<std::shared_ptr<RowAssignment<T>>>(0);
  std::shared_ptr<RowAssignment<T>> editPin;
  std::weak_ptr<portals::detail::ManagedModelSourceOf<T>> previousSource;
  std::uint64_t revision{};
  std::uint64_t desiredFirst{};
  CellSize viewport;
  bool dirty{true};
  bool retiredHost{};
  std::uint64_t intentGeneration{};
  std::uint64_t committedIntent{};
  bool resetRestoration{};
  std::shared_ptr<portals::detail::ModelViewHistory> history;
  std::uint64_t historyRole{};
  std::optional<NavigationPosition> observedCurrent;
  std::optional<NavigationPosition> observedReading;

  struct Attempt {
    std::weak_ptr<portals::detail::ManagedModelSourceOf<T>> source;
    std::uint64_t revision{};
    std::uint64_t intent{};
    CellSize viewport;
  } attempt;

  bool blockedAttempt{};
  Node<Widget> focusedNode;
  bool deferredRefresh{};
  double wheelFraction{};
  std::weak_ptr<portals::detail::ManagedModelSourceOf<T>> consumerSource;
  std::optional<ModelConsumer<T>> finiteConsumer;
  std::optional<ModelRequest<T>> finiteRequest;
  RowRange finiteRange;
  std::uint64_t finiteRevision{};

  std::expected<void, TreeError> replaceModel(Model<T> target, ModelRebindOptions rebind) override try {
    if (!active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    if constexpr (std::same_as<Source, Navigation<T>>) {
      auto changed = source.setModel(std::move(target), rebind);
      if (!changed) {
        return std::unexpected(EditAccess::cause(changed.error()));
      }
    } else {
      MountContext context;
      auto model = ModelAccess::resolve(scene, target, context);
      if (!model) {
        return std::unexpected(EditAccess::cause(model.error()));
      }
      if (!active()) {
        return std::unexpected(TreeError::CLOSED);
      }
      auto actual = RowAccess::source(*model);
      if (!actual) {
        return std::unexpected(actual.error());
      }
      auto allowed = beforeTransition(nullptr, *actual);
      if (!allowed) {
        return allowed;
      }
      auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::LAYOUT);
      if (!invalidated) {
        return invalidated;
      }
      auto previous = source;
      initialModel = *model;
      source = std::move(target);
      ModelAccess::publish(context);
    }
    resetRestoration = rebind.restore == ModelRestorePolicy::RESET;
    ++intentGeneration;
    dirty = true;
    blockedAttempt = false;
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }

  void closed() noexcept override {
    retire();
  }

  std::expected<void, TreeError>
  beforeTransition(const NavigationPosition *next,
                   const std::shared_ptr<portals::detail::ManagedModelSource> &targetSource) override {
    for (const auto &row : rows) {
      if (!row->dirty() || row->edit->options.mode != EditMode::ON_COMPLETION) {
        continue;
      }
      const bool sameSource = row->edit->fixed->source == targetSource;
      const bool visibleTarget = next && next->row && std::ranges::any_of(rows, [&](const auto &entry) {
                                   return entry->id == *next->row;
                                 });
      if (!sameSource || !visibleTarget) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    return {};
  }

  std::expected<void, TreeError> registerNavigation(ManagedNavigation navigation) {
    auto record = EditAccess::context(navigation);
    if (!record) {
      return std::unexpected(record.error());
    }
    if ((*record)->admittingEdit) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (!(*record)->editors) {
      (*record)->editors = std::make_unique<std::vector<std::weak_ptr<portals::detail::EditNavigationParticipant>>>(0);
    }
    (*record)->editors->push_back(this->shared_from_this());
    return {};
  }

  std::expected<void, TreeError> bindInput() {
    auto binding = RowAccess::app(scene).router().bind(
      node,
      [weak = this->weak_from_this()](VirtualRows<T> &, const RoutedEvent &event) {
        auto owner = weak.lock();
        if (!owner || !owner->active()) {
          return EventReply::IGNORED;
        }
        const auto *mouse = std::get_if<MouseInputEvent>(&event);
        const auto *key = std::get_if<KeyEvent>(&event);
        const bool wheel = mouse && mouse->action == MouseAction::SCROLL &&
                           (mouse->button == MouseButton::WHEEL_UP || mouse->button == MouseButton::WHEEL_DOWN);
        const bool page = key && key->action != KeyAction::RELEASE &&
                          (key->key == InputKey::PAGE_UP || key->key == InputKey::PAGE_DOWN);
        if (wheel || page) {
          const bool backwards = wheel ? mouse->button == MouseButton::WHEEL_UP : key->key == InputKey::PAGE_UP;
          if (wheel &&
              (mouse->modifiers.alt || mouse->modifiers.control || mouse->modifiers.super || mouse->modifiers.shift)) {
            return EventReply::IGNORED;
          }
          auto changed = page ? owner->requestPage(backwards) : owner->requestWheel(mouse->wheelDelta, backwards);
          if (!changed) {
            owner->failure = changed.error();
          }
          return EventReply::HANDLED;
        }
        const auto *action = std::get_if<ActionEvent>(&event);
        if (!action || (action->action != portals::Action::MOVE_UP && action->action != portals::Action::MOVE_DOWN &&
                        action->action != portals::Action::MOVE_HOME && action->action != portals::Action::MOVE_END)) {
          return EventReply::IGNORED;
        }
        std::expected<void, TreeError> changed;
        if (action->action == portals::Action::MOVE_UP) {
          changed = owner->requestMove(-1);
        } else if (action->action == portals::Action::MOVE_DOWN) {
          changed = owner->requestMove(1);
        } else if (action->action == portals::Action::MOVE_HOME) {
          changed =
            owner->sourceCapabilities ? owner->requestAnchor({}, SourceDirection::FORWARD) : owner->requestFirst(0);
        } else if (owner->exactCount) {
          changed = owner->requestFirst(*owner->exactCount ? *owner->exactCount - 1 : 0);
        } else {
          changed = owner->requestAnchor({SourceAnchorKind::TAIL, {}, {}}, SourceDirection::BACKWARD);
        }
        if (!changed) {
          owner->failure = changed.error();
        }
        return EventReply::HANDLED;
      },
      false);
    if (!binding) {
      return std::unexpected(binding.error());
    }
    inputBinding.emplace(std::move(*binding));
    return {};
  }

  bool active() const noexcept override {
    if (retiredHost || lifetime.expired()) {
      return false;
    }
    return RowAccess::tree(scene)
      .withScope(node,
                 [](const UiScope &) {
                 })
      .has_value();
  }

  void cancel() noexcept override {
    retire();
  }

  void retire() noexcept override {
    retiredHost = true;
    if (finiteRequest) {
      finiteRequest->cancel();
    }
    if (finiteConsumer) {
      (void)finiteConsumer->close();
    }
    for (auto &row : rows) {
      row->close();
    }
    if (editPin) {
      editPin->close();
    }
  }

  std::expected<void, TreeError> admitDraft(const RowAssignment<T> *candidate) {
    if (!active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (editPin && editPin.get() != candidate && editPin->dirty()) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    for (const auto &row : rows) {
      if (row.get() != candidate && row->dirty()) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    return {};
  }

  std::expected<void, TreeError> preflight(std::uint64_t target, std::size_t visible) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto &row = rows[index];
      const auto ordinal = first + index;
      if (ordinal < target || ordinal - target >= visible) {
        for (const auto &participant : row->participants) {
          auto allowed = participant->beforeLeave();
          if (!allowed) {
            return allowed;
          }
        }
      }
      if ((ordinal < target || ordinal - target >= visible) && row->dirty() &&
          row->edit->options.mode == EditMode::ON_COMPLETION) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    return {};
  }

  std::expected<void, TreeError> requestFirst(std::uint64_t target) override {
    if (!active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    const auto visible =
      static_cast<std::size_t>(viewport.height / options.rowHeight + (viewport.height % options.rowHeight != 0));
    auto allowed = preflight(target, visible);
    if (!allowed) {
      return allowed;
    }
    auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::PAINT);
    if (!invalidated) {
      return invalidated;
    }
    desiredFirst = target;
    ++intentGeneration;
    dirty = true;
    return {};
  }

  std::expected<void, TreeError> requestWheel(double magnitude, bool backwards) {
    if (!std::isfinite(magnitude) || magnitude <= 0) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    const auto accumulated = wheelFraction + (backwards ? -magnitude : magnitude);
    if (!std::isfinite(accumulated) || std::abs(accumulated) >= 0x1p63) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    const auto whole = std::trunc(accumulated);
    if (whole == 0) {
      wheelFraction = accumulated;
      return {};
    }
    auto moved = requestMove(static_cast<std::int64_t>(whole));
    if (moved) {
      wheelFraction = accumulated - whole;
    }
    return moved;
  }

  std::expected<void, TreeError> requestMove(std::int64_t delta) override {
    if (delta < 0) {
      const auto amount = static_cast<std::uint64_t>(-(delta + 1)) + 1;
      return requestFirst(desiredFirst - std::min(desiredFirst, amount));
    }
    const auto amount = static_cast<std::uint64_t>(delta);
    if (amount > std::numeric_limits<std::uint64_t>::max() - desiredFirst) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    return requestFirst(desiredFirst + amount);
  }

  std::expected<void, TreeError> requestPage(bool backwards) override {
    const auto step =
      std::max<std::uint64_t>(1, viewport.height / options.rowHeight + (viewport.height % options.rowHeight != 0));
    if (!backwards && desiredFirst > std::numeric_limits<std::uint64_t>::max() - step) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    return requestFirst(backwards ? desiredFirst - std::min(desiredFirst, step) : desiredFirst + step);
  }

  void prepared() noexcept override {
    service();
  }

  void layoutCompleted() noexcept override {
    if (dirty && active()) {
      service();
    }
  }

  void layoutPrepared(const LayoutSnapshot &layout) noexcept override {
    auto next = layout.viewport(node);
    if (next && *next != viewport) {
      viewport = *next;
      dirty = true;
      if (active()) {
        (void)RowAccess::tree(scene).invalidate(node, Invalidation::LAYOUT);
      }
    }
  }

  std::expected<std::shared_ptr<portals::detail::ManagedModelSourceOf<T>>, TreeError> currentSource() {
    if constexpr (std::same_as<Source, Navigation<T>>) {
      auto handle = source.managed();
      if (!handle) {
        return std::unexpected(EditAccess::cause(handle.error()));
      }
      auto record = EditAccess::context(*handle);
      if (!record) {
        return std::unexpected(record.error());
      }
      auto typed = std::dynamic_pointer_cast<portals::detail::ManagedModelSourceOf<T>>((*record)->model.lock());
      return typed ? std::expected<std::shared_ptr<portals::detail::ManagedModelSourceOf<T>>, TreeError>(typed)
                   : std::unexpected(TreeError::CLOSED);
    } else {
      return RowAccess::source(initialModel);
    }
  }

  void dispose(const std::shared_ptr<RowAssignment<T>> &row) noexcept {
    row->close();
    if (!lifetime.expired()) {
      (void)scene.remove(row->root);
    }
  }

  std::expected<std::shared_ptr<RowAssignment<T>>, TreeError>
  construct(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual,
            const std::shared_ptr<const void> &pin, const DeliveredRow<T> &value, ModelResource resource,
            SourceStamp incarnation) {
    auto row = std::make_shared<RowAssignment<T>>();
    auto root = scene.emplace<SizedWidget>(node, DEFAULT_VIEWPORT_POLICY);
    if (!root) {
      return std::unexpected(EditAccess::cause(root.error()));
    }
    row->root = *root;

    struct Rollback {
      RowsController &host;
      std::shared_ptr<RowAssignment<T>> row;
      bool committed{};

      ~Rollback() {
        if (!committed) {
          host.dispose(row);
        }
      }
    } rollback{*this, row};

    row->id = value.id;
    row->value = value.value;
    row->valueRevision = row->snapshotRevision = value.valueRevision;
    row->pin = pin;
    row->origin = std::make_shared<portals::detail::ManagedOrigin>();
    row->origin->tree = &RowAccess::tree(scene);
    row->origin->node = row->root;
    row->origin->lifetime = lifetime;
    row->model = EditAccess::model<T>(actual);
    row->edit = std::make_shared<EditState<T>>(Navigation<T>{}, options.editing);
    row->edit->fixed = std::make_unique<FixedEditContext<T>>();
    row->edit->fixed->position = NavigationPosition{resource, PositionKind::ITEM,        row->id,
                                                    0,        PositionAffinity::FORWARD, PositionResolution::EXACT};
    row->edit->fixed->source = actual;
    row->edit->fixed->sourceSession = incarnation;
    row->edit->fixed->origin = row->origin;
    row->edit->fixed->admitDraft = [host = this->weak_from_this(),
                                    weak = std::weak_ptr(row)]() -> std::expected<void, TreeError> {
      auto owner = host.lock();
      auto target = weak.lock();
      return owner && target ? owner->admitDraft(target.get()) : std::unexpected(TreeError::CLOSED);
    };
    row->erase = [weak = std::weak_ptr(row)]() -> std::expected<void, TreeError> {
      auto target = weak.lock();
      if (!target || !target->origin->check()) {
        return std::unexpected(TreeError::CLOSED);
      }
      auto sequence = EditAccess::sequence(target->model);
      if (!sequence || !sequence->current) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      auto lookup = sequence->current->locateRow(target->id);
      if (lookup.state != RowLookupState::FOUND) {
        return std::unexpected(TreeError::STALE_NODE);
      }
      auto removed = target->model.erase(lookup.index);
      return removed ? std::expected<void, TreeError>{} : std::unexpected(EditAccess::cause(removed.error()));
    };
    auto hidden = RowAccess::tree(scene).setVisible(row->root, false);
    if (!hidden) {
      dispose(row);
      return std::unexpected(hidden.error());
    }
    try {
      RowAccess::Scope scope(scene, row->origin);
      row->building = true;
      auto recipe = std::invoke(factory, RowContext<T>(row));
      row->building = false;
      auto mounted = mountAt(scene, row->root, std::move(recipe), controls);
      if (!mounted) {
        dispose(row);
        return std::unexpected(EditAccess::cause(mounted.error()));
      }
      auto supported = RowAccess::validateEditors(scene, row->root, row->edit->bindings, !row->participants.empty());
      if (!supported) {
        dispose(row);
        return std::unexpected(supported.error());
      }
      if (!active() || !row->origin->check()) {
        dispose(row);
        return std::unexpected(TreeError::CLOSED);
      }
      rollback.committed = true;
      return row;
    } catch (...) {
      dispose(row);
      throw;
    }
  }

  std::expected<void, TreeError> observeFocus() {
    if (editPin && !editPin->dirty()) {
      dirty = true;
      blockedAttempt = false;
    }
    auto focused = RowAccess::app(scene).router().focused();
    if (!focused) {
      return std::unexpected(focused.error());
    }
    if (focusedNode != *focused && deferredRefresh) {
      dirty = true;
      blockedAttempt = false;
    }
    focusedNode = *focused;
    return {};
  }

  std::expected<void, TreeError> ensureHistory() {
    if (history) {
      return {};
    }
    auto created = portals::detail::ModelViewHistory::create(lifetime, options.checkpoints);
    if (!created) {
      return std::unexpected(created.error());
    }
    auto role = (*created)->role();
    if (!role) {
      return std::unexpected(role.error());
    }
    historyRole = *role;
    history = std::move(*created);
    return {};
  }

  std::expected<std::optional<portals::detail::ModelViewHistoryChange>, TreeError>
  stageHistory(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual) {
    auto previous = previousSource.lock();
    if (!previous || previous == actual) {
      return std::optional<portals::detail::ModelViewHistoryChange>{};
    }
    auto ready = ensureHistory();
    if (!ready) {
      return std::unexpected(ready.error());
    }
    portals::detail::ModelViewCheckpoint checkpoint;
    checkpoint.first = first;
    if (!rows.empty()) {
      auto bookmark = ModelBookmark::create(
        previous->navigationResource,
        {PositionKind::ITEM, rows.front()->id, first, rows.front()->valueRevision, PositionAffinity::FORWARD}, {},
        options.checkpoints.bookmarks);
      if (!bookmark) {
        return std::unexpected(portals::detail::navigationTreeError(bookmark.error()));
      }
      checkpoint.reading = *bookmark;
    }
    auto staged = history->stage(previous, historyRole, std::move(checkpoint), this->weak_from_this());
    return staged ? std::expected<std::optional<portals::detail::ModelViewHistoryChange>, TreeError>(
                      std::optional(std::move(*staged)))
                  : std::unexpected(staged.error());
  }

  std::expected<std::uint64_t, TreeError>
  restoredFirst(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual, const ModelFrame<T> &frame,
                SourceStamp incarnation) {
    const bool switched = previousSource.lock() && previousSource.lock() != actual;
    auto target = switched ? std::uint64_t{} : desiredFirst;
    if (switched && history && !resetRestoration) {
      auto saved = history->find(actual, historyRole);
      if (saved) {
        target = saved->value.first;
        if (saved->value.reading) {
          auto resolver = FinitePositionResolver<T>::create(actual->navigationResource, incarnation, frame.snapshot(),
                                                            actual->navigationChanges);
          if (!resolver) {
            return std::unexpected(portals::detail::navigationTreeError(resolver.error()));
          }
          auto resolved = resolver->resolve({incarnation, frame.dataRevision(), 1, *saved->value.reading}, {});
          if (!resolved) {
            return std::unexpected(portals::detail::navigationTreeError(resolved.error()));
          }
          if (resolved->state == PositionResolution::EXACT || resolved->state == PositionResolution::NEAREST) {
            target = resolved->position.offset;
          }
        }
      }
    } else if (!switched && committedIntent == intentGeneration && revision != frame.revision() && !rows.empty()) {
      auto anchor = frame.locateRow(rows.front()->id);
      if (!anchor) {
        return std::unexpected(anchor.error());
      }
      if (anchor->state == RowLookupState::FOUND) {
        target = anchor->index;
      }
    }
    if constexpr (std::same_as<Source, Navigation<T>>) {
      auto navigation = source.managed();
      if (!navigation) {
        return std::unexpected(EditAccess::cause(navigation.error()));
      }
      const auto binding = ModelAccess::bindings(source);
      auto observe = [&](bool enabled, bool reading,
                         std::optional<NavigationPosition> &observed) -> std::expected<void, TreeError> {
        if (!enabled) {
          return {};
        }
        auto channel = reading ? navigation->readingAnchor() : navigation->current();
        if (!channel) {
          return std::unexpected(channel.error());
        }
        auto value = channel->snapshot();
        if (!value) {
          return std::unexpected(value.error());
        }
        const auto &position = (*value)->value;
        const bool changed = !observed || position != *observed;
        observed = position;
        if (changed && position.row && position.resource == actual->navigationResource && (!switched || reading)) {
          auto found = frame.locateRow(*position.row);
          if (!found) {
            return std::unexpected(found.error());
          }
          const auto visible = static_cast<std::uint64_t>(viewport.height / options.rowHeight +
                                                          (viewport.height % options.rowHeight != 0));
          if (found->state == RowLookupState::FOUND &&
              (reading || found->index < target || found->index - target >= visible)) {
            target = found->index;
          }
        }
        return {};
      };
      auto current = observe(binding.current, false, observedCurrent);
      if (!current) {
        return std::unexpected(current.error());
      }
      auto reading = observe(binding.readingAnchor, true, observedReading);
      if (!reading) {
        return std::unexpected(reading.error());
      }
    }
    return frame.rowCount() ? std::min(target, frame.rowCount() - 1) : 0;
  }

  std::expected<void, TreeError> update() {
    auto actual = currentSource();
    if (!actual || (*actual)->retired || !(*actual)->model) {
      return std::unexpected(actual ? TreeError::CLOSED : actual.error());
    }
    auto resource = portals::detail::modelNavigationResource(**actual);
    if (!resource) {
      return std::unexpected(resource.error());
    }
    auto frame = (*actual)->model->pin();
    if (!frame) {
      return std::unexpected(frame.error());
    }
    auto focus = observeFocus();
    if (!focus) {
      return focus;
    }
    const auto count = frame->rowCount();
    exactCount = count;
    if (blockedAttempt && !dirty && attempt.source.lock() == *actual && attempt.revision == frame->revision() &&
        attempt.intent == intentGeneration && attempt.viewport == viewport) {
      return {};
    }
    auto incarnation = EditAccess::sourceSession<T>(*actual);
    if (!incarnation) {
      return std::unexpected(incarnation.error());
    }
    auto saved = stageHistory(*actual);
    if (!saved) {
      return std::unexpected(saved.error());
    }
    auto restored = restoredFirst(*actual, *frame, *incarnation);
    if (!restored) {
      return std::unexpected(restored.error());
    }
    const auto target = *restored;
    const auto visible =
      viewport.width && viewport.height
        ? static_cast<std::size_t>(std::min<std::uint64_t>(
            count - target, viewport.height / options.rowHeight + (viewport.height % options.rowHeight != 0)))
        : 0;
    if (visible > options.maxRows) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    if (!dirty && previousSource.lock() == *actual && revision == frame->revision()) {
      return {};
    }
    auto allowed = preflight(target, visible);
    if (!allowed) {
      return allowed;
    }
    if (!visible) {
      return commitRows(*actual, {}, {}, frame->revision(), target, (*actual)->navigationResource, *incarnation,
                        std::move(*saved));
    }
    auto range = frame->read({target, visible});
    if (!range) {
      return std::unexpected(range.error());
    }
    if (range->state() == RangeState::PENDING) {
      if (consumerSource.lock() != *actual) {
        auto opened = (*actual)->model->openRange();
        if (!opened) {
          return std::unexpected(opened.error());
        }
        finiteRequest.reset();
        finiteConsumer.emplace(std::move(*opened));
        consumerSource = *actual;
      }
      const RowRange requested{target, visible};
      const bool sameRequest = finiteRequest && finiteRevision == frame->dataRevision() &&
                               finiteRange.first == target && finiteRange.count == visible;
      if (sameRequest) {
        auto completed = frame->range(finiteConsumer->id());
        if (!completed) {
          return std::unexpected(completed.error());
        }
        range = std::move(completed);
      } else {
        auto action = (*actual)->preparation;
        if (!action) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        auto requestedWork = finiteConsumer->request(requested);
        if (!requestedWork) {
          return std::unexpected(requestedWork.error());
        }
        finiteRange = requested;
        finiteRevision = frame->dataRevision();
        finiteRequest = std::move(*requestedWork);
        auto sent = (*action)(*frame, finiteRequest);
        if (!sent) {
          return sent;
        }
        return {};
      }
    }
    if (range->state() == RangeState::PENDING) {
      return {};
    }
    if (range->state() != RangeState::READY) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
    auto pin = std::make_shared<ModelRange<T>>(std::move(*range));
    attempt = {*actual, frame->revision(), intentGeneration, viewport};
    std::vector<DeliveredRow<T>> values(0);
    values.reserve(visible);
    for (const auto &value : pin->rows()) {
      values.push_back({value.id, &value.value, frame->dataRevision()});
    }
    return commitRows(*actual, pin, values, frame->revision(), target, (*actual)->navigationResource, *incarnation,
                      std::move(*saved));
  }

  std::expected<void, TreeError> commitRows(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual,
                                            const std::shared_ptr<const void> &pin,
                                            std::span<const DeliveredRow<T>> values, std::uint64_t publication,
                                            std::uint64_t target, ModelResource resource, SourceStamp incarnation,
                                            std::optional<portals::detail::ModelViewHistoryChange> saved = {},
                                            RowsCommitPublication metadata = {}) {
    const auto visible = values.size();
    const auto stagedIntent = intentGeneration;
    std::vector<std::shared_ptr<RowAssignment<T>>> staging(0);
    std::vector<RowPlacement> placement(0);
    staging.reserve(visible);
    placement.reserve(visible);

    struct Refresh {
      std::shared_ptr<RowAssignment<T>> row;
      const T *value{};
      std::uint64_t revision{};
      std::shared_ptr<const void> next;
      std::shared_ptr<const void> retired;
    };

    std::vector<Refresh> refresh(0);
    refresh.reserve(visible);
    bool deferred{};
    auto rollback = [&] {
      for (auto &row : staging) {
        if (std::ranges::find(rows, row) == rows.end() && row != editPin) {
          dispose(row);
        }
      }
    };
    try {
      for (const auto &value : values) {
        std::shared_ptr<RowAssignment<T>> row;
        const auto found = std::ranges::find_if(rows, [&](const auto &existing) {
          return existing->id == value.id && existing->edit->fixed->source == actual &&
                 existing->edit->fixed->sourceSession == incarnation &&
                 (existing->snapshotRevision == value.valueRevision || existing->retainsMountedState() ||
                  (focusedNode != Node<Widget>{} &&
                   RowAccess::tree(scene).contains(existing->root, focusedNode).value_or(false)));
        });
        if (found != rows.end()) {
          row = *found;
        } else if (editPin && editPin->id == value.id && editPin->edit->fixed->source == actual &&
                   editPin->edit->fixed->sourceSession == incarnation) {
          row = editPin;
        } else {
          auto created = construct(actual, value.pin ? value.pin : pin, value, resource, incarnation);
          if (!created) {
            rollback();
            return std::unexpected(created.error());
          }
          row = std::move(*created);
        }
        if (row->value != value.value || row->valueRevision != value.valueRevision) {
          refresh.push_back({row, value.value, value.valueRevision, value.pin ? value.pin : pin, {}});
        }
        deferred = deferred || row->snapshotRevision != value.valueRevision;
        placement.push_back({row->root, static_cast<std::int32_t>(staging.size() * options.rowHeight)});
        staging.push_back(std::move(row));
      }
      for (const auto &row : rows) {
        if (std::ranges::find(staging, row) != staging.end()) {
          continue;
        }
        for (const auto &participant : row->participants) {
          auto allowed = participant->beforeLeave();
          if (!allowed) {
            rollback();
            return allowed;
          }
        }
        if (row->dirty() && row->edit->options.mode == EditMode::ON_COMPLETION) {
          rollback();
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
      }
      std::shared_ptr<RowAssignment<T>> nextPin;
      if (editPin && editPin->dirty() && std::ranges::find(staging, editPin) == staging.end()) {
        nextPin = editPin;
      }
      for (const auto &row : rows) {
        if (row->dirty() && std::ranges::find(staging, row) == staging.end()) {
          if (nextPin && nextPin != row) {
            rollback();
            return std::unexpected(TreeError::WOULD_BLOCK);
          }
          nextPin = row;
        }
      }
      if (!active() || intentGeneration != stagedIntent) {
        rollback();
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      if (actual) {
        auto latest = currentSource();
        auto latestFrame = actual->model ? actual->model->pin()
                                         : std::expected<ModelFrame<T>, TreeError>(std::unexpected(TreeError::CLOSED));
        if (!latest || *latest != actual || !latestFrame || latestFrame->revision() != publication) {
          rollback();
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
      }
      auto &tree = RowAccess::tree(scene);
      // Validate every identity before the allocation/callback-free visibility publication.
      for (const auto &row : rows) {
        auto live = tree.inspect(row->root);
        if (!live) {
          rollback();
          return std::unexpected(live.error());
        }
      }
      for (const auto &row : staging) {
        auto live = tree.inspect(row->root);
        if (!live) {
          rollback();
          return std::unexpected(live.error());
        }
      }
      if (editPin) {
        auto live = tree.inspect(editPin->root);
        if (!live) {
          rollback();
          return std::unexpected(live.error());
        }
      }
      std::vector<Node<Widget>> visibleNodes(0);
      visibleNodes.reserve(staging.size());
      for (const auto &row : staging) {
        visibleNodes.push_back(row->root);
      }
      if (saved && !history->valid(*saved)) {
        rollback();
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto published = portals::detail::RowTreeAccess::publish(tree, node, visibleNodes);
      if (!published) {
        rollback();
        return std::unexpected(published.error());
      }
      for (auto &change : refresh) {
        change.retired.swap(change.row->pin);
        change.row->pin = change.next;
        change.row->value = change.value;
        change.row->valueRevision = change.revision;
      }
      rows.swap(staging);
      placements.swap(placement);
      editPin.swap(nextPin);
      first = desiredFirst = target;
      previousSource = actual;
      revision = publication;
      dirty = false;
      blockedAttempt = false;
      committedIntent = intentGeneration;
      deferredRefresh = deferred;
      resetRestoration = false;
      failure.reset();
      if (metadata.publish) {
        metadata.publish(metadata.context);
      }
      if (saved) {
        history->commit(std::move(*saved));
      }
      // Destructors and unmount callbacks see the fully published assignment set.
      for (const auto &row : staging) {
        if (std::ranges::find(rows, row) == rows.end() && row != editPin) {
          dispose(row);
        }
      }
      if (nextPin && std::ranges::find(rows, nextPin) == rows.end() && nextPin != editPin) {
        dispose(nextPin);
      }
      if (!active()) {
        return {};
      }
      if constexpr (std::same_as<Source, Navigation<T>>) {
        if (!rows.empty() && ModelAccess::bindings(source).readingAnchor) {
          auto navigation = source.managed();
          if (!navigation) {
            return std::unexpected(EditAccess::cause(navigation.error()));
          }
          auto bookmark = ModelBookmark::create(
            resource,
            {PositionKind::ITEM, rows.front()->id, target, rows.front()->valueRevision, PositionAffinity::FORWARD}, {},
            options.checkpoints.bookmarks);
          if (!bookmark) {
            return std::unexpected(portals::detail::navigationTreeError(bookmark.error()));
          }
          auto anchored = navigation->setReadingAnchor(*bookmark);
          if (!anchored) {
            return anchored;
          }
          if (!active()) {
            return {};
          }
        }
      }
      for (const auto &row : rows) {
        for (const auto label : row->reactiveLabels) {
          auto synced = RowAccess::syncCaption(scene, label);
          if (!synced) {
            return std::unexpected(EditAccess::cause(synced.error()));
          }
          if (!active()) {
            return {};
          }
        }
      }
      return {};
    } catch (...) {
      rollback();
      throw;
    }
  }

  void service() noexcept override {
    if (!active()) {
      retire();
      return;
    }
    if (portals::detail::RowTreeAccess::dispatching(RowAccess::app(scene).router())) {
      return;
    }
    try {
      auto result = update();
      if (!result) {
        failure = result.error();
        blockedAttempt = true;
        dirty = false;
      }
    } catch (const std::bad_alloc &) {
      failure = TreeError::NO_MEMORY;
      blockedAttempt = true;
      dirty = false;
    } catch (...) {
      failure = TreeError::CALLBACK_FAILED;
      blockedAttempt = true;
      dirty = false;
    }
  }
};

inline TreeError rowSourceError(SourceError error) noexcept {
  switch (error) {
  case SourceError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case SourceError::CLOSED:
    return TreeError::CLOSED;
  case SourceError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case SourceError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  case SourceError::IDENTITY_EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case SourceError::CALLBACK_FAILED:
    return TreeError::CALLBACK_FAILED;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}

template <class T> class RowWindowJob final : public BackgroundJob {
public:
  explicit RowWindowJob(WindowRequest<T> request) : request_(std::move(request)) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      result_ = request_.load(stop);
    } catch (const std::bad_alloc &) {
      result_ = std::unexpected(SourceError::NO_MEMORY);
    } catch (...) {
      result_ = std::unexpected(SourceError::CALLBACK_FAILED);
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto sent = request_.complete(result_);
    if (sent) {
      return {};
    }
    switch (sent.error()) {
    case SchedulerError::WOULD_BLOCK:
      return std::unexpected(AsyncError::WOULD_BLOCK);
    case SchedulerError::NO_MEMORY:
      return std::unexpected(AsyncError::NO_MEMORY);
    case SchedulerError::CLOSED:
      return std::unexpected(AsyncError::CLOSED);
    default:
      return std::unexpected(AsyncError::CALLBACK_FAILED);
    }
  }

private:
  WindowRequest<T> request_;
  std::expected<SourceReadReply<T>, SourceError> result_{std::unexpected(SourceError::WOULD_BLOCK)};
};

template <class T, class Factory> struct WindowRowsController final : RowsController<T, Model<T>, Factory> {
  using Base = RowsController<T, Model<T>, Factory>;
  enum class LoadKind {
    NONE,
    REPLACE,
    APPEND,
    PREPEND
  };

  struct Segment {
    WindowPin<T> pin;
    bool beforeEnd{};
    bool afterEnd{};
    bool temporaryAfter{};
  };

  WindowRowsController(ManagedScene &scene, WindowRowsModel<T> source, Factory create, ControlContext defaults)
      : Base(scene, Model<T>{}, std::move(create), std::move(defaults)), declaration(std::move(source)) {
  }

  ~WindowRowsController() override {
    retire();
  }

  WindowRowsModel<T> declaration;
  std::unique_ptr<WindowSource<T>> window;
  std::optional<WindowConsumer<T>> consumer;
  std::unique_ptr<BackgroundLatest> worker;
  std::optional<WindowRequest<T>> pending;
  std::array<Segment, 2> segments;
  std::size_t segmentCount{};
  std::size_t visibleRows{};
  std::size_t requestedRows{};
  std::int64_t offset{};
  std::int64_t wanted{};
  std::int64_t replacementOffset{};
  LoadKind loading{LoadKind::NONE};
  SourceAnchor anchor;
  SourceDirection direction{SourceDirection::FORWARD};
  bool initialLoad{true};
  bool entryKnown{true};
  const WindowRow<T> *lastRows{};
  std::uint64_t lastRevision{};
  std::uint64_t seenRevision{};
  std::uint64_t bufferRevision{};
  std::uint64_t bufferSerial{};
  std::uint64_t renderedSerial{};
  std::uint64_t publication{};
  std::uint64_t failedSerial{};
  std::uint64_t failedIntent{};
  std::int64_t failedOffset{};
  std::weak_ptr<WindowProvider<T>> preparedProvider;
  ModelResource preparedResource;
  std::uint64_t preparedRevision{};

  std::expected<void, TreeError> protocol(SourceError error) {
    this->sourceFailure = error;
    return std::unexpected(rowSourceError(error));
  }

  void retire() noexcept override {
    Base::retire();
    if (consumer) {
      consumer->close();
    }
    if (window) {
      window->close();
    }
    if (worker) {
      worker->cancel();
    }
    pending.reset();
  }

  std::size_t storedRows() const noexcept {
    std::size_t result{};
    for (std::size_t index = 0; index < segmentCount; ++index) {
      result += segments[index].pin.size();
    }
    return result;
  }

  SourceCursor before() const noexcept {
    return segmentCount ? segments[0].pin.page(0).before() : SourceCursor{};
  }

  SourceCursor after() const noexcept {
    if (!segmentCount) {
      return {};
    }
    const auto &pin = segments[segmentCount - 1].pin;
    return pin.page(pin.pageCount() - 1).after();
  }

  std::optional<RowId> rowAt(std::size_t index) const noexcept {
    for (std::size_t segment = 0; segment < segmentCount; ++segment) {
      for (std::size_t page = 0; page < segments[segment].pin.pageCount(); ++page) {
        const auto rows = segments[segment].pin.page(page).rows();
        if (index < rows.size()) {
          return rows[index].id;
        }
        index -= rows.size();
      }
    }
    return {};
  }

  std::expected<void, TreeError> initialize() {
    if (!window) {
      auto created = WindowSource<T>::create(RowAccess::tree(this->scene), this->node, declaration.provider,
                                             declaration.options, declaration.budget);
      if (!created) {
        return protocol(created.error());
      }
      this->sourceCapabilities = (*created)->capabilities();
      seenRevision = declaration.options.initialRevision;
      window = std::move(*created);
    }
    if (!consumer) {
      auto opened = window->openConsumer();
      if (!opened) {
        return protocol(opened.error());
      }
      consumer.emplace(std::move(*opened));
    }
    return {};
  }

  std::expected<void, TreeError> beforeMove(std::int64_t target, bool all = false) {
    for (std::size_t index = 0; index < this->rows.size(); ++index) {
      const auto ordinal = offset + static_cast<std::int64_t>(index);
      if (!all && ordinal >= target && ordinal - target < static_cast<std::int64_t>(visibleRows)) {
        continue;
      }
      const auto &row = this->rows[index];
      for (const auto &participant : row->participants) {
        auto allowed = participant->beforeLeave();
        if (!allowed) {
          return allowed;
        }
      }
      if (row->dirty() && row->edit->options.mode == EditMode::ON_COMPLETION) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    return {};
  }

  std::expected<void, TreeError> startLoad(SourceAnchor target, SourceDirection traversal, LoadKind kind,
                                           std::int64_t replacement = 0) {
    auto request = consumer->request(target, traversal, std::max<std::size_t>(1, requestedRows));
    if (!request) {
      return protocol(request.error());
    }
    if (!this->active()) {
      request->cancel();
      return std::unexpected(TreeError::CLOSED);
    }
    pending = *request;
    loading = kind;
    replacementOffset = replacement;
    anchor = target;
    direction = traversal;
    this->sourceStatus = WindowStatus::LOADING;
    this->sourceFailure.reset();
    return {};
  }

  std::expected<void, TreeError> requestFirst(std::uint64_t) override {
    return protocol(SourceError::UNSUPPORTED);
  }

  std::expected<void, TreeError> requestMove(std::int64_t delta) override {
    if (!this->active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    auto ready = initialize();
    if (!ready) {
      return ready;
    }
    if ((delta > 0 && wanted > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && wanted < std::numeric_limits<std::int64_t>::min() - delta)) {
      return protocol(SourceError::POSITION_EXHAUSTED);
    }
    auto target = wanted + delta;
    if (target < 0 && entryKnown) {
      target = 0;
    }
    if (target < 0 && !window->capabilities().backward) {
      return protocol(SourceError::UNSUPPORTED);
    }
    if (target < offset - this->options.maxPendingRows || target > offset + this->options.maxPendingRows) {
      return protocol(SourceError::WOULD_BLOCK);
    }
    auto allowed = beforeMove(target, loading == LoadKind::REPLACE);
    if (!allowed) {
      return allowed;
    }
    if (target == wanted) {
      return {};
    }
    auto invalidated = RowAccess::tree(this->scene).invalidate(this->node, Invalidation::PAINT);
    if (!invalidated) {
      return invalidated;
    }
    wanted = target;
    ++this->intentGeneration;
    this->dirty = true;
    this->blockedAttempt = false;
    return {};
  }

  std::expected<void, TreeError> requestPage(bool backwards) override {
    const auto page = static_cast<std::int64_t>(std::max<std::size_t>(1, visibleRows));
    return requestMove(backwards ? -page : page);
  }

  std::expected<void, TreeError> requestAnchor(SourceAnchor target, SourceDirection traversal) override {
    if (!this->active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    auto ready = initialize();
    if (!ready) {
      return ready;
    }
    auto allowed = beforeMove(0, true);
    if (!allowed) {
      return allowed;
    }
    auto requested = startLoad(target, traversal, LoadKind::REPLACE);
    if (!requested) {
      return requested;
    }
    wanted = 0;
    initialLoad = false;
    entryKnown = target.kind == SourceAnchorKind::ENTRY && traversal == SourceDirection::FORWARD;
    ++this->intentGeneration;
    this->dirty = true;
    this->blockedAttempt = false;
    return {};
  }

  std::expected<void, TreeError> notifyReady(std::uint64_t revision) override {
    auto ready = initialize();
    if (!ready) {
      return ready;
    }
    auto sent = window->notifyReady(window->stamp(), revision);
    return sent ? std::expected<void, TreeError>{} : protocol(sent.error());
  }

  std::expected<void, TreeError> replaceModel(Model<T>, ModelRebindOptions) override {
    return protocol(SourceError::UNSUPPORTED);
  }

  std::expected<std::optional<portals::detail::ModelViewHistoryChange>, TreeError> captureHistory() {
    auto provider = preparedProvider.lock();
    if (!provider || this->rows.empty() || !preparedRevision) {
      return std::optional<portals::detail::ModelViewHistoryChange>{};
    }
    auto ready = this->ensureHistory();
    if (!ready) {
      return std::unexpected(ready.error());
    }
    auto bookmark = ModelBookmark::create(
      preparedResource, {PositionKind::ITEM, this->rows.front()->id, 0, preparedRevision, PositionAffinity::FORWARD},
      {}, this->options.checkpoints.bookmarks);
    if (!bookmark) {
      this->sourceFailure = bookmark.error();
      return std::unexpected(rowSourceError(bookmark.error()));
    }
    portals::detail::ModelViewCheckpoint checkpoint;
    checkpoint.reading = *bookmark;
    auto saved = this->history->stageResource(preparedResource, provider, this->historyRole, std::move(checkpoint));
    return saved ? std::expected<std::optional<portals::detail::ModelViewHistoryChange>, TreeError>(
                     std::optional(std::move(*saved)))
                 : std::unexpected(saved.error());
  }
  std::expected<void, TreeError> replaceWindowModel(const WindowRowsModel<T> &target,
                                                    ModelRebindOptions rebind) override try {
    if (!this->active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (!target.provider || target.failure || !target.resource.identity.instance || !target.resource.identity.epoch ||
        !target.resource.lineage) {
      return protocol(target.failure.value_or(SourceError::INVALID_ARGUMENT));
    }
    auto allowed = beforeMove(0, true);
    if (!allowed) {
      return allowed;
    }
    if (declaration.provider == target.provider && declaration.resource == target.resource) {
      return rebind.restore == ModelRestorePolicy::RESET ? requestAnchor({}, SourceDirection::FORWARD)
                                                         : std::expected<void, TreeError>{};
    }
    const auto generation = this->intentGeneration;
    auto saved = captureHistory();
    if (!saved) {
      return std::unexpected(saved.error());
    }
    SourceAnchor restore;
    if (rebind.restore == ModelRestorePolicy::REMEMBER && this->history) {
      auto checkpoint = this->history->find(target.resource, this->historyRole, false);
      if (checkpoint && checkpoint->value.reading) {
        restore = {SourceAnchorKind::ROW, {}, checkpoint->value.reading->position().row};
      }
    }
    auto nextDeclaration = target;
    if (!nextDeclaration.budget && window) {
      nextDeclaration.budget = window->budget();
    }
    auto created = WindowSource<T>::create(RowAccess::tree(this->scene), this->node, nextDeclaration.provider,
                                           nextDeclaration.options, nextDeclaration.budget);
    if (!created) {
      return protocol(created.error());
    }
    if (restore.kind == SourceAnchorKind::ROW && !(*created)->capabilities().seekRow) {
      return protocol(SourceError::UNSUPPORTED);
    }
    auto opened = (*created)->openConsumer();
    if (!opened) {
      return protocol(opened.error());
    }
    if (!this->active() || this->intentGeneration != generation || (*saved && !this->history->valid(**saved))) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto invalidated = RowAccess::tree(this->scene).invalidate(this->node, Invalidation::LAYOUT);
    if (!invalidated) {
      return invalidated;
    }
    auto oldWindow = std::move(window);
    auto oldConsumer = std::move(consumer);
    auto oldWorker = std::move(worker);
    auto oldPending = std::move(pending);
    auto oldDeclaration = std::move(declaration);
    window = std::move(*created);
    consumer.emplace(std::move(*opened));
    declaration = std::move(nextDeclaration);
    pending.reset();
    this->sourceCapabilities = window->capabilities();
    this->sourceStatus = WindowStatus::EMPTY;
    this->sourceFailure.reset();
    this->failure.reset();
    requestedRows = 0;
    initialLoad = true;
    loading = LoadKind::NONE;
    anchor = restore;
    direction = SourceDirection::FORWARD;
    replacementOffset = wanted = 0;
    entryKnown = restore.kind == SourceAnchorKind::ENTRY;
    lastRows = nullptr;
    lastRevision = 0;
    seenRevision = declaration.options.initialRevision;
    ++this->intentGeneration;
    this->dirty = true;
    this->blockedAttempt = false;
    if (*saved) {
      this->history->commit(std::move(**saved));
    }
    if (oldConsumer) {
      oldConsumer->close();
    }
    if (oldWindow) {
      oldWindow->close();
    }
    if (oldWorker) {
      oldWorker->cancel();
    }
    return {};
  } catch (const std::bad_alloc &) {
    return protocol(SourceError::NO_MEMORY);
  } catch (...) {
    return protocol(SourceError::CALLBACK_FAILED);
  }

  std::expected<void, TreeError> dispatch(WindowRequest<T> request) {
    auto *executor = RowAccess::app(this->scene).background();
    if (!executor) {
      pending = std::move(request);
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (!worker) {
      worker = std::make_unique<BackgroundLatest>(*executor);
    }
    auto admitted = worker->replace(std::make_unique<RowWindowJob<T>>(std::move(request)));
    if (!admitted) {
      return std::unexpected(admitted.error() == BackgroundError::NO_MEMORY ? TreeError::NO_MEMORY
                                                                            : TreeError::WOULD_BLOCK);
    }
    return {};
  }

  std::expected<void, TreeError> serviceWork() {
    if (pending) {
      auto request = std::exchange(pending, {});
      auto sent = dispatch(*request);
      if (!sent) {
        return sent;
      }
    }
    if (worker) {
      auto serviced = worker->service();
      if (!serviced) {
        return std::unexpected(serviced.error() == BackgroundError::NO_MEMORY ? TreeError::NO_MEMORY
                                                                              : TreeError::WOULD_BLOCK);
      }
    }
    auto continuation = consumer->takeRequest();
    if (!continuation) {
      return protocol(continuation.error());
    }
    if (*continuation) {
      return dispatch(**continuation);
    }
    return {};
  }

  std::expected<void, TreeError> refill() {
    SourceAnchor target = anchor;
    auto local = wanted;
    if (segmentCount && offset >= 0 && static_cast<std::size_t>(offset) < storedRows()) {
      auto id = rowAt(static_cast<std::size_t>(offset));
      if (id && window->capabilities().seekRow) {
        target = {SourceAnchorKind::ROW, {}, *id};
        local = wanted - offset;
      } else {
        std::size_t segment{};
        std::int64_t first{};
        while (segment + 1 < segmentCount &&
               offset >= first + static_cast<std::int64_t>(segments[segment].pin.size())) {
          first += static_cast<std::int64_t>(segments[segment++].pin.size());
        }
        target = {SourceAnchorKind::CURSOR, segments[segment].pin.page(0).before(), {}};
        local = wanted - first;
      }
    }
    auto requested = startLoad(target, SourceDirection::FORWARD, LoadKind::REPLACE, local);
    if (requested) {
      wanted = local;
    }
    return requested;
  }

  void integrate(WindowPin<T> pin) {
    Segment next{pin};
    if (direction == SourceDirection::FORWARD) {
      next.afterEnd = pin.edge() == SourceEdge::FINAL_END;
      next.temporaryAfter = pin.edge() == SourceEdge::TEMPORARY_TAIL;
    } else {
      next.beforeEnd = pin.edge() == SourceEdge::FINAL_END;
    }
    if (loading == LoadKind::REPLACE || !segmentCount || bufferRevision != pin.dataRevision()) {
      segments[0] = std::move(next);
      segments[1] = {};
      segmentCount = 1;
      segments[0].beforeEnd = segments[0].beforeEnd || entryKnown;
      offset = replacementOffset;
    } else if (!pin.size()) {
      if (loading == LoadKind::APPEND) {
        segments[segmentCount - 1].afterEnd = next.afterEnd;
        segments[segmentCount - 1].temporaryAfter = next.temporaryAfter;
      } else {
        segments[0].beforeEnd = next.beforeEnd;
      }
    } else if (loading == LoadKind::APPEND) {
      if (segmentCount == 2 && wanted >= static_cast<std::int64_t>(segments[0].pin.size())) {
        const auto removed = static_cast<std::int64_t>(segments[0].pin.size());
        segments[0] = std::move(segments[1]);
        segmentCount = 1;
        offset -= removed;
        wanted -= removed;
        entryKnown = false;
      }
      if (segmentCount < 2) {
        segments[segmentCount++] = std::move(next);
      }
    } else if (loading == LoadKind::PREPEND) {
      const auto added = static_cast<std::int64_t>(pin.size());
      segments[1] = std::move(segments[0]);
      segments[0] = std::move(next);
      segmentCount = 2;
      offset += added;
      wanted += added;
      entryKnown = segments[0].beforeEnd;
    }
    bufferRevision = pin.dataRevision();
    lastRows = pin.page(0).rows().data();
    lastRevision = pin.dataRevision();
    loading = LoadKind::NONE;
    ++bufferSerial;
    this->dirty = true;
    this->blockedAttempt = false;
  }

  std::expected<bool, TreeError> needDelivery() {
    if (loading != LoadKind::NONE) {
      return true;
    }
    const auto total = static_cast<std::int64_t>(storedRows());
    if (!segmentCount) {
      return true;
    }
    if (wanted < 0) {
      if (segments[0].beforeEnd) {
        wanted = 0;
        return false;
      }
      auto requested =
        startLoad({SourceAnchorKind::CURSOR, before(), {}}, SourceDirection::BACKWARD, LoadKind::PREPEND);
      return requested ? std::expected<bool, TreeError>(true) : std::unexpected(requested.error());
    }
    const auto &last = segments[segmentCount - 1];
    const bool missing = wanted >= total || static_cast<std::uint64_t>(total - wanted) < visibleRows;
    if (!missing) {
      return false;
    }
    if (last.afterEnd || last.temporaryAfter) {
      wanted = total ? std::min(wanted, total - 1) : 0;
      return false;
    }
    if (this->sourceStatus == WindowStatus::FAILED) {
      return false;
    }
    if (segmentCount == 2 && wanted < static_cast<std::int64_t>(segments[0].pin.size())) {
      this->sourceFailure = SourceError::CONTINUATION_LIMIT;
      this->sourceStatus = WindowStatus::FAILED;
      return false;
    }
    auto requested = startLoad({SourceAnchorKind::CURSOR, after(), {}}, SourceDirection::FORWARD, LoadKind::APPEND);
    return requested ? std::expected<bool, TreeError>(true) : std::unexpected(requested.error());
  }

  std::expected<void, TreeError> renderBuffer() {
    if (wanted < 0 || wanted > static_cast<std::int64_t>(storedRows())) {
      return {};
    }
    if (!this->dirty && renderedSerial == bufferSerial && offset == wanted) {
      return {};
    }
    if (this->blockedAttempt && failedSerial == bufferSerial && failedIntent == this->intentGeneration &&
        failedOffset == wanted) {
      return {};
    }
    std::vector<DeliveredRow<T>> values(0);
    values.reserve(visibleRows);
    auto skip = static_cast<std::size_t>(wanted);
    for (std::size_t segment = 0; segment < segmentCount && values.size() < visibleRows; ++segment) {
      for (std::size_t page = 0; page < segments[segment].pin.pageCount() && values.size() < visibleRows; ++page) {
        auto data = segments[segment].pin.page(page);
        if (skip >= data.rows().size()) {
          skip -= data.rows().size();
          continue;
        }
        auto owner = std::make_shared<WindowPage<T>>(data);
        for (const auto &row : data.rows().subspan(skip)) {
          if (values.size() == visibleRows) {
            break;
          }
          values.push_back({row.id, &row.value, row.valueRevision, owner});
        }
        skip = 0;
      }
    }
    failedSerial = bufferSerial;
    failedIntent = this->intentGeneration;
    failedOffset = wanted;

    struct Metadata {
      WindowRowsController &host;
      std::weak_ptr<WindowProvider<T>> provider;
      ModelResource resource;
      std::uint64_t revision;
      std::int64_t offset;
      std::uint64_t serial;
    } metadata{*this, declaration.provider, declaration.resource, bufferRevision, wanted, bufferSerial};

    return this->commitRows({}, {}, values, ++publication, 0, declaration.resource, window->stamp(), {},
                            {&metadata, [](void *context) noexcept {
                               auto &next = *static_cast<Metadata *>(context);
                               next.host.preparedProvider.swap(next.provider);
                               next.host.preparedResource = next.resource;
                               next.host.preparedRevision = next.revision;
                               next.host.offset = next.offset;
                               next.host.renderedSerial = next.serial;
                             }});
  }

  std::expected<void, TreeError> updateWindow() {
    auto focus = this->observeFocus();
    if (!focus) {
      return focus;
    }
    auto ready = initialize();
    if (!ready) {
      return ready;
    }
    const auto demand = this->viewport.width && this->viewport.height
                          ? static_cast<std::size_t>(this->viewport.height / this->options.rowHeight +
                                                     (this->viewport.height % this->options.rowHeight != 0))
                          : 0;
    if (demand > this->options.maxRows) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    if (!demand) {
      if (requestedRows) {
        (void)consumer->cancel();
        if (worker) {
          worker->cancel();
        }
        pending.reset();
        loading = LoadKind::NONE;
        requestedRows = 0;
        initialLoad = true;
      }
      if (this->rows.empty()) {
        this->dirty = false;
        return {};
      }
      auto allowed = beforeMove(0, true);
      if (!allowed) {
        return allowed;
      }
      return this->commitRows({}, {}, {}, ++publication, 0, declaration.resource, window->stamp());
    }
    visibleRows = demand;
    const auto windowRows = demand + std::min(this->options.overscanRows, this->options.maxRows - demand);
    auto revision = window->dataRevision();
    if (!revision) {
      return protocol(revision.error());
    }
    if (requestedRows != windowRows || initialLoad || *revision != seenRevision) {
      requestedRows = windowRows;
      seenRevision = *revision;
      auto requested = initialLoad ? startLoad(anchor, direction, LoadKind::REPLACE, wanted) : refill();
      if (!requested) {
        return requested;
      }
      initialLoad = false;
    }
    auto serviced = serviceWork();
    if (!serviced) {
      return serviced;
    }
    auto status = consumer->status();
    if (!status) {
      return protocol(status.error());
    }
    this->sourceStatus = *status;
    auto failure = consumer->lastError();
    if (failure) {
      this->sourceFailure = *failure;
    }
    auto recovery = consumer->recovery();
    if (recovery) {
      this->sourceRecovery = *recovery;
    }
    auto pin = consumer->pin();
    if (!pin) {
      return protocol(pin.error());
    }
    const bool readyPin = *status == WindowStatus::READY || *status == WindowStatus::FINAL_END ||
                          *status == WindowStatus::TEMPORARY_TAIL ||
                          (*status == WindowStatus::FAILED && this->sourceFailure == SourceError::CONTINUATION_LIMIT);
    if (readyPin && pin->pageCount() &&
        (loading != LoadKind::NONE || pin->page(0).rows().data() != lastRows || pin->dataRevision() != lastRevision)) {
      integrate(*pin);
    }
    if (*status == WindowStatus::LOADING || *status == WindowStatus::PENDING ||
        *status == WindowStatus::HISTORY_UNAVAILABLE) {
      return {};
    }
    auto delivery = needDelivery();
    if (!delivery) {
      return std::unexpected(delivery.error());
    }
    return *delivery ? std::expected<void, TreeError>{} : renderBuffer();
  }

  void service() noexcept override {
    if (!this->active()) {
      retire();
      return;
    }
    if (portals::detail::RowTreeAccess::dispatching(RowAccess::app(this->scene).router())) {
      return;
    }
    try {
      auto result = updateWindow();
      if (!result) {
        this->failure = result.error();
        this->blockedAttempt = true;
        this->dirty = false;
      }
    } catch (const std::bad_alloc &) {
      this->failure = TreeError::NO_MEMORY;
      this->blockedAttempt = true;
      this->dirty = false;
    } catch (...) {
      this->failure = TreeError::CALLBACK_FAILED;
      this->blockedAttempt = true;
      this->dirty = false;
    }
  }
};

/// @endcond
} // namespace portals::ui::detail

namespace portals::ui {
/** @brief Describe real bounded row subtrees, mounted between existing application frames.
 * @tparam Source Finite model or explicit shared navigation descriptor.
 * @tparam Factory Callable returning a fresh ordinary description for a weak exact-record RowContext.
 * @param source Owning cold model binding. @param factory Owning row recipe factory, invoked only for staged rows.
 * @param options Positive fixed-height and mounted-row limits. @param policy Viewport layout bounds.
 * @return Cold host recipe; failed staging preserves the previous committed window.
 */
template <class Source, class Factory>
  requires requires { typename Source::Row; } &&
           (std::same_as<Source, Model<typename Source::Row>> || std::same_as<Source, Navigation<typename Source::Row>>)
auto virtualRows(Source source, Factory factory, VirtualRowsOptions options = {},
                 SizePolicy policy = DEFAULT_VIEWPORT_POLICY) {
  using T = typename Source::Row;
  auto description = detail::Access::cold<VirtualRows<T>>(
    [source = std::move(source), factory = std::move(factory), options,
     policy](ManagedScene &scene, Node<Widget> parent,
             detail::MountContext &context) mutable -> std::expected<Node<VirtualRows<T>>, ManagedError> {
      if (options.rowHeight <= 0 || !options.maxRows || options.maxPendingRows <= 0 ||
          options.maxPendingRows > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
      }
      auto model = detail::ModelAccess::source(scene, source, context);
      if (!model) {
        return std::unexpected(model.error());
      }
      auto controller = std::make_shared<detail::RowsController<T, Source, Factory>>(scene, source, std::move(factory),
                                                                                     context.controls);
      controller->options = options;
      controller->layoutLease = detail::RowAccess::layoutLease(scene);
      controller->initialModel = *model;
      auto node = scene.emplace<VirtualRows<T>>(parent, controller, policy);
      if (!node) {
        return std::unexpected(node.error());
      }
      controller->node = *node;
      detail::ModelAccess::keep(scene, *node, context);
      if constexpr (std::same_as<Source, Navigation<T>>) {
        auto navigation = detail::ModelAccess::resolve(scene, source, context);
        if (!navigation) {
          return std::unexpected(navigation.error());
        }
        auto registered = controller->registerNavigation(*navigation);
        if (!registered) {
          return std::unexpected(detail::error(*node, registered.error()));
        }
      }
      auto bound = controller->bindInput();
      if (!bound) {
        return std::unexpected(detail::error(*node, bound.error()));
      }
      auto admitted = detail::RowAccess::admit(scene, controller);
      return admitted ? node : std::unexpected(admitted.error());
    });
  return detail::Access::configuredControl(std::move(description), "VirtualRows");
}

/** @brief Describe real delegated rows from native cursor delivery without a finite extent.
 * @tparam T Immutable page row. @tparam Factory Fresh ordinary recipe factory receiving an exact-record weak context.
 * @param source Native window capability descriptor. @param factory Owning row factory.
 * @param options Fixed-height and bounded subtree/edit settings. @param policy Viewport bounds.
 * @return Cold host using the existing application worker and WindowSource delivery engine.
 */
template <class T, class Factory>
auto virtualRows(WindowRowsModel<T> source, Factory factory, VirtualRowsOptions options = {},
                 SizePolicy policy = DEFAULT_VIEWPORT_POLICY) {
  auto description = detail::Access::cold<VirtualRows<T>>(
    [source = std::move(source), factory = std::move(factory), options,
     policy](ManagedScene &scene, Node<Widget> parent,
             detail::MountContext &context) mutable -> std::expected<Node<VirtualRows<T>>, ManagedError> {
      if (!source.provider || source.failure || !source.resource.identity.instance || !source.resource.identity.epoch ||
          !source.resource.lineage || options.rowHeight <= 0 || !options.maxRows || options.maxPendingRows <= 0 ||
          options.maxPendingRows > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(detail::error(parent, source.failure ? detail::rowSourceError(*source.failure)
                                                                    : TreeError::INVALID_ARGUMENT));
      }
      auto background = detail::RowAccess::background(scene);
      if (!background) {
        return std::unexpected(background.error());
      }
      auto controller = std::make_shared<detail::WindowRowsController<T, Factory>>(
        scene, std::move(source), std::move(factory), context.controls);
      controller->options = options;
      controller->layoutLease = detail::RowAccess::layoutLease(scene);
      auto node = scene.emplace<VirtualRows<T>>(parent, controller, policy);
      if (!node) {
        return std::unexpected(node.error());
      }
      controller->node = *node;
      detail::ModelAccess::keep(scene, *node, context);
      auto bound = controller->bindInput();
      if (!bound) {
        return std::unexpected(detail::error(*node, bound.error()));
      }
      auto admitted = detail::RowAccess::admit(scene, controller);
      return admitted ? node : std::unexpected(admitted.error());
    });
  return detail::Access::configuredControl(std::move(description), "VirtualRows");
}
} // namespace portals::ui
