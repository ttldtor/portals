// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <atomic>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace portals;

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
} // namespace

// Allocation injection is confined to one checked registration boundary, never the test reporter.
void *operator new(std::size_t size) {
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
template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool hasCause(const ManagedError &error, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == cause;
}

// The managed fixture exercises real tree/router mutation that the owning facade does not expose.
// Public pipe/deferred/runtime registration is covered by the replay cases below.
struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ManagedOptions options = {}) : scene(take(ManagedScene::create(*app, options))) {
  }

  template <class Callback> void click(ui::Ref<Button> reference, Callback callback) {
    take(ui::detail::ClickAccess::attach(*scene, reference, std::move(callback), std::source_location::current()));
  }
};

static_assert(InputModifiers{.shift = true}.has(key::SHIFT));
static_assert(InputModifiers{.alt = true}.has(key::ALT));
static_assert(InputModifiers{.control = true}.has(key::CTRL));
static_assert(!InputModifiers{}.has(key::SHIFT));
static_assert(ui::detail::ClickCallback<decltype([](const ui::ClickParams &) {
})>);
static_assert(!ui::detail::ClickCallback<decltype([] {
  return 1;
})>);
static_assert(!ui::detail::ClickCallback<decltype([capture = std::unique_ptr<int>{}] {
})>);
static_assert(std::same_as<decltype(ui::hbox(LinearOptions{})), ui::Description<Row>>);
static_assert(std::same_as<decltype(ui::vbox(ui::Ref<Column>{}, LinearOptions{})), ui::Description<Column>>);
static_assert(ActionEvent{Action::ACTIVATE, {}, ClickSource::KEYBOARD} == ActionEvent{Action::ACTIVATE});
static_assert(noexcept(ActionEvent{} == ActionEvent{}));
static_assert(std::copy_constructible<ui::ClickParams>);
static_assert(!std::is_copy_assignable_v<ui::ClickParams>);
static_assert(std::is_const_v<decltype(std::declval<ui::ClickParams>().target)>);
static_assert(std::is_const_v<decltype(std::declval<ui::ClickParams>().mods)>);
static_assert(std::is_const_v<decltype(std::declval<ui::ClickParams>().source)>);
static_assert(std::is_const_v<decltype(std::declval<ui::ClickParams>().event)>);
} // namespace

TEST_CASE("pipe click owns FIFO callbacks and accepts modified keyboard activation") {
  example::ReplayTerminal terminal({{10, "\x1b ", {}}}, 50, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  std::vector<int> calls;

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::vbox(ui::hbox(ui::button(button, "Click"))), options) |
               ui::onClick(button,
                           [&] {
                             calls.push_back(1);
                           }) |
               ui::onClick(button,
                           [&](ui::ClickParams &params) {
                             calls.push_back(2);
                             CHECK(params.target == button.node());
                             CHECK(params.mods.has(key::ALT));
                             CHECK(params.source == ClickSource::KEYBOARD);
                             params.stopPropagation();
                           }) |
               ui::onClick(button, [&] {
                 calls.push_back(3);
               });

  REQUIRE(scene.prepareChecked());
  const auto result = scene.run(terminal);

  REQUIRE(result);
  CHECK(calls == std::vector<int>{1, 2});
}

TEST_CASE("pipe runtime append is admitted only on the next activation") {
  example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\r", {}}}, 50, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  std::vector<int> calls;
  ui::Scene *active{};
  bool appended{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::button(button, "Click"), options) |
               ui::onClick(button,
                           [&] {
                             calls.push_back(1);

                             if (!appended) {
                               appended = true;
                               *active | ui::onClick(button, [&] {
                                 calls.push_back(3);
                               });
                             }
                           }) |
               ui::onClick(button, [&] {
                 calls.push_back(2);
               });

  active = &scene;
  take(scene.run(terminal));
  CHECK(calls == std::vector<int>{1, 2, 1, 2, 3});
}

TEST_CASE("click source and original modifiers survive semantic translation and snapshots") {
  Fixture fixture;
  ui::Ref<Button> button;
  std::optional<ui::ClickParams> snapshot;
  int following{};

  take(ui::mount(*fixture.scene, ui::button(button, "Click")));
  fixture.click(button, [&](ui::ClickParams &params) {
    snapshot.emplace(params);
    snapshot->stop();
  });
  fixture.click(button, [&] {
    ++following;
  });
  take(fixture.app->router().setFocus(button.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER, {}, {.shift = true}, KeyAction::PRESS}));

  REQUIRE(snapshot);
  CHECK(snapshot->mods.has(key::SHIFT));
  CHECK(snapshot->source == ClickSource::KEYBOARD);
  CHECK(std::get<ActionEvent>(snapshot->event).source == ClickSource::KEYBOARD);
  CHECK(std::get<ActionEvent>(snapshot->event) == ActionEvent{Action::ACTIVATE, {.shift = true}});
  CHECK_FALSE(std::get<ActionEvent>(snapshot->event) == ActionEvent{Action::ACTIVATE});
  CHECK_FALSE(std::get<ActionEvent>(snapshot->event) == ActionEvent{Action::CANCEL, {.shift = true}});
  CHECK(following == 1);

  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE, {.control = true}}));
  CHECK(snapshot->source == ClickSource::PROGRAMMATIC);
  CHECK(snapshot->mods.has(key::CTRL));

  const auto viewport = take(CellRect::from({}, {30, 4}));

  take(fixture.app->tree().arrange(fixture.app->root(), viewport, viewport));
  take(fixture.app->tree().arrange(fixture.scene->root(), viewport, viewport));
  take(fixture.app->tree().arrange(button.node(), take(CellRect::from({}, {12, 1})), viewport));
  take(fixture.app->router().dispatch(MouseInputEvent{0, 0, MouseButton::LEFT, MouseAction::PRESS, {.alt = true}}));
  CHECK(snapshot->source == ClickSource::MOUSE);
  CHECK(snapshot->mods.has(key::ALT));
  CHECK(std::holds_alternative<MouseInputEvent>(snapshot->event));
  take(fixture.scene->remove(button.node()));
  CHECK(snapshot->target == button.node());
  CHECK(snapshot->mods.has(key::ALT));
}

TEST_CASE("stop alias ends remaining FIFO and existing button consumption ends ancestor propagation") {
  Fixture fixture;
  ui::Ref<Button> button;
  ui::Ref<Column> parent;
  int following{}, ancestor{};

  take(ui::mount(*fixture.scene, ui::vbox(parent, ui::button(button, "Click"))));
  take(
    fixture.scene->onEvent(parent.node(), [&](Column &, const RoutedEvent &) -> std::expected<EventReply, TreeError> {
      ++ancestor;

      return EventReply::HANDLED;
    }));
  fixture.click(button, [](ui::ClickParams &params) {
    params.stop();
  });
  fixture.click(button, [&] {
    ++following;
  });

  const auto result = take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));

  CHECK(result.status == DispatchStatus::HANDLED);
  CHECK(result.handler == button.node().id());
  CHECK(following == 0);
  CHECK(ancestor == 0);
}

TEST_CASE("remove hide disable and close suppress remaining callbacks and retire owning captures") {
  for (int mutation = 0; mutation < 7; ++mutation) {
    Fixture fixture;
    ui::Ref<Button> button;
    ui::Ref<Column> parent;
    int following{}, destroyed{};
    auto lifetime = std::shared_ptr<int>(new int(1), [&](int *value) {
      ++destroyed;
      delete value;
    });

    take(ui::mount(*fixture.scene, ui::vbox(parent, ui::button(button, "Click"))));
    fixture.click(button, [&, lifetime](ui::ClickParams &params) {
      const Node<Widget> target = mutation < 3 ? Node<Widget>(params.target) : Node<Widget>(parent.node());

      if (mutation == 0 || mutation == 3) {
        take(fixture.scene->remove(target));
      } else if (mutation == 1 || mutation == 4) {
        take(fixture.app->tree().setVisible(target, false));
      } else if (mutation == 2 || mutation == 5) {
        take(fixture.app->tree().setEnabled(target, false));
      } else {
        take(fixture.scene->close());
      }

      CHECK(destroyed == 0);
    });
    fixture.click(button, [&, lifetime] {
      ++following;
    });
    lifetime.reset();
    take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
    CHECK(following == 0);
    take(fixture.scene->close());
    take(fixture.app->router().focused());
    CHECK(destroyed == 1);
  }
}

TEST_CASE("first checked failure or exception ends FIFO with original diagnostic origin") {
  for (int failure = 0; failure < 3; ++failure) {
    Fixture fixture;
    ui::Ref<Button> button;
    int following{};

    take(ui::mount(*fixture.scene, ui::button(button, "Click")));

    const auto origin = std::source_location::current();

    take(ui::detail::ClickAccess::attach(
      *fixture.scene, button,
      [failure]() -> std::expected<void, TreeError> {
        if (failure == 1) {
          throw std::bad_alloc();
        }

        if (failure == 2) {
          throw std::runtime_error("click failure");
        }

        return std::unexpected(TreeError::INVALID_ARGUMENT);
      },
      origin));
    fixture.click(button, [&] {
      ++following;
    });
    take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));

    const auto diagnostics = take(fixture.scene->diagnostics());
    const auto cause = failure == 0   ? TreeError::INVALID_ARGUMENT
                       : failure == 1 ? TreeError::NO_MEMORY
                                      : TreeError::CALLBACK_FAILED;

    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(diagnostics.errors[0].error.phase == ManagedPhase::INPUT);
    CHECK(hasCause(diagnostics.errors[0].error, cause));
    CHECK(diagnostics.errors[0].origin.line() == origin.line());
    CHECK(following == 0);
  }
}

TEST_CASE("low level duplicate rejection and noarg modified admission remain unchanged") {
  Fixture fixture;
  ui::Ref<Button> oldButton, pipeButton;
  int oldCalls{}, pipeCalls{};

  take(ui::mount(*fixture.scene, ui::hbox(ui::button(oldButton, "Old"), ui::button(pipeButton, "Pipe"))));
  take(oldButton.onClick([&] {
    ++oldCalls;
  }));
  fixture.click(pipeButton, [&] {
    ++pipeCalls;
  });
  take(fixture.app->router().invoke(oldButton.node(), ActionEvent{Action::ACTIVATE, {.shift = true}}));
  take(fixture.app->router().invoke(pipeButton.node(), ActionEvent{Action::ACTIVATE, {.shift = true}}));
  CHECK(oldCalls == 0);
  CHECK(pipeCalls == 1);

  const auto duplicate = oldButton.onClick([] {
  });
  const auto mixed = ui::detail::ClickAccess::attach(
    *fixture.scene, oldButton,
    [] {
    },
    std::source_location::current());
  const auto reverseMixed = pipeButton.onClick([] {
  });

  REQUIRE_FALSE(duplicate);
  REQUIRE_FALSE(mixed);
  REQUIRE_FALSE(reverseMixed);
  CHECK(hasCause(duplicate.error(), TreeError::INVALID_ARGUMENT));
  CHECK(hasCause(mixed.error(), TreeError::INVALID_ARGUMENT));
  CHECK(hasCause(reverseMixed.error(), TreeError::INVALID_ARGUMENT));
}

TEST_CASE("editor command receipts retain their original target and report exactly once") {
  Fixture fixture;
  ui::Ref<Button> button;
  ui::Ref<LineEdit> editor;
  int following{};
  const auto model = take(fixture.scene->state(std::string("value")));

  take(ui::mount(*fixture.scene, ui::vbox(ui::button(button, "Undo"), ui::lineEdit(editor, model))));
  fixture.click(button, [&](ui::ClickParams &) {
    return editor.undo();
  });
  fixture.click(button, [&] {
    ++following;
  });
  take(fixture.scene->remove(editor.node()));
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));

  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.errors[0].error.node == editor.node().id());
  CHECK(hasCause(diagnostics.errors[0].error, TreeError::STALE_NODE));
  CHECK(following == 0);
}

TEST_CASE("finite click budgets reject atomically and removal restores the scene budget") {
  ManagedOptions options;

  options.maxClickHandlers = 2;
  options.maxClickHandlersPerNode = 1;

  Fixture fixture(options);
  ui::Ref<Button> first, second, third;
  int calls{};

  take(ui::mount(*fixture.scene,
                 ui::hbox(ui::button(first, "One"), ui::button(second, "Two"), ui::button(third, "Three"))));
  fixture.click(first, [&] {
    ++calls;
  });

  const auto perNode = ui::detail::ClickAccess::attach(
    *fixture.scene, first,
    [] {
    },
    std::source_location::current());

  REQUIRE_FALSE(perNode);
  CHECK(hasCause(perNode.error(), TreeError::NODE_LIMIT));
  fixture.click(second, [] {
  });

  const auto total = ui::detail::ClickAccess::attach(
    *fixture.scene, third,
    [] {
    },
    std::source_location::current());

  REQUIRE_FALSE(total);
  CHECK(hasCause(total.error(), TreeError::NODE_LIMIT));
  take(fixture.app->router().invoke(first.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(calls == 1);
  take(fixture.scene->remove(second.node()));
  fixture.click(third, [] {
  });

  for (bool zeroTotal : {false, true}) {
    auto invalid = options;

    (zeroTotal ? invalid.maxClickHandlers : invalid.maxClickHandlersPerNode) = 0;

    const auto result = ManagedScene::create(*fixture.app, invalid);

    REQUIRE_FALSE(result);
    CHECK(hasCause(result.error(), TreeError::INVALID_ARGUMENT));
  }
}

TEST_CASE("click registration checks empty callbacks stale refs foreign affinity and scene identity") {
  Fixture fixture;
  ui::Ref<Button> button;

  take(ui::mount(*fixture.scene, ui::button(button, "Click")));

  const auto empty =
    ui::detail::ClickAccess::attach(*fixture.scene, button, std::function<void()>{}, std::source_location::current());
  std::optional<ManagedError> foreign;

  REQUIRE_FALSE(empty);
  CHECK(hasCause(empty.error(), TreeError::INVALID_ARGUMENT));

  std::thread worker([&] {
    const auto result = ui::detail::ClickAccess::attach(
      *fixture.scene, button,
      [] {
      },
      std::source_location::current());

    if (!result) {
      foreign = result.error();
    }
  });

  worker.join();
  REQUIRE(foreign);
  CHECK(hasCause(*foreign, TreeError::WRONG_THREAD));

  auto wrongScene = ui::scene(ui::label("Other")) | ui::onClick(button, [] {
                    });
  const auto wrong = wrongScene.prepareChecked();

  REQUIRE_FALSE(wrong);
  CHECK(hasCause(wrong.error(), TreeError::INVALID_ARGUMENT));
  fixture.click(button, [] {
  });
  take(fixture.scene->remove(button.node()));

  const auto stale = ui::detail::ClickAccess::attach(
    *fixture.scene, button,
    [] {
    },
    std::source_location::current());

  REQUIRE_FALSE(stale);
  CHECK(hasCause(stale.error(), TreeError::STALE_NODE));
}

TEST_CASE("failed mount leaves refs unpublished and destroys pending click captures") {
  ui::Ref<Button> first, second;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weak = capture;
  auto scene =
    ui::scene(
      ui::vbox(ui::button(first, "First"),
               ui::button(second, "Fails").setup([](ManagedScene &, Node<Button>) -> std::expected<void, ManagedError> {
                 return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::INVALID_ARGUMENT, {}, {}});
               }))) |
    ui::onClick(first, [capture] {
    });

  capture.reset();

  const auto result = scene.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(first.node() == Node<Button>{});
  CHECK(second.node() == Node<Button>{});
  CHECK(weak.expired());
}

TEST_CASE("creating a new binding during dispatch retains existing WOULD_BLOCK admission") {
  Fixture fixture;
  ui::Ref<Button> first, second;
  std::optional<ManagedError> observed;

  take(ui::mount(*fixture.scene, ui::hbox(ui::button(first, "One"), ui::button(second, "Two"))));
  fixture.click(first, [&] {
    const auto result = ui::detail::ClickAccess::attach(
      *fixture.scene, second,
      [] {
      },
      std::source_location::current());

    if (!result) {
      observed = result.error();
    }
  });
  take(fixture.app->router().invoke(first.node(), ActionEvent{Action::ACTIVATE}));
  REQUIRE(observed);
  CHECK(hasCause(*observed, TreeError::WOULD_BLOCK));
  fixture.click(second, [] {
  });
}

TEST_CASE("queued click callbacks do not schedule idle redraws") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  int renders{};

  options.clock = terminal.clock();
  options.application.observer = {[](void *context, ApplicationPhase phase, std::chrono::nanoseconds) {
                                    if (phase == ApplicationPhase::RENDER) {
                                      ++*static_cast<int *>(context);
                                    }
                                  },
                                  &renders};

  auto scene = ui::scene(ui::button(button, "Idle"), options) |
               ui::onClick(button,
                           [] {
                           }) |
               ui::onClick(button, [] {
               });

  take(scene.run(terminal));
  CHECK(renders == 1);
}

TEST_CASE("allocation failures preserve click chains and consume no registration budget") {
  constexpr std::ptrdiff_t ALLOCATION_BUDGETS = 12;
  int failures{}, successes{};

  for (bool append : {false, true}) {
    for (std::ptrdiff_t budget = 0; budget < ALLOCATION_BUDGETS; ++budget) {
      ManagedOptions options;

      options.maxClickHandlers = 2;
      options.maxClickHandlersPerNode = 2;

      Fixture fixture(options);
      ui::Ref<Button> button;
      int first{}, second{};

      take(ui::mount(*fixture.scene, ui::button(button, "Click")));

      if (append) {
        fixture.click(button, [&] {
          ++first;
        });
      }

      std::function<void()> action = [&] {
        ++second;
      };
      std::expected<void, ManagedError> result;

      allocationsBeforeFailure = budget;
      result =
        ui::detail::ClickAccess::attach(*fixture.scene, button, std::move(action), std::source_location::current());
      allocationsBeforeFailure = -1;

      if (!result) {
        ++failures;
        CHECK(hasCause(result.error(), TreeError::NO_MEMORY));
        fixture.click(button, [&] {
          ++second;
        });
      } else {
        ++successes;
      }

      take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
      CHECK(first == (append ? 1 : 0));
      CHECK(second == 1);

      if (!append) {
        fixture.click(button, [&] {
          ++first;
        });
      }

      const auto overflow = ui::detail::ClickAccess::attach(
        *fixture.scene, button,
        [] {
        },
        std::source_location::current());

      REQUIRE_FALSE(overflow);
      CHECK(hasCause(overflow.error(), TreeError::NODE_LIMIT));
    }
  }

  CHECK(failures > 0);
  CHECK(successes > 0);
}

TEST_CASE("mutable callback state survives subsequent activations and retirement reentry is rejected") {
  Fixture fixture;
  ui::Ref<Button> button;
  std::vector<int> observed;
  bool destructorRan{};
  std::optional<ManagedError> reentry;

  take(ui::mount(*fixture.scene, ui::button(button, "Click")));
  fixture.click(button, [counter = 0, &observed]() mutable {
    observed.push_back(++counter);
  });
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(observed == std::vector<int>{1, 2});

  auto capture = std::shared_ptr<int>(new int(1), [&](int *value) {
    destructorRan = true;

    const auto result = ui::detail::ClickAccess::attach(
      *fixture.scene, button,
      [] {
      },
      std::source_location::current());

    if (!result) {
      reentry = result.error();
    }

    delete value;
  });

  fixture.click(button, [capture] {
  });
  capture.reset();
  take(fixture.scene->remove(button.node()));
  CHECK(destructorRan);
  REQUIRE(reentry);
  CHECK(hasCause(*reentry, TreeError::WOULD_BLOCK));
}

TEST_CASE("weak scene lifetime protects an admitted callback when its managed owner is destroyed") {
  Fixture fixture;
  ui::Ref<Button> button;
  int following{}, destroyed{};
  auto capture = std::shared_ptr<int>(new int(1), [&](int *value) {
    ++destroyed;
    delete value;
  });

  take(ui::mount(*fixture.scene, ui::button(button, "Click")));
  fixture.click(button, [&, capture] {
    fixture.scene.reset();
    CHECK(destroyed == 0);
  });
  fixture.click(button, [&, capture] {
    ++following;
  });
  capture.reset();
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(following == 0);
  CHECK(destroyed == 1);
  CHECK_FALSE(fixture.scene);
  take(fixture.app->router().focused());
}

TEST_CASE("foreign collector editor receipt preserves one original report and one receiving report") {
  Fixture original;
  Fixture receiving;
  ui::Ref<LineEdit> editor;
  ui::Ref<Button> button;
  int following{};
  const auto model = take(original.scene->state(std::string("value")));

  take(ui::mount(*original.scene, ui::lineEdit(editor, model)));
  take(ui::mount(*receiving.scene, ui::button(button, "Undo")));
  receiving.click(button, [editor] {
    return editor.undo();
  });
  receiving.click(button, [&] {
    ++following;
  });
  take(original.scene->remove(editor.node()));
  take(receiving.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));

  const auto primary = take(original.scene->diagnostics());
  const auto forwarded = take(receiving.scene->diagnostics());

  REQUIRE(primary.errors.size() == 1);
  REQUIRE(forwarded.errors.size() == 1);
  CHECK(primary.errors[0].error.node == editor.node().id());
  CHECK(forwarded.errors[0].error.node == button.node().id());
  CHECK(hasCause(primary.errors[0].error, TreeError::STALE_NODE));
  CHECK(hasCause(forwarded.errors[0].error, TreeError::STALE_NODE));
  CHECK(following == 0);
}

TEST_CASE("preparation prunes raw tree retirement and returns the click budget") {
  ManagedOptions options;

  options.maxClickHandlers = 1;

  Fixture fixture(options);
  ui::Ref<Button> first, second;

  take(ui::mount(*fixture.scene, ui::hbox(ui::button(first, "One"), ui::button(second, "Two"))));
  fixture.click(first, [] {
  });
  take(fixture.app->tree().remove(first.node()));
  take(fixture.scene->prepare(*fixture.app, {30, 4}));
  fixture.click(second, [] {
  });
  take(fixture.app->router().invoke(second.node(), ActionEvent{Action::ACTIVATE}));
}
