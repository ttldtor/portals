// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/model_edit.hpp>

namespace portals {
std::expected<EditFieldKey, TreeError> EditFieldKey::create(std::string_view value) {
  if (value.empty()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    EditFieldKey result;
    result.name_ = std::make_shared<const std::string>(value);

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::string_view EditFieldKey::name() const noexcept {
  return name_ ? std::string_view(*name_) : std::string_view{};
}

bool EditFieldKey::persistent() const noexcept {
  return static_cast<bool>(name_);
}

bool EditFieldKey::operator==(const EditFieldKey &other) const noexcept {
  return name_ && other.name_ ? *name_ == *other.name_ : !name_ && !other.name_ && local_ == other.local_;
}

EditFieldKey EditFieldKey::local(std::uint64_t identity) noexcept {
  EditFieldKey value;
  value.local_ = identity;

  return value;
}

bool editReceiptMatches(const EditTarget &target, const EditReceipt &receipt) noexcept {
  return target.resource.identity.instance && target.resource.identity.epoch && target.resource.lineage &&
         target.session.instance && target.session.epoch && target.sourceSession.instance &&
         target.sourceSession.epoch && target.row.value && target.row.generation && target.field != EditFieldKey{} &&
         target.baseVersion && target.sequence && target == receipt.target;
}
} // namespace portals

#include "line_edit_source.hpp"
#include "line_edit_validation.hpp"
#include <portals/detail/model_edit.hpp>
#include <portals/ui_form_field.hpp>

namespace portals::detail {
class ModelEditTextBridge final : public LineEditSource,
                                  public LineEditValidationGate,
                                  public std::enable_shared_from_this<ModelEditTextBridge> {
public:
  explicit ModelEditTextBridge(std::shared_ptr<EditTextBinding> value) : value_(std::move(value)) {
  }

  std::shared_ptr<LineEditValidationGate> gate() override {
    return shared_from_this();
  }

  LineEditStatus failedPublication() const noexcept override {
    return value_->publicationStatus;
  }

  EditTextDraft *draft() const noexcept override {
    return value_->displayed;
  }

  void painted() noexcept override {
    // Both raw targets are retained by the session. Painting neither releases owners nor runs callbacks.
    if (value_->displayed != value_->prepared) {
      epoch_ = std::max(epoch_, value_->prepared->generation) + 1;
      value_->prepared->generation = epoch_;
    }
    value_->displayed = value_->prepared;
    value_->displayedEntry = value_->preparedEntry;
  }

  bool writable() const noexcept override {
    return !value_->closed && value_->writable && value_->displayedEntry;
  }

  std::expected<void, TreeError> admit(std::size_t bytes, bool retain) override {
    auto admitted = value_->admit(bytes, retain);
    retentionRefused_ = !admitted && admitted.error() == TreeError::NODE_LIMIT;
    return admitted;
  }

  std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> snapshot() override {
    auto ready = value_->prepare();
    return ready ? value_->snapshot() : std::unexpected(ready.error());
  }

  std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> cancelSnapshot() override {
    auto snapshot = value_->cancelSnapshot();
    retentionRefused_ = value_->retentionRefused;
    return snapshot;
  }

  std::expected<PropertyUpdate<std::string>, TreeError> set(const std::string &candidate) override {
    auto result = publish(candidate);
    if (!result) {
      return std::unexpected(result.error());
    }
    return *result ? std::expected<PropertyUpdate<std::string>, TreeError>(std::move(**result))
                   : std::unexpected(TreeError::WOULD_BLOCK);
  }

  std::expected<std::optional<PropertyUpdate<std::string>>, TreeError> publish(const std::string &candidate) override {
    auto published = value_->publish(candidate);
    retentionRefused_ = value_->retentionRefused;
    return published;
  }

  std::expected<Subscription, TreeError> observe(Node<LineEdit> receiver, std::function<void()> callback) override {
    return value_->observe(receiver, std::move(callback));
  }

  void accepted() noexcept override {
    value_->accepted();
  }

  bool attach(std::shared_ptr<LineEditValidationGate> gate) override {
    if (!gate || validator_) {
      return false;
    }
    validator_ = std::move(gate);
    return true;
  }

  std::expected<EditorValidationDecision, TreeError> check(std::string_view candidate,
                                                           EditorValidationIdentity identity) override {
    return validator_ ? validator_->check(candidate, identity) : EditorValidationDecision::ACCEPT;
  }

  void retire() noexcept override {
    epoch_ = std::max(epoch_, value_->displayed->generation);
    value_->displayed->generation = epoch_;
    if (validator_) {
      validator_->retire();
    }
  }

  void hold() noexcept override {
    retentionRefused_ = false;
    if (validator_) {
      validator_->hold();
    }
  }

  void flush(bool afterBorrow) noexcept override {
    if (validator_) {
      validator_->flush(afterBorrow);
    }
  }

  void close() noexcept override {
    if (value_->closed) {
      return;
    }
    value_->closed = true;
    if (validator_) {
      validator_->close();
    }
  }

  void finished(LineEditResult &result, bool failure) override {
    if (retentionRefused_ && result.error == TreeError::NODE_LIMIT) {
      result.status = LineEditStatus::LIMIT_EXCEEDED;
      result.recoverable = true;
      result.operation = "modelEdit.retention";
    }
    retentionRefused_ = false;
    if (validator_) {
      validator_->finished(result, failure);
    }
  }

private:
  std::uint64_t epoch_{};
  bool retentionRefused_{};
  std::shared_ptr<EditTextBinding> value_;
  std::shared_ptr<LineEditValidationGate> validator_;
};
} // namespace portals::detail

namespace portals::ui::detail {
std::expected<void, ManagedError> Access::bindEditText(ManagedScene &scene, Node<LineEdit> node,
                                                       std::shared_ptr<portals::detail::EditTextBinding> source,
                                                       LineEditConfig config) {
  auto entry = scene.bindingEntry(node);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  try {
    LineEditOptions options;
    auto read = scene.app_.tree().read(node, [&](const LineEdit &edit) {
      options = portals::detail::LineEditSourceAccess::options(edit);
    });
    if (!read) {
      return std::unexpected(scene.error(ManagedPhase::BIND, node, read.error()));
    }
    auto initialized = source->initialize(options);
    if (!initialized) {
      return std::unexpected(scene.error(ManagedPhase::BIND, node, initialized.error()));
    }
    auto bridge = std::make_shared<portals::detail::ModelEditTextBridge>(source);
    const auto lifetime = std::weak_ptr<const void>(scene.lifetime_);
    auto feedback = std::make_shared<portals::detail::EditorCommandFeedback>();
    auto editing = std::make_shared<ManagedScene::EditorEventChain>();
    auto bound = portals::detail::LineEditSourceAccess::bind(
      scene.app_.tree(), scene.app_.router(), node, bridge,
      [&scene, node, lifetime, result = std::move(config.result)](LineEditResult receipt) {
        if (lifetime.expired()) {
          return;
        }
        if (receipt.error) {
          auto failure = scene.error(ManagedPhase::INPUT, node, *receipt.error);
          failure.operation = receipt.operation;
          if (receipt.recoverable) {
            (void)scene.reportError(std::move(failure));
          } else {
            scene.remember(failure);
          }
        }
        if (lifetime.expired()) {
          return;
        }
        if (result) {
          auto delivered = result(std::move(receipt));
          if (!delivered && !lifetime.expired()) {
            scene.remember(scene.error(ManagedPhase::INPUT, node, delivered.error()));
          }
        }
      },
      std::move(config.validate),
      LineEditEvents{[&scene, node, lifetime, editing](const TextEditedEvent &event) {
                       if (!lifetime.expired()) {
                         scene.dispatchEditing(node, editing, lifetime, event);
                       }
                     },
                     [&scene, node, lifetime, editing](const EditingFinishedEvent &event) {
                       if (!lifetime.expired()) {
                         scene.dispatchEditing(node, editing, lifetime, event);
                       }
                     }},
      {}, std::move(config.clipboard));
    if (!bound) {
      return std::unexpected(scene.error(ManagedPhase::BIND, node, bound.error()));
    }
    source->complete = portals::detail::LineEditSourceAccess::completion(*bound);
    source->setText = portals::detail::LineEditSourceAccess::setter(*bound);
    bound->commandFeedback(std::move(feedback));
    (*entry)->binding.emplace<LineEditBinding>(std::move(*bound));
    (*entry)->editing = std::move(editing);
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(scene.error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(scene.error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}
} // namespace portals::ui::detail

namespace portals::ui::detail {
std::expected<Subscription, TreeError>
Access::observeEditNavigation(ManagedNavigation navigation, Node<LineEdit> receiver, std::function<void()> callback) {
  auto record = navigation.lock();
  if (!record) {
    return std::unexpected(record.error());
  }
  auto property = (*record)->current.lock();
  if (!property) {
    return std::unexpected(property.error());
  }
  return (*property)->observe(receiver, [callback = std::move(callback)](LineEdit &, auto) {
    callback();
  });
}
} // namespace portals::ui::detail

#include <portals/ui_model_edit.hpp>

namespace portals::ui::detail {
EditorCommandResult EditAccess::command(ManagedNavigation navigation, std::expected<void, TreeError> result,
                                        std::source_location origin) {
  if (result) {
    return EditorCommandResult({}, {}, false);
  }
  auto record = context(navigation);
  std::weak_ptr<const void> lifetime;
  bool reported{};
  if (record) {
    lifetime = (*record)->lifetime;
    const ManagedError failure{ManagedPhase::INPUT, (*record)->scene, result.error(), {}, {}};
    reported = (*record)->owner->reportError(failure, {}, origin).has_value();
  }
  return EditorCommandResult(result, std::move(lifetime), reported);
}
} // namespace portals::ui::detail
