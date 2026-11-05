// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "external_log_view.hpp"
#include <portals/ui.hpp>

namespace portals::ui::detail {
std::expected<void, ManagedError> setLogModel(const std::shared_ptr<RefTarget> &target, Node<LogView> node,
                                              const LogSource &source, std::source_location origin) {
  const auto owner = eventScene(target);

  if (!owner) {
    return std::unexpected(owner.error());
  }

  auto result = (*owner)->setLogModel(node, source);

  if (!result) {
    result.error().registrationOrigin = origin;
  }

  return result;
}

LogViewConfig controlLogViewConfig(const ControlSettings &settings, LogViewConfig config) noexcept {
  config.limits.maxLines = settings.maxLines.value_or(config.limits.maxLines);
  config.limits.maxBytes = settings.maxBytes.value_or(config.limits.maxBytes);
  config.limits.maxBatchBytes = settings.maxBatchBytes.value_or(config.limits.maxBatchBytes);
  config.limits.maxRecordBytes = settings.maxRecordBytes.value_or(config.limits.maxRecordBytes);
  config.followTail = settings.followTail.value_or(config.followTail);
  config.readOnly = settings.readOnly.value_or(config.readOnly);

  return config;
}

namespace {
std::expected<std::optional<FrameOptions>, ManagedError> logFrame(const MountContext &context, WidthPolicy width) {
  std::optional<FrameOptions> base = context.frameOptions ? std::optional(*context.frameOptions) : FrameOptions{};
  auto effective = controlFrameOptions(*context.controlSettings, std::move(base), width);

  if (!effective) {
    return effective;
  }

  if (context.frameSettings) {
    effective = controlFrameOptions(*context.frameSettings, std::move(*effective), width);
  }

  if (effective && !*effective) {
    return std::unexpected(error({}, TreeError::INVALID_ARGUMENT));
  }

  return effective;
}
} // namespace
} // namespace portals::ui::detail

namespace portals::ui {
Description<LogView> logView(std::string_view initial, LogViewConfig config, ControlStyle style) {
  auto recipe = detail::Access::cold<LogView>(
    [config, style](ManagedScene &scene, Node<Widget> parent,
                    detail::MountContext &context) -> std::expected<Node<LogView>, ManagedError> {
      const auto effective = detail::controlLogViewConfig(*context.controlSettings, config);
      const auto valid = validateLogLimits(effective.limits);

      if (!valid) {
        return std::unexpected(detail::error(parent, valid.error()));
      }

      TextLimits limits;

      limits.max_input_bytes = effective.limits.maxBatchBytes;

      auto text = context.controlSettings->text->resolve(scene.widthPolicy(), limits);

      if (!text) {
        if (const auto *failure = std::get_if<TextError>(&text.error())) {
          return std::unexpected(detail::textError(parent, *failure));
        }

        auto failure = detail::error(parent, TreeError::INVALID_ARGUMENT);

        failure.resource = std::get<TextResourceError>(text.error());

        return std::unexpected(failure);
      }

      auto frame = detail::logFrame(context, scene.widthPolicy());

      if (!frame) {
        return std::unexpected(frame.error());
      }

      return scene.logView(parent, text->bytes(), effective, style, DEFAULT_LOG_VIEW_POLICY, std::move(*frame));
    });

  return detail::Access::controlArguments(detail::Access::configuredControl(std::move(recipe), "LogView"),
                                          ControlSettings{.text = TextValue(initial)});
}

Description<LogView> logView(Ref<LogView> reference, std::string_view initial, LogViewConfig config,
                             ControlStyle style) {
  return std::move(logView(initial, config, style)).reference(std::move(reference));
}

Description<LogView> logView(LogSource source, LogSourceViewConfig config, ControlStyle style) {
  auto recipe =
    detail::Access::cold<LogView>([source = std::move(source), config,
                                   style](ManagedScene &scene, Node<Widget> parent,
                                          detail::MountContext &context) -> std::expected<Node<LogView>, ManagedError> {
      LogViewConfig native;

      native.limits = {config.limits.maxRecords, config.limits.maxBytes, config.limits.maxRecordBytes,
                       config.limits.maxRecordBytes};
      native.followTail = config.followTail;
      native.readOnly = config.readOnly;

      const auto effective = detail::controlLogViewConfig(*context.controlSettings, native);
      auto frame = detail::logFrame(context, scene.widthPolicy());

      if (!frame) {
        return std::unexpected(frame.error());
      }

      auto node = scene.logView(parent, {}, effective, style, DEFAULT_LOG_VIEW_POLICY, std::move(*frame));

      if (!node) {
        return node;
      }

      auto bound = portals::detail::attachExternalLogView(scene, *node, source, effective);

      if (!bound) {
        auto failure = bound.error();
        auto removed = scene.remove(*node);

        if (!removed) {
          if (const auto *cleanup = std::get_if<TreeError>(&removed.error().cause)) {
            failure.cleanup = *cleanup;
          }
        }

        return std::unexpected(failure);
      }

      return node;
    });

  return detail::Access::configuredControl(std::move(recipe), "LogView");
}

Description<LogView> logView(Ref<LogView> reference, LogSource source, LogSourceViewConfig config, ControlStyle style) {
  return std::move(logView(std::move(source), config, style)).reference(std::move(reference));
}
} // namespace portals::ui
