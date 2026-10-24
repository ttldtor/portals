// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

namespace portals::ui::detail {
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
} // namespace portals::ui
