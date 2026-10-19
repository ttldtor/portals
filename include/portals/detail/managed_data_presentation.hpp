// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

// Included after ManagedScene; these owners belong to the existing UI lifecycle only.
namespace portals::detail {
/// @cond INTERNAL
class DataStateIndicator;

struct DataStateOwner {
  const std::thread::id thread{std::this_thread::get_id()};
  ManagedScene *scene{};
  std::weak_ptr<const void> lifetime;
};
enum class DataStateMode {
  OWNED,
  BOUND,
  AUTOMATIC
};

struct DataStateRecord {
  std::weak_ptr<DataStateOwner> owner;
  Node<DataStateView> node;
  Node<Widget> body;
  Node<Row> strip;
  Node<DataStateIndicator> indicator;
  Node<Label> message;
  Node<Button> retryButton;
  DataStateMode mode{DataStateMode::OWNED};
  DataPresentation current;
  DataStateConfig config;
  std::array<std::shared_ptr<const GraphemeText>, 6> messages;
  std::shared_ptr<const GraphemeText> retryText;
  std::array<std::shared_ptr<const GraphemeText>, 4> frames;
  std::shared_ptr<std::function<std::expected<void, TreeError>()>> action;
  std::function<std::expected<DataRangeStatus, TreeError>()> probe;
  std::function<std::expected<void, TreeError>(DataPresentation)> display;
  Subscription timer;
  std::uint64_t timerGeneration{};
  std::uint64_t revision{};
  std::size_t frame{};
  bool timerArmed{};
  bool mounted{};
  bool presented{};
  bool retired{};
  bool eligible{};
  bool retrying{};
  bool applying{};
  bool focusDirty{};
};

/// @endcond
} // namespace portals::detail

namespace portals {
template <class Source>
std::expected<Node<DataStateView>, ManagedError> ManagedScene::beginBoundDataState(Node<Widget> parent, Source source,
                                                                                   DataStateConfig config) {
  const auto input = [&] {
    if constexpr (std::same_as<Source, ManagedSource<DataPresentation>>) {
      return source.state_;
    } else {
      return source;
    }
  }();
  auto snapshot = input.snapshot();
  if (!snapshot || input.scene_ != root_.id()) {
    return std::unexpected(
      error(ManagedPhase::BIND, parent, snapshot ? TreeError::INVALID_ARGUMENT : snapshot.error()));
  }
  auto node = beginDataState(parent, (*snapshot)->value, std::move(config));
  if (!node) {
    return node;
  }
  auto found = std::ranges::find_if(dataStates_, [&](const auto &item) {
    return item->node == *node;
  });
  if (found == dataStates_.end()) {
    return std::unexpected(error(ManagedPhase::BIND, *node, TreeError::STALE_NODE));
  }
  auto record = *found;
  record->mode = detail::DataStateMode::BOUND;
  auto bound = observe(*node, input, [weak = std::weak_ptr(record)](const DataPresentation &value) {
    auto entered = weak.lock();
    auto owner = entered ? entered->owner.lock() : nullptr;
    if (!owner || owner->lifetime.expired() || !owner->scene || entered->retired) {
      return std::expected<void, TreeError>{};
    }
    return owner->scene->applyDataState(entered, value);
  });
  if (!bound) {
    auto failure = bound.error();
    auto removed = remove(*node);
    if (!removed) {
      failure.cleanup = std::get<TreeError>(removed.error().cause);
    }
    return std::unexpected(failure);
  }
  return node;
}

template <class Content>
std::expected<void, ManagedError> ManagedScene::finishDataState(Node<Widget> root, Node<Content> body, bool automatic) {
  auto found = std::ranges::find_if(dataStates_, [&](const auto &record) {
    return record->node == root;
  });
  if (found == dataStates_.end()) {
    return std::unexpected(error(ManagedPhase::BIND, root, TreeError::STALE_NODE));
  }
  auto record = *found;
  try {
    std::function<std::expected<DataRangeStatus, TreeError>()> probe;
    if (automatic) {
      if constexpr (requires(const Content &view) {
                      { view.dataStatus() } -> std::same_as<DataRangeStatus>;
                    }) {
        auto entry = find(body);
        if (!entry || entry->viewSource.expired()) {
          return std::unexpected(error(ManagedPhase::BIND, body, TreeError::INVALID_ARGUMENT));
        }
        probe = [weak = record->owner, source = entry->viewSource,
                 body]() -> std::expected<DataRangeStatus, TreeError> {
          auto owner = weak.lock();
          auto model = source.lock();
          if (!owner || owner->lifetime.expired() || !owner->scene || !model) {
            return std::unexpected(TreeError::CLOSED);
          }
          auto failure = model->dataError();
          if (!failure) {
            return std::unexpected(failure.error());
          }
          if (*failure) {
            return std::unexpected(**failure);
          }
          std::optional<DataRangeStatus> status;
          auto read = owner->scene->app_.tree().read(body, [&](const Content &view) {
            status = view.dataStatus();
          });
          return read ? std::expected<DataRangeStatus, TreeError>(*status) : std::unexpected(read.error());
        };
      } else {
        return std::unexpected(error(ManagedPhase::BIND, body, TreeError::INVALID_ARGUMENT));
      }
    }
    // Callback materialization can allocate. Recheck both identities before publishing the relationship.
    auto live = app_.tree().withScope(root, [](const UiScope &) {
    });
    auto child = app_.tree().withScope(body, [](const UiScope &) {
    });
    if (!live || !child || record->retired || closed_) {
      return std::unexpected(error(ManagedPhase::BIND, root,
                                   !live    ? live.error()
                                   : !child ? child.error()
                                            : TreeError::CLOSED));
    }
    record->body = body;
    record->probe = std::move(probe);
    record->mounted = true;
    auto applied = applyDataState(record, record->current);
    return applied ? std::expected<void, ManagedError>{}
                   : std::unexpected(error(ManagedPhase::BIND, root, applied.error()));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, root, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, root, TreeError::CALLBACK_FAILED));
  }
}
} // namespace portals
