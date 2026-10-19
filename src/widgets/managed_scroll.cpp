// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"
#include "scroll_controls_state.hpp"
#include "scroll_preparation_state.hpp"
#include <portals/managed.hpp>

#include <limits>

namespace portals {
std::expected<Node<ScrollBar>, ManagedError> ManagedScene::scrollBar(Node<Widget> parent,
                                                                     std::shared_ptr<ScrollController> controller,
                                                                     ScrollAxis axis, ScrollBarOptions options,
                                                                     ControlStyle style, bool geometryWriter) {
  return emplaceConfigured<ScrollBar>(parent, geometryWriter, std::move(controller), axis, widthPolicy(), options,
                                      style);
}

std::expected<void, TreeError> ManagedScene::attachScroll(const std::shared_ptr<Entry> &entry, Node<ScrollArea> node,
                                                          bool) {
  auto bound = bindScrollArea(app_.router(), app_.tree(), node);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  entry->binding.emplace<ActionBinding>(std::move(*bound));
  entry->scrollArea = node;
  return attachScrollValues(entry);
}

std::expected<void, TreeError> ManagedScene::attachScroll(const std::shared_ptr<Entry> &entry, Node<ScrollBar> node,
                                                          bool writer) {
  auto bound = bindScrollBar(app_.router(), app_.tree(), node, writer);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  entry->binding.emplace<ActionBinding>(std::move(*bound));
  return attachScrollValues(entry);
}

std::expected<void, TreeError> ManagedScene::attachScrollValues(const std::shared_ptr<Entry> &entry) {
  auto focus = detail::EventRouterAccess::observeFocus(app_.router());
  if (!focus) {
    return std::unexpected(focus.error());
  }
  auto candidate = scroll_ ? scroll_ : std::make_shared<detail::ManagedScrollState>(*focus);
  auto intent = std::make_shared<detail::ScrollIntentRecord>(entry->node.id(), *focus);
  entry->sync = [this, node = entry->node](Node<Widget> focused) {
    return syncScrollControl(app_.tree(), node, focused);
  };
  std::expected<void, TreeError> status{std::unexpected(TreeError::INVALID_ARGUMENT)};
  auto edited = app_.tree().edit(entry->node, [&](Widget &widget) {
    auto *state = detail::ScrollWidgetAccess::state(widget);
    if (!state || !state->lease || state->retired || state->node != entry->node.id()) {
      return;
    }
    state->intent = intent;
    entry->scrollController = state->controller;
    status = {};
  });
  if (!edited || !status) {
    return edited ? status : edited;
  }
  // No allocation follows publication into this entry. emplace publishes the scene owner only
  // after its checked entry insertion; a failing insertion destroys this scoped candidate.
  entry->scrollIntent = std::move(intent);
  entry->scrollOwner = scroll_ ? scroll_ : std::move(candidate);
  return {};
}

void ManagedScene::publishScroll(const std::shared_ptr<Entry> &entry) noexcept {
  if (!entry->scrollOwner) {
    return;
  }
  if (!scroll_) {
    scroll_ = entry->scrollOwner;
  }
  entry->scrollOwner = scroll_;
}

void ManagedScene::pruneScroll() noexcept {
  if (!scroll_) {
    return;
  }
  if (!std::ranges::any_of(entries_, [](const auto &entry) {
        return !entry->removed && entry->scrollIntent;
      })) {
    scroll_.reset();
  }
}

bool ManagedScene::usesGeometrySnapshot() const noexcept {
  if (!scroll_) {
    return false;
  }
  return std::ranges::any_of(entries_, [](const auto &entry) {
    return !entry->removed && entry->scrollArea != Node<ScrollArea>{};
  });
}

bool ManagedScene::requiresPointerMotion() const noexcept {
  return scroll_ && std::ranges::any_of(entries_, [](const auto &entry) {
           return !entry->removed && entry->scrollIntent;
         });
}

std::expected<void, ManagedError> ManagedScene::setScrollOffset(Node<Widget> node, ScrollAxis axis,
                                                                ScrollOffset offset) {
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return allowed;
  }
  auto entry = find(node);
  if (!entry || !entry->scrollIntent) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }
  auto result = portals::setScrollOffset(app_.tree(), node, axis, offset);
  return result ? std::expected<void, ManagedError>{}
                : std::unexpected(error(ManagedPhase::INPUT, node, result.error()));
}

std::expected<void, ManagedError> ManagedScene::setScrollMetrics(Node<ScrollBar> node, ScrollAxisMetrics metrics) {
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return allowed;
  }
  auto entry = find(node);
  if (!entry || !entry->scrollIntent) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }
  auto result = portals::setScrollMetrics(app_.tree(), node, metrics);
  return result ? std::expected<void, ManagedError>{}
                : std::unexpected(error(ManagedPhase::INPUT, node, result.error()));
}

bool ManagedScene::scrollIntentIntersects(const Entry &entry, std::span<const Node<Widget>> chain) const noexcept {
  return std::ranges::any_of(entries_, [&](const auto &area) {
    return !area->removed && area->scrollArea != Node<ScrollArea>{} &&
           area->scrollController == entry.scrollController &&
           std::ranges::find(chain, Node<Widget>(area->scrollArea)) != chain.end();
  });
}

std::expected<void, TreeError> ManagedScene::queueScrollReveal(Node<Widget> node, Node<Widget> root,
                                                               bool explicitRequest) {
  const auto construct = [&]() -> std::expected<void, TreeError> {
    if (!scroll_ || !usesGeometrySnapshot()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    const auto owner = scroll_;
    if (explicitRequest && owner->explicitSerial == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    // Request construction itself can allocate in checked Debug containers. Keep the old
    // target and its stamp together even if that allocation admits a focus transition.
    const auto focused = owner->focus->focused;
    const auto generation = owner->focus->generation;
    const auto serial = explicitRequest ? owner->explicitSerial + 1 : 0;
    auto staged = std::make_unique<detail::ScrollRequest>();

    auto &request = *staged;

    request.reveal = {node, root == Node<Widget>{} ? root_ : root};
    request.explicitRequest = explicitRequest;
    request.focused = focused;
    request.focusGeneration = generation;
    request.serial = serial;
    // Take intent baselines before any chain allocation can reenter a logical command.
    request.intents.reserve(entries_.size());
    for (const auto &entry : entries_) {
      if (entry->scrollIntent) {
        request.intents.push_back({entry->node.id(), entry->scrollIntent->serial});
      }
    }
    auto current = node;
    Node<Widget> aboveRoot;
    bool reached{};
    while (current != Node<Widget>{}) {
      auto value = app_.tree().inspect(current);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (!value->visible) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      request.chain.push_back(current);
      if (current == request.reveal.root) {
        reached = true;
        aboveRoot = value->parent;
        break;
      }
      current = value->parent;
    }
    if (!reached) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    // A limiting root does not make a hidden ancestor above it visible.
    for (auto ancestor = aboveRoot; ancestor != Node<Widget>{};) {
      auto value = app_.tree().inspect(ancestor);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (!value->visible) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      ancestor = value->parent;
    }
    const bool inArea = std::ranges::any_of(entries_, [&](const auto &entry) {
      return entry->scrollArea != Node<ScrollArea>{} && entry->node != node &&
             std::ranges::find(request.chain, entry->node) != request.chain.end();
    });
    if (!inArea) {
      return explicitRequest ? std::expected<void, TreeError>(std::unexpected(TreeError::INVALID_ARGUMENT))
                             : std::expected<void, TreeError>{};
    }
    if (scroll_ != owner || closed_) {
      return std::unexpected(TreeError::CLOSED);
    }
    // A recursive admission can win during allocation. Do not replace its newer explicit serial.
    if (explicitRequest && owner->explicitSerial + 1 != request.serial) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (!explicitRequest && owner->pending && owner->pending->explicitRequest) {
      return {};
    }
    auto visible = app_.tree().revealVisible(node);
    if (!visible) {
      return std::unexpected(visible.error());
    }
    if (!*visible) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto rootVisible = app_.tree().revealVisible(request.reveal.root);
    if (!rootVisible) {
      return std::unexpected(rootVisible.error());
    }
    if (!*rootVisible) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    // Publish the stationary holder without moving its checked Debug containers. Keep the
    // displaced request alive until pending and its serial have both been committed.
    owner->pending.swap(staged);
    if (explicitRequest) {
      owner->explicitSerial = owner->pending->serial;
    }
    return {};
  };
  std::expected<void, TreeError> result;
  // Pin record storage through fallible inspect copies; logical removal remains immediately visible.
  auto borrowed = app_.tree().withScope(node, [&](const UiScope &) {
    try {
      result = construct();
    } catch (const std::bad_alloc &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    }
  });
  return borrowed ? result : borrowed;
}

std::expected<void, ManagedError> ManagedScene::ensureVisible(Node<Widget> node, Node<Widget> root) {
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return allowed;
  }
  if (root != Node<Widget>{}) {
    auto live = app_.tree().read(root, [](const Widget &) {
    });
    if (!live) {
      return std::unexpected(error(ManagedPhase::INPUT, root, live.error()));
    }
  }
  if ((node != root_ && !find(node)) || (root != Node<Widget>{} && root != root_ && !find(root))) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }
  try {
    auto queued = queueScrollReveal(node, root, true);
    if (queued) {
      queued = app_.tree().invalidate(node, Invalidation::LAYOUT);
    }
    return queued ? std::expected<void, ManagedError>{}
                  : std::unexpected(error(ManagedPhase::INPUT, node, queued.error()));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, TreeError> ManagedScene::sampleScrollFocus() {
  if (!scroll_) {
    return {};
  }
  auto &state = *scroll_;
  const auto focus = state.focus;
  if (focus->retired || !focus->generation) {
    state.automaticExhausted = true;
    if (state.pending && !state.pending->explicitRequest) {
      state.pending.reset();
    }
    return {};
  }
  if (state.automaticExhausted ||
      (state.observedFocus == focus->focused && state.observedGeneration == focus->generation)) {
    return {};
  }
  const auto focused = focus->focused;
  const auto generation = focus->generation;
  if (state.pending && state.pending->explicitRequest) {
    state.observedFocus = focused;
    state.observedGeneration = generation;
    return {};
  }
  state.pending.reset();
  if (focused == Node<Widget>{} || !find(focused)) {
    state.observedFocus = focused;
    state.observedGeneration = generation;
    return {};
  }
  auto queued = queueScrollReveal(focused, root_, false);
  if (!queued && queued.error() != TreeError::STALE_NODE && queued.error() != TreeError::INVALID_ARGUMENT) {
    return queued;
  }
  // A failed fallible admission must remain retryable. A later focus transition during
  // successful allocation is still sampled next pass; never mark that newer stamp observed.
  state.observedFocus = focused;
  state.observedGeneration = generation;
  return {};
}

std::expected<void, TreeError> ManagedScene::prepareScrollFrame(const LayoutSnapshot &snapshot) {
  if (!usesGeometrySnapshot() || !scroll_) {
    return {};
  }
  try {
    auto sampled = sampleScrollFocus();
    if (!sampled) {
      return sampled;
    }
    const auto owner = scroll_;
    std::vector<Node<ScrollArea>> areas(0);
    areas.reserve(entries_.size());
    for (const auto &entry : entries_) {
      if (!entry->removed && entry->scrollArea != Node<ScrollArea>{}) {
        auto live = app_.tree().read(entry->node, [](const Widget &) {
        });
        if (!live && (live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED)) {
          entry->removed = true;
          entry->scrollIntent.reset();
          entry->scrollOwner.reset();
          entry->scrollController.reset();
          continue;
        }
        if (!live) {
          return live;
        }
        areas.push_back(entry->scrollArea);
      }
    }
    if (areas.empty()) {
      if (owner->pending && owner->pending->explicitRequest) {
        auto live = app_.tree().read(owner->pending->reveal.target, [](const Widget &) {
        });
        if (!live) {
          return live;
        }
      }
      owner->pending.reset();
      pruneScroll();
      return {};
    }
    auto &pending = owner->pending;
    if (pending) {
      pending->settled = false;
    }
    const auto reveal = pending ? std::optional(pending->reveal) : std::nullopt;
    const auto explicitSerial = pending && pending->explicitRequest ? pending->serial : 0;
    const auto focusNode = pending ? pending->focused : Node<Widget>{};
    const auto focusGeneration = pending ? pending->focusGeneration : 0;
    auto plan = detail::planScrollAreas(app_.tree(), areas, snapshot, reveal);
    if (!plan) {
      if (pending && !pending->explicitRequest && plan.error() == TreeError::STALE_NODE) {
        pending.reset();
        return app_.tree().invalidate(root_, Invalidation::LAYOUT);
      }
      return std::unexpected(plan.error());
    }
    auto fresh = detail::admitScrollPlan(app_.tree(), **plan);
    if (!fresh) {
      return std::unexpected(fresh.error());
    }
    if (*fresh == detail::ScrollSnapshotAdmission::STALE) {
      return app_.tree().invalidate(root_, Invalidation::LAYOUT);
    }
    bool admitted = static_cast<bool>(pending);
    bool repeat{};
    if (scroll_ != owner) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    // Every plan allocation is complete. Read only owning metadata and checked liveness now.
    if (pending && ((pending->explicitRequest && pending->serial != explicitSerial) ||
                    (!pending->explicitRequest &&
                     (pending->focused != focusNode || pending->focusGeneration != focusGeneration)))) {
      admitted = false;
      repeat = true;
    }
    if (admitted && !pending->explicitRequest &&
        (owner->focus->retired || owner->focus->focused != pending->focused ||
         owner->focus->generation != pending->focusGeneration || !owner->focus->generation)) {
      pending.reset();
      admitted = false;
      repeat = true;
    }
    if (admitted) {
      auto live = app_.tree().read(pending->reveal.target, [](const Widget &) {
      });
      if (!live) {
        if (pending->explicitRequest || live.error() != TreeError::STALE_NODE) {
          return live;
        }
        pending.reset();
        admitted = false;
      }
    }
    if (admitted) {
      for (const auto &entry : entries_) {
        if (!entry->scrollIntent || !scrollIntentIntersects(*entry, pending->chain)) {
          continue;
        }
        const auto &intent = *entry->scrollIntent;
        const auto baseline =
          std::ranges::find(pending->intents, entry->node.id(), &detail::ScrollIntentWatermark::node);
        const bool newer = baseline == pending->intents.end() ? intent.serial != 0 : intent.serial != baseline->serial;
        const bool sameFocusIntent = !pending->explicitRequest && intent.serial && intent.focusGeneration &&
                                     intent.focusGeneration == pending->focusGeneration &&
                                     intent.focused == pending->focused.id();
        if (newer || sameFocusIntent) {
          pending.reset();
          admitted = false;
          break;
        }
      }
    }
    bool revealEligible = admitted;
    if (admitted && !pending->explicitRequest) {
      auto eligible = app_.tree().inputEligible(pending->reveal.target);
      if (!eligible) {
        return std::unexpected(eligible.error());
      }
      revealEligible = *eligible;
    }
    auto refreshed = detail::refreshScrollPlans(app_.tree(), **plan, revealEligible);
    if (!refreshed) {
      return std::unexpected(refreshed.error());
    }
    if (*refreshed == detail::ScrollSnapshotAdmission::STALE) {
      return app_.tree().invalidate(root_, Invalidation::LAYOUT);
    }
    auto adopted = detail::adoptScrollAreas(app_.tree(), **plan);
    if (!adopted) {
      return std::unexpected(adopted.error());
    }
    if (adopted->admission == detail::ScrollSnapshotAdmission::STALE) {
      return app_.tree().invalidate(root_, Invalidation::LAYOUT);
    }
    if (admitted && pending) {
      pending->settled = !adopted->result.layoutChanged;
    }
    if (repeat) {
      return app_.tree().invalidate(root_, Invalidation::LAYOUT);
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

void ManagedScene::completeScrollFrame() noexcept {
  if (!scroll_ || !scroll_->pending || !scroll_->pending->settled) {
    return;
  }
  const auto &request = *scroll_->pending;
  if (request.explicitRequest) {
    scroll_->consumedExplicit = request.serial;
  } else if (request.focusGeneration) {
    scroll_->consumedFocus = request.focused;
    scroll_->consumedGeneration = request.focusGeneration;
  }
  scroll_->pending.reset();
}
} // namespace portals
