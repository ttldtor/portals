// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/line_edit_text.hpp>
#include <portals/detail/setup_failure.hpp>
#include <portals/message_box.hpp>
#include <portals/ui_ref.hpp>

namespace portals::ui {
namespace detail {
std::expected<NodeGeometry, ManagedError> geometry(const std::shared_ptr<RefTarget> &target) {
  auto owner = eventScene(target);
  if (!owner) {
    return std::unexpected(owner.error());
  }
  return (*owner)->geometry(target->node);
}

std::expected<ManagedScene *, ManagedError> eventScene(const std::shared_ptr<RefTarget> &target) {
  if (target && target->owner != std::this_thread::get_id()) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WRONG_THREAD, {}, {}});
  }

  if (target && target->failure) {
    return std::unexpected(*target->failure);
  }

  if (!target || target->lifetime.expired()) {
    return std::unexpected(
      ManagedError{ManagedPhase::BIND, target ? target->node.id() : NodeId{}, TreeError::CLOSED, {}, {}});
  }

  return target->scene;
}

namespace {
ManagedError failure(Node<Widget> node, TreeError cause) {
  return {ManagedPhase::TEXT, node.id(), cause, {}, {}};
}

ManagedError textFailure(Node<Widget> node, TextValueError cause) {
  auto result = failure(node, TreeError::INVALID_ARGUMENT);

  if (const auto *text = std::get_if<TextError>(&cause)) {
    result.text = *text;
    const auto kind = text->kind;

    if (kind == TextErrorKind::INPUT_LIMIT || kind == TextErrorKind::CLUSTER_LIMIT ||
        kind == TextErrorKind::GRAPHEME_LIMIT || kind == TextErrorKind::CELL_LIMIT) {
      result.cause = TreeError::NODE_LIMIT;
    }
  } else {
    result.resource = std::get<TextResourceError>(cause);
  }

  return result;
}
} // namespace

/** @brief Checked bridge to existing scene ownership and standard retained setters. */
struct RefAccess {
  static std::expected<void, TreeError> display(ManagedScene &scene, Node<Widget> target, DataPresentation value) {
    auto found = std::ranges::find_if(scene.dataStates_, [&](const auto &record) {
      return record->node == target;
    });
    if (found == scene.dataStates_.end()) {
      return std::unexpected(TreeError::STALE_NODE);
    }
    return setDataStateView(scene.app_.tree(), (*found)->node, value);
  }

  static DialogCommandResult dialogResult(std::expected<void, ManagedError> result) {
    return DialogCommandResult(std::move(result));
  }

  static bool eventReported(const ManagedScene &scene, const ManagedError &error) noexcept {
    return scene.shortReported(error);
  }

  static bool focusReported(const ManagedError &error) noexcept {
    return ManagedScene::focusReported(error);
  }

  static void reportDialog(ManagedError &failure, const std::shared_ptr<RefTarget> &target, const char *operation,
                           std::source_location origin) noexcept {
    // Closed/foreign owners never report through another entered facade's collector.
    if (!target || target->owner != std::this_thread::get_id() || target->lifetime.expired()) {
      return;
    }

    auto &scene = *target->scene;
    if (scene.closed_ || !scene.app_.tree().root()) {
      return;
    }

    scene.reportShortError(failure, operation, {}, "Dialog", origin, scene.actionAdmitted_);
  }

  static void report(ManagedError &failure, const std::shared_ptr<RefTarget> &target, const char *operation,
                     std::string_view key, std::string_view expectedType, std::source_location origin) noexcept {
    failure.operation = operation;
    failure.operationOrigin = origin;

    if (target && target->owner == std::this_thread::get_id() && !target->lifetime.expired()) {
      target->scene->reportShortError(failure, operation, key, expectedType, origin);
    } else {
      const auto ticket = SetupFailureScope::latch(failure);
    }
  }

  struct Snapshot {
    std::string bytes = std::string(0, '\0');
    WidthPolicy policy;
    TextLimits limits;
    bool draft{};
    std::uint64_t revision{};

    Snapshot() = default;

    // MSVC Debug string moves can allocate a proxy inside noexcept. Prepare storage, then swap.
    Snapshot(Snapshot &&other)
        : policy(other.policy), limits(other.limits), draft(other.draft), revision(other.revision) {
      bytes.swap(other.bytes);
    }
  };

  static std::expected<ManagedScene *, ManagedError> scene(const std::shared_ptr<RefTarget> &target) {
    // Never inspect the mutable slot or scene from a foreign thread.
    if (target && target->owner != std::this_thread::get_id()) {
      return std::unexpected(failure({}, TreeError::WRONG_THREAD));
    }

    if (target && target->failure) {
      return std::unexpected(*target->failure);
    }

    if (!target || target->lifetime.expired()) {
      return std::unexpected(failure(target ? target->node : Node<Widget>{}, TreeError::CLOSED));
    }

    auto *owner = target->scene;
    auto allowed = owner->boundary(ManagedPhase::TEXT, target->node);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (target->caption == Node<Control>{} && target->draft == Node<LineEdit>{}) {
      return std::unexpected(failure(target->node, TreeError::INVALID_ARGUMENT));
    }

    if (*owner->textMutating_) {
      return std::unexpected(failure(target->node, TreeError::WOULD_BLOCK));
    }

    return owner;
  }

  static std::expected<Snapshot, ManagedError> snapshot(ManagedScene &scene, const RefTarget &target) {
    Snapshot value;
    std::expected<void, TreeError> copied;
    const auto copy = [&](const auto &widget) {
      try {
        value.bytes = widget.text().bytes();
        value.policy = widget.text().policy();

        if constexpr (std::same_as<std::remove_cvref_t<decltype(widget)>, LineEdit>) {
          value.limits = widget.options_.limits;
          value.draft = true;
        }
      } catch (const std::bad_alloc &) {
        copied = std::unexpected(TreeError::NO_MEMORY);
      } catch (const std::length_error &) {
        copied = std::unexpected(TreeError::NODE_LIMIT);
      }
    };
    auto read = target.draft != Node<LineEdit>{} ? scene.app_.tree().read(target.draft, copy)
                                                 : scene.app_.tree().read(target.caption, copy);

    if (!read || !copied) {
      return std::unexpected(failure(target.node, !read ? read.error() : copied.error()));
    }

    if (value.draft) {
      auto entry = scene.find(target.node);
      auto *binding = entry ? std::get_if<LineEditBinding>(&entry->binding) : nullptr;
      auto revision = binding ? binding->draftRevision() : std::unexpected(TreeError::INVALID_ARGUMENT);

      if (!revision) {
        return std::unexpected(failure(target.node, revision.error()));
      }

      value.revision = *revision;
    }

    return value;
  }

  static std::expected<void, ManagedError> apply(ManagedScene &scene, const RefTarget &target, GraphemeText text,
                                                 std::uint64_t revision, std::optional<ProgrammaticChange> change) {
    auto allowed = scene.boundary(ManagedPhase::TEXT, target.node);

    if (!allowed) {
      return allowed;
    }

    std::expected<void, TreeError> updated;

    if (target.draft != Node<LineEdit>{}) {
      auto entry = scene.find(target.node);
      auto *binding = entry ? std::get_if<LineEditBinding>(&entry->binding) : nullptr;

      if (!binding) {
        return std::unexpected(failure(target.node, TreeError::INVALID_ARGUMENT));
      }

      updated = binding->setDraft(std::move(text), revision, change);
    } else {
      updated = setControlText(scene.app_.tree(), target.caption, std::move(text));
    }

    return updated ? std::expected<void, ManagedError>{} : std::unexpected(failure(target.node, updated.error()));
  }

  static std::expected<GraphemeText, ManagedError> committedSnapshot(ManagedScene &scene, const RefTarget &target) {
    auto entry = scene.find(target.node);

    if (!entry || !entry->textModel || !std::holds_alternative<LineEditBinding>(entry->binding)) {
      return std::unexpected(failure(target.node, TreeError::INVALID_ARGUMENT));
    }

    auto model = entry->textModel->snapshot();

    if (!model) {
      return std::unexpected(failure(target.node, model.error()));
    }

    WidthPolicy policy;
    TextLimits limits;
    auto read = scene.app_.tree().read(target.draft, [&](const LineEdit &editor) {
      policy = editor.text().policy();
      limits = editor.options_.limits;
    });

    if (!read) {
      return std::unexpected(failure(target.node, read.error()));
    }

    auto text = GraphemeText::from((*model)->value, policy, limits);

    if (!text) {
      return std::unexpected(textFailure(target.node, text.error()));
    }

    if (portals::detail::hasLineSeparator((*model)->value)) {
      return std::unexpected(failure(target.node, TreeError::INVALID_ARGUMENT));
    }

    return std::move(*text);
  }

  // Preparation already owns the UI turn. Compare borrowed bytes before allocating or segmenting.
  static std::expected<std::optional<GraphemeText>, ManagedError>
  capture(ManagedScene &scene, const RefTarget &target, bool committed, const GraphemeText *previous) {
    std::shared_ptr<const PropertyChange<std::string>> model;
    if (committed) {
      auto entry = scene.find(target.node);
      if (!entry || !entry->textModel) {
        return std::unexpected(failure(target.node, TreeError::INVALID_ARGUMENT));
      }
      auto value = entry->textModel->snapshot();
      if (!value) {
        return std::unexpected(failure(target.node, value.error()));
      }
      model = *value;
    } else if (target.draft != Node<LineEdit>{}) {
      auto entry = scene.find(target.node);
      auto *binding = entry ? std::get_if<LineEditBinding>(&entry->binding) : nullptr;
      auto revision = binding ? binding->draftRevision() : std::unexpected(TreeError::INVALID_ARGUMENT);
      if (!revision) {
        return std::unexpected(failure(target.node, revision.error()));
      }
    }

    std::expected<std::optional<GraphemeText>, ManagedError> result;
    const auto copy = [&](const auto &widget) {
      try {
        const auto bytes = model ? std::string_view(model->value) : widget.text().bytes();
        const auto policy = widget.text().policy();
        if (previous && previous->bytes() == bytes && previous->policy() == policy) {
          return;
        }
        TextLimits limits;
        if constexpr (std::same_as<std::remove_cvref_t<decltype(widget)>, LineEdit>) {
          limits = widget.options_.limits;
        }
        auto text = GraphemeText::from(bytes, policy, limits);
        if (!text) {
          result = std::unexpected(textFailure(target.node, text.error()));
        } else if (model && portals::detail::hasLineSeparator(bytes)) {
          result = std::unexpected(failure(target.node, TreeError::INVALID_ARGUMENT));
        } else {
          result->emplace(std::move(*text));
        }
      } catch (const std::bad_alloc &) {
        result = std::unexpected(failure(target.node, TreeError::NO_MEMORY));
      } catch (const std::length_error &) {
        result = std::unexpected(failure(target.node, TreeError::NODE_LIMIT));
      }
    };
    auto read = target.draft != Node<LineEdit>{} ? scene.app_.tree().read(target.draft, copy)
                                                 : scene.app_.tree().read(target.caption, copy);
    return read ? std::move(result) : std::unexpected(failure(target.node, read.error()));
  }

  static EditorCommandResult command(std::shared_ptr<RefTarget> target, bool redo, bool stop,
                                     std::source_location origin, bool clear) {
    auto owner = scene(target);
    std::weak_ptr<const void> collector;
    std::expected<void, TreeError> result;

    bool reported = false;

    if (!owner) {
      result = std::unexpected(std::get<TreeError>(owner.error().cause));
    } else {
      collector = target->lifetime;

      auto entry = (*owner)->find(target->node);
      auto *binding = entry ? std::get_if<LineEditBinding>(&entry->binding) : nullptr;

      if (!binding) {
        result = std::unexpected(TreeError::INVALID_ARGUMENT);
      } else if (stop) {
        result = binding->stopGrouping();
      } else {
        auto receipt = clear ? binding->clearCommand(origin) : binding->historyCommand(redo, origin);

        result = receipt.first;
        reported = receipt.second;
      }
    }

    // Publication/result callbacks may have destroyed the scene. Check the token before any scene access.

    if (!result && !reported && target && target->owner == std::this_thread::get_id() && !target->lifetime.expired()) {
      collector = target->lifetime;

      const auto commandFailure = failure(target->node, result.error());

      try {
        const ManagedDiagnosticContext context{std::string(clear  ? "lineEdit.clear"
                                                           : stop ? "lineEdit.stopGrouping"
                                                           : redo ? "lineEdit.redo"
                                                                  : "lineEdit.undo"),
                                               std::string(0, '\0'), std::string(0, '\0'), std::string("LineEdit"),
                                               origin};
        reported = target->scene->reportError(commandFailure, std::span(&context, 1), origin).has_value();
      } catch (const std::bad_alloc &) {
        reported = target->scene->reportError(commandFailure, {}, origin).has_value();
      }
    }

    return EditorCommandResult(result, std::move(collector), reported);
  }

  static std::expected<HistoryAvailability, TreeError> availability(std::shared_ptr<RefTarget> target) {
    auto owner = scene(target);

    if (!owner) {
      return std::unexpected(std::get<TreeError>(owner.error().cause));
    }

    auto entry = (*owner)->find(target->node);
    auto *binding = entry ? std::get_if<LineEditBinding>(&entry->binding) : nullptr;

    return binding ? binding->historyAvailability() : std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  static TextProperty mutate(TextProperty &property, const TextValue *expression, std::string_view literal, bool append,
                             std::source_location origin) {
    auto target = property.target_;
    auto resolver = property.resolver_;
    auto owner = scene(target);

    if (!owner) {
      property.result_ = std::unexpected(owner.error());
      property.report(append ? "text.append" : "text.set", {}, origin);

      return property;
    }

    struct MutationGuard {
      std::shared_ptr<bool> busy;

      ~MutationGuard() {
        *busy = false;
      }
    } guard{(*owner)->textMutating_};

    *guard.busy = true;

    try {
      std::string key(0, '\0');
      auto current = snapshot(**owner, *target);

      if (!current) {
        property.result_ = std::unexpected(current.error());
      } else {
        // Own all inputs before user code; neither widget nor expression bytes cross the resolver borrow.
        const auto owned = expression ? *expression : TextValue(literal);

        key = owned.resourceKey();

        TextLimits expressionLimits{current->limits.max_input_bytes, current->limits.max_input_bytes,
                                    current->limits.max_input_bytes, static_cast<std::size_t>(INT32_MAX)};
        auto resolved = resolver ? owned.resolve(current->policy, *resolver, expressionLimits)
                                 : owned.resolve(current->policy, expressionLimits);

        if (!resolved) {
          property.result_ = std::unexpected(textFailure(target->node, resolved.error()));
        } else if (target->lifetime.expired()) {
          property.result_ = std::unexpected(failure(target->node, TreeError::CLOSED));
        } else {
          std::string candidate(append ? std::string_view(current->bytes) : std::string_view{});

          candidate.append(resolved->bytes());

          auto text = GraphemeText::from(candidate, current->policy, current->limits);

          if (!text) {
            property.result_ = std::unexpected(textFailure(target->node, TextValueError{text.error()}));
          } else if (current->draft && portals::detail::hasLineSeparator(candidate)) {
            // These are valid Unicode; the one-line editor contract rejects them without inventing a TextError.
            property.result_ = std::unexpected(failure(target->node, TreeError::INVALID_ARGUMENT));
          } else {
            property.result_ = apply(**owner, *target, std::move(*text), current->revision, property.change_);
          }
        }
      }

      property.report(append ? "text.append" : "text.set", key, origin);

      return property;
    } catch (const std::bad_alloc &) {
      property.result_ = std::unexpected(failure(target->node, TreeError::NO_MEMORY));
    } catch (const std::length_error &) {
      property.result_ = std::unexpected(failure(target->node, TreeError::NODE_LIMIT));
    } catch (...) {
      property.result_ = std::unexpected(failure(target->node, TreeError::CALLBACK_FAILED));
    }

    property.report(append ? "text.append" : "text.set", {}, origin);

    return property;
  }
};

void reportEventError(const std::shared_ptr<RefTarget> &target, const ManagedError &error,
                      std::source_location origin) {
  auto scene = eventScene(target);

  if (scene && !RefAccess::eventReported(**scene, error)) {
    (void)(*scene)->reportError(error, {}, origin);
  }
}

void reportPasswordError(const std::shared_ptr<RefTarget> &target, ManagedError &failure, std::source_location origin) {
  auto scene = eventScene(target);

  if (scene && RefAccess::eventReported(**scene, failure)) {
    return;
  }

  RefAccess::report(failure, target, "password.value", {}, "Password", origin);
}

std::expected<PasswordValue, ManagedError> passwordValue(std::shared_ptr<RefTarget> target,
                                                         std::source_location origin) {
  auto scene = eventScene(target);
  auto result = scene ? (*scene)->passwordValue(target->secret)
                      : std::expected<PasswordValue, ManagedError>(std::unexpected(scene.error()));

  if (!result) {
    reportPasswordError(target, result.error(), origin);
  }

  return result;
}

std::expected<void, ManagedError> setPasswordValue(std::shared_ptr<RefTarget> target, PasswordValue value,
                                                   std::source_location origin) {
  auto scene = eventScene(target);
  auto result = scene ? (*scene)->setPasswordValue(target->secret, std::move(value))
                      : std::expected<void, ManagedError>(std::unexpected(scene.error()));

  if (!result) {
    reportPasswordError(target, result.error(), origin);
  }

  return result;
}

void reportFocusError(const std::shared_ptr<RefTarget> &target, const ManagedError &error,
                      std::source_location origin) {
  auto scene = eventScene(target);

  if (scene && !RefAccess::focusReported(error)) {
    (void)(*scene)->reportError(error, {}, origin);
  }
}

DialogCommandResult MessageAccess::command(std::shared_ptr<RefTarget> target, std::expected<void, ManagedError> result,
                                           std::source_location origin) {
  if (!result && !RefAccess::focusReported(result.error())) {
    RefAccess::reportDialog(result.error(), target, "MessageBox.command", origin);
  }
  return DialogCommandResult(std::move(result));
}

DialogCommandResult dialogCommand(const std::shared_ptr<RefTarget> &target, bool opening, std::source_location origin) {
  auto owner = eventScene(target);
  if (!owner) {
    return RefAccess::dialogResult(std::unexpected(owner.error()));
  }
  auto result = opening ? (*owner)->openDialog(target->node) : (*owner)->dismissDialog(target->node);
  if (!result && !RefAccess::focusReported(result.error())) {
    RefAccess::reportDialog(result.error(), target, opening ? "Dialog.open" : "Dialog.dismiss", origin);
  }
  return RefAccess::dialogResult(std::move(result));
}

EditorCommandResult editorCommand(std::shared_ptr<RefTarget> target, bool redo, bool stop, std::source_location origin,
                                  bool clear) {
  return RefAccess::command(std::move(target), redo, stop, origin, clear);
}

std::expected<HistoryAvailability, TreeError> editorAvailability(std::shared_ptr<RefTarget> target) {
  return RefAccess::availability(std::move(target));
}
} // namespace detail

std::expected<GraphemeText, ManagedError> TextSource::snapshot() const {
  auto owner = detail::RefAccess::scene(target_);

  if (!owner) {
    return std::unexpected(owner.error());
  }

  try {
    if (kind_ == Kind::COMMITTED) {
      return detail::RefAccess::committedSnapshot(**owner, *target_);
    }

    auto value = detail::RefAccess::snapshot(**owner, *target_);

    if (!value) {
      return std::unexpected(value.error());
    }

    auto text = GraphemeText::from(value->bytes, value->policy, value->limits);

    if (!text) {
      return std::unexpected(detail::textFailure(target_->node, text.error()));
    }

    return std::move(*text);
  } catch (const std::bad_alloc &) {
    return std::unexpected(detail::failure(target_->node, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(detail::failure(target_->node, TreeError::NODE_LIMIT));
  }
}

TextProperty::TextProperty(std::shared_ptr<detail::RefTarget> target, TextResolver resolver,
                           std::string_view expectedType, std::source_location origin,
                           std::optional<ProgrammaticChange> change)
    : target_(std::move(target)), resolver_(resolver ? std::make_shared<TextResolver>(std::move(resolver)) : nullptr),
      expectedType_(expectedType), origin_(origin), change_(change) {
}

std::expected<std::string, ManagedError> TextProperty::get() const {
  auto owner = detail::RefAccess::scene(target_);

  if (!owner) {
    return std::unexpected(owner.error());
  }

  try {
    auto value = detail::RefAccess::snapshot(**owner, *target_);

    if (!value) {
      return std::unexpected(value.error());
    }

    return std::expected<std::string, ManagedError>(std::in_place, std::string_view(value->bytes));
  } catch (const std::bad_alloc &) {
    return std::unexpected(detail::failure(target_->node, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(detail::failure(target_->node, TreeError::NODE_LIMIT));
  }
}

std::expected<void, ManagedError> TextProperty::result() const {
  return result_;
}

void TextProperty::report(const char *operation, std::string_view key, std::source_location origin) {
  if (result_) {
    return;
  }

  // An observer can attempt another operation through this same proxy. Preserve the outer receipt.
  auto original = result_.error();

  // Operation labels are library literals; no caller-owned view is retained in error metadata.
  detail::RefAccess::report(original, target_, operation, key, expectedType_, origin);

  result_ = std::unexpected(std::move(original));
}

TextProperty TextProperty::mutate(const TextValue &value, bool append, std::source_location origin) {
  return detail::RefAccess::mutate(*this, &value, {}, append, origin);
}

TextProperty TextProperty::mutate(std::string_view value, bool append, std::source_location origin) {
  return detail::RefAccess::mutate(*this, nullptr, value, append, origin);
}

TextProperty TextProperty::set(std::string_view value, std::source_location origin) {
  return mutate(value, false, origin);
}

TextProperty TextProperty::set(const TextValue &value, std::source_location origin) {
  return mutate(value, false, origin);
}

TextProperty TextProperty::append(std::string_view value, std::source_location origin) {
  return mutate(value, true, origin);
}

TextProperty TextProperty::append(const TextValue &value, std::source_location origin) {
  return mutate(value, true, origin);
}

TextProperty TextProperty::operator=(std::string_view value) {
  return set(value, origin_);
}

TextProperty TextProperty::operator=(const TextValue &value) {
  return set(value, origin_);
}

TextProperty TextProperty::operator+=(std::string_view value) {
  return append(value, origin_);
}

TextProperty TextProperty::operator+=(const TextValue &value) {
  return append(value, origin_);
}

TextResult TextProperty::readResult() const {
  auto owner = detail::RefAccess::scene(target_);

  if (!owner) {
    return TextResult(std::unexpected(owner.error()), resolver_, origin_);
  }

  try {
    auto value = detail::RefAccess::snapshot(**owner, *target_);

    if (!value) {
      return TextResult(std::unexpected(value.error()), resolver_, origin_);
    }

    auto text = GraphemeText::from(value->bytes, value->policy);

    if (!text) {
      return TextResult(std::unexpected(detail::textFailure(target_->node, TextValueError{text.error()})), resolver_,
                        origin_);
    }

    return TextResult(std::move(*text), resolver_, origin_);
  } catch (const std::bad_alloc &) {
    return TextResult(std::unexpected(detail::failure(target_->node, TreeError::NO_MEMORY)), resolver_, origin_);
  } catch (const std::length_error &) {
    return TextResult(std::unexpected(detail::failure(target_->node, TreeError::NODE_LIMIT)), resolver_, origin_);
  }
}

TextProperty TextProperty::mutate(const TextResult &value, bool append) {
  if (value.value_) {
    return mutate(value.value_->bytes(), append, origin_);
  }

  auto owner = detail::RefAccess::scene(target_);

  result_ = std::unexpected(owner ? value.value_.error() : owner.error());
  report(append ? "text.append.concat" : "text.assign.concat", {}, value.origin_);

  return *this;
}

TextProperty TextProperty::operator=(const TextResult &value) {
  return mutate(value, false);
}

TextProperty TextProperty::operator=(const TextProperty &value) {
  return mutate(value.readResult(), false);
}

TextProperty TextProperty::operator+=(const TextResult &value) {
  return mutate(value, true);
}

TextResult TextProperty::operator+(const TextProperty &value) const {
  return readResult() + value;
}

TextResult TextProperty::operator+(std::string_view value) const {
  return readResult() + value;
}

TextResult TextProperty::operator+(const TextValue &value) const {
  return readResult() + value;
}

TextResult TextProperty::operator+(const TextResult &value) const {
  return readResult() + value;
}

TextResult::TextResult(std::expected<GraphemeText, ManagedError> value, std::shared_ptr<TextResolver> resolver,
                       std::source_location origin)
    : value_(std::move(value)), resolver_(std::move(resolver)), origin_(origin) {
}

std::expected<GraphemeText, ManagedError> TextResult::result() const {
  return value_;
}

TextResult TextResult::join(std::string_view bytes) const {
  if (!value_) {
    return *this;
  }

  try {
    std::string candidate(value_->bytes());

    candidate.append(bytes);

    auto text = GraphemeText::from(candidate, value_->policy());

    if (!text) {
      return TextResult(std::unexpected(detail::textFailure({}, TextValueError{text.error()})), resolver_, origin_);
    }

    return TextResult(std::move(*text), resolver_, origin_);
  } catch (const std::bad_alloc &) {
    return TextResult(std::unexpected(detail::failure({}, TreeError::NO_MEMORY)), resolver_, origin_);
  } catch (const std::length_error &) {
    return TextResult(std::unexpected(detail::failure({}, TreeError::NODE_LIMIT)), resolver_, origin_);
  }
}

TextResult TextResult::operator+(const TextProperty &value) const {
  if (!value_) {
    return *this;
  }

  return *this + value.readResult();
}

TextResult TextResult::operator+(std::string_view value) const {
  return join(value);
}

TextResult TextResult::operator+(const TextValue &value) const {
  if (!value_) {
    return *this;
  }

  try {
    const auto owned = value;
    const TextLimits limits{TextLimits{}.max_input_bytes, TextLimits{}.max_input_bytes, TextLimits{}.max_input_bytes,
                            static_cast<std::size_t>(INT32_MAX)};
    auto resolved =
      resolver_ ? owned.resolve(value_->policy(), *resolver_, limits) : owned.resolve(value_->policy(), limits);

    if (!resolved) {
      return TextResult(std::unexpected(detail::textFailure({}, resolved.error())), resolver_, origin_);
    }

    return join(resolved->bytes());
  } catch (const std::bad_alloc &) {
    return TextResult(std::unexpected(detail::failure({}, TreeError::NO_MEMORY)), resolver_, origin_);
  } catch (const std::length_error &) {
    return TextResult(std::unexpected(detail::failure({}, TreeError::NODE_LIMIT)), resolver_, origin_);
  }
}

TextResult TextResult::operator+(const TextResult &value) const {
  if (!value_) {
    return *this;
  }

  if (!value.value_) {
    return value;
  }

  return join(value.value_->bytes());
}
} // namespace portals::ui

namespace portals {
std::expected<ManagedSource<GraphemeText>, ManagedError>
ManagedScene::liveText(const ui::TextSource &source, PropertyOptions options, std::source_location registered) {
  return attachLiveText(source, options, registered, true);
}

std::expected<ManagedSource<GraphemeText>, ManagedError> ManagedScene::attachLiveText(const ui::TextSource &source,
                                                                                      PropertyOptions options,
                                                                                      std::source_location registered,
                                                                                      bool publicUse) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  const auto fail = [&](TreeError cause) {
    auto failure = error(ManagedPhase::TEXT, root_, cause);
    failure.operation = "liveText.register";
    failure.registrationOrigin = registered;
    return std::unexpected(failure);
  };
  if (owner_ != std::this_thread::get_id()) {
    return fail(TreeError::WRONG_THREAD);
  }
  auto target = source.target_;
  auto owner = ui::detail::RefAccess::scene(target);
  if (!owner) {
    return std::unexpected(owner.error());
  }
  if (*owner != this || !options.max_observers) {
    return fail(TreeError::INVALID_ARGUMENT);
  }
  pruneLiveText();
  const auto kind = static_cast<int>(source.kind_);
  const char *operation = source.kind_ == ui::TextSource::Kind::CAPTION ? "liveText.caption"
                          : source.kind_ == ui::TextSource::Kind::DRAFT ? "liveText.draft"
                                                                        : "liveText.committed";
  const auto handle = [&](const std::shared_ptr<LiveTextRecord> &record) {
    ManagedState<GraphemeText> state;
    state.model_ = record->property;
    state.facade_ = facade_;
    state.lifetime_ = lifetime_;
    state.owner_ = owner_;
    state.scene_ = root_.id();
    state.origin_ = record->origin;
    return ManagedSource<GraphemeText>(std::move(state));
  };
  for (const auto &record : liveText_) {
    if (record->origin->node == target->node && record->kind == kind) {
      if (record->options.max_observers == options.max_observers && publicUse) {
        record->publicUse = true;
      }
      return record->options.max_observers == options.max_observers
               ? std::expected<ManagedSource<GraphemeText>, ManagedError>(handle(record))
               : fail(TreeError::INVALID_ARGUMENT);
    }
  }
  auto admitted = observerAdmission(target->node);
  if (!admitted) {
    return std::unexpected(admitted.error());
  }
  if (properties_.size() >= options_.maxStates) {
    return fail(TreeError::NODE_LIMIT);
  }

  try {
    auto initial =
      ui::detail::RefAccess::capture(*this, *target, source.kind_ == ui::TextSource::Kind::COMMITTED, nullptr);
    if (!initial) {
      auto failure = initial.error();
      failure.operation = operation;
      failure.registrationOrigin = registered;
      return std::unexpected(failure);
    }
    auto created =
      Property<GraphemeText>::create(app_.tree(), root_, std::move(**initial), Invalidation::PAINT, options);
    if (!created) {
      return fail(created.error());
    }
    auto record = std::make_shared<LiveTextRecord>();
    record->origin = std::make_shared<detail::ManagedOrigin>();
    record->origin->tree = &app_.tree();
    record->origin->node = target->node;
    record->origin->lifetime = lifetime_;
    record->kind = kind;
    record->options = options;
    record->publicUse = publicUse;
    record->registered = registered;
    record->operation = operation;
    record->property = std::shared_ptr<Property<GraphemeText>>(std::move(*created));
    record->capture = [this, target,
                       committed = source.kind_ == ui::TextSource::Kind::COMMITTED](const GraphemeText *last) {
      return ui::detail::RefAccess::capture(*this, *target, committed, last);
    };
    if (target->draft != Node<LineEdit>{}) {
      record->validateModel = [this, target] {
        return ui::detail::RefAccess::capture(*this, *target, true, nullptr);
      };
    }
    properties_.reserve(properties_.size() + 1);
    liveText_.reserve(liveText_.size() + 1);
    auto dirty = app_.tree().invalidate(root_, Invalidation::PAINT);
    if (!dirty) {
      return fail(dirty.error());
    }
    auto checked = record->origin->check();
    if (!checked) {
      return fail(checked.error());
    }
    auto result = handle(record);
    properties_.push_back(record->property);
    liveText_.push_back(std::move(record));
    return result;
  } catch (const std::bad_alloc &) {
    return fail(TreeError::NO_MEMORY);
  } catch (...) {
    return fail(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals

std::expected<void, portals::TreeError> portals::ui::detail::displayDataState(std::shared_ptr<RefTarget> target,
                                                                              DataPresentation value,
                                                                              std::source_location origin) {
  auto scene = eventScene(target);
  auto result = scene ? RefAccess::display(**scene, target->node, value)
                      : std::expected<void, TreeError>(std::unexpected(std::get_if<TreeError>(&scene.error().cause)
                                                                         ? std::get<TreeError>(scene.error().cause)
                                                                         : TreeError::CALLBACK_FAILED));
  if (!result) {
    auto failure =
      scene ? ManagedError{ManagedPhase::PREPARE, target->node.id(), result.error(), {}, {}} : scene.error();
    RefAccess::report(failure, target, "dataState.display", {}, "DataStateView", origin);
  }
  return result;
}
