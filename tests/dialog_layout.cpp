// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/dialog_detail.hpp"
#include <portals/dialog.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
}

void *operator new(std::size_t size) {
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  if (void *p = std::malloc(size ? size : 1)) {
    return p;
  }

  throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept {
  std::free(p);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *p) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t) noexcept {
  ::operator delete(p);
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

void operator delete(void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}

using namespace portals;

namespace {
int failures{};

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    std::abort();
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

CellRect rect(int x, int y, int width, int height) {
  return take(CellRect::from({x, y}, {width, height}));
}

void pureGeometry() {
  static_assert(!std::derived_from<Dialog, SizedWidget>);

  Dialog dialog;
  const std::array panel{MeasuredChild{{10, 4}, {}, {}}};

  check(take(dialog.arrangeChildren(panel, {21, 11})).front() == rect(5, 3, 10, 4), "center rounds spare cells down");
  check(take(dialog.childAvailable({100, 100})) == CellSize{60, 20}, "measurement respects maximum panel size");
  check(take(dialog.arrangeChildren({}, {10, 10})).empty(), "hidden panel has no rectangle");

  constexpr std::ptrdiff_t MAX_ALLOCATION_ATTEMPTS = 32;
  std::ptrdiff_t successfulAllocationIndex = -1;

  for (std::ptrdiff_t allocation = 0; allocation < MAX_ALLOCATION_ATTEMPTS; ++allocation) {
    allocation_test::remaining = allocation;

    const auto result = dialog.arrangeChildren(panel, {20, 10});

    allocation_test::remaining = -1;

    if (result) {
      successfulAllocationIndex = allocation;

      check(result->size() == 1 && result->front() == rect(5, 3, 10, 4),
            "allocation sweep preserves successful panel geometry");

      break;
    }

    check(result.error() == TreeError::NO_MEMORY, "each panel rectangle allocation failure is typed");
  }

  check(successfulAllocationIndex >= 0, "panel placement succeeds after all allocation failure indexes");
  std::cout << "Panel allocation sweep: " << successfulAllocationIndex
            << " typed failures; success at allocation index " << successfulAllocationIndex << '\n';

  const std::array many{panel.front(), panel.front()};

  check(dialog.arrangeChildren(many, {10, 10}).error() == TreeError::INVALID_ARGUMENT,
        "structural root refuses multiple panels");

  for (const auto margin : {0, 1, 30, std::numeric_limits<int>::max()}) {
    DialogOptions options;

    options.viewportMargin = margin;

    Dialog current(options);

    for (int width = 0; width < 22; ++width) {
      for (int height = 0; height < 14; ++height) {
        const auto placed = take(current.arrangeChildren(panel, {width, height})).front();

        check(rect(0, 0, width, height).contains(placed), "tiny panel remains within host");
        check(placed.size().width <= 10 && placed.size().height <= 4, "tiny panel never grows intrinsic size");
        check(width == 0 || height == 0 || !placed.empty(), "positive tiny viewport retains a usable panel");
      }
    }
  }

  MeasuredChild fixed{{100, 100}, {}, {}};

  fixed.policy.width = {100, 100, 100, SizeMode::FIXED};
  fixed.policy.height = fixed.policy.width;

  check(take(detail::placeDialogPanel({}, {8, 5}, fixed)) == rect(1, 1, 6, 3),
        "panel caps fixed and minimum sizes rather than narrowing the root");
  check(take(dialog.arrangeChildren(panel, {-1, -20})).front().empty(), "negative offered extents normalize to zero");

  DialogOptions anchored;

  anchored.placement = DialogPlacement::BELOW_START;
  check(take(detail::placeDialogPanel(anchored, {30, 15}, panel.front(), rect(3, 3, 5, 1))) == rect(3, 5, 10, 4),
        "below start includes configured gap");
  check(take(detail::placeDialogPanel(anchored, {30, 15}, panel.front(), rect(26, 12, 3, 1))) == rect(19, 7, 10, 4),
        "below flips above and clamps the trailing edge");

  anchored.placement = DialogPlacement::ABOVE_START;
  check(take(detail::placeDialogPanel(anchored, {30, 15}, panel.front(), rect(3, 1, 5, 1))) == rect(3, 3, 10, 4),
        "above symmetrically flips below");
  anchored.anchorGap = std::numeric_limits<int>::max();
  check(take(detail::placeDialogPanel(anchored, {30, 15}, panel.front(), rect(-100, -100, 5, 1))) == rect(1, 1, 10, 4),
        "negative anchor and maximum gap use wide intermediates before clamping");
  check(take(detail::placeDialogPanel({}, {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()},
                                      panel.front()))
            .size() == CellSize{10, 4},
        "maximum viewport centers without arithmetic overflow");
}

void validationAndHostIdentity() {
  auto invalid = [](DialogOptions options) {
    Dialog dialog(options);
    const std::array child{MeasuredChild{{4, 3}, {}, {}}};

    check(dialog.childAvailable({10, 10}).error() == TreeError::INVALID_ARGUMENT,
          "invalid settings reject measurement");
    check(dialog.measureChildren(child).error() == TreeError::INVALID_ARGUMENT, "invalid settings reject aggregation");
    check(dialog.arrangeChildren(child, {10, 10}).error() == TreeError::INVALID_ARGUMENT,
          "invalid settings reject placement");
  };
  DialogOptions options;

  options.mode = static_cast<DialogMode>(-1);
  invalid(options);
  options = {};
  options.placement = static_cast<DialogPlacement>(-1);
  invalid(options);
  options = {};
  options.maximumPanelSize.width = 0;
  invalid(options);
  options = {};
  options.maximumPanelSize.height = -1;
  invalid(options);
  options = {};
  options.viewportMargin = -1;
  invalid(options);
  options = {};
  options.anchorGap = -1;
  invalid(options);
  options = {};
  options.frame.maxPaintCells = 0;
  invalid(options);
  options = {};
  options.frame.glyphs = static_cast<PrimitiveGlyphs>(-1);
  invalid(options);

  OverlayHost host;
  const NodeId body{1, 2, 3};
  const std::array children{MeasuredChild{{7, 3}, {}, body}, MeasuredChild{{1000, 1000}, {}, {1, 3, 3}}};

  check(host.measureChildren(children).error() == TreeError::INVALID_ARGUMENT, "unbound host refuses layout");
  detail::DialogAccess::bindBody(host, body);
  check(take(host.measureChildren(children)) == CellSize{7, 3}, "dialog never changes body intrinsic extent");
  check(take(host.measureChildren(std::span(children).subspan(1))) == CellSize{},
        "hidden body never adopts a visible dialog's intrinsic extent");

  const auto placed = take(host.arrangeChildren(children, {20, 10}));

  check(placed[0] == rect(0, 0, 20, 10) && placed[1] == placed[0], "default body and roots share full allocation");

  OverlayHost invalidHost({0});

  detail::DialogAccess::bindBody(invalidHost, body);
  check(invalidHost.childAvailable({10, 10}).error() == TreeError::INVALID_ARGUMENT,
        "zero registration budget rejected");
}

struct Sink : PaintContext {
  NodeGeometry geometry;
  std::vector<RenderCommand> &output;

  Sink(NodeGeometry value, std::vector<RenderCommand> &commands) : geometry(value), output(commands) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      std::visit(
        [&](const auto &value) {
          const auto clip = take(value.clip.translated(geometry.surface.origin())).intersection(geometry.clip);

          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            output.emplace_back(TextCommand{take(translated(value.origin, geometry.surface.origin())), value.text, clip,
                                            value.style, value.z});
          } else {
            output.emplace_back(
              FillCommand{take(value.area.translated(geometry.surface.origin())), clip, value.style, value.z});
          }
        },
        command);
    }

    return {};
  }
};

void paintTree(WidgetTree &tree, Node<Widget> node, std::vector<RenderCommand> &commands) {
  const auto snapshot = take(tree.inspect(node));

  if (!snapshot.visible || !snapshot.geometry.valid) {
    return;
  }

  Sink sink(snapshot.geometry, commands);

  take(tree.paint(node, sink));

  for (const auto child : snapshot.children) {
    paintTree(tree, child, commands);
  }
}

struct Backdrop : Widget {
  CellRect bounds;
  GraphemeText text = take(GraphemeText::from("XXXXXXXXXXXXXXXXXXXX"));

  std::expected<CellSize, TreeError> measure(CellSize) override {
    return CellSize{20, 9};
  }

  std::expected<void, TreeError> arrange(CellRect local) override {
    bounds = rect(0, 0, local.size().width, local.size().height);

    return {};
  }

  std::expected<void, TreeError> paint(PaintContext &sink) override {
    for (int y = 0; y < bounds.size().height; ++y) {
      const RenderCommand command = TextCommand{{0, y}, text, bounds};
      const auto result = sink.submit(std::span(&command, 1));

      if (!result) {
        return result;
      }
    }

    return {};
  }
};

struct BlankRow : Widget {
  std::expected<CellSize, TreeError> measure(CellSize) override {
    return CellSize{0, 1};
  }
};

void retainedTreePainting() {
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  const auto root = take(tree->root());
  const auto host = take(tree->emplace<OverlayHost>(root));
  const auto body = take(tree->emplace<Backdrop>(host));
  const auto dialog = take(tree->emplace<Dialog>(host));
  ControlStyle style;

  style.normal.background = RgbColor{20, 30, 40};
  style.disabled.background = RgbColor{50, 60, 70};

  const auto panel = take(tree->emplace<detail::DialogPanel>(dialog, WidthPolicy{}, BorderOptions{}, style));
  const auto title = take(tree->emplace<Label>(panel, take(GraphemeText::from("\xe7\x95\x8c title"))));

  take(tree->emplace<Label>(panel, take(GraphemeText::from("body"))));
  take(tree->emplace<BlankRow>(panel));
  take(tree->edit(host, [&](OverlayHost &widget) {
    detail::DialogAccess::bindBody(widget, body.id());
  }));

  auto draw = [&](CellSize size) {
    std::optional<CellBuffer> output;
    const auto viewport = take(CellRect::from({}, size));

    take(tree->invalidate(root, Invalidation::LAYOUT));
    take(tree->frame([&] {
      take(layoutTree(*tree, root, viewport, viewport));

      std::vector<RenderCommand> commands;

      paintTree(*tree, root, commands);
      output.emplace(take(renderFullFrame(size, commands)));
    }));

    return std::move(*output);
  };

  const auto painted = draw({20, 9});
  const auto before = take(tree->inspect(body));
  const auto frame = take(tree->inspect(panel)).geometry;
  const auto heading = take(tree->inspect(title)).geometry;

  check(frame.surface == rect(5, 2, 10, 5), "Unicode heading contributes cell width plus existing border insets");
  check(heading.surface.size().width == 8, "heading layout uses cell width rather than UTF-8 bytes");
  check(take(painted.at({5, 2})).glyph == "\xe2\x94\x8c", "default frame paints Unicode border");
  check(take(painted.at({13, 5})).glyph == " ", "opaque interior erases text beneath unused body cells");
  check(take(painted.at({13, 5})).style.background == style.normal.background, "opaque fill shares frame style");
  check(take(painted.at({0, 0})).glyph == "X", "outside root remains transparent");

  take(tree->setVisible(dialog, false));

  const auto closed = draw({20, 9});
  const auto after = take(tree->inspect(body));

  check(before.parent == after.parent && before.geometry.local == after.geometry.local &&
          before.geometry.surface == after.geometry.surface && before.geometry.clip == after.geometry.clip,
        "hiding a dialog preserves body identity parent and all geometry");
  check(take(closed.at({5, 2})).glyph == "X", "normal repaint removes closed panel ghosts");

  take(tree->setVisible(dialog, true));
  take(tree->setEnabled(panel, false));
  take(syncBorder(*tree, panel));

  const auto disabled = draw({20, 9});

  check(take(disabled.at({13, 5})).style.background == style.disabled.background,
        "opaque fill reuses Border's synchronized disabled state");

  for (const auto size : {CellSize{0, 0}, CellSize{1, 1}, CellSize{2, 1}, CellSize{3, 3}}) {
    draw(size);
    check(rect(0, 0, size.width, size.height).contains(take(tree->inspect(panel)).geometry.surface),
          "retained tiny panel has contained geometry");
  }
}

struct CountingSink : PaintContext {
  int submitted{};

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    submitted += static_cast<int>(commands.size());

    return {};
  }
};

void paintBudgetAndWidthPolicy() {
  BorderOptions options;

  options.maxPaintCells = 8;

  detail::DialogPanel panel({}, options);
  CountingSink sink;

  take(panel.arrange(rect(0, 0, 3, 3)));
  check(panel.paint(sink).error() == TreeError::WOULD_BLOCK && sink.submitted == 0,
        "allocated opaque area is checked before submitting even one command");
  take(panel.arrange(rect(0, 0, 2, 2)));
  take(panel.paint(sink));
  check(sink.submitted == 5, "opaque panel emits one fill and four existing border cells");

  WidthPolicy wide;

  wide.ambiguous = AmbiguousWidth::WIDE;
  wide.revision = 42;

  detail::DialogPanel fallback(wide);
  std::vector<RenderCommand> commands;
  Sink collector({rect(0, 0, 2, 2), rect(0, 0, 2, 2), rect(0, 0, 2, 2), true}, commands);

  take(fallback.arrange(rect(0, 0, 2, 2)));
  take(fallback.paint(collector));
  check(std::get<TextCommand>(commands[1]).text.bytes() == "+", "ambiguous-wide frame falls back to ASCII");
  check(std::get<TextCommand>(commands[1]).text.policy() == wide, "frame retains complete width policy revision");
}
} // namespace

int main() {
  pureGeometry();
  validationAndHostIdentity();
  retainedTreePainting();
  paintBudgetAndWidthPolicy();

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
