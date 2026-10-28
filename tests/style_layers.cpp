// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/style_pss.hpp>

#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
thread_local bool recoverAfterFailure{};
thread_local bool failureObserved{};
thread_local std::size_t rejectedAllocations{};
thread_local std::size_t allocationsAfterFailure{};
thread_local void (*onAllocate)() = nullptr;
thread_local std::size_t allocationsBeforeHook{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  if (allocation_test::failureObserved) {
    ++allocation_test::allocationsAfterFailure;
  }

  if (allocation_test::onAllocate) {
    if (allocation_test::allocationsBeforeHook) {
      --allocation_test::allocationsBeforeHook;
    } else {
      std::exchange(allocation_test::onAllocate, nullptr)();
    }
  }
  if (allocation_test::remaining == 0) {
    ++allocation_test::rejectedAllocations;

    if (allocation_test::recoverAfterFailure) {
      allocation_test::failureObserved = true;
      allocation_test::remaining = -1;
    }

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
  static auto preview(StyleEngine &engine, const StyleSheet &sheet, StyleLayerHandle &handle,
                      std::optional<StyleLayerOptions> options = {}) {
    const auto capture = engine.capturePreview();
    return engine.mutate(StyleEngine::Mutation::PREVIEW, {}, {}, {}, &sheet, {}, {}, &handle, nullptr, nullptr, options,
                         &capture);
  }

  static auto finishPreview(StyleEngine &engine, StyleLayerHandle handle, bool commit) {
    const auto capture = engine.capturePreview();
    return engine.mutate(commit ? StyleEngine::Mutation::PREVIEW_COMMIT : StyleEngine::Mutation::PREVIEW_CANCEL, {}, {},
                         {}, nullptr, {}, {}, &handle, nullptr, nullptr, {}, &capture);
  }

  static auto previewAfterCopy(StyleEngine &engine, const StyleSheet &sheet, StyleLayerHandle &handle) {
    const auto capture = engine.capturePreview();
    const StyleSheet staged(sheet);
    return engine.mutate(StyleEngine::Mutation::PREVIEW, {}, {}, {}, &staged, {}, {}, &handle, nullptr, nullptr, {},
                         &capture);
  }

  static bool ownsPreview(const StyleEngine &engine, StyleLayerHandle handle) {
    return engine.ownsPreview(handle);
  }

  static bool layered(const StyleEngine &engine) {
    return engine.layerState_.enabled;
  }

  static auto lease(const StyleEngine &engine) {
    return engine.themeLease();
  }

  static auto validateLease(const StyleEngine &engine, detail::StyleThemeLease lease) {
    return engine.validateLease(lease);
  }

  static auto admit(StyleEngine &engine, const PssDocument &document) {
    StyleLayerHandle added;

    return engine.mutate(StyleEngine::Mutation::ADMIT, {}, StyleTier::APPLICATION, "document", &document.sheet, {}, {},
                         &added, nullptr, &document);
  }

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
      result = widget.computedStyle()->flatten();
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
      result = widget.computedStyle()->flatten();
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
std::optional<TreeError> allocatorRetirementError;

void retireRawTreeDuringComputedStaging() {
  stageHookFired = true;
  auto result = allocatorCloseTree ? allocatorTree->close() : allocatorTree->remove(allocatorSubject);
  allocatorRetired = static_cast<bool>(result);
  allocatorRetirementError = result ? std::nullopt : std::optional(result.error());
}
} // namespace

TEST_CASE("computed allocation staging blocks retirement and preserves coherent widget observers") {
  for (const bool layered : {false, true}) {
    for (const bool closing : {false, true}) {
      std::size_t blockedRetirements{};
      for (std::size_t phase = 0; phase < 5; ++phase) {
        Fixture fixture;
        if (layered) {
          take(fixture.engine.enableLayers());
          take(fixture.engine.addLayer(StyleTier::USER, "user", foreground(BLUE)));
        }
        auto survivor = take(fixture.tree->emplace<Widget>(fixture.root));
        take(fixture.engine.bind(survivor, {"Widget", "survivor", {}}));
        const auto revision = fixture.engine.revision();
        const auto previous = fixture.read();
        allocatorTree = fixture.tree.get();
        allocatorSubject = fixture.subject;
        allocatorCloseTree = closing;
        allocatorRetired = false;
        allocatorRetirementError.reset();
        stageHookFired = false;
        auto result = StyleLayerTestAccess::computedStage(fixture.engine, phase, retireRawTreeDuringComputedStaging);
        allocation_test::onAllocate = nullptr;
        allocation_test::allocationsBeforeHook = 0;
        if (stageHookFired) {
          ++blockedRetirements;
          CHECK_FALSE(allocatorRetired);
          CHECK(allocatorRetirementError == TreeError::WOULD_BLOCK);
        }

        REQUIRE(result);
        CHECK(fixture.engine.revision() == revision + 1);
        CHECK(fixture.tree->inspect(fixture.subject));
        CHECK(fixture.tree->inspect(survivor));
        CHECK(fixture.read() == previous);
        for (const auto node : {fixture.subject, survivor}) {
          take(fixture.tree->read(node, [&](const Widget &widget) {
            CHECK(widget.styleRevision() == revision + 1);
            REQUIRE(widget.computedStyle());
            CHECK(widget.computedStyle()->flatten() == previous);
          }));
        }

        if (phase == 0) {
          CHECK(stageHookFired);
        }
      }
      CHECK(blockedRetirements > 0);
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

TEST_CASE("document admission enables layers atomically and retains legacy theme") {
  ManagedFixture fixture;
  take(fixture.scene->theme(foreground(RED)));

  auto document = take(parsePssDocument("#subject { bold: true; }", "application.pss"));
  auto rejected =
    fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document, StyleLayerOptions{.maxLayers = 2});

  REQUIRE_FALSE(rejected);
  CHECK(cause(rejected.error(), TreeError::INVALID_ARGUMENT));
  CHECK(fixture.read().normal.foreground == RED);
  CHECK(take(fixture.scene->styleSources()).empty());
  REQUIRE_FALSE(fixture.scene->baseLayer(StyleTier::THEME));

  auto admitted = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);

  REQUIRE(admitted);
  CHECK(fixture.read().normal.foreground == RED);
  CHECK(fixture.read().normal.bold);

  auto sources = take(fixture.scene->styleSources());

  REQUIRE(sources.size() == 3);
  REQUIRE(sources[2].document);
  CHECK(*sources[2].document == document);
  REQUIRE(fixture.scene->validateStyleLayer(*admitted));
}

TEST_CASE("first document charges all three identities and every final aggregate budget before copies") {
  SUBCASE("identity and installation order reserve three values") {
    Fixture fixture;
    take(fixture.engine.replace(foreground(RED)));

    auto document = take(parsePssDocument("* { bold: true; }"));
    const auto revision = fixture.engine.revision();

    SUBCASE("id") {
      StyleLayerTestAccess::identity(fixture.engine, std::numeric_limits<std::uint64_t>::max() - 2);
    }

    SUBCASE("order") {
      StyleLayerTestAccess::order(fixture.engine, std::numeric_limits<std::uint64_t>::max() - 2);
    }

    allocation_test::remaining = 0;

    auto rejected = StyleLayerTestAccess::admit(fixture.engine, document);

    allocation_test::remaining = -1;

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().cause == TreeError::IDENTITY_EXHAUSTED);
    CHECK(fixture.engine.revision() == revision);
    CHECK(fixture.read().normal.foreground == RED);
    REQUIRE_FALSE(fixture.engine.baseLayer(StyleTier::THEME));
  }

  SUBCASE("final budgets include legacy theme and new declarations") {
    ManagedFixture fixture;
    take(fixture.scene->theme(foreground(RED)));

    auto document = take(parsePssDocument("* { bold: true; }", "budget.pss"));
    StyleLayerOptions options;
    const auto revision = fixture.revision();

    SUBCASE("layer slots") {
      options.maxLayers = 2;
    }
    SUBCASE("rules") {
      options.maxRules = 1;
    }
    SUBCASE("declarations") {
      options.maxDeclarations = 1;
    }
    SUBCASE("owning rule bytes") {
      options.maxOwnedBytes = 1;
    }
    SUBCASE("metadata aggregate product") {
      options.maxLayers = std::numeric_limits<std::size_t>::max();
    }

    allocation_test::remaining = 0;

    auto rejected = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document, options);

    allocation_test::remaining = -1;

    REQUIRE_FALSE(rejected);
    CHECK(cause(rejected.error(), TreeError::INVALID_ARGUMENT));
    CHECK(fixture.revision() == revision);
    CHECK(fixture.read().normal.foreground == RED);
    REQUIRE_FALSE(fixture.scene->baseLayer(StyleTier::THEME));
    CHECK(take(fixture.scene->styleSources()).empty());
  }
}

TEST_CASE("document fault sweep preserves legacy lease mode pending targets styles and revision until sole commit") {
  bool succeeded{};
  std::size_t failures{};

  for (std::ptrdiff_t budget = 0; budget < 240 && !succeeded; ++budget) {
    ManagedFixture fixture;

    auto live = take(sc::live(*fixture.scene, LIVE));
    auto document = take(parsePssDocument("#future { foreground: #0000ff; }", "application.pss"));
    const auto before = fixture.read();
    const auto revision = fixture.revision();

    take(fixture.scene->id(fixture.button.node(), "future"));
    allocation_test::remaining = budget;

    auto admitted = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);

    allocation_test::remaining = -1;

    if (!admitted) {
      ++failures;
      CHECK(cause(admitted.error(), TreeError::NO_MEMORY));
      CHECK(fixture.read() == before);
      CHECK(fixture.revision() == revision);
      CHECK(take(live.get<"color">()) == RED);
      REQUIRE_FALSE(fixture.scene->baseLayer(StyleTier::THEME));
      CHECK(take(fixture.scene->styleSources()).empty());

      take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

      CHECK(fixture.read().normal.foreground == RgbColor{0, 0, 255});
      CHECK(fixture.revision() == revision + 1);
    } else {
      succeeded = true;
      CHECK(fixture.read().normal.foreground == RgbColor{0, 0, 255});
      CHECK(fixture.revision() == revision + 1);
      CHECK(take(fixture.scene->styleSources()).size() == 3);
    }

    auto stale = live.get<"color">();

    REQUIRE_FALSE(stale);
    CHECK(cause(stale.error(), TreeError::INVALID_ARGUMENT));
  }

  CHECK(succeeded);
  CHECK(failures > 0);
}

TEST_CASE("documents deep copy ownership upsert stable order and adopt only omitted established budgets") {
  ManagedFixture fixture;
  StyleLayerOptions options{.maxLayers = 4};

  take(fixture.scene->enableStyleLayers(options));

  auto first = take(parsePssDocument("* { foreground: #ff0000; }", "first.pss"));
  auto second = take(parsePssDocument("* { foreground: #0000ff; }", "second.pss"));
  auto firstHandle = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "first", first));
  auto secondHandle = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "second", second, options));

  first.sourceName = "mutated caller";
  first.sheet.rules[0].declarations.foreground = GOLD;

  auto snapshot = take(fixture.scene->styleSources());

  REQUIRE(snapshot.size() == 4);
  REQUIRE(snapshot[2].document);
  CHECK(snapshot[2].document->sourceName == "first.pss");
  CHECK(snapshot[2].document->sheet.rules[0].declarations.foreground == RgbColor{255, 0, 0});
  CHECK(fixture.read().normal.foreground == RgbColor{0, 0, 255});

  auto replacement = take(parsePssDocument("* { foreground: #00ff00; }", "replacement.pss"));
  const auto revision = fixture.revision();
  auto incompatible = fixture.scene->styleDocument(StyleTier::APPLICATION, "first", replacement, StyleLayerOptions{});

  REQUIRE_FALSE(incompatible);
  CHECK(cause(incompatible.error(), TreeError::INVALID_ARGUMENT));
  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.scene->styleSources())[2].document == snapshot[2].document);

  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "first", replacement));
  take(fixture.scene->validateStyleLayer(firstHandle));
  take(fixture.scene->validateStyleLayer(secondHandle));

  CHECK(fixture.read().normal.foreground == RgbColor{0, 0, 255});

  auto replaced = take(fixture.scene->styleSources());

  CHECK(replaced[2].name == "first");
  CHECK(replaced[3].name == "second");
  CHECK(replaced[2].document->sourceName == "replacement.pss");
}

TEST_CASE("metadata only updates and equal low level replacements never call defaults revise or redraw") {
  ManagedFixture fixture;

  auto hook = take(fixture.scene->emplace<Hook>(fixture.scene->root()));

  take(fixture.scene->style(hook, {"Hook", "hook", {}}));

  auto document = take(parsePssDocument("* { foreground: #ff0000; }", "first.pss"));
  auto layer = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));
  int calls{};
  const auto revision = fixture.revision();

  take(fixture.app->tree().read(hook, [&](const Hook &widget) {
    calls = widget.calls;
  }));
  take(fixture.app->tree().consumeFrameRequest());
  document.sourceName = "same declarations new source.pss";

  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);

  take(fixture.app->tree().read(hook, [&](const Hook &widget) {
    CHECK(widget.calls == calls);
  }));

  auto sources = take(fixture.scene->styleSources());

  REQUIRE(sources[2].document);
  CHECK(sources[2].document->sourceName == document.sourceName);
  allocation_test::remaining = 0;

  auto equalDocument = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);
  auto equalSheet = fixture.scene->replaceLayer(layer, document.sheet);

  allocation_test::remaining = -1;

  REQUIRE(equalDocument);
  REQUIRE(equalSheet);
  CHECK(*equalSheet == Invalidation::NONE);
  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK_FALSE(take(fixture.scene->styleSources())[2].document);

  take(fixture.app->tree().read(hook, [&](const Hook &widget) {
    CHECK(widget.calls == calls);
  }));
}

TEST_CASE("equal document with pending metadata performs the one real style transaction") {
  ManagedFixture fixture;

  auto document = take(parsePssDocument("#future { foreground: #0000ff; }", "application.pss"));

  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  const auto revision = fixture.revision();

  take(fixture.scene->id(fixture.button.node(), "future"));
  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  CHECK(fixture.revision() == revision + 1);
  CHECK(fixture.read().normal.foreground == RgbColor{0, 0, 255});
}

TEST_CASE("mutable document rejects malformed metadata before indexing and retains exact typed error") {
  ManagedFixture fixture;

  auto original = take(parsePssDocument("* { foreground: #ff0000; foreground: #00ff00; bold: false; }", "source.pss"));
  auto layer = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", original));
  auto document = original;
  auto prior = take(fixture.scene->styleSources());
  const auto revision = fixture.revision();
  const auto before = fixture.read();

  SUBCASE("origin kind") {
    document.originKind = static_cast<PssOriginKind>(-1);
  }
  SUBCASE("rule origins count") {
    document.origins.clear();
  }
  SUBCASE("contiguous begin") {
    document.origins[0].declarationBegin = std::numeric_limits<std::size_t>::max();
  }
  SUBCASE("overflow count") {
    document.origins[0].declarationCount = std::numeric_limits<std::size_t>::max();
  }
  SUBCASE("escaped effective index") {
    document.origins[0].effectiveDeclaration[0] = std::numeric_limits<std::size_t>::max() - 1;
  }
  SUBCASE("wrong rule index") {
    document.declarations[0].ruleIndex = std::numeric_limits<std::size_t>::max();
  }
  SUBCASE("negative property") {
    document.declarations[0].property = static_cast<PssProperty>(-1);
  }
  SUBCASE("large property") {
    document.declarations[0].property = static_cast<PssProperty>(PSS_PROPERTY_COUNT);
  }
  SUBCASE("zero coordinate") {
    document.declarations[0].location.line = 0;
  }
  SUBCASE("unbounded coordinate") {
    document.origins[0].selector.offset = std::numeric_limits<std::size_t>::max();
  }
  SUBCASE("unordered coordinates") {
    document.declarations[1].location = document.origins[0].selector;
  }
  SUBCASE("last wins link") {
    document.origins[0].effectiveDeclaration[0] = 0;
  }
  SUBCASE("missing effective link") {
    document.origins[0].effectiveDeclaration[2] = NO_PSS_DECLARATION;
  }
  SUBCASE("inactive scalar width") {
    document.declarations[0].value.width = 1;
  }
  SUBCASE("inactive scalar color") {
    document.declarations[2].value.color = RED;
  }
  SUBCASE("inactive scalar boolean") {
    document.declarations[0].value.boolean = true;
  }
  SUBCASE("final declarations mismatch") {
    document.sheet.rules[0].declarations.bold = true;
  }
  SUBCASE("invalid selector state") {
    document.sheet.rules[0].selector.state = static_cast<StyleState>(-1);
  }
  SUBCASE("unclaimed occurrence") {
    --document.origins[0].declarationCount;
    document.origins[0].effectiveDeclaration[2] = NO_PSS_DECLARATION;
  }
  SUBCASE("source NUL") {
    document.sourceName = std::string("bad\0name", 8);
  }
  SUBCASE("source UTF8") {
    document.sourceName = std::string(1, '\xFF');
  }
  SUBCASE("source byte budget") {
    document.sourceName.assign(MAX_PSS_SOURCE_NAME_BYTES + 1, 'a');
  }
  SUBCASE("logical document budget") {
    document.sheet.rules[0].selector.type.assign(MAX_PSS_DOCUMENT_BYTES, 'a');
  }

  auto rejected = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);

  REQUIRE_FALSE(rejected);
  CHECK(cause(rejected.error(), TreeError::INVALID_ARGUMENT));
  CHECK(rejected.error().phase == ManagedPhase::STYLE);
  REQUIRE(rejected.error().pss);
  CHECK(rejected.error().pss->code != PssErrorCode::NONE);
  CHECK(fixture.read() == before);
  CHECK(fixture.revision() == revision);
  REQUIRE(fixture.scene->validateStyleLayer(layer));
  CHECK(take(fixture.scene->styleSources())[2].document == prior[2].document);

  if (document.sourceName.size() <= MAX_PSS_SOURCE_NAME_BYTES) {
    REQUIRE(rejected.error().styleSource);
    CHECK(*rejected.error().styleSource == document.sourceName);
  }
}

TEST_CASE("synthetic typed document ordinals are admitted separately from byte positions") {
  ManagedFixture fixture;
  constexpr auto COMPACT =
    sc::compactSheet(std::array{sc::flatRule(sc::selector(), sc::foreground(sc::rgb(1, 2, 3)))}, "typed");

  auto document = take(sc::materializeDocument(COMPACT));

  REQUIRE(document.originKind == PssOriginKind::SYNTHETIC_TYPED);
  REQUIRE(fixture.scene->styleDocument(StyleTier::USER, "typed", document));
  document.declarations[0].location.offset = 100;

  auto rejected = fixture.scene->styleDocument(StyleTier::USER, "typed", document);

  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().pss);
  CHECK(rejected.error().pss->code == PssErrorCode::INVALID_VALUE);
}

TEST_CASE("low level batch clear failure removal and cleanup follow actual layer source lifetime") {
  ManagedFixture fixture;

  auto application = take(parsePssDocument("* { bold: true; }", "application.pss"));
  auto user = take(parsePssDocument("* { foreground: #0000ff; }", "user.pss"));
  auto appLayer = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", application));
  auto userLayer = take(fixture.scene->styleDocument(StyleTier::USER, "user", user));
  auto snapshot = take(fixture.scene->styleSources());
  std::weak_ptr<const PssDocument> appDocument = snapshot[2].document;
  std::weak_ptr<const PssDocument> userDocument = snapshot[3].document;
  auto invalid = user.sheet;

  invalid.rules[0].declarations.minWidth = -1;

  const std::array invalidBatch{StyleLayerUpdate{appLayer, application.sheet}, StyleLayerUpdate{userLayer, invalid}};

  REQUIRE_FALSE(fixture.scene->updateLayers(invalidBatch));
  CHECK(take(fixture.scene->styleSources())[2].document == snapshot[2].document);
  CHECK(take(fixture.scene->styleSources())[3].document == snapshot[3].document);

  const std::array clearBatch{StyleLayerUpdate{appLayer, application.sheet}, StyleLayerUpdate{userLayer, std::nullopt}};

  take(fixture.scene->updateLayers(clearBatch));

  auto cleared = take(fixture.scene->styleSources());

  REQUIRE(cleared.size() == 3);
  CHECK_FALSE(cleared[2].document);
  REQUIRE_FALSE(fixture.scene->validateStyleLayer(userLayer));
  CHECK_FALSE(appDocument.expired());
  CHECK_FALSE(userDocument.expired());
  snapshot.clear();
  CHECK(appDocument.expired());
  CHECK(userDocument.expired());

  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", application));

  auto retained = take(fixture.scene->styleSources());
  std::weak_ptr<const PssDocument> retainedDocument = retained[2].document;

  take(fixture.scene->close());

  REQUIRE_FALSE(fixture.scene->styleSources());
  CHECK_FALSE(retainedDocument.expired());
  retained.clear();
  CHECK(retainedDocument.expired());
}

TEST_CASE("theme handoff and live apply retain unrelated application and user source rows") {
  ManagedFixture fixture;

  auto document = take(parsePssDocument("* { bold: true; }", "application.pss"));

  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  auto snapshot = take(fixture.scene->styleSources());

  take(fixture.scene->theme(foreground(GREEN)));

  CHECK(take(fixture.scene->styleSources())[2].document == snapshot[2].document);

  auto live = take(sc::live(*fixture.scene, LIVE));

  take(live.set<"color">(BLUE));

  CHECK(take(fixture.scene->styleSources())[2].document == snapshot[2].document);
  CHECK(fixture.read().normal.bold);
}

TEST_CASE("metadata allocation failures preserve document identity values revision and pending invalidation") {
  bool succeeded{};
  std::size_t failures{};

  for (std::ptrdiff_t budget = 0; budget < 120 && !succeeded; ++budget) {
    ManagedFixture fixture;

    auto document = take(parsePssDocument("* { bold: true; }", "old source.pss"));

    take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

    auto prior = take(fixture.scene->styleSources());
    const auto revision = fixture.revision();
    const auto before = fixture.read();

    take(fixture.app->tree().consumeFrameRequest());
    document.sourceName = "new source label with identical declarations.pss";
    allocation_test::remaining = budget;

    auto changed = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);

    allocation_test::remaining = -1;

    CHECK(fixture.read() == before);
    CHECK(fixture.revision() == revision);
    CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);

    if (!changed) {
      ++failures;
      CHECK(cause(changed.error(), TreeError::NO_MEMORY));
      REQUIRE(changed.error().pss);
      CHECK(changed.error().pss->code == PssErrorCode::ALLOCATION);
      CHECK(take(fixture.scene->styleSources())[2].document == prior[2].document);
    } else {
      succeeded = true;
      CHECK(take(fixture.scene->styleSources())[2].document->sourceName == document.sourceName);
    }
  }

  CHECK(succeeded);
  CHECK(failures > 0);
}

TEST_CASE("source snapshot allocation sweep catches every owning transfer and preserves installed documents") {
  bool succeeded{};
  std::size_t failures{};

  for (std::ptrdiff_t budget = 0; budget < 120 && !succeeded; ++budget) {
    ManagedFixture fixture;

    auto document = take(parsePssDocument("* { bold: true; }", "source.pss"));
    auto layer = take(fixture.scene->styleDocument(
      StyleTier::APPLICATION, "long application layer name requiring owned string storage", document));
    auto prior = take(fixture.scene->styleSources());

    allocation_test::remaining = budget;

    auto snapshot = fixture.scene->styleSources();

    allocation_test::remaining = -1;

    if (!snapshot) {
      ++failures;
      CHECK(cause(snapshot.error(), TreeError::NO_MEMORY));
    } else {
      succeeded = true;
      REQUIRE(snapshot->size() == 3);
      CHECK((*snapshot)[2].document == prior[2].document);
    }

    REQUIRE(fixture.scene->validateStyleLayer(layer));
    CHECK(take(fixture.scene->styleSources())[2].document == prior[2].document);
  }

  CHECK(succeeded);
  CHECK(failures > 0);
}

TEST_CASE("document staging and snapshot allocation reject scene mutation and cleanup reentry") {
  for (const bool snapshotting : {false, true}) {
    ManagedFixture fixture;

    auto document = take(parsePssDocument("* { bold: true; }", "source.pss"));
    auto layer = take(fixture.scene->styleDocument(
      StyleTier::APPLICATION, "long application layer name requiring owned string storage", document));
    const auto nested = foreground(BLUE);

    allocatorScene = fixture.scene.get();
    allocatorSheet = &nested;
    allocatorLayer = layer;
    allocatorSubject = fixture.button.node();
    stageHookFired = false;
    document.sourceName = "new owning source requiring a document copy.pss";
    allocation_test::allocationsBeforeHook = 0;
    allocation_test::onAllocate = mutateSceneDuringMetadataStaging;

    if (snapshotting) {
      auto snapshot = fixture.scene->styleSources();

      allocation_test::onAllocate = nullptr;
      REQUIRE(snapshot);
    } else {
      auto updated = fixture.scene->styleDocument(
        StyleTier::APPLICATION, "long application layer name requiring owned string storage", document);

      allocation_test::onAllocate = nullptr;
      REQUIRE(updated);
    }

    REQUIRE(stageHookFired);
    CHECK(stageNestedBlocked);
    REQUIRE(fixture.scene->validateStyleLayer(layer));
    REQUIRE(fixture.scene->close());
  }
}

TEST_CASE("style source admission snapshots and handle validation enforce owner frame foreign and stale boundaries") {
  ManagedFixture fixture;
  ManagedFixture other;

  auto document = take(parsePssDocument("* { bold: true; }", "source.pss"));
  auto layer = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  REQUIRE_FALSE(fixture.scene->validateStyleLayer({}));
  REQUIRE_FALSE(other.scene->validateStyleLayer(layer));

  bool wrongAdmission{};
  bool wrongSnapshot{};
  bool wrongValidation{};
  std::thread worker([&] {
    auto admitted = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);
    auto snapshot = fixture.scene->styleSources();
    auto validated = fixture.scene->validateStyleLayer(layer);

    wrongAdmission = !admitted && cause(admitted.error(), TreeError::WRONG_THREAD);
    wrongSnapshot = !snapshot && cause(snapshot.error(), TreeError::WRONG_THREAD);
    wrongValidation = !validated && cause(validated.error(), TreeError::WRONG_THREAD);
  });

  worker.join();

  CHECK(wrongAdmission);
  CHECK(wrongSnapshot);
  CHECK(wrongValidation);

  take(fixture.app->tree().frame([&] {
    auto admitted = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);
    auto snapshot = fixture.scene->styleSources();
    auto validated = fixture.scene->validateStyleLayer(layer);

    REQUIRE_FALSE(admitted);
    REQUIRE_FALSE(snapshot);
    REQUIRE_FALSE(validated);
    CHECK(cause(admitted.error(), TreeError::WOULD_BLOCK));
    CHECK(cause(snapshot.error(), TreeError::WOULD_BLOCK));
    CHECK(cause(validated.error(), TreeError::WOULD_BLOCK));
  }));

  take(fixture.scene->removeLayer(layer));

  REQUIRE_FALSE(fixture.scene->validateStyleLayer(layer));

  take(fixture.scene->addLayer(StyleTier::APPLICATION, "application", document.sheet));

  REQUIRE_FALSE(fixture.scene->validateStyleLayer(layer));
  take(fixture.scene->close());

  auto closed = fixture.scene->validateStyleLayer(layer);

  REQUIRE_FALSE(closed);
  CHECK(cause(closed.error(), TreeError::CLOSED));
}

TEST_CASE("unequal document upsert fault sweep preserves every source and pending registration") {
  bool succeeded{};
  std::size_t failures{};

  for (std::ptrdiff_t budget = 0; budget < 240 && !succeeded; ++budget) {
    ManagedFixture fixture;

    auto application = take(parsePssDocument("#subject { foreground: #ff0000; }", "old application.pss"));
    auto user = take(parsePssDocument("* { bold: true; }", "user.pss"));
    auto replacement = take(parsePssDocument("#future { foreground: #0000ff; }", "new application.pss"));
    auto layer = take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", application));

    take(fixture.scene->styleDocument(StyleTier::USER, "user", user));

    auto prior = take(fixture.scene->styleSources());
    const auto before = fixture.read();
    const auto revision = fixture.revision();

    take(fixture.scene->id(fixture.button.node(), "future"));
    allocation_test::remaining = budget;

    auto updated = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", replacement);

    allocation_test::remaining = -1;

    if (!updated) {
      ++failures;
      CHECK(cause(updated.error(), TreeError::NO_MEMORY));
      CHECK(fixture.read() == before);
      CHECK(fixture.revision() == revision);
      CHECK(take(fixture.scene->styleSources())[2].document == prior[2].document);
      CHECK(take(fixture.scene->styleSources())[3].document == prior[3].document);

      take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", replacement));
    } else {
      succeeded = true;
    }

    take(fixture.scene->validateStyleLayer(layer));

    CHECK(fixture.read().normal.foreground == RgbColor{0, 0, 255});
    CHECK(fixture.read().normal.bold);
    CHECK(fixture.revision() == revision + 1);
    CHECK(take(fixture.scene->styleSources())[3].document == prior[3].document);
  }

  CHECK(succeeded);
  CHECK(failures > 0);
}

TEST_CASE("throwing widget defaults leaves first document legacy mode authority and source rows intact") {
  ManagedFixture fixture;

  auto hook = take(fixture.scene->emplace<Hook>(fixture.scene->root()));

  take(fixture.scene->style(hook, {"Hook", "hook", {}}));

  auto live = take(sc::live(*fixture.scene, LIVE));
  auto document = take(parsePssDocument("* { bold: true; }", "application.pss"));
  const auto before = fixture.read();
  const auto revision = fixture.revision();

  take(fixture.app->tree().edit(hook, [&](Hook &widget) {
    widget.onDefaults = [] {
      throw std::runtime_error("defaults failure");
    };
  }));

  auto rejected = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);

  REQUIRE_FALSE(rejected);
  CHECK(cause(rejected.error(), TreeError::CALLBACK_FAILED));
  CHECK(fixture.read() == before);
  CHECK(fixture.revision() == revision);
  CHECK(take(fixture.scene->styleSources()).empty());
  CHECK(take(live.get<"color">()) == RED);
  REQUIRE_FALSE(fixture.scene->baseLayer(StyleTier::THEME));

  take(fixture.app->tree().edit(hook, [](Hook &widget) {
    widget.onDefaults = {};
  }));
  take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  CHECK(fixture.revision() == revision + 1);
  CHECK(take(fixture.scene->styleSources()).size() == 3);
}

TEST_CASE("byte provenance accepts BOM multiline UTF8 comments duplicates and explicit scalar false") {
  ManagedFixture fixture;

  auto document = take(
    parsePssDocument("\xef\xbb\xbf/* \xc3\xa9 */\n* {\n bold: true; bold: false;\n min-width: 0;\n}\n", "source.pss"));

  REQUIRE(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

  auto snapshot = take(fixture.scene->styleSources());

  REQUIRE(snapshot[2].document);
  CHECK(*snapshot[2].document == document);
}

TEST_CASE("one shot allocation failure never retries source ownership after original NO_MEMORY") {
  ManagedFixture fixture;

  auto document = take(parsePssDocument("* { bold: true; }", "owning source identity requires allocated storage.pss"));
  bool snapshotting{};

  SUBCASE("admitted document reaches engine ownership failure after pending targets were flushed") {
    snapshotting = false;
  }

  SUBCASE("source snapshot returns original allocation failure without a second allocation") {
    take(fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document));

    snapshotting = true;
  }

  allocation_test::rejectedAllocations = 0;
  allocation_test::allocationsAfterFailure = 0;
  allocation_test::failureObserved = false;
  allocation_test::recoverAfterFailure = true;
  allocation_test::remaining = 0;

  std::optional<ManagedError> failure;

  if (snapshotting) {
    auto result = fixture.scene->styleSources();

    if (!result) {
      failure = result.error();
    }
  } else {
    auto result = fixture.scene->styleDocument(StyleTier::APPLICATION, "application", document);

    if (!result) {
      failure = result.error();
    }
  }

  const auto rejectedAllocations = allocation_test::rejectedAllocations;
  const auto allocationsAfterFailure = allocation_test::allocationsAfterFailure;

  allocation_test::remaining = -1;
  allocation_test::recoverAfterFailure = false;
  allocation_test::failureObserved = false;

  REQUIRE(failure);
  CHECK(cause(*failure, TreeError::NO_MEMORY));
  CHECK(rejectedAllocations == 1);
  CHECK(allocationsAfterFailure == 0);
  CHECK_FALSE(failure->styleSource);

  if (!snapshotting) {
    REQUIRE(failure->pss);
    CHECK(failure->pss->code == PssErrorCode::ALLOCATION);
    CHECK(failure->pss->cause == TreeError::NO_MEMORY);
    CHECK(take(fixture.scene->styleSources()).empty());
  }
}

TEST_CASE("private theme preview masks the base and charges both resident contributions") {
  Fixture fixture;
  auto baseSheet = foreground(BLUE);

  baseSheet.rules[0].declarations.minWidth = 19;
  take(fixture.engine.theme(baseSheet));

  const auto lease = StyleLayerTestAccess::lease(fixture.engine);
  const auto before = fixture.engine.revision();
  StyleLayerHandle preview;
  StyleLayerOptions limited;

  limited.maxLayers = 2;

  auto rejected = StyleLayerTestAccess::preview(fixture.engine, foreground(RED), preview, limited);

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().cause == TreeError::INVALID_ARGUMENT);
  CHECK(fixture.engine.revision() == before);
  CHECK(StyleLayerTestAccess::validateLease(fixture.engine, lease));
  CHECK_FALSE(StyleLayerTestAccess::layered(fixture.engine));

  CHECK(take(StyleLayerTestAccess::preview(fixture.engine, foreground(RED), preview)) == Invalidation::LAYOUT);
  CHECK(fixture.read().normal.foreground == RED);
  CHECK(fixture.read().minWidth != 19);
  CHECK_FALSE(StyleLayerTestAccess::validateLease(fixture.engine, lease));
  CHECK(StyleLayerTestAccess::ownsPreview(fixture.engine, preview));

  StyleLayerHandle other;
  auto busy = StyleLayerTestAccess::preview(fixture.engine, foreground(GREEN), other);

  REQUIRE_FALSE(busy);
  CHECK(busy.error().cause == TreeError::WOULD_BLOCK);

  const auto permanent = take(fixture.engine.baseLayer(StyleTier::THEME));

  take(StyleLayerTestAccess::finishPreview(fixture.engine, preview, false));
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK(fixture.read().minWidth == 19);
  CHECK(StyleLayerTestAccess::layered(fixture.engine));
  CHECK_FALSE(StyleLayerTestAccess::validateLease(fixture.engine, lease));
  CHECK(take(fixture.engine.removeLayer(permanent)) == Invalidation::LAYOUT);
}

TEST_CASE("preview promotion and cancellation preserve later application and user layers") {
  Fixture fixture;

  take(fixture.engine.theme(foreground(BLUE)));

  StyleLayerHandle preview;

  CHECK(take(StyleLayerTestAccess::preview(fixture.engine, foreground(RED), preview)) == Invalidation::PAINT);

  const auto application = take(fixture.engine.addLayer(StyleTier::APPLICATION, "application", foreground(GREEN)));
  const auto user = take(fixture.engine.addLayer(StyleTier::USER, "user", foreground(GOLD)));

  CHECK(take(StyleLayerTestAccess::finishPreview(fixture.engine, preview, true)) == Invalidation::NONE);
  CHECK(fixture.read().normal.foreground == GOLD);
  take(fixture.engine.removeLayer(user));
  CHECK(fixture.read().normal.foreground == GREEN);
  take(fixture.engine.removeLayer(application));
  CHECK(fixture.read().normal.foreground == RED);

  StyleLayerHandle next;

  take(StyleLayerTestAccess::preview(fixture.engine, foreground(BLUE), next));
  take(StyleLayerTestAccess::finishPreview(fixture.engine, next, false));
  CHECK(fixture.read().normal.foreground == RED);
}

TEST_CASE("equal external theme takeover and permanent theme removal retire the private preview") {
  for (const int operation : {0, 1, 2}) {
    Fixture fixture;

    take(fixture.engine.theme(foreground(BLUE)));

    StyleLayerHandle preview;

    take(StyleLayerTestAccess::preview(fixture.engine, foreground(RED), preview));

    auto invalid = foreground(GREEN);

    invalid.rules[0].declarations.minWidth = -1;
    CHECK_FALSE(fixture.engine.theme(invalid));
    CHECK(StyleLayerTestAccess::ownsPreview(fixture.engine, preview));

    if (operation == 0) {
      take(fixture.engine.theme(foreground(RED)));
      CHECK(fixture.read().normal.foreground == RED);
    } else if (operation == 1) {
      take(fixture.engine.removeLayer(take(fixture.engine.baseLayer(StyleTier::THEME))));
      CHECK(fixture.read().normal.foreground != RED);
    } else {
      take(fixture.engine.replace(foreground(GREEN)));
      CHECK(fixture.read().normal.foreground == GREEN);
    }

    CHECK_FALSE(StyleLayerTestAccess::ownsPreview(fixture.engine, preview));

    const auto cancelled = StyleLayerTestAccess::finishPreview(fixture.engine, preview, false);

    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().cause == TreeError::INVALID_ARGUMENT);
  }
}

TEST_CASE("legacy equal mutation during owning preview staging invalidates the revision capture") {
  Fixture fixture;

  take(fixture.engine.theme(foreground(BLUE)));

  const auto candidate = foreground(RED);
  static StyleEngine *entered;
  static const StyleSheet *external;
  const auto equal = foreground(BLUE);

  entered = &fixture.engine;
  external = &equal;
  allocation_test::onAllocate = [] {
    take(entered->theme(*external));
  };
  allocation_test::allocationsBeforeHook = 0;

  StyleLayerHandle preview;
  const auto result = StyleLayerTestAccess::previewAfterCopy(fixture.engine, candidate, preview);

  allocation_test::onAllocate = nullptr;
  REQUIRE_FALSE(result);
  CHECK(result.error().cause == TreeError::INVALID_ARGUMENT);
  CHECK(fixture.read().normal.foreground == BLUE);
  CHECK_FALSE(StyleLayerTestAccess::layered(fixture.engine));
}

TEST_CASE("frame part attributes share layered admission and preserve computed state after rejection") {
  Fixture fixture;
  take(fixture.engine.enableLayers({.maxDeclarations = 4}));
  auto sheet = foreground(RED);
  sheet.rules[0].declarations.frameLine.foreground = GREEN;
  sheet.rules[0].declarations.frameTitle.foreground = BLUE;
  sheet.rules[0].declarations.frameEnds.bold = false;
  const auto layer = take(fixture.engine.addLayer(StyleTier::APPLICATION, "frame", sheet));
  const auto before = fixture.read();
  REQUIRE(before.frame[0].line);
  REQUIRE(before.frame[0].title);
  REQUIRE(before.frame[0].ends);
  CHECK(before.frame[0].line->foreground == GREEN);
  CHECK(before.frame[0].title->foreground == BLUE);
  CHECK_FALSE(before.frame[0].ends->bold);
  auto extra = foreground(GOLD);
  CHECK_FALSE(fixture.engine.addLayer(StyleTier::USER, "exceeds", extra));
  CHECK(fixture.read() == before);
  take(fixture.engine.removeLayer(layer));
  CHECK_FALSE(fixture.read().frame[0].title);
}

TEST_CASE("ordinary styles preserve default parts and match state-specific frame resolution without allocation") {
  const auto ordinary = take(parsePss("*{foreground:#123456;bold:true;min-width:7;}"
                                      "*:focused{italic:false;}*:disabled{underline:false;}"
                                      "*:selected{background:#456789;}*:header{bold:false;}"
                                      "*:caret{foreground:#abcdef;}"));
  const StyleTarget target{.type = "Widget", .id = "subject"};

  for (const bool suppliedParts : {false, true}) {
    ComputedStyle defaults;
    defaults.normal.italic = true;
    defaults.disabled.underline = true;
    defaults.frame[5].ends = CellStyle{};

    if (suppliedParts) {
      for (auto &parts : defaults.frame) {
        parts.line = CellStyle{};
        parts.line->foreground = RED;
        parts.line->bold = true;
        parts.title = CellStyle{};
        parts.title->background = GREEN;
        parts.title->italic = true;
        parts.ends = CellStyle{};
        parts.ends->foreground = BLUE;
        parts.ends->underline = true;
      }
    }

    auto withPart = ordinary;
    StyleRule caret;
    caret.selector.state = StyleState::CARET;
    caret.declarations.frameEnds.bold = defaults.frame[5].ends->bold;
    withPart.rules.push_back(caret);
    allocation_test::remaining = 0;
    const auto common = resolveStyle(ordinary, target, defaults);
    const auto resolved = resolveStyle(withPart, target, defaults);
    allocation_test::remaining = -1;
    CHECK(common == resolved);
    CHECK(common.frame == defaults.frame);
    CHECK(common.minWidth == 7);
    CHECK_FALSE(common.header.bold);
    CHECK_FALSE(common.focused.italic);
    CHECK_FALSE(common.disabled.underline);
    auto irrelevant = ordinary;
    caret.selector.type = "Other";
    irrelevant.rules.push_back(caret);
    CHECK(resolveStyle(irrelevant, target, defaults) == common);
  }
}

TEST_CASE("focused-only frame declarations keep state scope and independent layered field ranks") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  take(fixture.engine.theme(foreground(RED)));
  StyleSheet focused;
  StyleRule title;
  title.selector.state = StyleState::FOCUSED;
  title.declarations.frameTitle.bold = false;
  focused.rules.push_back(title);
  const auto focusedLayer = take(fixture.engine.addLayer(StyleTier::APPLICATION, "focused", focused));
  const auto before = fixture.read();
  REQUIRE(before.frame[1].title);
  CHECK_FALSE(before.frame[1].title->bold);
  CHECK(before.frame[1].title->foreground == RED);

  for (const std::size_t state : {0u, 2u, 3u, 4u, 5u}) {
    CHECK_FALSE(before.frame[state].title);
  }

  auto scoped = focused;
  scoped.scope = fixture.root.id();
  scoped.rules[0].declarations.frameTitle.foreground = GOLD;
  take(fixture.engine.addLayer(StyleTier::USER, "other-scope", scoped));
  CHECK(fixture.read() == before);
  StyleSheet application;
  title.selector.state = StyleState::ANY;
  title.selector.id = "subject";
  title.declarations.frameTitle.foreground = BLUE;
  title.declarations.frameTitle.bold = true;
  title.declarations.frameEnds.underline = false;
  application.rules.push_back(title);
  take(fixture.engine.addLayer(StyleTier::APPLICATION, "specific", application));
  const auto specific = fixture.read();
  REQUIRE(specific.frame[1].title);
  CHECK(specific.frame[1].title->foreground == BLUE);
  CHECK(specific.frame[1].title->bold);
  focused.rules[0].declarations.frameTitle.bold = false;
  take(fixture.engine.addLayer(StyleTier::USER, "user", focused));
  const auto user = fixture.read();
  REQUIRE(user.frame[1].title);
  CHECK(user.frame[1].title->foreground == BLUE);
  CHECK_FALSE(user.frame[1].title->bold);
  REQUIRE(user.frame[0].ends);
  CHECK_FALSE(user.frame[0].ends->underline);
  take(fixture.engine.removeLayer(focusedLayer));
  CHECK(fixture.read().frame[1].title == user.frame[1].title);
}

TEST_CASE("scrollbar parts use independent cascade ranks and preserve unused fast path defaults") {
  const auto source = take(parsePss("*{foreground:#112233;bold:true;}"
                                    "Widget{scrollbar-track-background:#445566;scrollbar-thumb-bold:false;}"
                                    "#subject:focused{scrollbar-ends-italic:false;scrollbar-corner-underline:true;}"
                                    "Widget:disabled{scrollbar-thumb-foreground:#abcdef;}"));
  const StyleTarget target{.type = "Widget", .id = "subject"};
  ComputedStyle defaults;
  defaults.normal.italic = true;
  auto resolved = resolveStyle(source, target, defaults);
  REQUIRE(resolved.scrollbar[0].track);
  REQUIRE(resolved.scrollbar[0].thumb);
  CHECK(resolved.scrollbar[0].track->foreground == RgbColor{17, 34, 51});
  CHECK(resolved.scrollbar[0].track->background == RgbColor{68, 85, 102});
  CHECK(resolved.scrollbar[0].track->italic);
  CHECK_FALSE(resolved.scrollbar[0].thumb->bold);
  CHECK_FALSE(resolved.scrollbar[0].ends);
  REQUIRE(resolved.scrollbar[1].ends);
  REQUIRE(resolved.scrollbar[1].corner);
  CHECK_FALSE(resolved.scrollbar[1].ends->italic);
  CHECK(resolved.scrollbar[1].corner->underline);
  CHECK(resolved.scrollbar[2].thumb->foreground == RgbColor{171, 205, 239});
  const auto ordinary = take(parsePss("*{foreground:#112233;bold:true;min-width:5;}"));
  for (auto &part : defaults.scrollbar) {
    part.track = CellStyle{};
    part.track->background = RED;
    part.thumb = CellStyle{};
    part.thumb->bold = true;
    part.ends = CellStyle{};
    part.ends->italic = false;
    part.corner = CellStyle{};
    part.corner->underline = true;
  }
  auto forced = ordinary;
  StyleRule marker;
  marker.selector.state = StyleState::CARET;
  marker.declarations.scrollbarCorner.underline = true;
  forced.rules.push_back(marker);
  allocation_test::remaining = 0;
  const auto common = resolveStyle(ordinary, target, defaults);
  const auto full = resolveStyle(forced, target, defaults);
  allocation_test::remaining = -1;
  CHECK(common == full);
  CHECK(common.scrollbar == defaults.scrollbar);
  CHECK(common.minWidth == 5);
}

TEST_CASE("scrollbar declarations count against existing layer budgets") {
  StyleLayerOptions options;
  options.maxDeclarations = 1;
  Fixture fixture;
  take(fixture.engine.enableLayers(options));
  const auto two = take(parsePss("*{scrollbar-track-foreground:#112233;scrollbar-thumb-bold:false;}"));
  const auto revision = fixture.engine.revision();
  CHECK_FALSE(fixture.engine.addLayer(StyleTier::APPLICATION, "too-many", two));
  CHECK(fixture.engine.revision() == revision);
}

TEST_CASE("frame and scrollbar parts coexist with independent ranks and ordinary layer removal") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  const auto base = take(parsePss("*{bold:true;frame-title-foreground:#010203;"
                                  "scrollbar-track-background:#040506;scrollbar-thumb-bold:true;}"));
  take(fixture.engine.defaults(base));
  const auto application = take(parsePss("#subject:focused{scrollbar-thumb-foreground:#070809;}"));
  take(fixture.engine.addLayer(StyleTier::APPLICATION, "specific", application));
  const auto user = take(parsePss("*:focused{scrollbar-thumb-bold:false;frame-title-foreground:#0a0b0c;}"));
  const auto layer = take(fixture.engine.addLayer(StyleTier::USER, "user", user));
  const auto result = fixture.read();

  REQUIRE(result.frame[1].title);
  REQUIRE(result.scrollbar[1].track);
  REQUIRE(result.scrollbar[1].thumb);
  CHECK(result.frame[1].title->foreground == RgbColor{10, 11, 12});
  CHECK(result.scrollbar[1].track->background == RgbColor{4, 5, 6});
  CHECK(result.scrollbar[1].thumb->foreground == RgbColor{7, 8, 9});
  CHECK_FALSE(result.scrollbar[1].thumb->bold);
  CHECK(result.scrollbar[0].thumb->bold);
  CHECK(result.frame[0].title->foreground == RgbColor{1, 2, 3});
  take(fixture.engine.removeLayer(layer));
  CHECK(fixture.read().scrollbar[1].thumb->bold);
  CHECK(fixture.read().frame[1].title->foreground == RgbColor{1, 2, 3});
}

TEST_CASE("theme scrollbar focus colors retain independent application overrides") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  take(fixture.engine.defaults(nordTheme()));
  const auto before = fixture.read();

  REQUIRE(before.scrollbar[1].thumb);
  CHECK(before.scrollbar[1].thumb->foreground == RgbColor{0x88, 0xc0, 0xd0});
  CHECK(before.scrollbar[1].thumb->background == RgbColor{0x2e, 0x34, 0x40});
  const auto application = take(parsePss("#subject:focused{scrollbar-thumb-foreground:#010203;"
                                         "scrollbar-track-background:#040506;scrollbar-ends-bold:false;}"));
  const auto layer = take(fixture.engine.addLayer(StyleTier::APPLICATION, "parts", application));
  const auto changed = fixture.read();

  REQUIRE(changed.scrollbar[1].track);
  REQUIRE(changed.scrollbar[1].thumb);
  REQUIRE(changed.scrollbar[1].ends);
  CHECK(changed.scrollbar[1].thumb->foreground == RgbColor{1, 2, 3});
  CHECK(changed.scrollbar[1].thumb->background == before.scrollbar[1].thumb->background);
  CHECK(changed.scrollbar[1].track->background == RgbColor{4, 5, 6});
  CHECK_FALSE(changed.scrollbar[1].ends->bold);
  CHECK(changed.normal == before.normal);
  CHECK(changed.disabled == before.disabled);
  take(fixture.engine.removeLayer(layer));
  CHECK(fixture.read() == before);
}

TEST_CASE("thumb segment styles inherit common state attributes through layers and retain explicit false") {
  Fixture fixture;
  take(fixture.engine.enableLayers());
  auto theme = take(parsePss(R"pss(* {
    scrollbar-thumb-foreground:#010203; scrollbar-thumb-background:#040506;
    scrollbar-thumb-bold:true; scrollbar-thumb-italic:true; scrollbar-thumb-underline:true;
    scrollbar-thumb-start-foreground:#070809; scrollbar-thumb-body-italic:false;
  } *:focused { scrollbar-thumb-background:#101112; }
    *:disabled { scrollbar-thumb-foreground:#202122; }
    *:selected { scrollbar-thumb-end-underline:false; }
    *:header { scrollbar-thumb-single-bold:false; }
    *:caret { scrollbar-thumb-body-background:#303132; })pss"));
  take(fixture.engine.theme(theme));
  auto application = take(parsePss("*{scrollbar-thumb-start-bold:false;scrollbar-thumb-end-foreground:#404142;}"));
  take(fixture.engine.addLayer(StyleTier::APPLICATION, "segments", application));
  const auto userLayer = take(fixture.engine.addLayer(
    StyleTier::USER, "user",
    take(parsePss("*{scrollbar-thumb-foreground:#505152;scrollbar-thumb-single-underline:false;}"))));
  const auto resolved = fixture.read();
  REQUIRE(resolved.scrollbarThumbSegments);
  const auto &states = *resolved.scrollbarThumbSegments;

  for (const auto &parts : states) {
    CHECK(parts.start.foreground == RgbColor{7, 8, 9});
    CHECK_FALSE(parts.start.bold);
    CHECK(parts.start.italic);
    CHECK(parts.start.underline);
    CHECK(parts.body.foreground == RgbColor{80, 81, 82});
    CHECK(parts.body.bold);
    CHECK_FALSE(parts.body.italic);
    CHECK(parts.end.foreground == RgbColor{64, 65, 66});
    CHECK(parts.single.foreground == RgbColor{80, 81, 82});
    CHECK_FALSE(parts.single.underline);
  }

  CHECK(states[0].start.background == RgbColor{4, 5, 6});
  CHECK(states[1].start.background == RgbColor{16, 17, 18});
  CHECK(states[2].body.foreground == RgbColor{80, 81, 82});
  CHECK_FALSE(states[3].end.underline);
  CHECK_FALSE(states[4].single.bold);
  CHECK(states[5].body.background == RgbColor{48, 49, 50});
  CHECK(states[5].start.background == RgbColor{4, 5, 6});
  CHECK(take(fixture.engine.refresh()) == Invalidation::NONE);

  const auto revision = fixture.engine.revision();
  auto invalid = application;
  invalid.rules[0].declarations.minWidth = -1;
  CHECK_FALSE(fixture.engine.replaceLayer(userLayer, invalid));
  CHECK(fixture.engine.revision() == revision);
  CHECK(fixture.read() == resolved);

  theme.rules[0].declarations.scrollbarThumb.background = RgbColor{90, 91, 92};
  take(fixture.engine.theme(theme));
  const auto changed = fixture.read();
  REQUIRE(changed.scrollbarThumbSegments);
  CHECK((*changed.scrollbarThumbSegments)[0].start.background == RgbColor{90, 91, 92});
  CHECK_FALSE((*changed.scrollbarThumbSegments)[0].start.bold);
  CHECK((*changed.scrollbarThumbSegments)[1].body.background == RgbColor{16, 17, 18});

  take(fixture.engine.removeLayer(userLayer));
  const auto withoutUser = fixture.read();
  CHECK((*withoutUser.scrollbarThumbSegments)[2].body.foreground == RgbColor{32, 33, 34});
  CHECK((*withoutUser.scrollbarThumbSegments)[0].single.underline);

  ComputedStyle defaults;
  defaults.normal.bold = true;
  const auto nonmatching =
    resolveStyle(take(parsePss("ScrollBar{scrollbar-thumb-start-bold:false;}")), {"Button", "", {}}, defaults);
  CHECK(nonmatching == defaults);
  CHECK_FALSE(nonmatching.scrollbarThumbSegments);
}

TEST_CASE("resolved thumb segments own copied values after source and engine retirement") {
  const auto retained = [] {
    Fixture fixture;
    take(
      fixture.engine.replace(take(parsePss("*{scrollbar-thumb-background:#010203;scrollbar-thumb-end-bold:true;}"))));
    return fixture.read();
  }();
  REQUIRE(retained.scrollbarThumbSegments);
  CHECK((*retained.scrollbarThumbSegments)[0].end.bold);
  CHECK((*retained.scrollbarThumbSegments)[1].end.background == RgbColor{1, 2, 3});
  auto defaults = retained;
  (*defaults.scrollbarThumbSegments)[0].start.foreground = RgbColor{7, 8, 9};
  const auto unchanged = resolveStyle({}, {"ScrollBar", "", {}}, defaults);
  CHECK(unchanged == defaults);
  CHECK((*retained.scrollbarThumbSegments)[0].start.foreground != RgbColor{7, 8, 9});
}
