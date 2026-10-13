// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../examples/replay/terminal.hpp"
#include <portals/shortcut.hpp>
#include <portals/ui.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

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

constexpr std::ptrdiff_t MAX_FAILURE_ATTEMPTS = 32;
} // namespace

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

using namespace portals;

static_assert((key::ALT + key::Q).scalar == U'Q');
static_assert((key::CTRL + key::ALT + key::Q).modifiers.control);

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
                           {"Ctrl + K + K", ShortcutErrorKind::UNSUPPORTED_SEQUENCE},
                           {"Ctrl+K, Ctrl+C", ShortcutErrorKind::UNSUPPORTED_SEQUENCE}};

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
  for (bool close : {false, true}) {
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
    REQUIRE(scene.onKey(*normalized, [&, capture] {
      if (close) {
        REQUIRE(scene.close());
      } else {
        REQUIRE(scene.onInput({}));
      }

      CHECK_FALSE(observer.expired());
    }));
    capture.reset();

    auto consumed = scene.input(**application, KeyEvent{.key = InputKey::F10});

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
  for (bool pipe : {false, true}) {
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

      if (pipe) {
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
  example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  int notices{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::lineEdit(field, "ready"), options);

  scene.onError([&](const ManagedDiagnostic &) {
    ++notices;
  });
  scene.onKey("F10", [&] {
    auto managed = scene.managed();

    REQUIRE(managed);
    REQUIRE(managed->get().remove(field.node()));

    return field.undo();
  });

  REQUIRE(scene.run(terminal));
  CHECK(notices == 1);
  const auto diagnostics = scene.diagnostics();

  REQUIRE(diagnostics);
  CHECK(diagnostics->errors.size() == 1);
}

TEST_CASE("facade method and pipe catch every shortcut ownership allocation failure") {
  for (bool pipe : {false, true}) {
    bool reachedSuccess{};
    int failures{};

    for (std::ptrdiff_t failureIndex{}; failureIndex < MAX_FAILURE_ATTEMPTS; ++failureIndex) {
      CAPTURE(pipe);
      CAPTURE(failureIndex);
      auto scene = ui::scene(ui::label("ready"));
      const auto origin = std::source_location::current();

      {
        AllocationFailure fault(failureIndex);

        if (pipe) {
          auto stage = ui::onKey(
            "F10 | Alt+Q",
            [] {
            },
            origin);

          scene | std::move(stage);
        } else {
          scene.onKey(
            "F10 | Alt+Q",
            [] {
            },
            origin);
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
