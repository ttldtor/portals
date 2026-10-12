// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/style_static.hpp>

#include <functional>
#include <limits>
#include <thread>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
thread_local void (*onAllocate)() = nullptr;
thread_local std::size_t allocationsBeforeHook{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  if (allocation_test::onAllocate) {
    if (allocation_test::allocationsBeforeHook) {
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
  if (auto *value = std::malloc(size ? size : 1)) {
    return value;
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

using namespace portals;
namespace sc = style::ct;

namespace portals {
// White-box limits are otherwise unreachable in a finite regression workload.
struct StyleLayerTestAccess {
  static auto computedStage(StyleEngine &engine, std::size_t phase, void (*hook)()) {
    auto bindings = engine.bindings_;
    StyleSheet empty;
    allocation_test::allocationsBeforeHook = phase;
    allocation_test::onAllocate = hook;
    return engine.install(empty, std::move(bindings));
  }

  static ComputedStyle resolve(const StyleEngine &engine, const StyleTarget &target, ComputedStyle defaults) {
    return engine.resolve(engine.sheet_, engine.layerState_, target, defaults);
  }

  static bool staging(const StyleEngine &engine) {
    return engine.stagingLayers_;
  }

  static void epoch(StyleEngine &engine, std::uint64_t value) {
    engine.layerState_.epoch = value;
  }

  static void identity(StyleEngine &engine, std::uint64_t value) {
    engine.layerState_.nextId = value;
  }

  static void order(StyleEngine &engine, std::uint64_t value) {
    engine.layerState_.nextOrder = value;
  }

  static void owner(StyleEngine &engine, std::uint64_t value) {
    engine.layerState_.owner = value;
  }
};
} // namespace portals

namespace {
template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

StyleSheet foreground(RgbColor value, std::string_view id = {}) {
  StyleRule rule;
  rule.selector.id = id;
  rule.declarations.foreground = value;
  return {.rules = {rule}};
}

constexpr RgbColor RED{220, 10, 10};
constexpr RgbColor GREEN{10, 220, 10};
constexpr RgbColor BLUE{10, 10, 220};
constexpr RgbColor GOLD{220, 220, 10};
constexpr auto LIVE = sc::sheet(sc::parameters(sc::parameter<"color">(sc::rgb(220, 10, 10))),
                                sc::rule(sc::selector(), sc::foreground(sc::token<"color">())));

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  StyleEngine engine{*tree};
  Node<Widget> subject = take(tree->emplace<Widget>(root));

  Fixture() {
    take(engine.bind(subject, {"Widget", "subject", {"one"}}));
  }

  ComputedStyle read() {
    ComputedStyle result;
    take(tree->read(subject, [&](const Widget &widget) {
      result = *widget.computedStyle();
    }));
    return result;
  }
};

struct Hook : Widget {
  mutable std::function<void()> onDefaults;
  std::int32_t width{};
  mutable int calls{};

  ComputedStyle styleDefaults() const override {
    ++calls;
    if (onDefaults) {
      onDefaults();
    }
    ComputedStyle result;
    result.minWidth = width;
    return result;
  }
};

struct ManagedFixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<Button> button;

  ManagedFixture() {
    take(ui::mount(*scene, ui::button("Action").reference(button).style({"Button", "subject", {}})));
    take(scene->prepare(*app, {40, 8}));
  }

  ComputedStyle read() {
    ComputedStyle result;
    take(app->tree().read(button.node(), [&](const Button &widget) {
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

bool cause(const ManagedError &failure, TreeError expected) {
  if (const auto *style = std::get_if<StyleError>(&failure.cause)) {
    return style->cause == expected;
  }
  if (const auto *tree = std::get_if<TreeError>(&failure.cause)) {
    return *tree == expected;
  }
  return false;
}
} // namespace

TEST_CASE("layer opt-in validates budgets and preserves legacy values") {
  Fixture fixture;
  take(fixture.engine.replace(foreground(RED)));
  auto before = fixture.read();
  const auto revision = fixture.engine.revision();
  auto rejected = fixture.engine.enableLayers({.maxLayers = 1});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().cause == TreeError::INVALID_ARGUMENT);
  CHECK(fixture.engine.revision() == revision);
  REQUIRE(fixture.engine.enableLayers());
  CHECK(fixture.read() == before);
  REQUIRE(fixture.engine.baseLayer(StyleTier::THEME));
  const auto enabledRevision = fixture.engine.revision();
  allocation_test::remaining = 0;
  auto equal = fixture.engine.enableLayers();
  allocation_test::remaining = -1;
  REQUIRE(equal);
  CHECK(*equal == Invalidation::NONE);
  CHECK(fixture.engine.revision() == enabledRevision);
  CHECK_FALSE(fixture.engine.enableLayers({.maxLayers = 17}));
}

TEST_CASE("tiers outrank specificity and absent fields preserve independent winners") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  take(fixture.engine.defaults(foreground(GOLD, "subject")));
  take(fixture.engine.theme(foreground(RED, "subject")));
  auto application = foreground(GREEN);
  application.rules[0].declarations.bold = true;
  auto app = take(fixture.engine.addLayer(StyleTier::APPLICATION, "app", application));
  auto user = foreground(BLUE);
  user.rules[0].declarations.bold = false;
  auto userHandle = take(fixture.engine.addLayer(StyleTier::USER, "user", user));
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK_FALSE(fixture.read().normal.bold);
  take(fixture.engine.removeLayer(userHandle));
  CHECK(fixture.read().normal.foreground == GREEN);
  CHECK(fixture.read().normal.bold);
  take(fixture.engine.removeLayer(app));
  CHECK(fixture.read().normal.foreground == RED);
  take(fixture.engine.removeLayer(take(fixture.engine.baseLayer(StyleTier::THEME))));
  CHECK(fixture.read().normal.foreground == GOLD);
}

TEST_CASE("within-tier specificity precedes stable layer order and rule order") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto first = take(fixture.engine.addLayer(StyleTier::APPLICATION, "first", foreground(RED, "subject")));
  auto second = take(fixture.engine.addLayer(StyleTier::APPLICATION, "second", foreground(GREEN)));
  CHECK(fixture.read().normal.foreground == RED);
  take(fixture.engine.replaceLayer(first, foreground(BLUE)));
  CHECK(fixture.read().normal.foreground == GREEN);
  StyleSheet two = foreground(RED);
  two.rules.push_back(foreground(GOLD).rules[0]);
  take(fixture.engine.replaceLayer(second, two));
  CHECK(fixture.read().normal.foreground == GOLD);
  take(fixture.engine.removeLayer(first));
  auto readded = take(fixture.engine.addLayer(StyleTier::APPLICATION, "first", foreground(BLUE)));
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK_FALSE(fixture.engine.replaceLayer(first, foreground(RED)));
  REQUIRE(fixture.engine.replaceLayer(readded, foreground(GREEN)));
}

TEST_CASE("bind bindBatch refresh and manual defaults use the complete layer range") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto override = take(fixture.engine.addLayer(StyleTier::USER, "override", foreground(BLUE)));
  take(fixture.engine.bind(fixture.subject, {"Changed", "subject", {}}));
  CHECK(fixture.read().normal.foreground == BLUE);
  const StyleRegistration registration{fixture.subject, {"Batch", "subject", {}}};
  take(fixture.engine.bindBatch(std::span(&registration, 1)));
  CHECK(fixture.read().normal.foreground == BLUE);
  take(fixture.engine.refresh());
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK_FALSE(fixture.engine.replaceLayer(take(fixture.engine.baseLayer(StyleTier::THEME)), foreground(RED)));
  CHECK_FALSE(fixture.engine.addLayer(StyleTier::DEFAULTS, "extra", {}));
  CHECK_FALSE(fixture.engine.addLayer(StyleTier::USER, "THEME", {}));
  CHECK_FALSE(fixture.engine.addLayer(StyleTier::USER, "override", {}));
  take(fixture.engine.removeLayer(override));
}

TEST_CASE("batch rejects duplicate foreign stale and invalid sheets without publication") {
  Fixture fixture;
  Fixture foreign;
  take(fixture.engine.enableLayers());
  take(foreign.engine.enableLayers());
  auto first = take(fixture.engine.addLayer(StyleTier::APPLICATION, "first", foreground(RED)));
  auto second = take(fixture.engine.addLayer(StyleTier::USER, "second", foreground(BLUE)));
  const auto revision = fixture.engine.revision();
  const auto before = fixture.read();
  const std::array duplicate{StyleLayerUpdate{first, foreground(GREEN)}, StyleLayerUpdate{first, std::nullopt}};
  CHECK_FALSE(fixture.engine.updateLayers(duplicate));
  auto other = take(foreign.engine.addLayer(StyleTier::USER, "foreign", {}));
  CHECK_FALSE(fixture.engine.removeLayer(other));
  CHECK_FALSE(fixture.engine.removeLayer({}));
  auto invalid = foreground(GREEN);
  invalid.rules[0].declarations.minWidth = -1;
  const std::array bad{StyleLayerUpdate{first, foreground(GREEN)}, StyleLayerUpdate{second, invalid}};
  CHECK_FALSE(fixture.engine.updateLayers(bad));
  CHECK(fixture.read() == before);
  CHECK(fixture.engine.revision() == revision);
  const std::array good{StyleLayerUpdate{first, foreground(GREEN)}, StyleLayerUpdate{second, std::nullopt}};
  REQUIRE(fixture.engine.updateLayers(good));
  CHECK(fixture.read().normal.foreground == GREEN);
  CHECK_FALSE(fixture.engine.removeLayer(second));
}

TEST_CASE("equal and empty changes allocate nothing while masked updates return NONE") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  const auto red = foreground(RED);
  auto low = take(fixture.engine.addLayer(StyleTier::APPLICATION, "low", red));
  take(fixture.engine.addLayer(StyleTier::USER, "high", foreground(BLUE)));
  const auto revision = fixture.engine.revision();
  allocation_test::remaining = 0;
  auto equal = fixture.engine.replaceLayer(low, red);
  auto empty = fixture.engine.updateLayers({});
  allocation_test::remaining = -1;
  REQUIRE(equal);
  REQUIRE(empty);
  CHECK(*equal == Invalidation::NONE);
  CHECK(fixture.engine.revision() == revision);
  CHECK(take(fixture.engine.replaceLayer(low, foreground(GREEN))) == Invalidation::NONE);
  CHECK(fixture.engine.revision() == revision + 1);
  auto user = take(fixture.engine.baseLayer(StyleTier::THEME));
  take(fixture.engine.removeLayer(user));
}

TEST_CASE("aggregate limits are checked before copying oversized borrowed strings") {
  Fixture fixture;
  SUBCASE("base slots count") {
    take(fixture.engine.enableLayers({.maxLayers = 2}));
    CHECK_FALSE(fixture.engine.addLayer(StyleTier::USER, "extra", {}));
  }
  SUBCASE("aggregate rules") {
    take(fixture.engine.enableLayers({.maxRules = 1}));
    take(fixture.engine.addLayer(StyleTier::APPLICATION, "one", foreground(RED)));
    CHECK_FALSE(fixture.engine.addLayer(StyleTier::USER, "two", foreground(BLUE)));
  }
  SUBCASE("explicit false counts as a declaration") {
    take(fixture.engine.enableLayers({.maxDeclarations = 1}));
    auto sheet = foreground(RED);
    sheet.rules[0].declarations.bold = false;
    CHECK_FALSE(fixture.engine.addLayer(StyleTier::USER, "two", sheet));
  }
  SUBCASE("resident replacement excludes the replaced old payload") {
    take(fixture.engine.enableLayers({.maxRules = 1}));
    auto handle = take(fixture.engine.addLayer(StyleTier::USER, "one", foreground(RED)));
    REQUIRE(fixture.engine.replaceLayer(handle, foreground(BLUE)));
  }
  SUBCASE("huge selector is rejected without allocation") {
    take(fixture.engine.enableLayers());
    auto huge = foreground(RED);
    huge.rules[0].selector.id.assign(2 * 1024 * 1024, 'x');
    allocation_test::remaining = 0;
    auto rejected = fixture.engine.addLayer(StyleTier::USER, "huge", huge);
    allocation_test::remaining = -1;
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().cause == TreeError::INVALID_ARGUMENT);
  }
}

TEST_CASE("whole replacement succeeds by revoking handles and failure preserves epoch") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto layer = take(fixture.engine.addLayer(StyleTier::USER, "one", foreground(BLUE)));
  auto invalid = foreground(RED);
  invalid.rules[0].selector.state = StyleState::FOCUSED;
  invalid.rules[0].declarations.minWidth = 2;
  CHECK_FALSE(fixture.engine.replace(invalid));
  REQUIRE(fixture.engine.replaceLayer(layer, foreground(GREEN)));
  take(fixture.engine.replace(foreground(RED)));
  CHECK(fixture.read().normal.foreground == RED);
  CHECK_FALSE(fixture.engine.removeLayer(layer));
  take(fixture.engine.enableLayers());
  CHECK_FALSE(fixture.engine.removeLayer(layer));
}

TEST_CASE("monotonic epoch identity order and owner reject overflow before commit") {
  Fixture fixture;
  constexpr auto LIMIT = std::numeric_limits<std::uint64_t>::max();
  SUBCASE("enable epoch") {
    StyleLayerTestAccess::epoch(fixture.engine, LIMIT);
    auto failed = fixture.engine.enableLayers();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().cause == TreeError::IDENTITY_EXHAUSTED);
  }
  SUBCASE("add identity") {
    take(fixture.engine.enableLayers());
    StyleLayerTestAccess::identity(fixture.engine, LIMIT);
    auto failed = fixture.engine.addLayer(StyleTier::USER, "one", {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().cause == TreeError::IDENTITY_EXHAUSTED);
  }
  SUBCASE("add order") {
    take(fixture.engine.enableLayers());
    StyleLayerTestAccess::order(fixture.engine, LIMIT);
    auto failed = fixture.engine.addLayer(StyleTier::USER, "one", {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().cause == TreeError::IDENTITY_EXHAUSTED);
  }
  SUBCASE("equal theme handoff owner") {
    take(fixture.engine.enableLayers());
    StyleLayerTestAccess::owner(fixture.engine, LIMIT);
    auto failed = fixture.engine.theme({});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().cause == TreeError::IDENTITY_EXHAUSTED);
  }
  SUBCASE("whole replace epoch") {
    take(fixture.engine.enableLayers());
    StyleLayerTestAccess::epoch(fixture.engine, LIMIT);
    auto failed = fixture.engine.replace({});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().cause == TreeError::IDENTITY_EXHAUSTED);
  }
}

TEST_CASE("wrong thread active frame defaults reentry and throwing defaults preserve transaction") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto handle = take(fixture.engine.addLayer(StyleTier::USER, "one", foreground(RED)));
  std::optional<StyleError> failure;
  std::thread other([&] {
    auto rejected = fixture.engine.removeLayer(handle);
    if (!rejected) {
      failure = rejected.error();
    }
  });
  other.join();
  REQUIRE(failure);
  CHECK(failure->cause == TreeError::WRONG_THREAD);
  take(fixture.tree->frame([&] {
    auto rejected = fixture.engine.removeLayer(handle);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().cause == TreeError::WOULD_BLOCK);
  }));
  auto hook = take(fixture.tree->emplace<Hook>(fixture.root));
  take(fixture.engine.bind(hook, {"Hook", "", {}}));
  take(fixture.tree->edit(hook, [&](Hook &widget) {
    widget.onDefaults = [&] {
      auto rejected = fixture.engine.removeLayer(handle);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().cause == TreeError::WOULD_BLOCK);
    };
  }));
  CHECK(take(fixture.engine.replaceLayer(handle, foreground(BLUE))) == Invalidation::PAINT);
  const auto before = fixture.read();
  const auto revision = fixture.engine.revision();
  take(fixture.tree->edit(hook, [](Hook &widget) {
    widget.onDefaults = [] {
      throw 7;
    };
  }));
  auto rejected = fixture.engine.replaceLayer(handle, foreground(GREEN));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().cause == TreeError::CALLBACK_FAILED);
  CHECK(fixture.read() == before);
  CHECK(fixture.engine.revision() == revision);
  take(fixture.tree->edit(hook, [](Hook &widget) {
    widget.onDefaults = {};
    widget.width = -1;
  }));
  CHECK_FALSE(fixture.engine.replaceLayer(handle, foreground(GREEN)));
}

TEST_CASE("layer allocation sweep keeps every committed value and handle on failure") {
  bool succeeded{};
  for (std::ptrdiff_t budget = 0; budget < 160 && !succeeded; ++budget) {
    Fixture fixture;
    take(fixture.engine.enableLayers());
    auto handle = take(fixture.engine.addLayer(StyleTier::USER, "one", foreground(RED)));
    const auto before = fixture.read();
    const auto revision = fixture.engine.revision();
    const auto next = foreground(BLUE);
    allocation_test::remaining = budget;
    auto changed = fixture.engine.replaceLayer(handle, next);
    allocation_test::remaining = -1;
    if (!changed) {
      CHECK(changed.error().cause == TreeError::NO_MEMORY);
      CHECK(fixture.read() == before);
      CHECK(fixture.engine.revision() == revision);
      REQUIRE(fixture.engine.replaceLayer(handle, next));
    } else {
      succeeded = true;
      CHECK(fixture.read().normal.foreground == BLUE);
    }
  }
  CHECK(succeeded);
}

TEST_CASE("managed live theme changes only its tier and equal external theme revokes authority") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  auto user = take(fixture.scene->addLayer(StyleTier::USER, "override", foreground(BLUE)));
  auto live = take(sc::live(*fixture.scene, LIVE));
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK(take(live.set<"color">(GREEN)));
  CHECK(fixture.read().normal.foreground == BLUE);
  take(fixture.scene->removeLayer(user));
  CHECK(fixture.read().normal.foreground == GREEN);
  take(fixture.scene->theme(foreground(GREEN)));
  auto stale = live.set<"color">(GREEN);
  REQUIRE_FALSE(stale);
  CHECK(cause(stale.error(), TreeError::INVALID_ARGUMENT));
  CHECK_FALSE(live.get<"color">());
  auto replacement = take(sc::live(*fixture.scene, LIVE));
  CHECK(take(replacement.get<"color">()) == RED);
  CHECK_FALSE(live.set<"color">(BLUE));
  take(fixture.scene->removeLayer(take(fixture.scene->baseLayer(StyleTier::THEME))));
  CHECK_FALSE(replacement.get<"color">());
}

TEST_CASE("successful opt-in revokes legacy controller while failed opt-in preserves it") {
  ManagedFixture fixture;
  auto live = take(sc::live(*fixture.scene, LIVE));
  CHECK_FALSE(fixture.scene->enableStyleLayers({.maxLayers = 1}));
  CHECK(take(live.get<"color">()) == RED);
  REQUIRE(live.set<"color">(GREEN));
  take(fixture.scene->enableStyleLayers());
  auto stale = live.set<"color">(GREEN);
  REQUIRE_FALSE(stale);
  CHECK(cause(stale.error(), TreeError::INVALID_ARGUMENT));
  CHECK_FALSE(live.get<"color">());
}

TEST_CASE("pending metadata and invalid layered mutation retain previous computed revision") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  auto layer = take(fixture.scene->addLayer(StyleTier::USER, "one", foreground(BLUE, "new")));
  const auto before = fixture.read();
  const auto revision = fixture.revision();
  take(fixture.scene->id(fixture.button.node(), "new"));
  auto invalid = foreground(RED);
  invalid.rules[0].declarations.minWidth = -1;
  CHECK_FALSE(fixture.scene->replaceLayer(layer, invalid));
  CHECK(fixture.read() == before);
  CHECK(fixture.revision() == revision);
  const auto same = foreground(BLUE, "new");
  take(fixture.scene->replaceLayer(layer, same));
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK(fixture.revision() == revision + 1);
}

TEST_CASE("failed layered theme takeover retains previous live owner and close is typed") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  auto live = take(sc::live(*fixture.scene, LIVE));
  auto invalid = foreground(BLUE);
  invalid.rules[0].declarations.minWidth = -1;
  CHECK_FALSE(fixture.scene->theme(invalid));
  CHECK(take(live.get<"color">()) == RED);
  REQUIRE(live.set<"color">(GREEN));
  auto copy = live;
  take(fixture.scene->close());
  auto closed = copy.get<"color">();
  REQUIRE_FALSE(closed);
  CHECK(cause(closed.error(), TreeError::CLOSED));
  auto layerClosed = fixture.scene->updateLayers({});
  REQUIRE_FALSE(layerClosed);
  CHECK(cause(layerClosed.error(), TreeError::CLOSED));
}

TEST_CASE("paint variants and minWidth follow independent tier winners") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto low = foreground(RED, "subject");
  low.rules[0].declarations.minWidth = 12;
  low.rules[0].declarations.background = GOLD;
  take(fixture.engine.addLayer(StyleTier::APPLICATION, "application", low));
  auto high = foreground(BLUE);
  high.rules[0].selector.state = StyleState::FOCUSED;
  high.rules[0].declarations.underline = false;
  auto user = take(fixture.engine.addLayer(StyleTier::USER, "focused", high));
  CHECK(fixture.read().normal.foreground == RED);
  CHECK(fixture.read().focused.foreground == BLUE);
  CHECK(fixture.read().focused.background == GOLD);
  CHECK(fixture.read().minWidth == 12);
  auto metric = foreground(GREEN);
  metric.rules[0].declarations.minWidth = 4;
  CHECK(take(fixture.engine.replaceLayer(user, metric)) == Invalidation::LAYOUT);
  CHECK(fixture.read().minWidth == 4);
  take(fixture.tree->consumeFrameRequest());
  CHECK(take(fixture.engine.replaceLayer(user, foreground(GREEN))) == Invalidation::LAYOUT);
  CHECK(take(fixture.tree->consumeFrameRequest()) == Invalidation::LAYOUT);
}

TEST_CASE("removed and destroyed engine observers cannot affect new owning engines") {
  Fixture fixture;
  StyleLayerHandle old;
  {
    StyleEngine temporary(*fixture.tree);
    take(temporary.enableLayers());
    old = take(temporary.addLayer(StyleTier::USER, "temporary", {}));
  }
  take(fixture.engine.enableLayers());
  auto rejected = fixture.engine.removeLayer(old);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().cause == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("managed equal empty and repeated opt-in allocate nothing without pending metadata") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  const auto sheet = foreground(RED);
  auto layer = take(fixture.scene->addLayer(StyleTier::USER, "user", sheet));
  const auto revision = fixture.revision();
  allocation_test::remaining = 0;
  auto equal = fixture.scene->replaceLayer(layer, sheet);
  auto empty = fixture.scene->updateLayers({});
  auto enabled = fixture.scene->enableStyleLayers();
  allocation_test::remaining = -1;
  REQUIRE(equal);
  REQUIRE(empty);
  REQUIRE(enabled);
  CHECK(*equal == Invalidation::NONE);
  CHECK(fixture.revision() == revision);
}

TEST_CASE("equal live values publish pending metadata through the same theme transaction") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  take(fixture.scene->addLayer(StyleTier::USER, "user", foreground(BLUE, "pending")));
  auto live = take(sc::live(*fixture.scene, LIVE));
  const auto revision = fixture.revision();
  take(fixture.scene->id(fixture.button.node(), "pending"));
  CHECK_FALSE(take(live.set<"color">(RED)));
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK(fixture.revision() == revision + 1);
}

TEST_CASE("managed layer allocation sweep preserves pending metadata and recomputes once on retry") {
  bool succeeded{};
  for (std::ptrdiff_t budget = 0; budget < 160 && !succeeded; ++budget) {
    ManagedFixture fixture;
    take(fixture.scene->enableStyleLayers());
    auto layer = take(fixture.scene->addLayer(StyleTier::USER, "user", foreground(RED, "pending")));
    const auto before = fixture.read();
    const auto revision = fixture.revision();
    take(fixture.scene->id(fixture.button.node(), "pending"));
    const auto next = foreground(BLUE, "pending");
    allocation_test::remaining = budget;
    auto changed = fixture.scene->replaceLayer(layer, next);
    allocation_test::remaining = -1;
    if (!changed) {
      CHECK(cause(changed.error(), TreeError::NO_MEMORY));
      CHECK(fixture.read() == before);
      CHECK(fixture.revision() == revision);
      REQUIRE(fixture.scene->replaceLayer(layer, next));
    } else {
      succeeded = true;
    }
    CHECK(fixture.read().normal.foreground == BLUE);
    CHECK(fixture.revision() == revision + 1);
  }
  CHECK(succeeded);
}

TEST_CASE("layered live allocation sweep preserves values and authority until commit") {
  bool succeeded{};
  for (std::ptrdiff_t budget = 0; budget < 200 && !succeeded; ++budget) {
    ManagedFixture fixture;
    take(fixture.scene->enableStyleLayers());
    auto live = take(sc::live(*fixture.scene, LIVE));
    const auto before = fixture.read();
    const auto revision = fixture.revision();
    allocation_test::remaining = budget;
    auto changed = live.set<"color">(GREEN);
    allocation_test::remaining = -1;
    if (!changed) {
      CHECK(cause(changed.error(), TreeError::NO_MEMORY));
      CHECK(fixture.read() == before);
      CHECK(fixture.revision() == revision);
      CHECK(take(live.get<"color">()) == RED);
    } else {
      succeeded = true;
      CHECK(take(live.get<"color">()) == GREEN);
      CHECK(fixture.read().normal.foreground == GREEN);
    }
  }
  CHECK(succeeded);
}

namespace {
ManagedScene *allocatorScene{};
const StyleSheet *allocatorSheet{};
bool allocatorTakeoverSucceeded{};

void takeoverDuringAllocation() {
  allocatorTakeoverSucceeded = bool(allocatorScene->theme(*allocatorSheet));
}

void enableDuringAllocation() {
  allocatorTakeoverSucceeded = bool(allocatorScene->enableStyleLayers());
}
} // namespace

TEST_CASE("live materialization rechecks authority after allocator theme takeover or opt-in") {
  ManagedFixture fixture;
  SUBCASE("layered apply takeover") {
    take(fixture.scene->enableStyleLayers());
    auto live = take(sc::live(*fixture.scene, LIVE));
    const auto sheet = foreground(BLUE);
    allocatorScene = fixture.scene.get();
    allocatorSheet = &sheet;
    allocation_test::onAllocate = takeoverDuringAllocation;
    auto stale = live.set<"color">(GREEN);
    allocation_test::onAllocate = nullptr;
    REQUIRE(allocatorTakeoverSucceeded);
    REQUIRE_FALSE(stale);
    CHECK(cause(stale.error(), TreeError::INVALID_ARGUMENT));
    CHECK(fixture.read().normal.foreground == BLUE);
  }
  SUBCASE("layered attach takeover") {
    take(fixture.scene->enableStyleLayers());
    auto prior = take(sc::live(*fixture.scene, LIVE));
    const auto sheet = foreground(BLUE);
    allocatorScene = fixture.scene.get();
    allocatorSheet = &sheet;
    allocation_test::onAllocate = takeoverDuringAllocation;
    auto stale = sc::live(*fixture.scene, LIVE);
    allocation_test::onAllocate = nullptr;
    REQUIRE(allocatorTakeoverSucceeded);
    REQUIRE_FALSE(stale);
    CHECK(cause(stale.error(), TreeError::INVALID_ARGUMENT));
    CHECK(fixture.read().normal.foreground == BLUE);
    CHECK_FALSE(prior.get<"color">());
  }
  SUBCASE("legacy apply opt-in") {
    auto live = take(sc::live(*fixture.scene, LIVE));
    allocatorScene = fixture.scene.get();
    allocation_test::onAllocate = enableDuringAllocation;
    auto stale = live.set<"color">(GREEN);
    allocation_test::onAllocate = nullptr;
    REQUIRE(allocatorTakeoverSucceeded);
    REQUIRE_FALSE(stale);
    CHECK(cause(stale.error(), TreeError::INVALID_ARGUMENT));
    CHECK(fixture.read().normal.foreground == RED);
  }
}

TEST_CASE("each bound widget defaults is called once per unequal layer transaction") {
  Fixture fixture;
  auto hook = take(fixture.tree->emplace<Hook>(fixture.root));
  take(fixture.engine.bind(hook, {"Hook", "hook", {}}));
  take(fixture.engine.enableLayers());
  auto layer = take(fixture.engine.addLayer(StyleTier::USER, "one", foreground(RED)));
  int before{};
  take(fixture.tree->read(hook, [&](const Hook &widget) {
    before = widget.calls;
  }));
  take(fixture.engine.replaceLayer(layer, foreground(BLUE)));
  take(fixture.tree->read(hook, [&](const Hook &widget) {
    CHECK(widget.calls == before + 1);
  }));
  const auto equal = foreground(BLUE);
  take(fixture.engine.replaceLayer(layer, equal));
  take(fixture.tree->read(hook, [&](const Hook &widget) {
    CHECK(widget.calls == before + 1);
  }));
}

TEST_CASE("layered live shared state survives calling handle destruction and rejects defaults reentry") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  auto hook = take(fixture.scene->emplace<Hook>(fixture.scene->root()));
  take(fixture.scene->style(hook, {"Hook", "", {}}));
  auto live = take(sc::live(*fixture.scene, LIVE));
  auto survivor = live;
  std::expected<bool, ManagedError> nested;
  take(fixture.app->tree().edit(hook, [&](Hook &widget) {
    widget.onDefaults = [&] {
      nested = survivor.set<"color">(BLUE);
      live = {};
    };
  }));
  auto changed = live.set<"color">(GREEN);
  REQUIRE(changed);
  CHECK(*changed);
  REQUIRE_FALSE(nested);
  CHECK(cause(nested.error(), TreeError::WOULD_BLOCK));
  CHECK(take(survivor.get<"color">()) == GREEN);
  CHECK(fixture.read().normal.foreground == GREEN);
  CHECK_FALSE(live.get<"color">());
}

TEST_CASE("layered live tuple failure and failed attach retain old theme authority") {
  constexpr auto METRIC =
    sc::sheet(sc::parameters(sc::parameter<"color">(sc::rgb(220, 10, 10)), sc::parameter<"width">(std::int32_t{2})),
              sc::rule(sc::selector(), sc::foreground(sc::token<"color">()), sc::minWidth(sc::token<"width">())));
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  auto live = take(sc::live(*fixture.scene, METRIC));
  const auto before = fixture.read();
  const auto revision = fixture.revision();
  auto failed = live.apply(sc::change<"color">(BLUE), sc::change<"width">(std::int32_t{-1}));
  REQUIRE_FALSE(failed);
  CHECK(cause(failed.error(), TreeError::INVALID_ARGUMENT));
  CHECK(fixture.read() == before);
  CHECK(fixture.revision() == revision);
  CHECK(take(live.get<"color">()) == RED);
  CHECK(take(live.get<"width">()) == 2);
  allocation_test::remaining = 0;
  auto noOwner = sc::live(*fixture.scene, LIVE);
  allocation_test::remaining = -1;
  REQUIRE_FALSE(noOwner);
  CHECK(cause(noOwner.error(), TreeError::NO_MEMORY));
  REQUIRE(live.set<"color">(GREEN));
}

TEST_CASE("base removal retains its slot and invalid batches cannot revoke theme controller") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  take(fixture.scene->styleDefaults(foreground(GOLD)));
  auto live = take(sc::live(*fixture.scene, LIVE));
  auto base = take(fixture.scene->baseLayer(StyleTier::THEME));
  const std::array duplicate{StyleLayerUpdate{base, std::nullopt}, StyleLayerUpdate{base, std::nullopt}};
  CHECK_FALSE(fixture.scene->updateLayers(duplicate));
  CHECK(take(live.get<"color">()) == RED);
  take(fixture.scene->removeLayer(base));
  CHECK(fixture.read().normal.foreground == GOLD);
  CHECK_FALSE(live.get<"color">());
  REQUIRE(fixture.scene->removeLayer(base));
  REQUIRE(fixture.scene->theme(foreground(BLUE)));
  CHECK(fixture.read().normal.foreground == BLUE);
}

TEST_CASE("enabling allocation failure retains legacy sheet and all existing metadata") {
  bool succeeded{};
  for (std::ptrdiff_t budget = 0; budget < 160 && !succeeded; ++budget) {
    Fixture fixture;
    take(fixture.engine.replace(foreground(RED)));
    const auto revision = fixture.engine.revision();
    const auto before = fixture.read();
    allocation_test::remaining = budget;
    auto enabled = fixture.engine.enableLayers();
    allocation_test::remaining = -1;
    if (!enabled) {
      CHECK(enabled.error().cause == TreeError::NO_MEMORY);
      CHECK(fixture.engine.revision() == revision);
      CHECK(fixture.read() == before);
      CHECK_FALSE(fixture.engine.baseLayer(StyleTier::THEME));
    } else {
      succeeded = true;
      CHECK(fixture.read() == before);
    }
  }
  CHECK(succeeded);
}

TEST_CASE("deferred new control registration resolves the existing override layer on prepare") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  take(fixture.scene->addLayer(StyleTier::USER, "user", foreground(BLUE)));
  ui::Ref<Button> added;
  take(ui::mount(*fixture.scene, ui::button("Later").reference(added).style({"Button", "later", {}})));
  take(fixture.scene->prepare(*fixture.app, {40, 8}));
  take(fixture.app->tree().read(added.node(), [](const Button &widget) {
    REQUIRE(widget.computedStyle());
    CHECK(widget.computedStyle()->normal.foreground == BLUE);
  }));
  CHECK(fixture.read().normal.foreground == BLUE);
}

TEST_CASE("PSS whole replacement failure preserves handles and successful replacement revokes them") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto layer = take(fixture.engine.addLayer(StyleTier::USER, "user", foreground(BLUE)));
  auto invalid = fixture.engine.replacePss("* { min-width: -1; }");
  REQUIRE_FALSE(invalid);
  REQUIRE(fixture.engine.replaceLayer(layer, foreground(GREEN)));
  REQUIRE(fixture.engine.replacePss("* { foreground: #dc0a0a; }"));
  CHECK(fixture.read().normal.foreground == RED);
  CHECK_FALSE(fixture.engine.removeLayer(layer));
}

namespace {
StyleEngine *allocatorEngine{};
StyleLayerHandle allocatorLayer;
Node<Widget> allocatorSubject;
bool stageHookFired{};
bool stageNestedBlocked{};

void mutateEngineDuringStaging() {
  if (!StyleLayerTestAccess::staging(*allocatorEngine)) {
    // Caller-owned by-value argument construction precedes entrypoint admission.
    allocation_test::onAllocate = mutateEngineDuringStaging;
    return;
  }
  stageHookFired = true;
  stageNestedBlocked = true;
  const auto check = [&](const auto &result) {
    stageNestedBlocked = stageNestedBlocked && !result && result.error().cause == TreeError::WOULD_BLOCK;
  };
  check(allocatorEngine->enableLayers());
  check(allocatorEngine->addLayer(StyleTier::USER, "nested", *allocatorSheet));
  check(allocatorEngine->replaceLayer(allocatorLayer, *allocatorSheet));
  check(allocatorEngine->removeLayer(allocatorLayer));
  check(allocatorEngine->updateLayers({}));
  const StyleLayerUpdate update{allocatorLayer, *allocatorSheet};
  check(allocatorEngine->updateLayers(std::span(&update, 1)));
  check(allocatorEngine->bind(allocatorSubject, {"Widget", "nested", {}}));
  const StyleRegistration registration{allocatorSubject, {"Widget", "nested", {}}};
  check(allocatorEngine->bindBatch(std::span(&registration, 1)));
  check(allocatorEngine->refresh());
  check(allocatorEngine->replace(*allocatorSheet));
  check(allocatorEngine->replacePss("* { foreground: #ffffff; }"));
}

void mutateSceneDuringMetadataStaging() {
  const auto base = allocatorScene->baseLayer(StyleTier::THEME);
  if (base || !cause(base.error(), TreeError::WOULD_BLOCK)) {
    allocation_test::onAllocate = mutateSceneDuringMetadataStaging;
    return;
  }
  stageHookFired = true;
  stageNestedBlocked = true;
  const auto check = [&](const auto &result) {
    stageNestedBlocked = stageNestedBlocked && !result && cause(result.error(), TreeError::WOULD_BLOCK);
  };
  check(allocatorScene->enableStyleLayers());
  check(allocatorScene->addLayer(StyleTier::USER, "nested", *allocatorSheet));
  check(allocatorScene->replaceLayer(allocatorLayer, *allocatorSheet));
  check(allocatorScene->removeLayer(allocatorLayer));
  check(allocatorScene->updateLayers({}));
  check(allocatorScene->style(allocatorSubject, {"Button", "nested", {}}));
  check(allocatorScene->id(allocatorSubject, "nested"));
  check(allocatorScene->emplace<Widget>(allocatorScene->root()));
  check(allocatorScene->remove(allocatorSubject));
  check(allocatorScene->theme(*allocatorSheet));
  check(allocatorScene->close());
}

void enableDuringLiveTransfer() {
  stageHookFired = true;
  const auto enabled = allocatorScene->enableStyleLayers();
  allocatorTakeoverSucceeded = bool(enabled);
  stageNestedBlocked = !enabled && cause(enabled.error(), TreeError::WOULD_BLOCK);
}
} // namespace

TEST_CASE("all engine owning stages reject allocator reentry before borrowed layer or binding references can die") {
  enum class Outer {
    ENABLE,
    ADD,
    REPLACE_LAYER,
    REMOVE,
    BATCH,
    BIND,
    BIND_BATCH,
    REFRESH,
    REPLACE,
    PSS
  };
  const std::array phases{std::size_t{0}, std::size_t{1},  std::size_t{2},  std::size_t{4},
                          std::size_t{8}, std::size_t{16}, std::size_t{32}, std::size_t{64}};
  for (const auto operation : {Outer::ENABLE, Outer::ADD, Outer::REPLACE_LAYER, Outer::REMOVE, Outer::BATCH,
                               Outer::BIND, Outer::BIND_BATCH, Outer::REFRESH, Outer::REPLACE, Outer::PSS}) {
    for (const auto phase : phases) {
      Fixture fixture;
      StyleLayerHandle layer;
      if (operation != Outer::ENABLE) {
        take(fixture.engine.enableLayers());
        layer = take(fixture.engine.addLayer(StyleTier::USER, "one", foreground(RED)));
      }
      const auto next = foreground(GREEN);
      const auto nested = foreground(BLUE);
      const StyleLayerUpdate update{layer, next};
      const StyleRegistration registration{fixture.subject, {"Widget", "subject", {}}};
      allocatorEngine = &fixture.engine;
      allocatorSheet = &nested;
      allocatorLayer = layer;
      allocatorSubject = fixture.subject;
      stageHookFired = false;
      allocation_test::allocationsBeforeHook = phase;
      allocation_test::onAllocate = mutateEngineDuringStaging;
      bool committed{};
      switch (operation) {
      case Outer::ENABLE:
        committed = bool(fixture.engine.enableLayers());
        break;
      case Outer::ADD:
        committed = bool(fixture.engine.addLayer(StyleTier::USER, "two", next));
        break;
      case Outer::REPLACE_LAYER:
        committed = bool(fixture.engine.replaceLayer(layer, next));
        break;
      case Outer::REMOVE:
        committed = bool(fixture.engine.removeLayer(layer));
        break;
      case Outer::BATCH:
        committed = bool(fixture.engine.updateLayers(std::span(&update, 1)));
        break;
      case Outer::BIND:
        committed = bool(fixture.engine.bind(fixture.subject, {"Widget", "subject", {}}));
        break;
      case Outer::BIND_BATCH:
        committed = bool(fixture.engine.bindBatch(std::span(&registration, 1)));
        break;
      case Outer::REFRESH:
        committed = bool(fixture.engine.refresh());
        break;
      case Outer::REPLACE:
        committed = bool(fixture.engine.replace(next));
        break;
      case Outer::PSS:
        committed = bool(fixture.engine.replacePss("* { foreground: #ffffff; }"));
        break;
      }
      allocation_test::onAllocate = nullptr;
      allocation_test::allocationsBeforeHook = 0;
      REQUIRE(committed);
      if (stageHookFired) {
        CHECK(stageNestedBlocked);
      }
      if (phase == 0) {
        CHECK(stageHookFired);
      }
    }
  }
}

TEST_CASE("legacy owning stages reject nested mode transition while retaining sequential legacy replacement") {
  for (const auto operation : {0, 1, 2, 3, 4}) {
    Fixture fixture;
    const auto initial = foreground(RED);
    take(fixture.engine.replace(initial));
    const auto nested = foreground(BLUE);
    const StyleRegistration registration{fixture.subject, {"Widget", "subject", {}}};
    allocatorEngine = &fixture.engine;
    allocatorSheet = &nested;
    allocatorLayer = {};
    allocatorSubject = fixture.subject;
    stageHookFired = false;
    allocation_test::onAllocate = mutateEngineDuringStaging;
    bool committed{};
    switch (operation) {
    case 0:
      committed = bool(fixture.engine.bind(fixture.subject, {"Widget", "subject", {}}));
      break;
    case 1:
      committed = bool(fixture.engine.bindBatch(std::span(&registration, 1)));
      break;
    case 2:
      committed = bool(fixture.engine.refresh());
      break;
    case 3:
      committed = bool(fixture.engine.replace(initial));
      break;
    case 4:
      committed = bool(fixture.engine.replacePss("* { foreground: #dc0a0a; }"));
      break;
    }
    allocation_test::onAllocate = nullptr;
    REQUIRE(committed);
    REQUIRE(stageHookFired);
    CHECK(stageNestedBlocked);
    CHECK_FALSE(fixture.engine.baseLayer(StyleTier::THEME));
    REQUIRE(fixture.engine.theme(nested));
    CHECK(fixture.read().normal.foreground == BLUE);
  }
}

TEST_CASE("managed pending metadata stage rejects allocator lifecycle and metadata reentry including close") {
  ManagedFixture fixture;
  take(fixture.scene->enableStyleLayers());
  auto layer = take(fixture.scene->addLayer(StyleTier::USER, "user", foreground(RED, "pending")));
  take(fixture.scene->id(fixture.button.node(), "pending"));
  const auto next = foreground(GREEN, "pending");
  const auto nested = foreground(BLUE);
  allocatorScene = fixture.scene.get();
  allocatorSheet = &nested;
  allocatorLayer = layer;
  allocatorSubject = fixture.button.node();
  stageHookFired = false;
  allocation_test::onAllocate = mutateSceneDuringMetadataStaging;
  auto changed = fixture.scene->replaceLayer(layer, next);
  allocation_test::onAllocate = nullptr;
  REQUIRE(changed);
  REQUIRE(stageHookFired);
  CHECK(stageNestedBlocked);
  CHECK(fixture.read().normal.foreground == GREEN);
  REQUIRE(fixture.scene->id(fixture.button.node(), "later"));
  REQUIRE(fixture.scene->close());
}

TEST_CASE("legacy live attach and apply check authority before transfer and reject transition once staging begins") {
  for (const bool attaching : {false, true}) {
    std::size_t materializationTakeovers{};
    std::size_t stagingRejections{};
    for (std::size_t phase = 0; phase < 96; ++phase) {
      ManagedFixture fixture;
      auto live = take(sc::live(*fixture.scene, LIVE));
      allocatorScene = fixture.scene.get();
      stageHookFired = false;
      allocatorTakeoverSucceeded = false;
      stageNestedBlocked = false;
      allocation_test::allocationsBeforeHook = phase;
      allocation_test::onAllocate = enableDuringLiveTransfer;
      bool committed{};
      std::optional<ManagedError> failure;
      if (attaching) {
        auto result = sc::live(*fixture.scene, LIVE);
        committed = bool(result);
        if (!result) {
          failure = result.error();
        }
      } else {
        auto result = live.set<"color">(GREEN);
        committed = bool(result);
        if (!result) {
          failure = result.error();
        }
      }
      allocation_test::onAllocate = nullptr;
      allocation_test::allocationsBeforeHook = 0;
      if (stageHookFired && allocatorTakeoverSucceeded) {
        ++materializationTakeovers;
        REQUIRE_FALSE(committed);
        REQUIRE(failure);
        CHECK(cause(*failure, TreeError::INVALID_ARGUMENT));
        CHECK(fixture.read().normal.foreground == RED);
      } else {
        REQUIRE(committed);
        if (stageHookFired) {
          ++stagingRejections;
          CHECK(stageNestedBlocked);
        }
        CHECK_FALSE(fixture.scene->baseLayer(StyleTier::THEME));
        CHECK(fixture.read().normal.foreground == (attaching ? RED : GREEN));
      }
    }
    CHECK(materializationTakeovers > 0);
    CHECK(stagingRejections > 0);
  }
}

TEST_CASE("plain legacy managed theme protects pending metadata before opt-in") {
  ManagedFixture fixture;
  take(fixture.scene->id(fixture.button.node(), "pending"));
  const auto next = foreground(GREEN, "pending");
  const auto nested = foreground(BLUE);
  allocatorScene = fixture.scene.get();
  allocatorSheet = &nested;
  allocatorLayer = {};
  allocatorSubject = fixture.button.node();
  stageHookFired = false;
  allocation_test::onAllocate = mutateSceneDuringMetadataStaging;
  auto changed = fixture.scene->theme(next);
  allocation_test::onAllocate = nullptr;
  REQUIRE(changed);
  REQUIRE(stageHookFired);
  CHECK(stageNestedBlocked);
  CHECK(fixture.read().normal.foreground == GREEN);
  CHECK_FALSE(fixture.scene->baseLayer(StyleTier::THEME));
}

namespace {
WidgetTree *allocatorTree{};
bool allocatorCloseTree{};
bool allocatorRetired{};

void retireRawTreeDuringComputedStaging() {
  stageHookFired = true;
  allocatorRetired = allocatorCloseTree ? bool(allocatorTree->close()) : bool(allocatorTree->remove(allocatorSubject));
}
} // namespace

TEST_CASE("computed allocation retirement is checked before dereferencing a widget observer") {
  for (const bool layered : {false, true}) {
    for (const bool closing : {false, true}) {
      std::size_t retirementFailures{};
      for (std::size_t phase = 0; phase < 5; ++phase) {
        Fixture fixture;
        if (layered) {
          take(fixture.engine.enableLayers());
          take(fixture.engine.addLayer(StyleTier::USER, "user", foreground(BLUE)));
        }
        auto survivor = take(fixture.tree->emplace<Widget>(fixture.root));
        take(fixture.engine.bind(survivor, {"Widget", "survivor", {}}));
        const auto revision = fixture.engine.revision();
        allocatorTree = fixture.tree.get();
        allocatorSubject = fixture.subject;
        allocatorCloseTree = closing;
        allocatorRetired = false;
        stageHookFired = false;
        auto result = StyleLayerTestAccess::computedStage(fixture.engine, phase, retireRawTreeDuringComputedStaging);
        allocation_test::onAllocate = nullptr;
        allocation_test::allocationsBeforeHook = 0;
        if (stageHookFired) {
          REQUIRE(allocatorRetired);
          if (!result) {
            ++retirementFailures;
            CHECK(result.error().cause == (closing ? TreeError::CLOSED : TreeError::STALE_NODE));
            CHECK(fixture.engine.revision() == revision);
            if (!closing) {
              take(fixture.tree->read(survivor, [&](const Widget &widget) {
                CHECK(widget.styleRevision() == revision);
              }));
            }
          } else {
            CHECK_FALSE(closing);
          }
        }
      }
      CHECK(retirementFailures > 0);
    }
  }
}

TEST_CASE("legacy flat rules and one theme layer agree for many selectors states and independent fields") {
  constexpr std::size_t RULE_COUNT = 896;
  constexpr std::size_t TARGET_COUNT = 28;
  StyleSheet rules;
  rules.rules.reserve(RULE_COUNT);
  for (std::size_t index = 0; index < RULE_COUNT; ++index) {
    StyleRule rule;
    if (index % 4 == 0) {
      rule.selector.type = "Widget";
    }
    if (index % 4 == 1) {
      rule.selector.type = "Other";
    }
    if (index % 4 == 2) {
      rule.selector.type = "*";
    }
    if (index % 5 == 0) {
      rule.selector.id = "subject";
    }
    if (index % 5 == 1) {
      rule.selector.id = "absent";
    }
    if (index % 3 == 0) {
      rule.selector.classes = {"one"};
    }
    if (index % 3 == 1) {
      rule.selector.classes = {"one", "two"};
    }
    rule.selector.state = static_cast<StyleState>(index % 7);
    if (index % 2 == 0) {
      rule.declarations.foreground = RgbColor{std::uint8_t(index % 256), 20, 40};
    }
    if (index % 3 == 0) {
      rule.declarations.background = RgbColor{40, std::uint8_t(index % 256), 20};
    }
    if (index % 5 == 0) {
      rule.declarations.bold = index % 10 != 0;
    }
    if (index % 6 == 0) {
      rule.declarations.italic = index % 12 != 0;
    }
    if (index % 8 == 0) {
      rule.declarations.underline = index % 16 != 0;
    }
    if (rule.selector.state == StyleState::ANY && index % 4 == 0) {
      rule.declarations.minWidth = static_cast<std::int32_t>(index % 30);
    }
    rules.rules.push_back(rule);
  }
  // A later equal-specificity source rule must still replace an earlier explicit false field.
  StyleRule first;
  first.selector.id = "subject";
  first.selector.type = "Widget";
  first.selector.classes = {"one", "two"};
  first.declarations.bold = false;
  first.declarations.minWidth = 31;
  rules.rules.push_back(first);
  first.declarations.bold = true;
  first.declarations.minWidth = 32;
  rules.rules.push_back(first);
  first.selector.state = StyleState::NORMAL;
  first.declarations.minWidth.reset();
  rules.rules.push_back(first);

  Fixture fixture;
  take(fixture.engine.replace(rules));
  take(fixture.engine.enableLayers());
  const auto defaults = resolveStyle(themeFromTokens({RED, BLUE, GREEN, GOLD, RED, GREEN}), {}, {.minWidth = 3});
  std::array<StyleTarget, TARGET_COUNT> targets;
  for (std::size_t index = 0; index < targets.size(); ++index) {
    targets[index].type = index % 2 == 0 ? "Widget" : "Other";
    targets[index].id = index % 3 == 0 ? "subject" : "different";
    if (index % 4 == 0) {
      targets[index].classes = {"one", "two"};
    }
    if (index % 4 == 1) {
      targets[index].classes = {"one"};
    }
    if (index % 4 == 2) {
      targets[index].classes = {"unmatched"};
    }
  }
  std::array<ComputedStyle, TARGET_COUNT> legacy;
  std::array<ComputedStyle, TARGET_COUNT> layered;
  allocation_test::remaining = 0;
  for (std::size_t index = 0; index < targets.size(); ++index) {
    legacy[index] = resolveStyle(rules, targets[index], defaults);
    layered[index] = StyleLayerTestAccess::resolve(fixture.engine, targets[index], defaults);
  }
  allocation_test::remaining = -1;
  for (std::size_t index = 0; index < targets.size(); ++index) {
    CHECK(legacy[index] == layered[index]);
  }
  CHECK(legacy[0].normal.bold);
  CHECK(legacy[0].minWidth == 32);
  take(fixture.engine.bind(fixture.subject, targets[0]));
  CHECK(fixture.read() == resolveStyle(rules, targets[0]));
}
