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
  std::optional<ActionBinding> focusBinding;
  std::shared_ptr<ScrollController> scrollController;
  std::uint64_t scrollRevision{};
  std::optional<std::uint64_t> desiredCurrent;
  std::optional<RowId> desiredCurrentRow;

  struct EnterIntent {
    ModelResource resource;
    std::uint64_t focusGeneration{};
  };

  std::optional<EnterIntent> pendingEnter;
  std::size_t retainedColumn{};
  std::optional<EditFieldKey> retainedField;
  bool selecting{};
  std::optional<RowId> restoredCurrent;
  std::vector<std::shared_ptr<RowAssignment<T>>> rows = std::vector<std::shared_ptr<RowAssignment<T>>>(0);
  std::shared_ptr<RowAssignment<T>> editPin;
  std::weak_ptr<portals::detail::ManagedModelSourceOf<T>> previousSource;
  std::uint64_t revision{};
  std::uint64_t dataRevision{};
  std::uint64_t desiredFirst{};
  CellSize viewport;
  bool dirty{true};
  std::optional<bool> desiredAutoHeight;
  std::uint64_t measuredGeometryRevision{};
  std::uint64_t blockedGeometryRevision{};
  bool recordHasBefore{};
  bool recordHasAfter{};
  bool allowEndBackfill{};
  bool lastPage{};
  std::int32_t desiredCellOffset{};
  std::int64_t pendingCells{};
  bool offsetFromEnd{};
  bool awaitingCellMove{};
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
  std::shared_ptr<ModelRange<T>> pendingViewport;
  std::weak_ptr<portals::detail::ManagedModelSourceOf<T>> pendingViewportSource;
  RowRange pendingViewportRows;
  std::uint64_t pendingViewportRevision{};

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

  std::expected<void, TreeError> prepareChrome() {
    auto glyph = GraphemeText::from(">", scene.widthPolicy());
    if (!glyph) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    this->currentGlyph = std::move(*glyph);
    auto scope = RowAccess::app(scene).router().focusScope(node, node);
    if (!scope) {
      return std::unexpected(scope.error());
    }
    focusBinding.emplace(std::move(*scope));
    auto composite = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node);
    if (!composite) {
      return composite;
    }
    if (options.scrollbar) {
      auto controller = ScrollController::create();
      if (!controller) {
        return std::unexpected(controller.error());
      }
      scrollController = *controller;
      auto bar = scene.scrollBar(node, scrollController);
      if (!bar) {
        return std::unexpected(EditAccess::cause(bar.error()));
      }
      this->scrollbar = *bar;
      auto chrome = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node, {}, *bar);
      if (!chrome) {
        return chrome;
      }
      auto hidden = RowAccess::tree(scene).setVisible(*bar, false);
      if (!hidden) {
        return hidden;
      }
    }
    return {};
  }

  EventReply handleInput(const RoutedEvent &event) {
    const auto *mouse = std::get_if<MouseInputEvent>(&event);
    const auto *key = std::get_if<KeyEvent>(&event);
    const auto *action = std::get_if<ActionEvent>(&event);
    if ((key && key->action != KeyAction::RELEASE && key->key == InputKey::F2) ||
        (action && action->action == portals::Action::ACTIVATE)) {
      auto entered = this->enterEditing();
      if (!entered) {
        this->failure = entered.error();
      } else {
        this->failure.reset();
      }
      return EventReply::HANDLED;
    }
    if (action && action->action == portals::Action::CANCEL) {
      auto left = this->leaveEditing();
      if (!left) {
        this->failure = left.error();
      } else {
        this->failure.reset();
      }
      return EventReply::HANDLED;
    }
    const bool wheel = mouse && mouse->action == MouseAction::SCROLL &&
                       (mouse->button == MouseButton::WHEEL_UP || mouse->button == MouseButton::WHEEL_DOWN);
    const bool page =
      key && key->action != KeyAction::RELEASE && (key->key == InputKey::PAGE_UP || key->key == InputKey::PAGE_DOWN);
    if (wheel || page) {
      pendingEnter.reset();
      const bool backwards = wheel ? mouse->button == MouseButton::WHEEL_UP : key->key == InputKey::PAGE_UP;
      if (wheel &&
          (mouse->modifiers.alt || mouse->modifiers.control || mouse->modifiers.super || mouse->modifiers.shift)) {
        return EventReply::IGNORED;
      }
      const auto step = static_cast<std::int64_t>(std::max<std::size_t>(1, this->rows.size()));
      auto changed = page ? (this->natural() || this->position.editing ? this->requestPage(backwards)
                                                                       : this->moveCurrent(backwards ? -step : step))
                          : this->requestWheel(mouse->wheelDelta, backwards);
      if (!changed) {
        this->failure = changed.error();
      }
      return EventReply::HANDLED;
    }
    if (!action || (action->action != portals::Action::MOVE_UP && action->action != portals::Action::MOVE_DOWN &&
                    action->action != portals::Action::MOVE_HOME && action->action != portals::Action::MOVE_END)) {
      return EventReply::IGNORED;
    }
    auto focused = RowAccess::app(this->scene).router().focused();
    if (!focused || *focused != this->node) {
      // An editor owns its arrow grammar. An ignored direction still belongs to
      // the row, rather than becoming a spatial jump to an unrelated outer control.
      return EventReply::HANDLED;
    }
    std::expected<void, TreeError> changed;
    pendingEnter.reset();
    if (action->action == portals::Action::MOVE_UP) {
      changed = this->moveCurrent(-1);
    } else if (action->action == portals::Action::MOVE_DOWN) {
      changed = this->moveCurrent(1);
    } else if (action->action == portals::Action::MOVE_HOME) {
      changed = this->sourceCapabilities ? this->selectAnchor({}, SourceDirection::FORWARD) : this->selectCurrent(0);
    } else if (this->exactCount) {
      changed = this->selectCurrent(*this->exactCount ? *this->exactCount - 1 : 0, true);
    } else {
      if (this->sourceCapabilities && (!this->sourceCapabilities->tail || !this->sourceCapabilities->backward)) {
        return EventReply::HANDLED;
      }
      changed = this->selectAnchor({SourceAnchorKind::TAIL, {}, {}}, SourceDirection::BACKWARD);
    }
    if (!changed) {
      this->failure = changed.error();
    }
    return EventReply::HANDLED;
  }

  std::expected<void, TreeError> bindInput() {
    auto prepared = prepareChrome();
    if (!prepared) {
      return prepared;
    }
    auto binding = RowAccess::app(scene).router().bind(
      node, [weak = this->weak_from_this()](VirtualRows<T> &, const RoutedEvent &event) {
        auto owner = weak.lock();
        return owner && owner->active() ? owner->handleInput(event) : EventReply::IGNORED;
      });
    if (!binding) {
      return std::unexpected(binding.error());
    }
    inputBinding.emplace(std::move(*binding));
    return {};
  }

  std::expected<void, TreeError> publishPosition() {
    if (this->positionState) {
      auto changed = this->positionState->value.set(this->position);
      if (!changed) {
        return std::unexpected(changed.error());
      }
    }
    return {};
  }

  std::expected<void, TreeError> enterEditing() {
    if (rows.empty() || !this->position.current) {
      return {};
    }
    const auto found = std::ranges::find(rows, *this->position.current, [](const auto &row) {
      return row->id;
    });
    if (found == rows.end()) {
      const auto generation = portals::detail::RowTreeAccess::focusGeneration(RowAccess::app(scene).router());
      if (!generation) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto revealed = moveCurrent(0);
      if (revealed && active() &&
          generation == portals::detail::RowTreeAccess::focusGeneration(RowAccess::app(scene).router())) {
        pendingEnter = EnterIntent{this->position.resource, generation};
      }
      return revealed;
    }
    if (retainedField) {
      for (const auto &weak : (*found)->edit->bindings) {
        auto binding = weak.lock();
        if (!binding || binding->closed) {
          continue;
        }
        const auto definition = std::ranges::find_if((*found)->edit->definitions, [&](const auto &entry) {
          return entry.get() == binding->definition && entry->key == *retainedField;
        });
        auto eligible = RowAccess::tree(scene).inputEligible(binding->receiver);
        if (definition != (*found)->edit->definitions.end() && eligible && *eligible) {
          auto local = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node, (*found)->root);
          auto focused = local ? RowAccess::app(scene).router().setFocus(binding->receiver) : local;
          if (!focused) {
            return focused;
          }
          this->position.editing = true;
          return publishPosition();
        }
      }
    }
    auto entered =
      portals::detail::RowTreeAccess::enterRow(RowAccess::app(scene).router(), node, (*found)->root, retainedColumn);
    if (!entered) {
      return std::unexpected(entered.error());
    }
    this->position.editing = *entered != node;
    return publishPosition();
  }

  std::expected<void, TreeError> leaveEditing() {
    pendingEnter.reset();
    auto observed = observeFocus();
    if (!observed) {
      return observed;
    }
    for (const auto &row : rows) {
      if (focusedNode == node || !RowAccess::tree(scene).contains(row->root, focusedNode).value_or(false)) {
        continue;
      }
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
    auto collapsed = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node);
    if (!collapsed) {
      return collapsed;
    }
    auto focused = RowAccess::app(scene).router().setFocus(node);
    if (!focused) {
      return focused;
    }
    this->position.editing = false;
    return publishPosition();
  }

  std::expected<void, TreeError> selectCurrent(std::uint64_t target, bool endPage = false) {
    if (!this->exactCount) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    target = *this->exactCount ? std::min(target, *this->exactCount - 1) : 0;
    const auto visible = std::max<std::uint64_t>(1, natural() ? rows.size() : viewport.height / options.rowHeight);
    auto nextFirst = desiredFirst;
    if (target < nextFirst) {
      nextFirst = target;
    } else if (target - nextFirst >= visible) {
      nextFirst = natural() ? target : target - visible + 1;
    }
    auto allowed = requestFirst(nextFirst);
    if (!allowed) {
      return allowed;
    }
    desiredCurrent = target;
    lastPage = natural() && endPage;
    desiredCellOffset = 0;
    pendingCells = 0;
    offsetFromEnd = false;
    selecting = true;
    return {};
  }

  virtual std::expected<void, TreeError> moveCurrent(std::int64_t delta) {
    const auto current = desiredCurrent.value_or(this->position.currentIndex.value_or(first));
    const auto amount = delta < 0 ? static_cast<std::uint64_t>(-(delta + 1)) + 1 : static_cast<std::uint64_t>(delta);
    if (delta < 0) {
      return selectCurrent(current - std::min(current, amount));
    }
    return selectCurrent(current + std::min(amount, std::numeric_limits<std::uint64_t>::max() - current));
  }

  std::expected<void, TreeError> selectAnchor(SourceAnchor anchor, SourceDirection direction) {
    auto admitted = this->requestAnchor(anchor, direction);
    if (!admitted) {
      return admitted;
    }
    this->currentSlot = direction == SourceDirection::BACKWARD ? options.maxRows - 1 : 0;
    selecting = true;
    return {};
  }

  std::expected<void, TreeError> syncScrollBar() {
    if (!scrollController) {
      return {};
    }
    auto snapshot = scrollController->snapshot();
    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }
    if (natural() && this->exactCount == std::optional<std::uint64_t>{1} && placements.size() == 1) {
      if (scrollRevision && snapshot->revision != scrollRevision) {
        auto changed = requestCells(static_cast<std::int64_t>(snapshot->vertical.offset.raster) - desiredCellOffset);
        if (!changed) {
          return changed;
        }
      }
      scrollRevision = snapshot->revision;
      return {};
    }
    if (scrollRevision && snapshot->revision != scrollRevision && this->exactCount &&
        snapshot->vertical.offset.raster != desiredFirst) {
      auto admitted = requestFirst(snapshot->vertical.offset.raster);
      if (!admitted) {
        // The bar has already published its user intent. A checked editor refusal
        // restores only that presentation; the committed viewport and draft never move.
        auto restored = setScrollOffset(RowAccess::tree(scene), this->scrollbar, ScrollAxis::VERTICAL, {first});
        if (restored) {
          auto current = scrollController->snapshot();
          if (current) {
            scrollRevision = current->revision;
          }
        }
        return admitted;
      }
    }
    scrollRevision = snapshot->revision;
    return {};
  }

  void updateOrientation(ModelResource resource) {
    auto next = this->position;
    const bool same = next.resource == resource;
    next.resource = resource;
    next.count = this->exactCount;
    next.firstIndex = this->exactCount ? std::optional(first) : std::nullopt;
    next.visible = rows.size();
    next.windowStatus = this->sourceStatus;
    this->visibleCurrentSlot.reset();
    if (this->exactCount) {
      if (*this->exactCount) {
        const auto index = std::min(desiredCurrent.value_or(first), *this->exactCount - 1);
        next.current = desiredCurrentRow;
        next.currentIndex = next.current ? std::optional(index) : std::nullopt;
        if (index >= first && index - first < rows.size()) {
          next.current = rows[static_cast<std::size_t>(index - first)]->id;
          this->currentSlot = static_cast<std::size_t>(index - first);
          this->visibleCurrentSlot = this->currentSlot;
        }
      } else {
        next.current.reset();
        next.currentIndex.reset();
      }
      recordHasBefore = first > 0;
      recordHasAfter = first + rows.size() < *this->exactCount;
      next.hasBefore = recordHasBefore || firstCellOffset > 0;
      next.hasAfter =
        recordHasAfter || (!placements.empty() &&
                           static_cast<std::int64_t>(placements.back().y) + placements.back().height > viewport.height);
    } else {
      next.currentIndex.reset();
      if (!same) {
        this->currentSlot = 0;
        next.current = restoredCurrent;
        restoredCurrent.reset();
      }
      const auto found = next.current ? std::ranges::find(rows, *next.current,
                                                          [](const auto &row) {
                                                            return row->id;
                                                          })
                                      : rows.end();
      if (selecting && !rows.empty()) {
        this->currentSlot = std::min(this->currentSlot, rows.size() - 1);
        next.current = rows[this->currentSlot]->id;
        this->visibleCurrentSlot = this->currentSlot;
      } else if (found != rows.end()) {
        this->currentSlot = static_cast<std::size_t>(found - rows.begin());
        this->visibleCurrentSlot = this->currentSlot;
      } else if (!rows.empty() && !next.current) {
        this->currentSlot = std::min(this->currentSlot, rows.size() - 1);
        next.current = rows[this->currentSlot]->id;
        this->visibleCurrentSlot = this->currentSlot;
      } else if (rows.empty() && !same) {
        next.current.reset();
      }
    }
    this->position = next;
    selecting = false;
  }

  std::expected<void, TreeError> admitCurrent(std::optional<RowId> row, ModelResource resource) {
    if constexpr (std::same_as<Source, Navigation<T>>) {
      if (ModelAccess::bindings(source).current) {
        auto navigation = source.managed();
        if (!navigation) {
          return std::unexpected(EditAccess::cause(navigation.error()));
        }
        auto channel = navigation->current();
        if (!channel) {
          return std::unexpected(channel.error());
        }
        auto current = channel->snapshot();
        if (!current) {
          return std::unexpected(current.error());
        }
        if ((*current)->value.resource != resource) {
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
        if (observedCurrent && *observedCurrent != (*current)->value && (*current)->value.row != row) {
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
        if ((*current)->value.row == row) {
          observedCurrent = (*current)->value;
          return {};
        }
        auto moved = row ? navigation->navigate(*row) : navigation->clearCurrent();
        if (!moved) {
          return moved;
        }
        auto accepted = channel ? channel->snapshot()
                                : std::expected<std::shared_ptr<const PropertyChange<NavigationPosition>>, TreeError>(
                                    std::unexpected(channel.error()));
        if (!accepted) {
          return std::unexpected(accepted.error());
        }
        if ((*accepted)->value.resource == resource && (*accepted)->value.row == row) {
          observedCurrent = (*accepted)->value;
        }
      }
    } else {
      (void)row;
      (void)resource;
    }
    return {};
  }

  std::expected<void, TreeError> publishScrollBar() {
    if (scrollController && this->exactCount) {
      if (natural() && *this->exactCount == 1 && placements.size() == 1) {
        auto metrics = setScrollMetrics(
          RowAccess::tree(scene), this->scrollbar,
          {static_cast<std::uint64_t>(placements.front().height), static_cast<std::uint64_t>(viewport.height)});
        if (!metrics) {
          return metrics;
        }
        auto moved = setScrollOffset(RowAccess::tree(scene), this->scrollbar, ScrollAxis::VERTICAL,
                                     {static_cast<std::uint64_t>(firstCellOffset)});
        if (!moved) {
          return moved;
        }
        auto snapshot = scrollController->snapshot();
        if (!snapshot) {
          return std::unexpected(snapshot.error());
        }
        scrollRevision = snapshot->revision;
        return {};
      }
      auto metrics = setScrollMetrics(RowAccess::tree(scene), this->scrollbar,
                                      {*this->exactCount, static_cast<std::uint64_t>(rows.size())});
      if (!metrics) {
        return metrics;
      }
      auto offset = setScrollOffset(RowAccess::tree(scene), this->scrollbar, ScrollAxis::VERTICAL, {first});
      if (!offset) {
        return offset;
      }
      auto snapshot = scrollController->snapshot();
      if (!snapshot) {
        return std::unexpected(snapshot.error());
      }
      scrollRevision = snapshot->revision;
    }
    return {};
  }

  std::expected<void, TreeError> publishOrientation(ModelResource resource, bool announce = true) {
    updateOrientation(resource);
    if (!announce) {
      return {};
    }
    auto bar = publishScrollBar();
    return bar ? publishPosition() : bar;
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

  virtual bool acceptedResource(ModelResource resource) {
    if (!active()) {
      return false;
    }
    auto actual = currentSource();
    return actual && (*actual)->navigationResource == resource;
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
    pendingViewport.reset();
    for (auto &row : rows) {
      row->close();
    }
    if (editPin) {
      editPin->close();
    }
  }

  bool natural() const noexcept {
    return desiredAutoHeight.value_or(options.autoRowHeight);
  }

  std::expected<void, TreeError> autoHeight(bool enabled) override {
    if (!active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (natural() == enabled) {
      return {};
    }
    if (!enabled && std::ranges::any_of(placements, [&](const auto &value) {
          return value.height > options.rowHeight;
        })) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::LAYOUT);
    if (!invalidated) {
      return invalidated;
    }
    desiredAutoHeight = enabled;
    desiredCellOffset = 0;
    pendingCells = 0;
    offsetFromEnd = false;
    ++intentGeneration;
    dirty = true;
    blockedAttempt = false;
    return {};
  }

  std::size_t rowDemand() const noexcept {
    if (viewport.width <= 0 || viewport.height <= 0) {
      return 0;
    }
    return natural() ? std::min(options.maxRows, static_cast<std::size_t>(viewport.height))
                     : static_cast<std::size_t>(viewport.height / options.rowHeight +
                                                (viewport.height % options.rowHeight != 0));
  }

  std::int32_t contentWidth(bool bar) const noexcept {
    return std::max(0, viewport.width - static_cast<std::int32_t>(options.currentIndicator) -
                         static_cast<std::int32_t>(bar));
  }

  std::expected<void, TreeError> refreshNaturalLayout() {
    if (!natural() || dirty || rows.empty() || !rowDemand()) {
      return {};
    }
    auto styled = RowAccess::flushStyles(scene);
    if (!styled) {
      return styled;
    }
    auto geometry = portals::detail::RowTreeAccess::geometryRevision(RowAccess::tree(scene));
    if (!geometry) {
      return std::unexpected(geometry.error());
    }
    if (*geometry == measuredGeometryRevision) {
      return {};
    }
    std::int64_t y = -firstCellOffset;
    for (std::size_t index = 0; index < rows.size(); ++index) {
      auto size = portals::detail::RowTreeAccess::measure(RowAccess::tree(scene), rows[index]->root,
                                                          {contentWidth(scrollbarVisible), viewport.height});
      if (!size) {
        return std::unexpected(size.error());
      }
      const auto height = std::max(1, size->height);
      if (placements[index].height != height || placements[index].y != y) {
        auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::LAYOUT);
        if (!invalidated) {
          return invalidated;
        }
        dirty = true;
        return {};
      }
      y += height;
    }
    measuredGeometryRevision = *geometry;
    return {};
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
    const auto visible = rowDemand();
    auto allowed = preflight(target, visible);
    if (!allowed) {
      return allowed;
    }
    auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::PAINT);
    if (!invalidated) {
      return invalidated;
    }
    if (desiredFirst != target) {
      desiredCellOffset = 0;
      offsetFromEnd = false;
    }
    desiredFirst = target;
    lastPage = false;
    allowEndBackfill = true;
    ++intentGeneration;
    dirty = true;
    return {};
  }

  virtual bool rowMovePending() const noexcept {
    return desiredFirst != first;
  }

  std::expected<void, TreeError> requestCells(std::int64_t delta) {
    if (!active()) {
      return std::unexpected(TreeError::CLOSED);
    }
    const auto limit = options.maxPendingRows;
    const auto carried = offsetFromEnd ? desiredCellOffset : 0;
    if (delta < -limit - pendingCells - carried || delta > limit - pendingCells - carried) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::LAYOUT);
    if (!invalidated) {
      return invalidated;
    }
    pendingCells += delta;
    allowEndBackfill = true;
    dirty = true;
    blockedAttempt = false;
    ++intentGeneration;
    return {};
  }

  std::expected<void, TreeError> applyCellIntent() {
    if (!natural() || !pendingCells || rows.empty() || rowMovePending() || awaitingCellMove || lastPage) {
      return {};
    }
    auto remaining = pendingCells;
    auto offset = static_cast<std::int64_t>(desiredCellOffset);
    std::int64_t movement{};
    bool fromEnd{};
    if (remaining < 0) {
      const auto consumed = std::min(-remaining, offset);
      offset -= consumed;
      remaining += consumed;
      if (remaining && recordHasBefore) {
        movement = -1;
        offset = remaining;
        remaining = 0;
        fromEnd = true;
      } else if (remaining) {
        remaining = 0;
      }
    } else {
      if (!recordHasAfter && !placements.empty()) {
        const auto bottom = static_cast<std::int64_t>(placements.back().y) + placements.back().height;
        remaining = std::min(remaining, std::max<std::int64_t>(0, bottom - viewport.height));
      }
      std::size_t index{};
      while (remaining && index < placements.size()) {
        const auto available = static_cast<std::int64_t>(placements[index].height) - offset;
        if (remaining < available) {
          offset += remaining;
          remaining = 0;
          break;
        }
        if (index + 1 == placements.size() && !recordHasAfter) {
          offset = placements[index].height - 1;
          remaining = 0;
          break;
        }
        remaining -= available;
        offset = 0;
        ++movement;
        ++index;
      }
    }
    if (movement) {
      auto moved = requestMove(movement);
      if (!moved) {
        return moved;
      }
      awaitingCellMove = rowMovePending();
    }
    pendingCells = remaining;
    desiredCellOffset = static_cast<std::int32_t>(offset);
    offsetFromEnd = fromEnd;
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
    auto moved =
      natural() ? requestCells(static_cast<std::int64_t>(whole)) : requestMove(static_cast<std::int64_t>(whole));
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
    if (natural()) {
      return requestCells(backwards ? -viewport.height : viewport.height);
    }
    const auto step = std::max<std::uint64_t>(1, rowDemand());
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
    if (natural() && this->position.editing && !rowMovePending() && !pendingCells && !rows.empty()) {
      try {
        auto focused = RowAccess::app(scene).router().focused();
        auto hostGeometry = RowAccess::tree(scene).inspect(node);
        if (focused && *focused != node && hostGeometry && hostGeometry->geometry.valid) {
          auto child = RowAccess::tree(scene).inspect(*focused);
          if (child && child->geometry.valid) {
            const auto &host = hostGeometry->geometry.surface;
            const auto &bounds = child->geometry.surface;
            const auto hostTop = host.origin().y;
            const auto hostBottom = static_cast<std::int64_t>(hostTop) + host.size().height;
            const auto childTop = bounds.origin().y;
            const auto childBottom = static_cast<std::int64_t>(childTop) + bounds.size().height;
            const auto delta = childTop < hostTop         ? static_cast<std::int64_t>(childTop) - hostTop
                               : childBottom > hostBottom ? childBottom - hostBottom
                                                          : 0;
            if (delta && bounds.size().height <= host.size().height) {
              auto admitted = requestCells(delta);
              if (!admitted) {
                failure = admitted.error();
              }
            }
          }
        }
      } catch (const std::bad_alloc &) {
        failure = TreeError::NO_MEMORY;
      } catch (...) {
        failure = TreeError::CALLBACK_FAILED;
      }
    }
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
    auto root = scene.emplace<SizedWidget>(node, natural() ? DEFAULT_FLEX_WIDTH_POLICY : DEFAULT_VIEWPORT_POLICY);
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
    this->hasFocus = *focused == node ||
                     (*focused != Node<Widget>{} && RowAccess::tree(scene).contains(node, *focused).value_or(false));
    auto editing = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto &row = rows[index];
      if (*focused != Node<Widget>{} && RowAccess::tree(scene).contains(row->root, *focused).value_or(false)) {
        auto column = portals::detail::RowTreeAccess::column(RowAccess::app(scene).router(), row->root, *focused);
        if (!column) {
          return std::unexpected(column.error());
        }
        retainedColumn = *column;
        retainedField.reset();
        for (const auto &weak : row->edit->bindings) {
          auto binding = weak.lock();
          if (!binding || binding->closed || binding->receiver != *focused) {
            continue;
          }
          const auto definition = std::ranges::find_if(row->edit->definitions, [&](const auto &entry) {
            return entry.get() == binding->definition && entry->key.persistent();
          });
          if (definition != row->edit->definitions.end()) {
            retainedField = (*definition)->key;
          }
        }
        this->currentSlot = index;
        dirty = dirty || this->position.current != row->id;
        selecting = selecting || this->position.current != row->id;
        if (this->exactCount) {
          desiredCurrent = first + index;
        }
        auto local = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node, row->root);
        if (!local) {
          return local;
        }
        editing = true;
        break;
      }
    }
    if (!editing) {
      auto collapsed = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node);
      if (!collapsed) {
        return collapsed;
      }
    }
    this->position.editing = editing;
    return publishPosition();
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
    if (natural()) {
      checkpoint.readingCellOffset = firstCellOffset;
    }
    if (this->position.current && this->position.currentIndex) {
      auto bookmark = ModelBookmark::create(previous->navigationResource,
                                            {PositionKind::ITEM, *this->position.current, *this->position.currentIndex,
                                             dataRevision, PositionAffinity::FORWARD},
                                            {}, options.checkpoints.bookmarks);
      if (!bookmark) {
        return std::unexpected(portals::detail::navigationTreeError(bookmark.error()));
      }
      checkpoint.current = *bookmark;
    }
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
    desiredCurrentRow.reset();
    auto target = switched ? std::uint64_t{} : desiredFirst;
    if (switched) {
      desiredCellOffset = 0;
      pendingCells = 0;
      lastPage = false;
      allowEndBackfill = false;
      desiredCurrent = 0;
    } else if (!selecting && this->position.current) {
      auto located = frame.locateRow(*this->position.current);
      if (!located) {
        return std::unexpected(located.error());
      }
      if (located->state == RowLookupState::FOUND) {
        desiredCurrent = located->index;
        desiredCurrentRow = this->position.current;
      } else {
        desiredCurrent = frame.rowCount() ? std::min(this->position.currentIndex.value_or(0), frame.rowCount() - 1) : 0;
      }
    }
    if (switched && history && !resetRestoration) {
      auto saved = history->find(actual, historyRole);
      if (saved) {
        target = saved->value.first;
        desiredCellOffset = natural() ? saved->value.readingCellOffset.value_or(0) : 0;
        if (saved->value.current) {
          auto resolver = FinitePositionResolver<T>::create(actual->navigationResource, incarnation, frame.snapshot(),
                                                            actual->navigationChanges);
          if (!resolver) {
            return std::unexpected(portals::detail::navigationTreeError(resolver.error()));
          }
          auto restored = resolver->resolve({incarnation, frame.dataRevision(), 1, *saved->value.current}, {});
          if (!restored) {
            return std::unexpected(portals::detail::navigationTreeError(restored.error()));
          }
          if (restored->state == PositionResolution::EXACT || restored->state == PositionResolution::NEAREST) {
            desiredCurrent = restored->position.offset;
            desiredCurrentRow = restored->position.row;
          }
        }
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
          if (!reading && found->state == RowLookupState::FOUND) {
            desiredCurrent = found->index;
            desiredCurrentRow = position.row;
          }
          const auto visible = static_cast<std::uint64_t>(rowDemand());
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

  std::expected<std::optional<ModelRange<T>>, TreeError>
  readFiniteRange(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual, const ModelFrame<T> &frame,
                  RowRange requested) {
    auto range = frame.read(requested);
    if (!range) {
      return std::unexpected(range.error());
    }
    if (range->state() == RangeState::PENDING) {
      if (consumerSource.lock() != actual) {
        auto opened = actual->model->openRange();
        if (!opened) {
          return std::unexpected(opened.error());
        }
        finiteRequest.reset();
        finiteConsumer.emplace(std::move(*opened));
        consumerSource = actual;
      }
      const bool sameRequest = finiteRequest && finiteRevision == frame.dataRevision() &&
                               finiteRange.first == requested.first && finiteRange.count == requested.count;
      if (sameRequest) {
        auto completed = frame.range(finiteConsumer->id());
        if (!completed) {
          return std::unexpected(completed.error());
        }
        range = std::move(completed);
      } else {
        auto action = actual->preparation;
        if (!action) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        auto work = finiteConsumer->request(requested);
        if (!work) {
          return std::unexpected(work.error());
        }
        finiteRange = requested;
        finiteRevision = frame.dataRevision();
        finiteRequest = std::move(*work);
        auto sent = (*action)(frame, finiteRequest);
        if (!sent) {
          return std::unexpected(sent.error());
        }
        return std::optional<ModelRange<T>>{};
      }
    }
    if (range->state() == RangeState::PENDING) {
      return std::optional<ModelRange<T>>{};
    }
    if (range->state() != RangeState::READY) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
    return std::optional<ModelRange<T>>(std::move(*range));
  }

  std::expected<bool, TreeError>
  prepareFiniteCurrent(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual,
                       const ModelFrame<T> &frame, std::uint64_t target, std::span<const DeliveredRow<T>> values) {
    if (!frame.rowCount()) {
      desiredCurrentRow.reset();
      return true;
    }
    const auto index = std::min(desiredCurrent.value_or(target), frame.rowCount() - 1);
    desiredCurrent = index;
    if (index >= target && index - target < values.size()) {
      desiredCurrentRow = values[static_cast<std::size_t>(index - target)].id;
      return true;
    }
    if (desiredCurrentRow) {
      return true;
    }
    // Resolve one qualified record independently of the visible page. The existing
    // single range consumer handles PENDING; no synchronous provider I/O or scan is added.
    auto point = readFiniteRange(actual, frame, {index, 1});
    if (!point) {
      return std::unexpected(point.error());
    }
    if (!*point) {
      return false;
    }
    desiredCurrentRow = (**point).rows().front().id;
    return true;
  }

  std::expected<void, TreeError> update() {
    auto scrolled = syncScrollBar();
    if (!scrolled) {
      return scrolled;
    }
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
        attempt.intent == intentGeneration && attempt.viewport == viewport &&
        (!natural() || blockedGeometryRevision ==
                         portals::detail::RowTreeAccess::geometryRevision(RowAccess::tree(scene)).value_or(0))) {
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
    const auto requestedFirst = *restored;
    auto target = requestedFirst;
    if (natural() && allowEndBackfill && count - target < rowDemand()) {
      target = count - std::min<std::uint64_t>(count, rowDemand());
    }
    const auto leadingRows = static_cast<std::size_t>(requestedFirst - target);
    const auto visible = viewport.width && viewport.height
                           ? static_cast<std::size_t>(std::min<std::uint64_t>(count - target, rowDemand()))
                           : 0;
    if (visible > options.maxRows) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    if (!dirty && previousSource.lock() == *actual && revision == frame->revision() && target == first) {
      auto measured = refreshNaturalLayout();
      if (!measured) {
        return measured;
      }
    }
    if (!dirty && previousSource.lock() == *actual && revision == frame->revision() && target == first &&
        desiredCurrent == this->position.currentIndex) {
      return {};
    }
    auto allowed = preflight(target, visible);
    if (!allowed) {
      return allowed;
    }
    if (!visible) {
      pendingViewport.reset();
      return commitRows(*actual, {}, {}, frame->revision(), target, (*actual)->navigationResource, *incarnation,
                        std::move(*saved));
    }
    auto pin = pendingViewport;
    if (!pin || pendingViewportSource.lock() != *actual || pendingViewportRevision != frame->dataRevision() ||
        pendingViewportRows.first != target || pendingViewportRows.count != visible) {
      pendingViewport.reset();
      auto range = readFiniteRange(*actual, *frame, {target, visible});
      if (!range) {
        return std::unexpected(range.error());
      }
      if (!*range) {
        return {};
      }
      pin = std::make_shared<ModelRange<T>>(std::move(**range));
    }
    attempt = {*actual, frame->revision(), intentGeneration, viewport};
    std::vector<DeliveredRow<T>> values(0);
    values.reserve(visible);
    for (const auto &value : pin->rows()) {
      values.push_back({value.id, &value.value, frame->dataRevision()});
    }
    auto current = prepareFiniteCurrent(*actual, *frame, target, values);
    if (!current) {
      return std::unexpected(current.error());
    }
    if (!*current) {
      // Retain this one bounded ready viewport while the same consumer resolves
      // an offscreen current; otherwise two PENDING intervals could replace each other forever.
      pendingViewport = pin;
      pendingViewportSource = *actual;
      pendingViewportRows = {target, visible};
      pendingViewportRevision = frame->dataRevision();
      return {};
    }
    pendingViewport.reset();
    return commitRows(*actual, pin, values, frame->revision(), target, (*actual)->navigationResource, *incarnation,
                      std::move(*saved), {}, leadingRows, target + values.size() == count, target > 0);
  }

  struct NaturalGeometry {
    std::vector<RowPlacement> placements = std::vector<RowPlacement>(0);
    std::size_t first{};
    std::int32_t offset{};
  };

  std::expected<NaturalGeometry, TreeError>
  measureNaturalRows(std::span<const std::shared_ptr<RowAssignment<T>>> staging, std::size_t leadingRows,
                     bool finalWindow, bool reserveBar, bool beforeWindow) {
    NaturalGeometry result;
    result.placements.reserve(staging.size());
    for (const auto &row : staging) {
      auto measured = portals::detail::RowTreeAccess::measure(RowAccess::tree(scene), row->root,
                                                              {contentWidth(reserveBar), viewport.height});
      if (!measured || !active()) {
        return std::unexpected(measured ? TreeError::CLOSED : measured.error());
      }
      const auto height = std::max(1, measured->height);
      result.placements.push_back({row->root, 0, height});
    }
    if (result.placements.empty()) {
      return std::expected<NaturalGeometry, TreeError>(std::in_place, result);
    }
    result.first = std::min(leadingRows, result.placements.size() - 1);
    const auto firstHeight = result.placements[result.first].height;
    const auto requested = offsetFromEnd ? static_cast<std::int64_t>(firstHeight) + desiredCellOffset
                                         : static_cast<std::int64_t>(desiredCellOffset);
    result.offset = static_cast<std::int32_t>(std::clamp<std::int64_t>(requested, 0, firstHeight - 1));
    std::int64_t remaining = -result.offset;
    for (std::size_t index = result.first; index < result.placements.size(); ++index) {
      remaining += result.placements[index].height;
    }
    if (finalWindow && (lastPage || (allowEndBackfill && remaining < viewport.height))) {
      result.first = result.placements.size();
      remaining = 0;
      while (result.first && remaining < viewport.height) {
        remaining += result.placements[--result.first].height;
      }
      result.offset = static_cast<std::int32_t>(std::max<std::int64_t>(0, remaining - viewport.height));
    }
    if (remaining < viewport.height && staging.size() == options.maxRows &&
        (!finalWindow || beforeWindow || result.first > 0)) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    std::int64_t y = -result.offset;
    std::size_t retained{};
    for (std::size_t index = result.first; index < result.placements.size() && y < viewport.height; ++index) {
      auto &placement = result.placements[index];
      if (y > std::numeric_limits<std::int32_t>::max() - placement.height) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      placement.y = static_cast<std::int32_t>(y);
      y += placement.height;
      ++retained;
    }
    result.placements.erase(result.placements.begin(),
                            result.placements.begin() + static_cast<std::ptrdiff_t>(result.first));
    result.placements.resize(retained);
    return std::expected<NaturalGeometry, TreeError>(std::in_place, result);
  }

  std::expected<void, TreeError> commitRows(const std::shared_ptr<portals::detail::ManagedModelSourceOf<T>> &actual,
                                            const std::shared_ptr<const void> &pin,
                                            std::span<const DeliveredRow<T>> values, std::uint64_t publication,
                                            std::uint64_t target, ModelResource resource, SourceStamp incarnation,
                                            std::optional<portals::detail::ModelViewHistoryChange> saved = {},
                                            RowsCommitPublication metadata = {}, std::size_t leadingRows = 0,
                                            bool finalWindow = false, bool beforeWindow = false) {
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
        placement.push_back(
          {row->root, static_cast<std::int32_t>(staging.size() * options.rowHeight), options.rowHeight});
        staging.push_back(std::move(row));
      }
      if (desiredAutoHeight && !*desiredAutoHeight && options.autoRowHeight) {
        auto styled = RowAccess::flushStyles(scene);
        if (!styled) {
          rollback();
          return styled;
        }
        for (const auto &row : staging) {
          auto measured = portals::detail::RowTreeAccess::measure(RowAccess::tree(scene), row->root,
                                                                  {contentWidth(scrollbarVisible), viewport.height});
          if (!measured || measured->height > options.rowHeight) {
            rollback();
            return std::unexpected(measured ? TreeError::INVALID_GEOMETRY : measured.error());
          }
        }
      }
      std::int32_t nextCellOffset{};
      if (natural() && !staging.empty()) {
        auto styled = RowAccess::flushStyles(scene);
        if (!styled) {
          rollback();
          return styled;
        }
        const bool reserveBar = this->scrollbar != Node<ScrollBar>{} && this->exactCount && viewport.width > 1;
        auto measured = measureNaturalRows(staging, leadingRows, finalWindow, reserveBar, beforeWindow);
        if (!measured) {
          rollback();
          return std::unexpected(measured.error());
        }
        const auto retained = measured->placements.size();
        const auto skip = measured->first;
        for (std::size_t index = 0; index < staging.size(); ++index) {
          if ((index < skip || index - skip >= retained) && std::ranges::find(rows, staging[index]) == rows.end() &&
              staging[index] != editPin) {
            dispose(staging[index]);
          }
        }
        staging.erase(staging.begin(), staging.begin() + static_cast<std::ptrdiff_t>(skip));
        staging.resize(retained);
        placement.swap(measured->placements);
        nextCellOffset = measured->offset;
        target += skip;
        std::erase_if(refresh, [&](const auto &value) {
          return std::ranges::find(staging, value.row) == staging.end();
        });
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
      const bool retiringFocus = focusedNode != Node<Widget>{} && std::ranges::any_of(rows, [&](const auto &row) {
                                   return std::ranges::find(staging, row) == staging.end() &&
                                          RowAccess::tree(scene).contains(row->root, focusedNode).value_or(false);
                                 });
      if (!active() || intentGeneration != stagedIntent) {
        rollback();
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto admitted = admitCurrent(desiredCurrentRow, resource);
      if (!admitted) {
        rollback();
        return admitted;
      }
      if (!active() || intentGeneration != stagedIntent) {
        rollback();
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      if (retiringFocus) {
        auto collapsed = portals::detail::RowTreeAccess::focusScope(RowAccess::app(scene).router(), node);
        auto focused = collapsed ? RowAccess::app(scene).router().setFocus(node) : collapsed;
        if (!focused) {
          rollback();
          return focused;
        }
        focusedNode = node;
        this->position.editing = false;
      }
      if (!active() || intentGeneration != stagedIntent) {
        rollback();
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto nextDataRevision = dataRevision;
      if (actual) {
        auto latest = currentSource();
        auto latestFrame = actual->model ? actual->model->pin()
                                         : std::expected<ModelFrame<T>, TreeError>(std::unexpected(TreeError::CLOSED));
        if (!latest || *latest != actual || !latestFrame || latestFrame->revision() != publication) {
          rollback();
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
        nextDataRevision = latestFrame->dataRevision();
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
      visibleNodes.reserve(staging.size() + 1);
      for (const auto &row : staging) {
        visibleNodes.push_back(row->root);
      }
      const bool showBar = this->scrollbar != Node<ScrollBar>{} && this->exactCount &&
                           ((natural() && *this->exactCount > 0) || *this->exactCount > staging.size()) &&
                           viewport.width > 1 && viewport.height > 0;
      if (showBar) {
        visibleNodes.push_back(this->scrollbar);
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
      if (desiredAutoHeight) {
        options.autoRowHeight = *desiredAutoHeight;
        desiredAutoHeight.reset();
      }
      if (natural() && offsetFromEnd && !placements.empty()) {
        const auto remainder = static_cast<std::int64_t>(placements.front().height) + desiredCellOffset;
        if (remainder < 0) {
          pendingCells += remainder;
        }
      }
      this->firstCellOffset = desiredCellOffset = nextCellOffset;
      offsetFromEnd = false;
      awaitingCellMove = false;
      // Empty committed data consumes queued movement; a zero-sized hidden viewport
      // retains it for later, and loading pages never reach this publication path.
      if (rows.empty() &&
          ((actual && this->exactCount == std::optional<std::uint64_t>{0}) ||
           (!actual && finalWindow &&
            (this->sourceStatus == WindowStatus::FINAL_END || this->sourceStatus == WindowStatus::TEMPORARY_TAIL)))) {
        pendingCells = 0;
      }
      allowEndBackfill = false;
      lastPage = false;
      this->scrollbarVisible = showBar;
      editPin.swap(nextPin);
      first = desiredFirst = target;
      previousSource = actual;
      revision = publication;
      dataRevision = nextDataRevision;
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
      auto orientation = publishOrientation(resource, false);
      if (!orientation) {
        return orientation;
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
      orientation = publishOrientation(resource);
      if (!orientation || !active()) {
        return orientation;
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
      measuredGeometryRevision = portals::detail::RowTreeAccess::geometryRevision(RowAccess::tree(scene)).value_or(0);
      if (natural() && pendingCells && !rows.empty()) {
        dirty = true;
        auto invalidated = RowAccess::tree(scene).invalidate(node, Invalidation::LAYOUT);
        if (!invalidated) {
          return invalidated;
        }
      }
      if (pendingEnter) {
        const auto requested = *std::exchange(pendingEnter, {});
        auto focused = RowAccess::app(scene).router().focused();
        if (!focused) {
          return std::unexpected(focused.error());
        }
        if (requested.resource == resource && *focused == node && requested.focusGeneration &&
            requested.focusGeneration ==
              portals::detail::RowTreeAccess::focusGeneration(RowAccess::app(scene).router()) &&
            acceptedResource(requested.resource)) {
          return enterEditing();
        }
      }
      return {};
    } catch (...) {
      rollback();
      throw;
    }
  }

  void recordFailure(TreeError error) noexcept {
    failure = error;
    blockedAttempt = true;
    blockedGeometryRevision =
      lifetime.expired() ? 0 : portals::detail::RowTreeAccess::geometryRevision(RowAccess::tree(scene)).value_or(0);
    if (desiredAutoHeight) {
      desiredCellOffset = firstCellOffset;
      pendingCells = 0;
      offsetFromEnd = false;
      awaitingCellMove = false;
      desiredAutoHeight.reset();
    }
    dirty = false;
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
      auto result = applyCellIntent();
      if (result) {
        result = update();
      }
      if (!result) {
        recordFailure(result.error());
      }
    } catch (const std::bad_alloc &) {
      recordFailure(TreeError::NO_MEMORY);
    } catch (...) {
      recordFailure(TreeError::CALLBACK_FAILED);
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

  bool acceptedResource(ModelResource resource) override {
    return this->active() && declaration.resource == resource;
  }
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
  std::optional<std::int64_t> deferredCurrentDelta;

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
    this->lastPage = false;
    this->allowEndBackfill = true;
    this->desiredCellOffset = 0;
    this->offsetFromEnd = false;
    ++this->intentGeneration;
    this->dirty = true;
    this->blockedAttempt = false;
    return {};
  }

  bool rowMovePending() const noexcept override {
    return wanted != offset;
  }

  std::expected<void, TreeError> requestPage(bool backwards) override {
    if (this->natural()) {
      return this->requestCells(backwards ? -this->viewport.height : this->viewport.height);
    }
    const auto page = static_cast<std::int64_t>(std::max<std::size_t>(1, visibleRows));
    return requestMove(backwards ? -page : page);
  }

  std::expected<void, TreeError> moveCurrent(std::int64_t delta) override {
    const auto limit = this->options.maxPendingRows;
    if (deferredCurrentDelta) {
      if (delta < -limit - *deferredCurrentDelta || delta > limit - *deferredCurrentDelta) {
        return protocol(SourceError::WOULD_BLOCK);
      }
      *deferredCurrentDelta += delta;
      return {};
    }
    if (this->rows.empty()) {
      return requestMove(delta);
    }
    auto current = wanted + static_cast<std::int64_t>(this->currentSlot);
    if (!this->selecting && this->position.current) {
      std::optional<std::size_t> found;
      for (std::size_t index = 0; index < storedRows(); ++index) {
        if (rowAt(index) == this->position.current) {
          found = index;
          break;
        }
      }
      if (found) {
        current = static_cast<std::int64_t>(*found);
      } else if (window->capabilities().seekRow) {
        auto requested = requestAnchor({SourceAnchorKind::ROW, {}, *this->position.current}, SourceDirection::FORWARD);
        if (!requested) {
          return requested;
        }
        deferredCurrentDelta = std::clamp(delta, -limit, limit);
        return {};
      } else {
        return protocol(SourceError::UNSUPPORTED);
      }
    }
    const auto boundedDelta = std::clamp(delta, -limit, limit);
    auto target = current + boundedDelta;
    if (entryKnown) {
      target = std::max(std::int64_t{}, target);
    }
    const auto visible =
      static_cast<std::int64_t>(std::max<std::size_t>(1, this->natural() ? this->rows.size() : visibleRows));
    const auto nextFirst = target < wanted              ? target
                           : target - wanted >= visible ? (this->natural() ? target : target - visible + 1)
                                                        : wanted;
    const auto movement = nextFirst - wanted;
    if (movement < 0 && !window->capabilities().backward && wanted == 0) {
      return {};
    }
    auto moved =
      movement ? requestMove(movement) : RowAccess::tree(this->scene).invalidate(this->node, Invalidation::PAINT);
    if (!moved) {
      return moved;
    }
    this->desiredCellOffset = 0;
    this->pendingCells = 0;
    this->offsetFromEnd = false;
    this->currentSlot = static_cast<std::size_t>(target - nextFirst);
    this->selecting = true;
    this->dirty = true;
    this->blockedAttempt = false;
    ++this->intentGeneration;
    return {};
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
    auto invalidated = RowAccess::tree(this->scene).invalidate(this->node, Invalidation::PAINT);
    if (!invalidated) {
      return invalidated;
    }
    auto requested = startLoad(target, traversal, LoadKind::REPLACE);
    if (!requested) {
      return requested;
    }
    wanted = 0;
    this->lastPage = this->natural() && target.kind == SourceAnchorKind::TAIL;
    this->allowEndBackfill = this->lastPage;
    this->desiredCellOffset = 0;
    this->pendingCells = 0;
    deferredCurrentDelta.reset();
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
    if (this->natural()) {
      checkpoint.readingCellOffset = this->firstCellOffset;
    }
    if (this->position.current) {
      auto current = ModelBookmark::create(
        preparedResource, {PositionKind::ITEM, *this->position.current, 0, preparedRevision, PositionAffinity::FORWARD},
        {}, this->options.checkpoints.bookmarks);
      if (!current) {
        this->sourceFailure = current.error();
        return std::unexpected(rowSourceError(current.error()));
      }
      checkpoint.current = *current;
    }
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
    std::optional<RowId> restoreCurrent;
    std::int32_t restoreCellOffset{};
    if (rebind.restore == ModelRestorePolicy::REMEMBER && this->history) {
      auto checkpoint = this->history->find(target.resource, this->historyRole, false);
      if (checkpoint && checkpoint->value.reading) {
        restore = {SourceAnchorKind::ROW, {}, checkpoint->value.reading->position().row};
        restoreCellOffset = this->natural() ? checkpoint->value.readingCellOffset.value_or(0) : 0;
        if (checkpoint->value.current) {
          restoreCurrent = checkpoint->value.current->position().row;
        }
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
    this->restoredCurrent = restoreCurrent;
    this->desiredCellOffset = restoreCellOffset;
    this->pendingCells = 0;
    this->lastPage = false;
    this->allowEndBackfill = false;
    this->offsetFromEnd = false;
    deferredCurrentDelta.reset();
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
      next.temporaryAfter = anchor.kind == SourceAnchorKind::TAIL && loading == LoadKind::REPLACE;
    }
    if (loading == LoadKind::REPLACE || !segmentCount || bufferRevision != pin.dataRevision()) {
      segments[0] = std::move(next);
      segments[1] = {};
      segmentCount = 1;
      segments[0].beforeEnd = segments[0].beforeEnd || entryKnown;
      offset = replacementOffset;
      if (direction == SourceDirection::BACKWARD && anchor.kind == SourceAnchorKind::TAIL) {
        wanted =
          std::max<std::int64_t>(0, static_cast<std::int64_t>(pin.size()) - static_cast<std::int64_t>(visibleRows));
        offset = wanted;
      }
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
      auto measured = this->refreshNaturalLayout();
      if (!measured || !this->dirty) {
        return measured;
      }
    }
    if (this->blockedAttempt && failedSerial == bufferSerial && failedIntent == this->intentGeneration &&
        failedOffset == wanted &&
        (!this->natural() ||
         this->blockedGeometryRevision ==
           portals::detail::RowTreeAccess::geometryRevision(RowAccess::tree(this->scene)).value_or(0))) {
      return {};
    }
    const auto total = static_cast<std::int64_t>(storedRows());
    const bool atEnd =
      segmentCount && (segments[segmentCount - 1].afterEnd || segments[segmentCount - 1].temporaryAfter);
    const auto readFirst =
      this->natural() && this->allowEndBackfill && atEnd && total - wanted < static_cast<std::int64_t>(visibleRows)
        ? std::max<std::int64_t>(0, total - static_cast<std::int64_t>(visibleRows))
        : wanted;
    const auto leadingRows = static_cast<std::size_t>(wanted - readFirst);
    std::vector<DeliveredRow<T>> values(0);
    values.reserve(visibleRows);
    auto skip = static_cast<std::size_t>(readFirst);
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
    const bool hasBefore = readFirst > 0 || (window->capabilities().backward && segmentCount && !segments[0].beforeEnd);
    const bool hasAfter =
      readFirst + static_cast<std::int64_t>(values.size()) < static_cast<std::int64_t>(storedRows()) ||
      (segmentCount && !segments[segmentCount - 1].afterEnd && !segments[segmentCount - 1].temporaryAfter);

    struct Metadata {
      WindowRowsController &host;
      std::weak_ptr<WindowProvider<T>> provider;
      ModelResource resource;
      std::uint64_t revision;
      std::int64_t offset;
      std::uint64_t serial;
      bool before;
      bool after;
      std::size_t delivered;
    } metadata{*this,    declaration.provider, declaration.resource, bufferRevision, readFirst, bufferSerial, hasBefore,
               hasAfter, values.size()};

    auto committed = this->commitRows(
      {}, {}, values, ++publication, 0, declaration.resource, window->stamp(), {},
      {&metadata,
       [](void *context) noexcept {
         auto &next = *static_cast<Metadata *>(context);
         next.host.preparedProvider.swap(next.provider);
         next.host.preparedResource = next.resource;
         next.host.preparedRevision = next.revision;
         next.host.offset = next.offset + static_cast<std::int64_t>(next.host.first);
         next.host.wanted = next.host.offset;
         next.host.renderedSerial = next.serial;
         next.host.recordHasBefore = next.before || next.host.first > 0;
         next.host.recordHasAfter = next.after || next.host.rows.size() + next.host.first < next.delivered;
         next.host.position.hasBefore = next.host.recordHasBefore || next.host.firstCellOffset > 0;
         next.host.position.hasAfter =
           next.host.recordHasAfter ||
           (!next.host.placements.empty() &&
            static_cast<std::int64_t>(next.host.placements.back().y) + next.host.placements.back().height >
              next.host.viewport.height);
       }},
      leadingRows, !hasAfter, hasBefore);
    if (!committed || !this->active()) {
      return committed;
    }
    if (deferredCurrentDelta) {
      const auto delta = *std::exchange(deferredCurrentDelta, {});
      return moveCurrent(delta);
    }
    return {};
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
    const auto demand = this->rowDemand();
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
      auto result = this->applyCellIntent();
      if (result) {
        result = updateWindow();
      }
      if (!result) {
        this->recordFailure(result.error());
      }
    } catch (const std::bad_alloc &) {
      this->recordFailure(TreeError::NO_MEMORY);
    } catch (...) {
      this->recordFailure(TreeError::CALLBACK_FAILED);
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
 * @param options Fixed-height defaults, opt-in natural sizing and bounded mounted-row limits. @param policy Viewport
 * layout bounds.
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
 * @param options Fixed or natural sizing with bounded subtree/edit settings. @param policy Viewport bounds.
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
