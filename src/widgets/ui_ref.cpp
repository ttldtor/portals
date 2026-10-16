// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/setup_failure.hpp>
#include <portals/ui_ref.hpp>

namespace portals::ui {
namespace detail {
std::expected<ManagedScene *, ManagedError> eventScene(const std::shared_ptr<RefTarget> &target) {
  if (target && target->owner != std::this_thread::get_id()) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WRONG_THREAD, {}, {}});
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
  static bool eventReported(const ManagedScene &scene, const ManagedError &error) noexcept {
    return scene.shortReported(error);
  }

  static bool focusReported(const ManagedError &error) noexcept {
    return ManagedScene::focusReported(error);
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

    if (!target || target->lifetime.expired()) {
      return std::unexpected(failure(target ? target->node : Node<Widget>{}, TreeError::CLOSED));
    }

    auto *owner = target->scene;
    auto allowed = owner->boundary(ManagedPhase::TEXT, target->node);

    if (!allowed) {
      return std::unexpected(allowed.error());
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

  static EditorCommandResult command(std::shared_ptr<RefTarget> target, bool redo, bool stop,
                                     std::source_location origin) {
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
        auto receipt = binding->historyCommand(redo, origin);

        result = receipt.first;
        reported = receipt.second;
      }
    }

    // Publication/result callbacks may have destroyed the scene. Check the token before any scene access.

    if (!result && !reported && target && target->owner == std::this_thread::get_id() && !target->lifetime.expired()) {
      collector = target->lifetime;

      const auto commandFailure = failure(target->node, result.error());

      try {
        const ManagedDiagnosticContext context{std::string(stop   ? "lineEdit.stopGrouping"
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
          } else if (current->draft && (candidate.find_first_of("\r\n") != std::string::npos ||
                                        candidate.contains("\xe2\x80\xa8") || candidate.contains("\xe2\x80\xa9"))) {
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

void reportFocusError(const std::shared_ptr<RefTarget> &target, const ManagedError &error,
                      std::source_location origin) {
  auto scene = eventScene(target);

  if (scene && !RefAccess::focusReported(error)) {
    (void)(*scene)->reportError(error, {}, origin);
  }
}

EditorCommandResult editorCommand(std::shared_ptr<RefTarget> target, bool redo, bool stop,
                                  std::source_location origin) {
  return RefAccess::command(std::move(target), redo, stop, origin);
}

std::expected<HistoryAvailability, TreeError> editorAvailability(std::shared_ptr<RefTarget> target) {
  return RefAccess::availability(std::move(target));
}
} // namespace detail

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
