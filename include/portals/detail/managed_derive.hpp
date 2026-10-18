// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

// Included after ManagedScene's definition; all records use its existing Property/Scheduler runtime.
namespace portals {
template <class T, class Callback>
void ManagedScene::prepareObserverRecord(const std::shared_ptr<ObserverRecord> &record,
                                         const std::shared_ptr<Property<T>> &source, Callback &&callback) {
  record->revision = [source = std::weak_ptr(source)]() -> std::expected<std::uint64_t, TreeError> {
    auto locked = source.lock();
    if (!locked) {
      return std::unexpected(TreeError::CLOSED);
    }
    auto snapshot = locked->snapshot();
    return snapshot ? std::expected<std::uint64_t, TreeError>((*snapshot)->revision)
                    : std::unexpected(snapshot.error());
  };
  record->deliver = ui::detail::currentSceneCallback(
    facadeContext(),
    [source = std::weak_ptr(source), scene = facadeContext(), action = std::forward<Callback>(callback)](
      const std::shared_ptr<ObserverRecord> &entered) mutable -> std::expected<std::uint64_t, TreeError> {
      auto locked = source.lock();
      if (!locked) {
        return std::unexpected(TreeError::CLOSED);
      }
      auto snapshot = locked->snapshot();
      if (!snapshot) {
        return std::unexpected(snapshot.error());
      }
      // The input and callable owners survive self/ancestor removal until this entered invocation returns.
      ui::detail::SetupFailureScope collector(entered, ObserverRecord::collectShortFailure, "managed.observe");
      ui::detail::CurrentSceneScope current(scene);
      auto status = [&]() {
        if constexpr (std::invocable<Callback &, const T &, const std::shared_ptr<ObserverRecord> &>) {
          return detail::invokeIgnoringValue(action, std::as_const((*snapshot)->value), entered);
        } else {
          return detail::invokeIgnoringValue(action, std::as_const((*snapshot)->value));
        }
      }();
      return status ? std::expected<std::uint64_t, TreeError>((*snapshot)->revision) : std::unexpected(status.error());
    });
}

template <class W, class T>
std::expected<Subscription, TreeError>
ManagedScene::connectObserverRecord(Node<W> receiver, const std::shared_ptr<Property<T>> &source,
                                    const std::shared_ptr<ObserverRecord> &record) {
  return source->observe(receiver, [this, weak = std::weak_ptr(record), lifetime = std::weak_ptr(lifetime_)](
                                     W &, std::shared_ptr<const PropertyChange<T>>) {
    auto entered = weak.lock();
    if (!lifetime.expired() && entered && !entered->initialPending) {
      (void)invokeObserver(entered, ManagedPhase::INPUT);
    }
  });
}

template <std::equality_comparable In, class Transform>
  requires detail::ManagedDerivation<Transform, In>
std::expected<ManagedSource<detail::DerivedOutput<Transform, In>>, ManagedError>
ManagedScene::derive(ManagedState<In> input, Transform &&transform, Invalidation kind, PropertyOptions options,
                     std::source_location origin) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  using Out = detail::DerivedOutput<Transform, In>;
  const auto failure = [&](TreeError cause, const char *operation = "derive.initial") {
    auto result = error(ManagedPhase::STATE, root_, cause);
    result.operation = operation;
    result.operationOrigin = origin;
    result.registrationOrigin = origin;
    return std::unexpected(result);
  };
  const auto admission = [&]() -> std::expected<void, ManagedError> {
    auto allowed = observerAdmission(root_);
    if (!allowed) {
      return failure(std::get<TreeError>(allowed.error().cause), "derive.register");
    }
    if (properties_.size() >= options_.maxStates || properties_.size() == properties_.max_size() ||
        observers_.size() == observers_.max_size()) {
      return failure(TreeError::NODE_LIMIT, "derive.register");
    }
    return {};
  };
  auto allowed = admission();
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if ((kind != Invalidation::PAINT && kind != Invalidation::LAYOUT) || !options.max_observers) {
    return failure(TreeError::INVALID_ARGUMENT, "derive.register");
  }
  auto source = input.lock();
  if (!source || input.scene_ != root_.id()) {
    return failure(source ? TreeError::INVALID_ARGUMENT : source.error(), "derive.register");
  }

  try {
    if constexpr (std::is_pointer_v<std::decay_t<Transform>> || requires { transform.operator bool(); }) {
      if (!static_cast<bool>(transform)) {
        return failure(TreeError::INVALID_ARGUMENT, "derive.register");
      }
    }
    auto record = std::make_shared<ObserverRecord>();
    record->scene = this;
    record->node = root_;
    record->runtime = false;
    record->operation = "derive.compute";
    record->origin = origin;

    // Keep one callable instance for initial and subsequent computations. Consume a stationary result:
    // moving checked MSVC strings through an intermediate expected can allocate inside noexcept moves.
    auto wrapped = ui::detail::currentSceneCallback(
      facadeContext(),
      [scene = facadeContext(),
       action = std::forward<Transform>(transform)](const In &value, const std::shared_ptr<ObserverRecord> &entered,
                                                    auto &&consume) mutable -> std::expected<void, TreeError> {
        ui::detail::SetupFailureScope collector(entered, ObserverRecord::collectShortFailure, "managed.derive");
        ui::detail::CurrentSceneScope current(scene);
        auto result = std::invoke(action, value);
        if (entered->shortFailure) {
          const auto *cause = std::get_if<TreeError>(&entered->shortFailure->cause);
          return std::unexpected(cause ? *cause : TreeError::CALLBACK_FAILED);
        }
        if constexpr (detail::IS_DERIVED_EXPECTED<decltype(result)>) {
          return result ? consume(*result) : std::unexpected(result.error());
        } else {
          return consume(result);
        }
      });
    auto callable = std::make_shared<decltype(wrapped)>(std::move(wrapped));
    allowed = admission();
    source = input.lock();
    if (!allowed) {
      return std::unexpected(allowed.error());
    }
    if (!source) {
      return failure(source.error(), "derive.register");
    }
    auto initial = (*source)->snapshot();
    if (!initial) {
      return failure(initial.error());
    }

    std::shared_ptr<Property<Out>> output;
    std::expected<void, TreeError> computed;
    {
      struct Computing {
        std::size_t &count;

        explicit Computing(std::size_t &active) : count(active) {
          ++count;
        }

        ~Computing() {
          --count;
        }
      } guard{observerCallbacks_};

      try {
        computed =
          (*callable)(std::as_const((*initial)->value), record, [&](Out &value) -> std::expected<void, TreeError> {
            auto created = [&] {
              if constexpr (std::same_as<Out, std::string>) {
                return Property<Out>::create(app_.tree(), root_, std::as_const(value), kind, options);
              } else {
                return Property<Out>::create(app_.tree(), root_, std::move(value), kind, options);
              }
            }();
            if (!created) {
              return std::unexpected(created.error());
            }
            output = std::shared_ptr<Property<Out>>(std::move(*created));
            return {};
          });
      } catch (const std::bad_alloc &) {
        computed = std::unexpected(TreeError::NO_MEMORY);
      } catch (...) {
        computed = std::unexpected(TreeError::CALLBACK_FAILED);
      }
    }
    if (record->shortFailure) {
      return std::unexpected(*record->shortFailure);
    }
    if (!computed) {
      return failure(computed.error());
    }

    prepareObserverRecord(
      record, *source,
      [callable, output = std::weak_ptr(output)](
        const In &value, const std::shared_ptr<ObserverRecord> &entered) -> std::expected<void, TreeError> {
        auto locked = output.lock();
        if (!locked) {
          return std::unexpected(TreeError::CLOSED);
        }
        return (*callable)(value, entered, [&](Out &candidate) -> std::expected<void, TreeError> {
          auto changed = [&] {
            if constexpr (std::same_as<Out, std::string>) {
              return locked->set(std::as_const(candidate));
            } else {
              return locked->set(std::move(candidate));
            }
          }();
          // Notification rejection is postcommit; downstream records recover their current snapshots.
          return changed ? std::expected<void, TreeError>{} : std::unexpected(changed.error());
        });
      });

    allowed = admission();
    source = input.lock();
    if (!allowed) {
      return std::unexpected(allowed.error());
    }
    if (!source) {
      return failure(source.error(), "derive.register");
    }
    auto current = (*source)->snapshot();
    if (!current) {
      return failure(current.error());
    }
    if ((*current)->revision != (*initial)->revision) {
      return failure(TreeError::WOULD_BLOCK);
    }
    properties_.reserve(properties_.size() + 1);
    observers_.reserve(observers_.size() + 1);
    auto token = connectObserverRecord(root_, *source, record);
    if (!token) {
      return failure(token.error(), "derive.register");
    }
    auto dirty = app_.tree().invalidate(root_, Invalidation::PAINT);
    if (!dirty) {
      return failure(dirty.error(), "derive.register");
    }

    ManagedState<Out> handle;
    handle.model_ = output;
    handle.facade_ = facade_;
    handle.lifetime_ = lifetime_;
    handle.owner_ = owner_;
    handle.scene_ = root_.id();
    record->subscription = std::move(*token);
    record->successful = (*initial)->revision;
    record->initialPending = false;
    record->runtime = true;
    properties_.push_back(std::move(output));
    observers_.push_back(std::move(record));
    return ManagedSource<Out>(std::move(handle));
  } catch (const std::bad_alloc &) {
    return failure(TreeError::NO_MEMORY);
  } catch (...) {
    return failure(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals
