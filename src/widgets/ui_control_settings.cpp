// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/control_context.hpp>

namespace portals::ui::detail {
namespace {
template <class T> void applySetting(T &target, const std::optional<T> &setting) {
  if (setting) {
    target = *setting;
  }
}

constexpr SceneResourceErrorCode resourceCode(ControlContextErrorCode code) noexcept {
  using C = ControlContextErrorCode;
  using R = SceneResourceErrorCode;
  switch (code) {
  case C::NO_MEMORY:
    return R::NO_MEMORY;
  case C::INVALID_LIMIT:
    return R::INVALID_LIMIT;
  case C::DUPLICATE_TYPE:
  case C::BUILTIN_SHADOW:
    return R::DUPLICATE_TYPE;
  case C::UNKNOWN_TYPE:
    return R::UNKNOWN_TYPE;
  case C::INHERITANCE_CYCLE:
    return R::CYCLIC_TYPE;
  case C::INHERITANCE_LIMIT:
  case C::STRUCT_DEPTH_LIMIT:
    return R::DEPTH_LIMIT;
  case C::DEFINITION_LIMIT:
    return R::TYPE_LIMIT;
  case C::MATERIALIZED_LEAF_LIMIT:
    return R::MATERIALIZATION_LIMIT;
  case C::UNKNOWN_PROPERTY:
  case C::UNSUPPORTED_PROPERTY:
    return R::UNKNOWN_PROPERTY;
  case C::DUPLICATE_PROPERTY:
    return R::DUPLICATE_PROPERTY;
  case C::INVALID_NAME:
  case C::INVALID_VALUE:
  case C::FIXED_CYCLE:
    return R::INVALID_VALUE;
  case C::CONFIGURATION_FAILED:
    return R::FACTORY;
  }
  return R::INVALID_VALUE;
}
} // namespace

ManagedError controlContextError(const ControlContextError &failure, Node<Widget> node) {
  const auto cause = failure.code == ControlContextErrorCode::NO_MEMORY              ? TreeError::NO_MEMORY
                     : failure.code == ControlContextErrorCode::CONFIGURATION_FAILED ? TreeError::CALLBACK_FAILED
                                                                                     : TreeError::INVALID_ARGUMENT;
  auto error = ManagedError{ManagedPhase::MOUNT, node.id(), cause, {}, {}};
  try {
    SceneResourceError resource;
    resource.code = resourceCode(failure.code);
    resource.line = 0;
    resource.column = 0;
    if (failure.diagnostic) {
      resource.propertyPath = std::shared_ptr<const std::string>(failure.diagnostic, &failure.diagnostic->path);
      if (const auto &origin = failure.diagnostic->origin) {
        resource.source = origin->sharedSourceIdentity
                            ? origin->sharedSourceIdentity
                            : std::shared_ptr<const std::string>(origin, &origin->sourceIdentity);
        resource.offset = origin->byteOffset;
        resource.line = origin->line;
        resource.column = origin->column;
      }
    }
    error.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
  } catch (const std::bad_alloc &) {
    error.cause = TreeError::NO_MEMORY;
  }
  return error;
}

std::expected<LineEditOptions, ManagedError> controlLineEditOptions(const ControlSettings &settings,
                                                                    const LineEditOptions &base, WidthPolicy policy,
                                                                    const TextResolver &resolver,
                                                                    Node<Widget> node) try {
  LineEditOptions options = base;
  applySetting(options.editMode, settings.editMode);
  applySetting(options.readOnly, settings.readOnly);
  applySetting(options.programmaticChange, settings.programmaticChange);
  applySetting(options.displayMode, settings.displayMode);
  const auto &history = settings.history;
  applySetting(options.history.enabled, history.enabled);
  applySetting(options.history.maxOperations, history.maxOperations);
  applySetting(options.history.maxBytes, history.maxBytes);
  applySetting(options.history.groupTyping, history.groupTyping);
  applySetting(options.history.groupDeletion, history.groupDeletion);
  options.widthPolicy = policy;
  if (settings.placeholder) {
    auto value = resolver ? settings.placeholder->resolve(policy, resolver, options.limits)
                          : settings.placeholder->resolve(policy, options.limits);
    if (!value) {
      auto error = ManagedError{ManagedPhase::MOUNT, node.id(), TreeError::INVALID_ARGUMENT, {}, {}};
      if (const auto *text = std::get_if<TextError>(&value.error())) {
        error.text = *text;
      } else {
        error.resource = std::get<TextResourceError>(value.error());
      }
      return std::unexpected(std::move(error));
    }
    const std::string placeholder(value->bytes());
    options.placeholder = placeholder;
  }
  return std::expected<LineEditOptions, ManagedError>(std::in_place, options);
} catch (const std::bad_alloc &) {
  return std::unexpected(ManagedError{ManagedPhase::MOUNT, node.id(), TreeError::NO_MEMORY, {}, {}});
}

} // namespace portals::ui::detail
