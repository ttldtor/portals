// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/views/scroll_controller_state.hpp"
#include "../src/widgets/glyph_profiles.hpp"
#include "../src/widgets/scroll_controls_state.hpp"
#include <portals/managed.hpp>
#include <portals/scroll_controls.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <source_location>
#include <thread>

namespace allocation_test {
thread_local bool fail{};
thread_local bool failDrag{};
thread_local std::size_t allocations{};
thread_local void (*hook)(){};
thread_local int beforeHook{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  ++allocation_test::allocations;
  if (allocation_test::failDrag && size == sizeof(portals::detail::ScrollDrag)) {
    allocation_test::failDrag = false;
    throw std::bad_alloc();
  }
  if (allocation_test::fail) {
    throw std::bad_alloc();
  }
  if (allocation_test::hook && allocation_test::beforeHook == 0) {
    auto hook = std::exchange(allocation_test::hook, nullptr);
    hook();
  } else if (allocation_test::hook) {
    --allocation_test::beforeHook;
  }
  if (auto *storage = std::malloc(size ? size : 1)) {
    return storage;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *storage) noexcept {
  std::free(storage);
}

void operator delete[](void *storage) noexcept {
  std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept {
  std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept {
  std::free(storage);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

namespace {
using namespace portals;
int failures{};

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E>
T take(std::expected<T, E> value, std::source_location location = std::source_location::current()) {
  if (!value) {
    std::cerr << "fixture error at " << location.line() << '\n';
    std::abort();
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

template <class T> bool error(const std::expected<T, TreeError> &value, TreeError expected) {
  return !value && value.error() == expected;
}

struct Content : SizedWidget {
  CellSize desired;

  explicit Content(CellSize extent) : desired(extent) {
  }

  std::expected<CellSize, TreeError> measure(CellSize) override {
    return desired;
  }
};

struct Sink : PaintContext {
  std::map<std::pair<int, int>, std::string> cells;
  std::vector<CellStyle> styles;
  std::size_t submitted{};
  bool fail{};

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    if (fail) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    for (const auto &command : commands) {
      ++submitted;
      if (const auto *text = std::get_if<TextCommand>(&command)) {
        cells[{text->origin.x, text->origin.y}] = std::string(text->text.bytes());
        styles.push_back(text->style);
      } else {
        const auto &fill = std::get<FillCommand>(command);
        cells[{fill.area.origin().x, fill.area.origin().y}] = " ";
        styles.push_back(fill.style);
      }
    }
    return {};
  }
};

struct TreeFixture {
  SchedulerTime now{};
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create({}, {[](void *context) noexcept {
                                                                       return *static_cast<SchedulerTime *>(context);
                                                                     },
                                                                     &now}));
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));

  void layout(CellSize size) {
    const auto bounds = take(CellRect::from({}, size));
    take(tree->frame([&] {
      take(layoutTree(*tree, root, bounds, bounds));
    }));
  }

  DispatchResult mouse(MouseAction action, MouseButton button, int x, int y) {
    return take(router->dispatch(MouseInputEvent{x, y, button, action, {}}));
  }

  DispatchResult key(InputKey key) {
    return take(router->dispatch(KeyEvent{key}));
  }

  Sink paint(Node<Widget> node) {
    Sink sink;
    take(tree->frame([&] {
      take(tree->paint(node, sink));
    }));
    return sink;
  }
};

struct AreaFixture : TreeFixture {
  std::shared_ptr<ScrollController> controller = take(ScrollController::create());
  Node<ScrollArea> area;
  Node<Content> content;

  explicit AreaFixture(ScrollAreaOptions options = {}, CellSize extent = {20, 30}, bool initialize = true) {
    area = take(tree->emplaceChecked<ScrollArea>(root, controller, WidthPolicy{}, options));
    content = take(tree->emplaceChecked<Content>(area, extent));
    if (initialize) {
      take(initializeScrollArea(*tree, area));
    }
  }

  AreaFixture(ScrollAreaOptions options, FrameOptions frame, CellSize extent = {20, 30}, bool initialize = true) {
    area = take(tree->emplaceChecked<ScrollArea>(root, controller, WidthPolicy{}, options, ControlStyle{}, SizePolicy{},
                                                 std::move(frame), std::in_place));
    content = take(tree->emplaceChecked<Content>(area, extent));

    if (initialize) {
      take(initializeScrollArea(*tree, area));
    }
  }

  void settle(CellSize size = {10, 6}) {
    layout(size);
    take(prepareScrollArea(*tree, area, take(scrollAreaLayout(*tree, area))));
    layout(size);
    take(prepareScrollArea(*tree, area, take(scrollAreaLayout(*tree, area))));
  }

  ScrollControllerSnapshot values() {
    return take(controller->snapshot());
  }
};

struct BarFixture : TreeFixture {
  std::shared_ptr<ScrollController> controller = take(ScrollController::create());
  Node<ScrollBar> bar;

  explicit BarFixture(ScrollBarOptions options = {}, ScrollAxis axis = ScrollAxis::VERTICAL) {
    bar = take(tree->emplaceChecked<ScrollBar>(root, controller, axis, WidthPolicy{}, options));
    take(initializeScrollBar(*tree, bar));
    take(setScrollMetrics(*tree, bar, {100, 10}));
    layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
  }

  ScrollControllerSnapshot values() {
    return take(controller->snapshot());
  }
};

void fractionalScrollbarPaint() {
  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture fixture({}, axis);
    take(setScrollbarSkin(*fixture.tree, fixture.bar, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK}));
    take(setScrollMetrics(*fixture.tree, fixture.bar, {11, 1}));
    fixture.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{3, 1} : CellSize{1, 3});
    take(fixture.tree->consumeFrameRequest());
    take(setScrollOffset(*fixture.tree, fixture.bar, axis, {0, .625}));
    check(take(fixture.tree->consumeFrameRequest()) == Invalidation::PAINT,
          "fraction-only position requests paint without layout");
    const auto painted = fixture.paint(fixture.bar);
    const auto cell = [axis](int index) {
      return axis == ScrollAxis::HORIZONTAL ? std::pair{index, 0} : std::pair{0, index};
    };
    check(painted.cells.at(cell(0)) == (axis == ScrollAxis::HORIZONTAL ? "\xe2\x96\x8f" : "\xe2\x96\x87") &&
            painted.cells.at(cell(1)) == (axis == ScrollAxis::HORIZONTAL ? "\xe2\x96\x8f" : "\xe2\x96\x87"),
          "first and extra trailing cells use complementary one-eighth boundaries");
    const RgbColor ink{220, 225, 232};
    const RgbColor surface{24, 28, 36};
    check(painted.styles[0].foreground == (axis == ScrollAxis::HORIZONTAL ? surface : ink) &&
            painted.styles[1].foreground == (axis == ScrollAxis::HORIZONTAL ? ink : surface),
          "plain default colors resolve to real RGB before complementary coverage");
    check(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE,
          "fractional paint remains idle without a scheduler");
    take(syncScrollControl(*fixture.tree, fixture.bar, fixture.bar));
    const auto focused = fixture.paint(fixture.bar);
    check(focused.styles[0].foreground == (axis == ScrollAxis::HORIZONTAL ? surface : RgbColor{75, 180, 220}),
          "plain focused profile supplies only missing local ink");
    take(fixture.tree->setEnabled(fixture.bar, false));
    take(syncScrollControl(*fixture.tree, fixture.bar));
    const auto disabled = fixture.paint(fixture.bar);
    check(disabled.styles[0].foreground == (axis == ScrollAxis::HORIZONTAL ? surface : RgbColor{125, 130, 140}),
          "plain disabled profile supplies only missing local ink");
    take(fixture.tree->setEnabled(fixture.bar, true));
    take(syncScrollControl(*fixture.tree, fixture.bar));
    ScrollbarSkin incompatible{.pattern = ScrollbarPattern::FRACTIONAL_BLOCK};
    incompatible.horizontal.thumbStart = "[";
    check(error(setScrollbarSkin(*fixture.tree, fixture.bar, incompatible), TreeError::INVALID_ARGUMENT),
          "fractional coverage cannot silently replace explicit caps");
    ScrollbarSkin literal{.thumbPattern = ScrollbarThumbPattern::FRACTIONAL_BLOCK};
    literal.horizontal.track = " ";
    literal.vertical.track = " ";
    take(setScrollbarSkin(*fixture.tree, fixture.bar, literal));
    check(fixture.paint(fixture.bar).cells == painted.cells,
          "explicit literal blank tracks are compatible without an unrelated pattern requirement");
  }
  TreeFixture widths;
  const auto wide = take(widths.tree->emplaceChecked<ScrollBar>(
    widths.root, std::shared_ptr<ScrollController>{}, ScrollAxis::VERTICAL, WidthPolicy{AmbiguousWidth::WIDE}));
  check(error(setScrollbarSkin(*widths.tree, wide, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK}),
              TreeError::INVALID_ARGUMENT),
        "coverage rejects a width policy that cannot admit every boundary as one cell");
  BarFixture ascii({.glyphs = PrimitiveGlyphs::ASCII});
  check(error(setScrollbarSkin(*ascii.tree, ascii.bar, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK}),
              TreeError::INVALID_ARGUMENT),
        "fractional opt-in cannot silently override an ASCII primitive contract");
}

void fractionalScrollbarStylesAndDrag() {
  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture bar({}, axis);
    take(setScrollbarSkin(*bar.tree, bar.bar, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK}));
    take(setScrollMetrics(*bar.tree, bar.bar, {11, 1}));
    bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{3, 1} : CellSize{1, 3});
    auto binding = take(bindScrollBar(*bar.router, *bar.tree, bar.bar));
    take(bar.router->setPointerCaptureCapabilities({true, true, true}));
    take(setScrollOffset(*bar.tree, bar.bar, axis, {0, .625}));
    for (const bool precise : {false, true}) {
      const auto send = [&](MouseAction action, std::int32_t position = 1) {
        MouseInputEvent event{axis == ScrollAxis::HORIZONTAL ? position : 0,
                              axis == ScrollAxis::VERTICAL ? position : 0,
                              MouseButton::LEFT,
                              action,
                              {}};
        if (precise) {
          event.precision =
            InputPointerPrecision{axis == ScrollAxis::HORIZONTAL ? static_cast<std::uint32_t>(position) * 8 : 0U,
                                  axis == ScrollAxis::VERTICAL ? static_cast<std::uint32_t>(position) * 8 : 0U,
                                  axis == ScrollAxis::HORIZONTAL ? 3U : 1U,
                                  axis == ScrollAxis::VERTICAL ? 3U : 1U,
                                  axis == ScrollAxis::HORIZONTAL ? 24U : 8U,
                                  axis == ScrollAxis::VERTICAL ? 24U : 8U,
                                  7};
        }
        take(bar.router->dispatch(event));
      };
      send(MouseAction::PRESS);
      check(take(bar.router->pointerCapture()) == bar.bar,
            "partially covered trailing cell acquires the existing capture owner");
      send(MouseAction::MOVE);
      const auto values = bar.values();
      const auto offset = axis == ScrollAxis::HORIZONTAL ? values.horizontal.offset : values.vertical.offset;
      check(offset.raster == 0 && offset.fraction == .625,
            "unchanged cell or precise move keeps the full unquantized logical anchor");
      send(MouseAction::MOVE, 2);
      const auto moved = bar.values();
      const auto next = axis == ScrollAxis::HORIZONTAL ? moved.horizontal.offset : moved.vertical.offset;
      check(next.raster == 5 && next.fraction == .625,
            "cell and precise moves preserve logical fraction rather than quantizing to paint units");
      send(MouseAction::MOVE, 99);
      const auto end = bar.values();
      const auto endOffset = axis == ScrollAxis::HORIZONTAL ? end.horizontal.offset : end.vertical.offset;
      check(endOffset.raster == 10 && endOffset.fraction == 0, "fractional dragging saturates at the exact endpoint");
      send(MouseAction::MOVE, 0);
      const auto start = bar.values();
      const auto startOffset = axis == ScrollAxis::HORIZONTAL ? start.horizontal.offset : start.vertical.offset;
      check(startOffset.raster == 0 && startOffset.fraction == 0, "fractional dragging saturates at the exact start");
      send(MouseAction::RELEASE);
      take(setScrollOffset(*bar.tree, bar.bar, axis, {0, .625}));
      bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{3, 1} : CellSize{1, 3});
    }

    StyleEngine engine(*bar.tree);
    take(engine.bind(bar.bar, {"ScrollBar", "", {}, true}));
    for (const auto &sheet : {modernTheme(), catppuccinLatteTheme()}) {
      take(engine.replace(sheet));
      for (int index = 0; index < 3; ++index) {
        take(bar.tree->setEnabled(bar.bar, index != 2));
        take(syncScrollControl(*bar.tree, bar.bar, index == 1 ? bar.bar : Node<Widget>{}));
        const auto painted = bar.paint(bar.bar);
        const auto resolved = resolveStyle(sheet, {"ScrollBar", "", {}, true});
        const auto base = index == 0 ? resolved.normal : index == 1 ? resolved.focused : resolved.disabled;
        const auto surface = resolved.scrollbar[index].track.value_or(base).background;
        const auto ink = resolved.scrollbar[index].thumb.value_or(base).foreground;
        check(painted.styles[0].foreground == (axis == ScrollAxis::HORIZONTAL ? surface : ink) &&
                painted.styles[1].foreground == (axis == ScrollAxis::HORIZONTAL ? ink : surface),
              "dark/light and focused/disabled resolved colors win over local profile defaults");
      }
    }
    take(bar.tree->setEnabled(bar.bar, true));
    take(syncScrollControl(*bar.tree, bar.bar));
    take(engine.replace(
      take(parsePss("*{scrollbar-thumb-single-foreground:#010203;scrollbar-track-background:#fafbfc;}"))));
    const auto segmented = bar.paint(bar.bar);
    check(segmented.styles[0].foreground ==
            (axis == ScrollAxis::HORIZONTAL ? RgbColor{250, 251, 252} : RgbColor{1, 2, 3}),
          "explicit segment ink and independent track surface survive complementary painting");
    const auto invalid = engine.replace(take(parsePss("*{scrollbar-track-pattern:ascii;}")));
    check(!invalid && bar.paint(bar.bar).cells == segmented.cells,
          "incompatible dynamic track style rejects without replacing prepared coverage");

    struct NoAllocationPaint final : PaintContext {
      std::expected<void, TreeError> submit(std::span<const RenderCommand>) override {
        return {};
      }
    } sink;

    std::expected<void, TreeError> painted;
    take(bar.tree->frame([&] {
      allocation_test::fail = true;
      painted = bar.tree->paint(bar.bar, sink);
      allocation_test::fail = false;
    }));
    check(painted.has_value(), "prepared fractional painting constructs no owning glyph or allocation");
  }
}

void fractionalProjectionAndPlacement() {
  TreeFixture shared;
  auto controller = take(ScrollController::create());
  const auto row = take(shared.tree->emplaceChecked<Row>(shared.root));
  const auto writer = take(shared.tree->emplaceChecked<ScrollBar>(row, controller));
  const auto projection = take(shared.tree->emplaceChecked<ScrollBar>(row, controller));
  take(initializeScrollBar(*shared.tree, writer));
  take(initializeScrollBar(*shared.tree, projection, false));
  take(setScrollMetrics(*shared.tree, writer, {11, 1}));
  for (const auto node : {writer, projection}) {
    take(setScrollbarSkin(*shared.tree, node, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK}));
  }
  shared.layout({2, 3});
  const auto before = take(shared.tree->inspect(projection)).geometry;
  take(shared.tree->consumeFrameRequest());
  take(setScrollOffset(*shared.tree, writer, ScrollAxis::VERTICAL, {0, .625}));
  check(take(shared.tree->consumeFrameRequest()) == Invalidation::PAINT,
        "shared logical-only publication requests PAINT");
  check(shared.paint(writer).cells == shared.paint(projection).cells &&
          shared.paint(projection).cells.at({0, 1}) == "\xe2\x96\x87" &&
          take(shared.tree->inspect(projection)).geometry.local == before.local,
        "all shared projections refresh fractional offsets without re-arranging their geometry");

  for (const auto placement : {ScrollPlacement::RESERVED, ScrollPlacement::OVERLAY, ScrollPlacement::ON_FRAME}) {
    for (const bool arrows : {false, true}) {
      ScrollAreaOptions options;
      options.placement = placement;
      options.arrows = arrows;
      options.frame = BorderOptions{};
      AreaFixture area(options);
      take(setScrollbarSkin(*area.tree, area.area, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK}));
      area.settle({10, 7});
      const auto layout = take(scrollAreaLayout(*area.tree, area.area));
      const auto child = take(area.tree->inspect(area.content)).geometry;
      take(area.tree->consumeFrameRequest());
      take(setScrollOffset(*area.tree, area.area, ScrollAxis::HORIZONTAL, {0, .875}));
      take(setScrollOffset(*area.tree, area.area, ScrollAxis::VERTICAL, {0, .875}));
      check(take(area.tree->consumeFrameRequest()) == Invalidation::PAINT,
            "both area fractions retain the text raster layout");
      const auto painted = area.paint(area.area);
      check(painted.cells.at({0, 0}) == "\xe2\x94\x8c" && painted.cells.at({9, 6}) == "\xe2\x94\x98" &&
              take(scrollAreaLayout(*area.tree, area.area)).layoutRevision == layout.layoutRevision &&
              take(area.tree->inspect(area.content)).geometry.local == child.local,
            "fractional ON_FRAME and other placements preserve corners and child raster geometry");
      if (arrows) {
        const auto horizontal = layout.placement.horizontal;
        const auto vertical = layout.placement.vertical;
        check(painted.cells.at({horizontal.origin.x, horizontal.origin.y}) == "\xe2\x97\x80" &&
                painted.cells.at({vertical.origin.x, vertical.origin.y}) == "\xe2\x96\xb2",
              "coverage never consumes functional arrow cells");
      }
      const auto oldValues = area.values();
      take(area.tree->consumeFrameRequest());
      allocation_test::fail = true;
      const auto rejected = setScrollbarSkin(*area.tree, area.area, {.pattern = ScrollbarPattern::FRACTIONAL_BLOCK});
      allocation_test::fail = false;
      check(error(rejected, TreeError::NO_MEMORY) && take(area.tree->consumeFrameRequest()) == Invalidation::NONE &&
              area.paint(area.area).cells == painted.cells && area.values().revision == oldValues.revision,
            "failed owning coverage preparation preserves prior glyphs, offsets and invalidation");
    }
  }
}

void sharedScrollbarPresetLifetime() {
  TreeFixture fixture;
  const WidthPolicy original{AmbiguousWidth::NARROW, EmojiWidth::TWO, 9981};
  auto controller = take(ScrollController::create());
  const auto area = take(fixture.tree->emplaceChecked<ScrollArea>(fixture.root, controller, original));
  const auto bar =
    take(fixture.tree->emplaceChecked<ScrollBar>(fixture.root, controller, ScrollAxis::VERTICAL, original));
  std::weak_ptr<const detail::ScrollbarPresetGlyphs> first;
  std::weak_ptr<const detail::ScrollbarPresetGlyphs> same;

  take(fixture.tree->read(area, [&](const ScrollArea &widget) {
    first = detail::ScrollWidgetAccess::state(widget).presetGlyphs;
  }));
  take(fixture.tree->read(bar, [&](const ScrollBar &widget) {
    same = detail::ScrollWidgetAccess::state(widget).presetGlyphs;
  }));
  check(first.lock() == same.lock(), "same-policy area and bar share the immutable prepared palette");

  const auto beforeReuse = allocation_test::allocations;
  {
    detail::ScrollWidgetState reuse(controller, original, PrimitiveGlyphs::ASCII);
  }
  const auto reuseAllocations = allocation_test::allocations - beforeReuse;
  allocation_test::beforeHook = static_cast<int>(reuseAllocations);
  allocation_test::hook = [] {
    allocation_test::fail = true;
  };
  bool preparationFailed{};

  try {
    detail::ScrollWidgetState failed(controller, {original.ambiguous, original.emoji, original.revision + 1},
                                     PrimitiveGlyphs::ASCII);
  } catch (const std::bad_alloc &) {
    preparationFailed = true;
  }

  allocation_test::fail = false;
  allocation_test::hook = nullptr;
  check(preparationFailed, "allocation failure during new palette preparation remains fallible");
  {
    detail::ScrollWidgetState reuse(controller, original, PrimitiveGlyphs::ASCII);
    check(reuse.presetGlyphs == first.lock(), "failed replacement leaves the prior weak cache entry usable");
  }

  for (const auto policy : {WidthPolicy{AmbiguousWidth::WIDE, EmojiWidth::TWO, original.revision},
                            WidthPolicy{AmbiguousWidth::NARROW, EmojiWidth::ONE, original.revision},
                            WidthPolicy{AmbiguousWidth::NARROW, EmojiWidth::TWO, original.revision + 1}}) {
    const auto other =
      take(fixture.tree->emplaceChecked<ScrollBar>(fixture.root, controller, ScrollAxis::HORIZONTAL, policy));
    std::weak_ptr<const detail::ScrollbarPresetGlyphs> replacement;

    take(fixture.tree->read(other, [&](const ScrollBar &widget) {
      const auto &palette = detail::ScrollWidgetAccess::state(widget).presetGlyphs;
      replacement = palette;
      check(palette != first.lock(), "each complete WidthPolicy difference prepares a distinct palette");

      for (const auto &glyph : *palette) {
        check(glyph.policy() == policy && glyph.measure() == 1,
              "every shared glyph retains the complete receiving policy");
      }
    }));
    {
      const auto survivor = first.lock();
      check(survivor && survivor->front().policy() == original && survivor->back().bytes() == "v",
            "existing controls retain their original prepared values after cache replacement");
    }

    take(fixture.tree->remove(other));
    check(replacement.expired(), "the thread-local cache does not strongly retain the last palette");
  }

  take(fixture.tree->remove(area));
  check(!first.expired(), "the surviving bar retains shared palette ownership after area removal");
  take(fixture.tree->remove(bar));
  check(first.expired() && same.expired(), "the final control releases its palette without a global strong owner");
}

void initializationAndLifetime() {
  AreaFixture f({}, {20, 30}, false);
  check(error(setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {2, 0}), TreeError::INVALID_ARGUMENT),
        "uninitialized commands are typed failures");
  f.layout({10, 6});
  check(f.paint(f.area).submitted == 0, "uninitialized paint is empty");
  check(error(scrollAreaLayout(*f.tree, f.area), TreeError::INVALID_ARGUMENT),
        "uninitialized preparation observation fails");
  take(initializeScrollArea(*f.tree, f.area));
  f.settle();
  take(setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {7, .5}));
  const auto revision = f.values().revision;
  allocation_test::fail = true;
  const auto again = initializeScrollArea(*f.tree, f.area);
  allocation_test::fail = false;
  check(again && f.values().revision == revision && f.values().vertical.offset.raster == 7,
        "repeat initialization needs no allocation and preserves position");
  auto second = take(f.tree->emplaceChecked<ScrollArea>(f.root, f.controller));
  check(error(initializeScrollArea(*f.tree, second), TreeError::INVALID_ARGUMENT), "second geometry writer rejected");
  auto projection = take(f.tree->emplaceChecked<ScrollBar>(f.root, f.controller));
  take(initializeScrollBar(*f.tree, projection, false));
  check(error(setScrollMetrics(*f.tree, projection, {50, 5}), TreeError::INVALID_ARGUMENT),
        "area metrics reject projection writer");
  check(error(initializeScrollBar(*f.tree, projection, true), TreeError::INVALID_ARGUMENT),
        "writer role cannot silently change");
  TreeFixture other;
  auto foreign = take(other.tree->emplaceChecked<ScrollBar>(other.root, f.controller));
  check(error(initializeScrollBar(*other.tree, foreign, false), TreeError::WRONG_SESSION),
        "live projections pin one tree session");
  take(f.tree->remove(f.area));
  take(initializeScrollArea(*f.tree, second));
  take(setScrollOffset(*f.tree, projection, ScrollAxis::VERTICAL, {4, 0}));
  check(f.values().vertical.offset.raster == 4, "projection and values survive removal of previous writer");
  take(f.tree->remove(second));
  take(f.tree->remove(projection));
  take(initializeScrollBar(*other.tree, foreign, false));
  check(error(setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {}), TreeError::STALE_NODE),
        "stale node command rejected");
}

void placementPaintAndHits() {
  for (auto placement : {ScrollPlacement::RESERVED, ScrollPlacement::OVERLAY, ScrollPlacement::ON_FRAME}) {
    ScrollAreaOptions options;
    options.placement = placement;
    options.frame = BorderOptions{PrimitiveGlyphs::ASCII, 16384};
    options.glyphs = PrimitiveGlyphs::ASCII;
    AreaFixture f(options);
    auto binding = take(bindScrollArea(*f.router, *f.tree, f.area));
    f.settle({10, 6});
    const auto observed = take(scrollAreaLayout(*f.tree, f.area));
    const auto viewport = placement == ScrollPlacement::RESERVED ? CellSize{7, 3} : CellSize{8, 4};
    check(observed.placement.viewport == viewport, "frame inset applied once, reserved gutter only when selected");
    const auto child = take(f.tree->inspect(f.content));
    check(child.geometry.clip.origin() == CellPoint{1, 1} &&
            child.geometry.clip.size() == (placement == ScrollPlacement::ON_FRAME ? CellSize{8, 4} : CellSize{7, 3}),
          "content clip excludes every owned edge cell");
    const auto track = observed.placement.vertical;
    check(take(f.router->hitTest(track.origin)) == f.area, "bar track belongs to same area hit owner");
    auto painted = f.paint(f.area);
    check(painted.cells[{0, 0}] == "+" && painted.cells[{9, 5}] == "+", "frame corners retained");
    check(painted.cells[{track.origin.x, track.origin.y}] == "#", "exact first endpoint thumb painted");
    if (placement == ScrollPlacement::ON_FRAME) {
      check(track.origin == CellPoint{9, 1} && track.length == 4 && !observed.placement.hasCorner,
            "ON_FRAME uses edge except corners and has no extra gutter");
      take(f.tree->edit(f.content, [](Content &content) {
        content.desired = {2, 2};
      }));
      take(f.tree->invalidate(f.content, Invalidation::LAYOUT));
      f.settle({10, 6});
      painted = f.paint(f.area);
      check(painted.cells[{9, 1}] == "|" && painted.cells[{1, 5}] == "-", "auto-hide restores frame edges");
    } else {
      check(observed.placement.hasCorner, "two non-frame bars own their intersection cell");
    }
    for (int width = 0; width <= 2; ++width) {
      for (int height = 0; height <= 2; ++height) {
        f.settle({width, height});
        const auto tiny = take(scrollAreaLayout(*f.tree, f.area));
        check(tiny.placement.viewport.width == 0 && tiny.placement.viewport.height == 0,
              "tiny frame has empty logical viewport");
        f.paint(f.area);
      }
    }
  }
  AreaFixture mutual({}, {9, 7});
  mutual.settle({10, 6});
  const auto placement = take(scrollAreaLayout(*mutual.tree, mutual.area)).placement;
  check(placement.verticalBar && !placement.horizontalBar && placement.viewport == CellSize{9, 6},
        "least fixed point does not add an unnecessary second bar");
  AreaFixture both({}, {10, 7});
  both.settle({10, 6});
  check(take(scrollAreaLayout(*both.tree, both.area)).placement.horizontalBar,
        "vertical gutter can force horizontal bar");
  ScrollAreaOptions mixedGlyphs;
  mixedGlyphs.frame = BorderOptions{PrimitiveGlyphs::ASCII, 16384};
  AreaFixture mixed(mixedGlyphs);
  mixed.settle();
  check(mixed.paint(mixed.area).cells[{0, 0}] == "+", "frame glyph override is independent of bar glyph mode");
}

void wheelKeysAndBoundary() {
  ScrollAreaOptions options;
  options.wheelStep = .5;
  AreaFixture f(options);
  auto binding = take(bindScrollArea(*f.router, *f.tree, f.area));
  int outer{};
  auto parent = take(f.router->bind(
    f.root,
    [&](Widget &, const RoutedEvent &) {
      ++outer;
      return EventReply::HANDLED;
    },
    false));
  f.settle();
  take(f.tree->consumeFrameRequest());
  auto reply = f.mouse(MouseAction::SCROLL, MouseButton::WHEEL_DOWN, 2, 2);
  check(reply.handler == f.area.id() && f.values().vertical.offset.fraction == .5 && outer == 0,
        "fraction-only wheel logically moves and consumes whole event");
  check(take(f.tree->consumeFrameRequest()) == Invalidation::PAINT, "fraction-only movement requests paint");
  f.mouse(MouseAction::SCROLL, MouseButton::WHEEL_DOWN, 2, 2);
  check(f.values().vertical.offset.raster == 1 && take(f.tree->consumeFrameRequest()) == Invalidation::LAYOUT,
        "next fractional wheel changes raster and requests layout");
  f.settle();
  take(f.router->setFocus(f.area));
  f.key(InputKey::END);
  check(f.values().vertical.offset.raster == 25, "End reaches extent minus actual viewport");
  f.settle();
  reply = f.mouse(MouseAction::SCROLL, MouseButton::WHEEL_DOWN, 2, 2);
  check(reply.handler == f.root.id() && outer == 1, "exact boundary bubbles whole wheel to ancestor");
  take(setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {24, .75}));
  f.settle();
  reply = f.mouse(MouseAction::SCROLL, MouseButton::WHEEL_DOWN, 2, 2);
  check(reply.handler == f.area.id() && outer == 1 && f.values().vertical.offset.raster == 25,
        "partial inner movement owns whole event with no remainder routing");
  f.settle();
  f.key(InputKey::HOME);
  check(f.values().vertical.offset.raster == 0, "Home resets exact offset");
  f.key(InputKey::PAGE_DOWN);
  check(f.values().vertical.offset.raster == 4, "page uses viewport minus overlap");
  f.key(InputKey::RIGHT);
  check(f.values().horizontal.offset.raster == 1, "right key moves independent horizontal axis");
  InputModifiers control;
  control.control = true;
  const auto beforeControlHome = f.values().revision;
  take(f.router->dispatch(KeyEvent{InputKey::HOME, 0, control}));
  check(f.values().horizontal.offset.raster == 0 && f.values().vertical.offset.raster == 0 &&
          f.values().revision == beforeControlHome + 1,
        "Control+Home publishes both axes atomically in one controller revision");
  take(f.router->dispatch(KeyEvent{InputKey::END, 0, control}));
  check(f.values().horizontal.offset.raster == 11 && f.values().vertical.offset.raster == 25,
        "Control+End reaches both exact viewport endpoints");
  InputModifiers shift;
  shift.shift = true;
  take(f.router->dispatch(KeyEvent{InputKey::PAGE_UP, 0, shift, KeyAction::REPEAT}));
  check(f.values().horizontal.offset.raster == 3 && f.values().vertical.offset.raster == 25,
        "Shift+PageUp applies horizontal viewport overlap on repeat");
  const auto beforeIgnored = f.values().revision;
  take(f.router->dispatch(KeyEvent{InputKey::UP, 0, shift}));
  take(f.router->dispatch(KeyEvent{InputKey::HOME, 0, {}, KeyAction::RELEASE}));
  check(f.values().revision == beforeIgnored, "unassigned modifiers and release preserve state");
  f.settle();
  take(f.router->dispatch(MouseInputEvent{2, 2, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, shift}));
  check(f.values().horizontal.offset.raster == 3 && f.values().horizontal.offset.fraction == .5 &&
          f.values().vertical.offset.raster == 25,
        "Shift+vertical wheel selects X without moving Y");
  auto child = take(f.router->bind(f.content, [](Content &, const RoutedEvent &) {
    return EventReply::HANDLED;
  }));
  take(f.router->setFocus(f.content));
  const auto before = f.values().revision;
  f.key(InputKey::DOWN);
  check(f.values().revision == before, "editable/consuming child preserves its key ownership");
}

void exactStandaloneAndReadonly() {
  ScrollBarOptions options;
  options.readOnly = true;
  BarFixture f(options);
  auto binding = take(bindScrollBar(*f.router, *f.tree, f.bar));
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  take(setScrollMetrics(*f.tree, f.bar, {maximum, 1}));
  take(setScrollOffset(*f.tree, f.bar, ScrollAxis::VERTICAL, {maximum - 2, .5}));
  check(f.values().vertical.offset.raster == maximum - 2 && f.values().vertical.offset.fraction == .5,
        "programmatic read-only position retains exact u64 above double precision");
  f.layout({1, 10});
  take(f.router->setFocus(f.bar));
  f.key(InputKey::HOME);
  f.mouse(MouseAction::SCROLL, MouseButton::WHEEL_UP, 0, 0);
  check(f.values().vertical.offset.raster == maximum - 2, "read-only rejects key and wheel mutations");
  take(setScrollOffset(*f.tree, f.bar, ScrollAxis::VERTICAL, {maximum, 0}));
  check(f.values().vertical.offset.raster == maximum - 1 && f.values().vertical.offset.fraction == 0,
        "exact endpoint clamps without u64 overflow");
  take(f.tree->consumeFrameRequest());
  take(setScrollOffset(*f.tree, f.bar, ScrollAxis::VERTICAL, {maximum - 1, 0}));
  check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "equal endpoint set is idle");
  check(error(setScrollOffset(*f.tree, f.bar, ScrollAxis::VERTICAL, {3, std::numeric_limits<double>::quiet_NaN()}),
              TreeError::INVALID_ARGUMENT),
        "invalid fraction rejected before publication");
  BarFixture keys;
  auto keysBinding = take(bindScrollBar(*keys.router, *keys.tree, keys.bar));
  take(keys.router->setFocus(keys.bar));
  keys.key(InputKey::END);
  check(keys.values().vertical.offset.raster == 90, "standalone End uses checked explicit metrics");
  keys.key(InputKey::PAGE_UP);
  check(keys.values().vertical.offset.raster == 81, "standalone PageUp retains overlap");
  keys.key(InputKey::LEFT);
  check(keys.values().vertical.offset.raster == 81, "orthogonal key is not consumed by standalone axis");
  keys.layout({1, 10});
  keys.mouse(MouseAction::SCROLL, MouseButton::WHEEL_LEFT, 0, 0);
  check(keys.values().vertical.offset.raster == 81,
        "orthogonal wheel preserves its X mapping and bypasses vertical bar");
}

void dragAllocationAdmission() {
  BarFixture bar;
  auto binding = take(bindScrollBar(*bar.router, *bar.tree, bar.bar));

  take(bar.router->setPointerCaptureCapabilities({true, true, true}));
  allocation_test::failDrag = true;
  const auto failed = bar.router->dispatch(MouseInputEvent{0, 0, MouseButton::LEFT, MouseAction::PRESS, {}});

  allocation_test::failDrag = false;
  check(error(failed, TreeError::NO_MEMORY) && take(bar.router->pointerCapture()) == Node<Widget>{} &&
          bar.values().vertical.offset.raster == 0,
        "begin-drag allocation failure is typed and preserves controller/capture state");
  bar.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 0);
  allocation_test::failDrag = true;
  const auto duplicate = bar.router->dispatch(MouseInputEvent{0, 0, MouseButton::LEFT, MouseAction::PRESS, {}});
  const auto moved = bar.router->dispatch(MouseInputEvent{0, 1, MouseButton::LEFT, MouseAction::MOVE, {}});
  const bool noAllocation = allocation_test::failDrag;

  allocation_test::failDrag = false;
  check(duplicate.has_value() && moved.has_value() && noAllocation && take(bar.router->pointerCapture()) == bar.bar,
        "duplicate press and MOVE retain the existing owning drag without allocating another anchor");
  bar.mouse(MouseAction::RELEASE, MouseButton::LEFT, 0, 1);
  take(bar.tree->read(bar.bar, [](const ScrollBar &widget) {
    check(!detail::ScrollWidgetAccess::state(widget).drag, "release frees the out-of-line drag payload");
  }));
  take(setScrollOffset(*bar.tree, bar.bar, ScrollAxis::VERTICAL, {}));
  bar.layout({1, 10});
  bar.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 0);
  take(bar.router->cancelPointerCapture());
  take(syncScrollControl(*bar.tree, bar.bar));
  take(bar.tree->read(bar.bar, [](const ScrollBar &widget) {
    check(!detail::ScrollWidgetAccess::state(widget).drag,
          "synchronization retires an externally cancelled drag payload");
  }));
}

void preciseThumbDragging() {
  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture bar({}, axis);
    auto binding = take(bindScrollBar(*bar.router, *bar.tree, bar.bar));

    take(bar.router->setPointerCaptureCapabilities({true, true, true}));
    take(setScrollOffset(*bar.tree, bar.bar, axis, {23, .25}));
    bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
    const auto metrics =
      axis == ScrollAxis::HORIZONTAL ? bar.values().horizontal.metrics : bar.values().vertical.metrics;
    const auto thumb = take(scrollThumb(metrics, 23, 10));
    const auto pressPixel = static_cast<std::uint32_t>(thumb.start * 13 + 5);
    const auto precise = [&](MouseAction action, std::uint32_t pixel, std::uint64_t generation = 9) {
      const auto cell = static_cast<std::int32_t>(pixel / 13);
      InputPointerPrecision sample{axis == ScrollAxis::HORIZONTAL ? pixel : 0,
                                   axis == ScrollAxis::VERTICAL ? pixel : 0,
                                   axis == ScrollAxis::HORIZONTAL ? 10U : 1U,
                                   axis == ScrollAxis::VERTICAL ? 10U : 1U,
                                   axis == ScrollAxis::HORIZONTAL ? 130U : 13U,
                                   axis == ScrollAxis::VERTICAL ? 130U : 13U,
                                   generation};

      return take(bar.router->dispatch(MouseInputEvent{axis == ScrollAxis::HORIZONTAL ? cell : 0,
                                                       axis == ScrollAxis::VERTICAL ? cell : 0,
                                                       MouseButton::LEFT,
                                                       action,
                                                       {},
                                                       sample}));
    };
    const auto offset = [&] {
      return axis == ScrollAxis::HORIZONTAL ? bar.values().horizontal.offset : bar.values().vertical.offset;
    };

    precise(MouseAction::PRESS, pressPixel);
    precise(MouseAction::MOVE, pressPixel);
    check(offset().raster == 23 && offset().fraction == .25,
          "nonzero precise anchor does not jump on unchanged pointer");
    precise(MouseAction::MOVE, pressPixel + 1);
    const auto once = offset();

    check(once.raster == 24 && once.fraction > 0 && once.fraction < 1,
          "one pixel moves within a cell using rational units on both axes");
    precise(MouseAction::MOVE, 10000);
    check(offset().raster == 90 && offset().fraction == 0, "precise drag reaches the exact maximum endpoint");
    precise(MouseAction::MOVE, 0);
    check(offset().raster == 0 && offset().fraction == 0, "precise drag reaches the exact start endpoint");
    precise(MouseAction::MOVE, pressPixel, 10);
    check(take(bar.router->pointerCapture()) == Node<Widget>{}, "changed precision generation cancels before movement");

    take(setScrollOffset(*bar.tree, bar.bar, axis, {1, .5}));
    bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
    precise(MouseAction::PRESS, 5);
    precise(MouseAction::MOVE, 5);
    check(offset().raster == 1 && offset().fraction == .5, "a quantized zero thumb still preserves a nonzero anchor");
    precise(MouseAction::RELEASE, 5);

    const auto maximum = std::numeric_limits<std::uint64_t>::max() - 10;

    take(setScrollMetrics(*bar.tree, bar.bar, {std::numeric_limits<std::uint64_t>::max(), 10}));
    take(setScrollOffset(*bar.tree, bar.bar, axis, {maximum - 1, .5}));
    bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
    const auto wide = take(scrollThumb({std::numeric_limits<std::uint64_t>::max(), 10}, maximum - 1, 10));
    const auto nearEnd = static_cast<std::uint32_t>(wide.start * 13 + 5);

    precise(MouseAction::PRESS, nearEnd);
    precise(MouseAction::MOVE, nearEnd);
    check(offset().raster == maximum - 1 && offset().fraction == .5,
          "precise anchor preserves uint64 values above 2^53");
    precise(MouseAction::MOVE, 10000);
    check(offset().raster == maximum && offset().fraction == 0, "wide precise drag reaches its exact uint64 endpoint");
    precise(MouseAction::RELEASE, 10000);
  }
}

void preciseAnchorIntegerBoundaries() {
  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    for (const bool forward : {true, false}) {
      BarFixture bar({}, axis);
      auto binding = take(bindScrollBar(*bar.router, *bar.tree, bar.bar));
      take(bar.router->setPointerCaptureCapabilities({true, true, true}));
      const ScrollAxisMetrics metrics{forward ? 11U : 12U, 10};
      const ScrollOffset anchor{forward ? 0U : 1U, forward ? 26.0 / 27.0 : 1.0 / 27.0};
      take(setScrollMetrics(*bar.tree, bar.bar, metrics));
      take(setScrollOffset(*bar.tree, bar.bar, axis, anchor));
      bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
      const auto thumb = take(scrollThumb(metrics, anchor.raster, 10));
      const auto pressPixel = static_cast<std::uint32_t>(thumb.start * 27 + 5);
      const auto precise = [&](MouseAction action, std::uint32_t pixel) {
        const auto cell = static_cast<std::int32_t>(pixel / 27);
        InputPointerPrecision sample{axis == ScrollAxis::HORIZONTAL ? pixel : 0,
                                     axis == ScrollAxis::VERTICAL ? pixel : 0,
                                     axis == ScrollAxis::HORIZONTAL ? 10U : 1U,
                                     axis == ScrollAxis::VERTICAL ? 10U : 1U,
                                     axis == ScrollAxis::HORIZONTAL ? 270U : 27U,
                                     axis == ScrollAxis::VERTICAL ? 270U : 27U,
                                     9};
        take(bar.router->dispatch(MouseInputEvent{axis == ScrollAxis::HORIZONTAL ? cell : 0,
                                                  axis == ScrollAxis::VERTICAL ? cell : 0,
                                                  MouseButton::LEFT,
                                                  action,
                                                  {},
                                                  sample}));
      };
      precise(MouseAction::PRESS, pressPixel);
      precise(MouseAction::MOVE, forward ? pressPixel + 1 : pressPixel - 1);
      const auto value = axis == ScrollAxis::HORIZONTAL ? bar.values().horizontal.offset : bar.values().vertical.offset;
      check(value.raster == 0 && value.fraction > 0 && value.fraction < 1,
            forward ? "rounded fraction cannot carry into the exact forward raster"
                    : "exact reverse borrow is preserved when rounded fractions compare equal");
      precise(MouseAction::RELEASE, pressPixel);
    }
  }
}

void dragAndTrackClick() {
  for (auto placement : {ScrollPlacement::RESERVED, ScrollPlacement::OVERLAY, ScrollPlacement::ON_FRAME}) {
    ScrollAreaOptions options;
    options.placement = placement;
    if (placement == ScrollPlacement::ON_FRAME) {
      options.frame = BorderOptions{};
    }
    AreaFixture f(options);
    auto binding = take(bindScrollArea(*f.router, *f.tree, f.area));
    take(f.router->setPointerCaptureCapabilities({true, true, true}));
    f.settle();
    const auto track = take(scrollAreaLayout(*f.tree, f.area)).placement.vertical;
    f.mouse(MouseAction::PRESS, MouseButton::LEFT, track.origin.x, track.origin.y);
    check(take(f.router->pointerCapture()) == f.area, "thumb acquires same-owner capture");
    f.mouse(MouseAction::MOVE, MouseButton::LEFT, track.origin.x + 100, track.origin.y + 100);
    check(f.values().vertical.offset.raster == scrollRange(f.values().vertical.metrics).maximum,
          "drag outside owner reaches exact end");
    f.mouse(MouseAction::MOVE, MouseButton::LEFT, track.origin.x + 100, track.origin.y - 100);
    check(f.values().vertical.offset.raster == 0 && take(f.router->pointerCapture()) == f.area,
          "next queued move after LAYOUT invalidation remains captured and reaches start");
    f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 100, 100);
    check(take(f.router->pointerCapture()) == Node<Widget>{}, "release ends drag exactly once");
    f.settle();
    f.mouse(MouseAction::PRESS, MouseButton::LEFT, track.origin.x, track.origin.y);
    auto observation = take(scrollAreaLayout(*f.tree, f.area));
    f.layout({12, 8});
    check(error(prepareScrollArea(*f.tree, f.area, observation), TreeError::WOULD_BLOCK),
          "stale layout observation rejected");
    take(prepareScrollArea(*f.tree, f.area, take(scrollAreaLayout(*f.tree, f.area))));
    check(take(f.router->pointerCapture()) == Node<Widget>{}, "adopted resize/track change cancels capture");
  }
  BarFixture bar;
  auto binding = take(bindScrollBar(*bar.router, *bar.tree, bar.bar));
  bar.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  check(bar.values().vertical.offset.raster == 9 && take(bar.router->pointerCapture()) == Node<Widget>{},
        "capability-less track click pages once");
  bar.layout({1, 10});
  bar.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 0);
  check(take(bar.router->pointerCapture()) == Node<Widget>{}, "capability-less thumb press does not invent drag");
  take(setScrollOffset(*bar.tree, bar.bar, ScrollAxis::VERTICAL, {}));
  bar.layout({1, 10});
  take(bar.router->setPointerCaptureCapabilities({true, true, true}));
  bar.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 0);
  bar.layout({1, 8});
  bar.mouse(MouseAction::MOVE, MouseButton::LEFT, 0, 7);
  check(take(bar.router->pointerCapture()) == Node<Widget>{} && bar.values().vertical.offset.raster == 0,
        "standalone changed track geometry cancels before moving offset");
}

void sharedMappingChange() {
  TreeFixture f;
  auto controller = take(ScrollController::create());
  auto row = take(f.tree->emplaceChecked<Row>(f.root));
  auto writer = take(f.tree->emplaceChecked<ScrollBar>(row, controller));
  auto projection = take(f.tree->emplaceChecked<ScrollBar>(row, controller));
  take(initializeScrollBar(*f.tree, writer));
  take(setScrollMetrics(*f.tree, writer, {100, 10}));
  auto binding = take(bindScrollBar(*f.router, *f.tree, projection, false));
  take(f.router->setPointerCaptureCapabilities({true, true, true}));
  f.layout({2, 10});
  f.mouse(MouseAction::PRESS, MouseButton::LEFT, 1, 0);
  check(take(f.router->pointerCapture()) == projection, "shared projection acquires its own input capture");
  take(setScrollOffset(*f.tree, writer, ScrollAxis::VERTICAL, {1, 0}));
  f.mouse(MouseAction::MOVE, MouseButton::LEFT, 1, 1);
  check(take(f.router->pointerCapture()) == projection,
        "offset-only shared revision and LAYOUT invalidation preserve the anchor");
  const auto before = take(controller->snapshot()).vertical.offset.raster;
  // The maximum remains 90 and the allocated track remains ten cells. Only the ratio changes.
  take(setScrollMetrics(*f.tree, writer, {200, 110}));
  f.mouse(MouseAction::MOVE, MouseButton::LEFT, 1, 9);
  check(take(f.router->pointerCapture()) == Node<Widget>{} &&
          take(controller->snapshot()).vertical.offset.raster == before,
        "foreign writer extent/viewport changes cancel before using a stale mapping, even with equal range");
}

void hiddenZeroBudgetAndErrors() {
  AreaFixture f;
  f.settle();
  take(setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {8, .5}));
  take(f.tree->setVisible(f.area, false));
  const auto hidden = take(scrollAreaLayout(*f.tree, f.area));
  take(prepareScrollArea(*f.tree, f.area, hidden));
  check(!hidden.visible && f.values().vertical.offset.raster == 8, "hidden preparation preserves position");
  take(f.tree->setVisible(f.area, true));
  f.settle({0, 0});
  check(f.values().vertical.offset.raster == 0 && f.values().vertical.offset.fraction == 0,
        "visible zero viewport really clamps both integral and fractional position");
  ScrollAreaOptions cap;
  cap.frame = BorderOptions{PrimitiveGlyphs::ASCII, 16384};
  cap.maxPaintCells = 1;
  AreaFixture limited(cap);
  limited.settle();
  Sink empty;
  std::expected<void, TreeError> painted;
  take(limited.tree->frame([&] {
    painted = limited.tree->paint(limited.area, empty);
  }));
  check(error(painted, TreeError::WOULD_BLOCK) && empty.submitted == 0,
        "whole paint budget fails before any submission");
  f.settle();
  Sink failing;
  failing.fail = true;
  take(f.tree->frame([&] {
    painted = f.tree->paint(f.area, failing);
  }));
  check(error(painted, TreeError::NODE_LIMIT), "sink typed error propagated unchanged");
  std::expected<void, TreeError> wrongThread;
  std::thread worker([&] {
    wrongThread = setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {});
  });
  worker.join();
  check(error(wrongThread, TreeError::WRONG_THREAD), "programmatic commands preserve tree affinity");
  take(f.tree->close());
  check(error(setScrollOffset(*f.tree, f.area, ScrollAxis::VERTICAL, {}), TreeError::CLOSED),
        "closed command reports CLOSED");
}

thread_local TreeFixture *removalFixture{};
thread_local Node<ScrollArea> removalNode;

void removeDuringAllocation() {
  take(removalFixture->tree->remove(removalNode));
}

void failureTransactions() {
  TreeFixture f;
  auto controller = take(ScrollController::create());
  auto area = take(f.tree->emplaceChecked<ScrollArea>(f.root, controller));
  allocation_test::fail = true;
  auto failed = initializeScrollArea(*f.tree, area);
  allocation_test::fail = false;
  check(error(failed, TreeError::NO_MEMORY), "initialize allocation failure is typed and atomic");
  take(initializeScrollArea(*f.tree, area));
  auto duplicate = take(f.tree->emplaceChecked<ScrollArea>(f.root, controller));
  auto rejected = bindScrollArea(*f.router, *f.tree, duplicate);
  check(error(rejected, TreeError::INVALID_ARGUMENT), "failed bind/initialize rejects second writer");
  take(f.tree->remove(area));
  auto retried = take(bindScrollArea(*f.router, *f.tree, duplicate));
  retried.disconnect();
  take(f.tree->remove(duplicate));
  auto selfRemoving = take(f.tree->emplaceChecked<ScrollArea>(f.root, controller));
  removalFixture = &f;
  removalNode = selfRemoving;
  allocation_test::hook = removeDuringAllocation;
  auto removed = initializeScrollArea(*f.tree, selfRemoving);
  allocation_test::hook = nullptr;
  check(error(removed, TreeError::STALE_NODE), "allocation reentry cannot publish a lease after self-removal");
  auto survivor = take(f.tree->emplaceChecked<ScrollArea>(f.root, controller));
  take(initializeScrollArea(*f.tree, survivor));
  ScrollAreaOptions invalid;
  invalid.placement = ScrollPlacement::ON_FRAME;
  auto bad =
    take(f.tree->emplaceChecked<ScrollArea>(f.root, std::shared_ptr<ScrollController>{}, WidthPolicy{}, invalid));
  check(error(initializeScrollArea(*f.tree, bad), TreeError::INVALID_ARGUMENT), "ON_FRAME without frame is rejected");
}

struct AcquisitionReentry {
  WidgetTree *tree{};
  Node<ScrollBar> node;
  bool writer{};
  bool failAfterInner{};
  std::expected<void, TreeError> result{std::unexpected(TreeError::INVALID_ARGUMENT)};
};

thread_local AcquisitionReentry *acquisitionReentry{};

void initializeDuringAllocation() {
  auto &context = *acquisitionReentry;
  context.result = initializeScrollBar(*context.tree, context.node, context.writer);
  allocation_test::fail = context.failAfterInner;
}

void reentrantWriterAdmission() {
  // Both empty-association allocations can reenter. A retained projection skips the first allocation.
  for (int scenario = 0; scenario < 8; ++scenario) {
    TreeFixture outerTree;
    TreeFixture foreignTree;
    auto controller = take(ScrollController::create());
    const bool retainedProjection = scenario == 2 || scenario == 6;
    const bool foreign = scenario == 5 || scenario == 6;
    const bool outerWriter = scenario != 3;
    const bool innerWriter = scenario != 4;
    const bool failAfterInner = scenario == 7;
    Node<ScrollBar> retained;
    if (retainedProjection) {
      retained = take(outerTree.tree->emplaceChecked<ScrollBar>(outerTree.root, controller));
      take(initializeScrollBar(*outerTree.tree, retained, false));
    }
    auto outer = take(outerTree.tree->emplaceChecked<ScrollBar>(outerTree.root, controller));
    auto &innerTree = foreign ? foreignTree : outerTree;
    auto inner = take(innerTree.tree->emplaceChecked<ScrollBar>(innerTree.root, controller));
    AcquisitionReentry context{innerTree.tree.get(), inner, innerWriter, failAfterInner};
    acquisitionReentry = &context;
    allocation_test::beforeHook = scenario == 1 ? 1 : 0;
    allocation_test::hook = initializeDuringAllocation;
    auto outerResult = initializeScrollBar(*outerTree.tree, outer, outerWriter);
    allocation_test::hook = nullptr;
    allocation_test::beforeHook = 0;
    allocation_test::fail = false;
    acquisitionReentry = nullptr;
    if (scenario == 6) {
      check(error(context.result, TreeError::WRONG_SESSION) && outerResult,
            "existing projection prevents foreign-session inner admission without harming outer writer");
    } else if (scenario == 3 || scenario == 4) {
      check(context.result && outerResult, "same-session writer/projection acquisitions join one winning association");
    } else {
      const auto expected = foreign          ? TreeError::WRONG_SESSION
                            : failAfterInner ? TreeError::NO_MEMORY
                                             : TreeError::INVALID_ARGUMENT;
      check(context.result && error(outerResult, expected),
            "outer acquisition preserves winning inner writer and returns its typed conflict/failure");
    }
    const bool outerWon = scenario == 4 || scenario == 6;
    auto &winnerTree = outerWon ? outerTree : innerTree;
    const auto winner = outerWon ? outer : inner;
    auto written = setScrollMetrics(*winnerTree.tree, winner, {100, 10});
    check(written.has_value(), "winning writer remains capable after outer allocation/conflict handling");
    auto loserWrite = setScrollMetrics(*outerTree.tree, outerWon ? inner : outer, {200, 20});
    if (scenario == 6) {
      loserWrite = setScrollMetrics(*foreignTree.tree, inner, {200, 20});
    }
    check(!loserWrite && take(controller->snapshot()).vertical.metrics.extent == 100 &&
            take(controller->snapshot()).vertical.metrics.viewport == 10,
          "rejected writer/projection cannot overwrite winning metrics");
    if (scenario == 3 || scenario == 4 || retainedProjection) {
      const auto projection = retainedProjection ? retained : outerWon ? inner : outer;
      check(setScrollOffset(*outerTree.tree, projection, ScrollAxis::VERTICAL, {3, 0}).has_value(),
            "same-session projection keeps the actual published association");
    }
    auto third = take(winnerTree.tree->emplaceChecked<ScrollBar>(winnerTree.root, controller));
    check(error(initializeScrollBar(*winnerTree.tree, third), TreeError::INVALID_ARGUMENT),
          "outer cleanup never detaches the live winning writer from controller association");
    take(winnerTree.tree->remove(winner));
    auto replacement = initializeScrollBar(*winnerTree.tree, third);
    check(replacement.has_value(), "writer retirement permits a genuine replacement after reentrant acquisition");
    if (replacement) {
      take(setScrollMetrics(*winnerTree.tree, third, {300, 30}));
      check(take(controller->snapshot()).vertical.metrics.extent == 300,
            "replacement writer publishes only after the previous winner retired");
    }
  }
}

void obsoleteAssociationAdmission() {
  using namespace portals::detail;
  auto controller = take(ScrollController::create({{}, {{100, 10}, {}}}));
  auto current = take(ScrollControllerAccess::acquire(controller, 77, true));
  auto obsoleteSession = std::make_shared<ScrollMountSession>(77);
  auto obsoleteWriter = std::make_shared<ScrollGeometryWriter>();
  obsoleteSession->writer = obsoleteWriter;
  ScrollControllerLease obsolete(controller, obsoleteSession, obsoleteWriter);
  auto plan = take(ScrollControllerAccess::stage(current));
  plan.association = obsoleteSession;
  plan.writer = obsoleteWriter;
  take(ScrollControllerAccess::offset(plan, ScrollAxis::VERTICAL, {5, 0}));
  check(error(ScrollControllerAccess::stage(obsolete), TreeError::WRONG_SESSION),
        "obsolete association cannot stage even when its numeric session matches");
  auto metricsPlan = plan;
  check(error(ScrollControllerAccess::metrics(obsolete, metricsPlan, {}, {200, 20}), TreeError::WRONG_SESSION),
        "obsolete association cannot stage metrics through its own retained writer");
  check(error(ScrollControllerAccess::commit(obsolete, std::move(plan)), TreeError::WRONG_SESSION) &&
          take(controller->snapshot()).vertical.offset.raster == 0,
        "commit validates current controller association before publishing an obsolete lease plan");
}

void advancedFrames() {
  constexpr ScrollAreaOptions LITERAL_OPTIONS{};

  static_assert(!LITERAL_OPTIONS.frame && LITERAL_OPTIONS.horizontal == ScrollBarPolicy::AUTO);

  ScrollAreaOptions options;
  options.placement = ScrollPlacement::ON_FRAME;
  options.glyphs = PrimitiveGlyphs::ASCII;
  FrameOptions frame;

  frame.title = "owned";
  frame.customGlyphs = FrameGlyphs{"a", "b", "c", "d", "=", "!"};

  AreaFixture retained(options, frame);

  frame.title.assign(100, 'z');
  frame.customGlyphs->topLeft = "z";
  frame.customGlyphs->horizontal = "z";
  retained.settle({10, 6});

  const auto painted = retained.paint(retained.area);

  check(painted.cells.at({0, 0}) == "a" && painted.cells.at({1, 0}) == "owned" && painted.cells.at({9, 5}) == "d" &&
          painted.cells.at({9, 1}) == "#",
        "advanced frame owns its compiled title/custom glyphs and preserves scrollbar glyphs");

  struct AllocationFreeSink : PaintContext {
    std::array<unsigned, 60> visits{};
    bool duplicate{};
    std::size_t submitted{};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        ++submitted;

        if (const auto *text = std::get_if<TextCommand>(&command)) {
          for (int i = 0; i < text->text.measure(); ++i) {
            const CellPoint point{text->origin.x + i, text->origin.y};

            if (text->clip.contains(point) && point.x >= 0 && point.x < 10 && point.y >= 0 && point.y < 6) {
              duplicate |= ++visits[static_cast<std::size_t>(point.y * 10 + point.x)] > 1;
            }
          }
        }
      }

      return {};
    }
  } sink;

  std::expected<void, TreeError> result;

  take(retained.tree->frame([&] {
    allocation_test::fail = true;
    result = retained.tree->paint(retained.area, sink);
    allocation_test::fail = false;
  }));
  check(result && sink.submitted != 0 && !sink.duplicate,
        "compiled frame/title/ON_FRAME tracks paint without allocation or duplicate owned cells");

  FrameOptions partial;

  partial.pattern = FramePattern::ASCII;
  partial.sides = {false, true, true, false};

  AreaFixture asymmetric(options, partial);

  asymmetric.settle({10, 6});

  const auto layout = take(scrollAreaLayout(*asymmetric.tree, asymmetric.area));
  const auto child = take(asymmetric.tree->inspect(asymmetric.content));

  check(layout.placement.viewportOrigin == CellPoint{} && layout.placement.viewport == CellSize{9, 5} &&
          child.geometry.clip.origin() == CellPoint{} && child.geometry.clip.size() == CellSize{9, 5},
        "partial frame measures, arranges and clips using its independent insets");
  check(layout.placement.horizontal.origin == CellPoint{0, 5} && layout.placement.horizontal.length == 9 &&
          layout.placement.vertical.origin == CellPoint{9, 0} && layout.placement.vertical.length == 5,
        "partial ON_FRAME excludes only its bottom-right corner");

  ScrollAreaOptions horizontalOnly = options;

  horizontalOnly.vertical = ScrollBarPolicy::NEVER;
  partial.sides = {false, false, true, false};

  AreaFixture collapsed(horizontalOnly, partial);

  collapsed.settle({5, 1});

  const auto tiny = take(scrollAreaLayout(*collapsed.tree, collapsed.area));
  const auto tinyPaint = collapsed.paint(collapsed.area);

  check(tiny.placement.horizontal.origin == CellPoint{0, 0} && tiny.placement.horizontal.length == 5 &&
          tinyPaint.submitted == 5 && tinyPaint.cells.size() == 5,
        "single bottom side owns a collapsed row without a second frame submission");

  for (const bool frameBudget : {false, true}) {
    FrameOptions bounded;
    ScrollAreaOptions boundedArea = options;

    bounded.title.assign(100, 'x');

    if (frameBudget) {
      bounded.border.maxPaintCells = 40;
    } else {
      boundedArea.maxPaintCells = 40;
    }

    AreaFixture limited(boundedArea, bounded);

    limited.settle({10, 6});

    Sink empty;

    take(limited.tree->frame([&] {
      result = limited.tree->paint(limited.area, empty);
    }));
    check(error(result, TreeError::WOULD_BLOCK) && empty.submitted == 0,
          "complete clipped title work respects frame and whole-area caps before any submission");
  }

  FrameOptions invalid;

  invalid.sides.bottom = false;

  AreaFixture rejected(options, invalid, {20, 30}, false);

  check(error(initializeScrollArea(*rejected.tree, rejected.area), TreeError::INVALID_ARGUMENT),
        "possible ON_FRAME horizontal bar rejects a missing bottom before initialization");

  auto replacement = take(rejected.tree->emplaceChecked<ScrollArea>(rejected.root, rejected.controller));

  check(initializeScrollArea(*rejected.tree, replacement).has_value(),
        "invalid advanced configuration acquires no controller writer lease");

  invalid = {};
  invalid.customGlyphs = FrameGlyphs{"ab", "+", "+", "+", "-", "|"};

  AreaFixture invalidText({}, invalid, {20, 30}, false);

  check(error(initializeScrollArea(*invalidText.tree, invalidText.area), TreeError::INVALID_ARGUMENT),
        "invalid custom glyph is retained as a typed preparation failure");

  TreeFixture oom;
  FrameOptions owning;

  owning.title.assign(100, 'x');
  allocation_test::fail = true;

  const auto failed =
    oom.tree->emplaceChecked<ScrollArea>(oom.root, std::shared_ptr<ScrollController>{}, WidthPolicy{},
                                         ScrollAreaOptions{}, ControlStyle{}, SizePolicy{}, owning, std::in_place);

  allocation_test::fail = false;
  check(error(failed, TreeError::NO_MEMORY), "advanced owning construction preserves typed NO_MEMORY");
}

void scrollbarSkins() {
  for (auto placement : {ScrollPlacement::RESERVED, ScrollPlacement::OVERLAY, ScrollPlacement::ON_FRAME}) {
    for (auto pattern :
         {ScrollbarPattern::LEGACY, ScrollbarPattern::FRAME, ScrollbarPattern::MINIMAL, ScrollbarPattern::ASCII}) {
      ScrollAreaOptions options;
      options.frame = BorderOptions{};
      options.placement = placement;
      FrameOptions frame;
      frame.pattern = FramePattern::DOUBLE;
      AreaFixture fixture(options, frame);
      fixture.settle({10, 6});
      take(setScrollOffset(*fixture.tree, fixture.area, ScrollAxis::HORIZONTAL, {5, 0}));
      take(setScrollOffset(*fixture.tree, fixture.area, ScrollAxis::VERTICAL, {8, 0}));
      fixture.settle({10, 6});
      const auto before = take(scrollAreaLayout(*fixture.tree, fixture.area));
      const auto values = fixture.values();
      ScrollbarSkin skin{.pattern = pattern, .ends = true};
      take(fixture.tree->consumeFrameRequest());
      take(setScrollbarSkin(*fixture.tree, fixture.area, skin));
      check(take(fixture.tree->consumeFrameRequest()) == Invalidation::PAINT,
            "skin change requests paint without invalidating layout");
      const auto after = take(scrollAreaLayout(*fixture.tree, fixture.area));
      const auto afterValues = fixture.values();
      check(before.layoutRevision == after.layoutRevision && before.geometry.local == after.geometry.local &&
              before.geometry.surface == after.geometry.surface && before.geometry.clip == after.geometry.clip &&
              before.geometry.valid == after.geometry.valid && before.placement.viewport == after.placement.viewport &&
              before.placement.viewportOrigin == after.placement.viewportOrigin &&
              afterValues.revision == values.revision &&
              afterValues.horizontal.offset.raster == values.horizontal.offset.raster &&
              afterValues.horizontal.offset.fraction == values.horizontal.offset.fraction &&
              afterValues.vertical.offset.raster == values.vertical.offset.raster &&
              afterValues.vertical.offset.fraction == values.vertical.offset.fraction,
            "skin preserves geometry, content viewport, controller revision and offsets");
      const auto painted = fixture.paint(fixture.area);
      for (const bool horizontal : {true, false}) {
        const auto track = horizontal ? before.placement.horizontal : before.placement.vertical;
        const auto value = horizontal ? values.horizontal : values.vertical;
        const auto thumb = take(scrollThumb(value.metrics, value.offset.raster, track.length, 1));
        for (int i = 0; i < track.length; ++i) {
          const bool inThumb = i >= thumb.start && i - thumb.start < thumb.length;
          const auto point = std::pair{track.origin.x + (horizontal ? i : 0), track.origin.y + (horizontal ? 0 : i)};
          const auto expected = inThumb                 ? (pattern == ScrollbarPattern::ASCII ? "#" : "\xe2\x96\x88")
                                : i == 0                ? (horizontal ? "<" : "^")
                                : i == track.length - 1 ? (horizontal ? ">" : "v")
                                : pattern == ScrollbarPattern::MINIMAL ? " "
                                : pattern == ScrollbarPattern::ASCII   ? "."
                                : pattern == ScrollbarPattern::FRAME   ? (horizontal ? "\xe2\x95\x90" : "\xe2\x95\x91")
                                                                       : "\xe2\x96\x91";
          check(painted.cells.at(point) == expected, "oriented preset/ends/thumb paint uses the existing track cells");
        }
      }
      take(fixture.tree->consumeFrameRequest());
      fixture.paint(fixture.area);
      check(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE,
            "skin paint does not request continuous redraw");
    }
  }

  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture fixture({}, axis);
    ScrollbarSkin skin{.pattern = ScrollbarPattern::FRAME, .ends = true};
    take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));
    auto painted = fixture.paint(fixture.bar);
    const auto last = axis == ScrollAxis::HORIZONTAL ? std::pair{9, 0} : std::pair{0, 9};
    check(painted.cells.at(last) == (axis == ScrollAxis::HORIZONTAL ? ">" : "v"),
          "standalone oriented ends are decorative");
    take(setScrollMetrics(*fixture.tree, fixture.bar, {10, 10}));
    fixture.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
    painted = fixture.paint(fixture.bar);
    check(painted.cells.at(last) == "\xe2\x96\x88", "fitting content thumb covers decorative ends");
    for (int length : {0, 1, 2}) {
      fixture.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{length, 1} : CellSize{1, length});
      check(fixture.paint(fixture.bar).submitted == static_cast<std::size_t>(length),
            "tiny bar has bounded exact paint work");
    }
  }

  FrameOptions frame;
  frame.customGlyphs = FrameGlyphs{"+", "+", "+", "+", "H", "V"};
  ScrollAreaOptions options{.placement = ScrollPlacement::ON_FRAME, .frame = BorderOptions{}};
  AreaFixture fixture(options, frame);
  fixture.settle();
  ScrollbarSkin skin{.pattern = ScrollbarPattern::FRAME};
  skin.horizontal.thumb = "x";
  skin.vertical.thumb = "y";
  skin.corner = "@";
  take(setScrollbarSkin(*fixture.tree, fixture.area, skin));
  skin.horizontal.thumb = "!";
  skin.vertical.thumb = "!";
  skin.corner = "!";
  const auto painted = fixture.paint(fixture.area);
  const auto geometry = take(scrollAreaLayout(*fixture.tree, fixture.area)).placement;
  check(painted.cells.at({geometry.horizontal.origin.x, geometry.horizontal.origin.y}) == "x" &&
          painted.cells.at({geometry.vertical.origin.x, geometry.vertical.origin.y}) == "y" &&
          painted.cells.at(
            {geometry.horizontal.origin.x + geometry.horizontal.length - 1, geometry.horizontal.origin.y}) == "H" &&
          painted.cells.at({geometry.vertical.origin.x, geometry.vertical.origin.y + geometry.vertical.length - 1}) ==
            "V" &&
          !geometry.hasCorner && painted.cells.at({0, 0}) == "+",
        "custom sources are owned, FRAME matches actual lines and ON_FRAME preserves outline corners");
  options.placement = ScrollPlacement::RESERVED;
  AreaFixture corners(options, frame);
  corners.settle();
  skin.corner = "@";
  take(setScrollbarSkin(*corners.tree, corners.area, skin));
  const auto corner = take(scrollAreaLayout(*corners.tree, corners.area)).placement;
  check(corner.hasCorner && corners.paint(corners.area).cells.at({corner.corner.x, corner.corner.y}) == "@",
        "custom corner applies to the existing reserved gutter intersection");
}

void segmentedScrollbarSkins() {
  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture fixture({}, axis);
    const auto cell = [&](int index) {
      return axis == ScrollAxis::HORIZONTAL ? std::pair{index, 0} : std::pair{0, index};
    };
    ScrollbarSkin skin{.pattern = ScrollbarPattern::ASCII};
    skin.trackPattern = ScrollbarPattern::MINIMAL;
    auto &glyphs = axis == ScrollAxis::HORIZONTAL ? skin.horizontal : skin.vertical;
    glyphs.thumb = "x";
    glyphs.thumbStart = "[";
    glyphs.thumbBody = "=";
    glyphs.thumbEnd = "]";
    glyphs.thumbSingle = "o";
    take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));

    for (const int length : {0, 1, 2, 3, 4}) {
      take(setScrollMetrics(*fixture.tree, fixture.bar, {10, 10}));
      fixture.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{length, 1} : CellSize{1, length});
      const auto painted = fixture.paint(fixture.bar);
      check(painted.submitted == static_cast<std::size_t>(length), "segments preserve exact tiny-channel work");

      if (length == 1) {
        check(painted.cells.at(cell(0)) == "o", "one-cell thumb selects single rather than two caps");
      } else if (length >= 2) {
        check(painted.cells.at(cell(0)) == "[" && painted.cells.at(cell(length - 1)) == "]",
              "two-cell and longer thumbs use distinct oriented caps");

        for (int index = 1; index + 1 < length; ++index) {
          check(painted.cells.at(cell(index)) == "=", "thumb interior uses prepared body");
        }
      }
    }

    take(setScrollMetrics(*fixture.tree, fixture.bar, {100, 40}));
    take(setScrollOffset(*fixture.tree, fixture.bar, axis, {30, 0}));
    fixture.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{10, 1} : CellSize{1, 10});
    const auto moved = fixture.paint(fixture.bar);
    check(moved.cells.at(cell(3)) == "[" && moved.cells.at(cell(4)) == "=" && moved.cells.at(cell(6)) == "]" &&
            moved.cells.at(cell(9)) == " ",
          "segments follow the moved thumb rather than the channel endpoints");

    glyphs.thumbBody.reset();
    glyphs.thumbStart.reset();
    glyphs.thumbEnd.reset();
    glyphs.thumbSingle.reset();
    take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));
    const auto legacy = fixture.paint(fixture.bar);
    check(legacy.cells.at(cell(3)) == "x" && legacy.cells.at(cell(6)) == "x",
          "old repeated thumb is the fallback for every absent segment");

    for (auto member : {&ScrollbarAxisGlyphs::thumbStart, &ScrollbarAxisGlyphs::thumbBody,
                        &ScrollbarAxisGlyphs::thumbEnd, &ScrollbarAxisGlyphs::thumbSingle}) {
      for (const auto &invalid :
           {std::string{}, std::string{"ab"}, std::string{"\n"}, std::string{"\xe7\x95\x8c"}, std::string(65, 'a')}) {
        glyphs.*member = invalid;
        check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::INVALID_ARGUMENT),
              "invalid custom segment rejects before publication");
        check(fixture.paint(fixture.bar).cells == legacy.cells, "failed segment preparation preserves the old skin");
      }

      (glyphs.*member).reset();
    }
  }

  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture fixture({.glyphs = PrimitiveGlyphs::ASCII, .arrows = true}, axis);
    ScrollbarSkin skin{.pattern = ScrollbarPattern::ASCII};
    skin.trackPattern = ScrollbarPattern::MINIMAL;
    skin.thumbPattern = ScrollbarThumbPattern::BLOCK;
    skin.arrowPattern = ScrollbarArrowPattern::TRIANGLE;
    take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));
    const auto painted = fixture.paint(fixture.bar);
    check(painted.cells.at({0, 0}) == (axis == ScrollAxis::HORIZONTAL ? "<" : "^"),
          "triangle preset respects explicit ASCII mode");
    check(painted.cells.at(axis == ScrollAxis::HORIZONTAL ? std::pair{1, 0} : std::pair{0, 1}) == "#",
          "block preset retains the existing ASCII fallback");
    check(painted.cells.at(axis == ScrollAxis::HORIZONTAL ? std::pair{8, 0} : std::pair{0, 8}) == " ",
          "independent minimal track survives umbrella ASCII");
  }
}

void segmentedScrollbarStyles() {
  const auto sheet = take(parsePss(R"pss(* {
    scrollbar-thumb-foreground:#010203; scrollbar-thumb-background:#040506;
    scrollbar-thumb-bold:true; scrollbar-thumb-italic:true; scrollbar-thumb-underline:true;
    scrollbar-thumb-start-foreground:#0a0b0c; scrollbar-thumb-start-bold:false;
    scrollbar-thumb-body-background:#141516; scrollbar-thumb-body-italic:false;
    scrollbar-thumb-end-foreground:#1e1f20; scrollbar-thumb-end-underline:false;
    scrollbar-thumb-single-foreground:#28292a; scrollbar-thumb-single-bold:false;
  } *:focused {scrollbar-thumb-background:#323334;}
    *:disabled {scrollbar-thumb-foreground:#3c3d3e;})pss"));
  const auto verify = [&](auto &fixture, Node<Widget> control, int length, bool custom) {
    if (custom) {
      ScrollbarSkin skin;
      skin.horizontal = {.thumbStart = "[", .thumbBody = "=", .thumbEnd = "]", .thumbSingle = "o"};
      skin.vertical = {.thumbStart = "A", .thumbBody = "I", .thumbEnd = "V", .thumbSingle = "O"};
      take(setScrollbarSkin(*fixture.tree, control, skin));
    }

    StyleEngine engine(*fixture.tree);
    take(engine.bind(control, {"Control", "", {}, true}));
    take(engine.replace(sheet));

    for (int state = 0; state < 3; ++state) {
      if (state == 2) {
        take(fixture.tree->setEnabled(control, false));
      }

      take(syncScrollControl(*fixture.tree, control, state == 1 ? control : Node<Widget>{}));
      const CellStyle common{state == 2 ? RgbColor{60, 61, 62} : RgbColor{1, 2, 3},
                             state == 1 ? RgbColor{50, 51, 52} : RgbColor{4, 5, 6}, true, true, true};
      struct SegmentSink : PaintContext {
        std::array<CellStyle, 4> expected;
        int length{};
        int cells{};
        bool correct{true};

        std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
          for (const auto &command : commands) {
            const auto *text = std::get_if<TextCommand>(&command);
            const auto segment = length == 1 ? 3 : cells == 0 ? 0 : cells == length - 1 ? 2 : 1;
            correct = correct && text && text->style == expected[segment];
            ++cells;
          }

          return {};
        }
      } sink;
      sink.length = length;
      sink.expected.fill(common);
      sink.expected[0].foreground = RgbColor{10, 11, 12};
      sink.expected[0].bold = false;
      sink.expected[1].background = RgbColor{20, 21, 22};
      sink.expected[1].italic = false;
      sink.expected[2].foreground = RgbColor{30, 31, 32};
      sink.expected[2].underline = false;
      sink.expected[3].foreground = RgbColor{40, 41, 42};
      sink.expected[3].bold = false;
      std::expected<void, TreeError> painted;

      take(fixture.tree->frame([&] {
        allocation_test::fail = true;
        painted = fixture.tree->paint(control, sink);
        allocation_test::fail = false;
      }));
      check(painted && sink.correct && sink.cells == length,
            "both scroll controls select exact segment styles for both axes and states without paint allocation");
    }
  };

  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    for (const int length : {0, 1, 2, 4}) {
      for (const bool custom : {false, true}) {
        BarFixture bar({}, axis);
        take(setScrollMetrics(*bar.tree, bar.bar, {10, 10}));
        bar.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{length, 1} : CellSize{1, length});
        verify(bar, bar.bar, length, custom);

        ScrollAreaOptions options;
        options.horizontal = axis == ScrollAxis::HORIZONTAL ? ScrollBarPolicy::ALWAYS : ScrollBarPolicy::NEVER;
        options.vertical = axis == ScrollAxis::VERTICAL ? ScrollBarPolicy::ALWAYS : ScrollBarPolicy::NEVER;
        AreaFixture area(options, axis == ScrollAxis::HORIZONTAL ? CellSize{length, 1} : CellSize{1, length});
        area.settle(axis == ScrollAxis::HORIZONTAL ? CellSize{length, 2} : CellSize{2, length});
        verify(area, area.area, length, custom);
      }
    }
  }
}

void scrollbarSkinFailures() {
  TreeFixture widths;
  const auto wide = take(widths.tree->emplaceChecked<ScrollBar>(
    widths.root, std::shared_ptr<ScrollController>{}, ScrollAxis::VERTICAL, WidthPolicy{AmbiguousWidth::WIDE}));
  ScrollbarSkin ambiguous;
  ambiguous.vertical.track = "\xc2\xb7";
  check(error(setScrollbarSkin(*widths.tree, wide, ambiguous), TreeError::INVALID_ARGUMENT),
        "custom glyph admission uses the complete receiving ambiguous-width policy");
  ambiguous.vertical.track.reset();
  ambiguous.vertical.thumbSingle = "\xc2\xb7";
  check(error(setScrollbarSkin(*widths.tree, wide, ambiguous), TreeError::INVALID_ARGUMENT),
        "custom segment admission respects receiving ambiguous-width policy");
  take(setScrollbarSkin(*widths.tree, wide, ScrollbarSkin{.pattern = ScrollbarPattern::FRAME}));
  BarFixture fixture;
  ScrollbarSkin skin{.pattern = ScrollbarPattern::ASCII};
  skin.vertical.thumbSingle = "o";
  skin.vertical.thumbBody = "I";
  take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));

  struct CountingPaint final : PaintContext {
    std::size_t submitted{};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      submitted += commands.size();

      return {};
    }
  } allocationFree;

  std::expected<void, TreeError> painted;
  take(fixture.tree->frame([&] {
    allocation_test::fail = true;
    painted = fixture.tree->paint(fixture.bar, allocationFree);
    allocation_test::fail = false;
  }));
  check(painted && allocationFree.submitted == 10, "prepared scrollbar glyphs paint without owning allocation");
  const auto before = fixture.paint(fixture.bar).cells;
  for (const std::string &source :
       {std::string(""), std::string("ab"), std::string("\xe7\x95\x8c"), std::string("\xcc\x81"), std::string("\n"),
        std::string(1, '\0'), std::string("\xff"), std::string(65, 'x')}) {
    skin.vertical.track = source;
    take(fixture.tree->consumeFrameRequest());
    check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::INVALID_ARGUMENT),
          "empty, multiple, wide, zero-width, control, NUL, malformed and over-cap glyphs reject before paint");
    check(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE && fixture.paint(fixture.bar).cells == before,
          "invalid skin preserves old preparation without invalidation");
  }
  skin.vertical.track = "e\xcc\x81";
  skin.maxGlyphBytes = 2;
  check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::INVALID_ARGUMENT),
        "configured per-glyph byte cap applies");
  skin.maxGlyphBytes = 3;
  take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));
  check(fixture.paint(fixture.bar).cells.at({0, 9}) == "e\xcc\x81", "one combining grapheme remains indivisible");
  skin.vertical.track.reset();
  skin.maxGlyphBytes = 0;
  check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::INVALID_ARGUMENT),
        "zero glyph cap rejects");
  skin.maxGlyphBytes = 64;
  skin.pattern = static_cast<ScrollbarPattern>(-1);
  check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::INVALID_ARGUMENT),
        "unknown native preset rejects");
  skin.pattern = ScrollbarPattern::ASCII;
  const auto previous = fixture.paint(fixture.bar).cells;
  allocation_test::fail = true;
  const auto failed = setScrollbarSkin(*fixture.tree, fixture.bar, skin);
  allocation_test::fail = false;
  check(error(failed, TreeError::NO_MEMORY) && fixture.paint(fixture.bar).cells == previous,
        "new fallible preparation translates allocation failure and preserves old skin");
  take(fixture.tree->frame([&] {
    check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::WOULD_BLOCK),
          "skin replacement is forbidden during paint frames");
  }));
  std::thread other([&] {
    check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::WRONG_THREAD),
          "skin replacement is UI-affine");
  });
  other.join();
  check(error(setScrollbarSkin(*fixture.tree, fixture.root, skin), TreeError::INVALID_ARGUMENT),
        "skin rejects unrelated controls");
  static BarFixture *entered;
  entered = &fixture;
  allocation_test::hook = [] {
    ScrollbarSkin nested{.pattern = ScrollbarPattern::MINIMAL};
    take(setScrollbarSkin(*entered->tree, entered->bar, nested));
  };
  allocation_test::beforeHook = 0;
  check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::WOULD_BLOCK),
        "nested skin publication wins over stale outer preparation");
  check(fixture.paint(fixture.bar).cells.at({0, 9}) == " ", "newer nested preparation remains installed");
  allocation_test::hook = [] {
    take(entered->tree->remove(entered->bar));
  };
  allocation_test::beforeHook = 0;
  check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::STALE_NODE),
        "removed owner receives no late preparation");
  check(error(setScrollbarSkin(*fixture.tree, fixture.bar, skin), TreeError::STALE_NODE), "stale skin target rejects");
}

void manualStyleAndAutomaticMetadata() {
  BarFixture f;
  take(syncScrollControl(*f.tree, f.bar, f.bar));
  auto focused = f.paint(f.bar);
  check(!focused.styles.empty() && focused.styles[0].bold && !focused.styles[0].underline,
        "focus presentation uses neutral bold without underline");
  take(f.tree->setEnabled(f.bar, false));
  take(syncScrollControl(*f.tree, f.bar, f.bar));
  f.layout({1, 10});
  auto disabled = f.paint(f.bar);
  check(!disabled.styles.empty() && disabled.styles[0].italic, "disabled presentation overrides focus");
  take(f.tree->consumeFrameRequest());
  take(syncScrollControl(*f.tree, f.bar, f.bar));
  check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE,
        "equal presentation sync does not create idle frames");
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto area = scene->emplace<ScrollArea>(scene->root());
  auto bar = scene->emplace<ScrollBar>(scene->root());
  check(area.has_value() && bar.has_value(), "managed style-only mount succeeds without runtime activation");
  StyleSheet sheet;
  StyleRule areaRule;
  areaRule.selector.type = "ScrollArea";
  areaRule.declarations.foreground = RgbColor{11, 22, 33};
  sheet.rules.push_back(areaRule);
  StyleRule barRule;
  barRule.selector.type = "ScrollBar";
  barRule.declarations.foreground = RgbColor{44, 55, 66};
  sheet.rules.push_back(barRule);
  check(scene->theme(sheet).has_value(), "theme accepts automatic targets");
  check(scene->prepare(*app, {20, 10}).has_value(), "style-only preparation succeeds");
  take(app->tree().read(*area, [](const ScrollArea &widget) {
    check(widget.computedStyle() && widget.computedStyle()->normal.foreground == RgbColor{11, 22, 33},
          "ScrollArea auto metadata resolves type-specific theme");
  }));
  take(app->tree().read(*bar, [](const ScrollBar &widget) {
    check(widget.computedStyle() && widget.computedStyle()->normal.foreground == RgbColor{44, 55, 66},
          "standalone ScrollBar auto metadata resolves type-specific theme");
  }));
}

void functionalArrowGeometry() {
  for (auto axis : {ScrollAxis::VERTICAL, ScrollAxis::HORIZONTAL}) {
    BarFixture f({.keyStep = 2, .glyphs = PrimitiveGlyphs::ASCII, .arrows = true}, axis);
    auto binding = take(bindScrollBar(*f.router, *f.tree, f.bar));
    auto paint = f.paint(f.bar);
    const auto cell = [&](int offset) {
      return axis == ScrollAxis::VERTICAL ? std::pair{0, offset} : std::pair{offset, 0};
    };
    check(paint.cells.at(cell(0)) == (axis == ScrollAxis::VERTICAL ? "^" : "<") &&
            paint.cells.at(cell(9)) == (axis == ScrollAxis::VERTICAL ? "v" : ">"),
          "arrows reserve separate end cells");
    check(paint.cells.at(cell(1)) == "#", "thumb begins after decrement button");
    f.mouse(MouseAction::PRESS, MouseButton::LEFT, cell(9).first, cell(9).second);
    const auto values = f.values();
    check((axis == ScrollAxis::VERTICAL ? values.vertical.offset.raster : values.horizontal.offset.raster) == 2,
          "old binding arrow click uses keyStep without timer services");
    for (int length = 0; length <= 3; ++length) {
      f.layout(axis == ScrollAxis::VERTICAL ? CellSize{1, length} : CellSize{length, 1});
      const auto small = f.paint(f.bar);
      check(small.submitted == static_cast<std::size_t>(length), "short channels keep their exact paint budget");
      if (length > 0) {
        check((small.cells.at(cell(0)) == (axis == ScrollAxis::VERTICAL ? "^" : "<")) == (length == 3),
              "functional arrow buttons collapse below three cells");
      }
    }
  }
  BarFixture readonly({.glyphs = PrimitiveGlyphs::ASCII, .readOnly = true, .arrows = true});
  auto binding = take(bindScrollBar(*readonly.router, *readonly.tree, readonly.bar));
  readonly.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  check(readonly.values().vertical.offset.raster == 0, "readonly arrow cannot change offset");
  take(setScrollOffset(*readonly.tree, readonly.bar, ScrollAxis::VERTICAL, {7, 0}));
  check(readonly.values().vertical.offset.raster == 7, "readonly checked commands stay available");
}

void heldArrows() {
  int errors{};
  BarFixture f({.keyStep = 2, .glyphs = PrimitiveGlyphs::ASCII, .arrows = true});
  auto binding = take(bindScrollBar(*f.router, *f.tree, f.bar,
                                    {[&] {
                                       return f.now;
                                     },
                                     [&](TreeError) {
                                       ++errors;
                                     }}));
  take(f.router->setPointerCaptureCapabilities({true, true, true}));
  f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  check(f.values().vertical.offset.raster == 2, "arrow press takes one immediate step");
  f.now = 349;
  check(take(f.scheduler->dispatch()).timers == 0 && f.values().vertical.offset.raster == 2,
        "hold waits full initial delay");
  f.now = 350;
  check(take(f.scheduler->dispatch()).timers == 1 && f.values().vertical.offset.raster == 4,
        "hold repeats at initial deadline");
  f.now = 410;
  check(take(f.scheduler->dispatch()).timers == 1 && f.values().vertical.offset.raster == 6,
        "hold repeats at configured interval");
  f.now = 1000;
  check(take(f.scheduler->dispatch()).timers == 1 && f.values().vertical.offset.raster == 8,
        "delayed dispatch has no catch-up burst");
  f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 0, 9);
  f.now = 2000;
  check(take(f.scheduler->dispatch()).timers == 0 && f.values().vertical.offset.raster == 8,
        "release cancels pending repeat");
  take(setScrollOffset(*f.tree, f.bar, ScrollAxis::VERTICAL, {89, 0}));
  f.layout({1, 10});
  f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  check(f.values().vertical.offset.raster == 90, "final arrow step clamps exactly");
  f.layout({1, 10});
  take(f.tree->consumeFrameRequest());
  const auto revision = f.values().revision;
  f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  f.now = 3000;
  check(take(f.scheduler->dispatch()).timers == 0 && f.values().revision == revision &&
          take(f.tree->consumeFrameRequest()) == Invalidation::NONE,
        "bound arrow adds no timer, publication or redraw");
  take(setScrollOffset(*f.tree, f.bar, ScrollAxis::VERTICAL, {87, 0}));
  f.layout({1, 10});
  f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  f.now = 3350;
  check(take(f.scheduler->dispatch()).timers == 1 && f.values().vertical.offset.raster == 90,
        "held arrow stops as soon as a repeat reaches the bound");
  f.now = 4000;
  check(take(f.scheduler->dispatch()).timers == 0, "reaching the bound never schedules another repeat");
  check(errors == 0, "ordinary arrow hold has no asynchronous errors");

  BarFixture horizontal({.arrows = true}, ScrollAxis::HORIZONTAL);
  auto horizontalBinding = take(bindScrollBar(*horizontal.router, *horizontal.tree, horizontal.bar,
                                              {[&] {
                                                 return horizontal.now;
                                               },
                                               [](TreeError) {
                                               }}));
  take(horizontal.router->setPointerCaptureCapabilities({true, true, true}));
  horizontal.mouse(MouseAction::PRESS, MouseButton::LEFT, 9, 0);
  horizontal.now = 350;
  take(horizontal.scheduler->dispatch());
  check(horizontal.values().horizontal.offset.raster == 2,
        "horizontal held arrow uses horizontal metrics and position");
  horizontal.mouse(MouseAction::RELEASE, MouseButton::LEFT, 9, 0);

  BarFixture clickOnly({.arrows = true});
  auto clickBinding = take(bindScrollBar(*clickOnly.router, *clickOnly.tree, clickOnly.bar,
                                         {[&] {
                                            return clickOnly.now;
                                          },
                                          [](TreeError) {
                                          }}));
  clickOnly.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  clickOnly.now = 1000;
  check(take(clickOnly.scheduler->dispatch()).timers == 0 && clickOnly.values().vertical.offset.raster == 1,
        "missing host capture support preserves one click without timer admission");
}

void arrowCancellationAndLifetime() {
  for (int mode = 0; mode < 7; ++mode) {
    int errors{};
    BarFixture f({.arrows = true});
    auto binding = take(bindScrollBar(*f.router, *f.tree, f.bar,
                                      {[&] {
                                         return f.now;
                                       },
                                       [&](TreeError) {
                                         ++errors;
                                       }}));
    take(f.router->setPointerCaptureCapabilities({true, true, true}));
    f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
    if (mode == 0)
      take(f.router->cancelPointerCapture());
    if (mode == 1)
      take(f.tree->setVisible(f.bar, false));
    if (mode == 2)
      take(f.tree->setEnabled(f.bar, false));
    if (mode == 3)
      take(f.tree->remove(f.bar));
    if (mode == 4)
      f.layout({1, 8});
    if (mode == 5)
      f.mouse(MouseAction::MOVE, MouseButton::LEFT, 0, 8);
    if (mode == 6)
      binding.disconnect();
    f.now = 350;
    take(f.scheduler->dispatch());
    check(f.values().vertical.offset.raster == 1 && errors == 0,
          "capture loss, eligibility, removal and resize stop hold");
    f.now = 1000;
    check(take(f.scheduler->dispatch()).timers == 0, "cancelled hold never rearms");
  }
  BarFixture removed({.arrows = true});
  auto binding = take(bindScrollBar(*removed.router, *removed.tree, removed.bar,
                                    {[&] {
                                       take(removed.tree->remove(removed.bar));
                                       return removed.now;
                                     },
                                     [](TreeError) {
                                     }}));
  take(removed.router->setPointerCaptureCapabilities({true, true, true}));
  removed.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  check(removed.values().vertical.offset.raster == 0, "reentrant removal from now prevents late initial publication");
  removed.now = 1000;
  check(take(removed.scheduler->dispatch()).timers == 0, "reentrant removal leaves no scheduled widget callback");
  check(error(validateScrollOptions(ScrollBarOptions{.arrowDelay = 0}), TreeError::INVALID_ARGUMENT) &&
          error(validateScrollOptions(ScrollAreaOptions{.arrowRepeat = 0}), TreeError::INVALID_ARGUMENT),
        "zero timing rejects");
}

void rebindHeldArrowServices() {
  for (bool pending : {false, true}) {
    for (bool replacementServices : {false, true}) {
      int firstClock{};
      int replacementClock{};
      BarFixture f({.arrows = true});
      auto first = take(bindScrollBar(*f.router, *f.tree, f.bar,
                                      {[&] {
                                         ++firstClock;
                                         return f.now;
                                       },
                                       [](TreeError) {
                                       }}));
      take(f.router->setPointerCaptureCapabilities({true, true, true}));
      if (pending) {
        f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
        f.layout({1, 10});
      }
      std::thread disconnect([&] {
        first.disconnect();
      });
      disconnect.join();
      const auto oldClockCalls = firstClock;
      auto replacement = replacementServices ? bindScrollBar(*f.router, *f.tree, f.bar,
                                                             ScrollRepeatServices{[&] {
                                                                                    ++replacementClock;
                                                                                    return f.now;
                                                                                  },
                                                                                  [](TreeError) {
                                                                                  }})
                                             : bindScrollBar(*f.router, *f.tree, f.bar);
      check(replacement.has_value(), "disconnected arrow services binding permits replacement");
      if (!replacement) {
        continue;
      }
      f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
      const auto pressedOffset = f.values().vertical.offset.raster;
      f.now = 350;
      take(f.scheduler->dispatch());
      check(firstClock == oldClockCalls, "rebound input never invokes retired services A");
      check(f.values().vertical.offset.raster == pressedOffset + (replacementServices ? 1 : 0),
            "only replacement services B repeat; old overload remains click-only");
      check(replacementClock == (replacementServices ? 2 : 0),
            "replacement clock exclusively owns initial and rearm admission");
      f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 0, 9);
      f.now = 1000;
      check(take(f.scheduler->dispatch()).timers == 0, "pending services A timer cannot drive a later binding");
    }
  }
}

void activeArrowBindingRejectsReplacementTransactionally() {
  int firstClock{};
  int rejectedClock{};
  BarFixture f({.arrows = true});
  auto first = take(bindScrollBar(*f.router, *f.tree, f.bar,
                                  {[&] {
                                     ++firstClock;
                                     return f.now;
                                   },
                                   [](TreeError) {
                                   }}));
  take(f.router->setPointerCaptureCapabilities({true, true, true}));
  f.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  const auto rejected = bindScrollBar(*f.router, *f.tree, f.bar,
                                      ScrollRepeatServices{[&] {
                                                             ++rejectedClock;
                                                             return f.now;
                                                           },
                                                           [](TreeError) {
                                                           }});
  check(error(rejected, TreeError::INVALID_ARGUMENT), "active binding exclusivity rejects replacement");
  f.now = 350;
  take(f.scheduler->dispatch());
  check(f.values().vertical.offset.raster == 2 && firstClock == 2 && rejectedClock == 0,
        "failed replacement preserves admitted binding A and never publishes B services");
  f.mouse(MouseAction::RELEASE, MouseButton::LEFT, 0, 9);

  BarFixture exact({.keyStep = std::numeric_limits<std::uint64_t>::max(), .arrows = true});
  take(setScrollMetrics(*exact.tree, exact.bar, {std::numeric_limits<std::uint64_t>::max(), 1}));
  exact.layout({1, 10});
  auto exactBinding = take(bindScrollBar(*exact.router, *exact.tree, exact.bar));
  exact.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 9);
  check(exact.values().vertical.offset.raster == std::numeric_limits<std::uint64_t>::max() - 1,
        "functional arrow preserves exact uint64 endpoint without narrowing or overflow");
  exact.layout({1, 10});
  exact.mouse(MouseAction::PRESS, MouseButton::LEFT, 0, 0);
  check(exact.values().vertical.offset.raster == 0, "maximal decrement clamps at exact zero");
}

void arrowAdmissionErrors() {
  for (int mode = 0; mode < 3; ++mode) {
    int calls{};
    std::optional<TreeError> asynchronous;
    BarFixture f({.arrows = true});
    auto binding = take(bindScrollBar(*f.router, *f.tree, f.bar,
                                      {[&]() -> SchedulerTime {
                                         ++calls;
                                         if (mode == 0 || (mode == 2 && calls == 2)) {
                                           throw TreeError::INVALID_ARGUMENT;
                                         }
                                         return mode == 1 ? std::numeric_limits<SchedulerTime>::max() - 100 : f.now;
                                       },
                                       [&](TreeError failure) {
                                         asynchronous = failure;
                                       }}));
    take(f.router->setPointerCaptureCapabilities({true, true, true}));
    const auto pressed = f.router->dispatch(MouseInputEvent{0, 9, MouseButton::LEFT, MouseAction::PRESS, {}});
    if (mode < 2) {
      check(error(pressed, mode == 0 ? TreeError::CALLBACK_FAILED : TreeError::INVALID_ARGUMENT) &&
              f.values().vertical.offset.raster == 0,
            "failed clock or deadline admission preserves initial offset");
    } else {
      take(pressed);
      f.now = 350;
      take(f.scheduler->dispatch());
      check(asynchronous == TreeError::CALLBACK_FAILED && f.values().vertical.offset.raster == 2,
            "late clock exception stops repeat and reaches typed error sink");
    }
    f.now = 1000;
    check(take(f.scheduler->dispatch()).timers == 0, "failed hold admission leaves no late repeat");
  }
}

void arrowGlyphPreparation() {
  BarFixture f({.glyphs = PrimitiveGlyphs::ASCII, .arrows = true});
  ScrollbarSkin skin;
  skin.vertical.backward = "U";
  skin.vertical.forward = "D";
  skin.vertical.start = "A";
  skin.vertical.end = "Z";
  skin.ends = true;
  take(setScrollbarSkin(*f.tree, f.bar, skin));
  auto painted = f.paint(f.bar);
  check(painted.cells.at({0, 0}) == "U" && painted.cells.at({0, 9}) == "D",
        "functional glyphs are independent of decorative ends");
  skin.vertical.backward = "";
  check(error(setScrollbarSkin(*f.tree, f.bar, skin), TreeError::INVALID_ARGUMENT),
        "empty functional arrow glyph rejects");
  skin.vertical.backward = "界";
  check(error(setScrollbarSkin(*f.tree, f.bar, skin), TreeError::INVALID_ARGUMENT),
        "wide arrow glyph rejects before painting");
  check(f.paint(f.bar).cells == painted.cells, "failed arrow preparation preserves installed skin");
}

void namedGlyphProfiles() {
  constexpr std::array PROFILES{GlyphProfile::UNICODE_GLYPHS, GlyphProfile::ASCII,    GlyphProfile::NF_ROUNDED,
                                GlyphProfile::NF_CHEVRON,     GlyphProfile::NF_SLASH, GlyphProfile::NF_BACKSLASH};

  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    BarFixture fixture({}, axis);
    StyleEngine engine(*fixture.tree);

    take(engine.bind(fixture.bar, {"ScrollBar", "", {}, true}));

    for (const auto profile : PROFILES) {
      StyleSheet sheet;
      sheet.rules.push_back({{}, {.glyphProfile = profile}});
      take(engine.replace(sheet));

      for (const int length : {1, 2, 5}) {
        take(setScrollMetrics(*fixture.tree, fixture.bar, {10, 10}));
        fixture.layout(axis == ScrollAxis::HORIZONTAL ? CellSize{length, 1} : CellSize{1, length});
        const auto painted = fixture.paint(fixture.bar);
        const auto cell = [&](int position) {
          return axis == ScrollAxis::HORIZONTAL ? std::pair{position, 0} : std::pair{0, position};
        };
        const auto sources = detail::profileSources({profile});
        const auto first = axis == ScrollAxis::HORIZONTAL ? 2 : 6;

        check(painted.cells.at(cell(0)) == sources[first + (length == 1 ? 3 : 0)],
              "named map selects one-cell or leading thumb glyph on each axis");

        if (length >= 2) {
          check(painted.cells.at(cell(length - 1)) == sources[first + 2], "named map preserves distinct trailing cap");
        }

        if (length > 2) {
          check(painted.cells.at(cell(1)) == sources[first + 1], "named map preserves thumb body");
        }
      }

      take(fixture.tree->read(fixture.bar, [&](const ScrollBar &widget) {
        const auto &profiles = detail::ScrollWidgetAccess::state(widget).glyphProfiles;

        check(profiles && profiles->states[0] == profiles->states[5],
              "equal logical states share owning prepared glyphs");
      }));

      struct NoAllocationSink : PaintContext {
        std::size_t cells{};

        std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
          cells += commands.size();

          return {};
        }
      } sink;

      std::expected<void, TreeError> painted;

      take(fixture.tree->frame([&] {
        allocation_test::fail = true;
        painted = fixture.tree->paint(fixture.bar, sink);
        allocation_test::fail = false;
      }));
      check(painted && sink.cells == 5, "named profile paint borrows owned resources without allocation");
    }

    take(engine.replace(take(parsePss("*{glyph-profile:nf-rounded;} *:focused{glyph-profile:nf-chevron;} "
                                      "*:disabled{glyph-profile:ascii;} *:selected{glyph-profile:nf-slash;} "
                                      "*:header{glyph-profile:nf-backslash;} *:caret{glyph-profile:unicode;}"))));
    take(fixture.tree->read(fixture.bar, [&](const ScrollBar &widget) {
      const auto &profiles = detail::ScrollWidgetAccess::state(widget).glyphProfiles;
      constexpr std::array EXPECTED{GlyphProfile::NF_ROUNDED, GlyphProfile::NF_CHEVRON,   GlyphProfile::ASCII,
                                    GlyphProfile::NF_SLASH,   GlyphProfile::NF_BACKSLASH, GlyphProfile::UNICODE_GLYPHS};

      for (std::size_t index = 0; index < EXPECTED.size(); ++index) {
        check(profiles->states[index]->scrollSelection.profile == EXPECTED[index],
              "every logical state owns the corresponding prepared profile");
      }
    }));

    for (int state = 0; state < 3; ++state) {
      take(fixture.tree->setEnabled(fixture.bar, state != 2));
      take(syncScrollControl(*fixture.tree, fixture.bar, state == 1 ? Node<Widget>(fixture.bar) : Node<Widget>{}));
      const auto cell = std::pair{0, 0};
      const auto profile = state == 0   ? GlyphProfile::NF_ROUNDED
                           : state == 1 ? GlyphProfile::NF_CHEVRON
                                        : GlyphProfile::ASCII;
      const auto sources = detail::profileSources({profile});
      check(fixture.paint(fixture.bar).cells.at(cell) == sources[axis == ScrollAxis::HORIZONTAL ? 2 : 6],
            "normal focused and disabled paints select their own prepared state");
    }

    take(fixture.tree->setEnabled(fixture.bar, true));
    take(syncScrollControl(*fixture.tree, fixture.bar, {}));
    take(engine.replace({}));
    take(fixture.tree->read(fixture.bar, [&](const ScrollBar &widget) {
      check(!detail::ScrollWidgetAccess::state(widget).glyphProfiles,
            "removing the last opt-in releases prepared glyph profiles");
    }));

    ScrollbarSkin manual;
    manual.glyphSelection = {GlyphProfile::NF_ROUNDED};
    take(setScrollbarSkin(*fixture.tree, fixture.bar, manual));
    take(engine.replace(take(parsePss("*{glyph-profile:ascii;glyph-fallback:ascii;}"))));
    manual.glyphSelection.profile = GlyphProfile::NF_SLASH;
    take(setScrollbarSkin(*fixture.tree, fixture.bar, manual));
    const auto cell = [&](int position) {
      return axis == ScrollAxis::HORIZONTAL ? std::pair{position, 0} : std::pair{0, position};
    };
    check(fixture.paint(fixture.bar).cells.at(cell(0)) == (axis == ScrollAxis::HORIZONTAL ? "[" : "+"),
          "replacing the manual skin retains the current PSS override");
    take(engine.replace({}));
    check(fixture.paint(fixture.bar).cells.at(cell(0)) == (axis == ScrollAxis::HORIZONTAL ? "\uE0BA" : "▄"),
          "removing PSS returns to the current manual selection");
    take(engine.replace(take(parsePss("*{scrollbar-thumb-pattern:ascii;}"))));
    check(fixture.paint(fixture.bar).cells.at(cell(0)) == "#", "explicit part pattern overrides profile segments");
    auto &literal = axis == ScrollAxis::HORIZONTAL ? manual.horizontal : manual.vertical;
    literal.thumb = "x";
    literal.thumbEnd = "Z";
    take(setScrollbarSkin(*fixture.tree, fixture.bar, manual));
    check(fixture.paint(fixture.bar).cells.at(cell(0)) == "x" && fixture.paint(fixture.bar).cells.at(cell(4)) == "Z",
          "literal common thumb and explicit segment remain highest priority");
    literal.thumbStart = "界";
    check(error(setScrollbarSkin(*fixture.tree, fixture.bar, manual), TreeError::INVALID_ARGUMENT),
          "invalid literal sources reject instead of invoking profile fallback");
    check(fixture.paint(fixture.bar).cells.at(cell(4)) == "Z", "invalid literal retains last valid owning profile");
  }

  {
    BarFixture fixture({}, ScrollAxis::HORIZONTAL);
    StyleEngine engine(*fixture.tree);
    take(engine.bind(fixture.bar, {"ScrollBar", "", {}, true}));
    ScrollbarSkin skin;
    skin.glyphSelection = {GlyphProfile::NF_ROUNDED};
    take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));
    static StyleEngine *nestedEngine;
    nestedEngine = &engine;
    allocation_test::hook = [] {
      take(nestedEngine->replace(take(parsePss("*{glyph-profile:ascii;}"))));
    };
    allocation_test::beforeHook = 0;
    skin.glyphSelection.profile = GlyphProfile::NF_SLASH;
    const auto changed = setScrollbarSkin(*fixture.tree, fixture.bar, skin);
    allocation_test::hook = nullptr;
    check(error(changed, TreeError::WOULD_BLOCK), "skin preparation cannot overwrite a newer reentrant style revision");
    take(fixture.tree->read(fixture.bar, [&](const ScrollBar &widget) {
      const auto &state = detail::ScrollWidgetAccess::state(widget);
      check(state.glyphSelection.profile == GlyphProfile::NF_ROUNDED &&
              state.glyphProfiles->states[0]->scrollSelection.profile == GlyphProfile::ASCII,
            "reentrant PSS publication and the previous manual profile remain coherent");
    }));
  }

  const WidthPolicy wide{AmbiguousWidth::WIDE};
  const auto fallback = take(detail::profileGlyph("\uE0B6", "◖", "[", wide, GlyphFallback::ASCII));
  check(fallback.bytes() == "[", "NF admission failure uses explicit ASCII fallback");
  check(detail::profileSources({GlyphProfile::NF_ROUNDED, GlyphFallback::ASCII})[6] == "+",
        "explicit ASCII fallback selects portable vertical caps without rotating NF");
}

void skinProfileSnapshotReentry() {
  BarFixture fixture({}, ScrollAxis::HORIZONTAL);
  StyleEngine engine(*fixture.tree);
  take(engine.bind(fixture.bar, {.type = "Control"}));
  take(engine.replacePss("*{glyph-profile:default;}"));
  ScrollbarSkin skin;
  const auto before = allocation_test::allocations;
  take(setScrollbarSkin(*fixture.tree, fixture.bar, skin));
  // DEFAULT prepares the identical skin but needs no subsequent glyph-profile allocation.
  const auto skinAllocations = allocation_test::allocations - before;
  check(skinAllocations > 0 && skinAllocations <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
        "skin-only allocation calibration is bounded");
  if (skinAllocations > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return;
  }

  std::weak_ptr<const StyleSnapshot> oldSnapshot;
  take(fixture.tree->read(fixture.bar, [&](const ScrollBar &widget) {
    oldSnapshot = widget.computedStyle();
  }));
  const auto replacement = take(parsePss("*{glyph-profile:ascii;}"));
  static StyleEngine *nestedEngine;
  static const StyleSheet *nestedSheet;
  static std::weak_ptr<const StyleSnapshot> *observedSnapshot;
  static bool retainedDuringHook;
  nestedEngine = &engine;
  nestedSheet = &replacement;
  observedSnapshot = &oldSnapshot;
  retainedDuringHook = false;
  allocation_test::hook = [] {
    take(nestedEngine->replace(*nestedSheet));
    retainedDuringHook = !observedSnapshot->expired();
  };
  allocation_test::beforeHook = static_cast<int>(skinAllocations);
  skin.glyphSelection.profile = GlyphProfile::NF_SLASH;
  const auto changed = setScrollbarSkin(*fixture.tree, fixture.bar, skin);
  const bool hookFired = allocation_test::hook == nullptr;
  allocation_test::hook = nullptr;
  check(hookFired && retainedDuringHook && oldSnapshot.expired() && error(changed, TreeError::WOULD_BLOCK),
        "profile preparation owns the old snapshot through reentrant publication and releases it on rejection");
  take(fixture.tree->read(fixture.bar, [&](const ScrollBar &widget) {
    const auto &state = detail::ScrollWidgetAccess::state(widget);
    check(state.glyphSelection.profile == GlyphProfile::DEFAULT && state.glyphProfiles &&
            state.glyphProfiles->states[0]->scrollSelection.profile == GlyphProfile::ASCII,
          "reentrant style resources survive while stale manual glyph preparation stays unpublished");
  }));
}

thread_local WidgetTree *profileReentryTree{};
thread_local StyleEngine *profileReentryEngine{};
thread_local Node<Widget> profileReentryNode;
thread_local bool profileMutationRejected{};
thread_local bool profileReentryRejected{};

void profileAllocationReentry() {
  auto removed = profileReentryTree->remove(profileReentryNode);
  auto refreshed = profileReentryEngine->refresh();
  profileMutationRejected = !removed && removed.error() == TreeError::WOULD_BLOCK;
  profileReentryRejected = !refreshed && refreshed.error().cause == TreeError::WOULD_BLOCK;
}

struct ProfileDefaultsBar : ScrollBar {
  using ScrollBar::ScrollBar;
  bool failProfile{};
  bool reenterProfile{};

  ComputedStyle styleDefaults() const override {
    auto result = ScrollBar::styleDefaults();

    if (reenterProfile) {
      allocation_test::hook = profileAllocationReentry;
      allocation_test::beforeHook = 0;
    }

    allocation_test::fail = failProfile;

    return result;
  }
};

void glyphProfileTransactions() {
  TreeFixture fixture;
  auto first = take(fixture.tree->emplaceChecked<ScrollBar>(fixture.root));
  auto second = take(fixture.tree->emplaceChecked<ProfileDefaultsBar>(fixture.root));
  StyleEngine engine(*fixture.tree);

  take(engine.bind(first, {"ScrollBar", "", {}, true}));
  take(engine.bind(second, {"ScrollBar", "", {}, true}));
  const auto before = engine.revision();
  auto sheet = take(parsePss("ScrollBar{glyph-profile:nf-rounded;}"));

  take(fixture.tree->edit(second, [&](ProfileDefaultsBar &bar) {
    bar.failProfile = true;
  }));
  take(fixture.tree->consumeFrameRequest());
  const auto failed = engine.replace(sheet);
  allocation_test::fail = false;
  check(!failed && failed.error().cause == TreeError::NO_MEMORY && engine.revision() == before,
        "profile preparation NO_MEMORY preserves style revision after another target was staged");
  check(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE,
        "failed all-target profile preparation never invalidates");
  take(fixture.tree->read(first, [&](const ScrollBar &bar) {
    check(!detail::ScrollWidgetAccess::state(bar).glyphProfiles && !bar.computedStyle()->glyphs()[0].profile,
          "failed later profile leaves earlier computed style and resources unpublished");
  }));
  take(fixture.tree->edit(second, [&](ProfileDefaultsBar &bar) {
    bar.failProfile = false;
    bar.reenterProfile = true;
  }));
  profileReentryTree = fixture.tree.get();
  profileReentryEngine = &engine;
  profileReentryNode = second;
  take(engine.replace(sheet));
  allocation_test::hook = nullptr;
  check(profileMutationRejected && profileReentryRejected,
        "profile allocator reentry cannot retire a target or reenter the style transaction");
  take(fixture.tree->edit(second, [&](ProfileDefaultsBar &bar) {
    bar.reenterProfile = false;
  }));
  take(engine.enableLayers());
  auto layer = take(engine.addLayer(StyleTier::APPLICATION, "profile", take(parsePss("*{glyph-profile:ascii;}"))));
  take(engine.removeLayer(layer));
  take(fixture.tree->read(first, [&](const ScrollBar &bar) {
    check(detail::ScrollWidgetAccess::state(bar).glyphProfiles->states[0]->scrollSelection.profile ==
            GlyphProfile::NF_ROUNDED,
          "removing an application layer restores the owning theme glyph resources");
  }));
}

} // namespace

int main() {
  fractionalScrollbarPaint();
  fractionalScrollbarStylesAndDrag();
  fractionalProjectionAndPlacement();
  namedGlyphProfiles();
  glyphProfileTransactions();
  skinProfileSnapshotReentry();
  sharedScrollbarPresetLifetime();
  functionalArrowGeometry();
  heldArrows();
  arrowAdmissionErrors();
  rebindHeldArrowServices();
  activeArrowBindingRejectsReplacementTransactionally();
  arrowCancellationAndLifetime();
  arrowGlyphPreparation();
  initializationAndLifetime();
  placementPaintAndHits();
  wheelKeysAndBoundary();
  exactStandaloneAndReadonly();
  dragAndTrackClick();
  preciseThumbDragging();
  preciseAnchorIntegerBoundaries();
  dragAllocationAdmission();
  sharedMappingChange();
  hiddenZeroBudgetAndErrors();
  failureTransactions();
  reentrantWriterAdmission();
  obsoleteAssociationAdmission();
  advancedFrames();
  scrollbarSkins();
  segmentedScrollbarSkins();
  segmentedScrollbarStyles();
  scrollbarSkinFailures();
  manualStyleAndAutomaticMetadata();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
