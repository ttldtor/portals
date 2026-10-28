// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/control_context.hpp>

#include <cmath>
#include <limits>
#include <utf8proc.h>

namespace portals::ui::detail {
SpinnerOptions controlSpinnerOptions(const ControlSettings &settings, SpinnerOptions options) {
  if (settings.spinnerPreset) {
    options.preset = *settings.spinnerPreset;
  }
  if (settings.spinnerFrames) {
    options.frames = *settings.spinnerFrames;
  }
  if (settings.spinnerTick) {
    options.interval = *settings.spinnerTick;
  }
  if (settings.spinnerDirection) {
    options.direction = *settings.spinnerDirection;
  }
  if (settings.spinnerAlignment) {
    options.alignment = *settings.spinnerAlignment;
  }
  if (settings.spinnerActive) {
    options.active = *settings.spinnerActive;
  }
  if (settings.spinnerReducedMotion) {
    options.reducedMotion = *settings.spinnerReducedMotion;
  }
  if (settings.spinnerLabel) {
    options.label = *settings.spinnerLabel;
  }
  if (settings.spinnerMaxFrames) {
    options.maxFrames = *settings.spinnerMaxFrames;
  }
  if (settings.spinnerMaxFrameBytes) {
    options.maxFrameBytes = *settings.spinnerMaxFrameBytes;
  }
  if (settings.spinnerMaxFrameCells) {
    options.maxFrameCells = *settings.spinnerMaxFrameCells;
  }
  if (settings.maxPaintCells) {
    options.maxPaintCells = *settings.maxPaintCells;
  }
  return options;
}

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

LinearOptions controlLinearOptions(const ControlSettings &settings, LinearOptions options) noexcept {
  applySetting(options.gap, settings.gap);
  applySetting(options.padding.left, settings.grid.paddingLeft);
  applySetting(options.padding.top, settings.grid.paddingTop);
  applySetting(options.padding.right, settings.grid.paddingRight);
  applySetting(options.padding.bottom, settings.grid.paddingBottom);
  applySetting(options.main_align, settings.linear.mainAlign);
  applySetting(options.cross_align, settings.linear.crossAlign);
  applySetting(options.overflow, settings.linear.overflow);

  return options;
}

TextLimits controlTextLimits(const ControlSettings &settings, TextLimits limits) noexcept {
  applySetting(limits.max_input_bytes, settings.limits.maxInputBytes);
  applySetting(limits.max_cluster_bytes, settings.limits.maxClusterBytes);
  applySetting(limits.max_graphemes, settings.limits.maxGraphemes);
  applySetting(limits.max_cells, settings.limits.maxCells);

  return limits;
}

std::expected<OverlayOptions, TreeError> controlOverlayOptions(const ControlSettings &settings,
                                                               OverlayOptions options) noexcept {
  applySetting(options.padding.left, settings.grid.paddingLeft);
  applySetting(options.padding.top, settings.grid.paddingTop);
  applySetting(options.padding.right, settings.grid.paddingRight);
  applySetting(options.padding.bottom, settings.grid.paddingBottom);
  applySetting(options.horizontal, settings.horizontalAlign);
  applySetting(options.vertical, settings.verticalAlign);
  const auto validAlign = [](LayoutAlign align) {
    return align == LayoutAlign::START || align == LayoutAlign::CENTER || align == LayoutAlign::END ||
           align == LayoutAlign::STRETCH;
  };
  if (options.padding.left < 0 || options.padding.top < 0 || options.padding.right < 0 || options.padding.bottom < 0 ||
      !validAlign(options.horizontal) || !validAlign(options.vertical)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  return options;
}

BorderOptions controlBorderOptions(const ControlSettings &settings, BorderOptions options) noexcept {
  applySetting(options.glyphs, settings.glyphs);
  applySetting(options.maxPaintCells, settings.maxPaintCells);

  return options;
}

GaugeOptions controlGaugeOptions(const ControlSettings &settings, GaugeOptions options) noexcept {
  applySetting(options.glyphs, settings.glyphs);
  applySetting(options.maxPaintCells, settings.maxPaintCells);
  applySetting(options.minimum, settings.minimum);
  applySetting(options.maximum, settings.maximum);
  applySetting(options.preferredWidth, settings.preferredWidth);

  return options;
}

namespace {
template <class Options> void applyScrollSettings(const ControlSettings &settings, Options &options) noexcept {
  applySetting(options.wheelStep, settings.wheelStep);
  applySetting(options.keyStep, settings.keyStep);
  applySetting(options.pageOverlap, settings.pageOverlap);
  applySetting(options.minimumThumb, settings.minimumThumb);
  applySetting(options.arrows, settings.arrows);
  applySetting(options.arrowDelay, settings.arrowDelay);
  applySetting(options.arrowRepeat, settings.arrowRepeat);
  applySetting(options.focusable, settings.focusable);
  applySetting(options.readOnly, settings.readOnly);
  applySetting(options.glyphs, settings.glyphs);
  applySetting(options.maxPaintCells, settings.maxPaintCells);
}
} // namespace

bool hasScrollbarSettings(const ControlScrollbarSettings &settings) noexcept {
  return settings.pattern || settings.ends || settings.maxGlyphBytes || settings.horizontal.track ||
         settings.horizontal.thumb || settings.horizontal.start || settings.horizontal.end || settings.vertical.track ||
         settings.vertical.thumb || settings.vertical.start || settings.vertical.end || settings.corner ||
         settings.horizontal.backward || settings.horizontal.forward || settings.vertical.backward ||
         settings.vertical.forward || settings.trackPattern || settings.thumbPattern || settings.arrowPattern ||
         settings.horizontal.thumbStart || settings.horizontal.thumbBody || settings.horizontal.thumbEnd ||
         settings.horizontal.thumbSingle || settings.vertical.thumbStart || settings.vertical.thumbBody ||
         settings.vertical.thumbEnd || settings.vertical.thumbSingle;
}

std::expected<ScrollAreaOptions, TreeError> controlScrollAreaOptions(const ControlSettings &settings,
                                                                     ScrollAreaOptions options) noexcept {
  applyScrollSettings(settings, options);
  applySetting(options.horizontal, settings.horizontal);
  applySetting(options.vertical, settings.vertical);
  applySetting(options.placement, settings.placement);

  if (settings.frame.enabled) {
    if (*settings.frame.enabled) {
      if (!options.frame) {
        options.frame.emplace();
      }
    } else {
      options.frame.reset();
    }
  }

  if (settings.frame.glyphs || settings.frame.maxPaintCells) {
    if (!options.frame) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    applySetting(options.frame->glyphs, settings.frame.glyphs);
    applySetting(options.frame->maxPaintCells, settings.frame.maxPaintCells);
  }

  const auto checked = validateScrollOptions(options);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  return options;
}

std::expected<ScrollBarOptions, TreeError> controlScrollBarOptions(const ControlSettings &settings,
                                                                   ScrollBarOptions options) noexcept {
  applyScrollSettings(settings, options);

  const auto checked = validateScrollOptions(options);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  return options;
}

bool validControlLiteral(std::string_view value) noexcept {
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
    return false;
  }
  auto bytes = reinterpret_cast<const utf8proc_uint8_t *>(value.data());
  auto remaining = static_cast<utf8proc_ssize_t>(value.size());
  while (remaining) {
    utf8proc_int32_t codepoint{};
    const auto consumed = utf8proc_iterate(bytes, remaining, &codepoint);
    if (consumed <= 0) {
      return false;
    }
    bytes += consumed;
    remaining -= consumed;
  }
  return true;
}

namespace {
template <class Options> void applyKeyHelpSettings(const ControlSettings &settings, Options &options) {
  applySetting(options.format.maxBytesPerBinding, settings.format.maxBytesPerBinding);
  applySetting(options.format.strokeSeparator, settings.format.strokeSeparator);
  applySetting(options.format.alternativeSeparator, settings.format.alternativeSeparator);
  applySetting(options.format.repeatMarker, settings.format.repeatMarker);
  applySetting(options.format.releaseMarker, settings.format.releaseMarker);
  applySetting(options.format.unknownMarker, settings.format.unknownMarker);
  applySetting(options.emptyText, settings.emptyText);
  applySetting(options.unavailableText, settings.unavailableText);
}
} // namespace

HelpViewOptions controlHelpViewOptions(const ControlSettings &settings, HelpViewOptions options) {
  applyKeyHelpSettings(settings, options);
  applySetting(options.initiallyOpen, settings.initiallyOpen);
  return options;
}

KeyHintsOptions controlKeyHintsOptions(const ControlSettings &settings, KeyHintsOptions options) {
  applyKeyHelpSettings(settings, options);
  applySetting(options.entrySeparator, settings.entrySeparator);
  applySetting(options.overflowMarker, settings.overflowMarker);
  return options;
}

VirtualListOptions controlVirtualListOptions(const ControlSettings &settings, VirtualListOptions options) noexcept {
  applySetting(options.row_height, settings.rowHeight);
  applySetting(options.overscan_rows, settings.overscanRows);

  return options;
}

VirtualTableOptions controlVirtualTableOptions(const ControlSettings &settings, VirtualTableOptions options) noexcept {
  applySetting(options.row_height, settings.rowHeight);
  applySetting(options.overscan_rows, settings.overscanRows);
  applySetting(options.max_columns, settings.maxColumns);
  applySetting(options.max_prepared_cells, settings.maxPreparedCells);
  applySetting(options.max_prepared_text_bytes, settings.maxPreparedTextBytes);

  return options;
}

std::expected<ChartOptions, TreeError> controlChartOptions(const ControlSettings &settings,
                                                           ChartOptions options) noexcept {
  applySetting(options.mode, settings.mode);
  applySetting(options.glyph_set, settings.chartGlyphSet);
  applySetting(options.candle_gap, settings.candleGap);

  const auto &range = settings.yRange;

  if (range.enabled) {
    if (*range.enabled) {
      if (!options.y_range) {
        options.y_range.emplace();
      }
    } else {
      options.y_range.reset();
    }
  }

  // Disabled bounds remain sparse inherited data; clearing a range must not reactivate them.
  if (range.enabled != false && (range.minimum || range.maximum)) {
    if (!options.y_range) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    applySetting(options.y_range->minimum, range.minimum);
    applySetting(options.y_range->maximum, range.maximum);
  }

  if ((range.enabled || range.minimum || range.maximum) && options.y_range &&
      (!std::isfinite(options.y_range->minimum) || !std::isfinite(options.y_range->maximum) ||
       options.y_range->minimum > options.y_range->maximum)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return options;
}

DataStateConfig controlDataStateConfig(const ControlSettings &settings, DataStateConfig config) {
  applySetting(config.showStatus, settings.showStatus);
  applySetting(config.statusRows, settings.statusRows);
  applySetting(config.contentPolicy, settings.contentPolicy);
  applySetting(config.spinnerInterval, settings.spinnerInterval);
  applySetting(config.labels.loading, settings.labels.loading);
  applySetting(config.labels.empty, settings.labels.empty);
  applySetting(config.labels.error, settings.labels.error);
  applySetting(config.labels.offline, settings.labels.offline);
  applySetting(config.labels.refreshing, settings.labels.refreshing);
  applySetting(config.labels.retry, settings.labels.retry);

  return config;
}

OverlayHostOptions controlOverlayHostOptions(const ControlSettings &settings, OverlayHostOptions options) noexcept {
  applySetting(options.maxLayers, settings.maxLayers);

  return options;
}

DialogOptions controlDialogOptions(const ControlSettings &settings, DialogOptions options) noexcept {
  const auto &dialog = settings.dialog;

  applySetting(options.mode, dialog.mode);
  applySetting(options.placement, dialog.placement);
  applySetting(options.maximumPanelSize.width, dialog.maximumPanelSize.width);
  applySetting(options.maximumPanelSize.height, dialog.maximumPanelSize.height);
  applySetting(options.viewportMargin, dialog.viewportMargin);
  applySetting(options.anchorGap, dialog.anchorGap);
  applySetting(options.closeOnEscape, dialog.closeOnEscape);
  applySetting(options.frame.glyphs, dialog.frame.glyphs);
  applySetting(options.frame.maxPaintCells, dialog.frame.maxPaintCells);
  applySetting(options.closeButton.enabled, dialog.closeButton.enabled);
  applySetting(options.closeButton.placement, dialog.closeButton.placement);
  applySetting(options.closeButton.focusable, dialog.closeButton.focusable);

  return options;
}

MessageOptions controlMessageOptions(const ControlSettings &settings, MessageOptions options, DialogButtons buttons) {
  options.dialog = controlDialogOptions(settings, options.dialog);
  const auto &message = settings.message;

  applySetting(options.labels.acknowledge, message.labels.acknowledge);
  applySetting(options.labels.confirm, message.labels.confirm);
  applySetting(options.labels.save, message.labels.save);
  applySetting(options.labels.discard, message.labels.discard);
  applySetting(options.labels.cancel, message.labels.cancel);
  applySetting(options.labels.yes, message.labels.yes);
  applySetting(options.labels.no, message.labels.no);
  applySetting(options.allowDestructiveDefault, message.allowDestructiveDefault);
  applySetting(options.textLimits.max_input_bytes, message.textLimits.maxInputBytes);
  applySetting(options.textLimits.max_cluster_bytes, message.textLimits.maxClusterBytes);
  applySetting(options.textLimits.max_graphemes, message.textLimits.maxGraphemes);
  applySetting(options.textLimits.max_cells, message.textLimits.maxCells);
  applySetting(options.maxBodyLines, message.maxBodyLines);

  const auto preset = dialogButtonPreset(message.buttons.value_or(buttons));

  if (preset && (message.order.first || message.order.second || message.order.third)) {
    if (options.order.empty()) {
      options.order.assign(preset->choices.begin(), preset->choices.begin() + preset->count);
    }

    options.order.resize(preset->count, MessageChoice::CANCEL);

    const std::array values{message.order.first, message.order.second, message.order.third};

    for (std::size_t i = 0; i < values.size(); ++i) {
      if (values[i]) {
        options.order.resize(std::max(options.order.size(), i + 1), MessageChoice::CANCEL);
        options.order[i] = *values[i];
      }
    }
  }

  if (message.defaultChoice.enabled == false) {
    options.defaultChoice.reset();
  } else if (message.defaultChoice.value) {
    options.defaultChoice = *message.defaultChoice.value;
  } else if (message.defaultChoice.enabled == true && !options.defaultChoice) {
    options.defaultChoice = preset ? preset->safeDefault : MessageChoice::CANCEL;
  }

  return options;
}

NotificationOptions controlNotificationOptions(const ControlSettings &settings, NotificationOptions options) {
  const auto &notification = settings.notification;

  applySetting(options.capacity, notification.capacity);
  applySetting(options.maxVisible, notification.maxVisible);
  applySetting(options.placement, notification.placement);
  applySetting(options.maximumWidth, notification.maximumWidth);
  applySetting(options.margin, notification.margin);
  applySetting(options.gap, notification.gap);
  applySetting(options.overflow, notification.overflow);
  applySetting(options.refreshDeduplicated, notification.refreshDeduplicated);
  applySetting(options.reducedMotion, notification.reducedMotion);
  applySetting(options.actionLabelBytes, notification.actionLabelBytes);
  applySetting(options.dedupKeyBytes, notification.dedupKeyBytes);
  applySetting(options.maxPaintCells, notification.maxPaintCells);
  applySetting(options.textLimits.max_input_bytes, notification.textLimits.maxInputBytes);
  applySetting(options.textLimits.max_cluster_bytes, notification.textLimits.maxClusterBytes);
  applySetting(options.textLimits.max_graphemes, notification.textLimits.maxGraphemes);
  applySetting(options.textLimits.max_cells, notification.textLimits.maxCells);

  return options;
}

ThemeSelectorOptions controlThemeSelectorOptions(const ControlSettings &settings, ThemeSelectorOptions options) {
  options.dialog = controlDialogOptions(settings, options.dialog);
  applySetting(options.title, settings.title);
  const auto &selector = settings.themeSelector;

  applySetting(options.apply, selector.labels.apply);
  applySetting(options.cancel, selector.labels.cancel);
  applySetting(options.closeKeepingPreview, selector.labels.closeKeepingPreview);
  applySetting(options.emptyStatus, selector.labels.emptyStatus);
  applySetting(options.errorStatus, selector.labels.errorStatus);
  applySetting(options.conflictStatus, selector.labels.conflictStatus);
  applySetting(options.textLimits.max_input_bytes, selector.textLimits.maxInputBytes);
  applySetting(options.textLimits.max_cluster_bytes, selector.textLimits.maxClusterBytes);
  applySetting(options.textLimits.max_graphemes, selector.textLimits.maxGraphemes);
  applySetting(options.textLimits.max_cells, selector.textLimits.maxCells);

  if (selector.openingShortcut.enabled == false) {
    options.openingShortcut.reset();
  } else if (selector.openingShortcut.value) {
    options.openingShortcut = *selector.openingShortcut.value;
  } else if (selector.openingShortcut.enabled == true && !options.openingShortcut) {
    options.openingShortcut.emplace("Ctrl+K, Ctrl+T");
  }

  return options;
}

std::expected<void, TreeError> controlThemeCatalogOptions(const ControlSettings &settings,
                                                          ThemeSelectionOptions &options,
                                                          std::optional<std::string> &initialId) {
  const auto &catalog = settings.themeCatalog;
  applySetting(options.maxChoices, catalog.maxChoices);
  applySetting(options.maxIdBytes, catalog.maxIdBytes);
  applySetting(options.maxLabelBytes, catalog.maxLabelBytes);
  applySetting(options.maxDescriptionBytes, catalog.maxDescriptionBytes);
  applySetting(options.maxTextBytes, catalog.maxTextBytes);
  applySetting(options.textLimits.max_input_bytes, catalog.textLimits.maxInputBytes);
  applySetting(options.textLimits.max_cluster_bytes, catalog.textLimits.maxClusterBytes);
  applySetting(options.textLimits.max_graphemes, catalog.textLimits.maxGraphemes);
  applySetting(options.textLimits.max_cells, catalog.textLimits.maxCells);

  if (catalog.initialId.enabled == false) {
    initialId.reset();
  } else if (catalog.initialId.value) {
    initialId = *catalog.initialId.value;
  } else if (catalog.initialId.enabled == true && !initialId) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto &layers = catalog.layers;
  if (layers.enabled == false) {
    options.layers.reset();
  } else if (layers.enabled == true || layers.maxLayers || layers.maxRules || layers.maxDeclarations ||
             layers.maxOwnedBytes) {
    if (!options.layers) {
      options.layers.emplace();
    }
    applySetting(options.layers->maxLayers, layers.maxLayers);
    applySetting(options.layers->maxRules, layers.maxRules);
    applySetting(options.layers->maxDeclarations, layers.maxDeclarations);
    applySetting(options.layers->maxOwnedBytes, layers.maxOwnedBytes);
  }

  return {};
}

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
  options.limits = controlTextLimits(settings, options.limits);
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
