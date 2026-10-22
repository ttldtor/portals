// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"
#include "message_box_state.hpp"
#include <portals/message_box.hpp>

#include <algorithm>
#include <limits>

namespace portals {
std::shared_ptr<detail::MessageRecord> ManagedScene::findMessage(Node<Widget> node) const {
  for (const auto &record : messages_) {
    if (record->dialog->root == node) {
      return record;
    }
  }
  return {};
}

std::expected<void, TreeError> ManagedScene::chooseMessage(const std::shared_ptr<detail::MessageRecord> &record,
                                                           MessageChoice choice) {
  if (app_.router().state_->dialogEnterRepeat) {
    return {};
  }

  if (!record || !record->current || record->current->done || record->current->result || record->current->starting) {
    return std::unexpected(TreeError::CLOSED);
  }

  auto request = record->current;

  if ((request->async && !detail::asyncInvocationCurrent(request->invocation)) || !messageParent(request->owner)) {
    return std::unexpected(TreeError::CLOSED);
  }

  request->choosing = choice;

  auto dismissed = dialogCommand(record->dialog->root, false, DialogDismissReason::PROGRAMMATIC);

  request->choosing.reset();

  if (!dismissed && !focusReported(dismissed.error())) {
    reportShortError(dismissed.error(), "MessageBox.choose", {}, "Dialog", std::source_location::current(),
                     actionAdmitted_);
  }

  return {};
}

void ManagedScene::retireMessages() noexcept {
  if (collectingMessages_) {
    return;
  }

  struct Collecting {
    bool &flag;

    ~Collecting() {
      flag = false;
    }
  } collecting{collectingMessages_};

  collectingMessages_ = true;
  for (const auto &request : messageRequests_) {
    if (request->reserved && request->released) {
      request->done = true;
    }
    if (closed_) {
      request->done = true;
      request->connection.subscription = {};
    }
  }
  for (const auto &request : messageRequests_) {
    if (!request->done || request->admitted) {
      continue;
    }
    if (auto record = request->record.lock(); record && record->current == request) {
      record->current.reset();
    }
    request->connection.subscription = {};
    request->callback = {};
  }
  std::erase_if(messageRequests_, [](const auto &request) {
    return request->done && !request->admitted;
  });
  std::erase_if(messages_, [&](const auto &record) {
    return closed_ || !app_.tree().inputParent(record->dialog->root);
  });
}

void ManagedScene::completeMessage(const std::shared_ptr<detail::MessageRecord> &record,
                                   const std::expected<void, ManagedError> &result, bool opening, bool committed) {
  if (!record || !record->current) {
    return;
  }
  const auto request = record->current;
  if (request->result || request->done) {
    return;
  }
  if (!committed || (opening && result)) {
    return;
  }
  // This is the single domain latch, after all P22 focus and dismissal callbacks returned.
  // The holder was adopted before publication; the following operations do not allocate or call users.
  request->ownerError = !app_.tree().inputParent(request->owner);
  request->dialogError = !app_.tree().inputParent(request->dialog);
  if (!result) {
    request->latch(std::unexpected(result.error()));
  } else if (closed_ || !app_.tree().inputParent(request->dialog)) {
    request->dialogError = true;
    request->latch(std::unexpected(
      error(ManagedPhase::CLEANUP, request->dialog, closed_ ? TreeError::CLOSED : TreeError::STALE_NODE)));
  } else if (!app_.tree().inputParent(request->owner)) {
    request->ownerError = true;
    request->latch(std::unexpected(error(ManagedPhase::CLEANUP, request->owner, TreeError::CLOSED)));
  } else if (request->async && !detail::asyncInvocationCurrent(request->invocation)) {
    request->latch(std::unexpected(error(ManagedPhase::CLEANUP, request->owner, TreeError::CLOSED)));
    request->done = true;
  } else {
    request->latch(request->choosing.value_or(MessageChoice::CANCEL));
  }
}

void ManagedScene::deliverMessage(const std::shared_ptr<detail::MessageRequest> &request) {
  auto record = request->record.lock();
  if (closed_ || request->done || !record || record->current != request || !request->result ||
      !app_.tree().inputParent(request->owner) || !app_.tree().inputParent(request->dialog)) {
    request->done = true;
    return;
  }
  request->admitted = true;
  const auto lifetime = std::weak_ptr(lifetime_);
  auto callback = std::move(request->callback);
  const auto result = request->result;
  try {
    const auto delivered = callback(*result);
    if (!delivered && !lifetime.expired() && !commandReported(delivered, lifetime)) {
      remember(error(ManagedPhase::INPUT, request->dialog, delivered.result_.error()), request->origin);
    }
  } catch (const std::bad_alloc &) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, request->dialog, TreeError::NO_MEMORY), request->origin);
    }
  } catch (...) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, request->dialog, TreeError::CALLBACK_FAILED), request->origin);
    }
  }
  request->admitted = false;
  request->done = true;
  request->connection.subscription = {};
}

std::expected<void, TreeError> ManagedScene::prepareMessages() {
  retireMessages();
  // A local owning snapshot is unnecessary: request adoption during callbacks can reallocate
  // the vector, so index only the original prefix and pin each request before any reentry.
  const auto count = messageRequests_.size();
  for (std::size_t i = 0; i < count && i < messageRequests_.size(); ++i) {
    auto request = messageRequests_[i];
    auto record = request->record.lock();
    if (request->done || request->reserved) {
      continue;
    }
    const bool dialogLive = static_cast<bool>(app_.tree().inputParent(request->dialog));
    const bool ownerLive = static_cast<bool>(app_.tree().inputParent(request->owner));
    const bool taskLive = !request->async || detail::asyncInvocationCurrent(request->invocation);
    const auto revision = app_.tree().inputRevision(request->dialog);
    const bool presentationRevoked =
      !request->result && !request->starting && record && dialogLive &&
      (!record->dialog->open || !revision || *revision != record->dialog->activationRevision);
    if (presentationRevoked) {
      (void)app_.router().modalRoot();
    }
    if (!dialogLive || !ownerLive || !taskLive || request->released || presentationRevoked) {
      if (!request->result) {
        request->ownerError = !ownerLive;
        request->dialogError = !dialogLive;
        request->latch(std::unexpected(error(ManagedPhase::CLEANUP,
                                             !dialogLive || presentationRevoked ? request->dialog : request->owner,
                                             !dialogLive ? TreeError::STALE_NODE : TreeError::CLOSED)));
      } else if ((!ownerLive && !request->ownerError) || (!dialogLive && !request->dialogError) || !taskLive) {
        request->done = true;
      }
      if (!request->async || !taskLive || request->released) {
        request->done = true;
      }
      if (record && record->current == request && record->dialog->open && dialogLive) {
        auto dismissed = dialogCommand(request->dialog, false, DialogDismissReason::PROGRAMMATIC);
        if (!dismissed) {
          if (!focusReported(dismissed.error())) {
            remember(dismissed.error(), request->origin);
          }
          const auto *cause = std::get_if<TreeError>(&dismissed.error().cause);
          return std::unexpected(cause ? *cause : TreeError::CALLBACK_FAILED);
        }
      }
    }
    if (request->done || !request->result || request->queued) {
      continue;
    }
    if (request->async) {
      auto published = request->reply.complete(request->result);
      if (published) {
        request->queued = true;
        continue;
      }
      if (published.error() == AsyncError::WOULD_BLOCK) {
        continue;
      }
      if (published.error() == AsyncError::CLOSED) {
        request->done = true;
        continue;
      }
      auto failure =
        error(ManagedPhase::INPUT, request->dialog,
              published.error() == AsyncError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED);
      failure.operation = "MessageBox.deliver";
      if (!request->transportReported) {
        request->transportReported = true;
        remember(failure, request->origin);
      }
      return std::unexpected(std::get<TreeError>(failure.cause));
    }
    auto published = request->connection.publisher.publish(request->result);
    if (published) {
      request->queued = true;
      continue;
    }
    if (published.error() == SchedulerError::WOULD_BLOCK) {
      continue;
    }
    if (published.error() == SchedulerError::CLOSED) {
      request->done = true;
      continue;
    }
    ManagedError failure{ManagedPhase::INPUT,
                         request->dialog.id(),
                         ApplicationError{ApplicationPhase::DISPATCH, published.error(), {}},
                         {},
                         {}};
    failure.operation = "MessageBox.deliver";
    if (!request->transportReported) {
      request->transportReported = true;
      remember(failure, request->origin);
    }
    return std::unexpected(published.error() == SchedulerError::NO_MEMORY ? TreeError::NO_MEMORY
                                                                          : TreeError::CALLBACK_FAILED);
  }
  retireMessages();
  return {};
}
} // namespace portals

namespace portals::ui::detail {
std::expected<void, ManagedError> MessageAccess::validate(ManagedScene &scene, Node<Widget> dialog,
                                                          Node<Widget> owner) {
  auto allowed = scene.boundary(ManagedPhase::INPUT, dialog);
  if (!allowed) {
    return allowed;
  }
  auto live = scene.messageParent(dialog);
  if (!live) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, live.error()));
  }
  auto record = scene.findMessage(dialog);
  if (!record || !record->dialog->ready) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::CLOSED));
  }
  auto ownerLive = scene.messageParent(owner);
  if (!ownerLive) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, owner, ownerLive.error()));
  }
  if (owner != scene.root_ && !scene.find(owner)) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, owner, TreeError::INVALID_ARGUMENT));
  }
  if (!scene.messageOwner(owner)) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, owner, TreeError::INVALID_ARGUMENT));
  }
  return {};
}

std::expected<std::shared_ptr<portals::detail::MessageRequest>, ManagedError>
MessageAccess::reserve(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner) {
  if (scene.collectingMessages_) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WOULD_BLOCK));
  }
  scene.retireMessages();
  auto valid = validate(scene, dialog, owner);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  auto record = scene.findMessage(dialog);
  if (record->current && !record->current->done) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WOULD_BLOCK));
  }
  if (scene.messageRequests_.size() >= scene.options_.maxMessageRequests) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::NODE_LIMIT));
  }
  if (record->nextId == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::IDENTITY_EXHAUSTED));
  }
  auto request = std::make_shared<portals::detail::MessageRequest>();
  request->holder = std::make_shared<portals::detail::MessageResultHolder>();
  request->record = record;
  request->dialog = dialog;
  request->owner = owner;
  request->id = record->nextId + 1;
  scene.messageRequests_.reserve(scene.messageRequests_.size() + 1);
  return request;
}

std::expected<void, ManagedError>
MessageAccess::start(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner,
                     std::function<EditorCommandResult(const MessageResult &)> callback, std::source_location origin) {
  if (!callback) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::INVALID_ARGUMENT));
  }
  auto reserved = reserve(scene, dialog, owner);
  if (!reserved) {
    return std::unexpected(reserved.error());
  }
  auto request = *reserved;
  request->callback.swap(callback);
  request->origin = origin;
  std::expected<SchedulerConnection<MessageResult>, SchedulerError> connection =
    std::unexpected(SchedulerError::CLOSED);
  auto scoped = scene.app_.tree().withScope(owner, [&](const UiScope &scope) {
    connection = scene.app_.scheduler().connect<MessageResult>(
      scope, [&scene, weak = std::weak_ptr(request),
              lifetime = std::weak_ptr(scene.lifetime_)](std::shared_ptr<const MessageResult>) {
        if (!lifetime.expired()) {
          if (auto live = weak.lock()) {
            scene.deliverMessage(live);
          }
        }
      });
  });
  if (!scoped) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, owner, scoped.error()));
  }
  if (!connection) {
    return std::unexpected(ManagedError{
      ManagedPhase::INPUT, owner.id(), ApplicationError{ApplicationPhase::DISPATCH, connection.error(), {}}, {}, {}});
  }
  request->connection = std::move(*connection);
  auto valid = validate(scene, dialog, owner);
  if (!valid) {
    return valid;
  }
  auto record = scene.findMessage(dialog);
  if ((record->current && !record->current->done) || record->nextId + 1 != request->id ||
      scene.messageRequests_.size() >= scene.options_.maxMessageRequests ||
      scene.messageRequests_.size() == scene.messageRequests_.capacity()) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WOULD_BLOCK));
  }
  auto previous = record->last;
  request->starting = true;
  record->nextId = request->id;
  record->current = request;
  record->last.reset();
  scene.messageRequests_.push_back(request);
  auto opened = scene.dialogCommand(dialog, true, DialogDismissReason::PROGRAMMATIC);
  request->starting = false;
  if (!opened && !request->result) {
    request->done = true;
    record->last = std::move(previous);
  }
  return opened;
}

std::expected<void, ManagedError> MessageAccess::cancel(ManagedScene &scene, Node<Widget> dialog) {
  auto allowed = scene.boundary(ManagedPhase::INPUT, dialog);
  if (!allowed) {
    return allowed;
  }
  auto live = scene.messageParent(dialog);
  if (!live) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, live.error()));
  }
  auto record = scene.findMessage(dialog);
  if (!record) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::CLOSED));
  }
  if (record->current && (record->current->reserved || record->current->starting)) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WOULD_BLOCK));
  }
  if (!record->dialog->open) {
    return {};
  }
  return scene.dialogCommand(dialog, false, DialogDismissReason::PROGRAMMATIC);
}

std::expected<std::shared_ptr<const MessageResult>, ManagedError> MessageAccess::last(ManagedScene &scene,
                                                                                      Node<Widget> dialog) {
  // Snapshot reads need only exact identity/lifetime checks, never a Debug borrow allocation.
  if (scene.owner_ != std::this_thread::get_id()) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WRONG_THREAD));
  }
  if (scene.closed_) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::CLOSED));
  }
  auto live = scene.messageParent(dialog);
  if (!live) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, live.error()));
  }
  auto record = scene.findMessage(dialog);
  if (!record) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::CLOSED));
  }
  return record->last ? std::expected<std::shared_ptr<const MessageResult>, ManagedError>(record->last)
                      : std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WOULD_BLOCK));
}

std::expected<AsyncReceive<MessageResult>, ManagedError>
MessageAccess::receive(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner, const UiAsyncContext &context) {
  try {
    auto state = context.control_.lock();
    auto validContext = portals::detail::asyncCheck(state);
    if (!validContext) {
      TreeError cause = TreeError::CLOSED;
      switch (validContext.error()) {
      case AsyncError::WRONG_THREAD:
        cause = TreeError::WRONG_THREAD;
        break;
      case AsyncError::NO_MEMORY:
        cause = TreeError::NO_MEMORY;
        break;
      case AsyncError::WOULD_BLOCK:
        cause = TreeError::WOULD_BLOCK;
        break;
      case AsyncError::INVALID_ARGUMENT:
        cause = TreeError::INVALID_ARGUMENT;
        break;
      case AsyncError::IDENTITY_EXHAUSTED:
        cause = TreeError::IDENTITY_EXHAUSTED;
        break;
      case AsyncError::CALLBACK_FAILED:
        cause = TreeError::CALLBACK_FAILED;
        break;
      case AsyncError::CANCELLED:
      case AsyncError::CLOSED:
        break;
      }
      return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, cause));
    }
    if (&portals::detail::asyncTree(state) != &scene.app_.tree() ||
        &portals::detail::asyncScheduler(state) != &scene.app_.scheduler() ||
        !scene.messageWithin(scene.root_, portals::detail::asyncNode(state))) {
      return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WRONG_SESSION));
    }
    auto reserved = reserve(scene, dialog, owner);
    if (!reserved) {
      return std::unexpected(reserved.error());
    }
    auto request = *reserved;
    auto lease = std::make_shared<portals::detail::MessageLease>();
    lease->request = request;
    lease->wake = portals::detail::asyncInvocation(context.control_).wake;
    request->lease = lease;
    request->async = true;
    request->reserved = true;
    request->context = context.control_;
    auto awaiter = context.receive<MessageResult>(
      [request, lease, &scene, lifetime = std::weak_ptr(scene.lifetime_)](AsyncReply<MessageResult> reply) {
        // Copies share one reservation, not a restartable provider. Refuse a second start
        // through AsyncReceive's existing provider-exception boundary before changing identity.
        if (!request->reserved) {
          throw std::bad_function_call();
        }
        request->reply = reply;
        request->reserved = false;
        request->invocation = portals::detail::asyncInvocation(request->context);
        if (lifetime.expired()) {
          request->done = true;
          return;
        }
        auto record = request->record.lock();
        if (!scene.messageParent(request->dialog)) {
          request->dialogError = true;
          request->ownerError = !scene.messageParent(request->owner);
          request->latch(std::unexpected(scene.error(ManagedPhase::CLEANUP, request->dialog, TreeError::STALE_NODE)));
          return;
        }
        if (!scene.messageParent(request->owner)) {
          request->ownerError = true;
          request->latch(std::unexpected(scene.error(ManagedPhase::CLEANUP, request->owner, TreeError::CLOSED)));
          return;
        }
        auto valid = validate(scene, request->dialog, request->owner);
        if (!valid || !record || record->current != request) {
          request->latch(std::unexpected(valid ? scene.error(ManagedPhase::CLEANUP, request->dialog, TreeError::CLOSED)
                                               : valid.error()));
          return;
        }
        request->starting = true;
        auto opened = scene.dialogCommand(request->dialog, true, DialogDismissReason::PROGRAMMATIC);
        request->starting = false;
        if (!opened && !request->result) {
          request->latch(std::unexpected(opened.error()));
        }
      });
    // A queued generic completion is scoped to the receiving context, which may outlive
    // this message's owner/dialog. Recheck those identities at actual delivery admission.
    awaiter.admission_ = {
      scene.lifetime_, request, &scene, [](const void *context, const std::shared_ptr<const void> &value) noexcept {
        const auto &scene = *static_cast<const ManagedScene *>(context);
        const auto &request = *std::static_pointer_cast<const portals::detail::MessageRequest>(value);
        return !scene.closed_ && !request.done && (request.ownerError || scene.messageParent(request.owner)) &&
               (request.dialogError || scene.messageParent(request.dialog));
      }};
    auto valid = validate(scene, dialog, owner);
    if (!valid) {
      return std::unexpected(valid.error());
    }
    auto record = scene.findMessage(dialog);
    if ((record->current && !record->current->done) || record->nextId + 1 != request->id ||
        scene.messageRequests_.size() >= scene.options_.maxMessageRequests ||
        scene.messageRequests_.size() == scene.messageRequests_.capacity()) {
      return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::WOULD_BLOCK));
    }
    record->nextId = request->id;
    record->current = request;
    record->last.reset();
    scene.messageRequests_.push_back(request);
    return awaiter;
  } catch (const std::bad_alloc &) {
    return std::unexpected(scene.error(ManagedPhase::INPUT, dialog, TreeError::NO_MEMORY));
  }
}
} // namespace portals::ui::detail
