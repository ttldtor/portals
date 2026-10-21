// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui.hpp>

#include <array>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

namespace allocationTest {
thread_local std::ptrdiff_t remaining{-1};

// GCC otherwise inlines the malloc-backed replacement delete and diagnoses a
// false allocation/deallocation mismatch at ordinary new expressions.
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void release(void *value) noexcept {
  std::free(value);
}
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }

  if (auto *value = std::malloc(size == 0 ? 1 : size)) {
    return value;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *value) noexcept {
  allocationTest::release(value);
}

void operator delete(void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));

  ui::Ref<LineEdit> edit;
  ui::Ref<Button> undoButton;
  ui::Ref<Button> other;
  ManagedState<std::string> model = take(scene->state(std::string("base")));

  std::vector<LineEditResult> outcomes = std::vector<LineEditResult>(0);

  explicit Fixture(ui::LineEditConfig config = {}, ApplicationOptions options = {})
      : app(take(Application::create(options))) {
    outcomes.reserve(64);
    config.result = [&, callback = std::move(config.result)](LineEditResult outcome) -> std::expected<void, TreeError> {
      outcomes.push_back(outcome);

      return callback ? callback(std::move(outcome)) : std::expected<void, TreeError>{};
    };
    take(ui::mount(*scene, ui::column(ui::lineEdit(model, std::move(config)).reference(edit),
                                      ui::button("Undo",
                                                 [field = edit] {
                                                   return field.undo();
                                                 })
                                        .reference(undoButton),
                                      ui::button("other", []() -> std::expected<void, TreeError> {
                                        return {};
                                      }).reference(other))));
    take(app->router().setFocus(edit.node()));
  }

  std::string draft() const {
    return take(edit.text().get());
  }

  std::string value() const {
    return take(model.snapshot())->value;
  }
};
} // namespace

TEST_CASE("history defaults are enabled and the command result keeps checked errors") {
  constexpr LineEditOptions OPTIONS{};
  static_assert(OPTIONS.history.enabled);
  static_assert(OPTIONS.history.maxOperations == 128);
  static_assert(OPTIONS.history.maxBytes == 1048576);
  static_assert(!OPTIONS.readOnly);
  static_assert(OPTIONS.programmaticChange == ProgrammaticChange::RECORD);
  static_assert(!std::is_convertible_v<EditorCommandResult, std::expected<void, TreeError>>);
  ui::Ref<LineEdit> empty;

  CHECK_FALSE(empty.canUndo());
  CHECK_FALSE(empty.historyAvailability());

  const auto command = empty.undo();

  REQUIRE_FALSE(command);
  CHECK(command.result().error() == TreeError::CLOSED);
}

TEST_CASE("recorded fluent edits undo and redo through the immediate publication path") {
  Fixture fixture;
  take((fixture.edit.text() = "first").result());
  take((fixture.edit.text() += " second").result());

  CHECK(fixture.edit.canUndo());
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "first");
  CHECK(fixture.value() == "first");
  CHECK(fixture.edit.canRedo());
  take(fixture.edit.redo().result());
  CHECK(fixture.draft() == "first second");
  CHECK(fixture.value() == "first second");
}

TEST_CASE("typing joins one group and navigation creates an editing boundary") {
  Fixture fixture;
  take(fixture.app->router().dispatch(TextInputEvent{U'a'}));
  take(fixture.app->router().dispatch(TextInputEvent{U'b'}));
  take(fixture.edit.undo().result());

  CHECK(fixture.draft() == "base");
  take(fixture.edit.redo().result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::LEFT}));
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "baseab");
}

TEST_CASE("explicit reset clears history even for equal text and ordinary no-op preserves redo") {
  Fixture fixture;
  take((fixture.edit.text() = "next").result());
  take(fixture.edit.undo().result());
  take((fixture.edit.text() = "base").result());

  CHECK(fixture.edit.canRedo());
  take((fixture.edit.text(ProgrammaticChange::RESET_BASELINE) = "base").result());
  CHECK_FALSE(fixture.edit.canUndo());
  CHECK_FALSE(fixture.edit.canRedo());
}

TEST_CASE("a declarative undo button consumes the same checked command result") {
  Fixture fixture;
  take((fixture.edit.text() = "next").result());
  take(fixture.app->router().invoke(fixture.undoButton.node(), {Action::ACTIVATE}));

  CHECK(fixture.value() == "base");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("read only blocks user editing and history but permits explicit programmatic replacement") {
  ui::LineEditConfig config;
  config.options.readOnly = true;
  Fixture fixture(std::move(config));
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));

  CHECK(fixture.draft() == "base");
  take((fixture.edit.text() = "loaded").result());
  CHECK(fixture.value() == "loaded");
  CHECK_FALSE(fixture.edit.canUndo());

  const auto command = fixture.edit.undo();

  REQUIRE_FALSE(command);
  CHECK(command.result().error() == TreeError::WOULD_BLOCK);
  REQUIRE(take(fixture.scene->diagnostics()).errors.size() == 1);
}

TEST_CASE("history retains Unicode joins and whole selection endpoints") {
  Fixture fixture;

  const std::string EMOJI = "\xf0\x9f\x91\xa9";

  take((fixture.edit.text() = EMOJI).result());
  take(fixture.app->router().dispatch(TextInputEvent{0x200d}));
  take(fixture.app->router().dispatch(TextInputEvent{0x1f4bb}));

  const auto joined = fixture.draft();

  take(fixture.app->tree().read(fixture.edit.node(), [&](const LineEdit &edit) {
    CHECK(edit.text().graphemes().size() == 1);
    CHECK(edit.cursor() == joined.size());
  }));
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == EMOJI);
  take(fixture.edit.redo().result());
  CHECK(fixture.draft() == joined);
  take(fixture.app->router().dispatch(KeyEvent{InputKey::HOME, 0, {.shift = true}}));
  take(fixture.app->router().dispatch(TextInputEvent{U'q'}));
  take(fixture.edit.undo().result());
  take(fixture.app->tree().read(fixture.edit.node(), [&](const LineEdit &edit) {
    CHECK(edit.text().bytes() == joined);
    CHECK(edit.cursor() == 0);
    CHECK(edit.anchor() == joined.size());
  }));
}

TEST_CASE("Undo applies every edit mode without clearing history on explicit confirmation") {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    CAPTURE(mode);
    ui::LineEditConfig config;
    config.options.editMode = mode;
    Fixture fixture(std::move(config));
    take((fixture.edit.text() = "next").result());
    CHECK(fixture.value() == (mode == EditMode::IMMEDIATE ? "next" : "base"));
    take(fixture.app->router().invoke(fixture.edit.node(), {Action::ACTIVATE}));
    CHECK(fixture.edit.canUndo());
    take(fixture.edit.undo().result());
    CHECK(fixture.draft() == "base");
    CHECK(fixture.value() == (mode == EditMode::IMMEDIATE ? "base" : "next"));
    take(fixture.app->router().invoke(fixture.edit.node(), {Action::ACTIVATE}));
    CHECK(fixture.value() == "base");
    CHECK(fixture.edit.canRedo());
  }
}

TEST_CASE("semantic rejection and validator exceptions preserve the admitted reversible Undo draft") {
  for (const bool throws : {false, true}) {
    bool reject = false;
    ui::LineEditConfig config;
    config.validate = [&](std::string_view) {
      if (reject && throws) {
        throw std::bad_alloc();
      }

      return !reject;
    };
    Fixture fixture(std::move(config));
    take((fixture.edit.text() = "next").result());
    reject = true;

    const auto command = fixture.edit.undo();

    CHECK(command.result().has_value() == !throws);
    CHECK(fixture.draft() == "base");
    CHECK(fixture.value() == "next");
    CHECK(fixture.edit.canRedo());
    CHECK(fixture.outcomes.back().status == (throws ? LineEditStatus::FAILED : LineEditStatus::VALIDATION_FAILED));
    reject = false;
    take(fixture.edit.redo().result());
    CHECK(fixture.draft() == "next");
  }
}

TEST_CASE("clean external revision resets history and the pending Undo becomes a no-op") {
  Fixture fixture;
  take((fixture.edit.text() = "next").result());
  take(fixture.model.set(std::string("external")));
  CHECK_FALSE(fixture.edit.canUndo());
  CHECK_FALSE(fixture.edit.canRedo());

  const auto calls = fixture.outcomes.size();

  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "external");
  CHECK(fixture.value() == "external");
  CHECK(fixture.outcomes.size() == calls);
  CHECK_FALSE(fixture.edit.canUndo());
}

TEST_CASE("dirty external revision freezes history until explicit RESET or Escape") {
  for (const bool reset : {false, true}) {
    ui::LineEditConfig config;
    config.options.editMode = EditMode::EXPLICIT;
    Fixture fixture(std::move(config));
    take((fixture.edit.text() = "draft").result());
    take(fixture.model.set(std::string("external")));

    const auto command = fixture.edit.undo();

    REQUIRE_FALSE(command);
    CHECK(command.result().error() == TreeError::WOULD_BLOCK);
    CHECK(fixture.draft() == "draft");
    CHECK(fixture.outcomes.back().status == LineEditStatus::CONFLICT);
    CHECK_FALSE(fixture.edit.canUndo());

    if (reset) {
      take((fixture.edit.text(ProgrammaticChange::RESET_BASELINE) = "loaded").result());
    } else {
      take(fixture.app->router().invoke(fixture.edit.node(), {Action::CANCEL}));
    }

    CHECK(fixture.draft() == (reset ? "loaded" : "external"));
    CHECK_FALSE(fixture.edit.canUndo());
    take(fixture.app->router().invoke(fixture.edit.node(), {Action::ACTIVATE}));
    CHECK(fixture.value() == (reset ? "loaded" : "external"));
  }
}

TEST_CASE("empty history queries commands and equal reset request no frame or model notification") {
  Fixture fixture;

  const auto revision = take(fixture.model.snapshot())->revision;

  take(fixture.app->tree().consumeFrameRequest());

  const auto calls = fixture.outcomes.size();

  CHECK_FALSE(fixture.edit.canUndo());
  CHECK_FALSE(fixture.edit.canRedo());
  take(fixture.edit.undo().result());
  take(fixture.edit.redo().result());
  take(fixture.edit.stopGrouping().result());
  take((fixture.edit.text(ProgrammaticChange::RESET_BASELINE) = "base").result());
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK(take(fixture.model.snapshot())->revision == revision);
  CHECK(fixture.outcomes.size() == calls);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("history chords normalize exact modifiers and can be remapped to function keys") {
  ui::LineEditConfig config;
  config.options.historyShortcuts.undo = EditorChord{InputKey::F2};
  config.options.historyShortcuts.redo = {EditorChord{InputKey::F3}, std::nullopt};
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "next").result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::F2, 0, {}, KeyAction::RELEASE}));
  CHECK(fixture.draft() == "next");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::F2}));
  CHECK(fixture.draft() == "base");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::F3, 0, {}, KeyAction::REPEAT}));
  CHECK(fixture.draft() == "next");
  Fixture defaults;
  take((defaults.edit.text() = "next").result());
  take(defaults.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'Z', {.control = true}}));
  CHECK(defaults.draft() == "base");
  take(defaults.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'Z', {.shift = true, .control = true}}));
  CHECK(defaults.draft() == "next");
}

TEST_CASE("reserved router identities invalid scalar and Undo Redo overlap reject binding") {
  for (const EditorChord chord :
       {EditorChord{InputKey::ENTER}, EditorChord{InputKey::LEFT}, EditorChord{InputKey::TAB},
        EditorChord{InputKey::CHARACTER, 0x110000}, EditorChord{InputKey::CHARACTER, U'y', {.control = true}}}) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    const auto model = take(scene->state(std::string("base")));

    ui::LineEditConfig config;
    config.options.historyShortcuts.undo = chord;

    const auto mounted = ui::mount(*scene, ui::lineEdit(model, config));

    REQUIRE_FALSE(mounted);
    CHECK(std::get<TreeError>(mounted.error().cause) == TreeError::INVALID_ARGUMENT);
  }
}

TEST_CASE("exact returned command failure is reported once by declarative and late button adapters") {
  for (const bool late : {false, true}) {
    bool fail = false;
    ui::LineEditConfig config;
    config.validate = [&](std::string_view) {
      if (fail) {
        throw std::bad_alloc();
      }

      return true;
    };
    Fixture fixture(std::move(config));
    ui::Ref<Button> added;

    if (late) {
      take(ui::mount(*fixture.scene, ui::control<Button>("late", {}, {}).reference(added)));
      take(fixture.scene->onClick(added.node(), [edit = fixture.edit] {
        return edit.undo();
      }));
    }

    take((fixture.edit.text() = "next").result());
    fail = true;
    take(fixture.app->router().invoke(late ? added.node() : fixture.undoButton.node(), {Action::ACTIVATE}));

    const auto diagnostics = take(fixture.scene->diagnostics());

    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) == TreeError::NO_MEMORY);
    CHECK(diagnostics.errors[0].error.node == fixture.edit.node().id());
  }
}

TEST_CASE("independent result callback failure with the same cause is never suppressed") {
  bool fail = false;
  ui::LineEditConfig config;
  config.validate = [&](std::string_view) {
    if (fail) {
      throw std::bad_alloc();
    }

    return true;
  };
  config.result = [&](LineEditResult) -> std::expected<void, TreeError> {
    return fail ? std::expected<void, TreeError>(std::unexpected(TreeError::NO_MEMORY))
                : std::expected<void, TreeError>{};
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "next").result());
  fail = true;
  take(fixture.app->router().invoke(fixture.undoButton.node(), {Action::ACTIVATE}));

  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 2);
  CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) == TreeError::NO_MEMORY);
  CHECK(std::get<TreeError>(diagnostics.errors[1].error.cause) == TreeError::NO_MEMORY);
}

TEST_CASE("explicit expected extraction loses provenance and independent adapter throws still report") {
  for (const bool throws : {false, true}) {
    bool fail = false;
    ui::LineEditConfig config;
    config.validate = [&](std::string_view) {
      if (fail) {
        throw std::bad_alloc();
      }

      return true;
    };
    Fixture fixture(std::move(config));
    ui::Ref<Button> added;

    if (throws) {
      take(ui::mount(*fixture.scene, ui::button("throw", [edit = fixture.edit]() -> EditorCommandResult {
                                       const auto command = edit.undo();

                                       (void)command;
                                       throw std::bad_alloc();
                                     }).reference(added)));
    } else {
      take(ui::mount(*fixture.scene, ui::button("expected", [edit = fixture.edit] {
                                       return edit.undo().result();
                                     }).reference(added)));
    }

    take((fixture.edit.text() = "next").result());
    fail = true;
    take(fixture.app->router().invoke(added.node(), {Action::ACTIVATE}));
    CHECK(take(fixture.scene->diagnostics()).errors.size() == 2);
  }
}

TEST_CASE("disabled ancestry and read only availability queries do not report or mutate") {
  Fixture fixture;
  take((fixture.edit.text() = "next").result());
  take(fixture.app->tree().setEnabled(fixture.scene->root(), false));
  take(fixture.app->tree().consumeFrameRequest());
  CHECK_FALSE(fixture.edit.canUndo());
  REQUIRE(fixture.edit.historyAvailability());
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);

  const auto command = fixture.edit.undo();

  REQUIRE_FALSE(command);
  CHECK(command.result().error() == TreeError::WOULD_BLOCK);
  CHECK(fixture.draft() == "next");
}

TEST_CASE("validator removal and scene destruction retain command state without borrowed widgets") {
  for (const bool destroyScene : {false, true}) {
    bool remove = false;
    Fixture *fixture = nullptr;
    ui::LineEditConfig config;
    config.validate = [&](std::string_view) {
      if (remove) {
        if (destroyScene) {
          fixture->scene.reset();
        } else {
          take(fixture->scene->remove(fixture->edit.node()));
        }
      }

      return true;
    };
    Fixture owner(std::move(config));
    fixture = &owner;
    take((owner.edit.text() = "next").result());
    remove = true;

    const auto command = owner.edit.undo();

    REQUIRE_FALSE(command);
    CHECK(command.result().error() == TreeError::STALE_NODE);

    if (destroyScene) {
      CHECK_FALSE(owner.model.snapshot());
      CHECK(owner.edit.undo().result().error() == TreeError::CLOSED);
    } else {
      CHECK(owner.value() == "next");
      CHECK(take(owner.scene->diagnostics()).errors.size() == 1);
    }
  }
}

TEST_CASE("wrong thread frame and same binding reentry return checked errors without unsafe access") {
  Fixture fixture;
  take((fixture.edit.text() = "next").result());

  std::optional<TreeError> foreign;

  std::thread worker([&] {
    foreign = fixture.edit.undo().result().error();
  });
  worker.join();
  CHECK(foreign == TreeError::WRONG_THREAD);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  take(fixture.app->tree().frame([&] {
    CHECK(fixture.edit.undo().result().error() == TreeError::WOULD_BLOCK);
    CHECK(fixture.edit.historyAvailability().error() == TreeError::WOULD_BLOCK);
  }));
  CHECK(fixture.draft() == "next");
}

TEST_CASE("allocation sweeps distinguish unchanged rejected Undo from admitted unpublished Undo") {
  const std::string LONG_TEXT(256, 'x');

  bool rejected = false;
  bool unpublished = false;
  bool published = false;

  for (std::ptrdiff_t budget = 0; budget < 128; ++budget) {
    Fixture fixture;
    take((fixture.edit.text() = LONG_TEXT).result());
    allocationTest::remaining = budget;

    const auto command = fixture.edit.undo();

    allocationTest::remaining = -1;

    if (fixture.draft() == LONG_TEXT) {
      rejected = true;
      REQUIRE_FALSE(command);
      CHECK(command.result().error() == TreeError::NO_MEMORY);
      CHECK(fixture.value() == LONG_TEXT);
      CHECK(fixture.edit.canUndo());
      CHECK_FALSE(fixture.edit.canRedo());
    } else {
      CHECK(fixture.draft() == "base");
      CHECK_FALSE(fixture.edit.canUndo());
      CHECK(fixture.edit.canRedo());

      if (command) {
        published = true;
        CHECK(fixture.value() == "base");
      } else {
        unpublished = true;
        CHECK(command.result().error() == TreeError::NO_MEMORY);
        CHECK(fixture.value() == LONG_TEXT);
      }
    }
  }

  CHECK(rejected);
  CHECK(unpublished);
  CHECK(published);
}

TEST_CASE("empty editor Undo chord is handled without bubbling or delivering an edit result") {
  Fixture fixture;
  int parentCalls = 0;
  auto parent = take(fixture.app->router().bind(fixture.scene->root(), [&](Widget &, const RoutedEvent &) {
    ++parentCalls;

    return EventReply::HANDLED;
  }));

  const auto outcomes = fixture.outcomes.size();

  take(fixture.app->tree().consumeFrameRequest());

  const auto result = take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}));

  CHECK(result.status == DispatchStatus::HANDLED);
  CHECK(parentCalls == 0);
  CHECK(fixture.outcomes.size() == outcomes);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

namespace {
struct MemoryClipboard : Clipboard {
  std::string bytes = std::string(0, '\0');

  int reads{};
  int writes{};
  bool failWrite{};

  std::expected<std::string, TreeError> read(std::size_t) override {
    ++reads;

    return std::expected<std::string, TreeError>(std::in_place, bytes);
  }

  std::expected<void, TreeError> write(std::string_view value) override {
    ++writes;

    if (failWrite) {
      return std::unexpected(TreeError::NO_MEMORY);
    }

    bytes.assign(value);

    return {};
  }
};
} // namespace

TEST_CASE("clipboard paste and cut form separate operations and failed writes preserve history") {
  auto clipboard = std::make_shared<MemoryClipboard>();

  clipboard->bytes = "paste";
  ui::LineEditConfig config;
  config.clipboard = clipboard;
  Fixture fixture(std::move(config));
  take(fixture.app->router().dispatch(TextInputEvent{U'a'}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'v', {.control = true}}));
  take(fixture.app->router().dispatch(TextInputEvent{U'b'}));
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "baseapaste");
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "basea");
  take(fixture.edit.redo().result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'a', {.control = true}}));
  clipboard->failWrite = true;
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'x', {.control = true}}));
  CHECK(fixture.draft() == "baseapaste");
  CHECK(fixture.edit.canRedo());
  clipboard->failWrite = false;
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'x', {.control = true}}));
  CHECK(fixture.draft().empty());
  CHECK_FALSE(fixture.edit.canRedo());
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "baseapaste");
  take(fixture.app->tree().read(fixture.edit.node(), [](const LineEdit &edit) {
    CHECK(edit.cursor() == edit.text().bytes().size());
    CHECK(edit.anchor() == 0);
  }));
}

TEST_CASE("read only cut paste and typing invoke neither clipboard nor validator while copy remains available") {
  auto clipboard = std::make_shared<MemoryClipboard>();

  int validations = 0;
  ui::LineEditConfig config;
  config.clipboard = clipboard;
  config.options.readOnly = true;
  config.validate = [&](std::string_view) {
    ++validations;

    return true;
  };
  Fixture fixture(std::move(config));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'a', {.control = true}}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'x', {.control = true}}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'v', {.control = true}}));
  take(fixture.app->router().dispatch(PasteInputEvent{"paste"}));
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::BACKSPACE}));
  CHECK(clipboard->reads == 0);
  CHECK(clipboard->writes == 0);
  CHECK(validations == 0);
  CHECK(fixture.draft() == "base");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'c', {.control = true}}));
  CHECK(clipboard->bytes == "base");
  CHECK(clipboard->writes == 1);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("deletion grouping is direction specific and configurable typing grouping is respected") {
  Fixture fixture;
  take((fixture.edit.text() = "abcd").result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::BACKSPACE}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::BACKSPACE}));
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "abcd");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::HOME}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::DELETE_KEY}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::DELETE_KEY}));
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "abcd");
  take(fixture.app->tree().read(fixture.edit.node(), [](const LineEdit &edit) {
    CHECK(edit.cursor() == 0);
  }));
  ui::LineEditConfig config;
  config.options.history.groupTyping = false;
  Fixture separate(std::move(config));
  take(separate.app->router().dispatch(TextInputEvent{U'a'}));
  take(separate.app->router().dispatch(TextInputEvent{U'b'}));
  take(separate.edit.undo().result());
  CHECK(separate.draft() == "basea");
}

TEST_CASE("finite operation budget evicts complete groups and disabled history accepts normal edits") {
  ui::LineEditConfig bounded;
  bounded.options.history.maxOperations = 1;
  Fixture fixture(std::move(bounded));
  take((fixture.edit.text() = "first").result());
  take((fixture.edit.text() = "second").result());
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "first");
  CHECK_FALSE(fixture.edit.canUndo());
  take(fixture.edit.undo().result());
  CHECK(fixture.edit.canRedo());
  ui::LineEditConfig disabled;
  disabled.options.history.enabled = false;
  Fixture off(std::move(disabled));
  take((off.edit.text() = "next").result());
  CHECK_FALSE(off.edit.canUndo());
  take(off.edit.undo().result());
  CHECK(off.draft() == "next");
}

TEST_CASE("finite byte budget rejects fluent and keyboard edits before changing the draft") {
  ui::LineEditConfig config;
  config.options.history.maxBytes = 1;
  Fixture fixture(std::move(config));

  const auto programmatic = fixture.edit.text() = "next";

  REQUIRE_FALSE(programmatic.result());
  CHECK(std::get<TreeError>(programmatic.result().error().cause) == TreeError::WOULD_BLOCK);
  CHECK(fixture.draft() == "base");
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  CHECK(fixture.outcomes.back().status == LineEditStatus::LIMIT_EXCEEDED);
  CHECK(fixture.draft() == "base");
  CHECK_FALSE(fixture.edit.canUndo());
  take((fixture.edit.text(ProgrammaticChange::RESET_BASELINE) = "load").result());
  CHECK(fixture.value() == "load");
}

TEST_CASE("known conflict permits ordinary draft editing but blocks Undo and external publication") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "draft").result());
  take(fixture.model.set(std::string("external")));
  take((fixture.edit.text() = "revised").result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::HOME}));
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  CHECK(fixture.draft() == "xrevised");

  const auto command = fixture.edit.undo();

  REQUIRE_FALSE(command);
  CHECK(command.result().error() == TreeError::WOULD_BLOCK);
  CHECK(fixture.draft() == "xrevised");
  CHECK(fixture.value() == "external");
}

TEST_CASE("pre-admission Ref failure and absent source command are reported once by a managed button") {
  for (const bool emptySource : {false, true}) {
    ui::LineEditConfig config;
    config.options.readOnly = true;
    Fixture fixture(std::move(config));
    ui::Ref<LineEdit> empty;
    ui::Ref<Button> button;
    take(ui::mount(*fixture.scene, ui::button("command", [source = emptySource ? empty : fixture.edit] {
                                     return source.undo();
                                   }).reference(button)));
    take(fixture.app->router().invoke(button.node(), {Action::ACTIVATE}));

    const auto diagnostics = take(fixture.scene->diagnostics());

    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) ==
          (emptySource ? TreeError::CLOSED : TreeError::WOULD_BLOCK));
  }
}

TEST_CASE("commands reported to a different collector are not suppressed in the receiving button scene") {
  ui::LineEditConfig config;
  config.options.readOnly = true;
  Fixture source(std::move(config));
  Fixture receiver;
  ui::Ref<Button> button;
  take(ui::mount(*receiver.scene, ui::button("cross scene", [edit = source.edit] {
                                    return edit.undo();
                                  }).reference(button)));
  take(receiver.app->router().invoke(button.node(), {Action::ACTIVATE}));
  CHECK(take(source.scene->diagnostics()).errors.size() == 1);
  CHECK(take(receiver.scene->diagnostics()).errors.size() == 1);
}

TEST_CASE("same-binding validator command reentry preserves the admitted history and reports its own failure") {
  Fixture *owner = nullptr;
  bool nested = false;
  ui::LineEditConfig config;
  config.validate = [&](std::string_view) {
    if (nested) {
      const auto result = owner->edit.redo();

      CHECK(result.result().error() == TreeError::WOULD_BLOCK);
      CHECK_FALSE(owner->edit.canUndo());
    }

    return true;
  };
  Fixture fixture(std::move(config));
  owner = &fixture;
  take((fixture.edit.text() = "next").result());
  nested = true;
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "base");
  CHECK(fixture.edit.canRedo());
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
}

TEST_CASE("fluent history allocation failures preserve dirty text and Redo until acceptance") {
  const std::string LONG_TEXT(256, 'x');

  for (const auto change : {ProgrammaticChange::RECORD, ProgrammaticChange::RESET_BASELINE}) {
    bool rejected = false;
    bool accepted = false;

    for (std::ptrdiff_t budget = 0; budget < 128; ++budget) {
      ui::LineEditConfig config;
      config.options.editMode = EditMode::EXPLICIT;
      Fixture fixture(std::move(config));
      take((fixture.edit.text() = "first").result());
      take((fixture.edit.text() = "second").result());
      take(fixture.edit.undo().result());

      auto property = fixture.edit.text(change);

      allocationTest::remaining = budget;

      const auto operation = property.set(LONG_TEXT);

      allocationTest::remaining = -1;

      if (!operation.result()) {
        rejected = true;
        CHECK(std::get<TreeError>(operation.result().error().cause) == TreeError::NO_MEMORY);
        CHECK(fixture.draft() == "first");
        CHECK(fixture.edit.canRedo());
      } else {
        accepted = true;
        CHECK(fixture.draft() == LONG_TEXT);
        CHECK_FALSE(fixture.edit.canRedo());
        CHECK(fixture.edit.canUndo() == (change == ProgrammaticChange::RECORD));
      }

      CHECK(fixture.value() == "base");
    }

    CHECK(rejected);
    CHECK(accepted);
  }
}

TEST_CASE("admitted command failure preserves its exact operation and public origin in the single diagnostic") {
  bool fail = false;
  ui::LineEditConfig config;
  config.validate = [&](std::string_view) {
    if (fail) {
      throw std::bad_alloc();
    }

    return true;
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "next").result());

  const auto origin = std::source_location::current();

  fail = true;

  const auto command = fixture.edit.undo(origin);

  REQUIRE_FALSE(command);

  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  REQUIRE(diagnostics.errors[0].contexts.size() == 1);
  CHECK(diagnostics.errors[0].contexts[0].operation == "lineEdit.undo");
  CHECK(diagnostics.errors[0].origin.line() == origin.line());
}

TEST_CASE("compact Undo button survives scene destruction in validator and result callbacks") {
  for (const bool inValidator : {false, true}) {
    bool destroy = false;
    Fixture *owner = nullptr;
    ui::LineEditConfig config;
    config.validate = [&](std::string_view) {
      if (destroy && inValidator) {
        owner->scene.reset();
      }

      return true;
    };
    config.result = [&](LineEditResult) -> std::expected<void, TreeError> {
      if (destroy && !inValidator) {
        owner->scene.reset();
      }

      return {};
    };
    Fixture fixture(std::move(config));
    owner = &fixture;
    take((fixture.edit.text() = "next").result());
    destroy = true;
    take(fixture.app->router().invoke(fixture.undoButton.node(), {Action::ACTIVATE}));
    CHECK_FALSE(fixture.scene);
    CHECK(fixture.edit.undo().result().error() == TreeError::CLOSED);
  }
}

TEST_CASE("checked action adapters tolerate defensive manual-router teardown") {
  enum class Outcome {
    SUCCESS,
    FAILURE,
    THROW
  };

  for (const bool composed : {false, true}) {
    for (const auto outcome : {Outcome::SUCCESS, Outcome::FAILURE, Outcome::THROW}) {
      Fixture fixture;
      ui::Ref<Button> button;

      take(ui::mount(*fixture.scene, ui::button(button, "teardown")));

      const auto node = button.node();
      auto action = [&]() -> std::expected<void, ManagedError> {
        fixture.scene.reset();

        if (outcome == Outcome::THROW) {
          throw std::runtime_error("after teardown");
        }

        if (outcome == Outcome::FAILURE) {
          return std::unexpected(ManagedError{ManagedPhase::INPUT, node.id(), TreeError::CALLBACK_FAILED, {}, {}});
        }

        return {};
      };

      if (composed) {
        take(ui::detail::ClickAccess::attach(*fixture.scene, button, action, std::source_location::current()));
      } else {
        take(fixture.scene->onClick(node, action));
      }

      // The application/router stay alive; this does not admit teardown during Scene::run or prepare.
      take(fixture.app->router().invoke(node, {Action::ACTIVATE}));
      CHECK_FALSE(fixture.scene);
      const auto observation = button.text().get();

      REQUIRE_FALSE(observation);
      CHECK(std::get<TreeError>(observation.error().cause) == TreeError::CLOSED);
    }
  }
}

TEST_CASE("programmatic history override remains owning across concatenation resource resolution and Ref destruction") {
  Fixture fixture;
  ui::Ref<Label> suffix;
  take(ui::mount(*fixture.scene, ui::label("suffix").reference(suffix)));
  take((fixture.edit.text() = "next").result());
  auto property = fixture.edit.text(ProgrammaticChange::RESET_BASELINE,
                                    [](std::string_view) -> std::expected<std::string, TextResourceError> {
                                      return std::string("resolved");
                                    });

  auto observation = fixture.edit;

  fixture.edit = ui::Ref<LineEdit>{};
  take((property = suffix.text() + "!").result());
  CHECK(take(observation.text().get()) == "suffix!");
  CHECK_FALSE(observation.canUndo());
  take(property.append(i18n("key")).result());
  CHECK(take(observation.text().get()) == "suffix!resolved");
  CHECK_FALSE(observation.canUndo());
}

TEST_CASE("rejected Undo model notifications do not reset history or repeat the admitted result") {
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  Fixture fixture({}, options);
  take((fixture.edit.text() = "next").result());
  take(fixture.edit.undo().result());
  REQUIRE(fixture.outcomes.back().commit);
  CHECK(fixture.outcomes.back().commit->rejected == 1);
  CHECK(fixture.value() == "base");
  CHECK(fixture.edit.canRedo());
  const auto outcomes = fixture.outcomes.size();
  take(fixture.app->scheduler().dispatch());
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  CHECK(fixture.edit.canRedo());
  CHECK(fixture.outcomes.size() == outcomes);
  take(fixture.edit.redo().result());
  CHECK(fixture.value() == "next");
}

TEST_CASE("clean external revision with equal bytes resets history without another redraw") {
  Fixture fixture;
  take((fixture.edit.text() = "next").result());
  const auto revision = take(fixture.model.snapshot())->revision;
  take(fixture.model.set(std::string("external")));
  take(fixture.model.set(std::string("next")));
  REQUIRE(take(fixture.model.snapshot())->revision != revision);
  take(fixture.app->tree().consumeFrameRequest());
  take(fixture.edit.undo().result());
  CHECK(fixture.draft() == "next");
  CHECK_FALSE(fixture.edit.canUndo());
  CHECK_FALSE(fixture.edit.canRedo());
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("low-level binding commands preserve move ownership and checked eligibility") {
  auto app = take(Application::create());
  const auto root = take(app->tree().root());
  const auto node = take(app->tree().emplace<LineEdit>(root));
  auto model = std::shared_ptr<Property<std::string>>(
    take(Property<std::string>::create(app->tree(), root, std::string("base"), Invalidation::LAYOUT)));
  int outcomes = 0;
  auto binding = take(bindLineEdit(app->tree(), app->router(), node, model, [&](LineEditResult) {
    ++outcomes;
  }));
  take(app->router().setFocus(node));
  take(app->router().dispatch(TextInputEvent{U'x'}));
  REQUIRE(binding.canUndo());
  auto moved = std::move(binding);
  CHECK(binding.undo().error() == TreeError::INVALID_ARGUMENT);
  CHECK(binding.historyAvailability().error() == TreeError::INVALID_ARGUMENT);
  take(moved.undo());
  CHECK(take(model->snapshot())->value == "base");
  CHECK(moved.canRedo());
  take(moved.stopGrouping());
  take(app->tree().setEnabled(node, false));
  CHECK_FALSE(moved.canRedo());
  CHECK(moved.redo().error() == TreeError::WOULD_BLOCK);
  CHECK(outcomes == 2);
}

TEST_CASE("binding allocation sweeps reject before initial adoption and leave registration retryable") {
  bool rejected = false;
  bool accepted = false;
  for (std::ptrdiff_t budget = 0; budget < 96; ++budget) {
    auto app = take(Application::create());
    const auto root = take(app->tree().root());
    const auto node = take(app->tree().emplace<LineEdit>(root));
    auto model = std::shared_ptr<Property<std::string>>(
      take(Property<std::string>::create(app->tree(), root, std::string("base"), Invalidation::LAYOUT)));
    std::function<void(LineEditResult)> callback = [](LineEditResult) {
    };
    allocationTest::remaining = budget;
    auto binding = bindLineEdit(app->tree(), app->router(), node, model, std::move(callback));
    allocationTest::remaining = -1;
    if (!binding) {
      rejected = true;
      CHECK(binding.error() == TreeError::NO_MEMORY);
      take(app->tree().read(node, [](const LineEdit &edit) {
        CHECK(edit.text().bytes().empty());
      }));
      auto retry = take(bindLineEdit(app->tree(), app->router(), node, model, [](LineEditResult) {
      }));
      take(app->tree().read(node, [](const LineEdit &edit) {
        CHECK(edit.text().bytes() == "base");
      }));
    } else {
      accepted = true;
      CHECK_FALSE(binding->canUndo());
      take(app->tree().read(node, [](const LineEdit &edit) {
        CHECK(edit.text().bytes() == "base");
      }));
    }
  }
  CHECK(rejected);
  CHECK(accepted);
}
