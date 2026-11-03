// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

namespace portals {
namespace {
ManagedError numericFailure(TreeError cause, NodeId node = {}) {
  return {ManagedPhase::STATE, node, cause, {}, {}};
}

ManagedError numericFailure(NumericError cause, NodeId node = {}) {
  auto failure = numericFailure(TreeError::INVALID_ARGUMENT, node);

  failure.numeric = cause;

  return failure;
}
} // namespace

std::expected<std::shared_ptr<const PropertyChange<NumericValue>>, ManagedError> NumericState::snapshot() const {
  const auto model = state_;
  auto current = model.snapshot();

  return current ? std::expected<std::shared_ptr<const PropertyChange<NumericValue>>, ManagedError>{*current}
                 : std::unexpected(numericFailure(current.error()));
}

std::expected<PropertyUpdate<NumericValue>, ManagedError> NumericState::set(double candidate) const {
  const auto model = state_;
  auto current = model.snapshot();

  if (!current) {
    return std::unexpected(numericFailure(current.error()));
  }

  auto value = (*current)->value.withValue(candidate);

  if (!value) {
    return std::unexpected(numericFailure(value.error()));
  }

  auto updated = model.set(*value);

  return updated ? std::expected<PropertyUpdate<NumericValue>, ManagedError>{*updated}
                 : std::unexpected(numericFailure(updated.error()));
}

std::expected<PropertyUpdate<NumericValue>, ManagedError> NumericState::configure(double initial,
                                                                                  NumericRange range) const {
  const auto model = state_;
  auto current = model.snapshot();

  if (!current) {
    return std::unexpected(numericFailure(current.error()));
  }

  auto value = NumericValue::create(initial, range);

  if (!value) {
    return std::unexpected(numericFailure(value.error()));
  }

  auto updated = model.set(*value);

  return updated ? std::expected<PropertyUpdate<NumericValue>, ManagedError>{*updated}
                 : std::unexpected(numericFailure(updated.error()));
}

std::expected<NumericState, ManagedError> ManagedScene::numericState(double initial, NumericRange range) {
  auto allowed = boundary(ManagedPhase::STATE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto value = NumericValue::create(initial, range);

  if (!value) {
    return std::unexpected(numericFailure(value.error(), root_.id()));
  }

  auto created = state(*value);

  return created ? std::expected<NumericState, ManagedError>{NumericState(*created)} : std::unexpected(created.error());
}

namespace detail {
struct ManagedSliderRecord {
  struct Change {
    std::function<EditorCommandResult(const PropertyUpdate<NumericValue> &)> action;
    std::source_location origin;
  };

  struct Finished {
    std::function<EditorCommandResult(const SliderOperationEvent &)> action;
    std::source_location origin;
  };

  NumericState model;
  std::vector<std::shared_ptr<Change>> changes = std::vector<std::shared_ptr<Change>>(0);
  std::vector<std::shared_ptr<Finished>> finished = std::vector<std::shared_ptr<Finished>>(0);
  bool retired{};
};
} // namespace detail

std::expected<std::shared_ptr<detail::ManagedSliderRecord>, ManagedError>
ManagedScene::sliderRecord(Node<Slider> node, ManagedPhase phase) {
  auto allowed = boundary(phase, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto entry = find(node);
  auto *binding = entry ? std::get_if<SliderBinding>(&entry->binding) : nullptr;
  auto record = binding ? std::static_pointer_cast<detail::ManagedSliderRecord>(binding->retained()) : nullptr;

  if (!record || record->retired) {
    return std::unexpected(error(phase, node, TreeError::INVALID_ARGUMENT));
  }

  return record;
}

std::expected<double, ManagedError> ManagedScene::sliderValue(Node<Slider> node) {
  auto record = sliderRecord(node, ManagedPhase::INPUT);

  if (!record) {
    return std::unexpected(record.error());
  }

  auto current = (*record)->model.snapshot();

  return current ? std::expected<double, ManagedError>{(*current)->value.value()} : std::unexpected(current.error());
}

std::expected<PropertyUpdate<NumericValue>, ManagedError> ManagedScene::setSliderValue(Node<Slider> node,
                                                                                       double value) {
  auto record = sliderRecord(node, ManagedPhase::INPUT);

  return record ? (*record)->model.set(value) : std::unexpected(record.error());
}

template <class Event>
void ManagedScene::dispatchNumeric(Node<Slider> node, const std::shared_ptr<detail::ManagedSliderRecord> &record,
                                   const std::weak_ptr<const void> &lifetime, const Event &event, std::size_t count) {
  const auto &handlers = [&]() -> const auto & {
    if constexpr (std::same_as<Event, SliderOperationEvent>) {
      return record->finished;
    } else {
      return record->changes;
    }
  }();
  auto origin = std::source_location::current();

  try {
    for (std::size_t index = 0; index < count; ++index) {
      if (lifetime.expired() || record->retired) {
        return;
      }

      if (closed_ || !app_.tree().inspect(node)) {
        return;
      }

      const auto handler = handlers[index];

      origin = handler->origin;
      const auto result = handler->action(event);

      if (!result) {
        if (!lifetime.expired() && !commandReported(result, lifetime)) {
          remember(error(ManagedPhase::INPUT, node, result.result_.error()), origin);
        }

        return;
      }
    }
  } catch (const std::bad_alloc &) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY), origin);
    }
  } catch (...) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED), origin);
    }
  }
}

std::expected<void, ManagedError>
ManagedScene::bind(Node<Slider> node, NumericState model,
                   std::function<std::expected<void, TreeError>(SliderResult)> changed) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(changed) incoming;

  incoming.swap(changed);
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  auto entry = bindingEntry(node);

  if (!entry) {
    return std::unexpected(entry.error());
  }

  auto property = model.state_.lock();

  if (!property || model.state_.scene_ != root_.id()) {
    return std::unexpected(error(ManagedPhase::BIND, node, property ? TreeError::INVALID_ARGUMENT : property.error()));
  }

  try {
    auto record = std::make_shared<detail::ManagedSliderRecord>();

    record->model = model;
    auto legacy = ui::detail::currentSceneCallback(facadeContext(), std::move(incoming));
    auto bound = bindSlider(
      app_.tree(), app_.router(), node, *property,
      [this, node, record, lifetime, legacy = std::move(legacy)](SliderResult receipt) {
        if (lifetime.expired() || record->retired) {
          return;
        }

        const auto original = receipt;
        const auto count = record->changes.size();

        try {
          if (!receipt) {
            remember(error(ManagedPhase::INPUT, node, receipt.error()));
          }

          if (lifetime.expired() || record->retired) {
            return;
          }

          if (legacy) {
            auto result = legacy(std::move(receipt));

            if (lifetime.expired() || record->retired) {
              return;
            }

            if (!result) {
              remember(error(ManagedPhase::INPUT, node, result.error()));

              return;
            }
          }

          if (original && original->changed) {
            dispatchNumeric(node, record, lifetime, *original, count);
          }
        } catch (const std::bad_alloc &) {
          if (!lifetime.expired()) {
            remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY));
          }
        } catch (...) {
          if (!lifetime.expired()) {
            remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED));
          }
        }
      },
      [this, node, record, lifetime](const SliderOperationEvent &event) {
        if (!lifetime.expired() && !record->retired) {
          dispatchNumeric(node, record, lifetime, event, record->finished.size());
        }
      });

    if (lifetime.expired()) {
      return std::unexpected(numericFailure(TreeError::CLOSED, node.id()));
    }

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    bound->retain(record);
    (*entry)->binding.emplace<SliderBinding>(std::move(*bound));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(numericFailure(TreeError::NO_MEMORY, node.id()));
  } catch (...) {
    return std::unexpected(numericFailure(TreeError::CALLBACK_FAILED, node.id()));
  }
}

void ManagedScene::retireNumeric(Entry &entry) noexcept {
  auto *binding = std::get_if<SliderBinding>(&entry.binding);
  auto record = binding ? std::static_pointer_cast<detail::ManagedSliderRecord>(binding->retained()) : nullptr;

  if (!record || record->retired) {
    return;
  }

  record->retired = true;
  changeHandlers_ -= record->changes.size();
  editingHandlers_ -= record->finished.size();
  record->changes.clear();
  record->finished.clear();
}

std::expected<void, ManagedError>
ManagedScene::registerNumericChange(Node<Slider> node,
                                    std::function<EditorCommandResult(const PropertyUpdate<NumericValue> &)> action,
                                    std::source_location origin) {
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  auto record = sliderRecord(node, ManagedPhase::BIND);

  if (!record) {
    return std::unexpected(record.error());
  }

  if (!action) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto handler = std::make_shared<detail::ManagedSliderRecord::Change>(std::move(action), origin);

    if (lifetime.expired()) {
      return std::unexpected(numericFailure(TreeError::CLOSED, node.id()));
    }

    if ((*record)->retired || changeHandlers_ >= options_.maxChangeHandlers ||
        (*record)->changes.size() >= options_.maxChangeHandlersPerNode) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
    }

    (*record)->changes.push_back(std::move(handler));
    ++changeHandlers_;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(numericFailure(TreeError::NO_MEMORY, node.id()));
  }
}

std::expected<void, ManagedError>
ManagedScene::registerNumericFinished(Node<Slider> node,
                                      std::function<EditorCommandResult(const SliderOperationEvent &)> action,
                                      std::source_location origin) {
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  auto record = sliderRecord(node, ManagedPhase::BIND);

  if (!record) {
    return std::unexpected(record.error());
  }

  if (!action) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto handler = std::make_shared<detail::ManagedSliderRecord::Finished>(std::move(action), origin);

    if (lifetime.expired()) {
      return std::unexpected(numericFailure(TreeError::CLOSED, node.id()));
    }

    if ((*record)->retired || editingHandlers_ >= options_.maxEditingHandlers ||
        (*record)->finished.size() >= options_.maxEditingHandlersPerNode) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
    }

    (*record)->finished.push_back(std::move(handler));
    ++editingHandlers_;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(numericFailure(TreeError::NO_MEMORY, node.id()));
  }
}
} // namespace portals
