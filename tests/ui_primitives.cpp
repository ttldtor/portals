// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/examples/gallery/gallery.hpp"
#include "support/examples/replay/terminal.hpp"
#include <portals/ui_style.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>

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

  if (auto *value = std::malloc(size ? size : 1)) {
    return value;
  }

  throw std::bad_alloc();
}

// Keep nonthrowing library allocations in the same fault-injection family.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return ::operator new(size, std::nothrow);
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void releaseAllocation(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value) noexcept {
  releaseAllocation(value);
}

void operator delete(void *value, std::size_t) noexcept {
  releaseAllocation(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  releaseAllocation(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  releaseAllocation(value);
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  releaseAllocation(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  releaseAllocation(value);
}

namespace {
using namespace portals;
std::uint_least32_t failureLine{};
constexpr BorderOptions ASCII_BORDER{PrimitiveGlyphs::ASCII, 16384};
constexpr GaugeOptions ASCII_GAUGE{0, 1, 10, PrimitiveGlyphs::ASCII, 16384};
static_assert(std::derived_from<Border, SizedWidget> && std::derived_from<Border, Layout>);
static_assert(!std::derived_from<Border, LinearLayout>);
static_assert(ui::width(7).change.extent == 7);
static_assert(ui::flexX(3).change.weight == 3);

template <class T>
concept CanSize = requires(T &&recipe) { std::forward<T>(recipe) | ui::width(4); };
static_assert(CanSize<ui::Description<Label>>);
static_assert(!CanSize<ui::Description<Widget>>);
static_assert(!CanSize<ui::Description<Label> &>);

template <class T>
concept CanChrome = requires(T &&recipe) { std::forward<T>(recipe).chrome(ButtonChrome::NONE); };
template <class T>
concept CanGlyphs = requires(T &&recipe) { std::forward<T>(recipe) | ui::glyphs(PrimitiveGlyphs::ASCII); };
template <class T>
concept CanGaugeRange = requires(T &&recipe) { std::forward<T>(recipe) | ui::range(-1, 1); };
static_assert(CanChrome<ui::Description<Button>> && !CanChrome<ui::Description<Label>>);
static_assert(CanGlyphs<ui::Description<Border>> && CanGlyphs<ui::Description<Gauge>>);
static_assert(!CanGlyphs<ui::Description<Button>> && !CanGlyphs<ui::Description<Widget>>);
static_assert(CanGaugeRange<ui::Description<Gauge>> && !CanGaugeRange<ui::Description<Border>>);

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  if (!result) {
    if (failureLine == 0) {
      failureLine = origin.line();
    }

    throw std::runtime_error("checked operation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void nothrowAllocationContracts() {
  constexpr std::size_t ALLOCATION_BYTES = 37;

  allocation_test::remaining = 2;
  auto *scalar = ::operator new(ALLOCATION_BYTES, std::nothrow);
  auto *array = ::operator new[](ALLOCATION_BYTES, std::nothrow);
  const auto remaining = allocation_test::remaining;
  allocation_test::remaining = -1;
  const bool allocated = scalar && array;
  ::operator delete(scalar, std::nothrow);
  ::operator delete[](array, std::nothrow);
  check(allocated && remaining == 0, "nothrow scalar and array allocations share the throwing allocation budget");

  allocation_test::remaining = 0;
  auto *failedScalar = ::operator new(ALLOCATION_BYTES, std::nothrow);
  auto *failedArray = ::operator new[](ALLOCATION_BYTES, std::nothrow);
  allocation_test::remaining = -1;
  const bool failed = !failedScalar && !failedArray;
  ::operator delete(failedScalar, std::nothrow);
  ::operator delete[](failedArray, std::nothrow);
  check(failed, "nothrow allocations translate injected bad_alloc to null without crossing allocation families");
}

struct Fixture {
  WidthPolicy policy;
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(WidthPolicy width = {}) : policy(width) {
    ApplicationOptions options;

    options.widthPolicy = width;
    app = take(Application::create(options));
    scene = take(ManagedScene::create(*app));
  }

  CellBuffer render(CellSize size) {
    take(scene->prepare(*app, size));
    take(app->tree().consumeFrameRequest());

    const auto viewport = take(CellRect::from({}, size));
    std::optional<CellBuffer> result;

    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), take(app->tree().root()), viewport, viewport));

      std::vector<RenderCommand> commands;
      auto collect = [&](auto &self, Node<Widget> node) -> void {
        const auto snapshot = take(app->tree().inspect(node));

        if (!snapshot.visible) {
          return;
        }

        gallery::SurfaceSink sink(snapshot.geometry, commands);

        take(app->tree().paint(node, sink));

        for (auto child : snapshot.children) {
          self(self, child);
        }
      };

      collect(collect, scene->root());
      result.emplace(take(renderFullFrame(size, commands, policy)));
    }));

    take(app->tree().consumeFrameRequest());

    return std::move(*result);
  }
};

std::string row(const CellBuffer &buffer, int y) {
  std::string result;

  for (int x = 0; x < buffer.size().width; ++x) {
    result += take(buffer.at({x, y})).glyph;
  }

  return result;
}

struct CountingSink final : PaintContext {
  std::size_t commands{};
  const GraphemeText *firstText{};
  std::optional<CellStyle> textStyle;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    commands += values.size();

    for (const auto &value : values) {
      if (const auto *text = std::get_if<TextCommand>(&value)) {
        firstText = &text->text;
        textStyle = text->style;
      }
    }

    return {};
  }
};

void borderContentAndInputClip() {
  Fixture fixture;
  ui::Ref<Button> button;
  ui::Ref<Border> frame;
  int calls{};

  take(ui::mount(*fixture.scene, ui::border(ui::plainButton("abcdefghij",
                                                            [&] {
                                                              ++calls;
                                                            })
                                              .reference(button)
                                              .width(10),
                                            ASCII_BORDER)
                                   .reference(frame)));

  const auto image = fixture.render({4, 3});

  check(row(image, 0) == "+--+" && row(image, 1) == "|ab|" && row(image, 2) == "+--+",
        "fixed child overflow is clipped to the border's inner rectangle");
  check(take(fixture.app->tree().inspect(button.node())).geometry.clip == take(CellRect::from({1, 1}, {2, 1})),
        "arranged child clip is the same inner rectangle used for paint/input");
  take(fixture.app->router().dispatch(MouseInputEvent{3, 1, MouseButton::LEFT, MouseAction::PRESS, {}}));
  check(calls == 0, "border edge does not activate overflowing child");
  take(fixture.app->router().dispatch(MouseInputEvent{1, 1, MouseButton::LEFT, MouseAction::PRESS, {}}));
  take(fixture.app->router().invoke(button.node(), {Action::ACTIVATE}));
  check(calls == 2, "plain button retains existing pointer and semantic action routing");
  take(fixture.app->tree().setEnabled(frame.node(), false));

  const auto disabled = fixture.render({4, 3});

  check(take(disabled.at({0, 0})).style.foreground == RgbColor{128, 128, 128} &&
          take(disabled.at({1, 1})).style.foreground == RgbColor{128, 128, 128},
        "disabled ancestor updates both border and existing button presentation");
  take(fixture.app->router().invoke(button.node(), {Action::ACTIVATE}));
  check(calls == 2, "disabled plain button remains ineligible");
}

void collapsedExtentsAndWidths() {
  for (const auto size :
       {CellSize{0, 0}, CellSize{0, 2}, CellSize{1, 1}, CellSize{1, 2}, CellSize{2, 1}, CellSize{2, 2}}) {
    Fixture fixture;

    take(ui::mount(*fixture.scene, ui::border(ui::label("hidden"), ASCII_BORDER)));

    const auto image = fixture.render(size);

    for (int y = 0; y < size.height; ++y) {
      for (int x = 0; x < size.width; ++x) {
        check(take(image.at({x, y})).glyph == "+", "collapsed border paints only deterministic corners");
      }
    }
  }

  for (const auto size :
       {CellSize{0, 0}, CellSize{1, 0}, CellSize{0, 2}, CellSize{1, 1}, CellSize{2, 1}, CellSize{2, 2}}) {
    Fixture fixture;

    take(ui::mount(*fixture.scene, ui::gauge(.5, ASCII_GAUGE)));

    const auto image = fixture.render(size);

    for (int y = 0; y < size.height; ++y) {
      check(row(image, y) == (size.width == 2 ? "# " : std::string(static_cast<std::size_t>(size.width), ' ')),
            "zero/one/two-cell gauge extents preserve deterministic floor rounding");
    }
  }

  WidthPolicy wide;

  wide.ambiguous = AmbiguousWidth::WIDE;
  wide.emoji = EmojiWidth::ONE;
  wide.revision = 71;

  Fixture fixture(wide);

  take(ui::mount(*fixture.scene, ui::border(ui::gauge(1))));

  const auto image = fixture.render({4, 3});

  check(row(image, 0) == "+--+" && row(image, 1) == "|##|",
        "ambiguous two-cell decorations fall back to ASCII under the complete width policy");
}

void unicodePlainButton() {
  for (const auto ambiguous : {AmbiguousWidth::NARROW, AmbiguousWidth::WIDE}) {
    WidthPolicy policy;

    policy.ambiguous = ambiguous;
    policy.revision = 17;

    Fixture fixture(policy);
    ui::Ref<Button> button;

    take(ui::mount(*fixture.scene, ui::plainButton(button, "\xc2\xb7")));

    const auto width = ambiguous == AmbiguousWidth::NARROW ? 1 : 2;
    const auto measured = take(fixture.app->tree().measure(button.node(), {}));

    check(measured == CellSize{width, 1}, "one-glyph button has no mandatory bracket cells");
    check(row(fixture.render({1, 1}), 0) == (width == 1 ? "\xc2\xb7" : " "),
          "single-glyph button clipping preserves the complete Unicode footprint");
    fixture.render({0, 1});
  }
}

void sizeOrderAndFailures() {
  Fixture fixture;
  ui::Ref<Label> first;
  ui::Ref<Label> second;

  take(ui::mount(*fixture.scene,
                 ui::vbox(ui::label("a").height(2).width(3).flexX(2).reference(first),
                          ui::label("b").reference(second) | ui::width(5) | ui::flexY(3) | ui::height(4))));
  take(fixture.app->tree().read(first.node(), [](const Label &widget) {
    const auto policy = widget.sizePolicy();

    check(policy.width.minimum == 3 && policy.width.maximum == 3 && policy.width.mode == SizeMode::FLEX &&
            policy.width.weight == 2 && policy.height.preferred == 2 && policy.height.mode == SizeMode::FIXED,
          "ordinary modifiers preserve other axes and flex bounds");
  }));
  take(fixture.app->tree().read(second.node(), [](const Label &widget) {
    const auto policy = widget.sizePolicy();

    check(policy.width.preferred == 5 && policy.height.preferred == 4 && policy.height.mode == SizeMode::FIXED,
          "pipe modifiers use the same ordered policy updates");
  }));

  const auto previous = take(fixture.app->tree().inspect(fixture.scene->root())).children;
  ui::Ref<Label> failedRef;
  const auto negative = ui::mount(*fixture.scene, ui::border(ui::label("bad").reference(failedRef).width(-1)));
  const auto zeroWeight = ui::mount(*fixture.scene, ui::gauge(.5) | ui::flex(0));

  check(!negative && std::get<TreeError>(negative.error().cause) == TreeError::INVALID_ARGUMENT && !zeroWeight,
        "invalid size/weight produce typed mount failures");
  check(failedRef.node() == Node<Label>{} &&
          take(fixture.app->tree().inspect(fixture.scene->root())).children == previous,
        "border shares the transaction: no early ref publication and complete rollback");
}

struct AvailableSized final : SizedWidget {
  std::expected<CellSize, TreeError> measure(CellSize available) override {
    return available;
  }
};

void zeroBasisFlexWeights() {
  Fixture fixture;
  ui::Ref<AvailableSized> table;
  ui::Ref<AvailableSized> chart;

  take(ui::mount(*fixture.scene, ui::vbox(ui::custom<AvailableSized>().reference(table).flexY(2),
                                          ui::custom<AvailableSized>().reference(chart).flexY())));
  fixture.render({20, 12});
  check(take(fixture.app->tree().inspect(table.node())).geometry.local.size().height == 8 &&
          take(fixture.app->tree().inspect(chart.node())).geometry.local.size().height == 4,
        "zero preferred flex bases preserve 2:1 weight allocation for viewport-sized intrinsic children");
}

void buttonTextErrorsMatch() {
  Fixture fixture;
  const std::array cases{std::string(TextLimits{}.max_input_bytes + 1, 'a'), std::string("\xff", 1)};
  const std::array expected{TreeError::NODE_LIMIT, TreeError::INVALID_ARGUMENT};
  const std::array expectedText{TextErrorKind::INPUT_LIMIT, TextErrorKind::INVALID_UTF8};

  for (std::size_t index = 0; index < cases.size(); ++index) {
    ui::Ref<Button> normalRef;
    ui::Ref<Button> plainRef;
    const auto normal = ui::mount(*fixture.scene, ui::button(cases[index]).reference(normalRef));
    const auto plain = ui::mount(*fixture.scene, ui::plainButton(cases[index]).reference(plainRef));

    check(!normal && !plain && normal.error().phase == ManagedPhase::MOUNT &&
            plain.error().phase == normal.error().phase,
          "normal and plain button preserve the same typed mount failure phase");
    check(std::get<TreeError>(normal.error().cause) == expected[index] &&
            std::get<TreeError>(plain.error().cause) == expected[index] && normal.error().text &&
            normal.error().text->kind == expectedText[index] && plain.error().text == normal.error().text &&
            !normal.error().cleanup && !plain.error().cleanup,
          "caption chrome preserves resource-limit/UTF-8 causes and exact TextError payload");
    check(normalRef.node() == Node<Button>{} && plainRef.node() == Node<Button>{} &&
            take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
          "rejected normal and plain captions publish no refs and leave no partial subtree");
  }

  int resolutions{};
  ui::TextResolver resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "resolved";
  };
  ui::Ref<Button> unpublished;
  const auto invalidChrome = ui::mount(
    *fixture.scene,
    ui::button(i18n("private-caption"), {}, {}, resolver).chrome(static_cast<ButtonChrome>(9)).reference(unpublished));
  const auto invalidGlyphs =
    ui::mount(*fixture.scene,
              ui::border(ui::label(i18n("private-content"), {}, {}, resolver)).glyphs(static_cast<PrimitiveGlyphs>(9)));

  check(!invalidChrome && !invalidGlyphs && resolutions == 0 && unpublished.node() == Node<Button>{},
        "native scalar enum validation precedes reachable caption resolvers and reference publication");
  check(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
        "rejected native primitive settings leave no mounted descendants");
}

void gaugeValidationAndNoop() {
  Fixture fixture;
  ui::Ref<Gauge> gauge;

  take(ui::mount(*fixture.scene, ui::gauge(.5, ASCII_GAUGE).reference(gauge)));

  const auto image = fixture.render({4, 1});

  check(row(image, 0) == "##  ", "determinate gauge fills floor(width*fraction) cells");
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE, "static gauge is idle");
  take(setGaugeValue(fixture.app->tree(), gauge.node(), .5));
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE, "equal gauge setter requests no redraw");
  check(!setGaugeValue(fixture.app->tree(), gauge.node(), std::numeric_limits<double>::quiet_NaN()),
        "NaN setter is rejected");
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE, "rejected value requests no redraw");
  take(setGaugeValue(fixture.app->tree(), gauge.node(), 1));
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::PAINT, "changed gauge requests paint only");
  check(row(fixture.render({4, 1}), 0) == "####", "maximum fills the gauge exactly");
  take(setGaugeValue(fixture.app->tree(), gauge.node(), 0));
  check(row(fixture.render({4, 1}), 0) == "    ", "minimum clears all previous fill");

  for (const auto value :
       {-1.0, 2.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    const auto result = ui::mount(*fixture.scene, ui::gauge(value));

    check(!result && std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT,
          "invalid initial values fail before creation");
  }

  for (const auto options : {GaugeOptions{1, 1}, GaugeOptions{2, 1},
                             GaugeOptions{-std::numeric_limits<double>::max(), std::numeric_limits<double>::max()}}) {
    check(!ui::mount(*fixture.scene, ui::gauge(1, options)), "invalid or unrepresentable range rejected");
  }
}

void primitiveScalarPaintParity() {
  for (const int mode : {0, 1, 2}) {
    Fixture fixture;
    auto recipe = [&]() -> ui::Description<Widget> {
      if (mode == 0) {
        return ui::detail::Access::erase(
          ui::border(
            ui::gauge(0).range(-1, 1).value(.5).preferredWidth(6).glyphs(PrimitiveGlyphs::ASCII).maxPaintCells(64))
            .glyphs(PrimitiveGlyphs::ASCII)
            .maxPaintCells(64));
      }

      if (mode == 1) {
        auto progress = ui::gauge(0) | ui::minimum(-1) | ui::maximum(1) | ui::value(.5) | ui::preferredWidth(6) |
                        ui::glyphs(PrimitiveGlyphs::ASCII) | ui::maxPaintCells(64);

        return ui::detail::Access::erase(ui::border(std::move(progress)) | ui::glyphs(PrimitiveGlyphs::ASCII) |
                                         ui::maxPaintCells(64));
      }

      ui::SceneResourceOptions options;
      options.format = ui::SceneResourceFormat::CONFIGURATION;

      return ui::resource("defaults{Border{glyphs=ASCII maxPaintCells=64} Gauge{glyphs=ASCII maxPaintCells=64}} "
                          "definitions{Progress=Gauge{minimum=-1e0 maximum=1 preferredWidth=6}} "
                          "scene{Border{Progress{value=.5}}}",
                          options);
    }();

    take(ui::mount(*fixture.scene, std::move(recipe)));
    const auto image = fixture.render({8, 3});

    check(row(image, 0) == "+------+" && row(image, 1) == "|####  |" && row(image, 2) == "+------+",
          "method pipe and configured resource share actual primitive paint and one real content edge");
    take(fixture.scene->prepare(*fixture.app, {8, 3}));
    check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE,
          "unchanged configured primitives do not schedule another frame");
  }

  Fixture receiving;
  ui::ControlContext context;
  context.defaults("Gauge", {.glyphs = PrimitiveGlyphs::ASCII, .minimum = 2, .preferredWidth = 6});
  ui::Ref<Gauge> gauge;
  GaugeOptions actual;
  actual.maximum = 10;
  take(ui::mount(*receiving.scene, ui::gauge(6, actual).reference(gauge), context));
  check(row(receiving.render({6, 1}), 0) == "###   ",
        "partial context bounds validate against actual receiving options instead of guessed constructor defaults");
  take(receiving.app->tree().read(gauge.node(), [](const Gauge &widget) {
    check(widget.value() == 6, "explicit constructor value overrides absent context value");
  }));
}

void chromeAcrossCaptionFactories() {
  Fixture fixture;
  auto model = take(fixture.scene->state(std::string("Go")));
  ui::Ref<Label> source;
  std::array<ui::Ref<Button>, 7> refs;
  std::vector<ui::Description<Button>> buttons;
  buttons.push_back(ui::button("Go").chrome(ButtonChrome::NONE).reference(refs[0]));
  buttons.push_back((ui::button(text("Go")) | ui::chrome(ButtonChrome::NONE)).reference(refs[1]));
  buttons.push_back(ui::plainButton("Go").chrome(ButtonChrome::BRACKETS).reference(refs[2]));
  buttons.push_back(ui::button(source.text()).chrome(ButtonChrome::NONE).reference(refs[3]));
  buttons.push_back(ui::button(model).chrome(ButtonChrome::NONE).reference(refs[4]));
  buttons.push_back(ui::button("Go",
                               [] {
                               })
                      .chrome(ButtonChrome::NONE)
                      .reference(refs[5]));
  buttons.push_back(ui::button("Go").variant("Frameless").reference(refs[6]));
  ui::ControlContext context;
  context.defaults("Button", {.chrome = ButtonChrome::BRACKETS})
    .define("Frameless", "Button", {.chrome = ButtonChrome::NONE});
  take(ui::mount(*fixture.scene, ui::vbox(ui::label(source, "Go"), ui::vbox(std::move(buttons))), context));

  for (std::size_t index = 0; index < refs.size(); ++index) {
    const auto measured = take(fixture.app->tree().measure(refs[index].node(), {}));

    check(measured == CellSize{index == 2 ? 6 : 2, 1},
          "ordinary expression plain live state action and named Button factories apply effective chrome");
  }

  const auto image = fixture.render({6, 8});

  for (std::size_t index = 0; index < refs.size(); ++index) {
    check(row(image, static_cast<int>(index + 1)) == (index == 2 ? "[ Go ]" : "Go    "),
          "effective chrome changes the real retained Button paint for every caption factory");
  }
}

void primitiveScopedNamesAndRollback() {
  Fixture fixture;
  ui::ControlContext context;
  context.define("Border", "Label").define("Gauge", "Label");
  int children{};
  ui::Ref<Border> border;
  ui::Ref<Label> content;
  auto failed = ui::mount(
    *fixture.scene,
    ui::border(
      ui::label(content, "hidden").setup([&](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
        ++children;

        return {};
      }))
      .reference(border),
    context);

  check(!failed && children == 0 && border.node() == Node<Border>{} && content.node() == Node<Label>{},
        "native Border cannot silently adopt a caller's unrelated Border declaration");
  check(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
        "rejected primitive identity leaves no partial mounted subtree");
  check(!ui::mount(*fixture.scene, ui::gauge(.5), context),
        "native Gauge cannot silently adopt a caller's unrelated Gauge declaration");

  for (const bool invalidRange : {false, true}) {
    int effects{};
    ui::Ref<Label> unpublished;
    auto child = invalidRange ? ui::gauge(.5).range(1, 1) : ui::gauge(.5).maxPaintCells(0);
    const auto rejected = ui::mount(
      *fixture.scene,
      ui::vbox(
        ui::label(unpublished, "earlier").setup([&](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
          ++effects;

          return {};
        }),
        std::move(child)));

    check(!rejected && unpublished.node() == Node<Label>{} &&
            take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
          "later invalid primitive rolls back earlier mounted children and never publishes references");
    check(effects == 1, "earlier setup follows the existing partial-mount callback order");
  }
}

void themeAndManualStyle() {
  Fixture fixture;
  ui::Ref<Border> border;
  ui::Ref<Gauge> gauge;
  ControlStyle manual;

  manual.normal.foreground = RgbColor{1, 2, 3};
  manual.normal.background = RgbColor{4, 5, 6};
  take(
    ui::mount(*fixture.scene,
              ui::border(ui::gauge(.5, ASCII_GAUGE, manual).reference(gauge), ASCII_BORDER, manual).reference(border)));

  const auto initial = fixture.render({6, 3});

  check(take(initial.at({0, 0})).style.foreground == RgbColor{1, 2, 3} &&
          take(initial.at({1, 1})).style.background == RgbColor{4, 5, 6},
        "primitive manual style owns foreground/background");

  StyleSheet theme;
  StyleRule rule;

  rule.selector.type = "*";
  rule.declarations.foreground = RgbColor{9, 8, 7};
  theme.rules.push_back(rule);
  take(fixture.scene->theme(theme));

  const auto changed = fixture.render({6, 3});

  check(take(changed.at({0, 0})).style.foreground == RgbColor{9, 8, 7} &&
          take(changed.at({1, 1})).style.foreground == RgbColor{9, 8, 7},
        "existing theme engine overrides primitive manual defaults");
  take(fixture.app->tree().setEnabled(border.node(), false));
  fixture.render({6, 3});
  take(fixture.app->tree().consumeFrameRequest());
  take(syncBorder(fixture.app->tree(), border.node()));
  take(syncGauge(fixture.app->tree(), gauge.node()));
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "equal disabled snapshots do not schedule more frames");
}

void scopedPrimitiveStyles() {
  ui::Ref<Border> border;
  ui::Ref<Gauge> gauge;
  ui::Ref<Button> button;
  ControlStyle defaults;

  defaults.normal.foreground = RgbColor{1, 2, 3};

  auto owner = ui::scene(ui::border(ui::vbox(ui::gauge(.5, ASCII_GAUGE, defaults).reference(gauge),
                                             ui::plainButton(button, "Exit", defaults)),
                                    ASCII_BORDER, defaults)
                           .reference(border));
  std::string source = "* { foreground: #112233; background: #445566; }";
  const auto buttonStyle = ui::style(button, source);

  source.assign(source.size(), 'x');
  owner | ui::style(border, "* { foreground: #123456; }");
  owner | ui::style(gauge, "* { foreground: #abcdef; }");
  owner | buttonStyle;
  take(owner.prepareChecked());

  auto verify = [&] {
    owner.configure([&](ManagedScene &managed, Application &app) {
      take(managed.prepare(app, {10, 5}));
      const auto viewport = take(CellRect::from({}, {10, 5}));
      take(app.tree().frame([&] {
        take(layoutTree(app.tree(), take(app.tree().root()), viewport, viewport));
        auto verifyNode = [&](auto node, RgbColor foreground) {
          take(app.tree().read(node, [&](const Widget &widget) {
            check(widget.computedStyle() && widget.computedStyle()->normal.foreground == foreground,
                  "exact Ref application style survives theme replacement");
          }));
          CountingSink sink;
          take(app.tree().paint(node, sink));
          check(sink.textStyle && sink.textStyle->foreground == foreground,
                "primitive paint consumes the resolved exact Ref foreground");
        };
        verifyNode(border.node(), {0x12, 0x34, 0x56});
        verifyNode(gauge.node(), {0xab, 0xcd, 0xef});
        verifyNode(button.node(), {0x11, 0x22, 0x33});
        take(app.tree().read(button.node(), [&](const Button &widget) {
          check(widget.computedStyle()->normal.background == RgbColor{0x44, 0x55, 0x66},
                "deferred scoped PSS owns its original source bytes");
        }));
      }));
    });
    take(owner.checked());
  };

  verify();
  for (const auto color : {RgbColor{9, 8, 7}, RgbColor{7, 8, 9}}) {
    StyleSheet theme;
    StyleRule rule;
    rule.selector.type = "*";
    rule.declarations.foreground = color;
    rule.declarations.background = color;
    theme.rules.push_back(rule);
    owner.theme(std::move(theme));
    verify();
  }
  take(owner.close());
}

void scopedStyleFactoryRollback() {
  ui::Ref<Button> button;
  ui::Ref<Border> border;
  const auto style = ui::style(button, "* { foreground: #123456; }");
  {
    auto failed = ui::scene(
      ui::border(ui::vbox(ui::plainButton(button, "first"), ui::gauge(std::numeric_limits<double>::quiet_NaN())))
        .reference(border));
    failed | style;
    const auto result = failed.prepareChecked();
    check(!result && std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT &&
            !result.error().cleanup && button.node() == Node<Button>{} && border.node() == Node<Border>{},
          "failed framed factory publishes neither Ref nor partial scoped style");
  }
  auto reused = ui::scene(ui::border(ui::plainButton(button, "second")).reference(border));
  reused | style;
  take(reused.prepareChecked());
  reused.configure([&](ManagedScene &, Application &app) {
    take(app.tree().read(button.node(), [&](const Button &widget) {
      check(widget.computedStyle() && widget.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56},
            "same owning scoped stage and Ref remain usable after failed factory cleanup");
    }));
  });
  take(reused.checked());
  take(reused.close());
}

void scopedPrimitiveIdle() {
  example::ReplayTerminal terminal({{20, "\x1b[21~", {}}}, 80, {20, 5});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Gauge> gauge;
  ui::Ref<Button> button;
  ui::Ref<Border> border;
  auto owner = ui::scene(
    ui::border(ui::vbox(ui::gauge(.5).reference(gauge), ui::plainButton(button, "Exit"))).reference(border), options);
  const auto gaugeStyle = ui::style(gauge, "* { bold: true; }");
  const auto buttonStyle = ui::style(button, "* { bold: true; }");
  const auto borderStyle = ui::style(border, "* { bold: true; }");
  owner | gaugeStyle;
  owner | buttonStyle;
  owner | borderStyle;
  int reuses{};
  owner.onKey(key::F10, [&] {
    owner | gaugeStyle;
    owner | buttonStyle;
    owner | borderStyle;
    ++reuses;
  });
  const auto result = take(owner.run(terminal));
  check(reuses == 1 && result.frames == 1 && terminal.closed,
        "reusing equal primitive scoped styles adds no idle frame and closes the terminal");
}

void paintBudgetAndAllocation() {
  Border border({}, ASCII_BORDER);
  Gauge gauge(.5, {}, ASCII_GAUGE);
  CountingSink sink;
  const auto huge =
    take(CellRect::from({}, {std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max()}));

  take(border.arrange(huge));
  take(gauge.arrange(huge));
  const auto failedBorder = border.paint(sink);
  const auto failedGauge = gauge.paint(sink);

  check(!failedBorder && failedBorder.error() == TreeError::WOULD_BLOCK && !failedGauge &&
          failedGauge.error() == TreeError::WOULD_BLOCK && sink.commands == 0,
        "huge clipped allocations reject before any paint submission or extent-sized loop");
  Border limited({}, {PrimitiveGlyphs::ASCII, 3});
  CountingSink limitedSink;

  take(limited.arrange(take(CellRect::from({}, {2, 2}))));
  check(limited.paint(limitedSink) == std::unexpected(TreeError::WOULD_BLOCK) && limitedSink.commands == 0,
        "configured perimeter cap rejects before submission at its exact boundary");

  take(border.arrange(take(CellRect::from({}, {6, 3}))));
  take(gauge.arrange(take(CellRect::from({}, {6, 1}))));

  allocation_test::remaining = 0;
  const auto paintedBorder = border.paint(sink);
  const auto paintedGauge = gauge.paint(sink);
  allocation_test::remaining = -1;

  check(paintedBorder && paintedGauge && sink.commands == 18, "primitive paint itself performs no heap allocation");
  check(sink.firstText && sink.firstText->bytes() == "#", "retained command text belongs to live widget after paint");
}

void allocationRollback() {
  bool succeeded{};

  constexpr std::ptrdiff_t MAX_ALLOCATION_ATTEMPTS = 512;

  for (std::ptrdiff_t budget = 0; budget < MAX_ALLOCATION_ATTEMPTS && !succeeded; ++budget) {
    Fixture fixture;
    ui::Ref<Label> child;
    ui::Ref<Border> border;
    auto recipe = ui::border(ui::label("payload").reference(child)).reference(border);

    allocation_test::remaining = budget;
    const auto result = ui::mount(*fixture.scene, std::move(recipe));
    allocation_test::remaining = -1;
    succeeded = result.has_value();

    if (succeeded) {
      std::cout << "framed mount succeeds after " << budget << " allocation attempts\n";
    }

    if (!succeeded) {
      check(std::get<TreeError>(result.error().cause) == TreeError::NO_MEMORY && !result.error().cleanup,
            "allocation failure preserves typed mount error and cleanup");
      check(child.node() == Node<Label>{} && border.node() == Node<Border>{} &&
              take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
            "allocation failure rolls back entire framed subtree and all ref reservations");
    }
  }

  check(succeeded, "bounded OOM sweep reaches a successful mount");
}

struct InvalidClip final : Row {
  std::expected<CellRect, TreeError> childClip(CellSize) const override {
    return take(CellRect::from({10, 10}, {1, 1}));
  }
};

void lowLevelClipCompatibility() {
  Fixture fixture;
  const auto root = take(fixture.app->tree().root());
  const auto parent = take(fixture.app->tree().emplace<Widget>(root));
  const auto child = take(fixture.app->tree().emplace<Widget>(parent));
  const auto viewport = take(CellRect::from({}, {10, 10}));
  const auto extra = take(CellRect::from({2, 2}, {1, 1}));

  take(fixture.app->tree().arrange(root, viewport, viewport));
  take(fixture.app->tree().arrange(parent, viewport, viewport));
  take(fixture.app->tree().arrange(child, viewport, extra));
  check(take(fixture.app->tree().inspect(child)).geometry.clip == viewport,
        "legacy three-argument child arrange still inherits parent clip and ignores viewport");
  take(fixture.app->tree().arrange(parent, viewport, viewport, extra));
  take(fixture.app->tree().arrange(child, viewport, viewport));
  check(take(fixture.app->tree().inspect(parent)).geometry.clip == extra &&
          take(fixture.app->tree().inspect(child)).geometry.clip == extra,
        "additive four-argument clip narrows node and all inherited descendants");
}

void invalidCustomClip() {
  Fixture fixture;

  take(ui::mount(*fixture.scene, ui::custom<InvalidClip>()));

  const auto viewport = take(CellRect::from({}, {2, 2}));
  std::expected<void, TreeError> layout;

  take(fixture.app->tree().frame([&] {
    layout = layoutTree(fixture.app->tree(), take(fixture.app->tree().root()), viewport, viewport);
  }));
  check(!layout && layout.error() == TreeError::INVALID_GEOMETRY, "invalid custom child clip is a typed layout error");
}
} // namespace

int main() {
  try {
    nothrowAllocationContracts();
    borderContentAndInputClip();
    collapsedExtentsAndWidths();
    unicodePlainButton();
    sizeOrderAndFailures();
    zeroBasisFlexWeights();
    buttonTextErrorsMatch();
    gaugeValidationAndNoop();
    primitiveScalarPaintParity();
    chromeAcrossCaptionFactories();
    primitiveScopedNamesAndRollback();
    themeAndManualStyle();
    scopedPrimitiveStyles();
    scopedStyleFactoryRollback();
    scopedPrimitiveIdle();
    paintBudgetAndAllocation();
    allocationRollback();
    lowLevelClipCompatibility();
    invalidCustomClip();
  } catch (const std::exception &error) {
    allocation_test::remaining = -1;
    std::cerr << error.what() << " (first checked failure line " << failureLine << ")\n";

    return 1;
  }

  std::cout << "compact primitive contracts passed\n";

  return 0;
}
