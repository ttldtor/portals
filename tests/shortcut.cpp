// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/replay/terminal.hpp"
#include <portals/shortcut.hpp>
#include <portals/ui.hpp>

#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};

class AllocationFailure {
public:
  explicit AllocationFailure(std::ptrdiff_t index) {
    allocationsBeforeFailure = index;
  }

  ~AllocationFailure() {
    disable();
  }

  void disable() noexcept {
    allocationsBeforeFailure = -1;
  }
};

constexpr std::ptrdiff_t MAX_FAILURE_ATTEMPTS = 128;

template <class Source>
concept LegacySelectionBehavior = requires(Source source) {
  portals::ui::onKey(
    source,
    [] {
    },
    portals::ShortcutBehavior{.repeat = true});
};

static_assert(!LegacySelectionBehavior<portals::KeySelection>);
static_assert(std::is_nothrow_copy_constructible_v<portals::KeySelection>);
static_assert(std::is_nothrow_move_constructible_v<portals::KeySelection>);
} // namespace

TEST_CASE("explicit shortcut phases preserve native action and duplicate alternatives invoke once") {
  using namespace portals;

  example::ReplayTerminal terminal;
  auto application = Application::create({}, terminal.clock());

  REQUIRE(application);

  auto owner = ManagedScene::create(**application);

  REQUIRE(owner);

  auto &scene = **owner;
  int presses{};
  int repeats{};
  int releases{};

  REQUIRE(scene.onKey(ui::press("W"), [&] {
    ++presses;
  }));
  REQUIRE(scene.onKey(ui::repeat(key::W), [&](const KeyEvent &event) {
    CHECK(event.action == KeyAction::REPEAT);
    ++repeats;
  }));
  REQUIRE(scene.onKey(ui::release({key::W, key::W}), [&] {
    ++releases;
  }));

  for (auto phase : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
    REQUIRE(scene.input(**application, KeyEvent{InputKey::CHARACTER, U'w', {}, phase}));
  }

  CHECK(presses == 1);
  CHECK(repeats == 1);
  CHECK(releases == 1);
  REQUIRE(scene.close());
}

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

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

using namespace portals;

TEST_CASE("multi-stroke shortcut strings execute through the managed input slot") {
  auto shortcut = parseShortcut("Ctrl+K+K");

  REQUIRE(shortcut);

  example::ReplayTerminal terminal;
  auto application = Application::create({}, terminal.clock());

  REQUIRE(application);

  auto owner = ManagedScene::create(**application);

  REQUIRE(owner);

  int calls{};
  auto &scene = **owner;

  REQUIRE(scene.onKey(*shortcut, [&] {
    ++calls;
  }));

  const KeyEvent stroke{InputKey::CHARACTER, U'K', {.control = true}};
  auto first = scene.input(**application, stroke);

  REQUIRE(first);
  CHECK(*first);
  CHECK(calls == 0);

  auto second = scene.input(**application, stroke);

  REQUIRE(second);
  CHECK(*second);
  CHECK(calls == 1);
  REQUIRE(scene.close());
}

static_assert((key::ALT + key::Q).scalar == U'Q');
static_assert((key::CTRL + key::ALT + key::Q).modifiers.control);
static_assert(!std::is_nothrow_default_constructible_v<ShortcutSequence>);
static_assert(!std::is_nothrow_move_constructible_v<ShortcutSequence>);

TEST_CASE("shortcut parser owns normalized alternatives and preserves typed offsets") {
  std::string source = " f10 | aLt + q | F10 ";
  auto shortcut = parseShortcut(source);

  REQUIRE(shortcut);
  source.assign(source.size(), '?');
  CHECK(shortcut->alternatives.size() == 3);
  CHECK(matchShortcut(*shortcut, KeyEvent{.key = InputKey::F10}));
  CHECK(matchShortcut(*shortcut, TextInputEvent{U'q', {.alt = true}}));
  CHECK(matchShortcut(*shortcut, TextInputEvent{U'Q', {.alt = true}}));
  CHECK_FALSE(matchShortcut(*shortcut, TextInputEvent{U'Q', {.shift = true, .alt = true}}));
  CHECK_FALSE(matchShortcut(*shortcut, TextInputEvent{U'q'}));
  CHECK_FALSE(matchShortcut(*shortcut, TextInputEvent{U'\u00e9', {.alt = true}}));
  CHECK_FALSE(matchShortcut(*shortcut, TextInputEvent{U'q', {.alt = true, .control = true}}));
  CHECK_FALSE(matchShortcut(*shortcut, KeyEvent{.key = InputKey::F10, .action = KeyAction::REPEAT}));
  CHECK(matchShortcut(*shortcut, KeyEvent{.key = InputKey::F10, .action = KeyAction::REPEAT}, {.repeat = true}));
  CHECK_FALSE(matchShortcut(*shortcut, KeyEvent{.key = InputKey::F10, .action = KeyAction::RELEASE}, {.repeat = true}));

  const auto failure = parseShortcut("F10 | Alt + Nope");

  REQUIRE_FALSE(failure);
  CHECK(failure.error().cause == ShortcutErrorKind::UNKNOWN_KEY);
  CHECK(failure.error().offset == 12);
}

TEST_CASE("shortcut parser rejects malformed input and obeys every configured budget") {
  struct Invalid {
    std::string_view source;
    ShortcutErrorKind cause;
  };

  const Invalid cases[] = {{"", ShortcutErrorKind::EMPTY_ALTERNATIVE},
                           {"F10 | ", ShortcutErrorKind::EMPTY_ALTERNATIVE},
                           {"| F10", ShortcutErrorKind::EMPTY_ALTERNATIVE},
                           {"Alt + + Q", ShortcutErrorKind::EMPTY_TOKEN},
                           {"Alt +", ShortcutErrorKind::EMPTY_TOKEN},
                           {"Alt", ShortcutErrorKind::EXPECTED_KEY},
                           {"Alt + Alt + Q", ShortcutErrorKind::DUPLICATE_MODIFIER},
                           {"LAlt + Q", ShortcutErrorKind::UNKNOWN_KEY},
                           {"AltGr + Q", ShortcutErrorKind::UNKNOWN_KEY},
                           {"\xc3\xa9", ShortcutErrorKind::UNKNOWN_KEY},
                           {"Ctrl+K, Escape", ShortcutErrorKind::RESERVED_CANCEL},
                           {"Ctrl+K | Ctrl+K,K", ShortcutErrorKind::AMBIGUOUS_PREFIX}};

  for (const auto &invalid : cases) {
    CAPTURE(invalid.source);
    const auto parsed = parseShortcut(invalid.source);

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().cause == invalid.cause);
  }

  CHECK(parseShortcut("F10", {.maxSourceBytes = 3}));
  CHECK(parseShortcut("F10", {.maxAlternatives = 1, .maxStrokes = 1}));
  CHECK(parseShortcut("F10 ", {.maxSourceBytes = 3}).error().cause == ShortcutErrorKind::SOURCE_LIMIT);
  CHECK(parseShortcut("F10|Q", {.maxAlternatives = 1}).error().cause == ShortcutErrorKind::ALTERNATIVE_LIMIT);
  CHECK(parseShortcut("Q+W", {.maxStrokes = 1}).error().cause == ShortcutErrorKind::STROKE_LIMIT);
  CHECK(parseShortcut("Q", {.maxSourceBytes = 0}).error().cause == ShortcutErrorKind::INVALID_OPTIONS);
  CHECK(parseShortcut("Q", {.maxAlternatives = 0}).error().cause == ShortcutErrorKind::INVALID_OPTIONS);
  CHECK(parseShortcut("Q", {.maxStrokes = 0}).error().cause == ShortcutErrorKind::INVALID_OPTIONS);
}

TEST_CASE("shortcut facade uses decoded Alt text once and keeps plain Unicode input routing") {
  example::ReplayTerminal terminal({{10, "q\xc3\xa9", {}}, {20, "\x1bq", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> editor;
  int oldCalls{};
  int newCalls{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::column(ui::lineEdit(editor, "")), options);

  scene.onKey(key::ALT + key::Q, [&] {
    ++oldCalls;
  });
  scene.onKey({key::F10, key::ALT + key::Q, key::ALT + key::Q}, [&](const KeyEvent &event) {
    CHECK(event.key == InputKey::CHARACTER);
    CHECK(event.scalar == U'q');
    CHECK(event.modifiers.alt);
    const auto text = editor.text().get();

    REQUIRE(text);
    CHECK(*text == "q\xc3\xa9");
    ++newCalls;
    REQUIRE(scene.quit());
  });

  REQUIRE(scene.checked());
  CHECK(scene.runApp(terminal) == 0);
  CHECK(oldCalls == 0);
  CHECK(newCalls == 1);
  CHECK(terminal.closed);
}

TEST_CASE("managed shortcuts register atomically under validation budget and allocation failures") {
  example::ReplayTerminal terminal;
  auto application = Application::create({}, terminal.clock());

  REQUIRE(application);

  auto owner = ManagedScene::create(**application, ManagedOptions{.maxKeyHandlers = 2});

  REQUIRE(owner);

  auto &scene = **owner;
  int prior{};
  int newer{};
  const InputEvent event = KeyEvent{.key = InputKey::F10};

  REQUIRE(scene.onKey(ui::key(InputKey::F10), [&] {
    ++prior;
  }));

  auto invalid = scene.onKey(Shortcut{}, [&] {
    ++newer;
  });

  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().shortcut);
  CHECK(invalid.error().shortcut->cause == ShortcutErrorKind::EMPTY_ALTERNATIVE);
  REQUIRE(scene.input(**application, event));
  CHECK(prior == 1);

  // Fault only the registration body, never the construction of its owning argument or the test reporter.
  auto normalized = parseShortcut("F10|F10");

  REQUIRE(normalized);
  AllocationFailure fault(0);

  auto failed = scene.onKey(std::move(*normalized), [&] {
    ++newer;
  });

  fault.disable();
  REQUIRE_FALSE(failed);
  CHECK(std::get<TreeError>(failed.error().cause) == TreeError::NO_MEMORY);
  REQUIRE(scene.input(**application, event));
  CHECK(prior == 2);
  CHECK(newer == 0);

  normalized = parseShortcut("F10|F10");

  REQUIRE(normalized);
  REQUIRE(scene.onKey(std::move(*normalized), [&] {
    ++newer;
  }));
  REQUIRE(scene.input(**application, event));
  CHECK(prior == 2);
  CHECK(newer == 1);

  normalized = parseShortcut("F10");

  REQUIRE(normalized);
  auto exhausted = scene.onKey(std::move(*normalized), [&] {
    ++prior;
  });

  REQUIRE_FALSE(exhausted);
  CHECK(std::get<TreeError>(exhausted.error().cause) == TreeError::NODE_LIMIT);
  REQUIRE(scene.input(**application, event));
  CHECK(newer == 2);
  REQUIRE(scene.close());
}

TEST_CASE("shortcut repeats are explicit and leave legacy repeat admission intact") {
  example::ReplayTerminal terminal;
  auto application = Application::create({}, terminal.clock());

  REQUIRE(application);

  auto owner = ManagedScene::create(**application);

  REQUIRE(owner);

  auto &scene = **owner;
  int legacy{};
  int command{};
  const InputEvent repeat = KeyEvent{.key = InputKey::F10, .action = KeyAction::REPEAT};
  auto normalized = parseShortcut("F10");

  REQUIRE(normalized);
  REQUIRE(scene.onKey(ui::key(InputKey::F10), [&] {
    ++legacy;
  }));
  REQUIRE(scene.onKey(*normalized, [&] {
    ++command;
  }));
  REQUIRE(scene.input(**application, repeat));
  CHECK(legacy == 1);
  CHECK(command == 0);
  REQUIRE(scene.onKey(*normalized,
                      [&] {
                        ++command;
                      },
                      {.repeat = true}));
  REQUIRE(scene.input(**application, repeat));
  CHECK(legacy == 1);
  CHECK(command == 1);
  REQUIRE(scene.close());
}

TEST_CASE("every allocation failure before shortcut publication preserves the previous binding") {
  for (bool moveArgument : {false, true}) {
    CAPTURE(moveArgument);
    bool reachedSuccess{};
    int failedAttempts{};

    for (std::ptrdiff_t failureIndex{}; failureIndex < MAX_FAILURE_ATTEMPTS; ++failureIndex) {
      example::ReplayTerminal terminal;
      auto application = Application::create({}, terminal.clock());

      REQUIRE(application);

      auto owner = ManagedScene::create(**application, ManagedOptions{.maxKeyHandlers = 2});

      REQUIRE(owner);

      auto &scene = **owner;
      int oldCalls{};
      int newCalls{};
      auto shortcut = parseShortcut("F10");

      REQUIRE(shortcut);
      REQUIRE(scene.onKey(ui::key(InputKey::F10), [&] {
        ++oldCalls;
      }));
      AllocationFailure fault(failureIndex);

      const auto registered = [&] {
        const auto callback = [&] {
          ++newCalls;
        };

        return moveArgument ? scene.onKey(std::move(*shortcut), callback) : scene.onKey(*shortcut, callback);
      }();

      fault.disable();

      if (registered) {
        reachedSuccess = true;
      } else {
        ++failedAttempts;
        CHECK(std::get<TreeError>(registered.error().cause) == TreeError::NO_MEMORY);
      }

      const auto consumed = scene.input(**application, KeyEvent{.key = InputKey::F10});

      REQUIRE(consumed);
      CHECK(*consumed);
      CHECK(oldCalls == (registered ? 0 : 1));
      CHECK(newCalls == (registered ? 1 : 0));
      REQUIRE(scene.close());

      if (reachedSuccess) {
        break;
      }
    }

    CHECK(reachedSuccess);
    CHECK(failedAttempts >= 2);
  }
}

TEST_CASE("shortcut parser allocation failure is a source-independent typed value") {
  AllocationFailure fault(0);

  const auto parsed = parseShortcut("Alt + Q");

  fault.disable();
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error().cause == ShortcutErrorKind::NO_MEMORY);
}

TEST_CASE("explicit action sets keep exact admission for every owning source shape") {
  for (int sourceKind : {0, 1, 2, 3}) {
    for (int selectionKind : {0, 1, 2, 3, 4}) {
      CAPTURE(sourceKind);
      CAPTURE(selectionKind);
      example::ReplayTerminal terminal;
      auto application = Application::create({}, terminal.clock());

      REQUIRE(application);

      auto owner = ManagedScene::create(**application);

      REQUIRE(owner);

      auto &scene = **owner;
      auto selected = [&]<class Source>(Source &&source) {
        switch (selectionKind) {
        case 0:
          return ui::press(std::forward<Source>(source));
        case 1:
          return ui::repeat(std::forward<Source>(source));
        case 2:
          return ui::release(std::forward<Source>(source));
        case 3:
          return ui::pressRepeat(std::forward<Source>(source));
        default:
          return ui::keyActions(std::forward<Source>(source),
                                {KeyAction::PRESS, KeyAction::RELEASE, KeyAction::RELEASE});
        }
      };
      auto selection = [&] {
        switch (sourceKind) {
        case 0:
          return selected(std::string("Alt + W"));
        case 1:
          return selected(key::ALT + key::W);
        case 2:
          return selected(std::initializer_list<key::Chord>{key::ALT + key::W, key::ALT + key::W});
        default:
          return selected(Shortcut{key::ALT + key::W});
        }
      }();
      auto alias = selection;
      int calls{};
      std::vector<KeyAction> phases;

      REQUIRE(scene.onKey(std::move(selection), [&](const KeyEvent &) {
        ++calls;
      }));
      REQUIRE(scene.onKey(alias, [&](const KeyEvent &event) {
        phases.push_back(event.action);
      }));

      for (auto phase : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
        const bool expected = selectionKind == 0   ? phase == KeyAction::PRESS
                              : selectionKind == 1 ? phase == KeyAction::REPEAT
                              : selectionKind == 2 ? phase == KeyAction::RELEASE
                              : selectionKind == 3 ? phase != KeyAction::RELEASE
                                                   : phase != KeyAction::REPEAT;
        const auto result = scene.input(**application, KeyEvent{InputKey::CHARACTER, U'w', {.alt = true}, phase});

        REQUIRE(result);
        CHECK(*result == expected);
      }

      CHECK(calls == 0);
      CHECK(phases.size() == (selectionKind >= 3 ? 2u : 1u));

      const auto extraModifier =
        scene.input(**application, KeyEvent{InputKey::CHARACTER, U'W', {.shift = true, .alt = true}, KeyAction::PRESS});

      REQUIRE(extraModifier);
      CHECK_FALSE(*extraModifier);

      const auto text = scene.input(**application, TextInputEvent{U'w', {.alt = true}});

      REQUIRE(text);
      CHECK(*text == (selectionKind == 0 || selectionKind >= 3));
      REQUIRE(scene.close());
    }
  }
}

TEST_CASE("selection helper errors retain typed offsets and onKey registration origin") {
  for (int failureKind : {0, 1, 2, 3}) {
    auto selection = [&] {
      switch (failureKind) {
      case 0:
        return ui::keyActions("W", {});
      case 1:
        return ui::keyActions(key::W, {KeyAction::PRESS, static_cast<KeyAction>(99)});
      case 2:
        return ui::release("Alt + Nope");
      default:
        return ui::repeat({key::UP, key::DOWN}, {.maxAlternatives = 1});
      }
    }();
    auto scene = ui::scene(ui::label("ready"));
    const auto origin = std::source_location::current();

    scene | ui::onKey(
              selection,
              [] {
              },
              origin);

    REQUIRE_FALSE(scene.checked());

    const auto diagnostics = scene.diagnostics();

    REQUIRE(diagnostics);
    REQUIRE(diagnostics->errors.size() == 1);
    const auto &failure = diagnostics->errors.front();

    CHECK(failure.origin.line() == origin.line());
    REQUIRE(failure.error.shortcut);
    CHECK(failure.error.shortcut->cause == (failureKind < 2    ? ShortcutErrorKind::INVALID_ACTION_SET
                                            : failureKind == 2 ? ShortcutErrorKind::UNKNOWN_KEY
                                                               : ShortcutErrorKind::ALTERNATIVE_LIMIT));
    CHECK(failure.error.shortcut->offset == (failureKind == 0 ? 0u : failureKind == 2 ? 6u : 1u));
  }
}

TEST_CASE("legacy brace behavior remains unambiguous and arbitrary phase subsets stay exact") {
  auto shortcut = parseShortcut("W");

  REQUIRE(shortcut);
  CHECK(matchShortcut(*shortcut, KeyEvent{InputKey::CHARACTER, U'w'}, {}));

  example::ReplayTerminal terminal;
  auto application = Application::create({}, terminal.clock());

  REQUIRE(application);

  auto owner = ManagedScene::create(**application);

  REQUIRE(owner);

  auto &scene = **owner;
  int calls{};

  REQUIRE(scene.onKey(ui::keyActions("W", {KeyAction::REPEAT, KeyAction::RELEASE}), [&] {
    ++calls;
  }));
  const auto press = scene.input(**application, KeyEvent{InputKey::CHARACTER, U'w'});

  REQUIRE(press);
  CHECK_FALSE(*press);
  REQUIRE(scene.input(**application, KeyEvent{InputKey::CHARACTER, U'w', {}, KeyAction::REPEAT}));
  REQUIRE(scene.input(**application, KeyEvent{InputKey::CHARACTER, U'w', {}, KeyAction::RELEASE}));
  CHECK(calls == 2);
  REQUIRE(scene.onKey(ui::keyActions("W", {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}), [&] {
    ++calls;
  }));

  for (auto phase : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
    REQUIRE(scene.input(**application, KeyEvent{InputKey::CHARACTER, U'w', {}, phase}));
  }

  CHECK(calls == 5);
  REQUIRE(scene.close());
}

TEST_CASE("selected registration catches library callback transfers after stage construction") {
  struct ThrowingMove {
    bool *fail;

    explicit ThrowingMove(bool &flag) : fail(&flag) {
    }

    ThrowingMove(const ThrowingMove &) = default;

    ThrowingMove(ThrowingMove &&other) : fail(other.fail) {
      if (*fail) {
        throw std::runtime_error("callback transfer");
      }
    }

    void operator()() const {
    }
  };

  for (bool pipe : {false, true}) {
    bool fail{};
    ThrowingMove callback(fail);
    auto selection = ui::release(key::F10);
    auto stage = ui::onKey(selection, callback);
    auto scene = ui::scene(ui::label("ready"));

    fail = true;

    if (pipe) {
      scene | std::move(stage);
    } else {
      scene.onKey(selection, callback);
    }

    const auto ready = scene.checked();

    REQUIRE_FALSE(ready);
    CHECK(std::get<TreeError>(ready.error().cause) == TreeError::CALLBACK_FAILED);
  }
}

TEST_CASE("explicit stages own temporary input and legacy bytes never synthesize repeat or release") {
  example::ReplayTerminal terminal({{10, "ww", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  int presses{};
  int repeats{};
  int releases{};

  options.clock = terminal.clock();

  std::string source = "W";
  auto selection = ui::pressRepeat(source);
  auto stage = ui::onKey(selection, [&] {
    ++presses;
  });

  source.assign("invalid");

  auto scene = ui::scene(ui::label("ready"), options) | std::move(stage);

  scene.onKey(ui::repeat({key::W}), [&] {
    ++repeats;
  });
  std::move(scene).onKey(ui::release(std::string("W")), [&] {
    ++releases;
  });

  REQUIRE(scene.checked());
  CHECK(scene.runApp(terminal) == 0);
  CHECK(presses == 2);
  CHECK(repeats == 0);
  CHECK(releases == 0);
  CHECK(terminal.closed);
}

TEST_CASE("selected helper and registration allocation failures keep prior handlers atomic") {
  for (int sourceKind : {0, 1, 2, 3, 4, 5}) {
    bool reachedSuccess{};
    int failures{};

    for (std::ptrdiff_t failureIndex{}; failureIndex < MAX_FAILURE_ATTEMPTS; ++failureIndex) {
      example::ReplayTerminal terminal;
      auto application = Application::create({}, terminal.clock());

      REQUIRE(application);

      auto owner = ManagedScene::create(**application, ManagedOptions{.maxKeyHandlers = 2});

      REQUIRE(owner);

      auto &scene = **owner;
      int prior{};
      int selected{};
      auto ready = ui::release(key::F10);
      Shortcut owning{key::F10};

      REQUIRE(scene.onKey(ui::key(InputKey::F10), [&] {
        ++prior;
      }));

      AllocationFailure fault(failureIndex);
      auto selection = [&] {
        switch (sourceKind) {
        case 0:
          return ui::release("F10");
        case 1:
          return ui::release(key::F10);
        case 2:
          return ui::release({key::F10});
        case 3:
          return ui::release(owning);
        case 4:
          return ui::release(std::move(owning));
        default:
          return ready;
        }
      }();

      if (sourceKind != 5) {
        fault.disable();
      }

      const auto registered = scene.onKey(selection, [&] {
        ++selected;
      });

      fault.disable();

      if (registered) {
        reachedSuccess = true;
      } else {
        ++failures;

        if (sourceKind == 5) {
          CHECK(std::get<TreeError>(registered.error().cause) == TreeError::NO_MEMORY);
        } else {
          REQUIRE(registered.error().shortcut);
          CHECK(registered.error().shortcut->cause == ShortcutErrorKind::NO_MEMORY);
        }
      }

      REQUIRE(scene.input(**application, KeyEvent{.key = InputKey::F10}));
      CHECK(prior == 1);

      const auto release = scene.input(**application, KeyEvent{.key = InputKey::F10, .action = KeyAction::RELEASE});

      REQUIRE(release);
      CHECK(*release == static_cast<bool>(registered));
      CHECK(selected == (registered ? 1 : 0));

      int recovered{};
      int overflowCalls{};

      if (!registered) {
        REQUIRE(scene.onKey(ready, [&] {
          ++recovered;
        }));
      }

      const auto overflow = scene.onKey(ui::press(key::F10), [&] {
        ++overflowCalls;
      });

      REQUIRE_FALSE(overflow);
      CHECK(std::get<TreeError>(overflow.error().cause) == TreeError::NODE_LIMIT);
      REQUIRE(scene.input(**application, KeyEvent{.key = InputKey::F10}));
      CHECK(prior == 2);
      REQUIRE(scene.input(**application, KeyEvent{.key = InputKey::F10, .action = KeyAction::RELEASE}));
      CHECK(selected == (registered ? 2 : 0));
      CHECK(recovered == (registered ? 0 : 1));
      CHECK(overflowCalls == 0);
      REQUIRE(scene.close());

      if (reachedSuccess) {
        break;
      }
    }

    CHECK(reachedSuccess);
    CHECK(failures > 0);
  }
}

TEST_CASE("parser and normalizer return typed OOM across every owning construction and transfer") {
  for (int sourceKind : {0, 1, 2, 3}) {
    bool reachedSuccess{};
    int failures{};

    for (std::ptrdiff_t failureIndex{}; failureIndex < MAX_FAILURE_ATTEMPTS; ++failureIndex) {
      CAPTURE(sourceKind);
      CAPTURE(failureIndex);
      Shortcut source{key::F10, key::ALT + key::Q, key::SHIFT + key::F2};
      AllocationFailure fault(failureIndex);
      const auto parsed = [&]() {
        switch (sourceKind) {
        case 0:
          return parseShortcut("F10 | Alt+Q | Shift+F2");
        case 1:
          return detail::prepareShortcut({key::F10, key::ALT + key::Q, key::SHIFT + key::F2});
        case 2:
          return detail::prepareShortcut(source);
        default:
          return detail::prepareShortcut(std::move(source));
        }
      }();

      fault.disable();

      if (parsed) {
        reachedSuccess = true;
        CHECK(parsed->alternatives.size() == 3);

        break;
      }

      ++failures;
      CHECK(parsed.error().cause == ShortcutErrorKind::NO_MEMORY);
    }

    CHECK(reachedSuccess);

    if (sourceKind != 3) {
      CHECK(failures >= 1);
    }
  }
}

TEST_CASE("shortcut callback captures survive replacement and close until invocation returns") {
  for (int mode : {0, 1, 2, 3}) {
    const bool close = (mode & 1) != 0;
    const bool selected = (mode & 2) != 0;
    example::ReplayTerminal terminal;
    auto application = Application::create({}, terminal.clock());

    REQUIRE(application);

    auto owner = ManagedScene::create(**application);

    REQUIRE(owner);

    auto &scene = **owner;
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> observer = capture;
    auto normalized = parseShortcut("F10");

    REQUIRE(normalized);
    auto callback = [&, capture] {
      if (close) {
        REQUIRE(scene.close());
      } else {
        REQUIRE(scene.onInput({}));
      }

      CHECK_FALSE(observer.expired());
    };

    REQUIRE((selected ? scene.onKey(ui::release(key::F10), std::move(callback))
                      : scene.onKey(*normalized, std::move(callback))));
    capture.reset();

    auto consumed = scene.input(
      **application, KeyEvent{.key = InputKey::F10, .action = selected ? KeyAction::RELEASE : KeyAction::PRESS});

    REQUIRE(consumed);
    CHECK(*consumed);
    CHECK(observer.expired());
    REQUIRE(scene.close());
  }
}

TEST_CASE("runtime shortcut pipe owns its source before deferred scene preparation") {
  example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  int calls{};
  std::string source = "F10 | Alt + Q";

  options.clock = terminal.clock();

  auto stage = ui::onKey(source, [&] {
    ++calls;
  });

  source.assign(source.size(), '?');

  auto scene = ui::scene(ui::label("ready"), options) | std::move(stage);

  REQUIRE(scene.checked());
  CHECK(scene.runApp(terminal) == 0);
  CHECK(calls == 1);
  CHECK(terminal.closed);
}

TEST_CASE("runtime shortcut syntax errors retain source location through bounded diagnostics") {
  auto scene = ui::scene(ui::label("ready"));
  const auto origin = std::source_location::current();

  scene.onKey(
    "Alt + Nope",
    [] {
    },
    origin);

  REQUIRE_FALSE(scene.checked());
  const auto diagnostics = scene.diagnostics();

  REQUIRE(diagnostics);
  REQUIRE_FALSE(diagnostics->errors.empty());
  CHECK(diagnostics->errors[0].origin.line() == origin.line());
  REQUIRE(diagnostics->errors[0].error.shortcut);
  CHECK(diagnostics->errors[0].error.shortcut->cause == ShortcutErrorKind::UNKNOWN_KEY);
  CHECK(diagnostics->errors[0].error.shortcut->offset == 6);
}

TEST_CASE("shortcut runtime failures retain registration origin and notify once") {
  for (int mode : {0, 1, 2, 3}) {
    const bool pipe = (mode & 1) != 0;
    const bool selected = (mode & 2) != 0;
    for (int failureKind : {0, 1, 2}) {
      CAPTURE(pipe);
      CAPTURE(failureKind);
      example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 100, {30, 4});
      ui::SceneOptions options;
      int notices{};

      options.clock = terminal.clock();

      auto scene = ui::scene(ui::label("ready"), options);
      const auto origin = std::source_location::current();

      scene.onError([&](const ManagedDiagnostic &diagnostic) {
        ++notices;
        CHECK(diagnostic.origin.line() == origin.line());
        CHECK(std::string_view(diagnostic.origin.file_name()) == origin.file_name());
        const auto expectedCause = failureKind == 0   ? TreeError::NODE_LIMIT
                                   : failureKind == 1 ? TreeError::CALLBACK_FAILED
                                                      : TreeError::NO_MEMORY;

        CHECK(std::get<TreeError>(diagnostic.error.cause) == expectedCause);
      });
      auto callback = [failureKind]() -> std::expected<void, TreeError> {
        if (failureKind == 1) {
          throw std::runtime_error("shortcut action failure");
        }

        if (failureKind == 2) {
          throw std::bad_alloc();
        }

        return std::unexpected(TreeError::NODE_LIMIT);
      };

      if (selected && pipe) {
        scene | ui::onKey(ui::press("F10"), callback, origin);
      } else if (selected) {
        scene.onKey(ui::press("F10"), callback, origin);
      } else if (pipe) {
        scene | ui::onKey("F10", callback, origin);
      } else {
        scene.onKey("F10", callback, origin);
      }

      REQUIRE(scene.checked());
      CHECK_FALSE(scene.run(terminal));
      CHECK(notices == 1);
      CHECK(terminal.closed);
    }
  }
}

TEST_CASE("nonmatching shortcuts retain fallback error provenance") {
  example::ReplayTerminal terminal;
  auto application = Application::create({}, terminal.clock());

  REQUIRE(application);

  auto owner = ManagedScene::create(**application);

  REQUIRE(owner);

  const auto fallbackOrigin = std::source_location::current();
  auto &scene = **owner;
  int notices{};

  REQUIRE(scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notices;
    CHECK(diagnostic.origin.line() == fallbackOrigin.line());
  }));
  REQUIRE(scene.onInput([fallbackOrigin](const InputEvent &) -> std::expected<bool, ManagedError> {
    ManagedError error{ManagedPhase::INPUT, {}, TreeError::NODE_LIMIT, {}, {}};

    error.registrationOrigin = fallbackOrigin;

    return std::unexpected(std::move(error));
  }));

  auto shortcut = parseShortcut("F10");

  REQUIRE(shortcut);
  REQUIRE(scene.onKey(*shortcut, [] {
  }));
  CHECK_FALSE(scene.input(**application, KeyEvent{.key = InputKey::F2}));
  CHECK(notices == 1);
  REQUIRE(scene.close());
}

TEST_CASE("new shortcut bindings preserve already reported editor command deduplication") {
  bool selected{};

  SUBCASE("legacy") {
  }
  SUBCASE("explicit selection") {
    selected = true;
  }

  example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  int notices{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::lineEdit(field, "ready"), options);

  scene.onError([&](const ManagedDiagnostic &) {
    ++notices;
  });
  auto callback = [&] {
    auto managed = scene.managed();

    REQUIRE(managed);
    REQUIRE(managed->get().remove(field.node()));

    return field.undo();
  };

  if (selected) {
    scene.onKey(ui::press("F10"), callback);
  } else {
    scene.onKey("F10", callback);
  }

  REQUIRE(scene.run(terminal));
  CHECK(notices == 1);
  const auto diagnostics = scene.diagnostics();

  REQUIRE(diagnostics);
  CHECK(diagnostics->errors.size() == 1);
}

TEST_CASE("facade method and pipe catch every shortcut ownership allocation failure") {
  for (int mode : {0, 1, 2, 3}) {
    const bool pipe = (mode & 1) != 0;
    const bool selected = (mode & 2) != 0;
    bool reachedSuccess{};
    int failures{};

    for (std::ptrdiff_t failureIndex{}; failureIndex < MAX_FAILURE_ATTEMPTS; ++failureIndex) {
      CAPTURE(pipe);
      CAPTURE(failureIndex);
      auto scene = ui::scene(ui::label("ready"));
      const auto origin = std::source_location::current();

      {
        AllocationFailure fault(failureIndex);

        const auto registerSource = [&]<class Source>(Source &&source) {
          if (pipe) {
            auto stage = ui::onKey(
              std::forward<Source>(source),
              [] {
              },
              origin);

            scene | std::move(stage);
          } else {
            scene.onKey(
              std::forward<Source>(source),
              [] {
              },
              origin);
          }
        };

        if (selected) {
          registerSource(ui::pressRepeat("F10 | Alt+Q | Ctrl+K,K"));
        } else {
          registerSource("F10 | Alt+Q | Ctrl+K,K");
        }
      }

      const auto ready = scene.checked();

      if (ready) {
        reachedSuccess = true;

        break;
      }

      ++failures;
      CHECK(std::get<TreeError>(ready.error().cause) == TreeError::NO_MEMORY);
    }

    CHECK(reachedSuccess);
    CHECK(failures >= 1);
  }
}

TEST_CASE("explicit and legacy shortcuts share newest matching priority across native phases") {
  for (bool rawLegacy : {false, true}) {
    for (int selectionKind : {0, 1, 2}) {
      for (bool selectedNewer : {false, true}) {
        CAPTURE(rawLegacy);
        CAPTURE(selectionKind);
        CAPTURE(selectedNewer);
        example::ReplayTerminal terminal;
        auto application = Application::create({}, terminal.clock());

        REQUIRE(application);

        auto owner = ManagedScene::create(**application);

        REQUIRE(owner);

        auto &scene = **owner;
        auto normalized = parseShortcut("F10");
        auto selection = selectionKind == 0   ? ui::pressRepeat(key::F10)
                         : selectionKind == 1 ? ui::repeat(key::F10)
                                              : ui::release(key::F10);
        int legacyCalls{};
        int selectedCalls{};
        std::optional<KeyAction> lastAction;
        auto legacyCallback = [&](const KeyEvent &event) {
          ++legacyCalls;
          lastAction = event.action;
        };
        const auto registerLegacy = [&] {
          return rawLegacy ? scene.onKey(ui::key(InputKey::F10), legacyCallback)
                           : scene.onKey(*normalized, legacyCallback, ShortcutBehavior{.repeat = true});
        };
        const auto registerSelected = [&] {
          return scene.onKey(selection, [&](const KeyEvent &event) {
            ++selectedCalls;
            lastAction = event.action;
          });
        };

        REQUIRE(normalized);

        if (selectedNewer) {
          REQUIRE(registerLegacy());
          REQUIRE(registerSelected());
        } else {
          REQUIRE(registerSelected());
          REQUIRE(registerLegacy());
        }

        for (auto phase : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
          CAPTURE(phase);
          const bool selectedAdmits = selectionKind == 0   ? phase != KeyAction::RELEASE
                                      : selectionKind == 1 ? phase == KeyAction::REPEAT
                                                           : phase == KeyAction::RELEASE;
          const bool legacyAdmits = phase != KeyAction::RELEASE;
          const bool selectedWins = selectedAdmits && (selectedNewer || !legacyAdmits);
          const bool legacyWins = legacyAdmits && (!selectedNewer || !selectedAdmits);
          const auto previousLegacy = legacyCalls;
          const auto previousSelected = selectedCalls;

          lastAction.reset();

          const auto consumed = scene.input(**application, KeyEvent{.key = InputKey::F10, .action = phase});

          REQUIRE(consumed);
          CHECK(*consumed == (selectedAdmits || legacyAdmits));
          CHECK(legacyCalls - previousLegacy == (legacyWins ? 1 : 0));
          CHECK(selectedCalls - previousSelected == (selectedWins ? 1 : 0));
          CHECK(lastAction.has_value() == (selectedAdmits || legacyAdmits));

          if (lastAction) {
            CHECK(*lastAction == phase);
          }
        }

        REQUIRE(scene.close());
      }
    }
  }
}

TEST_CASE("deferred selected callback transfer retains registration origin and reports once") {
  struct ThrowingMove {
    bool *fail;

    explicit ThrowingMove(bool &flag) : fail(&flag) {
    }

    ThrowingMove(const ThrowingMove &) = default;

    ThrowingMove(ThrowingMove &&other) : fail(other.fail) {
      if (*fail) {
        throw std::runtime_error("late callback transfer");
      }
    }

    void operator()() const {
    }
  };

  bool fail{};
  ThrowingMove callback(fail);
  auto selection = ui::release(key::F10);
  const auto origin = std::source_location::current();
  auto stage = ui::onKey(selection, callback, origin);
  auto scene = ui::scene(ui::label("ready")) | std::move(stage);

  REQUIRE(scene.checked());

  fail = true;

  REQUIRE_NOTHROW(scene.prepare());

  std::optional<decltype(scene.checked())> ready;

  REQUIRE_NOTHROW(ready.emplace(scene.checked()));
  REQUIRE(ready);
  REQUIRE_FALSE(*ready);
  CHECK(std::get<TreeError>(ready->error().cause) == TreeError::CALLBACK_FAILED);

  const auto diagnostics = scene.diagnostics();

  REQUIRE(diagnostics);
  REQUIRE(diagnostics->errors.size() == 1);
  CHECK(diagnostics->errors.front().origin.line() == origin.line());
  CHECK(std::string_view(diagnostics->errors.front().origin.file_name()) == origin.file_name());
  CHECK(std::get<TreeError>(diagnostics->errors.front().error.cause) == TreeError::CALLBACK_FAILED);
  CHECK_NOTHROW((void)scene.checked());

  const auto repeated = scene.diagnostics();

  REQUIRE(repeated);
  CHECK(repeated->errors.size() == 1);
}

namespace {
struct SequenceFixture {
  SchedulerTime now{};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit SequenceFixture(ManagedOptions options = {}, ApplicationOptions applicationOptions = {}) {
    auto application = Application::create(applicationOptions, {[](void *context) noexcept {
                                                                  return static_cast<SequenceFixture *>(context)->now;
                                                                },
                                                                this});

    REQUIRE(application);

    app = std::move(*application);

    auto owner = ManagedScene::create(*app, options);

    REQUIRE(owner);

    scene = std::move(*owner);
  }

  template <class Callback>
  void bind(std::string_view source, Callback callback, std::initializer_list<KeyAction> actions = {KeyAction::PRESS}) {
    REQUIRE(scene->onKey(ui::keyActions(source, actions), std::move(callback)));
  }

  bool send(char32_t scalar, KeyAction action = KeyAction::PRESS, InputModifiers modifiers = {}) {
    auto result = scene->input(*app, KeyEvent{InputKey::CHARACTER, scalar, modifiers, action});

    REQUIRE(result);

    return *result;
  }
};
} // namespace

TEST_CASE("sequence parser owns segments budgets and exact typed offsets") {
  auto compact = parseShortcut("Ctrl+K+K");
  auto explicitForm = parseShortcut("Ctrl+K,Ctrl+K");
  auto independent = parseShortcut("Ctrl+K,K | F10");
  auto modifiers = parseShortcut("Ctrl+K+Shift+C");

  REQUIRE(compact);
  REQUIRE(explicitForm);
  REQUIRE(independent);
  REQUIRE(modifiers);
  REQUIRE(compact->sequences.size() == 1);
  CHECK(compact->sequences.front().strokes[0].scalar == U'K');
  CHECK(compact->sequences.front().strokes[1].scalar == U'K');
  CHECK(compact->sequences.front().strokes[1].modifiers.control);
  CHECK(explicitForm->sequences.front().strokes[1].modifiers.control);
  CHECK(independent->alternatives.size() == 1);
  CHECK(independent->sequences.front().strokes[0].modifiers.control);
  CHECK_FALSE(independent->sequences.front().strokes[1].modifiers.control);
  CHECK(modifiers->sequences.front().strokes[0].modifiers.shift);
  CHECK(modifiers->sequences.front().strokes[1].modifiers.shift);
  CHECK_FALSE(matchShortcut(*compact, KeyEvent{InputKey::CHARACTER, U'K', {.control = true}}));
  CHECK(parseShortcut("Ctrl+K, Escape").error() == ShortcutError{ShortcutErrorKind::RESERVED_CANCEL, 8});
  CHECK(parseShortcut("Ctrl+K | Ctrl+K,K").error() == ShortcutError{ShortcutErrorKind::AMBIGUOUS_PREFIX, 9});
  CHECK(parseShortcut("Ctrl+K,K | Ctrl+K").error() == ShortcutError{ShortcutErrorKind::AMBIGUOUS_PREFIX, 11});
  CHECK(parseShortcut("A,B|F10", {.maxAlternatives = 1}).error().cause == ShortcutErrorKind::ALTERNATIVE_LIMIT);
  CHECK(parseShortcut("A,B,C", {.maxStrokes = 2}).error() == ShortcutError{ShortcutErrorKind::STROKE_LIMIT, 4});
  CHECK(parseShortcut("A,Ctrl+Escape"));
  CHECK(parseShortcut("A,B|A,B"));

  Shortcut typed{key::F10};

  typed.sequences.emplace_back(std::initializer_list<key::Chord>{key::A, key::B});
  CHECK(validateShortcut(typed));
  CHECK(validateShortcut(typed, {.maxAlternatives = 1}).error().cause == ShortcutErrorKind::ALTERNATIVE_LIMIT);

  typed.alternatives.push_back(key::A);
  CHECK(validateShortcut(typed).error().cause == ShortcutErrorKind::AMBIGUOUS_PREFIX);
}

TEST_CASE("sequence modifiers use observations without held state or replay") {
  SequenceFixture fixture;
  int calls{}, fallback{};

  REQUIRE(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++fallback;

    return false;
  }));
  fixture.bind("Ctrl+K+K", [&] {
    ++calls;
  });

  CHECK(fixture.send(U'K', KeyAction::PRESS, {.control = true}));
  CHECK_FALSE(fixture.send(U'K', KeyAction::RELEASE, {.control = true}));
  CHECK_FALSE(fixture.send(U'K'));
  CHECK(calls == 0);
  CHECK(fallback == 2);
  CHECK(fixture.send(U'K', KeyAction::PRESS, {.control = true}));
  CHECK(fixture.send(U'k', KeyAction::PRESS, {.control = true}));
  CHECK(calls == 1);

  fixture.bind("Ctrl+K,K", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'K', KeyAction::PRESS, {.control = true}));
  CHECK(fixture.send(U'K'));
  CHECK(calls == 2);
}

TEST_CASE("sequence phase masks apply independently to every stroke") {
  SequenceFixture fixture;
  int calls{}, rootCalls{};
  KeyAction last{};

  fixture.bind("C", [&] {
    ++rootCalls;
  });
  fixture.bind("A,B",
               [&](const KeyEvent &event) {
                 ++calls;
                 last = event.action;
               },
               {KeyAction::RELEASE});

  CHECK_FALSE(fixture.send(U'A'));
  CHECK(fixture.send(U'A', KeyAction::RELEASE));
  CHECK(fixture.send(U'C'));
  CHECK_FALSE(fixture.send(U'B'));
  CHECK(fixture.send(U'B', KeyAction::RELEASE));
  CHECK(calls == 1);
  CHECK(rootCalls == 1);
  CHECK(last == KeyAction::RELEASE);

  fixture.bind("A,B",
               [&](const KeyEvent &event) {
                 ++calls;
                 last = event.action;
               },
               {KeyAction::PRESS, KeyAction::RELEASE});
  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B', KeyAction::RELEASE));
  CHECK(calls == 2);
  CHECK(last == KeyAction::RELEASE);

  REQUIRE(fixture.scene->onInput({}));
  fixture.bind("A,B", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'A'));
  CHECK_FALSE(fixture.send(U'B', KeyAction::REPEAT));
  CHECK_FALSE(fixture.send(U'B', KeyAction::RELEASE));
  CHECK(fixture.send(U'B'));
  CHECK(calls == 3);

  auto repeated = parseShortcut("A,B");

  REQUIRE(repeated);
  REQUIRE(fixture.scene->onKey(*repeated,
                               [&] {
                                 ++calls;
                               },
                               {.repeat = true}));
  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B', KeyAction::REPEAT));
  CHECK(calls == 4);
}

TEST_CASE("ignored phase can replace pending through ordinary root prefix admission") {
  SequenceFixture fixture;
  int oldCalls{}, nextCalls{};

  fixture.bind("A,B",
               [&] {
                 ++oldCalls;
               },
               {KeyAction::RELEASE});
  fixture.bind("C,D", [&] {
    ++nextCalls;
  });

  CHECK(fixture.send(U'A', KeyAction::RELEASE));
  CHECK(fixture.send(U'C'));
  CHECK_FALSE(fixture.send(U'B', KeyAction::RELEASE));
  CHECK(fixture.send(U'D'));
  CHECK(oldCalls == 0);
  CHECK(nextCalls == 1);
}

TEST_CASE("shared prefix candidates and ambiguous terminals follow newest matching registration") {
  SequenceFixture fixture;
  int oldCalls{}, newCalls{};

  SUBCASE("compatible older branch survives a nonmatching newer branch") {
    fixture.bind("A,B", [&] {
      ++oldCalls;
    });
    fixture.bind("A,C", [&] {
      ++newCalls;
    });
    CHECK(fixture.send(U'A'));
    CHECK(fixture.send(U'B'));
    CHECK(oldCalls == 1);
    CHECK(newCalls == 0);
  }

  SUBCASE("newer longer prefix suppresses older terminal permanently") {
    fixture.bind("A,B", [&] {
      ++oldCalls;
    });
    fixture.bind("A,B,C", [&] {
      ++newCalls;
    });
    CHECK(fixture.send(U'A'));
    CHECK(fixture.send(U'B'));
    CHECK(oldCalls == 0);
    fixture.now = 1000;
    REQUIRE(fixture.app->scheduler().dispatch());
    CHECK(oldCalls == 0);
    CHECK(newCalls == 0);
    CHECK_FALSE(fixture.send(U'C'));
  }

  SUBCASE("newer terminal wins over older longer prefix") {
    fixture.bind("A,B,C", [&] {
      ++oldCalls;
    });
    fixture.bind("A,B", [&] {
      ++newCalls;
    });
    CHECK(fixture.send(U'A'));
    CHECK(fixture.send(U'B'));
    CHECK(newCalls == 1);
    CHECK_FALSE(fixture.send(U'C'));
  }

  SUBCASE("single terminal wins at root when newer") {
    fixture.bind("A,B", [&] {
      ++oldCalls;
    });
    fixture.bind("A", [&] {
      ++newCalls;
    });
    CHECK(fixture.send(U'A'));
    CHECK(newCalls == 1);
    CHECK_FALSE(fixture.send(U'B'));
  }
}

TEST_CASE("sequence prefixes preserve mixed legacy order and current mismatch routes once") {
  SequenceFixture fixture;
  int oldCalls{}, sequenceCalls{}, fallback{};

  REQUIRE(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++fallback;

    return false;
  }));

  SUBCASE("newer legacy terminal wins") {
    fixture.bind("A,B", [&] {
      ++sequenceCalls;
    });
    REQUIRE(fixture.scene->onKey(KeyEvent{InputKey::CHARACTER, U'A'}, [&] {
      ++oldCalls;
    }));
    CHECK(fixture.send(U'A'));
    CHECK(oldCalls == 1);
    CHECK_FALSE(fixture.send(U'B'));
  }

  SUBCASE("newer sequence suppresses legacy terminal") {
    REQUIRE(fixture.scene->onKey(KeyEvent{InputKey::CHARACTER, U'A'}, [&] {
      ++oldCalls;
    }));
    fixture.bind("A,B", [&] {
      ++sequenceCalls;
    });
    CHECK(fixture.send(U'A'));
    CHECK(fixture.send(U'B'));
    CHECK(oldCalls == 0);
    CHECK(sequenceCalls == 1);
  }

  SUBCASE("mismatch may start a new prefix and does not replay old input") {
    fixture.bind("A,B", [&] {
      ++oldCalls;
    });
    fixture.bind("C,D", [&] {
      ++sequenceCalls;
    });
    CHECK(fixture.send(U'A'));
    CHECK(fixture.send(U'C'));
    CHECK(fixture.send(U'D'));
    CHECK(sequenceCalls == 1);
    CHECK(fallback == 0);
    CHECK(fixture.send(U'A'));
    CHECK_FALSE(fixture.send(U'X'));
    CHECK(fallback == 1);
    CHECK(oldCalls == 0);
  }
}

TEST_CASE("sequence timeout is exclusive rearmed only by admitted strokes and never invokes commands") {
  SequenceFixture fixture({.keySequenceTimeout = 10});
  int calls{};

  fixture.bind("A,B,C", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'A'));
  fixture.now = 9;
  CHECK(fixture.send(U'B'));
  fixture.now = 10;
  REQUIRE(fixture.app->scheduler().dispatch());
  CHECK(fixture.send(U'C'));
  CHECK(calls == 1);

  CHECK(fixture.send(U'A'));
  fixture.now = 19;
  CHECK_FALSE(fixture.send(U'B', KeyAction::RELEASE));
  fixture.now = 20;
  CHECK_FALSE(fixture.send(U'B'));
  CHECK(calls == 1);

  CHECK(fixture.send(U'A'));
  fixture.now = 30;
  REQUIRE(fixture.app->scheduler().dispatch());
  CHECK_FALSE(fixture.send(U'B'));
  CHECK(calls == 1);

  fixture.now = std::numeric_limits<SchedulerTime>::max() - 1;
  CHECK(fixture.send(U'A'));
  fixture.now = std::numeric_limits<SchedulerTime>::max();
  CHECK_FALSE(fixture.send(U'B'));
  CHECK(calls == 1);
}

TEST_CASE("pending Escape cancels even release only while modified Escape remains a stroke") {
  SequenceFixture fixture;
  int calls{}, rootCalls{};

  fixture.bind("Escape", [&] {
    ++rootCalls;
  });
  fixture.bind("A,B",
               [&] {
                 ++calls;
               },
               {KeyAction::RELEASE});
  CHECK(fixture.send(U'A', KeyAction::RELEASE));

  auto cancelled = fixture.scene->input(*fixture.app, KeyEvent{InputKey::ESCAPE});

  REQUIRE(cancelled);
  CHECK(*cancelled);
  CHECK(rootCalls == 0);
  CHECK_FALSE(fixture.send(U'B', KeyAction::RELEASE));

  auto rootEscape = fixture.scene->input(*fixture.app, KeyEvent{InputKey::ESCAPE});

  REQUIRE(rootEscape);
  CHECK(*rootEscape);
  CHECK(rootCalls == 1);

  fixture.bind("A,Ctrl+Escape", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'A'));

  auto modified = fixture.scene->input(*fixture.app, KeyEvent{InputKey::ESCAPE, 0, {.control = true}});

  REQUIRE(modified);
  CHECK(*modified);
  CHECK(calls == 1);
}

TEST_CASE("focus and tree eligibility generations invalidate prefixes through away and back") {
  SequenceFixture fixture;
  auto caption = GraphemeText::from("button");

  REQUIRE(caption);

  auto first = fixture.scene->emplace<Button>(fixture.scene->root(), *caption);
  auto second = fixture.scene->emplace<Button>(fixture.scene->root(), *caption);

  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(fixture.scene->onClick(*first, [] {
  }));
  REQUIRE(fixture.scene->onClick(*second, [] {
  }));
  REQUIRE(fixture.app->router().setFocus(*first));

  int calls{};

  fixture.bind("A,B", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'A'));

  SUBCASE("focus away and back") {
    REQUIRE(fixture.app->router().setFocus(*second));
    REQUIRE(fixture.app->router().setFocus(*first));
  }

  SUBCASE("hide and show ancestor") {
    REQUIRE(fixture.app->tree().setVisible(fixture.scene->root(), false));
    REQUIRE(fixture.app->tree().setVisible(fixture.scene->root(), true));
  }

  SUBCASE("disable and enable focused node") {
    REQUIRE(fixture.app->tree().setEnabled(*first, false));
    REQUIRE(fixture.app->tree().setEnabled(*first, true));
  }

  SUBCASE("unrelated direct low level remove is conservative reset") {
    REQUIRE(fixture.app->tree().remove(*second));
  }

  SUBCASE("no-op and paint invalidation preserve prefix") {
    REQUIRE(fixture.app->tree().setVisible(*first, true));
    REQUIRE(fixture.app->tree().setEnabled(*first, true));
    REQUIRE(fixture.app->router().setFocus(*first));
    REQUIRE(fixture.app->tree().invalidate(*first, Invalidation::PAINT));
    CHECK(fixture.send(U'B'));
    CHECK(calls == 1);

    return;
  }

  CHECK_FALSE(fixture.send(U'B'));
  CHECK(calls == 0);
}

TEST_CASE("sequence completion clears before callbacks pins captures and preserves replacement") {
  SequenceFixture fixture;
  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> observer = lifetime;
  int next{};

  fixture.bind("A,B", [&, lifetime] {
    REQUIRE(fixture.scene->onInput({}));
    CHECK_FALSE(observer.expired());
    fixture.bind("C,D", [&] {
      ++next;
    });
    CHECK(fixture.send(U'C'));
  });
  lifetime.reset();

  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B'));
  CHECK(observer.expired());
  CHECK(fixture.send(U'D'));
  CHECK(next == 1);

  fixture.bind("A,B", [&] {
    REQUIRE(fixture.scene->close());
  });
  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B'));
  fixture.now = 2000;
  REQUIRE(fixture.app->scheduler().dispatch());
}

TEST_CASE("sequence replacement is atomic for every registration allocation failure") {
  int failures{};
  bool succeeded{};

  for (std::ptrdiff_t index{}; index < 96 && !succeeded; ++index) {
    SequenceFixture fixture;
    int calls{};

    fixture.bind("A,B", [&] {
      ++calls;
    });
    CHECK(fixture.send(U'A'));

    auto candidate = parseShortcut("C,D");

    REQUIRE(candidate);

    AllocationFailure fault(index);
    auto registered = fixture.scene->onKey(*candidate, [] {
    });

    fault.disable();

    if (registered) {
      succeeded = true;
      CHECK_FALSE(fixture.send(U'B'));
    } else {
      ++failures;
      CHECK(std::get<TreeError>(registered.error().cause) == TreeError::NO_MEMORY);
      CHECK(fixture.send(U'B'));
      CHECK(calls == 1);
    }
  }

  CHECK(succeeded);
  CHECK(failures > 0);
}

TEST_CASE("prefix timer allocation failure leaves idle through existing input stop handling") {
  for (bool rearm : {false, true}) {
    int failures{};
    bool succeeded{};

    for (std::ptrdiff_t index{}; index < 64 && !succeeded; ++index) {
      SequenceFixture fixture;
      int calls{};

      fixture.bind("A,B,C", [&] {
        ++calls;
      });

      if (rearm) {
        CHECK(fixture.send(U'A'));
      }

      AllocationFailure fault(index);
      auto admitted = fixture.scene->input(*fixture.app, KeyEvent{InputKey::CHARACTER, rearm ? U'B' : U'A'});

      fault.disable();

      if (admitted) {
        succeeded = true;
        CHECK(*admitted);
      } else {
        ++failures;
        CHECK(admitted.error() == TreeError::NO_MEMORY);
        CHECK_FALSE(fixture.send(U'B'));
        CHECK_FALSE(fixture.send(U'C'));
        CHECK(calls == 0);
      }
    }

    CHECK(succeeded);
    CHECK(failures > 0);
  }
}

TEST_CASE("prefix timer and reset never request idle redraw") {
  SequenceFixture fixture;
  int calls{};

  fixture.bind("A,B", [&] {
    ++calls;
  });
  REQUIRE(fixture.app->tree().consumeFrameRequest());
  CHECK(fixture.send(U'A'));

  auto prefixFrame = fixture.app->tree().consumeFrameRequest();

  REQUIRE(prefixFrame);
  CHECK(*prefixFrame == Invalidation::NONE);
  fixture.now = 1000;
  REQUIRE(fixture.app->scheduler().dispatch());

  auto timeoutFrame = fixture.app->tree().consumeFrameRequest();

  REQUIRE(timeoutFrame);
  CHECK(*timeoutFrame == Invalidation::NONE);
  CHECK(calls == 0);
}

TEST_CASE("sequence storage and helpers catch every ownership allocation failure") {
  for (int mode : {0, 1, 2, 3}) {
    CAPTURE(mode);
    int failures{};
    bool succeeded{};

    for (std::ptrdiff_t index{}; index < MAX_FAILURE_ATTEMPTS && !succeeded; ++index) {
      auto source = parseShortcut("F10|Ctrl+K,K|A,B,C");

      REQUIRE(source);

      AllocationFailure fault(index);
      const auto result = [&]() -> std::expected<Shortcut, ShortcutError> {
        if (mode == 0) {
          return parseShortcut("F10|Ctrl+K,K|A,B,C");
        }

        if (mode == 1) {
          return detail::prepareShortcut(*source);
        }

        if (mode == 2) {
          return detail::prepareShortcut(std::move(*source));
        }

        const auto selected = ui::release("F10|Ctrl+K,K|A,B,C");
        const auto &retained = detail::KeySelectionAccess::shortcut(selected);

        if (!retained) {
          return std::unexpected(retained.error());
        }

        return detail::prepareShortcut(**retained);
      }();

      fault.disable();

      if (result) {
        succeeded = true;
        CHECK(result->sequences.size() == 2);
      } else {
        ++failures;
        CHECK(result.error().cause == ShortcutErrorKind::NO_MEMORY);
      }
    }

    CHECK(succeeded);
    if (mode != 2) {
      CHECK(failures > 0);
    }
  }
}

TEST_CASE("sequence limits remain configurable above their defaults") {
  std::string alternatives;

  for (int index{}; index < 40; ++index) {
    alternatives += index ? "|A,B,C,D,E,F,G,H,I" : "A,B,C,D,E,F,G,H,I";
  }

  alternatives.insert(0, 4100, ' ');

  auto parsed = parseShortcut(alternatives, {.maxSourceBytes = 8192, .maxAlternatives = 40, .maxStrokes = 9});

  REQUIRE(parsed);
  CHECK(parsed->sequences.size() == 40);
  CHECK(parsed->sequences.front().strokes.size() == 9);

  SequenceFixture fixture({.maxKeyHandlers = 66});
  int calls{};

  for (int index{}; index < 66; ++index) {
    fixture.bind("A,B", [&] {
      ++calls;
    });
  }

  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B'));
  CHECK(calls == 1);

  SequenceFixture longFixture;
  int longCalls{};

  REQUIRE(longFixture.scene->onKey(*parsed,
                                   [&] {
                                     ++longCalls;
                                   },
                                   {}, {.maxSourceBytes = 8192, .maxAlternatives = 40, .maxStrokes = 9}));

  for (char32_t scalar = U'A'; scalar <= U'I'; ++scalar) {
    CHECK(longFixture.send(scalar));
  }

  CHECK(longCalls == 1);
}

TEST_CASE("sequence text and duplicate alternatives invoke once with original final event") {
  SequenceFixture fixture;
  int calls{};
  char32_t scalar{};

  fixture.bind("Alt+K,K | Alt+K,K", [&](const KeyEvent &event) {
    ++calls;
    scalar = event.scalar;
  });

  auto first = fixture.scene->input(*fixture.app, TextInputEvent{U'k', {.alt = true}});
  auto second = fixture.scene->input(*fixture.app, TextInputEvent{U'k'});

  REQUIRE(first);
  REQUIRE(second);
  CHECK(*first);
  CHECK(*second);
  CHECK(calls == 1);
  CHECK(scalar == U'k');
}

TEST_CASE("sequence action errors retain winning registration origin and one collector record") {
  for (int mode : {0, 1, 2}) {
    SequenceFixture fixture;
    auto source = parseShortcut("A,B");

    REQUIRE(source);

    const auto origin = std::source_location::current();
    const auto callback = [mode]() -> std::expected<void, TreeError> {
      if (mode == 1) {
        throw std::runtime_error("sequence action");
      }

      if (mode == 2) {
        throw std::bad_alloc();
      }

      return std::unexpected(TreeError::INVALID_ARGUMENT);
    };

    REQUIRE(fixture.scene->onKey(*source, callback, {}, {}, origin));
    CHECK(fixture.send(U'A'));

    const auto result = fixture.scene->input(*fixture.app, KeyEvent{InputKey::CHARACTER, U'B'});

    REQUIRE_FALSE(result);
    CHECK(result.error() == (mode == 0   ? TreeError::INVALID_ARGUMENT
                             : mode == 1 ? TreeError::CALLBACK_FAILED
                                         : TreeError::NO_MEMORY));

    auto diagnostics = fixture.scene->diagnostics();

    REQUIRE(diagnostics);
    REQUIRE(diagnostics->errors.size() == 1);
    CHECK(diagnostics->errors.front().origin.line() == origin.line());
  }
}

TEST_CASE("sequence rearm uses one live scheduler timer and bounded timer failure resets idle") {
  SequenceFixture fixture({}, {.scheduler = {.subscription_capacity = 1}});
  int calls{};

  fixture.bind("A,B,C,D", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B'));
  CHECK(fixture.send(U'C'));
  CHECK(fixture.send(U'D'));
  CHECK(calls == 1);

  constexpr int CANCEL_CYCLES = 32;

  for (int cycle{}; cycle < CANCEL_CYCLES; ++cycle) {
    CHECK(fixture.send(U'A'));

    const auto cancelled = fixture.scene->input(*fixture.app, KeyEvent{InputKey::ESCAPE});

    REQUIRE(cancelled);
    CHECK(*cancelled);
  }

  CHECK(fixture.send(U'A'));
  CHECK(fixture.send(U'B'));
  CHECK(fixture.send(U'C'));
  CHECK(fixture.send(U'D'));
  CHECK(calls == 2);

  Subscription blocker;

  REQUIRE(fixture.app->tree().withScope(fixture.scene->root(), [&](const UiScope &scope) {
    auto timer = fixture.app->scheduler().at(scope, 5000, [] {
    });

    REQUIRE(timer);

    blocker = std::move(*timer);
  }));

  const auto result = fixture.scene->input(*fixture.app, KeyEvent{InputKey::CHARACTER, U'A'});

  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::WOULD_BLOCK);
  CHECK_FALSE(fixture.send(U'B'));
  CHECK_FALSE(fixture.send(U'C'));
  CHECK_FALSE(fixture.send(U'D'));
  CHECK(calls == 2);
}

TEST_CASE("failed onInput publication preserves pending and zero sequence timeout is rejected") {
  SequenceFixture fixture;
  int calls{};

  fixture.bind("A,B", [&] {
    ++calls;
  });
  CHECK(fixture.send(U'A'));

  std::function<std::expected<bool, ManagedError>(const InputEvent &)> replacement =
    [](const InputEvent &) -> std::expected<bool, ManagedError> {
    return false;
  };

  AllocationFailure fault(0);
  const auto replaced = fixture.scene->onInput(std::move(replacement));

  fault.disable();

  REQUIRE_FALSE(replaced);
  CHECK(std::get<TreeError>(replaced.error().cause) == TreeError::NO_MEMORY);
  CHECK(fixture.send(U'B'));
  CHECK(calls == 1);

  const auto invalid = ManagedScene::create(*fixture.app, {.keySequenceTimeout = 0});

  REQUIRE_FALSE(invalid);
  CHECK(std::get<TreeError>(invalid.error().cause) == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("modifier-only segments report the first source modifier token") {
  struct Invalid {
    std::string_view source;
    std::size_t offset;
  };

  const Invalid cases[] = {{"Alt", 0},        {"  Alt + Ctrl  ", 2},     {"Alt, A", 0},        {"  Alt + Ctrl  , A", 2},
                           {"A,  Ctrl  ", 4}, {"A,  Alt + Ctrl , B", 4}, {"F10 | \t Ctrl ", 8}};

  for (const auto &invalid : cases) {
    CAPTURE(invalid.source);

    const auto parsed = parseShortcut(invalid.source);

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().cause == ShortcutErrorKind::EXPECTED_KEY);
    CHECK(parsed.error().offset == invalid.offset);
  }
}

TEST_CASE("ignored-phase root terminal preserves reentrant registration and new prefix") {
  for (bool replaceInput : {false, true}) {
    CAPTURE(replaceInput);

    SequenceFixture fixture;
    int oldCalls{}, terminalCalls{}, nextCalls{};

    fixture.bind("A,B",
                 [&] {
                   ++oldCalls;
                 },
                 {KeyAction::RELEASE});
    fixture.bind("C", [&] {
      ++terminalCalls;

      if (replaceInput) {
        REQUIRE(fixture.scene->onInput({}));
      }

      fixture.bind("D,E", [&] {
        ++nextCalls;
      });
      CHECK(fixture.send(U'D'));
    });

    CHECK(fixture.send(U'A', KeyAction::RELEASE));
    CHECK(fixture.send(U'C'));
    CHECK(terminalCalls == 1);
    CHECK_FALSE(fixture.send(U'B', KeyAction::RELEASE));
    CHECK(fixture.send(U'E'));
    CHECK(oldCalls == 0);
    CHECK(nextCalls == 1);
  }
}

TEST_CASE("ignored-phase root prefix arm failure clears live pending and retains winning origin") {
  SequenceFixture fixture;
  int oldCalls{}, nextCalls{};
  auto oldShortcut = parseShortcut("A,B");
  auto nextShortcut = parseShortcut("C,D");

  REQUIRE(oldShortcut);
  REQUIRE(nextShortcut);

  const auto oldOrigin = std::source_location::current();

  REQUIRE(fixture.scene->onKey(
    ui::release(*oldShortcut),
    [&] {
      ++oldCalls;
    },
    oldOrigin));

  const auto nextOrigin = std::source_location::current();

  REQUIRE(fixture.scene->onKey(
    *nextShortcut,
    [&] {
      ++nextCalls;
    },
    {}, {}, nextOrigin));
  CHECK(fixture.send(U'A', KeyAction::RELEASE));
  fixture.now = 5;

  AllocationFailure fault(0);
  const auto result = fixture.scene->input(*fixture.app, KeyEvent{InputKey::CHARACTER, U'C'});

  fault.disable();

  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::NO_MEMORY);
  CHECK_FALSE(fixture.send(U'B', KeyAction::RELEASE));
  CHECK_FALSE(fixture.send(U'D'));
  CHECK(oldCalls == 0);
  CHECK(nextCalls == 0);

  const auto diagnostics = fixture.scene->diagnostics();

  REQUIRE(diagnostics);
  REQUIRE(diagnostics->errors.size() == 1);
  CHECK(diagnostics->errors.front().error.phase == ManagedPhase::INPUT);
  CHECK(diagnostics->errors.front().origin.line() == nextOrigin.line());
  CHECK(diagnostics->errors.front().origin.line() != oldOrigin.line());
  REQUIRE(diagnostics->errors.front().error.registrationOrigin);
  CHECK(diagnostics->errors.front().error.registrationOrigin->line() == nextOrigin.line());
}
