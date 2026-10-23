// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/examples/replay/terminal.hpp"
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_action.hpp>

#include <atomic>
#include <cstdlib>
#include <optional>
#include <thread>

static_assert(requires(portals::ui::ActionSet &actions) {
  actions.action("exit", "Exit", portals::ui::quit);
  actions.add("exit", "Exit", portals::ui::quit);
});

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
thread_local void (*allocationHook)(){};
thread_local std::ptrdiff_t hookAfter{};
} // namespace

// Allocation injection is confined to one checked registration boundary, never the test reporter.
void *operator new(std::size_t size) {
  if (allocationHook && hookAfter-- == 0) {
    auto hook = std::exchange(allocationHook, nullptr);

    hook();
  }

  if (allocationsBeforeFailure.load() == 0) {
    throw std::bad_alloc();
  }

  if (allocationsBeforeFailure.load() > 0) {
    --allocationsBeforeFailure;
  }

  auto *memory = std::malloc(size ? size : 1);

  if (!memory) {
    throw std::bad_alloc();
  }

  return memory;
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
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

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  Node<Column> root = take(scene->emplace<Column>(scene->root()));

  Node<Button> button(const ui::ActionRef &action) {
    auto node = take(scene->emplace<Button>(root, take(GraphemeText::from(""))));

    take(scene->onClick(node, action));

    return node;
  }

  void prepare() {
    take(scene->prepare(*app, {40, 8}));
  }

  std::string caption(Node<Button> node) {
    std::string result;

    take(app->tree().read(node, [&](const Button &button) {
      result = button.text().bytes();
    }));

    return result;
  }
};

ManagedScene *hookScene{};
ui::ActionRef hookAction;
std::optional<std::expected<void, ManagedError>> hookResult;

void closeDuringAllocation() {
  hookResult.emplace(hookScene->close());
}

void attachDuringAllocation() {
  hookResult.emplace(hookScene->onKey(KeyEvent{InputKey::F2}, hookAction));
}
} // namespace

TEST_CASE("one action owns callback and synchronizes all button presentations") {
  Fixture fixture;
  ui::ActionSet actions;
  std::vector<ui::ActionInvocation> calls(0);
  auto action = actions.action("save", "Save", [&](const ui::ActionInvocation &context) {
    calls.push_back(context);
  });
  auto first = fixture.button(action);
  auto second = fixture.button(action);

  fixture.prepare();
  CHECK(fixture.caption(first) == "Save");
  CHECK(fixture.caption(second) == "Save");
  take(fixture.app->router().invoke(first, {Action::ACTIVATE, {.alt = true}, ClickSource::KEYBOARD}));
  REQUIRE(calls.size() == 1);
  CHECK(calls.front().source == ClickSource::KEYBOARD);
  CHECK(calls.front().modifiers.alt);

  auto metadata = *take(action.snapshot());
  metadata.caption = "Store";
  metadata.checked = true;
  REQUIRE(action.set(metadata));

  fixture.prepare();
  CHECK(fixture.caption(first) == "Store");
  CHECK(fixture.caption(second) == "Store");
  take(fixture.app->tree().read(second, [](const Button &button) {
    CHECK(button.checked());
  }));

  metadata.enabled = false;
  REQUIRE(action.set(metadata));
  REQUIRE(action.invoke());
  CHECK(calls.size() == 1);
  fixture.prepare();
  CHECK_FALSE(take(fixture.app->tree().inputEligible(first)));
  CHECK_FALSE(take(fixture.app->tree().inputEligible(second)));

  metadata.enabled = true;
  metadata.visible = false;
  REQUIRE(action.set(metadata));
  REQUIRE(action.invoke());
  CHECK(calls.size() == 1);
  fixture.prepare();
  CHECK_FALSE(take(fixture.app->tree().inputEligible(first)));
}

TEST_CASE("self rebind and removal preserve the entered callback capture") {
  Fixture fixture;
  ui::ActionSet actions;
  ui::ActionRef action;
  auto capture = std::make_shared<int>(17);
  std::weak_ptr<int> oldCapture = capture;
  int first{}, replacement{};
  action = actions.action("self", "First", [&, capture] {
    ++first;
    REQUIRE(action.rebind({{.caption = "Next"}, [&] {
                             ++replacement;
                           }}));
    CHECK_FALSE(oldCapture.expired());
    CHECK(*capture == 17);
  });
  capture.reset();
  fixture.button(action);
  REQUIRE(action.invoke());
  CHECK(oldCapture.expired());
  REQUIRE(action.invoke());
  CHECK(first == 1);
  CHECK(replacement == 1);

  capture = std::make_shared<int>(29);
  oldCapture = capture;
  REQUIRE(action.rebind({{.caption = "Remove"}, [&, capture] {
                           REQUIRE(action.remove());
                           CHECK_FALSE(oldCapture.expired());
                           CHECK(*capture == 29);
                         }}));
  capture.reset();
  REQUIRE(action.invoke());
  CHECK(oldCapture.expired());
  CHECK(action.snapshot().error() == TreeError::STALE_NODE);
  CHECK(action.invoke().result().error() == TreeError::STALE_NODE);
}

TEST_CASE("scene close revokes weak references and releases callback even while set survives") {
  Fixture fixture;
  ui::ActionSet actions;
  auto capture = std::make_shared<int>(7);
  std::weak_ptr<int> observed = capture;
  auto action = actions.action("close", "Close", [capture] {
  });
  capture.reset();
  fixture.button(action);
  REQUIRE_FALSE(observed.expired());
  take(fixture.scene->close());
  CHECK(observed.expired());
  CHECK(action.snapshot().error() == TreeError::CLOSED);
  CHECK(action.invoke().result().error() == TreeError::CLOSED);
}

TEST_CASE("exclusive groups are independent from organizational keys and reject invalid mutation atomically") {
  ui::ActionSet actions;
  auto first = actions.action("first", {{.caption = "First", .checked = true, .exclusiveGroup = "mode"}, [] {
                                        }});
  auto second = actions.action("second", {{.caption = "Second", .exclusiveGroup = "mode"}, [] {
                                          }});
  auto independent = actions.action("mode", {{.caption = "Independent", .checked = true}, [] {
                                             }});
  auto selected = *take(second.snapshot());
  selected.checked = true;
  REQUIRE(second.set(selected));
  CHECK_FALSE(take(first.snapshot())->checked);
  CHECK(take(second.snapshot())->checked);
  CHECK(take(independent.snapshot())->checked);

  auto invalid = selected;
  invalid.caption.assign(1, static_cast<char>(0xff));
  CHECK(second.set(invalid).result().error() == TreeError::INVALID_ARGUMENT);
  CHECK(take(second.snapshot())->caption == "Second");
  CHECK(take(second.snapshot())->checked);
  CHECK_FALSE(take(first.snapshot())->checked);
}

TEST_CASE("bounded action storage returns sticky and checked typed failures without partial admission") {
  ui::ActionSet actions({.maxActions = 1, .maxPresenters = 1, .maxMetadataBytes = 24, .maxKeyBytes = 8});
  auto first = take(actions.add("one", "One", [] {
  }));
  auto duplicate = actions.add("one", "Again", [] {
  });
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error() == TreeError::INVALID_ARGUMENT);
  CHECK(actions
          .add("two", "Two",
               [] {
               })
          .error() == TreeError::NODE_LIMIT);
  auto oversized = *take(first.snapshot());
  oversized.caption = std::string(30, 'x');
  CHECK(first.set(oversized).result().error() == TreeError::NODE_LIMIT);
  CHECK(take(first.snapshot())->caption == "One");
  REQUIRE(first.remove());
  REQUIRE(actions.add("two", "Two", [] {
  }));

  auto bad = actions.action("key-too-long", "Bad", [] {
  });
  CHECK(bad.snapshot().error() == TreeError::INVALID_ARGUMENT);
  example::ReplayTerminal terminal({}, 20, {40, 8});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  auto scene = ui::scene(ui::vbox(ui::button(bad)), options);
  CHECK_FALSE(scene.run(terminal));
  CHECK(terminal.output.empty());
}

TEST_CASE("callbacks and mutation receipts report once through existing keyboard collector") {
  Fixture fixture;
  ui::ActionSet actions;
  auto failure = actions.action("failure", "Fail", []() -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  });
  take(fixture.scene->onKey(KeyEvent{InputKey::F1}, failure));
  auto receipt = failure.invoke();
  CHECK_FALSE(receipt);
  CHECK(receipt.result().error() == TreeError::INVALID_ARGUMENT);
  auto diagnostics = take(fixture.scene->diagnostics());
  CHECK(diagnostics.errors.size() == 1);
}

TEST_CASE("keyboard and button share one action and each input invokes it once") {
  example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\x13", {}}, {30, "\x11", {}}}, 50, {40, 8});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::ActionSet actions;
  int calls{};
  auto save = actions.action("save", "Save", [&] {
    ++calls;
  });
  auto scene = ui::scene(ui::vbox(ui::button(save)), options) | ui::onKey("Ctrl+S", save) | ui::onKey("Ctrl+S", save) |
               ui::onKey("Ctrl+Q", ui::quit);
  REQUIRE(scene.run(terminal));
  CHECK(calls == 2);
  CHECK(terminal.closed);
}

TEST_CASE("action belongs to one scene and foreign thread cannot inspect or mutate it") {
  Fixture first;
  Fixture second;
  ui::ActionSet actions;
  auto action = actions.action("one", "One", [] {
  });
  first.button(action);
  auto rejected = second.scene->onKey(KeyEvent{InputKey::F1}, action);
  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::WRONG_SESSION);
  std::thread foreign([&] {
    CHECK(action.snapshot().error() == TreeError::WRONG_THREAD);
    CHECK(action.invoke().result().error() == TreeError::WRONG_THREAD);
  });
  foreign.join();
}

TEST_CASE("new action storage allocation failures preserve entry admission and exclusive selection") {
  int typedFailures{}, successfulAdmissions{};
  const std::string caption(80, 'a');

  for (std::ptrdiff_t budget = 0; budget < 160; ++budget) {
    ui::ActionSet actions({.maxActions = 1});

    allocationsBeforeFailure = budget;
    auto result = actions.add("key", caption, [] {
    });
    allocationsBeforeFailure = -1;

    if (result) {
      ++successfulAdmissions;

      break;
    }

    CHECK(result.error() == TreeError::NO_MEMORY);
    ++typedFailures;
    REQUIRE(actions.add("key", caption, [] {
    }));
  }

  CHECK(typedFailures > 0);
  CHECK(successfulAdmissions == 1);

  int stateFailures{}, successfulSelections{};
  for (std::ptrdiff_t budget = 0; budget < 160; ++budget) {
    ui::ActionSet actions;
    auto first = actions.action("first", {{.caption = "First", .checked = true, .exclusiveGroup = "mode"}, [] {
                                          }});
    auto second = actions.action("second", {{.caption = "Second", .exclusiveGroup = "mode"}, [] {
                                            }});
    auto candidate = *take(second.snapshot());
    candidate.checked = true;
    candidate.caption = caption;
    std::optional<EditorCommandResult> result;

    allocationsBeforeFailure = budget;
    try {
      result.emplace(second.set(candidate));
    } catch (const std::bad_alloc &) {
      // By-value caller argument construction precedes the checked mutation boundary.
    }
    allocationsBeforeFailure = -1;

    if (result && *result) {
      ++successfulSelections;
      CHECK_FALSE(take(first.snapshot())->checked);
      CHECK(take(second.snapshot())->checked);

      break;
    }

    if (result) {
      CHECK(result->result().error() == TreeError::NO_MEMORY);
      ++stateFailures;
    }

    CHECK(take(first.snapshot())->checked);
    CHECK_FALSE(take(second.snapshot())->checked);
    CHECK(take(second.snapshot())->caption == "Second");
  }

  CHECK(stateFailures > 0);
  CHECK(successfulSelections == 1);
}

TEST_CASE("explicit callback can destroy its scene without retaining an executable stale owner") {
  Fixture fixture;
  ui::ActionSet actions;
  auto action = actions.action("retire", "Retire", [&] {
    fixture.scene.reset();
  });
  fixture.button(action);
  auto result = action.invoke();
  REQUIRE_FALSE(result);
  CHECK(result.result().error() == TreeError::CLOSED);
  CHECK(action.snapshot().error() == TreeError::CLOSED);
}

TEST_CASE("recursive invocation and returned mutation failures retain exactly one collector receipt") {
  Fixture fixture;
  ui::ActionSet actions;
  ui::ActionRef recursive;
  recursive = actions.action("recursive", "Recursive", [&] {
    return recursive.invoke();
  });
  take(fixture.scene->onKey(KeyEvent{InputKey::F1}, recursive));
  auto result = fixture.scene->input(*fixture.app, KeyEvent{InputKey::F1});
  REQUIRE(result);
  CHECK(*result);
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
}

TEST_CASE("removed owning root denies action invocation even before scene destruction") {
  Fixture fixture;
  ui::ActionSet actions;
  int calls{};
  auto action = actions.action("root", "Root", [&] {
    ++calls;
  });
  fixture.button(action);
  take(fixture.app->tree().remove(fixture.scene->root()));
  CHECK(action.invoke().result().error() == TreeError::STALE_NODE);
  CHECK(calls == 0);
}

TEST_CASE("cold button recipe and key-only stage retain factory local declarations until mount") {
  example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\x13", {}}, {30, "\x11", {}}}, 50, {40, 8});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  int buttonCalls{}, keyCalls{};

  auto scene = [&] {
    ui::ActionSet buttons;
    ui::ActionSet keys;
    auto button = buttons.action("button", "Button", [&] {
      ++buttonCalls;
    });
    auto key = keys.action("key", "Key", [&] {
      ++keyCalls;
    });

    return ui::scene(ui::vbox(ui::button(button)), options) | ui::onKey("Ctrl+S", key) | ui::onKey("Ctrl+Q", ui::quit);
  }();

  REQUIRE(scene.run(terminal));
  CHECK(buttonCalls == 1);
  CHECK(keyCalls == 1);
}

TEST_CASE("action capture retirement masks the unrelated executing facade") {
  struct OnRetire {
    bool &retired;

    ~OnRetire() {
      retired = true;
      ui::quit();
    }
  };

  for (int mode = 0; mode < 3; ++mode) {
    Fixture independent;
    ui::ActionSet retiredActions;
    ui::ActionRef retiredAction;
    bool captureRetired{};
    auto capture = std::make_shared<OnRetire>(captureRetired);
    retiredAction = retiredActions.action("retire", "Retire", [capture, &retiredAction, mode] {
      if (mode == 2) {
        REQUIRE(retiredAction.remove());
      }
    });
    capture.reset();
    independent.button(retiredAction);

    example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\x13", {}}, {30, "\x11", {}}}, 50, {40, 8});
    ui::SceneOptions options;
    ui::ActionSet callerActions;
    int continued{};
    options.clock = terminal.clock();
    auto retire = callerActions.action("caller", "Caller", [&] {
      if (mode == 0) {
        REQUIRE(retiredAction.remove());
      } else if (mode == 1) {
        REQUIRE(retiredAction.rebind({{.caption = "Replacement"}, [] {
                                      }}));
      } else {
        REQUIRE(retiredAction.invoke());
      }
    });
    auto scene = ui::scene(ui::vbox(ui::button(retire)), options) |
                 ui::onKey("Ctrl+S",
                           [&] {
                             ++continued;
                           }) |
                 ui::onKey("Ctrl+Q", ui::quit);

    REQUIRE(scene.run(terminal));
    CHECK(captureRetired);
    CHECK(continued == 1);
  }
}

TEST_CASE("action attachment revalidates owner close after allocation before publishing") {
  Fixture fixture;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> observed = capture;

  {
    ui::ActionSet actions;
    auto action = actions.action("close", "Close", [capture] {
    });
    capture.reset();
    hookScene = fixture.scene.get();
    hookResult.reset();
    hookAfter = 0;
    allocationHook = closeDuringAllocation;

    auto result = fixture.scene->onKey(KeyEvent{InputKey::F1}, action);
    allocationHook = nullptr;
    hookScene = nullptr;

    REQUIRE(hookResult);
    CHECK(*hookResult);
    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::CLOSED);
  }

  CHECK(observed.expired());
}

TEST_CASE("action attachment preserves the independently published scene owner") {
  Fixture outer;
  Fixture inner;
  ui::ActionSet actions;
  int calls{};
  auto action = actions.action("shared", "Shared", [&] {
    ++calls;
  });
  hookScene = inner.scene.get();
  hookAction = action;
  hookResult.reset();
  hookAfter = 0;
  allocationHook = attachDuringAllocation;

  auto result = outer.scene->onKey(KeyEvent{InputKey::F1}, action);
  allocationHook = nullptr;
  hookScene = nullptr;
  hookAction = {};

  REQUIRE(hookResult);
  REQUIRE(*hookResult);
  REQUIRE_FALSE(result);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::WRONG_SESSION);
  take(outer.scene->close());
  REQUIRE(action.invoke());
  CHECK(calls == 1);
  take(inner.scene->close());
  CHECK(action.snapshot().error() == TreeError::CLOSED);
}

TEST_CASE("bare quit is admitted by shared action declarations") {
  using namespace portals;
  example::ReplayTerminal terminal({{10, "\r", {}}}, 1000, {40, 8});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  const auto status = [&] {
    ui::ActionSet actions;
    auto exit = actions.action("exit", "Exit", ui::quit);

    return ui::scene(ui::vbox(ui::button(exit)), options);
  } | ui::runApp(terminal);

  CHECK(status == 0);
  CHECK(terminal.closed);
  CHECK(options.clock.read(options.clock.context) < 1000);
}
