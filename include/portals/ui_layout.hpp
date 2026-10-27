// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_construction.hpp>

namespace portals::ui {

inline Description<Overlay> detail::Access::overlayControl(OverlayOptions options, SizePolicy policy) {
  auto description =
    cold<Overlay>([options, policy](ManagedScene &scene, Node<Widget> parent,
                                    MountContext &context) -> std::expected<Node<Overlay>, ManagedError> {
      auto effective = controlOverlayOptions(*context.controlSettings, options);
      if (!effective) {
        return std::unexpected(error(parent, effective.error()));
      }
      return scene.emplace<Overlay>(parent, *effective, policy);
    });
  return configuredControl(std::move(description), "Overlay");
}

/** @tparam Children Consuming ordinary child recipes, including an empty pack.
 * @param options Actual owning ordinary Overlay insets/alignment.
 * @param policy Actual parent size policy, retained beneath explicit sizing modifiers.
 * @param children Real children mounted in their original sibling paint/hit order.
 * @return One native Overlay with transactional cleanup and deferred reference publication.
 */
template <detail::DescriptionInput... Children>
Description<Overlay> overlay(OverlayOptions options, SizePolicy policy, Children &&...children) {
  auto description = detail::Access::overlayControl(options, policy);
  (detail::Access::layoutChild(description, children), ...);
  (description.state_->children.push_back(
     [child =
        std::forward<Children>(children)](ManagedScene &scene, Node<Widget> parent,
                                          detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
       auto mounted = detail::Access::mount(std::move(child), scene, parent, context);
       return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
     }),
   ...);
  return description;
}

/** @tparam Children Consuming ordinary child recipes.
 * @param options Actual owning Overlay settings; absent sparse fields retain these values.
 * @param children Real children in original order.
 * @return Native Overlay recipe using the existing default SizePolicy.
 */
template <detail::DescriptionInput... Children>
Description<Overlay> overlay(OverlayOptions options, Children &&...children) {
  return overlay(options, SizePolicy{}, std::forward<Children>(children)...);
}

/** @tparam Children Consuming ordinary child recipes, including none.
 * @param children Real child edges in original order.
 * @return Native Overlay with existing default options and size policy.
 */
template <detail::DescriptionInput... Children> Description<Overlay> overlay(Children &&...children) {
  return overlay(OverlayOptions{}, SizePolicy{}, std::forward<Children>(children)...);
}

/** @tparam Children Consuming ordinary child recipes.
 * @param reference Empty observing slot published only after complete mount.
 * @param options Actual owning Overlay layout options.
 * @param policy Actual parent size policy.
 * @param children Real child edges in original order.
 * @return Native Ref-first Overlay recipe using the same checked option path.
 */
template <detail::DescriptionInput... Children>
Description<Overlay> overlay(Ref<Overlay> reference, OverlayOptions options, SizePolicy policy,
                             Children &&...children) {
  return overlay(options, policy, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming ordinary child recipes.
 * @param reference Empty observing slot.
 * @param options Actual owning Overlay layout options.
 * @param children Real child edges in original order.
 * @return Native Ref-first Overlay using the existing default SizePolicy.
 */
template <detail::DescriptionInput... Children>
Description<Overlay> overlay(Ref<Overlay> reference, OverlayOptions options, Children &&...children) {
  return overlay(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming ordinary child recipes.
 * @param reference Empty observing slot.
 * @param children Real child edges in original order.
 * @return Native Ref-first Overlay using existing default options and size policy.
 */
template <detail::DescriptionInput... Children>
Description<Overlay> overlay(Ref<Overlay> reference, Children &&...children) {
  return overlay(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @brief Compose a retained linear layout with consuming child recipes.
 * @tparam T Existing Row or Column subtype accepting LinearOptions.
 * @tparam Children Description types, passed as rvalues.
 * @param options Owning layout settings, validated by the existing layout engine.
 * @param children Owning recipes, mounted in argument order after the parent setup.
 * @return Single-use parent recipe; no second runtime or retained description tree is created.
 */
template <class T, detail::DescriptionInput... Children>
Description<T> linear(LinearOptions options, Children &&...children) {
  auto description = detail::Access::configuredControl(
    detail::Access::cold<T>([options](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) mutable {
      if (context.controlSettings) {
        options = detail::controlLinearOptions(*context.controlSettings, options);
      }
      return scene.emplace<T>(parent, options);
    }),
    detail::Access::controlType<T>());

  (detail::Access::layoutChild(description, children), ...);

  (description.state_->children.push_back(
     [child =
        std::forward<Children>(children)](ManagedScene &scene, Node<Widget> parent,
                                          detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
       auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

       return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
     }),
   ...);

  return detail::Access::controlArguments(std::move(description), {.gap = options.gap});
}

/** @brief Immediately consume a range into the existing linear-layout mount transaction.
 * @tparam T Existing Row or Column subtype accepting LinearOptions.
 * @tparam R Input range of one Description type; elements must be rvalues, or R an rvalue non-view, non-borrowed
 * container.
 * @param options Owning layout settings, validated by the existing layout engine.
 * @param children Recipes consumed once, in iteration order, before this function returns.
 * @return Single-use parent owning every recipe; an empty range creates an empty layout.
 * Use views::as_rvalue for explicit transfer from lvalue containers. Borrowed lvalue elements,
 * including those exposed by temporary views, are rejected. No iterator or view is retained.
 * Custom non-view, non-borrowed ranges must own their elements; that ownership cannot be inferred.
 * Construction/iteration may throw; already transferred source elements are not restored.
 * Mount preserves the original child error, subtree rollback and deferred reference publication.
 */
template <class T, detail::DescriptionRange R> Description<T> linear(LinearOptions options, R &&children) {
  auto description = detail::Access::configuredControl(
    detail::Access::cold<T>([options](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) mutable {
      if (context.controlSettings) {
        options = detail::controlLinearOptions(*context.controlSettings, options);
      }
      return scene.emplace<T>(parent, options);
    }),
    detail::Access::controlType<T>());

  for (auto &&child : children) {
    detail::Access::layoutChild(description, child);
    description.state_->children.push_back(
      [child = std::move(child)](ManagedScene &scene, Node<Widget> parent,
                                 detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
        auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      });
  }

  return detail::Access::controlArguments(std::move(description), {.gap = options.gap});
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param options Owning horizontal layout settings.
 * @param children Recipes transferred now, in left-to-right order; lvalue elements require explicit transfer.
 * @return Owning row recipe with transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Row> row(LinearOptions options, R &&children) {
  return linear<Row>(options, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param children Recipes transferred now, in left-to-right order; lvalue elements require explicit transfer.
 * @return Owning row recipe with default settings and transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Row> row(R &&children) {
  return detail::Access::controlArguments(linear<Row>({}, std::forward<R>(children)), {});
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param options Owning vertical layout settings.
 * @param children Recipes transferred now, in top-to-bottom order; lvalue elements require explicit transfer.
 * @return Owning column recipe with transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Column> column(LinearOptions options, R &&children) {
  return linear<Column>(options, std::forward<R>(children));
}

/** @tparam R Immediately consumable range of one Description type, as required by linear.
 * @param children Recipes transferred now, in top-to-bottom order; lvalue elements require explicit transfer.
 * @return Owning column recipe with default settings and transactional child mounting.
 */
template <detail::DescriptionRange R> Description<Column> column(R &&children) {
  return detail::Access::controlArguments(linear<Column>({}, std::forward<R>(children)), {});
}

/** @tparam Children Rvalue Description types.
 * @param options Owning horizontal layout settings.
 * @param children Owning child recipes in left-to-right order.
 * @return Single-use row recipe.
 */
template <detail::DescriptionInput... Children> Description<Row> row(LinearOptions options, Children &&...children) {
  return linear<Row>(options, std::forward<Children>(children)...);
}

/** @brief Describe a semantic header with independent local rows and shared grid columns.
 * @tparam Children Consuming real child descriptions. @param children Real controls in structural order.
 * @return Owning section; no Widget, focus stop or separate tree is introduced.
 */
template <detail::DescriptionInput... Children> GridSection header(Children &&...children) {
  GridSection section(GridRegion::HEADER);

  (section.children_.push_back(detail::Access::erase(std::forward<Children>(children))), ...);

  return section;
}

/** @brief Describe semantic body rows, sharing their parent's columns with its header.
 * @tparam Children Consuming real child descriptions. @param children Real controls in body structural order.
 * @return Owning section; spans cannot cross the header/body boundary.
 */
template <detail::DescriptionInput... Children> GridSection body(Children &&...children) {
  GridSection section(GridRegion::BODY);

  (section.children_.push_back(detail::Access::erase(std::forward<Children>(children))), ...);

  return section;
}

/** @brief Compose a grid using the existing managed mount transaction.
 * @tparam Children Consuming child Description types, including custom final widgets.
 * @param children Owning recipes mounted in sibling order; no placement is inferred.
 * @return Single-use grid recipe; failed child mount rolls back the whole subtree.
 * columns/rows opt into automatic placement, checked before factories or setup. Without
 * these modifiers, setGridOptions retains the existing explicit full-identity contract.
 * Construction may allocate; mounting retains the existing typed error and cleanup rules.
 */
template <detail::GridInput... Children> Description<Grid> grid(Children &&...children) {
  auto layout = std::make_shared<detail::RecipeLayout>();

  layout->gridContainer = true;

  auto description =
    detail::Access::cold<Grid>([layout](ManagedScene &scene, Node<Widget> parent, detail::MountContext &) {
      if (layout->grid) {
        return scene.emplace<Grid>(parent, detail::resolvedGridOptions(*layout), GridPlacementMode::DECLARATIVE,
                                   SizePolicy{}, layout->scroll, scene.widthPolicy());
      }

      GridOptions options;

      options.presentation = layout->presentation;

      return scene.emplace<Grid>(parent, options);
    });

  description.state_->layout = layout;
  [[maybe_unused]] const auto append = [&](auto &&child) {
    if constexpr (std::same_as<std::remove_cvref_t<decltype(child)>, GridSection>) {
      detail::Access::gridSection(description, std::forward<decltype(child)>(child));
    } else {
      ++layout->directItems;
      detail::Access::gridChild(description, std::forward<decltype(child)>(child));
    }
  };

  (append(std::forward<Children>(children)), ...);

  return description;
}

/** @tparam Children Rvalue Description types.
 * @param children Owning child recipes in left-to-right order.
 * @return Single-use row recipe with default layout settings.
 */
template <detail::DescriptionInput... Children> Description<Row> row(Children &&...children) {
  return detail::Access::controlArguments(linear<Row>({}, std::forward<Children>(children)...), {});
}

/** @tparam Children Rvalue Description types.
 * @param options Owning vertical layout settings.
 * @param children Owning child recipes in top-to-bottom order.
 * @return Single-use column recipe.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(LinearOptions options, Children &&...children) {
  return linear<Column>(options, std::forward<Children>(children)...);
}

/** @tparam Children Rvalue Description types.
 * @param children Owning child recipes in top-to-bottom order.
 * @return Single-use column recipe with default layout settings.
 */
template <detail::DescriptionInput... Children> Description<Column> column(Children &&...children) {
  return detail::Access::controlArguments(linear<Column>({}, std::forward<Children>(children)...), {});
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty layout slot published after complete mount.
 * @param children Owning child recipes.
 * @return Existing column recipe with one additional observation.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(Ref<Column> reference, Children &&...children) {
  return column(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty layout slot published after complete mount.
 * @param children Owning child recipes.
 * @return Existing row recipe with one additional observation.
 */
template <detail::DescriptionInput... Children> Description<Row> row(Ref<Row> reference, Children &&...children) {
  return row(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes, mounted in argument order.
 * @param reference Empty grid slot published after complete mount.
 * @param children Owning child recipes; placement remains explicit after mount.
 * @return Existing grid recipe with one additional observation.
 */
template <detail::GridInput... Children> Description<Grid> grid(Ref<Grid> reference, Children &&...children) {
  return grid(std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes.
 * @param reference Empty row observation published after complete mount.
 * @param options Existing owning linear layout options.
 * @param children Owning child recipes, mounted in argument order.
 * @return Existing row recipe retaining its layout configuration.
 */
template <detail::DescriptionInput... Children>
Description<Row> row(Ref<Row> reference, LinearOptions options, Children &&...children) {
  return row(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Children Consuming child recipes.
 * @param reference Empty column observation published after complete mount.
 * @param options Existing owning linear layout options.
 * @param children Owning child recipes, mounted in argument order.
 * @return Existing column recipe retaining its layout configuration.
 */
template <detail::DescriptionInput... Children>
Description<Column> column(Ref<Column> reference, LinearOptions options, Children &&...children) {
  return column(options, std::forward<Children>(children)...).reference(std::move(reference));
}

/** @tparam Args Arguments accepted by an existing row overload, including references and layout options.
 * @param args Owning child recipes and optional row configuration, forwarded unchanged.
 * @return The existing horizontal row recipe; ownership, checked failures and layout semantics are unchanged.
 */
template <class... Args>
  requires requires(Args &&...args) { row(std::forward<Args>(args)...); }
auto hbox(Args &&...args) {
  return row(std::forward<Args>(args)...);
}

/** @tparam Args Arguments accepted by an existing column overload, including references and layout options.
 * @param args Owning child recipes and optional column configuration, forwarded unchanged.
 * @return The existing vertical column recipe; ownership, checked failures and layout semantics are unchanged.
 */
template <class... Args>
  requires requires(Args &&...args) { column(std::forward<Args>(args)...); }
auto vbox(Args &&...args) {
  return column(std::forward<Args>(args)...);
}

/** @brief Owning exact ordinary Overlay layout stage; derived OverlayHost is deliberately excluded. */
struct OverlayLayoutModifier {
  std::optional<LayoutPadding> padding;  ///< Complete nonnegative shared canonical insets.
  std::optional<LayoutAlign> horizontal; ///< Ordinary Overlay column alignment.
  std::optional<LayoutAlign> vertical;   ///< Ordinary Overlay row alignment.

  /** @param description Consumed exact Overlay recipe, including notification compositions.
   * @param modifier Owning sparse ordinary layout values.
   * @return Recipe staging the same checked leaves as its compact methods.
   */
  friend Description<Overlay> operator|(Description<Overlay> &&description, OverlayLayoutModifier modifier) {
    if (modifier.padding) {
      (void)std::move(description).padding(*modifier.padding);
    }
    if (modifier.horizontal) {
      (void)std::move(description).horizontalAlign(*modifier.horizontal);
    }
    if (modifier.vertical) {
      (void)std::move(description).verticalAlign(*modifier.vertical);
    }
    return std::move(description);
  }
};

/** @param insets Nonnegative ordinary Overlay content insets.
 * @return Owning exact Overlay stage; containing Grid relationships remain independent.
 */
inline OverlayLayoutModifier padding(LayoutPadding insets) {
  return {insets, {}, {}};
}

/** @param align Ordinary Overlay alignment across columns.
 * @return Owning exact Overlay stage, checked before native construction.
 */
inline OverlayLayoutModifier horizontalAlign(LayoutAlign align) {
  return {{}, align, {}};
}

/** @param align Ordinary Overlay alignment across rows.
 * @return Owning exact Overlay stage; derived OverlayHost policies are unchanged.
 */
inline OverlayLayoutModifier verticalAlign(LayoutAlign align) {
  return {{}, {}, align};
}

/** @brief Owning common scroll-input pipe stage using the native sparse settings path. */
struct ScrollInputModifier {
  ControlSettings settings; ///< Sparse wheel/key/page/thumb/focus overrides; no controller is retained.

  /** @tparam T Exactly native ScrollArea or ScrollBar.
   * @param description Consumed native recipe.
   * @param modifier Owning sparse input values.
   * @return Recipe staging the same checked properties as its compact methods.
   */
  template <class T>
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  friend Description<T> operator|(Description<T> &&description, ScrollInputModifier modifier) {
    if (modifier.settings.wheelStep) {
      (void)std::move(description).wheelStep(*modifier.settings.wheelStep);
    }
    if (modifier.settings.keyStep) {
      (void)std::move(description).keyStep(*modifier.settings.keyStep);
    }
    if (modifier.settings.pageOverlap) {
      (void)std::move(description).pageOverlap(*modifier.settings.pageOverlap);
    }
    if (modifier.settings.arrows) {
      (void)std::move(description).arrows(*modifier.settings.arrows);
    }
    if (modifier.settings.minimumThumb) {
      (void)std::move(description).minimumThumb(*modifier.settings.minimumThumb);
    }
    if (modifier.settings.focusable) {
      (void)std::move(description).focusable(*modifier.settings.focusable);
    }

    return std::move(description);
  }
};

/** @param cells Positive finite logical wheel step.
 * @return Owning native area/bar input stage.
 */
inline ScrollInputModifier wheelStep(double cells) {
  return {{.wheelStep = cells}};
}

/** @param cells Positive exact 64-bit logical key step.
 * @return Owning native area/bar input stage without platform-size narrowing.
 */
inline ScrollInputModifier keyStep(std::uint64_t cells) {
  return {{.keyStep = cells}};
}

/** @param cells Exact 64-bit page overlap, including zero.
 * @return Owning native area/bar input stage.
 */
inline ScrollInputModifier pageOverlap(std::uint64_t cells) {
  return {{.pageOverlap = cells}};
}

/** @param enabled Reserve functional arrow buttons when the actual channel fits.
 * @return Owning native area/bar stage with automatic managed hold support.
 */
inline ScrollInputModifier arrows(bool enabled = true) {
  return {{.arrows = enabled}};
}

/** @param cells Positive minimum thumb length in cells.
 * @return Owning native area/bar input stage.
 */
inline ScrollInputModifier minimumThumb(std::int32_t cells) {
  return {{.minimumThumb = cells}};
}

/** @param enabled Whether the native area/standalone bar participates in focus traversal.
 * @return Owning native area/bar input stage.
 */
inline ScrollInputModifier focusable(bool enabled = true) {
  return {{.focusable = enabled}};
}

/** @brief Owning skin modifier shared by ordinary ScrollArea and ScrollBar. */
struct ScrollbarModifier {
  ScrollbarSkin skin;                       ///< Owned glyph and preparation policy.
  std::optional<ScrollbarPattern> preset{}; ///< Preset-only stage preserves omitted context leaves.

  /** @tparam T Exactly ScrollArea or ScrollBar. @param description Consumed recipe.
   * @param modifier Owning skin. @return Recipe with equivalent method configuration.
   */
  template <class T>
    requires(std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>)
  friend Description<T> operator|(Description<T> &&description, ScrollbarModifier modifier) {
    if (modifier.preset) {
      return std::move(description).scrollbar(*modifier.preset);
    }

    return std::move(description).scrollbar(std::move(modifier.skin));
  }
};

/** @param pattern Borrowed legacy/frame/minimal/ascii preset. @return Owning scroll skin stage. */
inline ScrollbarModifier scrollbar(std::string_view pattern) {
  return {{}, detail::scrollbarPattern(pattern)};
}

/** @param skin Owning custom glyph source. @return Owning scroll skin stage, checked at mount. */
inline ScrollbarModifier scrollbar(ScrollbarSkin skin) {
  return {std::move(skin)};
}

/** @brief Owning ScrollArea-only placement/frame pipe stage; frame leaves never enable a frame implicitly. */
struct ScrollAreaModifier {
  ControlSettings settings; ///< Sparse native area placement and existing one-cell frame leaves.

  /** @param description Consumed native area recipe.
   * @param modifier Owning sparse placement/frame values.
   * @return Recipe staging the same leaves as its compact methods.
   */
  friend Description<ScrollArea> operator|(Description<ScrollArea> &&description, ScrollAreaModifier modifier) {
    if (modifier.settings.horizontal) {
      (void)std::move(description).horizontal(*modifier.settings.horizontal);
    }
    if (modifier.settings.vertical) {
      (void)std::move(description).vertical(*modifier.settings.vertical);
    }
    if (modifier.settings.placement) {
      (void)std::move(description).placement(*modifier.settings.placement);
    }
    if (modifier.settings.frame.enabled) {
      (void)std::move(description).frameEnabled(*modifier.settings.frame.enabled);
    }
    if (modifier.settings.frame.glyphs) {
      (void)std::move(description).frameGlyphs(*modifier.settings.frame.glyphs);
    }
    if (modifier.settings.frame.maxPaintCells) {
      (void)std::move(description).frameMaxPaintCells(*modifier.settings.frame.maxPaintCells);
    }

    return std::move(description);
  }
};

/** @param policy Horizontal bar presence policy.
 * @return Owning native area-only stage.
 */
inline ScrollAreaModifier horizontal(ScrollBarPolicy policy) {
  return {{.horizontal = policy}};
}

/** @param policy Vertical bar presence policy.
 * @return Owning native area-only stage.
 */
inline ScrollAreaModifier vertical(ScrollBarPolicy policy) {
  return {{.vertical = policy}};
}

/** @param placement Existing gutter/overlay/frame placement.
 * @return Owning native area-only stage checked against its actual frame options.
 */
inline ScrollAreaModifier placement(ScrollPlacement placement) {
  return {{.placement = placement}};
}

/** @return Native area-only stage placing bars on an existing frame; a missing frame stays a typed error. */
inline ScrollAreaModifier scrollOnFrame() {
  return placement(ScrollPlacement::ON_FRAME);
}

/** @param enabled True creates a missing default one-cell frame; false removes it.
 * @return Owning native area-only stage.
 */
inline ScrollAreaModifier frameEnabled(bool enabled = true) {
  return {{.frame = {.enabled = enabled}}};
}

/** @param glyphs One-cell glyph mode for an effective enabled frame.
 * @return Owning native area-only stage, without implicit frame creation.
 */
inline ScrollAreaModifier frameGlyphs(PrimitiveGlyphs glyphs) {
  return {{.frame = {.glyphs = glyphs}}};
}

/** @param cells Positive paint cap for an effective enabled one-cell frame.
 * @return Owning native area-only stage, without implicit frame creation.
 */
inline ScrollAreaModifier frameMaxPaintCells(std::size_t cells) {
  return {{.frame = {.maxPaintCells = cells}}};
}

/** @brief Owning bar-axis stage; projection/writer/controller ownership is not changed. */
struct ScrollAxisModifier {
  ScrollAxis value; ///< Effective native bar axis.

  /** @param description Consumed native bar recipe.
   * @param modifier Owning axis override.
   * @return Recipe staging its existing projection-axis constructor input.
   */
  friend Description<ScrollBar> operator|(Description<ScrollBar> &&description, ScrollAxisModifier modifier) {
    return std::move(description).axis(modifier.value);
  }
};

/** @param axis Effective native bar projection axis.
 * @return Owning bar-only sparse axis stage.
 */
inline ScrollAxisModifier axis(ScrollAxis axis) {
  return {axis};
}

inline std::expected<void, ManagedError> detail::Access::applyScrollbarSkin(ManagedScene &scene, Node<Widget> node,
                                                                            const ControlSettings &settings) {
  if (!hasScrollbarSettings(settings.scrollbar)) {
    return {};
  }

  try {
    const auto &source = settings.scrollbar;
    ScrollbarSkin skin{.pattern = source.pattern.value_or(ScrollbarPattern::LEGACY),
                       .ends = source.ends.value_or(false),
                       .maxGlyphBytes = source.maxGlyphBytes.value_or(DEFAULT_SCROLLBAR_GLYPH_BYTES),
                       .horizontal = source.horizontal,
                       .vertical = source.vertical,
                       .corner = source.corner};
    auto applied = setScrollbarSkin(scene.app_.tree(), node, skin);

    return applied ? std::expected<void, ManagedError>{} : std::unexpected(error(node, applied.error()));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(node, TreeError::NO_MEMORY));
  }
}

inline Description<ScrollArea> detail::Access::scrollAreaControl(ScrollAreaOptions options,
                                                                 std::shared_ptr<ScrollController> controller,
                                                                 ControlStyle style) {
  auto description = cold<ScrollArea>([options, controller = std::move(controller),
                                       style](ManagedScene &scene, Node<Widget> parent, MountContext &context) {
    const auto finish = [&](std::expected<Node<ScrollArea>, ManagedError> node) {
      if (!node)
        return node;
      auto applied = applyScrollbarSkin(scene, *node, *context.controlSettings);
      return applied ? node : std::expected<Node<ScrollArea>, ManagedError>(std::unexpected(applied.error()));
    };
    auto baseOptions = options;

    if (context.frameOptions) {
      baseOptions.frame = context.frameOptions->border;
    }

    auto effective = controlScrollAreaOptions(*context.controlSettings, baseOptions);

    if (!effective) {
      return std::expected<Node<ScrollArea>, ManagedError>(std::unexpected(error(parent, effective.error())));
    }

    const bool advanced =
      context.frameOptions || context.frameSettings || hasFrameSettings(context.controlSettings->frame);

    if (advanced) {
      std::optional<FrameOptions> base;

      if (context.frameOptions) {
        base = *context.frameOptions;
      } else if (effective->frame) {
        base.emplace();
        base->border = *effective->frame;
      }

      auto framed = controlFrameOptions(*context.controlSettings, std::move(base), scene.widthPolicy());

      if (!framed) {
        return std::expected<Node<ScrollArea>, ManagedError>(std::unexpected(framed.error()));
      }

      if (context.frameSettings) {
        framed = controlFrameOptions(*context.frameSettings, std::move(*framed), scene.widthPolicy());

        if (!framed) {
          return std::expected<Node<ScrollArea>, ManagedError>(std::unexpected(framed.error()));
        }
      }

      if (*framed) {
        return finish(scene.emplace<ScrollArea>(parent, controller, scene.widthPolicy(), *effective, style,
                                                SizePolicy{}, **framed, std::in_place));
      }

      effective->frame.reset();
    }

    return finish(scene.emplace<ScrollArea>(parent, controller, scene.widthPolicy(), *effective, style));
  });

  return configuredControl(std::move(description), "ScrollArea");
}

inline Description<ScrollBar> detail::Access::scrollBarControl(std::shared_ptr<ScrollController> controller,
                                                               ScrollAxis axis, ScrollBarOptions options,
                                                               ControlStyle style, bool writer) {
  auto description = cold<ScrollBar>([controller = std::move(controller), axis, options, style,
                                      writer](ManagedScene &scene, Node<Widget> parent,
                                              MountContext &context) -> std::expected<Node<ScrollBar>, ManagedError> {
    if (!writer && !controller) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }

    auto effective = controlScrollBarOptions(*context.controlSettings, options);
    const auto effectiveAxis = context.controlSettings->axis.value_or(axis);

    if (!effective) {
      return std::unexpected(error(parent, effective.error()));
    }

    if (effectiveAxis != ScrollAxis::HORIZONTAL && effectiveAxis != ScrollAxis::VERTICAL) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }

    auto node = scene.scrollBar(parent, controller, effectiveAxis, *effective, style, writer);

    if (!node)
      return node;

    auto applied = applyScrollbarSkin(scene, *node, *context.controlSettings);

    return applied ? node : std::expected<Node<ScrollBar>, ManagedError>(std::unexpected(applied.error()));
  });

  return configuredControl(std::move(description), "ScrollBar");
}

template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Child &&child, ScrollAreaOptions options,
                                   std::shared_ptr<ScrollController> controller, ControlStyle style) {
  auto description = detail::Access::scrollAreaControl(options, std::move(controller), style);
  detail::Access::layoutChild(description, child);
  description.state_->children.push_back(
    [child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> parent,
                                         detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
      auto mounted = detail::Access::mount(std::move(child), scene, parent, context);
      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    });
  return description;
}

/** @tparam Child Consuming ordinary content recipe.
 * @param reference Empty area slot, published only after the complete subtree mounts.
 * @param child Single ordinary content root.
 * @param options Owning placement/input settings.
 * @param controller Optional owning shared values.
 * @param style Manual appearance defaults.
 * @return Ref-first recipe with automatic lifecycle and checked rollback.
 */
template <detail::DescriptionInput Child>
Description<ScrollArea> scrollArea(Ref<ScrollArea> reference, Child &&child, ScrollAreaOptions options = {},
                                   std::shared_ptr<ScrollController> controller = {}, ControlStyle style = {}) {
  return scrollArea(std::forward<Child>(child), options, std::move(controller), style).reference(std::move(reference));
}

/** @param axis Projected axis.
 * @param options Owning input/paint settings.
 * @param style Manual defaults beneath themes.
 * @return Standalone bar with automatic default controller and sole metrics-writer lease.
 */
inline Description<ScrollBar> scrollBar(ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return detail::Access::scrollBarControl({}, axis, options, style, true);
}

/** @param controller Owning values shared with an area's metrics writer.
 * @param axis Projected axis.
 * @param options Owning input/paint settings.
 * @param style Manual defaults beneath themes.
 * @return Read projection; a missing controller is rejected rather than creating orphan metrics.
 */
inline Description<ScrollBar> scrollBar(std::shared_ptr<ScrollController> controller,
                                        ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return detail::Access::scrollBarControl(std::move(controller), axis, options, style, false);
}

/** @param reference Empty bar observer published after successful mount.
 * @param axis Selected standalone axis.
 * @param options Owning bar settings.
 * @param style Manual defaults.
 * @return Ref-first standalone writer recipe.
 */
inline Description<ScrollBar> scrollBar(Ref<ScrollBar> reference, ScrollAxis axis = ScrollAxis::VERTICAL,
                                        ScrollBarOptions options = {}, ControlStyle style = {}) {
  return scrollBar(axis, options, style).reference(std::move(reference));
}

/** @param reference Empty observer published after successful mount.
 * @param controller Owning area-shared values.
 * @param axis Selected projection axis.
 * @param options Owning bar settings.
 * @param style Manual defaults.
 * @return Ref-first area projection recipe with automatic binding and cleanup.
 */
inline Description<ScrollBar> scrollBar(Ref<ScrollBar> reference, std::shared_ptr<ScrollController> controller,
                                        ScrollAxis axis = ScrollAxis::VERTICAL, ScrollBarOptions options = {},
                                        ControlStyle style = {}) {
  return scrollBar(std::move(controller), axis, options, style).reference(std::move(reference));
}

template <class T> void detail::Access::frameSettings(Description<T> &description, const ControlSettings &settings) {
  if (!description.state_ || !hasFrameSettings(settings.frame)) {
    return;
  }

  auto &frame = description.frameRecipe();

  frame.source = std::make_unique<ControlSettings>();
  frame.source->frame = settings.frame;
  frame.source->origin = settings.origin;

  for (const auto &origin : settings.origins) {
    if (origin.path.starts_with("frame.")) {
      frame.source->origins.push_back(origin);
    }
  }
}

inline Description<Border> detail::Access::framedControl(FrameOptions options, ControlStyle style) {
  return cold<Border>([options = std::move(options), style](ManagedScene &scene, Node<Widget> parent, MountContext &) {
    const auto valid = Border::validateFrame(options);

    if (!valid) {
      return std::expected<Node<Border>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
    }

    auto node = scene.emplace<Border>(parent, scene.widthPolicy(), options, style, SizePolicy{}, std::in_place);

    if (!node) {
      return node;
    }

    std::expected<void, TreeError> prepared;
    const auto inspected = scene.app_.tree().read(*node, [&](const Border &frame) {
      prepared = frame.preparation();
    });

    if (!inspected || !prepared) {
      return std::expected<Node<Border>, ManagedError>(
        scene.rollback(*node, inspected ? prepared.error() : inspected.error()));
    }

    return node;
  });
}

inline Description<Border> detail::Access::borderControl(BorderOptions options, ControlStyle style) {
  auto description = cold<Border>([options, style](ManagedScene &scene, Node<Widget> parent, MountContext &context) {
    const auto effective = controlBorderOptions(*context.controlSettings, options);
    const auto valid = Border::validate(effective);

    if (!valid) {
      return std::expected<Node<Border>, ManagedError>(std::unexpected(detail::error(parent, valid.error())));
    }

    return scene.emplace<Border>(parent, scene.widthPolicy(), effective, style);
  });

  return configuredControl(std::move(description), "Border");
}

template <detail::DescriptionInput Child>
Description<Border> border(Child &&child, BorderOptions options, ControlStyle style) {
  auto description = detail::Access::borderControl(options, style);

  detail::Access::layoutChild(description, child);
  description.state_->children.push_back(
    [child = std::forward<Child>(child)](ManagedScene &scene, Node<Widget> parent,
                                         detail::MountContext &context) mutable -> std::expected<void, ManagedError> {
      const auto mounted = detail::Access::mount(std::move(child), scene, parent, context);

      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    });

  return description;
}

/** @tparam Child Consuming Description type.
 * @param child Owning child mounted within the frame transaction.
 * @param options Owning frame glyph/paint-work policy.
 * @param style Owning manual appearances beneath themes.
 * @return Same owning frame recipe as border, with identical layout and failure semantics.
 */
template <detail::DescriptionInput Child>
Description<Border> frame(Child &&child, BorderOptions options = {}, ControlStyle style = {}) {
  return border(std::forward<Child>(child), options, style);
}

/** @brief Owning frame pipe stage over the same transactional border factory. */
struct BorderModifier {
  BorderOptions options; ///< Owning glyph/work settings.
  ControlStyle style;    ///< Owning manual appearances beneath themes.

  /** @tparam T Widget subtype described by the consuming input.
   * @param description Owning child recipe consumed by the frame.
   * @param modifier Owning glyph, style and finite work settings.
   * @return Owning frame containing the input in the same mount transaction.
   */
  template <class T> friend Description<Border> operator|(Description<T> &&description, BorderModifier modifier) {
    return border(std::move(description), modifier.options, modifier.style);
  }
};

/** @param options Owning frame glyph selection and finite paint cap.
 * @param style Owning manual appearances beneath themes.
 * @return Frame stage for a consuming description; no view/widget borrow is retained.
 */
inline BorderModifier border(BorderOptions options = {}, ControlStyle style = {}) {
  return {options, style};
}

/** @param options Owning frame glyph selection and finite paint cap.
 * @param style Owning manual appearances beneath themes.
 * @return The same frame stage as border(options, style).
 */
inline BorderModifier frame(BorderOptions options = {}, ControlStyle style = {}) {
  return {options, style};
}

/** @brief Owning title-decoration pipe stage, sharing method validation and lifetime. */
struct TitleDecorationModifier {
  TitleDecoration preset{TitleDecoration::NONE}; ///< Checked preset.
  std::optional<CustomTitleDecoration> custom{}; ///< Optional complete custom ends.
  /** @brief Preserve fallible owning transfers. */
  ~TitleDecorationModifier() = default;

  /** @tparam T Sized widget type. @param description Consuming recipe. @param modifier Owning ends.
   * @return Same recipe as titleDecoration method.
   */
  template <class T> friend Description<T> operator|(Description<T> &&description, TitleDecorationModifier modifier) {
    if (modifier.custom) {
      return std::move(description).titleDecoration(*modifier.custom);
    }

    return std::move(description).titleDecoration(modifier.preset);
  }
};

/** @param preset Title end preset. @return Owning pipe modifier. */
inline TitleDecorationModifier titleDecoration(TitleDecoration preset) {
  return {preset, {}};
}

/** @param name none/spaced/square/angle/round. @return Pipe modifier; unknown names fail at mount. */
inline TitleDecorationModifier titleDecoration(std::string_view name) {
  return {detail::titleDecorationPreset(name), {}};
}

/** @param custom Owning complete ends. @return Pipe modifier with checked mount validation. */
inline TitleDecorationModifier titleDecoration(CustomTitleDecoration custom) {
  return {TitleDecoration::NONE, custom};
}

/** @brief Owning compact decoration stage; all text is retained until checked mount. */
struct FramedModifier {
  /** @brief Keep owning transfers fallible on checked standard-library implementations. */
  ~FramedModifier() = default;

  FrameOptions options;               ///< Owning settings, including the title for a compact pattern.
  std::optional<std::string> pattern; ///< Owning compact name, checked by the consuming recipe.

  /** @tparam T Sized widget whose source identity remains unchanged.
   * @param description Consuming child recipe.
   * @param modifier Owning frame settings.
   * @return Same typed recipe, with an outer frame or the native ScrollArea frame configured.
   */
  template <class T>
    requires std::derived_from<T, SizedWidget>
  friend Description<T> operator|(Description<T> &&description, FramedModifier modifier) {
    if (modifier.pattern) {
      return std::move(description).framed(*modifier.pattern, modifier.options.title);
    }

    return std::move(description).framed(std::move(modifier.options));
  }
};

/** @brief Advanced decoration stage with an explicitly observed retained Border owner. */
struct ObservedFramedModifier {
  /** @brief Preserve fallible owning transfers alongside the observing reference. */
  ~ObservedFramedModifier() = default;

  Ref<Border> reference; ///< Empty observer published only when the whole mount succeeds.
  FramedModifier frame;  ///< Owning compact or advanced frame configuration.

  /** @tparam T Sized source widget other than ScrollArea, which owns its frame directly.
   * @param description Consuming source recipe; its Ref and handlers continue to identify T.
   * @param modifier Owning configuration and observing outer slot.
   * @return Same typed source recipe; the separate Border Ref observes outer geometry and removal.
   */
  template <class T>
    requires(std::derived_from<T, SizedWidget> && !std::same_as<T, ScrollArea>)
  friend Description<T> operator|(Description<T> &&description, ObservedFramedModifier modifier) {
    if (modifier.frame.pattern) {
      return std::move(description)
        .framed(std::move(modifier.reference), *modifier.frame.pattern, modifier.frame.options.title);
    }

    return std::move(description).framed(std::move(modifier.reference), std::move(modifier.frame.options));
  }
};

/** @param options Owning advanced frame settings; default is a single full outline.
 * @return Stage preserving the original typed source recipe and checked mount/cleanup boundary.
 */
inline FramedModifier framed(FrameOptions options = {}) {
  return {std::move(options), {}};
}

/** @param pattern Compact single/plain, rounded, double, heavy or ascii spelling.
 * @return Owning stage; invalid names are reported by prepare/runApp before this control's setup.
 */
inline FramedModifier framed(std::string_view pattern) {
  return {{}, std::optional<std::string>(std::in_place, pattern)};
}

/** @param pattern Owning compact name, checked at mount.
 * @param title UTF-8 top-edge title copied now and checked with the existing frame limits at mount.
 * @return Owning frame stage preserving the source recipe's identity and outer dimensions.
 */
inline FramedModifier framed(std::string_view pattern, std::string_view title) {
  auto modifier = framed(pattern);

  modifier.options.title = title;

  return modifier;
}

/** @param reference Empty observer for explicit outer geometry, visibility and removal.
 * @param options Owning advanced frame settings.
 * @return Stage for ordinary sized source recipes; ScrollArea keeps its own existing owner instead.
 */
inline ObservedFramedModifier framed(Ref<Border> reference, FrameOptions options = {}) {
  return {std::move(reference), framed(std::move(options))};
}

/** @param reference Empty observer for the separate retained Border.
 * @param pattern Checked compact preset spelling, copied by the returned stage.
 * @return Stage preserving the source Ref and publishing the outer Ref only after success.
 */
inline ObservedFramedModifier framed(Ref<Border> reference, std::string_view pattern) {
  return {std::move(reference), framed(pattern)};
}

/** @param reference Empty observer published only after complete mount success.
 * @param pattern Checked compact preset name.
 * @param title Owning top-edge title checked at mount.
 * @return Frame stage with the existing outer Border publication contract.
 */
inline ObservedFramedModifier framed(Ref<Border> reference, std::string_view pattern, std::string_view title) {
  return {std::move(reference), framed(pattern, title)};
}

} // namespace portals::ui
