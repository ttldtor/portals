// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "managed_error_helpers.hpp"
#include <portals/detail/setup_failure.hpp>
#include <portals/managed.hpp>

#include <algorithm>

namespace portals {
void ManagedScene::pruneLiveText() noexcept {
  struct Pruning {
    bool &flag;
    bool previous;

    ~Pruning() {
      flag = previous;
    }
  } guard{pruningObservers_, pruningObservers_};

  pruningObservers_ = true;
  // Complete the logical mark first. Disconnecting a direct edge can release arbitrary captures.
  for (const auto &record : liveText_) {
    (void)record->origin->check();
  }
  for (const auto &record : observers_) {
    (void)observerLive(record);
  }
  for (const auto &source : models_) {
    if (source->origin && (source->origin->retired || source->origin->closed)) {
      source->retired = true;
    }
    if (source->receiver != Node<Widget>{}) {
      auto receiver = app_.tree().withScope(source->receiver, [](const UiScope &) {
      });
      if (!receiver && retired(receiver.error())) {
        source->retired = true;
      }
    }
  }
  for (const auto &record : liveText_) {
    if (!record->publicUse && !coldMountPending_ &&
        !std::ranges::any_of(observers_,
                             [&](const auto &edge) {
                               return !edge->retired && edge->inputOrigin == record->origin;
                             }) &&
        !std::ranges::any_of(models_, [&](const auto &edge) {
          return !edge->retired && edge->origin == record->origin;
        })) {
      record->origin->retired = true;
    }
  }
  pruneObservers();
  for (const auto &source : models_) {
    if (source->retired) {
      source->retire();
    }
  }
  std::erase_if(models_, [](const auto &source) {
    return source->retired;
  });
  std::erase_if(originProperties_, [&](const auto &record) {
    auto receiver = app_.tree().withScope(record.receiver, [](const UiScope &) {
    });
    if (record.origin->retired || record.origin->closed || (!receiver && retired(receiver.error()))) {
      if (auto property = record.property.lock()) {
        std::erase(properties_, property);
      }
      return true;
    }
    return record.property.expired();
  });
  std::erase_if(liveText_, [&](const auto &record) {
    if (!record->origin->retired && !record->origin->closed) {
      return false;
    }
    std::erase(properties_, std::shared_ptr<void>(record->property));
    return true;
  });
}

std::expected<bool, TreeError> ManagedScene::captureLiveText() {
  struct Capturing {
    bool &flag;
    bool previous;

    ~Capturing() {
      flag = previous;
    }
  } guard{preparing_, preparing_};

  preparing_ = true;
  pruneLiveText();
  bool changed{};
  const auto snapshot = liveText_;
  for (const auto &record : snapshot) {
    auto live = record->origin->check();
    if (!live) {
      if (record->origin->retired || record->origin->closed) {
        continue;
      }
      return std::unexpected(live.error());
    }
    auto previous = record->property->snapshot();
    if (!previous) {
      return std::unexpected(previous.error());
    }
    auto next = record->capture(&(*previous)->value);
    if (!next) {
      auto failure = next.error();
      failure.phase = ManagedPhase::PREPARE;
      failure.operation = record->operation;
      failure.registrationOrigin = record->registered;
      remember(failure);
      return std::unexpected(bridge(failure));
    }
    if (*next) {
      auto updated = record->property->set(std::move(**next));
      if (!updated) {
        remember(error(ManagedPhase::PREPARE, record->origin->node, updated.error()));
        return std::unexpected(updated.error());
      }
      changed = changed || updated->changed;
    }
  }
  return changed;
}

void ManagedScene::pruneObservers() noexcept {
  struct Pruning {
    bool &flag;
    bool previous;

    ~Pruning() {
      flag = previous;
    }
  } guard{pruningObservers_, pruningObservers_};

  pruningObservers_ = true;
  // Mark every affected edge before disconnecting owners whose destructors may reenter.
  for (const auto &record : observers_) {
    (void)observerLive(record);
  }

  std::erase_if(observers_, [&](const auto &record) {
    if (record->retired) {
      record->subscription = {};
    }
    return record->retired;
  });
}

std::expected<bool, TreeError> ManagedScene::observerLive(const std::shared_ptr<ObserverRecord> &record) {
  if (record->retired) {
    return false;
  }

  if (record->inputOrigin) {
    auto origin = record->inputOrigin->check();

    if (!origin) {
      if (record->inputOrigin->retired || record->inputOrigin->closed || record->inputOrigin->lifetime.expired()) {
        record->retired = true;

        return false;
      }

      return std::unexpected(origin.error());
    }
  }

  auto live = app_.tree().withScope(record->node, [](const UiScope &) {
  });

  if (!live && retired(live.error())) {
    record->retired = true;

    return false;
  }

  return live ? std::expected<bool, TreeError>(true) : std::unexpected(live.error());
}

std::expected<void, ManagedError> ManagedScene::observerAdmission(Node<Widget> node) {
  auto allowed = boundary(ManagedPhase::BIND, node);
  if (!allowed) {
    return allowed;
  }
  if (recoveringObservers_ || closing_) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::WOULD_BLOCK));
  }
  if (node != root_ && !find(node)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }
  pruneLiveText();
  const auto nodeCount = std::ranges::count(observers_, node, [](const auto &record) {
    return record->node;
  });
  const auto captures = std::ranges::count_if(liveText_, [&](const auto &record) {
    return record->origin->node == node;
  });
  const auto boundModels = std::ranges::count_if(models_, [](const auto &source) {
    return source->bound();
  });
  const auto rootModels = node == root_ ? boundModels : 0;
  if (observers_.size() + liveText_.size() + static_cast<std::size_t>(boundModels) >= options_.maxObservers ||
      static_cast<std::size_t>(nodeCount + captures + rootModels) >= options_.maxObserversPerNode) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }
  return {};
}

std::expected<void, TreeError> ManagedScene::invokeObserver(const std::shared_ptr<ObserverRecord> &record,
                                                            ManagedPhase phase) {
  if (closed_ || record->retired || record->running || observerMountPending() ||
      (phase == ManagedPhase::INPUT && recoveringObservers_)) {
    return {};
  }
  auto live = observerLive(record);
  if (live && !*live) {
    record->subscription = {};
    return {};
  }

  struct Invoking {
    ObserverRecord &record;
    std::size_t &count;

    ~Invoking() {
      record.running = false;
      --count;
    }
  } guard{*record, observerCallbacks_};

  record->running = true;
  ++observerCallbacks_;
  record->shortFailure.reset();
  record->shortAttempt = nullptr;
  record->promotesFailure = false;

  const auto report = [&](TreeError cause) -> std::expected<void, TreeError> {
    record->failed = true;
    auto failure = error(phase, record->node, cause);
    if (record->textFailure) {
      failure.text = *record->textFailure;
      if (failure.text && record->inputOrigin) {
        failure.node = record->inputOrigin->node.id();
      }
    }
    if (record->operation) {
      failure.operation = record->operation;
      failure.operationOrigin = record->origin;
      failure.registrationOrigin = record->origin;
    }
    remember(failure);
    return std::unexpected(cause);
  };
  const auto reportShort = [&]() -> std::expected<void, TreeError> {
    record->failed = true;
    // Same-scene short collectors promoted their exact history receipt at synchronous admission.
    // A foreign collector has its own history; retain the unchanged failure in this runner too.
    if (!failure_) {
      remember(*record->shortFailure);
    }
    app_.requestStop();
    return std::unexpected(bridge(*record->shortFailure));
  };
  try {
    if (!live) {
      return report(live.error());
    }
    auto latest = record->revision();
    if (!latest) {
      return report(latest.error());
    }
    if (!record->initialPending && record->successful == *latest) {
      record->failed = false;
      return {};
    }

    auto delivered = record->deliver(record);
    if (record->shortFailure) {
      return reportShort();
    }
    if (!delivered) {
      return report(delivered.error());
    }
    record->successful = *delivered;
    record->initialPending = false;
    record->failed = false;
    return {};
  } catch (const std::bad_alloc &) {
    return record->shortFailure ? reportShort() : report(TreeError::NO_MEMORY);
  } catch (...) {
    return record->shortFailure ? reportShort() : report(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, TreeError> ManagedScene::recoverObservers(std::size_t maxPasses, bool *invoked, bool *remaining,
                                                              bool deferFailedLive) {
  if (invoked) {
    *invoked = false;
  }
  if (remaining) {
    *remaining = false;
  }
  if (!maxPasses) {
    maxPasses = options_.maxObserverRecoveryPasses;
  }
  if (observers_.empty()) {
    return {};
  }

  struct Recovering {
    bool &flag;

    ~Recovering() {
      flag = false;
    }
  } guard{recoveringObservers_};

  recoveringObservers_ = true;

  try {
    pruneLiveText();
    if (observers_.empty()) {
      return {};
    }
    // Immutable owners survive immediate registry erase and deferred capture retirement.
    const auto snapshot = observers_;
    const auto deferred = [&](const auto &record) {
      if (!deferFailedLive || !record->failed) {
        return false;
      }
      auto origin = record->recoveryOrigin.lock();
      // Failed live computations need the next built-in sync/capture before retrying. An owning
      // derived output remains independent when its original widget or capture has retired.
      return origin && origin->check() && std::ranges::any_of(liveText_, [&](const auto &capture) {
               return capture->origin == origin;
             });
    };
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
      for (const auto &record : snapshot) {
        if (deferred(record)) {
          continue;
        }
        const auto before = record->successful;
        const bool initial = record->initialPending;
        auto result = invokeObserver(record, ManagedPhase::PREPARE);
        if (invoked) {
          *invoked = *invoked || before != record->successful || (initial && !record->initialPending);
        }
        if (!result) {
          return result;
        }
      }

      bool pending{};
      for (const auto &record : snapshot) {
        // A later callback may have removed an earlier origin through the low-level tree.
        // Retire it before the weak source revision lookup, just as INPUT admission does.
        auto live = observerLive(record);

        if (!live) {
          remember(error(ManagedPhase::PREPARE, record->node, live.error()));

          return std::unexpected(live.error());
        }

        if (!*live || deferred(record)) {
          continue;
        }
        auto current = record->revision();
        if (!current) {
          remember(error(ManagedPhase::PREPARE, record->node, current.error()));
          return std::unexpected(current.error());
        }
        pending = pending || record->initialPending || record->successful != *current;
      }
      if (!pending) {
        return {};
      }
    }
    if (remaining) {
      *remaining = true;
      return {};
    }
    remember(error(ManagedPhase::PREPARE, root_, TreeError::WOULD_BLOCK));
    return std::unexpected(TreeError::WOULD_BLOCK);
  } catch (const std::bad_alloc &) {
    remember(error(ManagedPhase::PREPARE, root_, TreeError::NO_MEMORY));
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

} // namespace portals
