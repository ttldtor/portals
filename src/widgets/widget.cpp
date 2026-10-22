// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/layout_traversal.hpp>
#include <portals/layout_snapshot.hpp>
#include <portals/widget.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>

namespace portals {
namespace {
constexpr std::size_t MAXIMUM_TREE_DEPTH = 256;

std::atomic<std::uint64_t> next_session{1};

std::uint64_t issueSession() noexcept {
  auto value = next_session.load(std::memory_order_relaxed);

  while (value != std::numeric_limits<std::uint64_t>::max()) {
    if (next_session.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
      return value;
    }
  }

  return 0;
}

constexpr std::uint64_t nextGeneration(std::uint64_t value) noexcept {
  return value == std::numeric_limits<std::uint64_t>::max() ? 0 : value + 1;
}

static_assert(nextGeneration(std::numeric_limits<std::uint64_t>::max()) == 0);
static_assert(nextGeneration(std::numeric_limits<std::uint64_t>::max() - 1) ==
              std::numeric_limits<std::uint64_t>::max());
} // namespace

struct WidgetTree::State {
  struct RegionalInputGate {
    NodeId owner;
    NodeId child;
    bool open{};
    std::uint64_t admittedGeometryEpoch{};
  };

  struct Record {
    NodeId id;

    Record *parent{};
    // The count constructor permits allocation failures from debug iterator proxies.
    std::vector<std::unique_ptr<Record>> children = std::vector<std::unique_ptr<Record>>(0);
    std::unique_ptr<Widget> widget;
    UiScope scope;
    NodeGeometry geometry;
    std::uint64_t geometryEpochCounter{};
    std::optional<std::uint64_t> currentGeometryEpoch;
    std::unique_ptr<ParentLayoutEdge> parentLayoutEdge;
    std::unique_ptr<RegionalInputGate> regionalInputGate;
    std::uint64_t visibilityRevision{1};
    std::uint64_t inputRevision{1};
    std::size_t depth{};

    bool live{true};
    bool visible{true};
    bool enabled{true};
  };

  struct Slot {
    Record *record{};

    std::uint64_t generation{1};
  };

  Scheduler &scheduler;

  WidgetTreeOptions options;
  ErrorSink sink;

  void *context;

  const std::thread::id owner{std::this_thread::get_id()};
  std::uint64_t session{};
  std::uint64_t inputGeneration{1};
  std::uint64_t geometryRevision{1};
  std::vector<Slot> slots;
  std::unique_ptr<Record> root;
  std::size_t count{};
  std::size_t borrows{};
  std::size_t frames{};
  DiagnosticContext pending_diagnostic{};
  DiagnosticContext frame_diagnostic{};
  std::size_t lifecycle{};

  bool extension{};
  bool reclaiming{};
  bool removal_pending{};
  bool closed{};
  bool reporting{};

  Invalidation dirty{Invalidation::LAYOUT};

  State(Scheduler &value, WidgetTreeOptions limits, ErrorSink error_sink, void *error_context)
      : scheduler(value), options(limits), sink(error_sink), context(error_context), slots(limits.max_nodes) {
  }

  std::expected<void, TreeError> check() const {
    if (std::this_thread::get_id() != owner) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }

    if (closed) {
      return std::unexpected(TreeError::CLOSED);
    }

    return {};
  }

  std::expected<Record *, TreeError> find(NodeId id) const {
    if (auto result = check(); !result) {
      return std::unexpected(result.error());
    }

    if (id.session == 0) {
      return std::unexpected(TreeError::STALE_NODE);
    }

    if (id.session != session) {
      return std::unexpected(TreeError::WRONG_SESSION);
    }

    if (id.slot >= slots.size()) {
      return std::unexpected(TreeError::STALE_NODE);
    }

    const auto &slot = slots[id.slot];

    if (!slot.record || !slot.record->live || slot.generation != id.generation) {
      return std::unexpected(TreeError::STALE_NODE);
    }

    return slot.record;
  }

  void changeInputEligibility(Record &record) noexcept {
    // Share the existing monotonic input clock; an ancestor stamp also revokes descendants.
    record.inputRevision = inputGeneration ? inputGeneration + 1 : 0;
  }

  static void invalidateGeometry(Record &record, const Record *visibilityRoot = nullptr, std::uint64_t revision = 0,
                                 bool inheritedVisibility = false) noexcept {
    record.geometry.valid = false;
    record.currentGeometryEpoch.reset();
    record.parentLayoutEdge.reset();
    const bool visibilityChanged = inheritedVisibility || &record == visibilityRoot;
    if (visibilityChanged) {
      record.visibilityRevision = revision;
    }

    for (auto &child : record.children) {
      if (child) {
        invalidateGeometry(*child, visibilityRoot, revision, visibilityChanged);
      }
    }
  }

  void replaceGeometry() noexcept {
    // Zero permanently revokes observation after exhaustion; retained layout remains usable.
    if (geometryRevision) {
      geometryRevision = nextGeneration(geometryRevision);
    }
  }

  void invalidate(Invalidation kind, const Record *visibilityRoot = nullptr) noexcept {
    if (auto *recorder = scheduler.diagnostics(); recorder && recorder->enabled()) {
      auto traceContext = recorder->current();

      if (kind != Invalidation::NONE && frames == 0) {
        if (!pending_diagnostic.frameId || pending_diagnostic.period != traceContext.period) {
          pending_diagnostic = traceContext;
          pending_diagnostic.frameId = recorder->nextIdentity();
        }

        traceContext.frameId = pending_diagnostic.frameId;
        recorder->record({.context = traceContext, .event = DiagnosticEvent::FRAME_REQUESTED});
      }

      recorder->record({.context = traceContext,
                        .event = DiagnosticEvent::INVALIDATION,
                        .reason = kind == Invalidation::NONE ? DiagnosticReason::FRAME_NOT_REQUESTED
                                                             : DiagnosticReason::VALUE_CHANGED});
    }

    if (kind == Invalidation::LAYOUT) {
      replaceGeometry();
      dirty = kind;

      if (root) {
        invalidateGeometry(*root, visibilityRoot, geometryRevision);
      }
    } else if (kind == Invalidation::PAINT && dirty == Invalidation::NONE) {
      dirty = kind;
    }
  }

  static void markDead(Record &record) noexcept {
    record.live = false;
    record.geometry.valid = false;
    record.currentGeometryEpoch.reset();
    record.parentLayoutEdge.reset();

    for (auto &child : record.children) {
      if (child) {
        markDead(*child);
      }
    }
  }

  static void closeScopes(Record &record) {
    for (auto &child : record.children) {
      if (child) {
        closeScopes(*child);
      }
    }

    (void)record.scope.close();
  }

  static bool shown(const Record &record, bool input) noexcept {
    for (auto current = &record; current; current = current->parent) {
      if (!current->visible ||
          (input && (!current->enabled || (current->regionalInputGate && !current->regionalInputGate->open)))) {
        return false;
      }
    }

    return true;
  }
};

void Widget::mount() {
}

void Widget::unmount() {
}

ComputedStyle Widget::styleDefaults() const {
  return {};
}

const std::optional<ComputedStyle> &Widget::computedStyle() const noexcept {
  return computedStyle_;
}

std::uint64_t Widget::styleRevision() const noexcept {
  return styleRevision_;
}

std::expected<CellSize, TreeError> Widget::measure(CellSize) {
  return CellSize{};
}

std::expected<void, TreeError> Widget::arrange(CellRect) {
  return {};
}

std::expected<void, TreeError> Widget::paint(PaintContext &) {
  return {};
}

WidgetTree::WidgetTree(std::unique_ptr<State> state) : state_(std::move(state)) {
}

WidgetTree::Borrow::Borrow(WidgetTree &value, bool is_frame) : tree(value), frame(is_frame) {
  ++tree.state_->borrows;

  if (frame) {
    ++tree.state_->frames;
  }
}

WidgetTree::Borrow::~Borrow() {
  if (frame) {
    --tree.state_->frames;
  }

  // Removal revokes identities immediately, but nested callbacks can still hold references.
  // The final borrow is the only safe point at which unmount/destruction may reclaim storage.
  if (--tree.state_->borrows == 0) {
    tree.reclaim();
  }
}

TreeError WidgetTree::fromScheduler(SchedulerError error) {
  switch (error) {
  case SchedulerError::INVALID_ARGUMENT:
    return TreeError::INVALID_ARGUMENT;
  case SchedulerError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case SchedulerError::WRONG_SCHEDULER:
    return TreeError::WRONG_SESSION;
  case SchedulerError::CLOSED:
    return TreeError::CLOSED;
  case SchedulerError::WOULD_BLOCK:
  case SchedulerError::REENTRANT_DISPATCH:
    return TreeError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case SchedulerError::IDENTITY_EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case SchedulerError::CALLBACK_FAILED:
    return TreeError::CALLBACK_FAILED;
  }

  return TreeError::INVALID_ARGUMENT;
}

std::expected<std::unique_ptr<WidgetTree>, TreeError>
WidgetTree::create(Scheduler &scheduler, WidgetTreeOptions options, ErrorSink sink, void *context) {
  auto scope = scheduler.makeScope();

  if (!scope) {
    return std::unexpected(fromScheduler(scope.error()));
  }

  if (options.max_nodes == 0 || options.max_depth == 0 || options.max_depth > MAXIMUM_TREE_DEPTH ||
      options.max_nodes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(State::Slot)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    auto state = std::make_unique<State>(scheduler, options, sink, context);

    state->session = issueSession();

    if (!state->session) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    state->root = std::make_unique<State::Record>();

    auto &root = *state->root;

    root.widget = std::make_unique<Widget>();
    root.scope = std::move(*scope);
    root.id = {state->session, 0, 1};
    root.depth = 1;

    state->slots[0].record = &root;
    state->count = 1;

    auto tree = std::unique_ptr<WidgetTree>(new WidgetTree(std::move(state)));

    root.widget->mount();

    return tree;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

WidgetTree::~WidgetTree() {
  if (std::this_thread::get_id() != state_->owner || state_->borrows || state_->reclaiming) {
    std::terminate();
  }

  (void)close();
}

void WidgetTree::report(NodeId id, TreeError error) noexcept {
  if (state_->sink && !state_->reporting) {
    state_->reporting = true;
    state_->sink(state_->context, id, error);
    state_->reporting = false;
  }
}

std::expected<Widget *, TreeError> WidgetTree::access(NodeId id, bool mutation) const {
  auto value = state_->find(id);

  if (!value) {
    return std::unexpected(value.error());
  }

  if (mutation && state_->frames) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  return (*value)->widget.get();
}

std::expected<Node<Widget>, TreeError> WidgetTree::root() const {
  if (auto result = state_->check(); !result) {
    return std::unexpected(result.error());
  }

  if (!state_->root || !state_->root->live) {
    return std::unexpected(TreeError::STALE_NODE);
  }

  return Node<Widget>(state_->root->id);
}

std::expected<Node<Widget>, TreeError> WidgetTree::inputParent(Node<Widget> node) const {
  auto value = state_->find(node.id());

  if (!value) {
    return std::unexpected(value.error());
  }

  const auto *parent = (*value)->parent;

  return parent ? Node<Widget>(parent->id) : Node<Widget>{};
}

std::expected<bool, TreeError> WidgetTree::revealVisible(Node<Widget> node) const {
  auto record = state_->find(node.id());
  if (!record) {
    return std::unexpected(record.error());
  }
  return State::shown(**record, false);
}

std::expected<std::uint64_t, TreeError> WidgetTree::geometryRevision() const {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }
  if (state_->closed) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (!state_->geometryRevision) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  return state_->geometryRevision;
}

std::expected<WidgetTree::GeometryObservation, TreeError> WidgetTree::observeGeometry(Node<Widget> node) const {
  auto value = state_->find(node.id());
  if (!value) {
    return std::unexpected(value.error());
  }
  if (state_->frames) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  return GeometryObservation{(*value)->geometry, (*value)->visibilityRevision, State::shown(**value, false)};
}

std::expected<bool, TreeError> WidgetTree::contains(Node<Widget> ancestor, Node<Widget> node) const {
  const auto parent = state_->find(ancestor.id());

  if (!parent) {
    return std::unexpected(parent.error());
  }

  const auto child = state_->find(node.id());

  if (!child) {
    return std::unexpected(child.error());
  }

  for (auto *record = *child; record; record = record->parent) {
    if (record == *parent) {
      return true;
    }
  }

  return false;
}

std::expected<NodeSnapshot, TreeError> WidgetTree::inspect(Node<Widget> node) const {
  auto value = state_->find(node.id());

  if (!value) {
    return std::unexpected(value.error());
  }

  const auto &record = **value;

  try {
    NodeSnapshot result;

    if (record.parent) {
      result.parent = Node<Widget>(record.parent->id);
    }

    result.geometry = record.geometry;
    result.geometryEpoch = record.currentGeometryEpoch;
    result.regionalInputOpen = !record.regionalInputGate || record.regionalInputGate->open;
    if (record.parentLayoutEdge) {
      result.parentLayoutEdge = *record.parentLayoutEdge;
    }
    result.visible = record.visible;
    result.enabled = record.enabled;
    result.children.reserve(record.children.size());

    for (const auto &child : record.children) {
      if (child && child->live) {
        result.children.push_back(Node<Widget>(child->id));
      }
    }

    // Avoid a noexcept vector move allocating a debug iterator proxy on MSVC.
    // The copy may throw inside this boundary; the expected prvalue is elided.
    return std::expected<NodeSnapshot, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> WidgetTree::canInsert(NodeId parent) const {
  auto record = state_->find(parent);

  if (!record) {
    return std::unexpected(record.error());
  }

  if (state_->frames || state_->lifecycle || state_->reclaiming) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (state_->count >= state_->options.max_nodes) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  if ((*record)->depth >= state_->options.max_depth) {
    return std::unexpected(TreeError::DEPTH_LIMIT);
  }

  return {};
}

std::expected<NodeId, TreeError> WidgetTree::attach(NodeId parent, std::unique_ptr<Widget> widget, bool checked) {
  if (auto allowed = canInsert(parent); !allowed) {
    return std::unexpected(allowed.error());
  }

  auto &record = **state_->find(parent);
  auto slot = std::ranges::find_if(state_->slots, [](const State::Slot &entry) {
    return !entry.record && entry.generation != 0;
  });

  if (slot == state_->slots.end()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  auto node_scope = scheduler().makeScope();

  if (!node_scope) {
    return std::unexpected(fromScheduler(node_scope.error()));
  }

  auto child = std::make_unique<State::Record>();

  child->id = {state_->session, static_cast<std::size_t>(slot - state_->slots.begin()), slot->generation};
  child->parent = &record;
  child->depth = record.depth + 1;
  child->widget = std::move(widget);
  child->scope = std::move(*node_scope);

  record.children.reserve(record.children.size() + 1);

  auto pointer = child.get();
  const auto id = pointer->id;

  record.children.push_back(std::move(child));
  slot->record = pointer;
  ++state_->count;
  state_->invalidate(Invalidation::LAYOUT);
  ++state_->lifecycle;

  bool mountFailed{};

  try {
    pointer->widget->mount();
  } catch (...) {
    mountFailed = true;
    report(id, TreeError::CALLBACK_FAILED);
  }

  --state_->lifecycle;

  if (checked && mountFailed) {
    // The enclosing borrow retains storage even when mount or the sink already removed this node.
    if (state_->find(id)) {
      (void)remove(Node<Widget>(id));
    }

    return std::unexpected(TreeError::CALLBACK_FAILED);
  }

  if (checked) {
    if (auto live = state_->find(id); !live) {
      return std::unexpected(live.error());
    }
  }

  return id;
}

std::expected<void, TreeError> WidgetTree::remove(Node<Widget> node) {
  auto record = state_->find(node.id());

  if (!record) {
    return std::unexpected(record.error());
  }

  if (state_->frames) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  Borrow guard(*this);

  // Revoke every descendant identity before closing scopes: releasing callback captures can
  // reenter the tree, but must never obtain a fresh borrow into the logically removed subtree.
  if (state_->inputGeneration) {
    ++state_->inputGeneration;
  }

  State::markDead(**record);
  state_->removal_pending = true;
  state_->invalidate(Invalidation::LAYOUT);
  State::closeScopes(**record);

  return {};
}

std::expected<void, TreeError> WidgetTree::commitOverlay(Node<Widget> host, Node<Widget> body, Node<Widget> layer,
                                                         std::span<const Node<Widget>> order, bool visible) {
  auto parent = state_->find(host.id());
  auto target = state_->find(layer.id());

  if (!parent || !target) {
    return std::unexpected(parent ? target.error() : parent.error());
  }
  auto &children = (*parent)->children;
  if (static_cast<std::size_t>(std::ranges::count_if(children,
                                                     [](const auto &child) {
                                                       return child->live;
                                                     })) != order.size() + 1 ||
      children.empty() || children.front()->id != body.id() || (*target)->parent != *parent) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  for (std::size_t i = 0; i != order.size(); ++i) {
    auto child = state_->find(order[i].id());
    if (!child) {
      return std::unexpected(child.error());
    }
    if ((*child)->parent != *parent || order[i] == body ||
        std::ranges::find(order.first(i), order[i]) != order.first(i).end()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
  }

  bool changed = (*target)->visible != visible;
  // All fallible validation precedes pointer rotation. Record addresses and parents stay stable.
  for (std::size_t i = 0; i != order.size(); ++i) {
    auto first = children.begin() + static_cast<std::ptrdiff_t>(i + 1);
    auto found = std::find_if(first, children.end(), [&](const auto &child) {
      return child->id == order[i].id();
    });
    changed |= first != found;
    std::rotate(first, found, std::next(found));
  }
  if ((*target)->visible != visible) {
    state_->changeInputEligibility(**target);
  }
  (*target)->visible = visible;
  if (changed) {
    if (state_->inputGeneration) {
      ++state_->inputGeneration;
    }
    state_->invalidate(Invalidation::LAYOUT, *target);
  }
  return {};
}

std::expected<void, TreeError> WidgetTree::close() {
  if (std::this_thread::get_id() != state_->owner) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (state_->closed) {
    return {};
  }

  if (state_->frames) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  Borrow guard(*this);

  state_->closed = true;

  if (state_->inputGeneration) {
    ++state_->inputGeneration;
  }
  state_->removal_pending = true;

  if (state_->root) {
    State::markDead(*state_->root);
    State::closeScopes(*state_->root);
  }

  return {};
}

void WidgetTree::reclaim() noexcept {
  if (state_->reclaiming || !state_->removal_pending) {
    return;
  }

  state_->reclaiming = true;
  // Hooks can mark an already visited sibling dead. A repeated pass is bounded by
  // the initial node count: insertion is rejected throughout reclamation.
  auto sweep = [&](auto &&self, std::unique_ptr<State::Record> &node) -> void {
    if (!node) {
      return;
    }

    for (auto &child : node->children) {
      self(self, child);
    }

    std::erase_if(node->children, [](const auto &child) {
      return !child;
    });

    if (node->live) {
      return;
    }

    // A child unmount may remove this ancestor after its other children were visited.
    for (auto &child : node->children) {
      self(self, child);
    }

    node->children.clear();

    const auto id = node->id;

    {
      Borrow guard(*this);

      ++state_->lifecycle;

      try {
        node->widget->unmount();
      } catch (...) {
        report(id, TreeError::CALLBACK_FAILED);
      }

      --state_->lifecycle;
    }

    // Unpublish the address before running the widget destructor. Advance the generation only
    // after destruction; zero permanently retires an exhausted slot rather than reviving old IDs.
    auto &slot = state_->slots[id.slot];

    slot.record = nullptr;
    node.reset();
    slot.generation = nextGeneration(slot.generation);
    --state_->count;
  };
  bool pending;

  do {
    sweep(sweep, state_->root);
    pending = std::ranges::any_of(state_->slots, [](const State::Slot &slot) {
      return slot.record && !slot.record->live;
    });
  } while (pending);

  state_->removal_pending = false;
  state_->reclaiming = false;
}

std::uint64_t WidgetTree::inputGeneration() const noexcept {
  return state_->inputGeneration;
}

const UiScope &WidgetTree::scope(NodeId id) const {
  return state_->slots[id.slot].record->scope;
}

Scheduler &WidgetTree::scheduler() const {
  return state_->scheduler;
}

std::expected<void, TreeError> WidgetTree::setVisible(Node<Widget> node, bool visible) {
  auto checked = access(node.id(), true);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  auto &record = *state_->slots[node.id().slot].record;

  if (record.visible != visible) {
    state_->changeInputEligibility(record);
    record.visible = visible;

    if (state_->inputGeneration) {
      ++state_->inputGeneration;
    }

    state_->invalidate(Invalidation::LAYOUT, &record);
  }

  return {};
}

std::expected<void, TreeError> WidgetTree::setEnabled(Node<Widget> node, bool enabled) {
  auto checked = access(node.id(), true);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  auto &record = *state_->slots[node.id().slot].record;

  if (record.enabled != enabled) {
    state_->changeInputEligibility(record);
    record.enabled = enabled;

    if (state_->inputGeneration) {
      ++state_->inputGeneration;
    }

    state_->invalidate(Invalidation::PAINT);
  }

  return {};
}

std::expected<bool, TreeError> WidgetTree::inputEligible(Node<Widget> node) const {
  auto record = state_->find(node.id());

  if (!record) {
    return std::unexpected(record.error());
  }

  return State::shown(**record, true);
}

std::expected<std::uint64_t, TreeError> WidgetTree::inputRevision(Node<Widget> node) const {
  auto record = state_->find(node.id());
  if (!record) {
    return std::unexpected(record.error());
  }
  std::uint64_t revision{};
  for (auto *ancestor = *record; ancestor; ancestor = ancestor->parent) {
    if (!ancestor->inputRevision) {
      return 0;
    }
    revision = std::max(revision, ancestor->inputRevision);
  }
  return revision;
}

std::expected<bool, TreeError> WidgetTree::inputEligible(Node<Widget> node, Node<Widget> reveal) const {
  auto record = state_->find(node.id());
  if (!record) {
    return std::unexpected(record.error());
  }
  for (auto *ancestor = *record; ancestor; ancestor = ancestor->parent) {
    if (!ancestor->enabled || (!ancestor->visible && ancestor->id != reveal.id()) ||
        (ancestor->regionalInputGate && !ancestor->regionalInputGate->open)) {
      return false;
    }
  }
  return true;
}

std::expected<bool, TreeError>
detail::LayoutTraversalAccess::adoptRegionalInput(WidgetTree &tree, const LayoutSnapshot &snapshot,
                                                  std::span<const RegionalInputOwnerProposal> owners) {
  if (auto checked = tree.state_->check(); !checked) {
    return std::unexpected(checked.error());
  }

  if (tree.state_->frames || tree.state_->extension) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (!snapshot.geometry_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (snapshot.session_ != tree.state_->session) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }

  if (!snapshot.geometryRevision_ || !tree.state_->geometryRevision || !snapshot.inputGeneration_ ||
      !tree.state_->inputGeneration) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  if (snapshot.geometryRevision_ != tree.state_->geometryRevision ||
      snapshot.inputGeneration_ != tree.state_->inputGeneration) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  // The existing frame/reentry guards protect staging from user allocation
  // hooks. No layout traversal, user callback or scheduler dispatch is added.
  WidgetTree::Borrow guard(tree, true);
  tree.state_->extension = true;

  struct RestoreExtension {
    bool &extension;

    ~RestoreExtension() {
      extension = false;
    }
  } restore{tree.state_->extension};

  using Record = WidgetTree::State::Record;
  using Gate = WidgetTree::State::RegionalInputGate;

  struct Change {
    Record *record;
    std::unique_ptr<Gate> next;
  };

  try {
    std::vector<Change> changes(0);
    std::vector<const RegionalInputOwnerProposal *> orderedOwners(0);

    if (owners.size() > tree.state_->options.max_nodes) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    std::size_t proposalCount{};
    orderedOwners.reserve(owners.size());

    for (const auto &owner : owners) {
      if (owner.children.size() > tree.state_->options.max_nodes - proposalCount) {
        return std::unexpected(TreeError::NODE_LIMIT);
      }

      proposalCount += owner.children.size();
      orderedOwners.push_back(&owner);
    }

    std::ranges::sort(orderedOwners, {}, [](const RegionalInputOwnerProposal *value) {
      return value->owner.id().slot;
    });

    for (std::size_t i = 1; i < orderedOwners.size(); ++i) {
      if (orderedOwners[i - 1]->owner == orderedOwners[i]->owner) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }

    for (const auto *batchPointer : orderedOwners) {
      const auto &batch = *batchPointer;
      auto owner = tree.state_->find(batch.owner.id());

      if (!owner) {
        return std::unexpected(owner.error());
      }

      const auto capturedOwner = snapshot.geometry(batch.owner);

      if (!capturedOwner) {
        return std::unexpected(capturedOwner.error());
      }

      if (!capturedOwner->visible || !(*owner)->geometry.valid || !capturedOwner->geometryEpoch ||
          capturedOwner->geometryEpoch != (*owner)->currentGeometryEpoch) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }

      std::vector<const RegionalInputProposal *> index(0);
      index.reserve(batch.children.size());

      for (const auto &proposal : batch.children) {
        auto child = tree.state_->find(proposal.child);

        if (!child) {
          return std::unexpected(child.error());
        }

        if ((*child)->parent != *owner || !(*child)->geometry.valid ||
            (*child)->currentGeometryEpoch != proposal.geometryEpoch || !(*child)->parentLayoutEdge) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        const auto &edge = *(*child)->parentLayoutEdge;
        const auto expectedInput = proposal.open ? RegionalInput::OPEN : RegionalInput::CLOSED;

        if (edge.parent != batch.owner.id() || edge.child != proposal.child ||
            edge.geometryEpoch != proposal.geometryEpoch || edge.proposedInput != expectedInput) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        index.push_back(&proposal);
      }

      const auto slot = [](const RegionalInputProposal *value) {
        return value->child.slot;
      };

      std::ranges::sort(index, {}, slot);

      for (std::size_t i = 1; i < index.size(); ++i) {
        if (index[i - 1]->child == index[i]->child) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
      }

      for (auto &child : (*owner)->children) {
        if (!child || !child->live) {
          continue;
        }

        const auto found = std::ranges::lower_bound(index, child->id.slot, {}, slot);
        const auto *desired = found != index.end() && (*found)->child == child->id ? *found : nullptr;
        const auto *current = child->regionalInputGate.get();

        if (child->visible && !child->geometry.valid) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        if (!desired && child->geometry.valid && child->parentLayoutEdge &&
            child->parentLayoutEdge->proposedInput != RegionalInput::UNCHANGED) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        if (current && current->owner != batch.owner.id()) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }

        // Epoch is an audit stamp, never semantic gate state. A fresh layout
        // with the same admission must not request another preparation pass.
        if ((!current && !desired) ||
            (current && desired && current->child == desired->child && current->open == desired->open)) {
          continue;
        }

        std::unique_ptr<Gate> next;

        if (desired) {
          next = std::make_unique<Gate>(batch.owner.id(), desired->child, desired->open, desired->geometryEpoch);
        }

        changes.push_back({child.get(), std::move(next)});
      }
    }

    if (changes.empty()) {
      return false;
    }

    if (!tree.state_->inputGeneration || tree.state_->inputGeneration == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    for (auto &change : changes) {
      change.record->regionalInputGate = std::move(change.next);
      tree.state_->changeInputEligibility(*change.record);
    }

    ++tree.state_->inputGeneration;
    tree.state_->invalidate(Invalidation::LAYOUT);

    return true;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> WidgetTree::invalidate(Node<Widget> node, Invalidation kind) {
  auto checked = access(node.id(), true);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  if (kind != Invalidation::NONE && kind != Invalidation::PAINT && kind != Invalidation::LAYOUT) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  state_->invalidate(kind);

  return {};
}

std::expected<Invalidation, TreeError> WidgetTree::consumeFrameRequest() {
  if (auto checked = state_->check(); !checked) {
    return std::unexpected(checked.error());
  }

  if (state_->frames) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (auto *recorder = diagnostics(); recorder && recorder->enabled()) {
    if (state_->dirty == Invalidation::NONE && recorder->current().correlationId) {
      recorder->note(DiagnosticEvent::FRAME_NOT_REQUESTED, DiagnosticReason::FRAME_NOT_REQUESTED);
    } else if (state_->dirty != Invalidation::NONE) {
      state_->frame_diagnostic = state_->pending_diagnostic;
      state_->pending_diagnostic = {};
    }
  }

  return std::exchange(state_->dirty, Invalidation::NONE);
}

Diagnostics *WidgetTree::diagnostics() const noexcept {
  return state_->scheduler.diagnostics();
}

DiagnosticContext WidgetTree::diagnosticFrame() const noexcept {
  auto *recorder = diagnostics();

  return recorder && recorder->enabled() ? state_->frame_diagnostic : DiagnosticContext{};
}

std::expected<void, TreeError> WidgetTree::beginFrame() const {
  if (auto checked = state_->check(); !checked) {
    return checked;
  }

  if (state_->borrows || state_->reclaiming) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  return {};
}

std::expected<CellSize, TreeError> WidgetTree::measure(Node<Widget> node, CellSize available) {
  auto record = state_->find(node.id());

  if (!record) {
    return std::unexpected(record.error());
  }

  if (state_->extension) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (!State::shown(**record, false)) {
    return CellSize{};
  }

  Borrow guard(*this, true);

  state_->extension = true;

  std::expected<CellSize, TreeError> result;

  try {
    result = (*record)->widget->measure(available.normalized());
  } catch (...) {
    result = std::unexpected(TreeError::CALLBACK_FAILED);
  }

  state_->extension = false;

  if (!result) {
    report(node.id(), result.error());
  } else {
    *result = result->normalized();

    if (const auto &style = (*record)->widget->computedStyle()) {
      result->width = std::max(result->width, style->minWidth);
    }
  }

  return result;
}

std::expected<void, TreeError> WidgetTree::arrange(Node<Widget> node, CellRect local, CellRect viewport) {
  return arrangeImpl(node, local, viewport, std::nullopt);
}

std::expected<void, TreeError> WidgetTree::arrange(Node<Widget> node, CellRect local, CellRect viewport,
                                                   CellRect contentClip) {
  return arrangeImpl(node, local, viewport, contentClip);
}

std::expected<void, TreeError> WidgetTree::arrangeImpl(Node<Widget> node, CellRect local, CellRect viewport,
                                                       std::optional<CellRect> contentClip,
                                                       const ParentLayoutEdge *parentEdge) {
  auto record = state_->find(node.id());

  if (!record) {
    return std::unexpected(record.error());
  }

  if (state_->extension) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto &value = **record;

  // A parent rearrangement invalidates descendant surface coordinates too. Publish fresh
  // geometry only after the hook succeeds; failures leave hit-testing unable to use stale cells.
  state_->replaceGeometry();
  State::invalidateGeometry(value);

  if (!State::shown(value, false)) {
    return {};
  }

  if (value.geometryEpochCounter == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  std::unique_ptr<ParentLayoutEdge> ownedEdge;
  if (parentEdge) {
    if (!value.parent || parentEdge->parent != value.parent->id || parentEdge->child != value.id ||
        (parentEdge->revealAxes != RevealAxes::BOTH && parentEdge->revealAxes != RevealAxes::HORIZONTAL_ONLY) ||
        (parentEdge->proposedInput != RegionalInput::UNCHANGED && parentEdge->proposedInput != RegionalInput::OPEN &&
         parentEdge->proposedInput != RegionalInput::CLOSED)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    try {
      ownedEdge = std::make_unique<ParentLayoutEdge>(*parentEdge);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    }

    ownedEdge->geometryEpoch = value.geometryEpochCounter + 1;
  }

  CellRect surface = local;
  CellRect clip = viewport;

  if (value.parent) {
    if (!value.parent->geometry.valid) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    auto translated = local.translated(value.parent->geometry.surface.origin());

    if (!translated) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    surface = *translated;
    clip = value.parent->geometry.clip;
  }

  if (contentClip) {
    clip = clip.intersection(*contentClip);
  }

  Borrow guard(*this, true);

  state_->extension = true;

  std::expected<void, TreeError> result;

  try {
    result = value.widget->arrange(local);
  } catch (...) {
    result = std::unexpected(TreeError::CALLBACK_FAILED);
  }

  state_->extension = false;

  if (!result) {
    report(node.id(), result.error());

    return result;
  }

  value.geometry = {local, surface, surface.intersection(clip), true};
  value.currentGeometryEpoch = ++value.geometryEpochCounter;
  value.parentLayoutEdge = std::move(ownedEdge);
  state_->invalidate(Invalidation::PAINT);

  return {};
}

std::expected<void, TreeError> WidgetTree::paint(Node<Widget> node, PaintContext &context) {
  auto record = state_->find(node.id());

  if (!record) {
    return std::unexpected(record.error());
  }

  if (!state_->frames || state_->extension) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (!State::shown(**record, false)) {
    return {};
  }

  if (!(*record)->geometry.valid) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  if ((*record)->geometry.clip.empty()) {
    return {};
  }

  Borrow guard(*this, true);

  state_->extension = true;

  std::expected<void, TreeError> result;

  try {
    result = (*record)->widget->paint(context);
  } catch (...) {
    result = std::unexpected(TreeError::CALLBACK_FAILED);
  }

  state_->extension = false;

  if (!result) {
    report(node.id(), result.error());
  }

  return result;
}
} // namespace portals
