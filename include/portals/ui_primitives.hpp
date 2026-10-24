// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_construction.hpp>

namespace portals::ui {

/** @brief Owning Button decoration pipe stage; shares sparse staging with Description::chrome. */
struct ChromeModifier {
  ButtonChrome value; ///< Owning checked decoration selection.

  /** @param description Consumed standard Button recipe.
   * @param modifier Owning decoration override.
   * @return Recipe staging the same final property as the compact method.
   */
  friend Description<Button> operator|(Description<Button> &&description, ChromeModifier modifier) {
    return std::move(description).chrome(modifier.value);
  }
};

/** @param value Checked Button decoration selection.
 * @return Owning Button-only sparse pipe stage.
 */
inline ChromeModifier chrome(ButtonChrome value) {
  return {value};
}

/** @brief Owning native Border/Gauge/scroll decoration pipe stage. */
struct PrimitiveDecorationModifier {
  std::optional<PrimitiveGlyphs> glyphs;    ///< Present one-cell decoration mode.
  std::optional<std::size_t> maxPaintCells; ///< Present positive paint-work cap.

  /** @tparam T Exactly the native Border, Gauge, ScrollArea or ScrollBar.
   * @param description Consumed native primitive recipe.
   * @param modifier Owning sparse decoration override.
   * @return Recipe staging the same settings as the supported compact methods.
   */
  template <class T>
    requires(std::same_as<T, Border> || std::same_as<T, Gauge> || std::same_as<T, Spinner> ||
             std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  friend Description<T> operator|(Description<T> &&description, PrimitiveDecorationModifier modifier) {
    if (modifier.glyphs) {
      (void)std::move(description).glyphs(*modifier.glyphs);
    }

    if (modifier.maxPaintCells) {
      (void)std::move(description).maxPaintCells(*modifier.maxPaintCells);
    }

    return std::move(description);
  }
};

/** @param value One-cell decoration mode, checked before native construction.
 * @return Owning native Border/Gauge/scroll sparse pipe stage.
 */
inline PrimitiveDecorationModifier glyphs(PrimitiveGlyphs value) {
  return {value, {}};
}

/** @param cells Positive allocated paint-work cap.
 * @return Owning native Border/Gauge/scroll sparse pipe stage.
 */
inline PrimitiveDecorationModifier maxPaintCells(std::size_t cells) {
  return {{}, cells};
}

/** @brief Owning Gauge-only scalar pipe stage, checked against the receiving constructor options. */
struct GaugeScalarModifier {
  ControlSettings settings; ///< Owning sparse value/range/preferred-width override.

  /** @param description Consumed native Gauge recipe.
   * @param modifier Owning sparse override; no widget/model borrow is retained.
   * @return Recipe staging the same settings as the supported compact methods.
   */
  friend Description<Gauge> operator|(Description<Gauge> &&description, GaugeScalarModifier modifier) {
    if (modifier.settings.value) {
      (void)std::move(description).value(*modifier.settings.value);
    }

    if (modifier.settings.minimum) {
      (void)std::move(description).minimum(*modifier.settings.minimum);
    }

    if (modifier.settings.maximum) {
      (void)std::move(description).maximum(*modifier.settings.maximum);
    }

    if (modifier.settings.preferredWidth) {
      (void)std::move(description).preferredWidth(*modifier.settings.preferredWidth);
    }

    return std::move(description);
  }
};

/** @param initial Finite initial Gauge value within the effective range.
 * @return Owning Gauge-only value stage.
 */
inline GaugeScalarModifier value(double initial) {
  return {{.value = initial}};
}

/** @param bound Finite inclusive Gauge lower bound.
 * @return Owning Gauge-only lower-bound stage.
 */
inline GaugeScalarModifier minimum(double bound) {
  return {{.minimum = bound}};
}

/** @param bound Finite inclusive Gauge upper bound, greater than the effective lower bound.
 * @return Owning Gauge-only upper-bound stage.
 */
inline GaugeScalarModifier maximum(double bound) {
  return {{.maximum = bound}};
}

/** @param minimum Finite inclusive Gauge lower bound.
 * @param maximum Finite inclusive Gauge upper bound with a finite positive span.
 * @return Owning Gauge-only range stage.
 */
inline GaugeScalarModifier range(double minimum, double maximum) {
  return {{.minimum = minimum, .maximum = maximum}};
}

/** @param cells Nonnegative intrinsic Gauge width in terminal cells.
 * @return Owning Gauge-only preferred-width stage.
 */
inline GaugeScalarModifier preferredWidth(std::int32_t cells) {
  return {{.preferredWidth = cells}};
}

/** @brief Owning finite Spinner stage; it borrows no widget or scheduler. */
struct SpinnerModifier {
  ControlSettings patch; ///< Sparse checked indicator configuration.

  /** @param description Consumed exact Spinner recipe.
   * @param modifier Owning sparse frame/animation patch.
   * @return Recipe using the same checked methods as fluent spelling.
   */
  friend Description<Spinner> operator|(Description<Spinner> &&description, SpinnerModifier modifier) {
    if (modifier.patch.spinnerPreset) {
      (void)std::move(description).preset(*modifier.patch.spinnerPreset);
    }
    if (modifier.patch.spinnerFrames) {
      (void)std::move(description).frames(*modifier.patch.spinnerFrames);
    }
    if (modifier.patch.spinnerTick) {
      (void)std::move(description).interval(*modifier.patch.spinnerTick);
    }
    if (modifier.patch.spinnerDirection) {
      (void)std::move(description).direction(*modifier.patch.spinnerDirection);
    }
    if (modifier.patch.spinnerAlignment) {
      (void)std::move(description).alignment(*modifier.patch.spinnerAlignment);
    }
    if (modifier.patch.spinnerActive) {
      (void)std::move(description).active(*modifier.patch.spinnerActive);
    }
    if (modifier.patch.spinnerReducedMotion) {
      (void)std::move(description).reducedMotion(*modifier.patch.spinnerReducedMotion);
    }
    if (modifier.patch.spinnerLabel) {
      (void)std::move(description).label(*modifier.patch.spinnerLabel);
    }
    if (modifier.patch.spinnerMaxFrames) {
      (void)std::move(description).maxFrames(*modifier.patch.spinnerMaxFrames);
    }
    if (modifier.patch.spinnerMaxFrameBytes) {
      (void)std::move(description).maxFrameBytes(*modifier.patch.spinnerMaxFrameBytes);
    }
    if (modifier.patch.spinnerMaxFrameCells) {
      (void)std::move(description).maxFrameCells(*modifier.patch.spinnerMaxFrameCells);
    }
    return std::move(description);
  }
};

/** @param value Finite owning Spinner preset setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier preset(SpinnerPreset value) {
  return {{.spinnerPreset = value}};
}

/** @param value Finite owning Spinner frames setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier frames(std::string_view value) {
  return {{.spinnerFrames = std::string(value)}};
}

/** @param value Finite owning Spinner interval setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier interval(SchedulerTime value) {
  return {{.spinnerTick = value}};
}

/** @param value Finite owning Spinner direction setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier direction(SpinnerDirection value) {
  return {{.spinnerDirection = value}};
}

/** @param value Finite owning Spinner alignment setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier alignment(LayoutAlign value) {
  return {{.spinnerAlignment = value}};
}

/** @param value Finite owning Spinner active setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier active(bool value) {
  return {{.spinnerActive = value}};
}

/** @param value Finite owning Spinner reducedMotion setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier reducedMotion(bool value) {
  return {{.spinnerReducedMotion = value}};
}

/** @param value Finite owning Spinner label setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier spinnerLabel(TextValue value) {
  return {{.spinnerLabel = std::move(value)}};
}

/** @param value UTF-8 label bytes copied by this stage.
 * @return Exact Spinner label stage; ui::label remains the Label factory.
 */
inline SpinnerModifier spinnerLabel(std::string_view value) {
  return spinnerLabel(TextValue(value));
}

/** @param value Finite owning Spinner maxFrames setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier maxFrames(std::size_t value) {
  return {{.spinnerMaxFrames = value}};
}

/** @param value Finite owning Spinner maxFrameBytes setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier maxFrameBytes(std::size_t value) {
  return {{.spinnerMaxFrameBytes = value}};
}

/** @param value Finite owning Spinner maxFrameCells setting, checked at preparation.
 * @return Exact Spinner configuration stage.
 */
inline SpinnerModifier maxFrameCells(std::size_t value) {
  return {{.spinnerMaxFrameCells = value}};
}

/** @param options Owning finite single-row frames and animation options.
 * @param style Manual active/dim appearances beneath themes.
 * @param policy Existing owning size constraints.
 * @param resolver Optional synchronous label localization adapter, retained only until mounting.
 * @return Single-use retained indicator recipe; animation uses the receiving scene's scheduler.
 */
inline Description<Spinner> spinner(SpinnerOptions options = {}, ControlStyle style = {}, SizePolicy policy = {},
                                    TextResolver resolver = {}) {
  auto description =
    detail::Access::cold<Spinner>([options = std::move(options), style, policy, resolver = std::move(resolver)](
                                    ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) {
      auto effective = detail::controlSpinnerOptions(*context.controlSettings, options);
      return scene.spinner(parent, std::move(effective), style, policy, resolver);
    });
  return detail::Access::configuredControl(std::move(description), "Spinner");
}

/** @param reference Empty shared observer, published only after complete mount.
 * @param options Owning finite animation configuration.
 * @param style Manual active/dim appearances beneath theme rules.
 * @param policy Existing size constraints.
 * @return Observed retained indicator recipe using the existing scheduler.
 */
inline Description<Spinner> spinner(Ref<Spinner> reference, SpinnerOptions options = {}, ControlStyle style = {},
                                    SizePolicy policy = {}) {
  return spinner(std::move(options), style, policy).reference(std::move(reference));
}

/** @param value Finite value in the inclusive configured range; checked at mount.
 * @param options Owning finite bounds, intrinsic width, glyph mode and allocated-area paint cap.
 * @param style Owning manual appearances beneath theme declarations.
 * @param policy Owning layout constraints, validated by the existing solver.
 * @return Single-use determinate gauge recipe; invalid values/settings preserve typed mount errors.
 */
inline Description<Gauge> gauge(double value, GaugeOptions options = {}, ControlStyle style = {},
                                SizePolicy policy = {}) {
  auto description = detail::Access::cold<Gauge>(
    [value, options, style, policy](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) {
      const auto &settings = *context.controlSettings;
      const auto effective = detail::controlGaugeOptions(settings, options);
      const auto effectiveValue = settings.value.value_or(value);
      const auto valid = Gauge::validate(effectiveValue, effective);

      if (!valid) {
        return std::expected<Node<Gauge>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
      }

      return scene.emplace<Gauge>(parent, effectiveValue, scene.widthPolicy(), effective, style, policy);
    });

  return detail::Access::controlArguments(detail::Access::configuredControl(std::move(description), "Gauge"),
                                          {.value = value});
}

/** @tparam R Exactly a Gauge Ref, deduced so empty braces retain the value-first overload.
 * @param reference Shared weak observer, published only after complete mount.
 * @param value Initial finite value in the inclusive configured range.
 * @param options Owning bounds, width, glyph mode and paint cap.
 * @param style Owning manual appearances beneath theme declarations. @param policy Owning layout constraints.
 * @return Observed determinate gauge recipe; preserves the ordinary gauge's validation and resource settings.
 */
template <class R>
  requires std::same_as<std::remove_cvref_t<R>, Ref<Gauge>>
inline Description<Gauge> gauge(R &&reference, double value, GaugeOptions options = {}, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return gauge(value, options, style, policy).reference(std::forward<R>(reference));
}

} // namespace portals::ui
