// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "form_field_async.hpp"

#include <portals/background.hpp>
#include <portals/ui_scene.hpp>

namespace portals::ui::detail {
namespace {
using portals::detail::EditorValidationDecision;
using portals::detail::EditorValidationIdentity;

struct ValidationReply {
  EditorValidationIdentity identity;
  FieldValidation value;
  std::optional<TreeError> error;
  std::optional<AsyncError> async;
  std::optional<BackgroundError> background;
  std::optional<TextError> text;
  const char *operation{"formField.validateAsync"};
};

AsyncError publicationError(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::CLOSED:
    return AsyncError::CLOSED;
  default:
    return AsyncError::INVALID_ARGUMENT;
  }
}

TreeError backgroundCause(BackgroundError error) noexcept {
  switch (error) {
  case BackgroundError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case BackgroundError::CLOSED:
  case BackgroundError::UNAVAILABLE:
    return TreeError::CLOSED;
  case BackgroundError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  case BackgroundError::START_FAILED:
    return TreeError::CALLBACK_FAILED;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}

class ValidationJob final : public BackgroundJob {
public:
  ValidationJob(std::string candidate, AsyncFieldValidator work, std::shared_ptr<ValidationReply> reply,
                Publisher<ValidationReply> publisher, std::size_t maxMessage)
      : candidate_(std::move(candidate)), work_(std::move(work)), reply_(std::move(reply)),
        publisher_(std::move(publisher)), maxMessage_(maxMessage) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      auto value = work_(candidate_, stop);

      if (!value) {
        reply_->error = value.error();
      } else if (value->status != FieldStatus::VALID && value->status != FieldStatus::INVALID) {
        reply_->error = TreeError::INVALID_ARGUMENT;
      } else if (value->message.size() > maxMessage_) {
        reply_->error = TreeError::INVALID_ARGUMENT;
        reply_->text = TextError{TextErrorKind::INPUT_LIMIT, 0};
        reply_->operation = "formField.validateAsync.result";
      } else {
        auto checked = GraphemeText::from(value->message);

        if (!checked) {
          reply_->error = TreeError::INVALID_ARGUMENT;
          reply_->text = checked.error();
          reply_->operation = "formField.validateAsync.result";
        } else {
          reply_->value = std::move(*value);
        }
      }
    } catch (const std::bad_alloc &) {
      reply_->error = TreeError::NO_MEMORY;
      reply_->async = AsyncError::NO_MEMORY;
    } catch (...) {
      reply_->error = TreeError::CALLBACK_FAILED;
      reply_->async = AsyncError::CALLBACK_FAILED;
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto sent = publisher_.publish(reply_);

    return sent ? std::expected<void, AsyncError>{} : std::unexpected(publicationError(sent.error()));
  }

private:
  std::string candidate_;
  AsyncFieldValidator work_;
  std::shared_ptr<ValidationReply> reply_;
  Publisher<ValidationReply> publisher_;
  std::size_t maxMessage_;
};

class FieldAsync final : public portals::detail::LineEditValidationGate,
                         public SceneActivity,
                         public std::enable_shared_from_this<FieldAsync> {
public:
  FieldAsync(ManagedScene &scene, Application &app, Node<FormField> field, Node<Widget> editor,
             AsyncFieldValidator work, FieldValidationLimits limits,
             std::function<void(std::shared_ptr<const FieldValidation>)> stage)
      : scene_(scene), app_(app), field_(field), editor_(editor), work_(std::move(work)), limits_(limits),
        stage_(std::move(stage)) {
  }

  ~FieldAsync() override {
    if (background_) {
      background_->cancel();
    }
  }

  std::expected<void, TreeError> connect() {
    std::expected<void, TreeError> result;
    auto admitted = app_.tree().withScope(field_, [&](const UiScope &scope) {
      auto connected = app_.scheduler().connect<ValidationReply>(
        scope, [weak = weak_from_this()](std::shared_ptr<const ValidationReply> reply) {
          if (auto owner = weak.lock()) {
            owner->receive(std::move(reply));
          }
        });

      if (!connected) {
        result = std::unexpected(connected.error() == SchedulerError::NO_MEMORY     ? TreeError::NO_MEMORY
                                 : connected.error() == SchedulerError::WOULD_BLOCK ? TreeError::WOULD_BLOCK
                                                                                    : TreeError::INVALID_ARGUMENT);

        return;
      }

      connection_.emplace(std::move(*connected));
    });

    return admitted ? result : admitted;
  }

  void completion(portals::detail::EditorValidationComplete complete) {
    complete_ = std::move(complete);
  }

  std::expected<EditorValidationDecision, TreeError> check(std::string_view candidate,
                                                           EditorValidationIdentity identity) override {
    if (terminal_) {
      return std::unexpected(TreeError::CLOSED);
    }

    if (approval_ == identity) {
      approval_.reset();

      return EditorValidationDecision::ACCEPT;
    }

    if (current_ == identity) {
      // The synchronous bridge staged its cheap-rule result; keep semantic validation visibly pending.
      stage_(pending_);

      return EditorValidationDecision::PENDING;
    }

    if (candidate.size() > limits_.maxCandidateBytes) {
      stage_(candidateError_);
      limitFailure_ = true;

      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto reply = std::make_shared<ValidationReply>();

    reply->identity = identity;

    auto job = std::make_unique<ValidationJob>(std::string(candidate), work_, reply, connection_->publisher,
                                               limits_.maxMessageBytes);
    if (!active_) {
      auto admitted = SceneAccess::admitActivity(scene_, shared_from_this());

      if (!admitted) {
        stage_(error_);

        return std::unexpected(std::get<TreeError>(admitted.error().cause));
      }
    }

    stage_(pending_);
    current_ = identity;
    prepared_ = std::move(job);
    reply_ = std::move(reply);
    cancelled_ = true;
    active_ = true;

    return EditorValidationDecision::PENDING;
  }

  void retire() noexcept override {
    current_.reset();
    approval_.reset();
    cancelled_ = true;
    readyToService_ = false;
    pendingDelivery_ = false;
    delivered_.reset();
  }

  void flush(bool afterBorrow) noexcept override {
    // Routed input still has an outer EventRouter tree borrow here. Cancellation belongs to afterDispatch service.
    readyToService_ = true;

    if (terminal_ && afterBorrow) {
      cancel();
    }
  }

  void close() noexcept override {
    cancel();
  }

  void hold() noexcept override {
    readyToService_ = false;
  }

  void finished(LineEditResult &result, bool workerFailure) override {
    if (result.recoverable && !result.operation) {
      result.operation = "formField.validateAsync";
    }

    if (limitFailure_) {
      limitFailure_ = false;
      result.status = LineEditStatus::LIMIT_EXCEEDED;
      result.text = TextError{TextErrorKind::INPUT_LIMIT, 0};
      result.operation = "formField.validateAsync.candidate";

      return;
    }

    if (result.status == LineEditStatus::COMMITTED && delivered_) {
      stage_(std::shared_ptr<const FieldValidation>(delivered_, &delivered_->value));
    } else if (result.status == LineEditStatus::VALIDATION_FAILED && delivered_ &&
               delivered_->value.status == FieldStatus::INVALID) {
      stage_(std::shared_ptr<const FieldValidation>(delivered_, &delivered_->value));
    } else if (result.status == LineEditStatus::CANCELLED) {
      stage_(unchecked_);
    } else if (result.status == LineEditStatus::CONFLICT) {
      stage_(conflict_);
    } else if (result.status == LineEditStatus::FAILED) {
      stage_(error_);
    }

    if (workerFailure && delivered_ && delivered_->error) {
      result.async = delivered_->async;
      result.background = delivered_->background;
      result.text = delivered_->text;
      result.operation = delivered_->operation;
    }
  }

  bool active() const noexcept override {
    return active_;
  }

  void cancel() noexcept override {
    // Scene close can reenter from an editor callback. Do not run arbitrary stop callbacks in that transaction.
    const bool safe = readyToService_;

    terminal_ = true;
    retire();

    if (safe) {
      if (background_) {
        background_->cancel();
      }

      prepared_.reset();
      active_ = false;
    }
  }

  void service() noexcept override {
    if (!readyToService_) {
      return;
    }

    try {
      auto live = app_.tree().withScope(editor_, [](const UiScope &) {
      });

      if (!live) {
        terminal_ = true;
        current_.reset();
        cancelled_ = true;
      }

      if (cancelled_) {
        cancelled_ = false;

        if (background_) {
          background_->cancel();
        }

        // A stop callback can reenter through a UI-thread host hook and prepare a newer intent.
        // Its retirement flag belongs to that intent; let the next service drain it before admission.
        if (cancelled_ || !readyToService_) {
          return;
        }
      }

      if (terminal_ || !current_) {
        prepared_.reset();
        reply_.reset();
        active_ = false;

        if (!terminal_) {
          (void)setFormFieldValidation(app_.tree(), field_, FieldValidation{});
        }

        return;
      }

      if (pendingDelivery_) {
        // Retry delivery through the scheduler, never invoke application callbacks inside the activity visitor.
        auto sent = connection_->publisher.publish(delivered_);

        if (sent) {
          pendingDelivery_ = false;
        } else if (sent.error() != SchedulerError::WOULD_BLOCK && sent.error() != SchedulerError::NO_MEMORY) {
          current_.reset();
          active_ = false;
        }

        return;
      }

      if (prepared_) {
        auto *executor = app_.background();

        if (!executor) {
          fail(BackgroundError::UNAVAILABLE);

          return;
        }

        if (!background_) {
          background_ = std::make_unique<BackgroundLatest>(*executor);
        }

        auto admitted = background_->replace(std::move(prepared_));

        if (!admitted) {
          fail(admitted.error());

          return;
        }
      }

      if (background_) {
        auto retried = background_->service();

        if (!retried) {
          fail(retried.error());
        }
      }
    } catch (const std::bad_alloc &) {
      fail(BackgroundError::NO_MEMORY);
    } catch (...) {
      fail(BackgroundError::START_FAILED);
    }
  }

private:
  void fail(BackgroundError error) noexcept {
    const auto identity = current_;
    if (background_) {
      // A terminal rejected admission can still leave BackgroundLatest's owning pending slot populated.
      background_->cancel();
    }

    if (current_ != identity || cancelled_ || !readyToService_) {
      return;
    }

    prepared_.reset();

    if (reply_) {
      reply_->error = backgroundCause(error);
      reply_->background = error;
      delivered_ = reply_;
      pendingDelivery_ = true;

      auto sent = connection_->publisher.publish(delivered_);

      if (sent) {
        pendingDelivery_ = false;
      }
    }
  }

  void receive(std::shared_ptr<const ValidationReply> reply) {
    if (terminal_ || current_ != reply->identity) {
      return;
    }

    delivered_ = reply;
    active_ = false;

    if (!delivered_->error && delivered_->value.status == FieldStatus::VALID) {
      approval_ = reply->identity;
    }

    auto completed = complete_(reply->identity, delivered_->value.status == FieldStatus::VALID, delivered_->error);

    if (!completed && completed.error() == TreeError::WOULD_BLOCK) {
      active_ = true;
      pendingDelivery_ = true;

      return;
    }

    current_.reset();
    approval_.reset();
    delivered_.reset();
    reply_.reset();
  }

  ManagedScene &scene_;
  Application &app_;
  Node<FormField> field_;
  Node<Widget> editor_;
  AsyncFieldValidator work_;
  FieldValidationLimits limits_;
  std::function<void(std::shared_ptr<const FieldValidation>)> stage_;
  std::shared_ptr<const FieldValidation> pending_ = std::make_shared<const FieldValidation>(FieldValidation::pending());
  std::shared_ptr<const FieldValidation> unchecked_ = std::make_shared<const FieldValidation>();
  std::shared_ptr<const FieldValidation> error_ = std::make_shared<const FieldValidation>(
    FieldValidation::error("The validation operation failed; edit or confirm to retry"));
  std::shared_ptr<const FieldValidation> conflict_ = std::make_shared<const FieldValidation>(
    FieldValidation::error("The model changed; restore the current value before retrying"));
  std::shared_ptr<const FieldValidation> candidateError_ =
    std::make_shared<const FieldValidation>(FieldValidation::error("Candidate exceeds the validation byte limit"));
  portals::detail::EditorValidationComplete complete_;
  std::optional<SchedulerConnection<ValidationReply>> connection_;
  std::unique_ptr<BackgroundLatest> background_;
  std::unique_ptr<BackgroundJob> prepared_;
  std::shared_ptr<ValidationReply> reply_;
  std::shared_ptr<const ValidationReply> delivered_;
  std::optional<EditorValidationIdentity> current_;
  std::optional<EditorValidationIdentity> approval_;
  bool cancelled_{};
  bool terminal_{};
  bool active_{};
  bool limitFailure_{};
  bool readyToService_{};
  bool pendingDelivery_{};
};
} // namespace

std::expected<void, ManagedError> attachAsyncField(ManagedScene &scene, Node<FormField> field, Node<Widget> editor,
                                                   LineEditBinding &binding, AsyncFieldValidator work,
                                                   FieldValidationLimits limits,
                                                   std::function<void(std::shared_ptr<const FieldValidation>)> stage) {
  auto &app = SceneAccess::application(scene);
  if (!limits.maxCandidateBytes || !limits.maxMessageBytes) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, field.id(), TreeError::INVALID_ARGUMENT, {}, {}});
  }

  auto required = SceneAccess::requireBackground(scene);

  if (!required) {
    return required;
  }

  auto controller = std::make_shared<FieldAsync>(scene, app, field, editor, std::move(work), limits, std::move(stage));
  auto connected = controller->connect();

  if (!connected) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, field.id(), connected.error(), {}, {}});
  }

  auto complete = portals::detail::LineEditValidationAccess::attach(binding, controller);

  if (!complete) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, field.id(), complete.error(), {}, {}});
  }

  controller->completion(std::move(*complete));

  return {};
}
} // namespace portals::ui::detail
