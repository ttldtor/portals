// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/styles/style_roles.hpp"
#include <doctest.h>
#include <functional>
#include <portals/style_pss.hpp>
#include <portals/ui_style.hpp>
#include <thread>
#include <utility>

#include <cstdlib>
#include <new>

namespace role_allocation_test {
thread_local bool failNext{};
thread_local std::size_t rejected{};
thread_local portals::StyleRoleRegistration *retireOnNext{};
thread_local void (*onAllocate)() = nullptr;
thread_local std::function<void()> callback;
} // namespace role_allocation_test

void *operator new(std::size_t size) {
  if (role_allocation_test::failNext) {
    role_allocation_test::failNext = false;
    ++role_allocation_test::rejected;
    throw std::bad_alloc();
  }
  if (auto hook = std::exchange(role_allocation_test::onAllocate, nullptr)) {
    hook();
  }
  if (auto *registration = std::exchange(role_allocation_test::retireOnNext, nullptr)) {
    *registration = {};
  }
  if (auto result = std::malloc(size ? size : 1)) {
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
  std::free(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  std::free(value);
}

namespace portals {
struct StyleLayerTestAccess {
  static auto registryPin(const StyleEngine &engine) {
    return engine.roleRegistrations_;
  }
};
} // namespace portals

namespace portals::detail {
struct StyleRoleTestAccess {
  static std::shared_ptr<StyleRoleState> state(const StyleRoleRegistration &registration) {
    return registration.state_;
  }
};
} // namespace portals::detail

using namespace std::string_view_literals;

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

constexpr RgbColor RED{220, 10, 10};
constexpr RgbColor GREEN{10, 220, 10};
constexpr RgbColor BLUE{10, 10, 220};

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  StyleEngine engine{*tree};
  Node<Widget> subject = take(tree->emplace<Widget>(root));

  Fixture() {
    take(engine.bind(subject, {"Widget", "subject"}));
  }

  StyleRoleRegistration roles(std::initializer_list<std::string_view> names = {"body", "gutter", "position"}) {
    return take(engine.registerRoles(subject, std::span(names.begin(), names.size())));
  }
};

struct RoleAllocationHook : Widget {
  bool reject{};
  bool duringAllocation{};
  mutable StyleRoleRegistration *retire{};

  ComputedStyle styleDefaults() const override {
    role_allocation_test::failNext = reject;
    if (retire) {
      if (duringAllocation) {
        role_allocation_test::retireOnNext = retire;
      } else {
        *retire = {};
      }
      retire = nullptr;
    }
    return {};
  }
};

struct ThrowDefaults : Widget {
  bool fail{};

  ComputedStyle styleDefaults() const override {
    if (fail) {
      throw std::bad_alloc();
    }
    return {};
  }
};
} // namespace

TEST_CASE("explicit role namespace preserves legacy unknown-property diagnostics") {
  for (const auto source : {"Widget{frame-lnie-foreground:#123456;}", "Widget{gutter-foreground:#123456;}",
                            "Widget{role--foreground:#123456;}", "Widget{role-gutter-froeground:#123456;}"}) {
    const auto parsed = parsePssDocument(source);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == PssErrorCode::UNKNOWN_PROPERTY);
  }
  const auto parsed =
    take(parsePssDocument("Widget{role-gutter-foreground:#010203;role-body-foreground:#040506;"
                          "role-gutter-bold:true;role-gutter-bold:false;role-gutter-background:#070809;"
                          "role-gutter-italic:true;role-gutter-underline:true;}",
                          "roles.pss"));
  REQUIRE(parsed.sheet.rules.size() == 1);
  REQUIRE(parsed.sheet.rules[0].roles);
  REQUIRE(parsed.sheet.rules[0].roles->size() == 2);
  const auto &gutter = parsed.sheet.rules[0].roles->front();
  CHECK(gutter.name == "gutter");
  CHECK(gutter.paint.foreground == RgbColor{1, 2, 3});
  CHECK(gutter.paint.background == RgbColor{7, 8, 9});
  CHECK(gutter.paint.bold == false);
  CHECK(gutter.paint.italic == true);
  CHECK(gutter.paint.underline == true);
  REQUIRE(parsed.declarations[1].roleName);
  CHECK(*parsed.declarations[1].roleName == "body");
  auto owner = ui::scene(ui::label("roles"));
  owner | ui::style(parsed);
  REQUIRE(owner.prepareChecked());
  REQUIRE(owner.close());
}

TEST_CASE("role-only publication changes paint while an equal base remains unchanged") {
  Fixture f;
  auto roles = f.roles();
  take(f.engine.replacePss("Widget{foreground:#112233;role-gutter-foreground:#010203;}"));
  auto first = roles.snapshot();
  REQUIRE(first);
  CHECK(first->find("body")->foreground == RgbColor{17, 34, 51});
  CHECK(first->find("gutter")->foreground == RgbColor{1, 2, 3});
  const auto changed = take(f.engine.replacePss("Widget{foreground:#112233;role-gutter-foreground:#040506;}"));
  CHECK(changed == Invalidation::PAINT);
  CHECK(roles.snapshot() != first);
  CHECK(roles.snapshot()->find("gutter")->foreground == RgbColor{4, 5, 6});
  CHECK(first->find("gutter")->foreground == RgbColor{1, 2, 3});
  const auto snapshot = roles.snapshot();
  CHECK(take(f.engine.refresh()) == Invalidation::NONE);
  CHECK(roles.snapshot() == snapshot);
  CHECK(roles.revision() == f.engine.revision());
  CHECK_FALSE(snapshot->find("missing"));
  CHECK_FALSE(snapshot->find("gutter", static_cast<StyleState>(255)));
}

TEST_CASE("role layers states and explicit false share the ordinary cascade") {
  Fixture f;
  auto roles = f.roles();
  take(f.engine.enableLayers());
  const auto theme =
    take(parsePss("Widget{role-gutter-foreground:#010203;role-gutter-bold:true;}"
                  "Widget:focused{role-gutter-foreground:#040506;}Widget:disabled{role-gutter-italic:true;}"));
  take(f.engine.theme(theme));
  const auto user = take(f.engine.addLayer(StyleTier::USER, "roles",
                                           take(parsePss("#subject{foreground:#AABBCC;role-gutter-bold:false;}"))));
  CHECK(roles.snapshot()->find("gutter")->foreground == RgbColor{1, 2, 3});
  CHECK_FALSE(roles.snapshot()->find("gutter")->bold);
  CHECK(roles.snapshot()->find("gutter", StyleState::FOCUSED)->foreground == RgbColor{4, 5, 6});
  CHECK(roles.snapshot()->find("gutter", StyleState::DISABLED)->italic);
  CHECK(roles.snapshot()->find("body")->foreground == RgbColor{170, 187, 204});
  CHECK(take(f.engine.theme(take(parsePss("Widget{role-gutter-foreground:#070809;}")))) == Invalidation::PAINT);
  CHECK(roles.snapshot()->find("gutter")->foreground == RgbColor{7, 8, 9});
  take(f.engine.removeLayer(user));
  CHECK_FALSE(roles.snapshot()->find("body")->foreground);
}

TEST_CASE("registration limits and argument failures leave existing publication untouched") {
  Fixture f;
  auto roles = f.roles();
  const auto original = roles.snapshot();
  const auto revision = f.engine.revision();
  const std::array duplicate{"body"sv, "body"sv};
  CHECK_FALSE(f.engine.registerRoles(f.subject, duplicate));
  CHECK_FALSE(f.engine.registerRoles(f.subject, {}));
  CHECK(roles.snapshot() == original);
  CHECK(f.engine.revision() == revision);
  CHECK(take(f.engine.unregisterRoles(roles)) == Invalidation::PAINT);
  CHECK_FALSE(roles.snapshot());
  CHECK(roles.revision() == 0);
  CHECK_FALSE(f.engine.unregisterRoles(roles));
  auto replacement = f.roles({"custom-part"});
  REQUIRE(replacement.snapshot());
  StyleEngine limited(*f.tree, 4, {1, 1, 1});
  take(limited.bind(f.subject, {"Widget"}));
  const std::array one{"body"sv};
  auto single = take(limited.registerRoles(f.subject, one));
  const auto other = take(f.tree->emplace<Widget>(f.root));
  take(limited.bind(other, {"Widget"}));
  CHECK_FALSE(limited.registerRoles(other, one));
}

TEST_CASE("default failure rolls back base role data and membership together") {
  Fixture f;
  const auto hook = take(f.tree->emplace<ThrowDefaults>(f.root));
  take(f.engine.bind(hook, {"Widget"}));
  auto roles = f.roles();
  const auto before = roles.snapshot();
  const auto revision = f.engine.revision();
  take(f.tree->edit(hook, [](ThrowDefaults &widget) {
    widget.fail = true;
  }));
  const auto failed = f.engine.replacePss("Widget{role-body-foreground:#010203;}");
  REQUIRE_FALSE(failed);
  CHECK(failed.error().cause == TreeError::NO_MEMORY);
  CHECK(roles.snapshot() == before);
  CHECK(f.engine.revision() == revision);
  const auto unregistered = f.engine.unregisterRoles(roles);
  REQUIRE_FALSE(unregistered);
  CHECK(roles.snapshot() == before);
  take(f.tree->edit(hook, [](ThrowDefaults &widget) {
    widget.fail = false;
  }));
  CHECK(take(f.engine.refresh()) == Invalidation::NONE);
  CHECK(roles.snapshot() == before);
}

TEST_CASE("retained role snapshots survive registration node and engine retirement") {
  std::shared_ptr<const StyleRolePalette> retained;
  StyleRoleRegistration handle;
  {
    Fixture f;
    handle = f.roles();
    take(f.engine.replacePss("Widget{role-body-foreground:#010203;}"));
    retained = handle.snapshot();
    take(f.tree->remove(f.subject));
    CHECK_FALSE(f.engine.registerRoles(f.subject, std::array{"body"sv}));
    take(f.engine.refresh());
    CHECK(retained->find("body")->foreground == RgbColor{1, 2, 3});
  }
  CHECK_FALSE(handle.snapshot());
  CHECK(retained->find("body")->foreground == RgbColor{1, 2, 3});
}

TEST_CASE("foreign-thread observations reject before mutable projection state") {
  Fixture f;
  auto roles = f.roles();
  const auto prior = roles.snapshot();
  bool rejected{};
  std::thread foreign([&] {
    rejected = !roles.snapshot() && roles.revision() == 0;
  });
  foreign.join();
  CHECK(rejected);
  CHECK(roles.snapshot() == prior);
}

TEST_CASE("role names are owned and runtime destinations remain bounded") {
  Fixture f;
  std::string name(63, 'a');
  const auto expected = name;
  auto roles = take(f.engine.registerRoles(f.subject, std::array{std::string_view(name)}));
  name.assign("changed");
  CHECK(roles.snapshot()->find(expected));
  take(f.engine.unregisterRoles(roles));
  for (const auto invalid :
       {std::string(64, 'a'), std::string("Upper"), std::string("bad_thing"), std::string("bad-")}) {
    CHECK_FALSE(f.engine.registerRoles(f.subject, std::array{std::string_view(invalid)}));
  }
  portals::style::ct::detail::PssCountSink sink;
  const auto parsed =
    portals::detail::PssParser("Widget{role-gutter-foreground:#010203;}", sink, portals::detail::PssPolicy::STRICT_UTF8,
                               style::ct::MAX_CT_PSS_BYTES, style::ct::MAX_CT_PSS_RULES)
      .parse();
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error().code == PssErrorCode::INVALID_VALUE);
  const auto param = parsePssDocument("Widget{role-gutter-foreground:$accent;}");
  REQUIRE_FALSE(param);
  CHECK(param.error().code == PssErrorCode::INVALID_VALUE);
}

TEST_CASE("forged semantic destination provenance is rejected before managed installation") {
  auto forged = take(parsePssDocument("Widget{role-gutter-foreground:#010203;}"));
  forged.declarations.front().roleName = std::make_shared<const std::string>("body");
  auto owner = ui::scene(ui::label("forgery"));
  owner | ui::style(forged);
  const auto prepared = owner.prepareChecked();
  REQUIRE_FALSE(prepared);
  REQUIRE(prepared.error().pss);
  CHECK(prepared.error().pss->code == PssErrorCode::INVALID_VALUE);
}

TEST_CASE("allocation in the new role projection rolls back the complete style transaction") {
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  StyleEngine engine(*tree);
  const auto node = take(tree->emplace<RoleAllocationHook>(take(tree->root())));
  take(engine.bind(node, {"Widget"}));
  auto registration = take(engine.registerRoles(node, std::array{"body"sv}));
  const auto previous = registration.snapshot();
  const auto revision = engine.revision();
  const auto candidate = take(parsePss("Widget{role-body-foreground:#010203;}"));
  take(tree->edit(node, [](RoleAllocationHook &widget) {
    widget.reject = true;
  }));
  const auto rejectedBefore = role_allocation_test::rejected;
  const auto failed = engine.replace(candidate);
  REQUIRE_FALSE(failed);
  CHECK(failed.error().cause == TreeError::NO_MEMORY);
  CHECK(role_allocation_test::rejected == rejectedBefore + 1);
  CHECK_FALSE(role_allocation_test::failNext);
  CHECK(registration.snapshot() == previous);
  CHECK(engine.revision() == revision);
  take(tree->edit(node, [](RoleAllocationHook &widget) {
    widget.reject = false;
  }));
  CHECK(take(engine.refresh()) == Invalidation::NONE);
  CHECK(registration.snapshot() == previous);
}

TEST_CASE("retiring the consumer during defaults or role allocation cannot publish through a state pin") {
  for (const bool duringAllocation : {false, true}) {
    CAPTURE(duringAllocation);
    auto scheduler = take(Scheduler::create());
    auto tree = take(WidgetTree::create(*scheduler));
    StyleEngine engine(*tree);
    const auto node = take(tree->emplace<RoleAllocationHook>(take(tree->root())));
    take(engine.bind(node, {"Widget"}));
    auto registration = take(engine.registerRoles(node, std::array{"body"sv}));
    // A probe pins only state, like a staging transaction; it is not a consumer.
    const auto state = portals::detail::StyleRoleTestAccess::state(registration);
    const auto palette = state->palette;
    const auto revision = state->revision;
    const auto candidate = take(parsePss("Widget{role-body-foreground:#010203;}"));
    take(tree->edit(node, [&](RoleAllocationHook &widget) {
      widget.retire = &registration;
      widget.duringAllocation = duringAllocation;
    }));
    take(engine.replace(candidate));
    CHECK_FALSE(registration.snapshot());
    CHECK(state->participation.expired());
    CHECK(state->palette == palette);
    CHECK(state->revision == revision);
    CHECK_FALSE(role_allocation_test::retireOnNext);
    CHECK(take(engine.refresh()) == Invalidation::NONE);
    auto replacement = take(engine.registerRoles(node, {"body"}));
    CHECK(replacement.snapshot());
    CHECK(state->revision == revision);
  }
}

TEST_CASE("installed role declarations freeze an externally retained mutable alias") {
  Fixture f;
  auto registration = f.roles();
  auto patches = std::make_shared<std::vector<StyleRolePatch>>();
  patches->push_back({.name = "body", .paint = {.foreground = RED}});
  StyleRule rule;
  rule.roles = patches;
  take(f.engine.replace({.rules = {rule}}));
  const auto palette = registration.snapshot();
  patches->front().paint.foreground = BLUE;
  CHECK(take(f.engine.refresh()) == Invalidation::NONE);
  CHECK(registration.snapshot() == palette);
  CHECK(palette->find("body")->foreground == RED);
  take(f.engine.enableLayers());
  const auto layer = take(f.engine.addLayer(StyleTier::USER, "aliased", {.rules = {rule}}));
  CHECK(registration.snapshot()->find("body")->foreground == BLUE);
  patches->front().paint.foreground = GREEN;
  CHECK(take(f.engine.refresh()) == Invalidation::NONE);
  CHECK(registration.snapshot()->find("body")->foreground == BLUE);
  take(f.engine.removeLayer(layer));
  CHECK(registration.snapshot()->find("body")->foreground == RED);
}

TEST_CASE("managed semantic document freezes staged source and both normal admission paths") {
  for (const bool staged : {false, true}) {
    CAPTURE(staged);
    auto document = take(parsePssDocument("Label{role-gutter-foreground:#010203;}"));
    auto patches = std::make_shared<std::vector<StyleRolePatch>>(*document.sheet.rules.front().roles);
    auto name = std::make_shared<std::string>("gutter");
    document.sheet.rules.front().roles = patches;
    document.declarations.front().roleName = name;
    auto owner = ui::scene(ui::label("roles"));
    if (staged) {
      owner | ui::style(document);
      // A cold stage must already own these fields before its later prepare.
      patches->front().paint.foreground = BLUE;
      name->assign("changed-before-prepare");
      REQUIRE(owner.prepareChecked());
    } else {
      REQUIRE(owner.prepareChecked());
      take(owner.managed()->get().styleDocument(StyleTier::APPLICATION, "direct", document));
    }
    auto sources = take(owner.managed()->get().styleSources());
    REQUIRE(sources.back().document);
    const auto frozen = sources.back().document;
    patches->front().paint.foreground = GREEN;
    name->assign("changed-after-admission");
    CHECK(frozen->sheet.rules.front().roles->front().paint.foreground == RgbColor{1, 2, 3});
    CHECK(*frozen->declarations.front().roleName == "gutter");
    if (!staged) {
      patches->front().paint.foreground = RgbColor{1, 2, 3};
      name->assign("gutter");
      document.sourceName = "updated.pss";
      // Equal declarations but changed metadata take the other admission branch.
      take(owner.managed()->get().styleDocument(StyleTier::APPLICATION, "direct", document));
      sources = take(owner.managed()->get().styleSources());
      const auto updated = sources.back().document;
      REQUIRE(updated);
      patches->front().paint.foreground = BLUE;
      name->assign("changed-after-metadata-only");
      CHECK(updated->sourceName == "updated.pss");
      CHECK(updated->sheet.rules.front().roles->front().paint.foreground == RgbColor{1, 2, 3});
      CHECK(*updated->declarations.front().roleName == "gutter");
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("runtime semantic patch count is bounded before installation") {
  std::string source = "Widget{";
  for (std::size_t index = 0; index <= MAX_STYLE_ROLE_PATCHES; ++index) {
    source.append("role-p" + std::to_string(index) + "-bold:true;");
  }
  source.append("}");
  REQUIRE(source.size() < MAX_PSS_BYTES);
  const auto parsed = parsePss(source);
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error().cause == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("owning role stages reject allocator reentry before their borrowed registry can change") {
  for (const bool removing : {false, true}) {
    for (unsigned action = 0; action < 3; ++action) {
      CAPTURE(removing);
      CAPTURE(action);
      Fixture f;
      const auto other = take(f.tree->emplace<Widget>(f.root));
      const auto nestedNode = take(f.tree->emplace<Widget>(f.root));
      take(f.engine.bind(other, {"Widget"}));
      take(f.engine.bind(nestedNode, {"Widget"}));
      auto survivor = take(f.engine.registerRoles(other, {"body"}));
      StyleRoleRegistration outer;
      if (removing) {
        outer = f.roles({"body"});
      }
      const auto previousPalette = survivor.snapshot();
      const auto revision = f.engine.revision();
      // This test-owned pin makes the RED state-loss reproduction deterministic
      // without executing the independently traced dangling-span/UAF variant.
      const auto borrowedRegistryPin = portals::StyleLayerTestAccess::registryPin(f.engine);
      REQUIRE(borrowedRegistryPin);
      std::optional<TreeError> nestedCause;
      bool nestedSucceeded{};
      StyleRoleRegistration nestedOwner;
      role_allocation_test::callback = [&] {
        if (action == 0) {
          auto result = f.engine.registerRoles(nestedNode, {"body"});
          nestedSucceeded = result.has_value();
          if (result) {
            nestedOwner = *result;
          } else {
            nestedCause = result.error().cause;
          }
        } else {
          auto result = action == 1 ? f.engine.unregisterRoles(survivor) : f.engine.refresh();
          nestedSucceeded = result.has_value();
          if (!result) {
            nestedCause = result.error().cause;
          }
        }
      };
      role_allocation_test::onAllocate = [] {
        role_allocation_test::callback();
      };
      if (removing) {
        take(f.engine.unregisterRoles(outer));
      } else {
        outer = take(f.engine.registerRoles(f.subject, {"body"}));
      }
      role_allocation_test::callback = {};
      CHECK_FALSE(role_allocation_test::onAllocate);
      CHECK_FALSE(nestedSucceeded);
      CHECK(nestedCause == TreeError::WOULD_BLOCK);
      CHECK(f.engine.revision() == revision + 1);
      CHECK(survivor.snapshot() == previousPalette);
      CHECK(survivor.revision() == f.engine.revision());
      CHECK(bool(outer.snapshot()) == !removing);
      take(f.engine.replacePss("Widget{role-body-foreground:#010203;}"));
      if (const auto palette = survivor.snapshot()) {
        CHECK(palette->find("body")->foreground == RgbColor{1, 2, 3});
      } else {
        CHECK_MESSAGE(false, "surviving registration was retired by allocator reentry");
      }
      if (nestedOwner.snapshot()) {
        CHECK(nestedOwner.revision() == f.engine.revision());
      }
    }
  }
}
