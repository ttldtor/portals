// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui_style.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace portals;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <class T, class E> T checked(std::expected<T, E> result) {
  check(result.has_value(), "Installed scroll consumer setup or command failed");

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

// The host initially withholds drag guarantees, then explicitly admits them on UI.
struct Terminal : example::ReplayTerminal {
  using ReplayTerminal::ReplayTerminal;
  bool drag{};

  PointerCaptureCapabilities pointerCaptureCapabilities() const noexcept override {
    return drag ? PointerCaptureCapabilities{true, true, true} : PointerCaptureCapabilities{};
  }
};

void exactGeometry() {
  constexpr auto MAXIMUM = std::numeric_limits<std::uint64_t>::max() - 1;
  constexpr ScrollAxisMetrics METRICS{std::numeric_limits<std::uint64_t>::max(), 1};
  const auto thumb = checked(scrollThumb(METRICS, MAXIMUM, 17));

  check(scrollRange(METRICS).maximum == MAXIMUM && thumb.start == thumb.travel,
        "Exact uint64 range endpoint was rounded");
  check(checked(scrollOffsetForThumb(thumb.travel, thumb.travel, MAXIMUM)) == MAXIMUM,
        "Inverse thumb endpoint was rounded");

  const auto invalid = scrollThumb(METRICS, 0, -1);

  check(!invalid && invalid.error() == TreeError::INVALID_ARGUMENT, "Geometry lost its typed argument error");

  const auto reserved = checked(solveScrollLayout({10, 5}, {20, 10}));
  ScrollLayoutOptions overlayOptions;

  overlayOptions.placement = ScrollPlacement::OVERLAY;

  const auto overlay = checked(solveScrollLayout({10, 5}, {20, 10}, overlayOptions));

  check(reserved.viewport == CellSize{9, 4} && overlay.viewport == CellSize{10, 5} &&
          overlay.childClipSize == CellSize{9, 4},
        "Reserved/overlay placement changed its nominal viewport or owned edge clip");

  ScrollAreaOptions frame;

  frame.placement = ScrollPlacement::ON_FRAME;

  const auto missingFrame = validateScrollOptions(frame);

  check(!missingFrame && missingFrame.error() == TreeError::INVALID_ARGUMENT,
        "ON_FRAME accepted a missing owned frame");
}
} // namespace

int main() {
  using namespace portals;
  exactGeometry();
  namespace sc = style::ct;
  constexpr auto SEGMENTS = sc::compactSheet(std::array{sc::flatRule(
    sc::type<"ScrollBar">(), sc::scrollbarThumbStartForeground(sc::rgb(10, 11, 12)), sc::scrollbarThumbBodyBold(false),
    sc::scrollbarThumbEndItalic(true), sc::scrollbarThumbSingleUnderline(true))});
  const auto segmentSheet = sc::materialize(SEGMENTS);
  const auto segmentPreview = resolveStyle(segmentSheet, {"ScrollBar", "", {}});
  check(segmentPreview.scrollbarThumbSegments.has_value(), "Installed compact thumb declarations did not resolve");
  check((*segmentPreview.scrollbarThumbSegments)[0].start.foreground == RgbColor{10, 11, 12} &&
          !(*segmentPreview.scrollbarThumbSegments)[0].body.bold &&
          (*segmentPreview.scrollbarThumbSegments)[0].end.italic &&
          (*segmentPreview.scrollbarThumbSegments)[0].single.underline,
        "Installed typed thumb segment attributes disagree");

  ScrollControllerOptions initial;

  initial.vertical = {{std::numeric_limits<std::uint64_t>::max(), 1},
                      {std::numeric_limits<std::uint64_t>::max() - 1, 0}};

  auto controller = checked(ScrollController::create(initial));
  const auto pinned = checked(controller->snapshot());
  Terminal terminal({{100, "a", {}}, {200, "b", {}}, {300, "c", {}}}, 500, {50, 10});
  ui::SceneOptions sceneOptions;
  ScrollAreaOptions framed;
  ui::Ref<ScrollArea> outer;
  ui::Ref<ScrollArea> inner;
  ui::Ref<ScrollBar> projection;
  ui::Ref<ScrollBar> standalone;
  Application *application{};
  int inputs{};

  sceneOptions.clock = terminal.clock();
  framed.frame = BorderOptions{};
  framed.placement = ScrollPlacement::ON_FRAME;

  auto scene = ui::scene(
    [&](ManagedScene &, Application &app) {
      application = &app;

      return ui::hbox(
        ui::scrollArea(
          outer,
          ui::vbox(ui::label("Header"),
                   ui::scrollArea(inner,
                                  ui::vbox(ui::label("One"), ui::label("Two"), ui::label("Three"), ui::label("Four"),
                                           ui::label("Five"), ui::label("Six"), ui::label("Seven"), ui::label("Eight")),
                                  framed) |
                     ui::height(5),
                   ui::label("Footer")),
          {}, controller) |
          ui::width(20) | ui::height(5) | ui::scrollbar("frame") | ui::scrollTrack("minimal") |
          ui::scrollThumb("block") | ui::scrollArrows("ascii"),
        ui::scrollBar(projection, controller) | ui::height(5),
        ui::scrollBar(standalone).scrollbar("ascii") | ui::height(8));
    },
    sceneOptions);

  // clang-format off
  scene
    | ui::style("ScrollArea{scrollbar-track-foreground:#8090a0;scrollbar-thumb-bold:true;scrollbar-thumb-start-foreground:#0a0b0c;scrollbar-thumb-body-bold:false;} ScrollBar{scrollbar-corner-underline:false;scrollbar-thumb-end-italic:true;scrollbar-thumb-single-underline:true;scrollbar-vertical-track-glyph:\".\";scrollbar-vertical-thumb-start-glyph:\"[\";scrollbar-vertical-thumb-body-glyph:\"=\";scrollbar-vertical-thumb-end-glyph:\"]\";scrollbar-vertical-thumb-single-glyph:\"Q\";}")
    | ui::configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
        checked(application->tree().read(outer.node(), [&](const ScrollArea &widget) {
          check(widget.computedStyle() && widget.computedStyle()->scrollbarThumbSegments(),
                "Installed ScrollArea segment styles did not reach mounted preparation");
          const auto &parts = (*widget.computedStyle()->scrollbarThumbSegments())[0];
          check(parts.start.foreground == RgbColor{10, 11, 12} && !parts.body.bold,
                "Installed ScrollArea segment fallback or explicit false changed");
        }));
        checked(application->tree().read(standalone.node(), [&](const ScrollBar &widget) {
          check(widget.computedStyle() && widget.computedStyle()->scrollbarThumbSegments(),
                "Installed ScrollBar segment styles did not reach mounted preparation");
          const auto &parts = (*widget.computedStyle()->scrollbarThumbSegments())[0];
          check(parts.end.italic && parts.single.underline, "Installed ScrollBar segment attributes changed");
        }));
        check(owner.requiresPointerMotion(), "Mounted scroll controls did not request the native profile");
        checked(outer.setOffset(ScrollAxis::VERTICAL, {}));
        checked(standalone.setMetrics({std::numeric_limits<std::uint64_t>::max(), 1}));

        return owner.onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
          auto &router = application->router();

          if (++inputs == 1) {
            const auto geometry = checked(projection.geometry());
            const auto origin = geometry.surface.origin();
            const auto values = checked(controller->snapshot());

            check(values.vertical.metrics.extent > values.vertical.metrics.viewport,
                  "Ordinary content did not establish its retained extent");
            check(checked(inner.geometry()).valid, "Nested ON_FRAME area was not prepared");
            checked(router.dispatch(MouseInputEvent{origin.x, origin.y, MouseButton::LEFT, MouseAction::PRESS, {}}));
            check(checked(router.pointerCapture()) == Node<Widget>{}, "Capability-less host admitted drag");

            terminal.drag = true;
            checked(router.setPointerCaptureCapabilities(terminal.pointerCaptureCapabilities()));
            checked(router.dispatch(MouseInputEvent{origin.x, origin.y, MouseButton::LEFT, MouseAction::PRESS, {}}));
            check(checked(router.pointerCapture()) == projection.node(), "Public bar did not acquire capture");
            checked(router.dispatch(MouseInputEvent{origin.x + 100, origin.y + 100, MouseButton::LEFT,
                                                    MouseAction::MOVE, {}}));
            check(checked(controller->snapshot()).vertical.offset.raster == scrollRange(values.vertical.metrics).maximum,
                  "Captured outside movement did not reach the range endpoint");
            checked(router.dispatch(MouseInputEvent{origin.x + 100, origin.y + 100, MouseButton::LEFT,
                                                    MouseAction::RELEASE, {}}));
            check(checked(router.pointerCapture()) == Node<Widget>{}, "LEFT release retained capture");
          } else if (inputs == 2) {
            checked(outer.setOffset(ScrollAxis::VERTICAL, {1, .25}));

            const auto values = checked(controller->snapshot());
            const auto denied = owner.setScrollMetrics(projection.node(), {100, 5});
            const auto invalid = setScrollOffset(application->tree(), outer.node(), ScrollAxis::VERTICAL, {1, 1});

            check(values.vertical.offset.raster == 1 && values.vertical.offset.fraction == .25,
                  "Mounted checked command lost its exact fractional offset");
            check(!denied && std::get<TreeError>(denied.error().cause) == TreeError::INVALID_ARGUMENT,
                  "Shared projection acquired the area's metrics writer lease");
            check(!invalid && invalid.error() == TreeError::INVALID_ARGUMENT,
                  "Mounted command lost its typed invalid-fraction error");
            checked(owner.remove(projection.node()));
            checked(outer.setOffset(ScrollAxis::VERTICAL, {}));
            check(checked(controller->snapshot()).vertical.offset.raster == 0,
                  "Projection removal retired the remaining area");
          } else {
            const auto origin = checked(standalone.geometry()).surface.origin();

            checked(router.dispatch(MouseInputEvent{origin.x, origin.y, MouseButton::LEFT, MouseAction::PRESS, {}}));
            check(checked(router.pointerCapture()) == standalone.node(), "Standalone bar did not acquire capture");
            checked(owner.remove(standalone.node()));
            check(checked(router.pointerCapture()) == Node<Widget>{}, "Removal retained pointer capture");
            checked(owner.remove(outer.node()));
            check(!owner.setScrollOffset(outer.node(), ScrollAxis::VERTICAL, {}),
                  "Removed area accepted a command");
            check(!owner.requiresPointerMotion(), "Last removal retained the session input requirement");
            check(checked(router.pointerCapture()) == Node<Widget>{}, "Removal retained pointer capture");
          }

          return true;
        });
      });
  // clang-format on

  const auto code = scene | ui::runApp(terminal);
  const auto surviving = checked(controller->snapshot());

  check(code == 0 && terminal.closed && inputs == 3, "Scene runner failed or skipped terminal cleanup");
  check(pinned.vertical.offset.raster == initial.vertical.offset.raster,
        "An owning snapshot changed after area adoption and cleanup");
  check(surviving.vertical.offset.raster == 0, "Controller did not survive scene cleanup");

  return 0;
}
