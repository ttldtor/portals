// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "glyph_profiles.hpp"
#include "scroll_controls_state.hpp"
#include <portals/grid.hpp>

#include <limits>
#include <stdexcept>

namespace portals {
namespace {
GraphemeText scrollGlyph(std::string_view unicode, std::string_view ascii, WidthPolicy width, PrimitiveGlyphs mode) {
  auto text = GraphemeText::from(mode == PrimitiveGlyphs::UNICODE ? unicode : ascii, width).value();
  return text.measure() == 1 ? text : GraphemeText::from(ascii, width).value();
}

std::shared_ptr<const detail::ScrollbarPresetGlyphs> scrollPresetGlyphs(WidthPolicy width) {
  // Keep only the latest policy weakly; live controls own their immutable prepared values.
  thread_local std::weak_ptr<const detail::ScrollbarPresetGlyphs> cached;

  if (auto prepared = cached.lock(); prepared && prepared->front().policy() == width) {
    return prepared;
  }

  auto prepared = std::make_shared<const detail::ScrollbarPresetGlyphs>(
    detail::ScrollbarPresetGlyphs{GraphemeText::from(" ", width).value(), GraphemeText::from(".", width).value(),
                                  GraphemeText::from("#", width).value(), GraphemeText::from("<", width).value(),
                                  GraphemeText::from(">", width).value(), GraphemeText::from("^", width).value(),
                                  GraphemeText::from("v", width).value()});

  // Publish only after all owning text and the shared control block are ready.
  cached = prepared;

  return prepared;
}

template <class Function>
std::expected<void, TreeError> editScroll(WidgetTree &tree, Node<Widget> node, Function function) {
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = detail::ScrollWidgetAccess::state(widget);
    if (!state || !state->lease || state->retired) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    result = function(*state);
  });
  return edited ? result : edited;
}
} // namespace

namespace detail {
ScrollWidgetState::ScrollWidgetState(std::shared_ptr<ScrollController> values, WidthPolicy width, PrimitiveGlyphs mode,
                                     bool arrows)
    : controller(std::move(values)),
      glyphs{scrollGlyph("\xe2\x94\x8c", "+", width, mode), scrollGlyph("\xe2\x94\x90", "+", width, mode),
             scrollGlyph("\xe2\x94\x94", "+", width, mode), scrollGlyph("\xe2\x94\x98", "+", width, mode),
             scrollGlyph("\xe2\x94\x80", "-", width, mode), scrollGlyph("\xe2\x94\x82", "|", width, mode),
             scrollGlyph("\xe2\x96\x91", ".", width, mode), scrollGlyph("\xe2\x96\x88", "#", width, mode)},
      presetGlyphs(scrollPresetGlyphs(width)) {
  if (arrows) {
    arrowGlyphs = std::make_unique<std::array<GraphemeText, 4>>(
      std::array{scrollGlyph("\xe2\x97\x80", "<", width, mode), scrollGlyph("\xe2\x96\xb6", ">", width, mode),
                 scrollGlyph("\xe2\x96\xb2", "^", width, mode), scrollGlyph("\xe2\x96\xbc", "v", width, mode)});
  }
  if (!controller) {
    auto created = ScrollController::create();
    if (!created) {
      throw std::bad_alloc();
    }
    controller = std::move(*created);
  }
}

ScrollWidgetState *ScrollWidgetAccess::state(Widget &widget) {
  if (auto *area = dynamic_cast<ScrollArea *>(&widget)) {
    return &state(*area);
  }
  if (auto *bar = dynamic_cast<ScrollBar *>(&widget)) {
    return &state(*bar);
  }
  if (auto *grid = dynamic_cast<Grid *>(&widget)) {
    return grid->scroll_.get();
  }
  return nullptr;
}

const ScrollWidgetState *ScrollWidgetAccess::state(const Widget &widget) {
  if (const auto *area = dynamic_cast<const ScrollArea *>(&widget)) {
    return &state(*area);
  }
  if (const auto *bar = dynamic_cast<const ScrollBar *>(&widget)) {
    return &state(*bar);
  }
  if (const auto *grid = dynamic_cast<const Grid *>(&widget)) {
    return grid->scroll_.get();
  }
  return nullptr;
}

std::expected<void, TreeError> validateScrollWidget(const ScrollWidgetState &state) {
  if (state.frameError) {
    return std::unexpected(*state.frameError);
  }

  auto area = state.area;

  if (state.advancedFrame) {
    area.frame = state.advancedFrame->options.border;
  }

  auto valid = state.isArea ? validateScrollOptions(area) : validateScrollOptions(state.bar);
  if (!valid) {
    return valid;
  }
  if (state.isArea && !solveScrollLayout({}, {}, scrollLayoutOptions(state))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (state.retired || (!state.isArea && state.axis != ScrollAxis::HORIZONTAL && state.axis != ScrollAxis::VERTICAL)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  auto observed = state.controller->snapshot();
  return observed ? std::expected<void, TreeError>{} : std::unexpected(observed.error());
}

std::expected<bool, TreeError> initializeScrollWidget(WidgetTree &tree, Node<Widget> node, bool writer) {
  std::expected<bool, TreeError> result{false};
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = ScrollWidgetAccess::state(widget);
    if (!state) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    auto valid = validateScrollWidget(*state);
    if (!valid) {
      result = std::unexpected(valid.error());
      return;
    }
    if (state->lease) {
      if (state->node != node.id() || static_cast<bool>(state->lease->writer) != writer) {
        result = std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      return;
    }
    auto lease = ScrollControllerAccess::acquire(state->controller, node.id().session, writer);
    if (!lease) {
      result = std::unexpected(lease.error());
      return;
    }
    // Reentrant allocation can retire this widget. Never publish its newly acquired lease afterward.
    auto live = tree.inspect(node);
    if (!live || state->retired) {
      result = std::unexpected(live ? TreeError::STALE_NODE : live.error());
      return;
    }
    if (state->lease) {
      result = std::unexpected(TreeError::WOULD_BLOCK);
      return;
    }
    state->node = node.id();
    state->lease.emplace(std::move(*lease));
    result = true;
  });
  return edited ? result : std::unexpected(edited.error());
}

std::expected<void, TreeError> releaseScrollInitialization(WidgetTree &tree, Node<Widget> node) {
  auto edited = tree.edit(node, [&](Widget &widget) {
    if (auto *state = ScrollWidgetAccess::state(widget)) {
      state->capture.disconnect();
      cancelScrollRepeat(*state);
      state->lease.reset();
      state->node = {};
    }
  });
  // Removed controls already released their own lease in unmount.
  return !edited && (edited.error() == TreeError::STALE_NODE || edited.error() == TreeError::CLOSED)
           ? std::expected<void, TreeError>{}
           : edited;
}

std::expected<ScrollControllerChange, TreeError> commitScroll(WidgetTree &tree, Node<Widget> node,
                                                              ScrollWidgetState &state, ScrollControllerPlan plan,
                                                              ScrollPublication publication) {
  if (!state.lease || state.retired || state.node != node.id()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (publication == ScrollPublication::POSITION) {
    auto intent = checkScrollIntent(state, plan);
    if (!intent) {
      return std::unexpected(intent.error());
    }
  }
  auto change = ScrollControllerAccess::commit(*state.lease, std::move(plan));
  if (!change) {
    return std::unexpected(change.error());
  }
  if (publication == ScrollPublication::POSITION && change->logicalChanged) {
    recordScrollIntent(state);
  }
  if (change->metricsChanged) {
    state.capture.disconnect();
    cancelScrollRepeat(state);
  }
  if (change->metricsChanged || change->rasterChanged) {
    auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);
    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }
  }
  return change;
}
} // namespace detail

ScrollArea::ScrollArea(std::shared_ptr<ScrollController> controller, WidthPolicy width, ScrollAreaOptions options,
                       ControlStyle style, SizePolicy policy)
    : SizedWidget(policy), state_(std::make_unique<detail::ScrollWidgetState>(std::move(controller), width,
                                                                              options.glyphs, options.arrows)) {
  state_->isArea = true;
  state_->area = std::move(options);
  state_->style = style;
  if (state_->area.frame) {
    state_->legacyFrameOptions.border = *state_->area.frame;

    auto prepared = detail::prepareFrameText(width, state_->legacyFrameOptions);

    if (prepared) {
      state_->legacyFrameText = std::move(*prepared);
    } else {
      state_->frameError = prepared.error();
    }
  }
}

ScrollArea::ScrollArea(std::shared_ptr<ScrollController> controller, WidthPolicy width, ScrollAreaOptions options,
                       ControlStyle style, SizePolicy policy, FrameOptions frame, std::in_place_t)
    : SizedWidget(policy), state_(std::make_unique<detail::ScrollWidgetState>(std::move(controller), width,
                                                                              options.glyphs, options.arrows)) {
  state_->isArea = true;
  state_->area = options;
  state_->style = style;

  auto prepared = detail::prepareFrameText(width, frame);

  if (!prepared) {
    state_->frameError = prepared.error();

    return;
  }

  state_->advancedFrame = std::make_unique<detail::PreparedScrollFrame>(std::move(frame), std::move(*prepared));
  state_->frameError.reset();
}

ScrollArea::~ScrollArea() = default;

void ScrollArea::unmount() {
  detail::cancelScrollRepeat(*state_);
  state_->retired = true;
  state_->intent.reset();
  state_->capture.disconnect();
  state_->lease.reset();
}

ScrollBar::ScrollBar(std::shared_ptr<ScrollController> controller, ScrollAxis axis, WidthPolicy width,
                     ScrollBarOptions options, ControlStyle style, SizePolicy policy)
    : SizedWidget(policy), state_(std::make_unique<detail::ScrollWidgetState>(std::move(controller), width,
                                                                              options.glyphs, options.arrows)) {
  state_->axis = axis;
  state_->bar = options;
  state_->style = style;
}

ScrollBar::~ScrollBar() = default;

void ScrollBar::unmount() {
  detail::cancelScrollRepeat(*state_);
  state_->retired = true;
  state_->intent.reset();
  state_->capture.disconnect();
  state_->lease.reset();
}

std::expected<void, TreeError> initializeScrollArea(WidgetTree &tree, Node<ScrollArea> node) {
  auto result = detail::initializeScrollWidget(tree, node, true);
  return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
}

std::expected<void, TreeError> initializeScrollBar(WidgetTree &tree, Node<ScrollBar> node, bool geometryWriter) {
  auto result = detail::initializeScrollWidget(tree, node, geometryWriter);
  return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
}

std::expected<void, TreeError> setScrollOffset(WidgetTree &tree, Node<Widget> node, ScrollAxis axis,
                                               ScrollOffset offset) {
  return editScroll(tree, node, [&](detail::ScrollWidgetState &state) -> std::expected<void, TreeError> {
    auto plan = detail::ScrollControllerAccess::stage(*state.lease);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto changed = detail::ScrollControllerAccess::offset(*plan, axis, offset);
    if (!changed) {
      return changed;
    }
    auto result = detail::commitScroll(tree, node, state, std::move(*plan));
    return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
  });
}

std::expected<void, TreeError> setScrollMetrics(WidgetTree &tree, Node<ScrollBar> node, ScrollAxisMetrics metrics) {
  return editScroll(tree, node, [&](detail::ScrollWidgetState &state) -> std::expected<void, TreeError> {
    auto plan = detail::ScrollControllerAccess::stage(*state.lease);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto staged = detail::ScrollControllerAccess::metrics(
      *state.lease, *plan, state.axis == ScrollAxis::HORIZONTAL ? metrics : plan->horizontalMetrics,
      state.axis == ScrollAxis::VERTICAL ? metrics : plan->verticalMetrics);
    if (!staged) {
      return staged;
    }
    auto result = detail::commitScroll(tree, node, state, std::move(*plan), detail::ScrollPublication::PREPARATION);
    if (result && result->metricsChanged) {
      state.capture.disconnect();
    }
    return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
  });
}

std::expected<ScrollAreaLayoutSnapshot, TreeError> scrollAreaLayout(WidgetTree &tree, Node<ScrollArea> node) {
  auto inspected = tree.inspect(node);
  if (!inspected) {
    return std::unexpected(inspected.error());
  }
  bool visible = true;
  for (Node<Widget> current = node; current.id().session != 0;) {
    auto ancestor = tree.inspect(current);
    if (!ancestor) {
      return std::unexpected(ancestor.error());
    }
    visible &= ancestor->visible;
    current = ancestor->parent;
  }
  std::expected<ScrollAreaLayoutSnapshot, TreeError> result{std::unexpected(TreeError::INVALID_ARGUMENT)};
  auto read = tree.read(node, [&](const ScrollArea &area) {
    result = detail::ScrollWidgetAccess::snapshot(area, node.id(), inspected->geometry, visible);
  });
  return read ? result : std::unexpected(read.error());
}

std::expected<void, TreeError> prepareScrollArea(WidgetTree &tree, Node<ScrollArea> node,
                                                 const ScrollAreaLayoutSnapshot &layout) {
  return editScroll(tree, node, [&](detail::ScrollWidgetState &state) -> std::expected<void, TreeError> {
    if (layout.node != node.id()) {
      return std::unexpected(layout.node.session != node.id().session ? TreeError::WRONG_SESSION
                                                                      : TreeError::STALE_NODE);
    }
    if (!layout.visible) {
      return {};
    }
    if (!layout.geometry.valid) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    if (layout.layoutRevision != state.layoutRevision) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (layout.extent.width != state.extent.width || layout.extent.height != state.extent.height ||
        layout.geometry.local.size() != state.allocation) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    auto placement = solveScrollLayout(layout.geometry.local.size(), layout.extent, detail::scrollLayoutOptions(state));
    if (!placement || placement->viewport != layout.placement.viewport ||
        placement->viewportOrigin != layout.placement.viewportOrigin) {
      return std::unexpected(placement ? TreeError::INVALID_GEOMETRY : placement.error());
    }
    auto plan = detail::ScrollControllerAccess::stage(*state.lease);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto staged = detail::ScrollControllerAccess::metrics(
      *state.lease, *plan, {layout.extent.width, static_cast<std::uint64_t>(placement->viewport.width)},
      {layout.extent.height, static_cast<std::uint64_t>(placement->viewport.height)});
    if (!staged) {
      return staged;
    }
    const bool trackChanged = !state.adoptedGeometry || !state.adoptedSurface ||
                              *state.adoptedSurface != layout.geometry.surface.origin() ||
                              state.adoptedGeometry->horizontal.origin != placement->horizontal.origin ||
                              state.adoptedGeometry->horizontal.length != placement->horizontal.length ||
                              state.adoptedGeometry->vertical.origin != placement->vertical.origin ||
                              state.adoptedGeometry->vertical.length != placement->vertical.length;
    if (trackChanged) {
      state.capture.disconnect();
    }
    auto changed = detail::commitScroll(tree, node, state, std::move(*plan), detail::ScrollPublication::PREPARATION);
    if (!changed) {
      return std::unexpected(changed.error());
    }
    if (trackChanged || changed->metricsChanged) {
      state.capture.disconnect();
    }
    state.adoptedGeometry = *placement;
    state.adoptedSurface = layout.geometry.surface.origin();
    return {};
  });
}

std::expected<void, TreeError> syncScrollControl(WidgetTree &tree, Node<Widget> node, Node<Widget> focused) {
  auto eligible = tree.inputEligible(node);
  if (!eligible) {
    return std::unexpected(eligible.error());
  }
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = detail::ScrollWidgetAccess::state(widget);
    if (!state) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    bool hasFocus = *eligible && focused == node;

    if (*eligible && state->isArea && focused != Node<Widget>{}) {
      auto inside = tree.contains(node, focused);

      if (!inside) {
        result = std::unexpected(inside.error());
        return;
      }

      hasFocus = *inside;
    }

    if (state->enabled != *eligible || state->focused != hasFocus) {
      result = tree.invalidate(node, Invalidation::PAINT);

      if (result) {
        state->enabled = *eligible;
        state->focused = hasFocus;
      }
    }
    if (!*eligible || !state->capture.connected()) {
      detail::cancelScrollRepeat(*state);
    }
  });
  return edited ? result : edited;
}

namespace {
std::expected<std::unique_ptr<detail::PreparedScrollbarSkin>, TreeError>
prepareScrollbarSkin(const detail::ScrollWidgetState &state, const ScrollbarSkin &skin) {
  if (!detail::validGlyphSelection(skin.glyphSelection) || skin.pattern < ScrollbarPattern::LEGACY ||
      skin.pattern > ScrollbarPattern::ASCII || !skin.maxGlyphBytes ||
      (skin.trackPattern &&
       (*skin.trackPattern < ScrollbarPattern::LEGACY || *skin.trackPattern > ScrollbarPattern::ASCII)) ||
      (skin.thumbPattern &&
       (*skin.thumbPattern < ScrollbarThumbPattern::LEGACY || *skin.thumbPattern > ScrollbarThumbPattern::ASCII)) ||
      (skin.arrowPattern &&
       (*skin.arrowPattern < ScrollbarArrowPattern::LEGACY || *skin.arrowPattern > ScrollbarArrowPattern::ASCII))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const std::array sources{&skin.horizontal.track, &skin.horizontal.thumb, &skin.horizontal.start,
                           &skin.horizontal.end,   &skin.vertical.track,   &skin.vertical.thumb,
                           &skin.vertical.start,   &skin.vertical.end,     &skin.corner};

  for (std::size_t index = 0; index < sources.size(); ++index) {
    if (*sources[index] && ((*sources[index])->size() > skin.maxGlyphBytes ||
                            ((index % 4 < 2 && index < 8) && (*sources[index])->empty()))) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
  }

  auto result = std::make_unique<detail::PreparedScrollbarSkin>();
  const auto policy = state.glyphs[6].policy();
  const auto *frame = state.advancedFrame     ? &state.advancedFrame->text
                      : state.legacyFrameText ? &*state.legacyFrameText
                                              : nullptr;

  result->trackPattern = skin.trackPattern;
  result->thumbPattern = skin.thumbPattern;
  result->arrowPattern = skin.arrowPattern;

  if (skin.pattern != ScrollbarPattern::LEGACY) {
    result->trackPattern = skin.trackPattern.value_or(skin.pattern);
  }

  if (skin.pattern == ScrollbarPattern::ASCII && !skin.thumbPattern) {
    result->thumbPattern = ScrollbarThumbPattern::ASCII;
  }

  const auto trackPattern = skin.trackPattern.value_or(skin.pattern);
  const auto thumbPattern = skin.thumbPattern.value_or(
    skin.pattern == ScrollbarPattern::ASCII ? ScrollbarThumbPattern::ASCII : ScrollbarThumbPattern::LEGACY);
  const auto mode = state.isArea ? state.area.glyphs : state.bar.glyphs;

  for (std::size_t axis = 0; axis < 2; ++axis) {
    const auto first = axis * 4;
    result->glyphs[first] = trackPattern == ScrollbarPattern::FRAME
                              ? (frame ? frame->glyphs[4 + axis] : state.glyphs[4 + axis])
                              : state.glyphs[6];
    result->glyphs[first + 1] = state.glyphs[7];

    if (trackPattern == ScrollbarPattern::MINIMAL || trackPattern == ScrollbarPattern::ASCII) {
      result->glyphs[first] = GraphemeText::from(trackPattern == ScrollbarPattern::ASCII ? "." : " ", policy).value();
    }

    if (thumbPattern == ScrollbarThumbPattern::ASCII) {
      result->glyphs[first + 1] = GraphemeText::from("#", policy).value();
    }

    result->glyphs[first + 2] = GraphemeText::from(axis == 0 ? "<" : "^", policy).value();
    result->glyphs[first + 3] = GraphemeText::from(axis == 0 ? ">" : "v", policy).value();
  }

  for (std::size_t index = 0; index < sources.size(); ++index) {
    if (!*sources[index]) {
      continue;
    }

    const auto &source = **sources[index];

    if (source.empty()) {
      result->glyphs[index].reset();

      continue;
    }

    auto text = GraphemeText::from(source, policy,
                                   {.max_input_bytes = skin.maxGlyphBytes,
                                    .max_cluster_bytes = skin.maxGlyphBytes,
                                    .max_graphemes = 1,
                                    .max_cells = 1});

    if (!text || text->graphemes().size() != 1 || text->measure() != 1 ||
        text->graphemes().front().display != TextDisplay::PRINTABLE) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    result->glyphs[index] = std::move(*text);
    result->customGlyphs[index] = true;
  }

  const std::array segmentSources{&skin.horizontal.thumbStart,  &skin.horizontal.thumbBody, &skin.horizontal.thumbEnd,
                                  &skin.horizontal.thumbSingle, &skin.vertical.thumbStart,  &skin.vertical.thumbBody,
                                  &skin.vertical.thumbEnd,      &skin.vertical.thumbSingle};

  for (std::size_t index = 0; index < segmentSources.size(); ++index) {
    if (!*segmentSources[index]) {
      continue;
    }

    const auto &source = **segmentSources[index];
    auto text = GraphemeText::from(source, policy,
                                   {.max_input_bytes = skin.maxGlyphBytes,
                                    .max_cluster_bytes = skin.maxGlyphBytes,
                                    .max_graphemes = 1,
                                    .max_cells = 1});

    if (source.empty() || source.size() > skin.maxGlyphBytes || !text || text->graphemes().size() != 1 ||
        text->measure() != 1 || text->graphemes().front().display != TextDisplay::PRINTABLE) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    result->thumbSegments[index] = std::move(*text);
  }

  result->ends = skin.ends;

  if (skin.arrowPattern && *skin.arrowPattern != ScrollbarArrowPattern::LEGACY) {
    const auto arrowMode = *skin.arrowPattern == ScrollbarArrowPattern::ASCII ? PrimitiveGlyphs::ASCII : mode;
    result->arrows = {
      scrollGlyph("\xe2\x97\x80", "<", policy, arrowMode), scrollGlyph("\xe2\x96\xb6", ">", policy, arrowMode),
      scrollGlyph("\xe2\x96\xb2", "^", policy, arrowMode), scrollGlyph("\xe2\x96\xbc", "v", policy, arrowMode)};
  }

  const std::array arrowSources{&skin.horizontal.backward, &skin.horizontal.forward, &skin.vertical.backward,
                                &skin.vertical.forward};
  for (std::size_t index = 0; index < arrowSources.size(); ++index) {
    if (!*arrowSources[index]) {
      continue;
    }
    const auto &source = **arrowSources[index];
    auto text = GraphemeText::from(source, policy,
                                   {.max_input_bytes = skin.maxGlyphBytes,
                                    .max_cluster_bytes = skin.maxGlyphBytes,
                                    .max_graphemes = 1,
                                    .max_cells = 1});
    if (source.empty() || source.size() > skin.maxGlyphBytes || !text || text->graphemes().size() != 1 ||
        text->measure() != 1 || text->graphemes().front().display != TextDisplay::PRINTABLE) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    result->arrows[index] = std::move(*text);
    result->customArrows[index] = true;
  }

  return result;
}
} // namespace

std::expected<void, TreeError> setScrollbarSkin(WidgetTree &tree, Node<Widget> node, const ScrollbarSkin &skin) {
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](Widget &widget) {
    // Grid's internal scrolling retains its existing options; this API targets ordinary controls only.
    if (!dynamic_cast<ScrollArea *>(&widget) && !dynamic_cast<ScrollBar *>(&widget)) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);

      return;
    }

    auto &state = *detail::ScrollWidgetAccess::state(widget);
    const auto revision = state.skinRevision;
    const auto styleRevision = widget.styleRevision();

    if (state.retired || revision == std::numeric_limits<std::uint64_t>::max()) {
      result = std::unexpected(state.retired ? TreeError::STALE_NODE : TreeError::IDENTITY_EXHAUSTED);

      return;
    }

    try {
      auto prepared = prepareScrollbarSkin(state, skin);

      if (!prepared) {
        result = std::unexpected(prepared.error());

        return;
      }

      auto profiles = detail::GlyphProfileAccess::prepare(widget, widget.computedStyle().value_or(ComputedStyle{}),
                                                          &skin.glyphSelection);

      if (!profiles) {
        result = std::unexpected(profiles.error());

        return;
      }

      // Allocator reentrancy can retire this owner or install a newer skin during preparation.
      auto live = tree.inspect(node);

      if (!live || state.retired || state.skinRevision != revision || widget.styleRevision() != styleRevision) {
        result = std::unexpected(!live ? live.error() : state.retired ? TreeError::STALE_NODE : TreeError::WOULD_BLOCK);

        return;
      }

      result = tree.invalidate(node, Invalidation::PAINT);

      if (result) {
        state.scrollbarSkin = std::move(*prepared);
        state.glyphSelection = skin.glyphSelection;
        state.glyphProfiles = std::move(*profiles);
        ++state.skinRevision;
      }
    } catch (const std::bad_alloc &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
    }
  });

  return edited ? result : edited;
}
} // namespace portals
