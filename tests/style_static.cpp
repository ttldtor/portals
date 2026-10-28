// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/style_static.hpp>

#include <thread>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
thread_local void (*onAllocate)() = nullptr;
thread_local std::size_t allocationsBeforeHook{};
thread_local std::unique_ptr<portals::ManagedScene> *sceneToDestroy{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  if (allocation_test::onAllocate) {
    if (allocation_test::allocationsBeforeHook > 0) {
      --allocation_test::allocationsBeforeHook;
    } else {
      std::exchange(allocation_test::onAllocate, nullptr)();
    }
  }

  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  if (void *result = std::malloc(size ? size : 1)) {
    return result;
  }

  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;
namespace sc = style::ct;
constexpr auto PARAMETERS =
  sc::parameters(sc::parameter<"background">(sc::rgb(20, 40, 60)), sc::parameter<"foreground">(sc::rgb(200, 220, 240)),
                 sc::parameter<"bold">(false), sc::parameter<"width">(std::int32_t{2}));
constexpr auto SHEET = sc::sheet(
  PARAMETERS,
  sc::rule(sc::selector(), sc::foreground(sc::token<"foreground">()), sc::background(sc::token<"background">()),
           sc::bold(sc::token<"bold">()), sc::minWidth(sc::token<"width">())),
  sc::rule(sc::selector<"Button", "action", "primary">(), sc::italic(true)),
  sc::rule(sc::selector<"Button", "action", "primary">(), sc::underline(true)),
  sc::rule(sc::selector<"Button">(StyleState::FOCUSED),
           sc::background(sc::mix(sc::token<"background">(), sc::rgb(255, 255, 255), 25))));

static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(SHEET.rules())>> == 4);
static_assert(std::get<0>(SHEET.parameters().defaults()) == RgbColor{20, 40, 60});
static_assert(std::get<1>(SHEET.rules()).selector.TYPE.view() == "Button");
static_assert(std::get<1>(SHEET.rules()).selector.ID.view() == "action");
static_assert(std::get<1>(SHEET.rules()).selector.CLASS_NAMES[0] == "primary");
static_assert(sc::detail::declarations(std::get<3>(SHEET.rules()), SHEET.parameters(), SHEET.parameters().defaults())
                .background == RgbColor{78, 93, 108});
static_assert(sc::detail::declarations(std::get<0>(SHEET.rules()), SHEET.parameters(), SHEET.parameters().defaults())
                .minWidth == 2);
constexpr auto EMPTY = sc::sheet(sc::parameters());
static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(EMPTY.rules())>> == 0);

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool cause(const ManagedError &failure, TreeError expected) {
  if (auto value = std::get_if<TreeError>(&failure.cause)) {
    return *value == expected;
  }

  if (auto value = std::get_if<StyleError>(&failure.cause)) {
    return value->cause == expected;
  }

  return false;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> owner = take(ManagedScene::create(*app));
  ui::Ref<Button> button;

  Fixture() {
    take(ui::mount(*owner, ui::button("Action").reference(button).style({"Button", "action", {"primary"}})));
    take(owner->prepare(*app, {40, 8}));
    take(app->tree().consumeFrameRequest());
  }

  ComputedStyle read() {
    ComputedStyle result;
    take(app->tree().read(button.node(), [&](const Button &widget) {
      REQUIRE(widget.computedStyle());
      result = *widget.computedStyle();
    }));

    return result;
  }

  std::uint64_t revision() {
    std::uint64_t result{};
    take(app->tree().read(button.node(), [&](const Button &widget) {
      result = widget.styleRevision();
    }));

    return result;
  }
};

StyleSheet dynamicSheet() {
  return {.rules = {{{},
                     {.foreground = RgbColor{200, 220, 240},
                      .background = RgbColor{20, 40, 60},
                      .bold = false,
                      .italic = {},
                      .underline = {},
                      .minWidth = 2}},
                    {{"Button", "action", {"primary"}, StyleState::ANY},
                     {.foreground = {}, .background = {}, .bold = {}, .italic = true, .underline = {}, .minWidth = {}}},
                    {{"Button", "action", {"primary"}, StyleState::ANY},
                     {.foreground = {}, .background = {}, .bold = {}, .italic = {}, .underline = true, .minWidth = {}}},
                    {{"Button", "", {}, StyleState::FOCUSED},
                     {.foreground = {},
                      .background = RgbColor{78, 93, 108},
                      .bold = {},
                      .italic = {},
                      .underline = {},
                      .minWidth = {}}}}};
}

struct DefaultsHook final : Widget {
  mutable std::function<void()> callback;

  ComputedStyle styleDefaults() const override {
    if (callback) {
      callback();
    }

    return {};
  }
};
} // namespace

TEST_CASE("static rule expansion preserves every existing cascade state and source order") {
  const auto before = dynamicSheet();
  const auto after = sc::materialize(SHEET);

  for (const auto &target : std::array{StyleTarget{"Button", "action", {"primary"}},
                                       StyleTarget{"Button", "other", {"primary"}}, StyleTarget{"Label", "", {}}}) {
    CHECK(resolveStyle(after, target) == resolveStyle(before, target));
  }

  constexpr auto PRIORITY =
    sc::sheet(sc::parameters(), sc::rule(sc::selector(), sc::foreground(sc::rgb(1, 2, 3)), sc::bold(true)),
              sc::rule(sc::selector<"Button", "action">(), sc::foreground(sc::rgb(4, 5, 6))),
              sc::rule(sc::selector<"Button", "action">(), sc::foreground(sc::rgb(7, 8, 9))),
              sc::rule(sc::selector<"Button">(), sc::foreground(sc::rgb(10, 11, 12))));
  const auto computed = resolveStyle(sc::materialize(PRIORITY), {"Button", "action", {}});

  CHECK(computed.normal.foreground == RgbColor{7, 8, 9});
  CHECK(computed.normal.bold);
  CHECK(sc::materialize(EMPTY).rules.empty());
}

TEST_CASE("constant standard theme reproduces existing ThemeTokens helper for all states") {
  constexpr ThemeTokens TOKENS{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}, {16, 17, 18}};
  constexpr auto STANDARD = sc::themeFromTokens(TOKENS);
  static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(STANDARD.rules())>> == 6);
  static_assert(std::get<4>(STANDARD.parameters().defaults()) == TOKENS.selection);
  const auto before = portals::themeFromTokens(TOKENS);
  const auto after = sc::materialize(STANDARD);
  CHECK(resolveStyle(after, {"Button", "", {}}) == resolveStyle(before, {"Button", "", {}}));
  CHECK(resolveStyle(after, {"LineEdit", "", {}}) == resolveStyle(before, {"LineEdit", "", {}}));
  CHECK(resolveStyle(after, {"Border", "", {}}) == resolveStyle(before, {"Border", "", {}}));
  const ComputedStyle italicFallback{.selected = {.bold = true, .italic = true}};

  CHECK_FALSE(resolveStyle(after, {"Table", "", {}}, italicFallback).selected.italic);
  CHECK(resolveStyle(after, {"Table", "", {}}, italicFallback) ==
        resolveStyle(before, {"Table", "", {}}, italicFallback));

  const CellStyle expectedFrame{TOKENS.accent, TOKENS.background, true};
  const auto initialFrame = resolveStyle(after, {"Border", "", {}}).frame[1];

  CHECK(initialFrame.line == expectedFrame);
  CHECK(initialFrame.title == expectedFrame);
  CHECK(initialFrame.ends == expectedFrame);

  const auto initialScrollbar = resolveStyle(after, {"ScrollArea", "", {}}).scrollbar[1];

  CHECK(initialScrollbar.track == expectedFrame);
  CHECK(initialScrollbar.thumb == expectedFrame);
  CHECK(initialScrollbar.ends == expectedFrame);
  CHECK(initialScrollbar.corner == expectedFrame);

  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, STANDARD));
  CHECK(take(live.set<"selection">(RgbColor{70, 80, 90})));
  CHECK(fixture.read().selected.background == RgbColor{70, 80, 90});
  CHECK(take(live.set<"accent">(RgbColor{20, 30, 40})));
  const auto updatedFrame = fixture.read().frame[1];

  REQUIRE(updatedFrame.line);
  CHECK(updatedFrame.line->foreground == RgbColor{20, 30, 40});
  CHECK(updatedFrame.line->background == TOKENS.background);
  CHECK(updatedFrame.title == updatedFrame.line);
  CHECK(updatedFrame.ends == updatedFrame.line);

  const auto updatedScrollbar = fixture.read().scrollbar[1];

  CHECK(updatedScrollbar.track == updatedFrame.line);
  CHECK(updatedScrollbar.thumb == updatedFrame.line);
  CHECK(updatedScrollbar.ends == updatedFrame.line);
  CHECK(updatedScrollbar.corner == updatedFrame.line);
}

TEST_CASE("live scalar changes use existing paint layout invalidation and equal no-op") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  take(fixture.app->tree().consumeFrameRequest());
  const auto initial = fixture.revision();
  auto equal = live.set<"background">(RgbColor{20, 40, 60});

  CHECK_FALSE(take(equal));
  CHECK(fixture.revision() == initial);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK(take(live.set<"background">(RgbColor{40, 60, 80})));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::PAINT);
  CHECK(fixture.read().normal.background == RgbColor{40, 60, 80});
  CHECK(fixture.read().focused.background == RgbColor{93, 108, 123});
  CHECK(take(live.set<"width">(std::int32_t{12})));
  CHECK(fixture.read().minWidth == 12);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::LAYOUT);
  CHECK(take(fixture.app->tree().inspect(fixture.owner->root())).children ==
        std::vector<Node<Widget>>{fixture.button.node()});
}

TEST_CASE("atomic palette batch commits all values only after existing engine success") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  take(fixture.app->tree().consumeFrameRequest());
  const auto old = fixture.read();
  const auto revision = fixture.revision();
  const auto invalid = live.apply(sc::change<"background">(RgbColor{1, 2, 3}), sc::change<"width">(std::int32_t{-1}));

  REQUIRE_FALSE(invalid);
  CHECK(cause(invalid.error(), TreeError::INVALID_ARGUMENT));
  CHECK(std::holds_alternative<StyleError>(invalid.error().cause));
  CHECK(take(live.get<"background">()) == RgbColor{20, 40, 60});
  CHECK(take(live.get<"width">()) == 2);
  CHECK(fixture.read() == old);
  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK(take(live.apply(sc::change<"foreground">(RgbColor{255, 255, 255}), sc::change<"background">(RgbColor{1, 2, 3}),
                        sc::change<"bold">(true))));
  CHECK(fixture.read().normal.foreground == RgbColor{255, 255, 255});
  CHECK(fixture.read().normal.background == RgbColor{1, 2, 3});
  CHECK(fixture.read().normal.bold);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::PAINT);
}

TEST_CASE("allocation sweep leaves live values and installed styles atomic") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  take(fixture.app->tree().consumeFrameRequest());
  const auto old = fixture.read();
  const auto revision = fixture.revision();
  bool committed{};
  std::size_t failures{};

  for (std::ptrdiff_t budget = 0; budget < 256 && !committed; ++budget) {
    allocation_test::remaining = budget;
    const auto result = live.set<"background">(RgbColor{90, 100, 110});
    allocation_test::remaining = -1;

    if (result) {
      committed = *result;
    } else {
      ++failures;
      CHECK(cause(result.error(), TreeError::NO_MEMORY));
      CHECK(take(live.get<"background">()) == RgbColor{20, 40, 60});
      CHECK(fixture.read() == old);
      CHECK(fixture.revision() == revision);
      CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
    }
  }

  CHECK(committed);
  CHECK(failures > 0);
  CHECK(take(live.get<"background">()) == RgbColor{90, 100, 110});
}

TEST_CASE("failed live attachment publishes no controller and preserves existing mounted styles") {
  Fixture fixture;
  const auto before = fixture.read();
  const auto revision = fixture.revision();
  allocation_test::remaining = 0;
  const auto failed = sc::live(*fixture.owner, SHEET);
  allocation_test::remaining = -1;

  REQUIRE_FALSE(failed);
  CHECK(cause(failed.error(), TreeError::NO_MEMORY));
  CHECK(fixture.read() == before);
  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.owner->close());
  CHECK(take(fixture.app->tree().inspect(fixture.app->root())).children.empty());
}

TEST_CASE("materialized selectors own bytes after constant descriptor scope ends") {
  const auto detached = [] {
    constexpr auto LOCAL =
      sc::sheet(sc::parameters(),
                sc::rule(sc::selector<"Button", "long-local-identifier-owned-by-the-rule", "primary", "toolbar">(),
                         sc::bold(true)));

    return sc::materialize(LOCAL);
  }();
  CHECK(detached.rules[0].selector.type == "Button");
  CHECK(detached.rules[0].selector.id == "long-local-identifier-owned-by-the-rule");
  CHECK(detached.rules[0].selector.classes == std::vector<std::string>{"primary", "toolbar"});
  CHECK(
    resolveStyle(detached, {"Button", "long-local-identifier-owned-by-the-rule", {"toolbar", "primary"}}).normal.bold);
}

TEST_CASE("equal live update allocates nothing and creates no style transaction") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  const auto revision = fixture.revision();
  take(fixture.app->tree().consumeFrameRequest());
  allocation_test::remaining = 0;
  const auto result = live.apply(sc::change<"background">(RgbColor{20, 40, 60}), sc::change<"width">(std::int32_t{2}));
  allocation_test::remaining = -1;

  CHECK_FALSE(take(result));
  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("allocation reentry can destroy scene before live installation reaches its observer") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  allocation_test::sceneToDestroy = &fixture.owner;
  allocation_test::onAllocate = [] {
    allocation_test::sceneToDestroy->reset();
  };
  const auto result = live.set<"background">(RgbColor{90, 100, 110});

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::CLOSED));
  CHECK_FALSE(fixture.owner);
  CHECK_FALSE(live.get<"background">());
}

TEST_CASE("attachment rechecks scene lifetime after state and sheet allocations") {
  for (std::size_t skipped = 0; skipped < 2; ++skipped) {
    Fixture fixture;
    const auto root = fixture.owner->root().id();
    allocation_test::sceneToDestroy = &fixture.owner;
    allocation_test::allocationsBeforeHook = skipped;
    allocation_test::onAllocate = [] {
      allocation_test::sceneToDestroy->reset();
    };
    const auto result = sc::live(*fixture.owner, SHEET);

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::CLOSED));
    CHECK(result.error().node == root);
    CHECK_FALSE(fixture.owner);
  }
}

TEST_CASE("attachment allocation failure reports captured root after allocator destroys scene") {
  Fixture fixture;
  const auto root = fixture.owner->root().id();
  allocation_test::sceneToDestroy = &fixture.owner;
  allocation_test::onAllocate = [] {
    allocation_test::sceneToDestroy->reset();
    throw std::bad_alloc();
  };
  const auto result = sc::live(*fixture.owner, SHEET);

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::NO_MEMORY));
  CHECK(result.error().node == root);
  CHECK_FALSE(fixture.owner);
}

TEST_CASE("copies share values but never retain scene lifetime or bypass its boundary") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  auto copy = live;
  CHECK(take(copy.set<"bold">(true)));
  CHECK(take(live.get<"bold">()));
  std::expected<bool, ManagedError> wrong;
  std::thread worker([&] {
    wrong = copy.set<"bold">(true);
  });
  worker.join();
  REQUIRE_FALSE(wrong);
  CHECK(cause(wrong.error(), TreeError::WRONG_THREAD));
  take(fixture.app->tree().frame([&] {
    const auto blocked = copy.set<"bold">(true);
    REQUIRE_FALSE(blocked);
    CHECK(cause(blocked.error(), TreeError::WOULD_BLOCK));
  }));
  take(fixture.owner->close());
  const auto closed = copy.set<"bold">(true);
  REQUIRE_FALSE(closed);
  CHECK(cause(closed.error(), TreeError::CLOSED));
  fixture.owner.reset();
  const auto expired = copy.get<"bold">();
  REQUIRE_FALSE(expired);
  CHECK(cause(expired.error(), TreeError::CLOSED));
}

TEST_CASE("custom defaults reentry is blocked and hook may destroy calling handle") {
  Fixture fixture;
  auto hook = take(fixture.owner->emplace<DefaultsHook>(fixture.owner->root()));
  take(fixture.owner->style(hook, {"Custom", "", {}}));
  auto live = take(sc::live(*fixture.owner, SHEET));
  auto survivor = live;
  std::expected<bool, ManagedError> nested;

  take(fixture.app->tree().edit(hook, [&](DefaultsHook &widget) {
    widget.callback = [&] {
      nested = survivor.set<"bold">(true);
      live = {};
    };
  }));
  const auto result = live.set<"background">(RgbColor{2, 4, 6});

  CHECK(take(result));
  REQUIRE_FALSE(nested);
  CHECK(cause(nested.error(), TreeError::WOULD_BLOCK));
  CHECK(take(survivor.get<"background">()) == RgbColor{2, 4, 6});
  CHECK(fixture.read().normal.background == RgbColor{2, 4, 6});
  CHECK_FALSE(live.get<"background">());
}

TEST_CASE("later custom defaults failure does not publish partially staged styles or values") {
  Fixture fixture;
  auto hook = take(fixture.owner->emplace<DefaultsHook>(fixture.owner->root()));
  take(fixture.owner->style(hook, {"Custom", "", {}}));
  auto live = take(sc::live(*fixture.owner, SHEET));
  take(fixture.app->tree().edit(hook, [](DefaultsHook &widget) {
    widget.callback = [] {
      throw std::runtime_error("failure after the first widget");
    };
  }));
  take(fixture.app->tree().consumeFrameRequest());
  const auto old = fixture.read();
  const auto revision = fixture.revision();
  const auto result = live.set<"background">(RgbColor{2, 4, 6});

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::CALLBACK_FAILED));
  CHECK(fixture.read() == old);
  CHECK(fixture.revision() == revision);
  CHECK(take(live.get<"background">()) == RgbColor{20, 40, 60});
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("managed callback owns copied observer and remains usable after local handle dies") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  take(fixture.owner->onClick(fixture.button.node(), [copy = live] {
    (void)copy.set<"bold">(true);
  }));
  live = {};
  take(fixture.app->router().invoke(fixture.button.node(), {Action::ACTIVATE}));
  CHECK(fixture.read().normal.bold);
  take(fixture.owner->close());
  take(fixture.app->router().focused());
  CHECK(take(fixture.app->tree().inspect(fixture.app->root())).children.empty());
}

TEST_CASE("external runtime themes remain composable and equal updates do not silently reinstall") {
  Fixture fixture;
  auto live = take(sc::live(*fixture.owner, SHEET));
  take(fixture.owner->theme(turboTheme()));
  const auto runtime = fixture.read();
  const auto revision = fixture.revision();
  CHECK_FALSE(take(live.set<"bold">(false)));
  CHECK(fixture.read() == runtime);
  CHECK(fixture.revision() == revision);
  CHECK(take(live.set<"bold">(true)));
  CHECK(fixture.read().normal.background == RgbColor{20, 40, 60});
}
