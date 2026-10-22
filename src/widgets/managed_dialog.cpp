// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

#include "action_state.hpp"
#include "dialog_detail.hpp"
#include "message_box_state.hpp"

#include <algorithm>
#include <limits>

namespace portals {
std::expected<Node<Widget>, TreeError> ManagedScene::messageParent(Node<Widget> node) const {
  return app_.tree().inputParent(node);
}

bool ManagedScene::messageWithin(Node<Widget> root, Node<Widget> node) const {
  return app_.router().within(root, node);
}

bool ManagedScene::messageOwner(Node<Widget> owner) const {
  if (owner == root_) {
    return true;
  }

  auto &router = app_.router();
  const auto modal = router.modalRoot();

  return dialogs_ && (router.within(dialogs_->body, owner) || (modal != Node<Widget>{} && router.within(modal, owner)));
}

std::shared_ptr<detail::DialogRecord> EventRouter::dialogLayer(Node<Widget> node) const {
  auto host = state_->dialogs.lock();
  if (!host || host->retired) {
    return {};
  }
  for (const auto &layer : host->layers) {
    if (!layer->retired && within(layer->root, node)) {
      return layer;
    }
  }
  return {};
}

Node<Widget> EventRouter::modalRoot(Node<Widget> exclude) const {
  auto host = state_->dialogs.lock();
  if (!host || host->retired) {
    return {};
  }
  for (auto &record : host->layers) {
    auto &layer = *record;
    auto eligible = state_->tree.inputEligible(layer.root);
    auto revision = state_->tree.inputRevision(layer.root);
    auto geometry = state_->tree.observeGeometry(layer.root);
    if (!eligible) {
      layer.retired = true;
      layer.open = false;
    } else if (!*eligible || !revision || !*revision || (layer.open && *revision != layer.activationRevision)) {
      if (layer.open) {
        layer.keepDisabledPicture = geometry && geometry->visible && !*eligible;
      }
      layer.open = false;
    }
  }
  for (auto it = host->layers.rbegin(); it != host->layers.rend(); ++it) {
    if ((*it)->open && (*it)->root != exclude && (*it)->options.mode == DialogMode::MODAL) {
      return (*it)->root;
    }
  }
  return {};
}

bool EventRouter::dialogEligible(Node<Widget> node, Node<Widget> reveal, Node<Widget> exclude) const {
  if (state_->dialogs.expired()) {
    return true;
  }
  auto modal = modalRoot(exclude);
  auto layer = dialogLayer(node);
  if (layer && layer->root != reveal && (!layer->open || layer->root == exclude)) {
    return false;
  }
  auto opening = dialogLayer(reveal);
  if (opening && opening->options.mode == DialogMode::MODAL) {
    modal = reveal;
  }
  // Ancestors remain traversable; focus and explicit input perform the stricter subtree check.
  return modal == Node<Widget>{} || within(modal, node) || within(node, modal);
}

std::shared_ptr<detail::DialogRecord> ManagedScene::findDialog(Node<Widget> node) const {
  if (dialogs_) {
    for (const auto &record : dialogs_->layers) {
      if (record->root == node && !record->retired) {
        return record;
      }
    }
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::overlayHost(Node<OverlayHost> host, Node<Widget> body) {
  auto allowed = boundary(ManagedPhase::BIND, host);
  if (!allowed) {
    return allowed;
  }
  if (dialogOperation_ || collectingDialogs_) {
    return std::unexpected(error(ManagedPhase::BIND, host, TreeError::WOULD_BLOCK));
  }
  retireDialogs();
  try {
    auto &router = app_.router();
    auto existing = router.state_->dialogs.lock();
    if ((existing && !existing->retired) || !find(host) || !find(body)) {
      return std::unexpected(error(ManagedPhase::BIND, host, TreeError::INVALID_ARGUMENT));
    }
    if (auto notifications = router.state_->notifications.lock();
        notifications && !notifications->retired && !router.within(body, notifications->wrapper)) {
      return std::unexpected(error(ManagedPhase::BIND, host, TreeError::INVALID_ARGUMENT));
    }
    if (auto registration = router.registration(host); registration && registration->focusable) {
      return std::unexpected(error(ManagedPhase::BIND, host, TreeError::INVALID_ARGUMENT));
    }
    auto record = std::make_shared<detail::DialogHostRecord>();
    record->root = host;
    record->body = body;
    auto read = app_.tree().read(host, [&](const OverlayHost &widget) {
      record->maximum = widget.hostOptions().maxLayers;
    });
    auto parent = app_.tree().inputParent(body);
    if (!read || !parent) {
      return std::unexpected(error(ManagedPhase::BIND, host, read ? parent.error() : read.error()));
    }
    if (!record->maximum || *parent != host ||
        router.state_->registrations.size() + router.state_->scopes.size() + !router.state_->notifications.expired() >=
          router.state_->options.max_registrations) {
      return std::unexpected(
        error(ManagedPhase::BIND, host,
              !record->maximum || *parent != host ? TreeError::INVALID_ARGUMENT : TreeError::NODE_LIMIT));
    }
    std::expected<void, TreeError> publication;
    auto edited = app_.tree().edit(host, [&](OverlayHost &widget) {
      auto last = app_.tree().inputParent(body);
      if (!last || *last != host || !find(body)) {
        publication = std::unexpected(last ? TreeError::INVALID_ARGUMENT : last.error());
        return;
      }
      detail::DialogAccess::bindBody(widget, body.id());
      record->ready = true;
      dialogs_ = record;
      router.state_->dialogs = record;
    });
    return !edited || !publication
             ? std::unexpected(error(ManagedPhase::BIND, host, edited ? publication.error() : edited.error()))
             : std::expected<void, ManagedError>{};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, host, TreeError::NO_MEMORY));
  }
}

std::expected<Node<Widget>, ManagedError> ManagedScene::makeDialogPanel(Node<Dialog> node, BorderOptions options) {
  auto scope = focusScope(node);
  if (!scope) {
    return std::unexpected(scope.error());
  }
  auto panel = emplace<detail::DialogPanel>(node, widthPolicy(), options);
  if (!panel) {
    return std::unexpected(panel.error());
  }
  try {
    auto entry = find(*panel);
    entry->target = std::make_shared<StyleTarget>(StyleTarget{.type = "Border", .classes = {"dialog-panel"}});
    entry->sync = [this, node = Node<Border>(*panel)](Node<Widget> focused) {
      return syncBorder(app_.tree(), node, focused);
    };
    entry->stylePending = true;
    stylesDirty_ = true;
    return Node<Widget>(*panel);
  } catch (const std::bad_alloc &) {
    return rollback(*panel, TreeError::NO_MEMORY);
  }
}

std::expected<void, ManagedError> ManagedScene::dialog(Node<Dialog> node, Node<Widget> panel, Node<Widget> body) {
  auto allowed = boundary(ManagedPhase::BIND, node);
  if (!allowed) {
    return allowed;
  }
  if (dialogOperation_ || collectingDialogs_) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::WOULD_BLOCK));
  }
  retireDialogs();
  try {
    auto host = dialogs_;
    auto &router = app_.router();
    if (!host || host->retired || !find(node) || !find(panel) || !find(body) || findDialog(node)) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }
    if (host->layers.size() >= host->maximum) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
    }
    auto record = std::make_shared<detail::DialogRecord>();
    record->root = node;
    record->panel = panel;
    record->body = body;
    record->host = host;
    auto read = app_.tree().read(node, [&](const Dialog &widget) {
      record->options = widget.options();
    });
    auto parent = app_.tree().inputParent(node);
    auto panelParent = app_.tree().inputParent(panel);
    if (!read || !parent || !panelParent) {
      return std::unexpected(error(ManagedPhase::BIND, node,
                                   !read     ? read.error()
                                   : !parent ? parent.error()
                                             : panelParent.error()));
    }
    if (*parent != host->root || *panelParent != node || !router.within(panel, body)) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }
    auto valid = Dialog::validate(record->options);
    if (!valid) {
      return std::unexpected(error(ManagedPhase::BIND, node, valid.error()));
    }
    host->layers.reserve(host->layers.size() + 1);
    auto entry = find(node);
    std::optional<ActionBinding> scoped;
    if (!router.scope(node)) {
      auto registered = router.focusScope(node);
      if (!registered) {
        return std::unexpected(error(ManagedPhase::BIND, node, registered.error()));
      }
      scoped.emplace(std::move(*registered));
    }
    auto binding = router.bind(
      node,
      [this, record, life = std::weak_ptr<const void>(lifetime_)](Dialog &, const RoutedEvent &event) {
        const auto *action = std::get_if<ActionEvent>(&event);
        if (!record->open || !action || action->action != Action::CANCEL || !record->options.closeOnEscape ||
            life.expired()) {
          return EventReply::IGNORED;
        }
        if (app_.router().state_->dialogCancelRepeat) {
          return EventReply::HANDLED;
        }
        auto result = dialogCommand(record->root, false, DialogDismissReason::CANCEL);
        if (!result && !shortReported(result.error())) {
          auto failure = result.error();
          failure.operation = "Dialog.dismiss";
          failure.operationOrigin = std::source_location::current();
          if (!failure_) {
            // This internal dialog primary also owns its attempted overflow diagnostic.
            dialogPrimaryRecorded_ = true;
          }
          remember(failure, *failure.operationOrigin);
        }
        return EventReply::HANDLED;
      },
      false);
    if (!binding) {
      return std::unexpected(error(ManagedPhase::BIND, node, binding.error()));
    }
    // The root is cold and unobservable until the containing mount publishes its references.
    std::expected<void, TreeError> committed;
    auto hidden = app_.tree().edit(node, [&](Dialog &) {
      auto lastParent = app_.tree().inputParent(node);
      auto lastPanel = app_.tree().inputParent(panel);
      if (!lastParent || !lastPanel) {
        committed = std::unexpected(lastParent ? lastPanel.error() : lastParent.error());
        return;
      }
      if (dialogs_ != host || *lastParent != host->root || *lastPanel != node || !router.within(panel, body) ||
          host->layers.size() >= host->maximum || host->layers.size() == host->layers.capacity() || !find(body)) {
        committed = std::unexpected(TreeError::INVALID_ARGUMENT);
        return;
      }
      committed = app_.tree().setVisible(node, false);
      if (committed) {
        if (scoped) {
          entry->focusScope = std::move(*scoped);
        }
        entry->binding = std::move(*binding);
        record->ready = true;
        host->layers.push_back(record);
      }
    });
    if (!hidden || !committed) {
      return std::unexpected(error(ManagedPhase::BIND, node, hidden ? committed.error() : hidden.error()));
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> ManagedScene::dialogAnchor(Node<Widget> node, Node<Widget> anchor,
                                                             DialogPlacement placement) {
  auto allowed = boundary(ManagedPhase::BIND, node);
  if (!allowed) {
    return allowed;
  }
  auto record = findDialog(node);
  auto live = app_.tree().inputParent(anchor);
  if (!live) {
    return std::unexpected(error(ManagedPhase::BIND, anchor, live.error()));
  }
  if (!record || record->open || record->activationRevision ||
      (placement != DialogPlacement::CENTER && placement != DialogPlacement::BELOW_START &&
       placement != DialogPlacement::ABOVE_START) ||
      !app_.router().within(dialogs_->body, anchor)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }
  std::expected<void, TreeError> committed;
  auto updated = app_.tree().edit(record->root, [&](Dialog &widget) {
    auto last = app_.tree().inputParent(anchor);
    if (!last) {
      committed = std::unexpected(last.error());
      return;
    }
    if (!dialogs_ || !app_.router().within(dialogs_->body, anchor) || findDialog(node) != record) {
      committed = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    detail::DialogAccess::anchor(widget, {}, placement);
    record->anchor = anchor;
    record->options.placement = placement;
  });
  return !updated || !committed
           ? std::unexpected(error(ManagedPhase::BIND, node, updated ? committed.error() : updated.error()))
           : std::expected<void, ManagedError>{};
}

std::expected<void, ManagedError>
ManagedScene::setDialogDismiss(Node<Dialog> node,
                               std::function<std::expected<void, TreeError>(DialogDismissReason)> callback) {
  auto allowed = boundary(ManagedPhase::BIND, node);
  if (!allowed) {
    return allowed;
  }
  try {
    auto record = findDialog(node);
    if (!record || record->dismissed || !callback) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }
    auto owned = std::make_shared<decltype(callback)>(std::move(callback));
    allowed = boundary(ManagedPhase::BIND, node);
    if (!allowed) {
      return allowed;
    }
    record->dismissed = std::move(owned);
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

std::expected<bool, ManagedError> ManagedScene::dialogOpened(Node<Widget> node) {
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  (void)app_.router().modalRoot();
  auto record = findDialog(node);
  if (!record) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }
  return record->open;
}

bool ManagedScene::modalDialogInput() {
  return app_.router().modalRoot() != Node<Widget>{};
}

void ManagedScene::retireDialogs() noexcept {
  if (!dialogs_ || collectingDialogs_) {
    return;
  }

  struct Collecting {
    bool &active;

    ~Collecting() {
      active = false;
    }
  } collecting{collectingDialogs_};

  collectingDialogs_ = true;
  auto host = dialogs_;
  (void)app_.router().modalRoot();
  auto live = app_.tree().inputEligible(host->root);
  if (!live || closed_) {
    host->retired = true;
  }
  for (const auto &record : host->layers) {
    auto eligible = app_.tree().inputEligible(record->root);
    if (!eligible || host->retired) {
      record->retired = true;
      record->open = false;
    } else if (!*eligible) {
      record->open = false;
    }
  }
  // Retire every identity before releasing user captures. An admitted dismissal keeps
  // its independent shared function owner until it returns, including self-removal.
  for (const auto &record : host->layers) {
    if (record->retired) {
      record->dismissed.reset();
    }
  }
  std::erase_if(host->layers, [](const auto &record) {
    return record->retired;
  });
  if (host->retired) {
    dialogs_.reset();
  }
}

std::expected<void, TreeError> ManagedScene::changeDialog(Node<Widget> node, bool opening, DialogDismissReason reason) {
  auto &router = app_.router();
  auto host = dialogs_;
  if (!host || host->retired) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (host->operating || dialogOperation_ || collectingDialogs_ || router.state_->deliveringFocus || mountDepth_ ||
      coldMountPending_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  struct Operation {
    std::shared_ptr<detail::DialogHostRecord> host;
    bool &scene;

    ~Operation() {
      host->staging = false;
      host->operating = false;
      scene = false;
    }
  } operation{host, dialogOperation_};

  host->operating = true;
  host->staging = true;
  dialogOperation_ = true;

  try {
    retireDialogs();
    auto record = findDialog(node);
    if (!record || !record->ready || !host->ready) {
      return std::unexpected(record ? TreeError::WOULD_BLOCK : TreeError::STALE_NODE);
    }
    (void)router.modalRoot();
    if (record->open == opening) {
      return {};
    }
    auto modal = router.modalRoot();
    if (!opening && record->options.mode == DialogMode::MODAL && modal != node) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto message = record->message.lock();
    auto request = message ? message->current : nullptr;
    auto targetRevision = app_.tree().inputRevision(node);
    if (!targetRevision) {
      return std::unexpected(targetRevision.error());
    }
    detail::FocusChange change;
    auto dismissed = record->dismissed;
    const auto stage = [&]() -> std::expected<void, TreeError> {
      std::optional<CellRect> anchor;
      std::uint64_t geometryRevision{};
      if (opening && record->options.placement != DialogPlacement::CENTER) {
        if (record->anchor == Node<Widget>{}) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        auto hostGeometry = app_.tree().observeGeometry(host->root);
        auto anchorGeometry = app_.tree().observeGeometry(record->anchor);
        auto revision = app_.tree().geometryRevision();
        if (!hostGeometry || !anchorGeometry || !revision) {
          return std::unexpected(!hostGeometry     ? hostGeometry.error()
                                 : !anchorGeometry ? anchorGeometry.error()
                                                   : revision.error());
        }
        if (!anchorGeometry->visible || !hostGeometry->geometry.valid || !anchorGeometry->geometry.valid ||
            anchorGeometry->geometry.clip.intersection(anchorGeometry->geometry.surface).empty()) {
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
        const auto origin = hostGeometry->geometry.surface.origin();
        const auto source = anchorGeometry->geometry.surface.origin();
        const auto x = std::int64_t(source.x) - origin.x;
        const auto y = std::int64_t(source.y) - origin.y;
        if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
            y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max()) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }
        auto rectangle = CellRect::from({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                                        anchorGeometry->geometry.surface.size());
        if (!rectangle) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }
        anchor = *rectangle;
        geometryRevision = *revision;
      }

      std::vector<std::shared_ptr<detail::DialogRecord>> layers(0);
      layers.reserve(host->layers.size());
      for (const auto &layer : host->layers) {
        if (layer != record) {
          layers.push_back(layer);
        }
      }
      auto position = record->options.mode == DialogMode::MODAL
                        ? layers.end()
                        : std::find_if(layers.begin(), layers.end(), [](const auto &layer) {
                            return layer->options.mode == DialogMode::MODAL;
                          });
      layers.insert(position, record);
      std::vector<Node<Widget>> order(0);
      order.reserve(layers.size());
      for (const auto &layer : layers) {
        order.push_back(layer->root);
      }

      auto focusOrder = router.focusOrder(opening ? node : Node<Widget>{}, opening ? Node<Widget>{} : node);
      if (!focusOrder) {
        return std::unexpected(focusOrder.error());
      }
      auto next = router.state_->focus;
      std::unique_ptr<detail::FocusReturnRegistration> saved;
      std::size_t returnIndex = router.state_->focusReturns.size();
      for (std::size_t i = 0; i != router.state_->focusReturns.size(); ++i) {
        if (router.state_->focusReturns[i]->root == node) {
          returnIndex = i;
          break;
        }
      }
      if (opening && record->options.mode == DialogMode::MODAL) {
        auto *scope = router.scope(node);
        if (!scope) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        const bool existingReturn = returnIndex != router.state_->focusReturns.size();
        if (!existingReturn && router.state_->focusReturns.size() >= router.state_->options.max_registrations) {
          return std::unexpected(TreeError::NODE_LIMIT);
        }
        // Raw visibility revocation can leave focus inside the ended activation. Reopening
        // keeps its original external opener, without accumulating another return owner.
        const bool retainOpener = existingReturn && router.within(node, next);
        const auto previous = retainOpener ? router.state_->focusReturns[returnIndex]->previous : next;
        const auto &previousScopes =
          retainOpener ? router.state_->focusReturns[returnIndex]->previousScopes : router.state_->focusScopes;
        saved =
          std::make_unique<detail::FocusReturnRegistration>(node, previous, previousScopes, scope->cancellation, true);
        if (!existingReturn) {
          router.state_->focusReturns.reserve(router.state_->focusReturns.size() + 1);
        }
        next = router.scopeEntry(node, *focusOrder, false);
        if (record->initialFocus != Node<Widget>{} && std::ranges::contains(*focusOrder, record->initialFocus)) {
          next = record->initialFocus;
        }
      } else if (!opening && (record->options.mode == DialogMode::MODAL || router.within(node, next))) {
        if (returnIndex != router.state_->focusReturns.size()) {
          const auto &previous = *router.state_->focusReturns[returnIndex];
          next = std::ranges::contains(*focusOrder, previous.previous)
                   ? previous.previous
                   : router.fallbackFocus(previous.previousScopes, *focusOrder);
        } else {
          next = router.fallbackFocus(router.state_->focusScopes, *focusOrder);
        }
      }
      const bool changesFocus =
        record->options.mode == DialogMode::MODAL || (!opening && router.within(node, router.state_->focus));
      const auto focusGeneration = router.state_->focusGeneration;
      if (changesFocus) {
        auto staged = router.stageFocus(next, change);
        if (!staged) {
          return std::unexpected(staged.error());
        }
      } else {
        change.previous = router.state_->focus;
        change.next = router.state_->focus;
      }
      // Pin notification captures before the last allocation/admission boundary.
      std::expected<void, TreeError> committed;
      auto pin = app_.tree().edit(record->root, [&](Dialog &widget) {
        // tree.edit may allocate a Debug borrow. Recheck every mutated identity here.
        auto hostEligible = app_.tree().inputEligible(host->root);
        auto rootParent = app_.tree().inputParent(record->root);
        auto panelParent = app_.tree().inputParent(record->panel);
        auto bodyParent = app_.tree().inputParent(record->body);
        auto hostBodyParent = app_.tree().inputParent(host->body);
        auto revision = app_.tree().geometryRevision();
        auto rootEligible = app_.tree().inputEligible(node, opening ? node : Node<Widget>{});
        auto lastTargetRevision = app_.tree().inputRevision(node);
        if (!hostEligible || !rootParent || !panelParent || !bodyParent || !hostBodyParent || !revision ||
            !rootEligible) {
          committed = std::unexpected(!hostEligible     ? hostEligible.error()
                                      : !rootParent     ? rootParent.error()
                                      : !panelParent    ? panelParent.error()
                                      : !bodyParent     ? bodyParent.error()
                                      : !hostBodyParent ? hostBodyParent.error()
                                      : !revision       ? revision.error()
                                                        : rootEligible.error());
          return;
        }
        if (anchor) {
          auto lastAnchor = app_.tree().inputParent(record->anchor);
          if (!lastAnchor) {
            committed = std::unexpected(lastAnchor.error());
            return;
          }
        }
        if (!*hostEligible || (opening && !*rootEligible) || *rootParent != host->root ||
            *panelParent != record->root || !router.within(record->panel, record->body) || !find(record->body) ||
            dialogs_ != host ||
            (anchor && (*revision != geometryRevision || !router.within(host->body, record->anchor))) ||
            router.state_->focusGeneration != focusGeneration) {
          committed = std::unexpected(anchor && *revision != geometryRevision ? TreeError::WOULD_BLOCK
                                                                              : TreeError::INVALID_ARGUMENT);
          return;
        }
        if (!lastTargetRevision || *lastTargetRevision != *targetRevision || record->open == opening) {
          committed = std::unexpected(lastTargetRevision ? TreeError::WOULD_BLOCK : lastTargetRevision.error());
          return;
        }
        if (changesFocus && next != Node<Widget>{}) {
          auto eligible = app_.tree().inputEligible(next, opening ? node : Node<Widget>{});
          if (!eligible || !*eligible) {
            committed = std::unexpected(eligible ? TreeError::INVALID_ARGUMENT : eligible.error());
            return;
          }
        }
        if (message && request && !request->result) {
          if (message->current != request || request->done ||
              (request->async && !detail::asyncInvocationCurrent(request->invocation)) ||
              !app_.tree().inputParent(request->owner)) {
            committed = std::unexpected(TreeError::CLOSED);
            return;
          }
          if (!app_.tree().inputParent(message->paragraph) || !app_.tree().inputParent(message->actions)) {
            committed = std::unexpected(TreeError::STALE_NODE);
            return;
          }
          for (std::size_t i = 0; i < message->count; ++i) {
            if (!app_.tree().inputParent(message->buttons[i])) {
              committed = std::unexpected(TreeError::STALE_NODE);
              return;
            }
          }
        }
        committed = app_.tree().commitOverlay(host->root, host->body, node, order, opening);
        if (!committed) {
          return;
        }
        detail::DialogAccess::anchor(widget, anchor, record->options.placement);
        host->layers.swap(layers);
        record->open = opening;
        record->keepDisabledPicture = false;
        record->activationRevision = *app_.tree().inputRevision(record->root);
        if (saved) {
          if (returnIndex != router.state_->focusReturns.size()) {
            router.state_->focusReturns.erase(router.state_->focusReturns.begin() +
                                              static_cast<std::ptrdiff_t>(returnIndex));
          }
          saved->publish(router.state_->focusReturnSerial);
          router.state_->focusReturns.push_back(std::move(saved));
        } else if (!opening && returnIndex != router.state_->focusReturns.size()) {
          router.state_->focusReturns.erase(router.state_->focusReturns.begin() +
                                            static_cast<std::ptrdiff_t>(returnIndex));
        }
        router.publishFocus(change);
        // Capture retirement follows this protected publication, before user callbacks.
      });
      if (!pin || !committed) {
        return std::unexpected(pin ? committed.error() : pin.error());
      }
      return {};
    };
    std::expected<void, TreeError> staged;
    // Retain all tree records across staging allocations, including removal from allocation hooks.
    // The borrow ends before any focus or dismissal callback is admitted.
    auto protectedStage = app_.tree().edit(host->root, [&](OverlayHost &) {
      try {
        staged = stage();
      } catch (const std::bad_alloc &) {
        // Internal materialization retains its typed allocation error before edit's
        // general user-callback exception boundary translates escaping exceptions.
        staged = std::unexpected(TreeError::NO_MEMORY);
      } catch (...) {
        staged = std::unexpected(TreeError::CALLBACK_FAILED);
      }
    });
    if (!protectedStage || !staged) {
      return std::unexpected(protectedStage ? staged.error() : protectedStage.error());
    }
    host->staging = false;
    // The existing capture owner is independent of focusRoot: a broad root can still
    // contain background controls newly excluded by this published modal boundary.
    const auto activeModal = router.modalRoot();
    if (opening && activeModal != Node<Widget>{} && router.state_->pointerCapture &&
        !router.within(activeModal, router.state_->pointerCapture->node)) {
      auto cancelled = router.retirePointerCapture(false);
      if (!cancelled) {
        return cancelled;
      }
    }
    auto notified = router.notifyFocus(change);
    if (!opening && dismissed && !record->retired) {
      auto live = app_.tree().inputParent(node);
      if (live) {
        std::expected<void, TreeError> result;
        try {
          result = (*dismissed)(reason);
        } catch (const std::bad_alloc &) {
          result = std::unexpected(TreeError::NO_MEMORY);
        } catch (...) {
          result = std::unexpected(TreeError::CALLBACK_FAILED);
        }
        if (notified && !result) {
          notified = result;
        }
      }
    }
    return notified;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, TreeError> ManagedScene::prepareDialogs() {
  retireDialogs();
  auto host = dialogs_;
  if (!host) {
    return {};
  }
  try {
    struct Placement {
      std::shared_ptr<detail::DialogRecord> record;
      std::optional<CellRect> anchor;
      bool hide{};
    };

    std::vector<Placement> placements(0);
    placements.reserve(host->layers.size());
    // Capture all owning observations before the first sibling invalidation.
    auto hostGeometry = app_.tree().observeGeometry(host->root);
    if (!hostGeometry) {
      return std::unexpected(hostGeometry.error());
    }
    for (const auto &record : host->layers) {
      if (!record->open) {
        if (!record->keepDisabledPicture) {
          placements.push_back({record, {}, true});
        }
        continue;
      }
      std::optional<CellRect> anchor;
      if (record->options.placement != DialogPlacement::CENTER) {
        auto observed = app_.tree().observeGeometry(record->anchor);
        if (observed && observed->visible && hostGeometry->geometry.valid && observed->geometry.valid &&
            !observed->geometry.surface.intersection(observed->geometry.clip).empty()) {
          const auto source = observed->geometry.surface.origin();
          const auto origin = hostGeometry->geometry.surface.origin();
          const auto x = std::int64_t(source.x) - origin.x;
          const auto y = std::int64_t(source.y) - origin.y;
          if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
              y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max()) {
            return std::unexpected(TreeError::INVALID_GEOMETRY);
          }
          auto value = CellRect::from({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                                      observed->geometry.surface.size());
          if (!value) {
            return std::unexpected(TreeError::INVALID_GEOMETRY);
          }
          anchor = *value;
        } else if (!observed && observed.error() != TreeError::STALE_NODE) {
          return std::unexpected(observed.error());
        }
      }
      placements.push_back({record, anchor});
    }
    auto expectedRevision = app_.tree().geometryRevision();
    if (!expectedRevision) {
      return std::unexpected(expectedRevision.error());
    }
    for (const auto &placement : placements) {
      bool changed{};
      std::expected<void, TreeError> admitted;
      auto updated = app_.tree().edit(placement.record->root, [&](Dialog &widget) {
        auto revision = app_.tree().geometryRevision();
        if (!revision || *revision != *expectedRevision) {
          admitted = std::unexpected(revision ? TreeError::WOULD_BLOCK : revision.error());
          return;
        }
        if (placement.hide) {
          admitted = app_.tree().setVisible(placement.record->root, false);
          return;
        }
        changed = detail::DialogAccess::anchor(widget, placement.anchor, placement.record->options.placement);
      });
      if (!updated) {
        if (updated.error() == TreeError::STALE_NODE) {
          continue;
        }
        return updated;
      }
      if (!admitted) {
        return admitted;
      }
      if (changed) {
        auto invalidated = app_.tree().invalidate(placement.record->root, Invalidation::LAYOUT);
        if (!invalidated) {
          return invalidated;
        }
      }
      expectedRevision = app_.tree().geometryRevision();
      if (!expectedRevision) {
        return std::unexpected(expectedRevision.error());
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}
} // namespace portals
