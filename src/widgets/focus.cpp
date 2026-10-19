// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"

#include <algorithm>
#include <tuple>

namespace portals {
EventRouter::EventRouter(std::unique_ptr<State> state) : state_(std::move(state)) {
}

std::expected<std::unique_ptr<EventRouter>, TreeError> EventRouter::create(WidgetTree &tree,
                                                                           EventRouterOptions options) {
  auto root = tree.root();

  if (!root) {
    return std::unexpected(root.error());
  }

  if (options.max_registrations == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    auto state = std::make_unique<State>(tree, options);

    return std::unique_ptr<EventRouter>(new EventRouter(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

EventRouter::~EventRouter() {
  if (state_->owner != std::this_thread::get_id() || state_->dispatching || state_->deliveringFocus ||
      state_->collecting || state_->closing) {
    std::terminate();
  }

  state_->closing = true;

  if (auto observation = state_->focusObservation.lock()) {
    observation->retired = true;
    observation->focused = {};
    observation->generation = 0;
  }
  state_->focusObservation.reset();

  // Capture tokens never invoke cancellation callbacks or retain action captures.
  if (state_->pointerCapture) {
    state_->pointerCapture->cancellation->connected.store(false);
    state_->pointerCapture.reset();
  }

  state_->registrations.clear();
  state_->scopes.clear();
}

std::uint64_t EventRouter::focusGeneration() const noexcept {
  return state_->focusGeneration;
}

WidgetTree &EventRouter::tree() {
  return state_->tree;
}

std::expected<void, TreeError> EventRouter::check() const {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (state_->closing) {
    return std::unexpected(TreeError::CLOSED);
  }

  if (state_->collecting) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (auto host = state_->dialogs.lock(); host && host->staging) {
    // Dialog staging borrows focus ancestry across allocations; public reentry cannot mutate it.
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto root = state_->tree.root();

  if (!root) {
    return std::unexpected(root.error());
  }

  return state_->tree.edit(*root, [](Widget &) {
  });
}

std::shared_ptr<detail::ActionRegistration> EventRouter::registration(Node<Widget> node) const {
  for (const auto &entry : state_->registrations) {
    if (entry->node == node && entry->cancellation->connected.load()) {
      return entry;
    }
  }

  return {};
}

std::expected<void, TreeError> EventRouter::collect() {
  auto allowed = check();

  if (!allowed) {
    if (allowed.error() == TreeError::CLOSED) {
      static_cast<void>(retirePointerCapture(false));
    }

    return allowed;
  }

  (void)modalRoot();

  for (auto &entry : state_->registrations) {
    auto live = tree().inputEligible(entry->node);

    if (!live && live.error() != TreeError::STALE_NODE) {
      return std::unexpected(live.error());
    }

    if (!live) {
      entry->cancellation->connected.store(false);
    }
  }

  for (auto &entry : state_->scopes) {
    auto live = tree().inputEligible(entry.root);

    if (!live && live.error() != TreeError::STALE_NODE) {
      return std::unexpected(live.error());
    }

    if (!live) {
      entry.cancellation->connected.store(false);
    }
  }

  auto capture = checkedPointerCapture();

  if (!capture) {
    return std::unexpected(capture.error());
  }

  // Destructors of captures cannot reenter while registry storage is being changed.
  struct CollectionGuard {
    bool &active;

    explicit CollectionGuard(bool &value) : active(value) {
      active = true;
    }

    ~CollectionGuard() {
      active = false;
    }
  } guard(state_->collecting);

  // Active delivery keeps its own shared owner until the callback returns.
  std::erase_if(state_->registrations, [](const auto &entry) {
    return !entry->cancellation->connected.load();
  });
  std::erase_if(state_->scopes, [](const auto &entry) {
    return !entry.cancellation->connected.load();
  });
  std::erase_if(state_->history, [&](auto node) {
    return !registration(node);
  });

  return {};
}

detail::FocusScopeRegistration *EventRouter::scope(Node<Widget> root) const {
  for (auto &entry : state_->scopes) {
    if (entry.root == root && entry.cancellation->connected.load()) {
      return &entry;
    }
  }

  return nullptr;
}

bool EventRouter::within(Node<Widget> root, Node<Widget> node) const {
  while (node.id().session != 0) {
    if (node == root) {
      return true;
    }

    auto parent = state_->tree.inputParent(node);

    if (!parent) {
      return false;
    }

    node = *parent;
  }

  return false;
}

Node<Widget> EventRouter::scopeEntry(Node<Widget> root, const std::vector<Node<Widget>> &order, bool reverse) const {
  auto *entry = scope(root);

  if (!entry) {
    return {};
  }

  for (auto candidate : {entry->last, entry->defaultTarget}) {
    if (std::ranges::contains(order, candidate) && within(root, candidate)) {
      return candidate;
    }
  }

  Node<Widget> candidate;

  for (auto node : order) {
    if (within(root, node)) {
      candidate = node;

      if (!reverse) {
        break;
      }
    }
  }

  // A fallback enters nested scopes, while a remembered/default identity is exact.
  return entering(candidate, root, order, reverse);
}

Node<Widget> EventRouter::entering(Node<Widget> node, Node<Widget> previous, const std::vector<Node<Widget>> &order,
                                   bool reverse) const {
  Node<Widget> outermost;

  for (auto ancestor = node; ancestor.id().session != 0;) {
    if (scope(ancestor) && !within(ancestor, previous)) {
      outermost = ancestor;
    }

    auto parent = state_->tree.inputParent(ancestor);

    if (!parent) {
      break;
    }

    ancestor = *parent;
  }

  return outermost.id().session != 0 ? scopeEntry(outermost, order, reverse) : node;
}

std::expected<ActionBinding, TreeError> EventRouter::focusScope(Node<Widget> root, Node<Widget> defaultTarget) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state_->dispatching || state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto live = tree().inputEligible(root);

  if (!live) {
    return std::unexpected(live.error());
  }

  if (defaultTarget.id().session != 0) {
    auto valid = tree().inputEligible(defaultTarget);

    if (!valid) {
      return std::unexpected(valid.error());
    }

    if (!within(root, defaultTarget)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
  }

  if (scope(root)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (state_->registrations.size() + state_->scopes.size() + !state_->dialogs.expired() +
        !state_->notifications.expired() >=
      state_->options.max_registrations) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  try {
    auto cancellation = std::make_shared<detail::ActionCancellation>();
    std::vector<Node<Widget>> ancestry(0);

    for (auto ancestor = state_->focus; ancestor.id().session != 0;) {
      if (ancestor == root || scope(ancestor)) {
        ancestry.push_back(ancestor);
      }

      auto parent = tree().inputParent(ancestor);

      if (!parent) {
        // A retired focus is recovered lazily; keep its existing scope ancestry.
        ancestry = state_->focusScopes;

        break;
      }

      ancestor = *parent;
    }

    const auto remembered = within(root, state_->focus) ? state_->focus : Node<Widget>{};

    state_->scopes.push_back({root, defaultTarget, remembered, cancellation});
    state_->focusScopes.swap(ancestry);

    return ActionBinding(cancellation);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> EventRouter::focusDefault(Node<Widget> node) {
  auto allowed = collect();

  if (!allowed) {
    return allowed;
  }

  if (state_->dispatching || state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  for (auto ancestor = node; ancestor.id().session != 0;) {
    auto parent = tree().inputParent(ancestor);

    if (!parent) {
      return std::unexpected(parent.error());
    }

    if (auto *entry = scope(ancestor)) {
      entry->defaultTarget = node;

      return {};
    }

    ancestor = *parent;
  }

  return std::unexpected(TreeError::INVALID_ARGUMENT);
}

std::expected<Node<Widget>, TreeError> EventRouter::enterScope(Node<Widget> root) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto live = tree().inputEligible(root);

  if (!live) {
    return std::unexpected(live.error());
  }

  if (!scope(root)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  auto modal = modalRoot();
  if (modal != Node<Widget>{} && !within(modal, root)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (auto layer = dialogLayer(root); layer && !layer->open) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    auto order = focusOrder();

    if (!order) {
      return std::unexpected(order.error());
    }

    auto changed = changeFocus(scopeEntry(root, *order, false));

    if (!changed) {
      return std::unexpected(changed.error());
    }

    return checkedFocus();
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::enterFocusScope(Node<Widget> root) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto eligible = tree().inputEligible(root);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }
  auto modal = modalRoot();
  if (modal != Node<Widget>{} && !within(modal, root)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto *entry = scope(root);

  if (!*eligible || !entry || std::ranges::any_of(state_->focusReturns, [&](const auto &saved) {
        return saved->root == root;
      })) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto previous = state_->focus;
  const auto depth = state_->focusReturns.size();

  if (depth >= state_->options.max_registrations) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  try {
    auto order = focusOrder();

    if (!order) {
      return std::unexpected(order.error());
    }

    auto saved =
      std::make_unique<detail::FocusReturnRegistration>(root, previous, state_->focusScopes, entry->cancellation);
    state_->focusReturns.reserve(depth + 1);
    if (state_->requiredFocusReceipt == root &&
        state_->focusReturnSerial == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    saved->publish(state_->focusReturnSerial);
    state_->focusReturns.push_back(std::move(saved));

    auto changed = changeFocus(scopeEntry(root, *order, false));

    if (!changed) {
      if (state_->focus == previous) {
        state_->focusReturns.resize(depth);
      }

      return std::unexpected(changed.error());
    }

    return checkedFocus();
  } catch (const std::bad_alloc &) {
    // Allocation in changeFocus precedes its publication. Callback exceptions are translated there.
    state_->focusReturns.resize(depth);

    return std::unexpected(TreeError::NO_MEMORY);
  }
}

bool EventRouter::endedFocusScope(std::size_t index) const {
  const auto &saved = *state_->focusReturns[index];
  auto eligible = state_->tree.inputEligible(saved.root);

  const auto layer = saved.dialog ? dialogLayer(saved.root) : nullptr;
  return !saved.cancellation->connected.load() || !eligible || !*eligible || (saved.dialog && (!layer || !layer->open));
}

Node<Widget> EventRouter::fallbackFocus(const std::vector<Node<Widget>> &scopes,
                                        const std::vector<Node<Widget>> &order) const {
  for (auto root : scopes) {
    if (scope(root)) {
      auto next = scopeEntry(root, order, false);

      if (next.id().session != 0) {
        return next;
      }
    }
  }

  for (auto it = state_->history.rbegin(); it != state_->history.rend(); ++it) {
    if (std::ranges::contains(order, *it)) {
      return *it;
    }
  }

  return order.empty() ? Node<Widget>{} : order.front();
}

std::expected<Node<Widget>, TreeError> EventRouter::returnFocusScopes(std::size_t first) {
  while (first != 0 && endedFocusScope(first - 1)) {
    --first;
  }

  try {
    auto order = focusOrder();

    if (!order) {
      return std::unexpected(order.error());
    }

    const auto &saved = *state_->focusReturns[first];
    const auto next =
      std::ranges::contains(*order, saved.previous) ? saved.previous : fallbackFocus(saved.previousScopes, *order);
    const auto previous = state_->focus;
    auto changed = changeFocus(next);

    if (changed || state_->focus != previous) {
      state_->focusReturns.resize(first);
    }

    return changed ? checkedFocus() : std::unexpected(changed.error());
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::leaveFocusScope(Node<Widget> root) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (state_->focusReturns.empty() || state_->focusReturns.back()->root != root) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return returnFocusScopes(state_->focusReturns.size() - 1);
}

std::expected<bool, TreeError> EventRouter::recoverFocusScopes() {
  if (state_->focusReturns.empty() || !endedFocusScope(state_->focusReturns.size() - 1)) {
    return false;
  }

  if (state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto returned = returnFocusScopes(state_->focusReturns.size() - 1);

  return returned ? std::expected<bool, TreeError>(true) : std::unexpected(returned.error());
}

std::expected<std::vector<Node<Widget>>, TreeError> EventRouter::focusOrder(Node<Widget> reveal, Node<Widget> exclude) {
  std::vector<Node<Widget>> result(0);
  std::vector<Node<Widget>> pending(0);
  auto root = tree().root();

  if (!root) {
    return std::unexpected(root.error());
  }

  // Inspection does not call user code or mutate registrations on this UI thread.
  // Borrow entries only for this walk; keep cancellation checks live for worker tokens.
  std::vector<const detail::ActionRegistration *> index(0);
  const auto identity = [](const detail::ActionRegistration *entry) {
    const auto id = entry->node.id();

    return std::tuple{id.session, id.slot, id.generation};
  };

  index.reserve(state_->registrations.size());

  for (const auto &entry : state_->registrations) {
    if (entry->focusable) {
      index.push_back(entry.get());
    }
  }

  std::ranges::sort(index, {}, identity);
  pending.push_back(*root);

  while (!pending.empty()) {
    const auto node = pending.back();
    auto snapshot = tree().inspect(node);

    pending.pop_back();

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    if ((!snapshot->visible && node != reveal) || !snapshot->enabled || !dialogEligible(node, reveal, exclude)) {
      continue;
    }

    const auto id = node.id();
    const auto found = std::ranges::lower_bound(index, std::tuple{id.session, id.slot, id.generation}, {}, identity);

    auto modal = modalRoot(exclude);
    if (auto opening = dialogLayer(reveal); opening && opening->options.mode == DialogMode::MODAL) {
      modal = reveal;
    }
    if (found != index.end() && (*found)->node == node && (*found)->cancellation->connected.load() &&
        (modal == Node<Widget>{} || within(modal, node))) {
      result.push_back(node);
    }

    // Reverse-push onto the LIFO stack visits siblings in insertion order. Skipping an
    // ineligible ancestor prunes the whole subtree without consulting arranged geometry.
    pending.insert(pending.end(), snapshot->children.rbegin(), snapshot->children.rend());
  }

  // Copy inside the allocation boundary; MSVC debug vector moves may allocate in noexcept.
  return std::expected<std::vector<Node<Widget>>, TreeError>(std::in_place, result);
}

std::expected<void, TreeError> EventRouter::deliverFocus(Node<Widget> node, bool focused) {
  auto entry = registration(node);

  if (!entry || !entry->focusCallback) {
    return {};
  }

  auto live = tree().inspect(node);

  if (!live) {
    return live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED ? std::expected<void, TreeError>{}
                                                                                      : std::unexpected(live.error());
  }

  if (focused) {
    auto eligible = tree().inputEligible(node);

    if (!eligible) {
      return std::unexpected(eligible.error());
    }

    if (!*eligible || state_->focus != node) {
      return {};
    }
  }

  try {
    return entry->focusCallback(focused);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, TreeError> EventRouter::stageFocus(Node<Widget> node, detail::FocusChange &change) {
  if (state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  change.previous = state_->focus;
  change.next = node;
  for (auto ancestor = node; ancestor.id().session != 0;) {
    if (scope(ancestor)) {
      change.ancestry.push_back(ancestor);
    }
    auto parent = tree().inputParent(ancestor);
    if (!parent) {
      return std::unexpected(parent.error());
    }
    ancestor = *parent;
  }
  state_->history.reserve(state_->history.size() + (node.id().session != 0));
  auto layer = dialogLayer(node);
  if (layer && layer->options.mode == DialogMode::NON_MODAL && !within(layer->root, state_->focus) &&
      !std::ranges::any_of(state_->focusReturns, [&](const auto &entry) {
        return entry->root == layer->root;
      })) {
    auto *registered = scope(layer->root);
    if (!registered) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (state_->focusReturns.size() >= state_->options.max_registrations) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    change.entry = std::make_unique<detail::FocusReturnRegistration>(layer->root, state_->focus, state_->focusScopes,
                                                                     registered->cancellation, true);
    state_->focusReturns.reserve(state_->focusReturns.size() + 1);
  }
  return {};
}

void EventRouter::publishFocus(detail::FocusChange &change) noexcept {
  const auto node = change.next;
  if (node == state_->focus) {
    return;
  }
  auto previousLayer = dialogLayer(change.previous);
  auto nextLayer = dialogLayer(node);
  if (previousLayer && (previousLayer->options.mode == DialogMode::NON_MODAL || !previousLayer->open) &&
      !within(previousLayer->root, node) && (!nextLayer || nextLayer->options.mode != DialogMode::MODAL)) {
    std::erase_if(state_->focusReturns, [&](const auto &entry) {
      return entry->dialog && entry->root == previousLayer->root;
    });
  }
  if (change.entry) {
    change.entry->publish(state_->focusReturnSerial);
    state_->focusReturns.push_back(std::move(change.entry));
  }
  if (node.id().session != 0) {
    auto found = std::ranges::find(state_->history, node);
    if (found != state_->history.end()) {
      state_->history.erase(found);
    }
    state_->history.push_back(node);
  }
  state_->focus = node;
  state_->focusScopes.swap(change.ancestry);
  for (auto root : state_->focusScopes) {
    if (auto *entry = scope(root)) {
      entry->last = node;
    }
  }
  if (state_->focusGeneration) {
    ++state_->focusGeneration;
  }
  // Publish the optional owning observation before any reentrant focus delivery.
  if (auto observation = state_->focusObservation.lock()) {
    observation->focused = node;
    observation->generation = state_->focusGeneration;
  }
}

std::expected<void, TreeError> EventRouter::notifyFocus(const detail::FocusChange &change) {
  if (change.previous == change.next) {
    return {};
  }

  // Only this actual transition can leave the capture focus subtree. Acquiring from a
  // non-focusable pointer owner does not require changing a pre-existing external focus.
  if (state_->pointerCapture && !within(state_->pointerCapture->focusRoot, change.next)) {
    auto cancelled = retirePointerCapture(false);

    if (!cancelled) {
      return cancelled;
    }
  }

  if (auto *recorder = tree().diagnostics(); recorder && recorder->enabled()) {
    const auto id = change.next.id();
    recorder->note(DiagnosticEvent::FOCUS, DiagnosticReason::NONE, {id.session, id.slot, id.generation});
  }
  for (auto node : {change.previous, change.next}) {
    if (node.id().session != 0) {
      auto status = tree().invalidate(node, Invalidation::PAINT);
      if (!status && status.error() != TreeError::STALE_NODE && status.error() != TreeError::CLOSED) {
        return status;
      }
    }
  }

  struct Guard {
    bool &active;

    ~Guard() {
      active = false;
    }
  } guard{state_->deliveringFocus};

  state_->deliveringFocus = true;
  auto lost = deliverFocus(change.previous, false);
  auto gained = deliverFocus(change.next, true);
  return lost ? gained : lost;
}

std::expected<void, TreeError> EventRouter::changeFocus(Node<Widget> node) {
  if (state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  if (node == state_->focus) {
    return {};
  }
  detail::FocusChange change;
  auto staged = stageFocus(node, change);
  if (!staged) {
    return std::unexpected(staged.error());
  }
  publishFocus(change);
  return notifyFocus(change);
}

std::expected<Node<Widget>, TreeError> EventRouter::checkedFocus() {
  auto entry = registration(state_->focus);

  if (!entry || !entry->focusable) {
    return Node<Widget>{};
  }

  auto eligible = tree().inputEligible(state_->focus);

  if (!eligible) {
    return eligible.error() == TreeError::STALE_NODE || eligible.error() == TreeError::CLOSED
             ? std::expected<Node<Widget>, TreeError>(Node<Widget>{})
             : std::unexpected(eligible.error());
  }

  // A callback can retire the selected target. Do not start another callback cycle here.
  return *eligible && dialogEligible(state_->focus) ? state_->focus : Node<Widget>{};
}

std::expected<Node<Widget>, TreeError> EventRouter::focused() {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto returned = recoverFocusScopes();

  if (!returned || *returned) {
    return returned ? checkedFocus() : std::unexpected(returned.error());
  }

  try {
    auto order = focusOrder();

    if (!order) {
      return std::unexpected(order.error());
    }

    if (std::ranges::contains(*order, state_->focus)) {
      return state_->focus;
    }

    Node<Widget> next = fallbackFocus(state_->focusScopes, *order);

    auto changed = changeFocus(next);

    if (!changed) {
      return std::unexpected(changed.error());
    }

    return checkedFocus();
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> EventRouter::setFocus(Node<Widget> node) {
  auto allowed = collect();

  if (!allowed) {
    return allowed;
  }

  auto eligible = tree().inputEligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  auto entry = registration(node);

  auto modal = modalRoot();
  if (!*eligible || !entry || !entry->focusable || !dialogEligible(node) ||
      (modal != Node<Widget>{} && !within(modal, node))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    return changeFocus(node);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::traverse(bool reverse) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto returned = recoverFocusScopes();

  if (!returned || *returned) {
    return returned ? checkedFocus() : std::unexpected(returned.error());
  }

  try {
    auto order = focusOrder();

    if (!order) {
      return std::unexpected(order.error());
    }

    Node<Widget> next;

    if (!order->empty()) {
      auto position = std::ranges::find(*order, state_->focus);

      if (position == order->end()) {
        next = reverse ? order->back() : order->front();
      } else if (reverse) {
        next = position == order->begin() ? order->back() : *std::prev(position);
      } else {
        ++position;
        next = position == order->end() ? order->front() : *position;
      }
    }

    auto changed = changeFocus(next);

    if (!changed) {
      return std::unexpected(changed.error());
    }

    return checkedFocus();
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::hitTest(CellPoint point) {
  auto allowed = check();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  try {
    auto root = tree().root();
    std::vector<Node<Widget>> pending{*root};
    Node<Widget> hit;

    while (!pending.empty()) {
      const auto node = pending.back();
      auto snapshot = tree().inspect(node);

      pending.pop_back();

      if (!snapshot) {
        return std::unexpected(snapshot.error());
      }

      const auto &geometry = snapshot->geometry;

      if (!snapshot->visible || !snapshot->enabled || !dialogEligible(node) || !geometry.valid ||
          !geometry.surface.contains(point) || !geometry.clip.contains(point)) {
        continue;
      }

      // Retaining the last preorder hit gives descendants precedence over their ancestors
      // and later sibling subtrees precedence over earlier ones, matching painting order.
      const auto host = state_->dialogs.lock();
      auto layer = dialogLayer(node);
      const auto notifications = state_->notifications.lock();
      if ((!notifications || notifications->retired || !notifications->ready || notifications->root != node) &&
          (!host || host->root != node) &&
          (!layer || layer->root != node || layer->options.mode == DialogMode::MODAL)) {
        hit = node;
      }
      pending.insert(pending.end(), snapshot->children.rbegin(), snapshot->children.rend());
    }

    return hit;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}
} // namespace portals
