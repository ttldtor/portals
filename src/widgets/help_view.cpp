// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_help_state.hpp"
#include <limits>

namespace portals {
std::weak_ptr<detail::ActionCancellation> ManagedScene::helpScope(Node<Widget> node) const noexcept {
  const auto *scope = app_.router().scope(node);
  return scope ? std::weak_ptr(scope->cancellation) : std::weak_ptr<detail::ActionCancellation>{};
}

std::uint64_t ManagedScene::helpActivation(Node<Widget> node) const noexcept {
  for (const auto &saved : app_.router().state_->focusReturns) {
    if (saved->root == node && saved->cancellation->connected.load()) {
      return saved->activation;
    }
  }
  return 0;
}

std::expected<void, ManagedError> ManagedScene::changeHelp(Node<Widget> node, bool opening) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::WRONG_THREAD));
  }
  if (keyStaging_) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::WOULD_BLOCK));
  }
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return allowed;
  }
  const auto entry = find(node);
  if (!entry || !entry->keyHelp || !entry->keyHelp->full) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }
  auto record = entry->keyHelp;
  using Phase = detail::KeyHelpRecord::Phase;
  if (record->phase == Phase::OPENING || record->phase == Phase::CLOSING) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::WOULD_BLOCK));
  }
  if (opening) {
    const auto scope = record->scope.lock();
    const auto current = helpScope(node);
    if (!scope || !scope->connected.load() || current.owner_before(scope) || scope.owner_before(current)) {
      return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
    }
  }
  auto activation = helpActivation(node);
  if (record->phase == Phase::OPEN && activation != record->activation) {
    record->phase = Phase::CLOSED;
  }
  if (opening && record->phase == Phase::OPEN) {
    return {};
  }
  if (!opening && record->phase == Phase::CLOSED) {
    return {};
  }
  if (opening) {
    if (activation) {
      return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
    }
    if (app_.router().state_->focusReturnSerial == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::IDENTITY_EXHAUSTED));
    }
    record->phase = Phase::OPENING;
    auto shown = app_.tree().setVisible(node, true);
    auto eligible = app_.tree().inputEligible(node);
    if (!shown || !eligible || !*eligible) {
      (void)app_.tree().setVisible(node, false);
      record->phase = Phase::CLOSED;
      return std::unexpected(error(ManagedPhase::INPUT, node,
                                   !shown      ? shown.error()
                                   : !eligible ? eligible.error()
                                               : TreeError::INVALID_ARGUMENT));
    }

    struct ReceiptGuard {
      EventRouter::State &state;
      Node<Widget> previous;

      ~ReceiptGuard() {
        state.requiredFocusReceipt = previous;
      }
    } receipt{*app_.router().state_, app_.router().state_->requiredFocusReceipt};

    app_.router().state_->requiredFocusReceipt = node;
    auto entered = enterFocusScope(node);
    record->activation = helpActivation(node);
    if (!record->activation) {
      (void)app_.tree().setVisible(node, false);
      record->phase = Phase::CLOSED;
    } else {
      record->phase = Phase::OPEN;
    }
    return entered;
  }
  record->phase = Phase::CLOSING;
  auto returned = leaveFocusScope(node);
  const bool committed = helpActivation(node) != record->activation;
  if (!committed) {
    record->phase = Phase::OPEN;
    return returned;
  }
  record->activation = 0;
  record->phase = Phase::CLOSED;
  auto hidden = app_.tree().setVisible(node, false);
  if (!returned) {
    return returned;
  }
  return hidden ? std::expected<void, ManagedError>{}
                : std::unexpected(error(ManagedPhase::INPUT, node, hidden.error()));
}
} // namespace portals

namespace portals::ui::detail {
std::expected<void, ManagedError> KeyHelpAccess::command(ManagedScene &scene, Node<Widget> node, bool opening) {
  return scene.changeHelp(node, opening);
}

std::expected<void, ManagedError> helpCommand(const std::shared_ptr<RefTarget> &target, bool opening,
                                              std::source_location origin) {
  auto scene = eventScene(target);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  auto result = KeyHelpAccess::command(**scene, target->node, opening);
  if (!result && !result.error().operation) {
    result.error().operation = opening ? "HelpView.open" : "HelpView.close";
    result.error().operationOrigin = origin;
  }
  return result;
}

Description<HelpView> KeyHelpAccess::full(Ref<HelpView> reference, HelpViewOptions options, ControlStyle style) {
  auto recipe = Access::cold<HelpView>([options = std::move(options), style](
                                         ManagedScene &scene, Node<Widget> parent,
                                         MountContext &context) mutable -> std::expected<Node<HelpView>, ManagedError> {
    if (!options.format.maxBytesPerBinding) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }
    if (options.emptyText.size() > scene.options_.maxKeyProjectionBytes ||
        options.unavailableText.size() > scene.options_.maxKeyProjectionBytes) {
      return std::unexpected(error(parent, TreeError::NODE_LIMIT));
    }
    auto marker = GraphemeText::from(options.unavailableText, scene.widthPolicy());
    if (!marker) {
      auto failure = error(parent, TreeError::INVALID_ARGUMENT);
      failure.text = marker.error();
      return std::unexpected(failure);
    }
    auto record = std::make_shared<portals::detail::KeyHelpRecord>();
    record->full = options;
    record->presentation = std::make_shared<portals::detail::KeyHelpPresentation>(std::move(*marker));
    SizePolicy policy;
    policy.height.mode = SizeMode::FLEX;
    auto root = scene.emplace<HelpView>(parent, policy);
    if (!root) {
      return root;
    }
    record->root = *root;
    auto scope = scene.focusScope(*root);
    if (!scope) {
      return std::unexpected(scope.error());
    }
    record->scope = scene.helpScope(*root);
    auto content =
      Access::describe<portals::detail::HelpBody>([record, style](ManagedScene &owner, Node<Widget> parent) {
        auto body = owner.emplace<portals::detail::HelpBody>(parent, record->presentation, style);
        if (!body) {
          return body;
        }
        auto bound = owner.onEvent(*body, [](portals::detail::HelpBody &, const RoutedEvent &) {
          return std::expected<EventReply, TreeError>(EventReply::IGNORED);
        });
        if (!bound) {
          return std::expected<Node<portals::detail::HelpBody>, ManagedError>(std::unexpected(bound.error()));
        }
        auto initial = owner.focusDefault(*body);
        if (!initial) {
          return std::expected<Node<portals::detail::HelpBody>, ManagedError>(std::unexpected(initial.error()));
        }
        record->body = *body;
        return body;
      });
    auto area = Access::mount(scrollArea(std::move(content)), scene, *root, context);
    if (!area) {
      return std::unexpected(area.error());
    }
    const auto entry = scene.find(*root);
    entry->keyHelp = record;
    entry->viewPrepare = [&scene, record](const LayoutSnapshot &snapshot) {
      return scene.prepareKeyHelp(record, snapshot);
    };
    auto hidden = scene.app_.tree().setVisible(*root, false);
    if (!hidden) {
      return std::unexpected(error(*root, hidden.error()));
    }
    if (options.initiallyOpen) {
      context.finalize.push_back([&scene, root = *root] {
        return scene.changeHelp(root, true);
      });
    }
    return root;
  });
  return std::move(recipe).reference(std::move(reference));
}

Description<KeyHints> KeyHelpAccess::hints(Ref<HelpView> help, KeyHintsOptions options, ControlStyle style) {
  auto target = help.target();
  return Access::cold<KeyHints>([target = std::move(target), options = std::move(options),
                                 style](ManagedScene &scene, Node<Widget> parent,
                                        MountContext &context) mutable -> std::expected<Node<KeyHints>, ManagedError> {
    if (!options.format.maxBytesPerBinding || options.overflowMarker.empty()) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }
    if (options.emptyText.size() > scene.options_.maxKeyProjectionBytes ||
        options.unavailableText.size() > scene.options_.maxKeyProjectionBytes ||
        options.overflowMarker.size() > scene.options_.maxKeyProjectionBytes ||
        options.entrySeparator.size() > scene.options_.maxKeyProjectionBytes) {
      return std::unexpected(error(parent, TreeError::NODE_LIMIT));
    }
    auto marker = GraphemeText::from(options.unavailableText, scene.widthPolicy());
    auto empty = GraphemeText::from("", scene.widthPolicy());
    if (!marker || !empty) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }
    auto record = std::make_shared<portals::detail::KeyHelpRecord>();
    record->hints = options;
    record->presentation = std::make_shared<portals::detail::KeyHelpPresentation>(std::move(*marker));
    auto root = scene.emplace<KeyHints>(parent, std::move(*empty), style);
    if (!root) {
      return root;
    }
    record->root = *root;
    // An unprepared strip has no visible overflow action and must not enter the focus order.
    auto inactive = scene.app_.tree().setEnabled(*root, false);
    if (!inactive) {
      return std::unexpected(error(*root, inactive.error()));
    }
    auto edited = scene.app_.tree().edit(*root, [&](KeyHints &widget) {
      widget.presentation_ = record->presentation;
    });
    if (!edited) {
      return std::unexpected(error(*root, edited.error()));
    }
    auto bound = scene.onClick(Node<Button>(*root), [&scene, record]() -> std::expected<void, ManagedError> {
      if (!record->presentation->current || !record->presentation->text || !record->presentation->text->overflow) {
        return {};
      }
      return scene.changeHelp(record->help, true);
    });
    if (!bound) {
      return std::unexpected(bound.error());
    }
    const auto entry = scene.find(*root);
    entry->keyHelp = record;
    entry->viewPrepare = [&scene, record](const LayoutSnapshot &snapshot) {
      return scene.prepareKeyHelp(record, snapshot);
    };
    context.finalize.push_back([&scene, &context, target, record]() -> std::expected<void, ManagedError> {
      if (!target) {
        return std::unexpected(error(record->root, TreeError::CLOSED));
      }
      const RefTarget *resolved = target.get();
      for (const auto &pending : context.pending) {
        if (pending.identity == target.get()) {
          resolved = &pending.target;
          break;
        }
      }
      if (resolved->owner != std::this_thread::get_id()) {
        return std::unexpected(error(record->root, TreeError::WRONG_THREAD));
      }
      if (resolved->lifetime.expired()) {
        return std::unexpected(error(record->root, TreeError::CLOSED));
      }
      if (resolved->scene != &scene) {
        return std::unexpected(error(record->root, TreeError::INVALID_ARGUMENT));
      }
      auto help = scene.find(resolved->node);
      if (!help || !help->keyHelp || !help->keyHelp->full) {
        return std::unexpected(error(record->root, TreeError::INVALID_ARGUMENT));
      }
      // Keep the complete checked identity; no dynamic name lookup or remount retargeting.
      record->help = resolved->node;
      return {};
    });
    return root;
  });
}
} // namespace portals::ui::detail

namespace portals::ui {
Description<HelpView> helpView(Ref<HelpView> reference, HelpViewOptions options, ControlStyle style) {
  return detail::KeyHelpAccess::full(std::move(reference), std::move(options), style);
}

Description<KeyHints> keyHints(Ref<HelpView> help, KeyHintsOptions options, ControlStyle style) {
  return detail::KeyHelpAccess::hints(std::move(help), std::move(options), style);
}
} // namespace portals::ui
