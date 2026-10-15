// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <portals/style_static.hpp>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};

void oomTerminate() {
  // Keep an actual noexcept violation observable without recursively faulting the test reporter.
  allocationsBeforeFailure = -1;
  std::abort();
}
} // namespace

// Process-local allocation failure is armed only around the public boundary under test.
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
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

bool treeCause(const ManagedError &failure, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&failure.cause);

  return value && *value == cause;
}

struct Lifetime {
  int &destroyed;

  ~Lifetime() {
    ++destroyed;
  }
};

struct Mounted final : Widget {
  int &alive;

  explicit Mounted(int &count) : alive(count) {
    ++alive;
  }

  ~Mounted() override {
    --alive;
  }
};

struct InvalidNativeInput {
#ifdef _WIN32
  HANDLE previous = GetStdHandle(STD_INPUT_HANDLE);

  InvalidNativeInput() {
    REQUIRE(SetStdHandle(STD_INPUT_HANDLE, INVALID_HANDLE_VALUE));
  }

  ~InvalidNativeInput() {
    SetStdHandle(STD_INPUT_HANDLE, previous);
  }
#else
  int previous = dup(STDIN_FILENO);

  InvalidNativeInput() {
    REQUIRE(previous >= 0);
    REQUIRE(::close(STDIN_FILENO) == 0);
  }

  ~InvalidNativeInput() {
    dup2(previous, STDIN_FILENO);
    ::close(previous);
  }
#endif
};

struct WrongConfiguration {
  int operator()(ManagedScene &) const {
    return 0;
  }
};

struct WrongLegacyComposition {
  int operator()(ManagedScene &) const;
  ui::Description<Label> operator()(ManagedScene &, Application &) const;
};

struct BorrowedComposition {
  ui::Description<Label> &operator()(ManagedScene &, Application &) const;
};

struct SceneFactory {
  ui::Scene operator()() const;
};

struct WrongProgram {
  void operator()() const;
};

template <class Callable>
concept ProgramEntry = requires(Callable function) { ui::runApp(function); };

struct FailingTerminal : example::ReplayTerminal {
  using example::ReplayTerminal::ReplayTerminal;

  std::expected<CellSize, TerminalError> size() const override {
    return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 7});
  }

  std::expected<void, TerminalError> close() noexcept override {
    (void)example::ReplayTerminal::close();

    return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 9});
  }
};

struct StderrCapture {
  FILE *file{};
  int previous{};
  bool restored{};

  StderrCapture() {
#ifdef _WIN32
    REQUIRE(tmpfile_s(&file) == 0);
#else
    file = std::tmpfile();
#endif
    REQUIRE(file);
    std::fflush(stderr);
#ifdef _WIN32
    previous = _dup(_fileno(stderr));
    REQUIRE(previous >= 0);
    REQUIRE(_dup2(_fileno(file), _fileno(stderr)) == 0);
#else
    previous = dup(fileno(stderr));
    REQUIRE(previous >= 0);
    REQUIRE(dup2(fileno(file), fileno(stderr)) >= 0);
#endif
  }

  void restore() {
    if (restored) {
      return;
    }

    std::fflush(stderr);
#ifdef _WIN32
    _dup2(previous, _fileno(stderr));
    _close(previous);
#else
    dup2(previous, fileno(stderr));
    ::close(previous);
#endif
    restored = true;
  }

  std::string contents() {
    restore();
    std::rewind(file);

    std::string result;
    char buffer[512];

    while (const auto count = std::fread(buffer, 1, sizeof(buffer), file)) {
      result.append(buffer, count);
    }

    return result;
  }

  ~StderrCapture() {
    restore();
    std::fclose(file);
  }
};

struct OrderedFailureTerminal : FailingTerminal {
  using FailingTerminal::FailingTerminal;
  long stderrAtClose{-1};

  std::expected<void, TerminalError> close() noexcept override {
    std::fflush(stderr);
#ifdef _WIN32
    stderrAtClose = _tell(_fileno(stderr));
#else
    stderrAtClose = static_cast<long>(lseek(fileno(stderr), 0, SEEK_CUR));
#endif

    return FailingTerminal::close();
  }
};

struct AllocationFailureTerminal : FailingTerminal {
  using FailingTerminal::FailingTerminal;

  std::expected<void, TerminalError> close() noexcept override {
    auto result = FailingTerminal::close();

    allocationsBeforeFailure = 0;

    return result;
  }
};

static_assert(!ui::detail::SceneConfiguration<WrongConfiguration>);
static_assert(!ui::detail::SceneConfiguration<WrongLegacyComposition>);
static_assert(!ui::detail::SceneFactory<WrongLegacyComposition>);
static_assert(!ui::detail::SceneFactory<BorrowedComposition>);
static_assert(!ProgramEntry<SceneFactory>);
static_assert(!ProgramEntry<WrongProgram>);
static_assert(!std::is_move_assignable_v<ui::Scene>);
static_assert(!std::is_copy_constructible_v<ui::Scene>);

template <class Input, class Stage>
concept PipeExpression =
  requires(Input &&input, Stage &&stage) { std::forward<Input>(input) | std::forward<Stage>(stage); };

using PrepareStage = decltype(ui::prepare());
using RunStage = decltype(ui::run());
using RunAppStage = decltype(ui::runApp());
static_assert(PipeExpression<ui::Scene &, PrepareStage &>);
static_assert(PipeExpression<ui::Scene &, PrepareStage>);
static_assert(PipeExpression<ui::Scene, PrepareStage &>);
static_assert(PipeExpression<ui::Scene, PrepareStage>);
static_assert(!PipeExpression<const ui::Scene &, PrepareStage>);
static_assert(!PipeExpression<ui::Scene &, const PrepareStage &>);
static_assert(!PipeExpression<ui::Description<Label>, PrepareStage>);
static_assert(!PipeExpression<PrepareStage, PrepareStage>);
static_assert(!PipeExpression<ui::Scene &, RunStage>);
static_assert(PipeExpression<ui::Scene, RunStage>);
static_assert(PipeExpression<ui::Scene &, RunAppStage>);
static_assert(PipeExpression<ui::Scene, RunAppStage>);
static_assert(std::same_as<decltype(std::declval<ui::Scene &>() | std::declval<PrepareStage &>()), ui::Scene &>);
static_assert(std::same_as<decltype(std::declval<ui::Scene>() | std::declval<PrepareStage>()), ui::Scene>);
} // namespace

TEST_CASE("lazy scene prepares an ordinary form once without terminal I/O") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::Ref<LineEdit> field;
  ui::Ref<Button> button;
  ui::SceneOptions options;
  int factories{}, configured{}, clicked{};

  options.clock = terminal.clock();

  auto scene = ui::scene(
    [&](ManagedScene &) {
      ++factories;

      return ui::column(ui::lineEdit(field, "initial"), ui::button(button, "Apply"));
    },
    options);

  scene
    .configure([&](ManagedScene &) -> std::expected<void, ManagedError> {
      ++configured;

      return button.onClick([&] {
        ++clicked;
      });
    })
    .prepare()
    .prepare();

  take(scene.checked());
  CHECK(factories == 1);
  CHECK(configured == 1);
  CHECK(terminal.output.empty());
  CHECK_FALSE(terminal.closed);
  CHECK(take(field.text().get()) == "initial");
  take(scene.run(terminal));
  CHECK(terminal.closed);
  CHECK_FALSE(field.text().get());
  CHECK_FALSE(scene.run(terminal));
}

TEST_CASE("scene composition borrows its existing application and prefers legacy generic callbacks") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  Application *created{};
  int configured{}, stopped{};

  options.clock = terminal.clock();

  auto scene = ui::scene(
    [&](ManagedScene &owner, Application &app) {
      created = &app;
      CHECK(owner.widthPolicy() == app.widthPolicy());

      return ui::lineEdit(field, "ready");
    },
    options);

  scene.configure([&](ManagedScene &, Application &app) -> std::expected<void, ManagedError> {
    CHECK(&app == created);
    ++configured;

    auto focused = app.router().setFocus(field.node());

    return focused ? std::expected<void, ManagedError>{}
                   : std::unexpected(ManagedError{ManagedPhase::BIND, field.node().id(), focused.error(), {}, {}});
  });
  scene.onStop([&] {
    ++stopped;
    CHECK(take(field.text().get()) == "ready");
  });
  take(scene.run(terminal));
  CHECK(configured == 1);
  CHECK(stopped == 1);
  take(scene.close());
  CHECK(stopped == 1);

  auto legacy = ui::scene([](auto &, auto &...extra) {
    static_assert(sizeof...(extra) == 0);

    return ui::label("legacy");
  });

  legacy.configure([](auto &, auto &...extra) {
    static_assert(sizeof...(extra) == 0);
  });
  take(legacy.prepareChecked());
  take(legacy.close());
}

TEST_CASE("scene stop hook runs once after partial preparation and before managed retirement") {
  for (const bool failFactory : {false, true}) {
    ui::Ref<Label> label;
    int stopped{};

    auto scene = ui::scene([&](ManagedScene &) -> std::expected<ui::Description<Label>, ManagedError> {
      if (failFactory) {
        return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::INVALID_ARGUMENT, {}, {}});
      }

      return ui::label(label, "mounted");
    });

    scene.onStop([&] {
      ++stopped;

      if (!failFactory) {
        CHECK(take(label.text().get()) == "mounted");
      }
    });
    scene.configure([](ManagedScene &) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    });

    CHECK_FALSE(scene.prepareChecked());
    CHECK(stopped == 1);
    take(scene.close());
    CHECK(stopped == 1);
    CHECK_FALSE(label.text().get());
  }
}

TEST_CASE("scene stop exceptions retain cleanup and never prevent mounted widget retirement") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Label> label;
  int stopped{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label(label, "mounted"), options);

  scene.onStop([&] {
    ++stopped;

    throw std::runtime_error("stop failed");
  });

  const auto result = scene.run(terminal);

  REQUIRE_FALSE(result);
  CHECK(result.error().phase == ManagedPhase::CLEANUP);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::CALLBACK_FAILED);
  CHECK(stopped == 1);
  CHECK(terminal.closed);
  CHECK_FALSE(label.text().get());
  take(scene.close());
  CHECK(stopped == 1);
}

TEST_CASE("scene stop preserves prior runner and restoration failures") {
  FailingTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int stopped{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("mounted"), options);

  scene.onStop([&]() -> std::expected<void, ManagedError> {
    ++stopped;

    return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
  });

  const auto result = scene.run(terminal);

  REQUIRE_FALSE(result);
  CHECK(std::holds_alternative<ApplicationError>(result.error().cause));
  REQUIRE(result.error().application);
  CHECK(result.error().application->restoration.has_value());
  CHECK(result.error().cleanup == TreeError::INVALID_ARGUMENT);
  CHECK(stopped == 1);

  const auto diagnostics = take(scene.diagnostics());

  CHECK(std::ranges::any_of(diagnostics.errors, [](const auto &diagnostic) {
    return diagnostic.error.phase == ManagedPhase::CLEANUP && treeCause(diagnostic.error, TreeError::INVALID_ARGUMENT);
  }));
}

TEST_CASE("scene stop reentry is rejected and idle destruction still stops once") {
  int stopped{};

  {
    auto scene = ui::scene(ui::label("idle"));

    scene.onStop([&] {
      ++stopped;

      const auto closed = scene.close();

      REQUIRE_FALSE(closed);
      CHECK(std::get<TreeError>(closed.error().cause) == TreeError::WOULD_BLOCK);
    });
  }

  CHECK(stopped == 1);
}

TEST_CASE("scene stop preserves exact returned failure and independent root cleanup failure") {
  for (const bool blockRemoval : {false, true}) {
    Application *application{};
    int stopped{};
    auto scene = ui::scene([&](ManagedScene &, Application &app) {
      application = &app;

      return ui::label("mounted");
    });
    const auto origin = std::source_location::current();

    scene.onStop([&]() -> std::expected<void, ManagedError> {
      ++stopped;
      ManagedError failure{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}};

      failure.operation = "provider.stop";
      failure.operationOrigin = origin;

      return std::unexpected(failure);
    });
    take(scene.prepareChecked());

    const auto close = [&] {
      const auto result = scene.close();

      REQUIRE_FALSE(result);
      CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
      CHECK(std::string_view(result.error().operation) == "provider.stop");
      CHECK(result.error().operationOrigin->line() == origin.line());
      CHECK(result.error().cleanup == (blockRemoval ? std::optional{TreeError::WOULD_BLOCK} : std::nullopt));
    };

    if (blockRemoval) {
      take(application->tree().frame(close));
    } else {
      close();
    }

    take(scene.close());
    CHECK(stopped == 1);
  }
}

TEST_CASE("scene stop body and owned capture retirement suspend unrelated current and setup contexts") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Label> missing;
  int stopped{}, retired{}, later{};
  bool attributed{}, currentAvailable{};

  options.clock = terminal.clock();

  struct Capture {
    ui::Ref<Label> &missing;
    int &retired;
    bool &attributed;
    bool &currentAvailable;

    ~Capture() {
      currentAvailable = ui::detail::SceneAccess::currentRef().quit().has_value();
      ui::quit();

      const auto result = missing.text() = "retiring stop";

      attributed = result.result().error().setupStopped;
      ++retired;
    }
  };

  auto inner = ui::scene(ui::label("inner"));
  auto outer = ui::scene(ui::label("outer"), options);

  inner.onStop([&, capture = std::make_unique<Capture>(missing, retired, attributed, currentAvailable)] {
    ++stopped;
    CHECK_FALSE(ui::detail::SceneAccess::currentRef().quit());
    ui::quit();

    const auto result = missing.text() = "stopping";

    CHECK_FALSE(result.result().error().setupStopped);
  });
  outer.configure([&](ManagedScene &) {
    take(inner.close());
  });
  outer.configure([&](ManagedScene &) {
    ++later;
  });

  take(outer.prepareChecked());
  CHECK(stopped == 1);
  CHECK(retired == 1);
  CHECK_FALSE(attributed);
  CHECK_FALSE(currentAvailable);
  CHECK(later == 1);
  take(outer.run(terminal));
  CHECK_FALSE(terminal.output.empty());
}

TEST_CASE("scene stop handles pre-owner preparation failure and foreign registration") {
  int stopped{}, foreign{};
  ui::SceneOptions options;

  options.application.framesPerSecond = 0;

  auto scene = ui::scene(ui::label("unprepared"), options);

  scene.onStop([&] {
    ++stopped;
  });

  std::thread worker([&] {
    scene.onStop([&] {
      ++foreign;
    });

    const auto closed = scene.close();

    REQUIRE_FALSE(closed);
    CHECK(treeCause(closed.error(), TreeError::WRONG_THREAD));
  });

  worker.join();
  CHECK_FALSE(scene.prepareChecked());
  CHECK(stopped == 1);
  CHECK(foreign == 0);
  take(scene.close());
  CHECK(stopped == 1);
}

TEST_CASE("scene stop retains an exact secondary error without a managed owner") {
  ui::SceneOptions options;
  int stopped{};

  options.application.framesPerSecond = 0;

  auto scene = ui::scene(ui::label("unprepared"), options);
  const auto origin = std::source_location::current();
  const ApplicationError shutdown{ApplicationPhase::STOP, TerminalError{TerminalErrorKind::SYSTEM, 37},
                                  TerminalError{TerminalErrorKind::SYSTEM, 41}};

  scene.onStop([&]() -> std::expected<void, ManagedError> {
    ++stopped;
    ManagedError failure{ManagedPhase::CLEANUP, {}, shutdown, shutdown, {}};

    failure.operation = "provider.stop";
    failure.operationOrigin = origin;
    failure.registrationOrigin = origin;

    return std::unexpected(failure);
  });

  const auto result = scene.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(result.error().phase == ManagedPhase::PREPARE);
  CHECK(result.error().cleanup == TreeError::CALLBACK_FAILED);

  const auto diagnostics = take(scene.diagnostics());

  REQUIRE(diagnostics.errors.size() == 2);
  const auto &secondary = diagnostics.errors[1];

  CHECK(std::string_view(secondary.error.operation) == "provider.stop");
  CHECK(secondary.origin.line() == origin.line());
  CHECK(secondary.error.registrationOrigin->line() == origin.line());
  REQUIRE(std::holds_alternative<ApplicationError>(secondary.error.cause));
  const auto &cause = std::get<ApplicationError>(secondary.error.cause);

  CHECK(cause.phase == ApplicationPhase::STOP);
  CHECK(std::get<TerminalError>(cause.cause).native_code == 37);
  REQUIRE(cause.restoration);
  CHECK(cause.restoration->native_code == 41);
  REQUIRE(secondary.error.application);
  CHECK(secondary.error.application->restoration->native_code == 41);
  CHECK(stopped == 1);
  take(scene.close());
  CHECK(take(scene.diagnostics()).errors.size() == 2);

  options.managed.diagnostics.maxErrors = 1;

  auto limited = ui::scene(ui::label("bounded fallback"), options);

  limited.onStop([&]() -> std::expected<void, ManagedError> {
    return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, shutdown, {}, {}});
  });
  CHECK_FALSE(limited.prepareChecked());

  const auto bounded = take(limited.diagnostics());

  CHECK(bounded.errors.size() == 1);
  CHECK(bounded.droppedErrors == 1);
}

TEST_CASE("scene direct quit rejects shutdown reentry before completed state") {
  for (const bool run : {false, true}) {
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    int stopped{};

    options.clock = terminal.clock();

    auto scene = ui::scene(ui::label("mounted"), options);
    const auto observer = scene.ref();

    scene.onStop([&] {
      ++stopped;

      const auto direct = scene.quit();
      const auto weak = observer.quit();

      CHECK_FALSE(direct);

      if (!direct) {
        CHECK(treeCause(direct.error(), TreeError::WOULD_BLOCK));
      }

      REQUIRE_FALSE(weak);
      CHECK(treeCause(weak.error(), run ? TreeError::WOULD_BLOCK : TreeError::CLOSED));
    });

    if (run) {
      take(scene.run(terminal));
    } else {
      take(scene.prepareChecked());
      take(scene.close());
    }

    CHECK(scene.checked());
    CHECK(stopped == 1);
  }
}

TEST_CASE("managed run rejects a stop hook before entering application terminal work") {
  struct CountingTerminal : example::ReplayTerminal {
    using example::ReplayTerminal::ReplayTerminal;
    mutable int sizes{};
    int reads{};

    std::expected<CellSize, TerminalError> size() const override {
      ++sizes;

      return example::ReplayTerminal::size();
    }

    TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
      ++reads;

      return example::ReplayTerminal::read(deadline, stop);
    }
  } terminal({}, 100, {30, 4});

  ui::SceneOptions options;
  ui::Ref<Label> label;
  ManagedScene *owner{};

  options.clock = terminal.clock();

  auto scene = ui::scene(
    [&](ManagedScene &managed) {
      owner = &managed;

      return ui::label(label, "mounted");
    },
    options);

  scene.onStop([&] {
    const auto result = owner->run(terminal);

    REQUIRE_FALSE(result);
    CHECK(treeCause(result.error(), TreeError::WOULD_BLOCK));
    CHECK(terminal.sizes == 0);
    CHECK(terminal.reads == 0);
    CHECK_FALSE(terminal.closed);
    CHECK(terminal.output.empty());
    CHECK(take(label.text().get()) == "mounted");
  });
  take(scene.prepareChecked());
  take(scene.close());
}

TEST_CASE("standard scene execution supports deferred idempotent quit") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::Ref<LineEdit> field;
  ui::SceneOptions options;

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::lineEdit(field, "ready"), options);

  take(scene.quit());
  take(scene.quit());
  scene.prepare();

  REQUIRE(scene.checked());
  CHECK(take(field.text().get()) == "ready");
  CHECK(scene.runApp(terminal) == 0);
  CHECK(terminal.closed);
  CHECK(terminal.output.empty());
  CHECK(treeCause(scene.quit().error(), TreeError::CLOSED));
}

TEST_CASE("quit during recipe and configuration waits for complete successful preparation") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::Ref<LineEdit> field;
  ui::SceneOptions options;

  options.clock = terminal.clock();

  ui::Scene scene = ui::scene(
    [&](ManagedScene &) {
      take(scene.quit());

      return ui::lineEdit(field, "mounted after quit");
    },
    options);

  scene.configure([&](ManagedScene &) {
    take(scene.quit());
    field.text() += " and configured";
  });
  scene.prepare();

  REQUIRE(scene.checked());
  CHECK(take(field.text().get()) == "mounted after quit and configured");
  CHECK(scene.runApp(terminal) == 0);
  CHECK(terminal.closed);
}

TEST_CASE("compact button and key callbacks quit without retiring the active closure") {
  for (bool key : {false, true}) {
    example::ReplayTerminal terminal({{10, key ? "\x1b[21~" : "\r", {}}}, 100, {30, 4});
    ui::SceneOptions options;
    int alive{};
    bool afterQuit{};

    options.clock = terminal.clock();

    ui::Scene scene = ui::scene(ui::column(ui::custom<Mounted>(std::ref(alive)), ui::button("Quit",
                                                                                            [&] {
                                                                                              take(scene.quit());
                                                                                              take(scene.quit());
                                                                                              CHECK(alive == 1);
                                                                                              afterQuit = true;
                                                                                            })),
                                options);

    scene.onKey(ui::key(InputKey::F10), [&] {
      take(scene.quit());
      CHECK(alive == 1);
      afterQuit = true;
    });

    CHECK(scene.runApp(terminal) == 0);
    CHECK(afterQuit);
    CHECK(alive == 0);
    CHECK(terminal.closed);
  }
}

TEST_CASE("quit rejects foreign thread moved facade and notification reentry") {
  auto scene = ui::scene(ui::label("ready"));
  bool foreign{};

  std::thread worker([&] {
    foreign = treeCause(scene.quit().error(), TreeError::WRONG_THREAD);
  });

  worker.join();
  CHECK(foreign);
  REQUIRE(scene.checked());

  auto moved = std::move(scene);

  CHECK(treeCause(scene.quit().error(), TreeError::CLOSED));
  take(moved.quit());

  auto notified = ui::scene(ui::label("observer"));
  int calls{};

  notified.onError([&](const ManagedDiagnostic &) {
    ++calls;
    CHECK(treeCause(notified.quit().error(), TreeError::WOULD_BLOCK));
  });
  notified.prepare().configure([](ManagedScene &owner) {
    take(owner.reportError({ManagedPhase::INPUT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
  });

  CHECK(calls == 1);
  CHECK(treeCause(notified.checked().error(), TreeError::WOULD_BLOCK));
}

TEST_CASE("runApp reports one final primary with restoration after terminal close and cleanup") {
  StderrCapture capture;
  OrderedFailureTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int alive{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::custom<Mounted>(std::ref(alive)), options);

  CHECK(scene.runApp(terminal) == 1);
  CHECK(terminal.closed);
  CHECK(terminal.stderrAtClose == 0);
  CHECK(alive == 0);
  CHECK(scene.runApp(terminal) == 1);

  const auto output = capture.contents();

  CHECK(output.find("Portals: scene failed") == 0);
  CHECK(output.find("restoration=") != std::string::npos);
  CHECK(output.find("native=9") != std::string::npos);
  CHECK(std::count(output.begin(), output.end(), '\n') == 1);
}

TEST_CASE("custom observer exceptions preserve the primary and independent default output") {
  StderrCapture capture;
  FailingTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int calls{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options);

  scene.onError([&](const ManagedDiagnostic &) {
    ++calls;

    throw std::runtime_error("observer failure");
  });

  CHECK(scene.runApp(terminal) == 1);
  CHECK(scene.runApp(terminal) == 1);
  CHECK(calls == 1);
  CHECK(std::holds_alternative<ApplicationError>(scene.checked().error().cause));
  CHECK(take(scene.diagnostics()).observerFailures == 1);
  CHECK(capture.contents().find("Portals: scene failed") == 0);
}

TEST_CASE("default recoverable summary is nonfatal and never requests an idle frame") {
  StderrCapture capture;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int renders{};

  options.clock = terminal.clock();
  options.application.observer = {[](void *context, ApplicationPhase phase, std::chrono::nanoseconds) {
                                    if (phase == ApplicationPhase::RENDER) {
                                      ++*static_cast<int *>(context);
                                    }
                                  },
                                  &renders};

  auto scene = ui::scene(ui::label("ready"), options);

  scene.prepare().configure([](ManagedScene &owner) {
    take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
  });

  CHECK(scene.runApp(terminal) == 0);
  CHECK(terminal.closed);
  CHECK(renders == 1);
  CHECK(take(scene.diagnostics()).errors.size() == 1);

  const auto output = capture.contents();

  CHECK(output.find("recoverable diagnostics: retained=1, dropped=0") != std::string::npos);
  CHECK(output.find("scene failed") == std::string::npos);
}

TEST_CASE("cleared custom observer does not repeat its reports and resumes future fatal fallback") {
  StderrCapture capture;
  FailingTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int calls{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options);

  scene.onError([&](const ManagedDiagnostic &) {
    ++calls;
  });
  scene.prepare().configure([](ManagedScene &owner) {
    take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
  });
  scene.onError({});

  CHECK(scene.runApp(terminal) == 1);
  CHECK(scene.runApp(terminal) == 1);
  CHECK(calls == 1);

  const auto output = capture.contents();

  CHECK(output.find("recoverable diagnostics") == std::string::npos);
  CHECK(std::count(output.begin(), output.end(), '\n') == 1);
}

TEST_CASE("construction before the final stage remains outside its boundary") {
  StderrCapture capture;
  int destroyed{};

  CHECK_THROWS_AS(([&]() -> ui::Scene {
                    Lifetime lifetime{destroyed};

                    throw std::runtime_error("recipe construction");
                  }() | ui::runApp()),
                  std::runtime_error);
  CHECK(destroyed == 1);
  CHECK(capture.contents().empty());
}

TEST_CASE("late button self removal preserves quit callback until it returns") {
  example::ReplayTerminal terminal({{10, "\r", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  int destroyed{};
  bool returned{};

  options.clock = terminal.clock();

  {
    auto scene = ui::scene(ui::button(button, "Remove and quit"), options);
    auto lifetime = std::make_shared<Lifetime>(destroyed);

    scene.prepare().configure([&, lifetime](ManagedScene &owner) {
      return button.onClick([&, lifetime, button] {
        take(owner.remove(button.node()));
        take(scene.quit());
        CHECK(destroyed == 0);
        returned = true;
      });
    });
    lifetime.reset();

    CHECK(scene.runApp(terminal) == 0);
    CHECK(returned);
    CHECK(terminal.closed);
    // Disconnected router captures retire on its next collecting operation or destruction.
  }

  CHECK(destroyed == 1);
}

TEST_CASE("unadmitted and nested runApp calls cannot print over a live terminal") {
  StderrCapture capture;
  example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  bool foreignRejected{}, nestedRejected{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options);

  std::thread worker([&] {
    foreignRejected = scene.runApp(terminal) == 1;
  });

  worker.join();
  CHECK(foreignRejected);

  auto moved = std::move(scene);

  CHECK(scene.runApp(terminal) == 1);
  moved.onKey(ui::key(InputKey::F10), [&] {
    nestedRejected = moved.runApp(terminal) == 1;
  });

  CHECK(moved.runApp(terminal) == 1);
  CHECK(nestedRejected);

  const auto output = capture.contents();

  CHECK(output.find("TreeError(WOULD_BLOCK)") != std::string::npos);
  CHECK(std::count(output.begin(), output.end(), '\n') == 1);
}

TEST_CASE("construction allocation failure before the final stage propagates") {
  bool threw{};

  allocationsBeforeFailure = 0;

  try {
    (void)(ui::scene(ui::label("allocation boundary")) | ui::runApp());
  } catch (const std::bad_alloc &) {
    threw = true;
  }

  allocationsBeforeFailure = -1;

  CHECK(threw);
}

TEST_CASE("scene default primary reporting does not allocate after terminal restoration") {
  StderrCapture capture;
  AllocationFailureTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options);
  const auto status = scene.runApp(terminal);

  allocationsBeforeFailure = -1;

  CHECK(status == 1);
  CHECK(terminal.closed);
  CHECK(capture.contents().find("Portals: scene failed") == 0);
}

TEST_CASE("runApp retires pre-owner sticky failure captures before default reporting") {
  StderrCapture capture;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int destroyed{};
  bool cleanBeforeReport{};

  options.maxConfigurations = 1;

  auto lifetime = std::shared_ptr<int>(new int(1), [&](int *value) {
    ++destroyed;
    std::fflush(stderr);
#ifdef _WIN32
    cleanBeforeReport = _tell(_fileno(stderr)) == 0;
#else
    cleanBeforeReport = lseek(fileno(stderr), 0, SEEK_CUR) == 0;
#endif
    delete value;
  });
  auto scene = ui::scene(ui::label("never mounted"), options);

  scene.configure([lifetime](ManagedScene &) {
  });
  scene.configure([](ManagedScene &) {
  });
  lifetime.reset();

  REQUIRE_FALSE(scene.checked());
  CHECK(scene.runApp(terminal) == 1);
  CHECK(destroyed == 1);
  CHECK(cleanBeforeReport);
  CHECK_FALSE(terminal.closed);
  CHECK(capture.contents().find("TreeError(NODE_LIMIT)") != std::string::npos);
}

TEST_CASE("recoverable summary reports bounded history independently of cleared observer") {
  for (bool custom : {false, true}) {
    StderrCapture capture;
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    int calls{};

    options.clock = terminal.clock();
    options.managed.diagnostics.maxErrors = 1;

    auto scene = ui::scene(ui::label("ready"), options);

    if (custom) {
      scene.onError([&](const ManagedDiagnostic &) {
        ++calls;
      });
    }

    scene.prepare().configure([](ManagedScene &owner) {
      take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
    });

    if (custom) {
      scene.onError({});
    }

    scene.configure([](ManagedScene &owner) {
      take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
      take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
    });

    CHECK(scene.runApp(terminal) == 0);
    CHECK(calls == (custom ? 1 : 0));

    const auto output = capture.contents();

    CHECK(output.find("recoverable diagnostics: retained=1, dropped=2") != std::string::npos);
  }
}

TEST_CASE("bounded pipeline shares the method implementation") {
  for (const bool consumeScene : {false, true}) {
    for (const bool temporaryStage : {false, true}) {
      for (const bool failSetup : {false, true}) {
        example::ReplayTerminal terminal({}, 100, {30, 4});
        ui::SceneOptions options;
        ui::Ref<LineEdit> field;
        std::vector<int> order;
        int factories{};

        options.clock = terminal.clock();

        auto scene = ui::scene(
          [&](ManagedScene &) {
            ++factories;

            return ui::lineEdit(field, "ready");
          },
          options);
        auto first = ui::configure([&](ManagedScene &) -> std::expected<void, ManagedError> {
          order.push_back(1);

          if (failSetup) {
            return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
          }

          return {};
        });
        auto registerConfiguration = [&](ui::Scene &owner) {
          auto second = ui::configure([&](ManagedScene &) {
            order.push_back(2);
          });

          if (temporaryStage) {
            CHECK(&(owner | std::move(second)) == &owner);
          } else {
            CHECK(&(owner | second) == &owner);
          }

          CHECK(factories == 0);
          CHECK(order.empty());
          CHECK(terminal.output.empty());
          CHECK_FALSE(terminal.closed);
        };
        auto exercise = [&](ui::Scene &owner) {
          auto preparation = ui::prepare();

          if (temporaryStage) {
            CHECK(&(owner | std::move(preparation)) == &owner);
          } else {
            CHECK(&(owner | preparation) == &owner);
          }

          CHECK(factories == 1);
          CHECK(order == (failSetup ? std::vector<int>{1} : std::vector<int>{1, 2}));
          CHECK(terminal.output.empty());
          CHECK_FALSE(terminal.closed);

          const auto result = std::move(owner) | ui::run(terminal);

          REQUIRE(bool(result) == !failSetup);
          CHECK_FALSE(field.text().get());

          if (failSetup) {
            CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
            CHECK_FALSE(terminal.closed);
            CHECK(terminal.output.empty());
          } else {
            CHECK(terminal.closed);
          }
        };

        if (consumeScene) {
          auto moved = temporaryStage ? std::move(scene) | std::move(first) : std::move(scene) | first;

          CHECK_FALSE(scene.checked());
          registerConfiguration(moved);
          exercise(moved);
        } else {
          if (temporaryStage) {
            CHECK(&(scene | std::move(first)) == &scene);
          } else {
            CHECK(&(scene | first) == &scene);
          }

          registerConfiguration(scene);
          exercise(scene);
        }
      }
    }
  }
}

TEST_CASE("configuration order and owned initial expressions preserve editor modes and Undo") {
  ui::Ref<LineEdit> field;
  ui::LineEditConfig config;
  std::vector<int> order;
  int lookups{};

  config.options.editMode = EditMode::EXPLICIT;

  auto scene = ui::scene(ui::lineEdit(field, i18n("field"), config,
                                      [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
                                        ++lookups;
                                        CHECK(key == "field");

                                        return "translated";
                                      }));

  scene
    .configure([&](ManagedScene &) {
      order.push_back(1);
    })
    .configure([&](ManagedScene &) -> std::expected<void, ManagedError> {
      order.push_back(2);

      return {};
    })
    .prepare();

  take(scene.checked());
  CHECK(lookups == 1);
  CHECK(order == std::vector<int>{1, 2});
  CHECK(take(field.text().get()) == "translated");
  take(field.text().set("changed").result());
  CHECK(field.canUndo());
  REQUIRE(field.undo());
  CHECK(take(field.text().get()) == "translated");
  take(scene.close());
  take(scene.close());
  CHECK_FALSE(scene.prepareChecked());
  CHECK(lookups == 1);
}

TEST_CASE("failed partial mount never publishes references and retires all mounted widgets") {
  int alive{}, configured{};
  ui::Ref<LineEdit> field;

  auto scene = ui::scene(ui::column(
    ui::custom<Mounted>(std::ref(alive)),
    ui::lineEdit(field, "initial").setup([](ManagedScene &, Node<LineEdit>) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED, {}, {}});
    })));

  scene
    .configure([&](ManagedScene &) {
      ++configured;
    })
    .prepare();

  const auto result = scene.checked();

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::CALLBACK_FAILED));
  CHECK(alive == 0);
  CHECK(configured == 0);
  CHECK(field.node() == Node<LineEdit>{});
  CHECK(take(scene.diagnostics()).errors.size() == 1);
  CHECK_FALSE(scene.run());
}

TEST_CASE("factory errors and exceptions are once-only and close the partial owner") {
  for (const bool throwing : {false, true}) {
    int calls{}, alive{};

    auto scene = ui::scene([&](ManagedScene &owner) -> std::expected<ui::Description<Label>, ManagedError> {
      ++calls;
      take(owner.emplace<Mounted>(owner.root(), alive));

      if (throwing) {
        throw std::runtime_error("factory");
      }

      return std::unexpected(ManagedError{ManagedPhase::STATE, owner.root().id(), TreeError::NODE_LIMIT, {}, {}});
    });

    scene.prepare().prepare();

    REQUIRE_FALSE(scene.checked());
    CHECK(calls == 1);
    CHECK(alive == 0);
    CHECK(treeCause(scene.checked().error(), throwing ? TreeError::CALLBACK_FAILED : TreeError::NODE_LIMIT));
  }
}

TEST_CASE("configuration failure preserves published stale identity and skips later actions") {
  ui::Ref<Button> button;
  int skipped{};

  auto scene = ui::scene(ui::button(button, "ready"));

  scene
    .configure([](ManagedScene &) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::NODE_LIMIT, {}, {}});
    })
    .configure([&](ManagedScene &) {
      ++skipped;
    })
    .prepare();

  REQUIRE_FALSE(scene.checked());
  CHECK(button.node() != Node<Button>{});
  CHECK_FALSE(button.text().get());
  CHECK(skipped == 0);
  CHECK_FALSE(scene.managed());
}

TEST_CASE("pre-owner errors retain ApplicationError and notify late observers once") {
  ui::SceneOptions options;
  int notifications{}, factories{};

  options.application.framesPerSecond = 0;

  auto scene = ui::scene(
    [&](ManagedScene &) {
      ++factories;

      return ui::label("unreachable");
    },
    options);

  scene.prepare()
    .onError([&](const ManagedDiagnostic &diagnostic) {
      ++notifications;
      CHECK(diagnostic.error.node == NodeId{});
      CHECK(std::holds_alternative<ApplicationError>(diagnostic.error.cause));
      CHECK(diagnostic.contexts.empty());
      CHECK_FALSE(scene.close());
      throw std::runtime_error("observer");
    })
    .onError([&](const ManagedDiagnostic &) {
      ++notifications;
    });

  REQUIRE_FALSE(scene.checked());
  CHECK(std::holds_alternative<ApplicationError>(scene.checked().error().cause));
  CHECK(notifications == 1);
  CHECK(factories == 0);

  const auto diagnostics = take(scene.diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.observerFailures == 1);
  CHECK(diagnostics.errors[0].observerFailed);
}

TEST_CASE("prepare and configuration lifecycle reentry never retire an active owner") {
  ui::Ref<Button> button;
  auto scene = ui::scene(ui::button(button, "ready"));
  bool remainedLive{};

  scene
    .configure([&](ManagedScene &) {
      const auto close = scene.close();

      CHECK_FALSE(close);
      CHECK(treeCause(close.error(), TreeError::WOULD_BLOCK));
      remainedLive = button.text().get().has_value();
    })
    .prepare();

  CHECK(remainedLive);
  REQUIRE_FALSE(scene.checked());
  CHECK(treeCause(scene.checked().error(), TreeError::WOULD_BLOCK));
  CHECK_FALSE(button.text().get());
}

TEST_CASE("runtime key closures compose, permit theme changes and reject lifecycle reentry") {
  example::ReplayTerminal terminal({{10, "\x1bOQ", {}}, {20, "\x1bOR", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Label> label;
  int first{}, second{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label(label, "ready"), options);

  scene
    .onKey(ui::key(InputKey::F2),
           [&] {
             ++first;
             scene.theme({});
             CHECK(scene.checked());
           })
    .onKey(ui::key(InputKey::F3), [&] {
      ++second;

      const auto nested = scene.run(terminal);

      CHECK_FALSE(nested);
      CHECK(treeCause(nested.error(), TreeError::WOULD_BLOCK));
      CHECK(label.text().get());
    });

  const auto result = scene.run(terminal);

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::WOULD_BLOCK));
  CHECK(first == 1);
  CHECK(second == 1);
  CHECK(terminal.closed);
  CHECK_FALSE(label.text().get());
}

TEST_CASE("native open failure closes prepared widgets before returning") {
  ui::Ref<Button> button;
  int alive{};
  int notifications{};

  auto scene = ui::scene(ui::column(ui::custom<Mounted>(std::ref(alive)), ui::button(button, "ready")));

  scene.onError([&](const ManagedDiagnostic &) {
    ++notifications;
    const auto blocked = scene.close();

    CHECK_FALSE(blocked);
    CHECK(treeCause(blocked.error(), TreeError::WOULD_BLOCK));
    throw std::runtime_error("native observer");
  });

  take(scene.prepareChecked());
  REQUIRE(alive == 1);

  const auto result = [&] {
    InvalidNativeInput invalid;

    return scene.run();
  }();

  REQUIRE_FALSE(result);
  const auto *application = std::get_if<ApplicationError>(&result.error().cause);

  REQUIRE(application);
  CHECK(application->phase == ApplicationPhase::OPEN);
  CHECK(std::holds_alternative<TerminalError>(application->cause));
  CHECK(alive == 0);
  CHECK(notifications == 1);
  CHECK(take(scene.diagnostics()).observerFailures == 1);
  CHECK_FALSE(button.text().get());
  CHECK_FALSE(scene.run());
  take(scene.close());
}

TEST_CASE("finish after stop notifies the terminal failure once and preserves restoration") {
  FailingTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  int notifications{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::button(button, "ready"), options);

  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notifications;
    CHECK_FALSE(button.text().get());
    CHECK(std::holds_alternative<ApplicationError>(diagnostic.error.cause));
    CHECK(diagnostic.error.application->restoration->native_code == 9);
    CHECK_FALSE(scene.close());
    throw std::runtime_error("terminal observer");
  });

  const auto result = scene.run(terminal);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().application);
  CHECK(result.error().application->restoration->native_code == 9);
  CHECK(notifications == 1);
  scene.onError([&](const ManagedDiagnostic &) {
    ++notifications;
  });
  CHECK(notifications == 1);
  CHECK(terminal.closed);

  const auto batch = take(scene.diagnostics());

  CHECK(batch.errors.size() == 1);
  CHECK(batch.observerFailures == 1);
}

TEST_CASE("nonfatal reports and same-cause checked configuration retain both boundary contexts") {
  ui::Ref<Button> button;
  auto scene = ui::scene(ui::button(button, "ready"));

  scene
    .configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
      const ManagedError failure{ManagedPhase::BIND, button.node().id(), TreeError::NODE_LIMIT, {}, {}};
      const ManagedDiagnosticContext context{.operation = "custom.report"};

      take(owner.reportError(failure, std::span(&context, 1)));

      return std::unexpected(failure);
    })
    .prepare();

  REQUIRE_FALSE(scene.checked());
  const auto batch = take(scene.diagnostics());

  REQUIRE(batch.errors.size() == 2);
  CHECK(batch.errors[0].contexts[0].operation == "custom.report");
  CHECK(batch.errors[1].contexts[0].operation == "scene.configure");
}

TEST_CASE("short setup failure finishes the current lambda and skips dependent configuration and terminal") {
  for (const int mode : {0, 1, 2}) {
    ui::Ref<Label> label;
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    options.clock = terminal.clock();
    int tail{};
    int later{};
    int notifications{};
    auto scene = mode ? ui::scene(ui::label(label, "ready"), options) : ui::scene(ui::label("ready"), options);

    scene
      .onError([&](const ManagedDiagnostic &) {
        ++notifications;
      })
      .configure([&](ManagedScene &owner) {
        if (mode == 2) {
          take(owner.remove(label.node()));
        }
        label.text() = std::string_view("\xff", 1);
        ++tail;
      })
      .configure([&](ManagedScene &) {
        ++later;
      });

    CHECK(scene.checked());
    CHECK(tail == 0);
    const auto result = scene.run(terminal);
    REQUIRE_FALSE(result);
    CHECK(tail == 1);
    CHECK(later == 0);
    CHECK(notifications == 1);
    CHECK_FALSE(terminal.closed);
    const auto batch = take(scene.diagnostics());
    REQUIRE(batch.errors.size() == 1);
    CHECK(batch.errors[0].error.phase == ManagedPhase::TEXT);
  }
}

TEST_CASE("returning the exact short setup result does not duplicate its report") {
  ui::Ref<Label> label;
  auto scene = ui::scene(ui::label(label, "ready"));
  int notifications{};
  scene
    .onError([&](const ManagedDiagnostic &) {
      ++notifications;
    })
    .configure([&](ManagedScene &) {
      return (label.text() = std::string_view("\xff", 1)).result();
    });

  REQUIRE_FALSE(scene.prepareChecked());
  CHECK(notifications == 1);
  CHECK(take(scene.diagnostics()).errors.size() == 1);
}

TEST_CASE("short setup preserves the first operation site across observer reentry and callback exceptions") {
  for (const int mode : {0, 1, 2}) {
    ui::Ref<Label> label;
    auto scene = ui::scene(ui::label(label, "ready"));
    const auto origin = std::source_location::current();
    int notifications{};
    int later{};
    scene
      .onError([&](const ManagedDiagnostic &diagnostic) {
        ++notifications;
        CHECK(diagnostic.error.setupStopped);
        REQUIRE_FALSE(scene.checked());
        CHECK(scene.checked().error().text == TextError{TextErrorKind::INVALID_UTF8, 0});
        CHECK_FALSE(scene.close());
        auto &owner = take(scene.managed()).get();
        take(owner.reportError({ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, {}}));
        throw std::runtime_error("observer");
      })
      .configure([&](ManagedScene &) {
        if (mode == 0) {
          label.text({}, origin) = std::string_view("\xff", 1);
        } else if (mode == 1) {
          label.text().append(std::string_view("\xff", 1), origin);
        } else {
          label.text().set(std::string_view("\xff", 1), origin);
        }
        throw std::bad_alloc();
      })
      .configure([&](ManagedScene &) {
        ++later;
      });

    const auto result = scene.prepareChecked();
    REQUIRE_FALSE(result);
    CHECK(result.error().phase == ManagedPhase::TEXT);
    CHECK(result.error().text == TextError{TextErrorKind::INVALID_UTF8, 0});
    CHECK(std::string_view(result.error().operation) == (mode == 1 ? "text.append" : "text.set"));
    REQUIRE(result.error().operationOrigin);
    CHECK(result.error().operationOrigin->line() == origin.line());
    CHECK(std::string_view(result.error().execution) == "scene.configure");
    CHECK(later == 0);
    CHECK(notifications == 1);
    const auto batch = take(scene.diagnostics());
    CHECK(batch.observerFailures == 1);
    CHECK(batch.suppressedNotifications >= 1);
  }
}

TEST_CASE("short setup stop survives full history and missing observer") {
  for (const bool observe : {false, true}) {
    ui::SceneOptions options;
    options.managed.diagnostics.maxErrors = 1;
    ui::Ref<Label> label;
    auto scene = ui::scene(ui::label(label, "ready"), options);
    int notifications{};
    int tail{};
    if (observe) {
      scene.onError([&](const ManagedDiagnostic &) {
        ++notifications;
      });
    }
    scene
      .configure([](ManagedScene &owner) {
        take(owner.reportError({ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, {}}));
      })
      .configure([&](ManagedScene &) {
        label.text() = std::string_view("\xff", 1);
        ++tail;
      });

    const auto result = scene.prepareChecked();
    REQUIRE_FALSE(result);
    CHECK(result.error().text == TextError{TextErrorKind::INVALID_UTF8, 0});
    CHECK(result.error().setupStopped);
    CHECK(tail == 1);
    CHECK(notifications == (observe ? 2 : 0));
    const auto batch = take(scene.diagnostics());
    CHECK(batch.errors.size() == 1);
    CHECK(batch.droppedErrors == 1);
  }
}

TEST_CASE("short setup receipt and original source survive diagnostic context allocation failure") {
  ui::Ref<Label> label;
  ui::Ref<Label> missing;
  auto scene = ui::scene(ui::label(label, "ready"));
  take(scene.prepareChecked());
  const auto origin = std::source_location::current();
  const auto expression = label.text() + missing.text({}, origin);
  int notifications{};
  bool firstCause{};
  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notifications;
    firstCause = treeCause(diagnostic.error, TreeError::CLOSED) && diagnostic.error.setupStopped;
  });
  scene.configure([&](ManagedScene &) {
    allocationsBeforeFailure = 0;
    auto result = label.text() = expression;
    allocationsBeforeFailure = -1;
    return result.result();
  });
  allocationsBeforeFailure = -1;

  REQUIRE_FALSE(scene.checked());
  CHECK(firstCause);
  CHECK(notifications == 1);
  CHECK(treeCause(scene.checked().error(), TreeError::CLOSED));
  CHECK(std::string_view(scene.checked().error().operation) == "text.assign.concat");
  REQUIRE(scene.checked().error().operationOrigin);
  CHECK(scene.checked().error().operationOrigin->line() == origin.line());
  const auto batch = take(scene.diagnostics());
  CHECK(batch.errors.size() == 1);
  CHECK(batch.errors[0].contexts.empty());
  CHECK(batch.errors[0].contextUnavailable);
  CHECK(batch.contextFailures == 1);
}

TEST_CASE("nested setup scopes restore their parent and do not leak into runtime mutations") {
  ui::Ref<Label> missing;
  auto outer = ui::scene(ui::label("outer"));
  auto inner = ui::scene(ui::label("inner"));
  int later{};
  int innerTail{};
  outer
    .configure([&](ManagedScene &) {
      inner.configure([&](ManagedScene &) {
        missing.text() = "fail";
        ++innerTail;
      });
      CHECK_FALSE(inner.prepareChecked());
    })
    .configure([&](ManagedScene &) {
      ++later;
    });
  take(outer.prepareChecked());
  CHECK(later == 1);
  CHECK(innerTail == 1);
  auto failed = missing.text() = "outside";
  CHECK_FALSE(failed.result());
  CHECK_FALSE(failed.result().error().setupStopped);
  CHECK(outer.checked());

  auto parent = ui::scene(ui::label("parent"));
  auto child = ui::scene(ui::label("child"));
  parent
    .configure([&](ManagedScene &) {
      child
        .configure([](ManagedScene &) {
        })
        .prepare();
      missing.text() = "parent failure";
    })
    .configure([&](ManagedScene &) {
      ++later;
    });
  REQUIRE_FALSE(parent.prepareChecked());
  CHECK(child.checked());
  CHECK(later == 1);
}

TEST_CASE("short setup attributes foreign targets to the entered caller without foreign notification") {
  ui::Ref<Label> foreign;
  std::atomic<bool> published{};
  std::atomic<bool> release{};
  int foreignNotifications{};
  std::thread worker([&] {
    ui::Ref<Label> local;
    auto scene = ui::scene(ui::label(local, "foreign"));
    scene.onError([&](const ManagedDiagnostic &) {
      ++foreignNotifications;
    });
    take(scene.prepareChecked());
    foreign = local;
    published = true;
    while (!release.load()) {
      std::this_thread::yield();
    }
  });
  while (!published.load()) {
    std::this_thread::yield();
  }
  auto caller = ui::scene(ui::label("caller"));
  int tail{};
  caller.configure([&](ManagedScene &) {
    foreign.text() = "rejected";
    ++tail;
  });
  const auto result = caller.prepareChecked();
  release = true;
  worker.join();

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::WRONG_THREAD));
  CHECK(result.error().setupStopped);
  CHECK(tail == 1);
  CHECK(foreignNotifications == 0);
  CHECK(take(caller.diagnostics()).errors.size() == 1);
}

TEST_CASE("checked stationary setup catches an unbound short failure before mounting") {
  ui::Ref<Label> missing;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::RunAppOptions options;
  options.scene.clock = terminal.clock();
  options.terminal = &terminal;
  int tail{};
  int later{};
  int reports{};
  options.report = [&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    CHECK(treeCause(*report.failure, TreeError::CLOSED));
    CHECK(report.failure->setupStopped);
    CHECK(std::string_view(report.failure->execution) == "scene.setup");
  };
  const int code = ui::runApp(
    [] {
      return ui::label("ready");
    },
    [&](ui::Scene &scene) {
      missing.text() = "unbound";
      ++tail;
      scene.configure([&](ManagedScene &) {
        ++later;
      });
    },
    options);

  CHECK(code == 1);
  CHECK(tail == 1);
  CHECK(later == 0);
  CHECK(reports == 1);
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("setup deduplicates a copied short receipt but preserves a fresh equal-valued boundary error") {
  for (const bool exact : {false, true}) {
    ui::Ref<Label> label;
    auto scene = ui::scene(ui::label(label, "ready"));
    int notifications{};
    scene.onError([&](const ManagedDiagnostic &) {
      ++notifications;
    });
    take(scene.prepareChecked());
    const auto failed = (label.text() = std::string_view("\xff", 1)).result();
    REQUIRE_FALSE(failed);
    CHECK(scene.checked());
    scene.configure([&](ManagedScene &) -> std::expected<void, ManagedError> {
      if (exact) {
        return failed;
      }
      ManagedError fresh{failed.error().phase, failed.error().node, failed.error().cause, {}, {}};
      fresh.text = failed.error().text;
      return std::unexpected(fresh);
    });

    CHECK_FALSE(scene.checked());
    CHECK(notifications == (exact ? 1 : 2));
    CHECK(take(scene.diagnostics()).errors.size() == (exact ? 1 : 2));
  }
}

TEST_CASE("short setup keeps different collector contexts distinct and observer captures pinned") {
  ui::Ref<Label> label;
  auto target = ui::scene(ui::label(label, "ready"));
  take(target.prepareChecked());
  int targetNotifications{};
  int setupNotifications{};
  target.onError([&](const ManagedDiagnostic &) {
    ++targetNotifications;
  });
  auto setup = ui::scene(ui::label("setup"));
  setup.onError([&](const ManagedDiagnostic &) {
    ++setupNotifications;
  });
  setup.configure([&](ManagedScene &) {
    return (label.text() = std::string_view("\xff", 1)).result();
  });
  CHECK_FALSE(setup.prepareChecked());
  CHECK(target.checked());
  CHECK(targetNotifications == 1);
  CHECK(setupNotifications == 1);
  CHECK(take(target.diagnostics()).errors.size() == 1);
  CHECK(take(setup.diagnostics()).errors.size() == 1);

  ui::Ref<Label> self;
  auto scene = ui::scene(ui::label(self, "ready"));
  bool pinned{};
  bool retired{};

  struct Capture {
    bool &retired;

    ~Capture() {
      retired = true;
    }
  };

  auto capture = std::make_shared<Capture>(retired);
  auto weak = std::weak_ptr(capture);
  take(scene.prepareChecked());
  auto &owner = take(scene.managed()).get();
  take(owner.onError([&, capture](const ManagedDiagnostic &) {
    CHECK_FALSE(owner.onError({}));
    pinned = !weak.expired();
    CHECK_FALSE(owner.close());
  }));
  capture.reset();
  scene.configure([&](ManagedScene &) {
    self.text() = std::string_view("\xff", 1);
  });
  CHECK_FALSE(scene.checked());
  CHECK(pinned);
  CHECK(retired);
  CHECK(take(scene.diagnostics()).errors.size() == 1);
}

TEST_CASE("short setup notification suppression retains the first cause without a duplicate record") {
  ui::Ref<Label> label;
  auto scene = ui::scene(ui::label(label, "ready"));
  int notifications{};
  scene
    .onError([&](const ManagedDiagnostic &) {
      if (++notifications == 1) {
        label.text() = std::string_view("\xff", 1);
        REQUIRE_FALSE(scene.checked());
        CHECK(treeCause(scene.checked().error(), TreeError::WOULD_BLOCK));
      }
    })
    .configure([](ManagedScene &owner) {
      take(owner.reportError({ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, {}}));
    });

  REQUIRE_FALSE(scene.prepareChecked());
  CHECK(notifications == 2);
  const auto batch = take(scene.diagnostics());
  CHECK(batch.errors.size() == 2);
  CHECK(batch.suppressedNotifications == 1);
  CHECK(treeCause(batch.errors[1].error, TreeError::WOULD_BLOCK));
}

TEST_CASE("checked editor commands inside void setup retain their existing recoverable policy") {
  ui::Ref<LineEdit> field;
  auto scene = ui::scene(ui::lineEdit(field, "ready"));
  int tail{};
  int later{};
  scene
    .configure([&](ManagedScene &owner) {
      take(owner.remove(field.node()));
      const auto command = field.undo();
      CHECK_FALSE(command);
      ++tail;
    })
    .configure([&](ManagedScene &) {
      ++later;
    });

  take(scene.prepareChecked());
  CHECK(tail == 1);
  CHECK(later == 1);
  CHECK(take(scene.diagnostics()).errors.size() == 1);
}

TEST_CASE("nested runtime errors do not inherit an outer setup failure scope") {
  example::ReplayTerminal terminal({{10, "\x1bOQ", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Label> label;
  auto runtime = ui::scene(ui::label(label, "ready"), options);
  int callbacks{};
  runtime.onKey(ui::key(InputKey::F2), [&] {
    label.text() = std::string_view("\xff", 1);
    ++callbacks;
  });
  auto setup = ui::scene(ui::label("setup"));
  int later{};
  setup
    .configure([&](ManagedScene &) {
      CHECK(runtime.run(terminal));
    })
    .configure([&](ManagedScene &) {
      ++later;
    });

  CHECK(setup.prepareChecked());
  CHECK(later == 1);
  CHECK(callbacks == 1);
  const auto batch = take(runtime.diagnostics());
  REQUIRE(batch.errors.size() == 1);
  CHECK_FALSE(batch.errors[0].error.setupStopped);
}

TEST_CASE("nested setup deferred error delivery cannot poison its outer setup scope") {
  ui::Ref<Label> missing;
  auto outer = ui::scene(ui::label("outer"));
  auto inner = ui::scene(ui::label("inner"));
  auto third = ui::scene(ui::label("third"));
  int later{};
  int notifications{};
  inner.onError([&](const ManagedDiagnostic &) {
    ++notifications;
    auto rejected = missing.text() = "observer failure";
    CHECK_FALSE(rejected.result());
    CHECK_FALSE(rejected.result().error().setupStopped);
    third.configure([&](ManagedScene &) {
      missing.text() = "third setup";
    });
    CHECK_FALSE(third.prepareChecked());
  });
  outer
    .configure([&](ManagedScene &) {
      inner.configure([&](ManagedScene &) {
        missing.text() = "inner setup";
      });
      CHECK_FALSE(inner.prepareChecked());
    })
    .configure([&](ManagedScene &) {
      ++later;
    });

  CHECK(outer.prepareChecked());
  CHECK(later == 1);
  CHECK(notifications == 1);
  CHECK_FALSE(inner.checked());
  CHECK_FALSE(third.checked());
}

TEST_CASE("pre-owner short setup failure retains its typed cause while all allocations are rejected") {
  ui::Ref<Label> missing;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::RunAppOptions options;
  options.scene.clock = terminal.clock();
  options.terminal = &terminal;
  int notifications{};
  int reports{};
  int tail{};
  bool observerCause{};
  bool finalCause{};
  options.report = [&](const ui::SceneReport &report) {
    // Release the fault before the test framework can allocate assertion diagnostics.
    allocationsBeforeFailure = -1;
    ++reports;
    finalCause = report.failure && treeCause(*report.failure, TreeError::CLOSED) &&
                 report.failure->phase == ManagedPhase::TEXT && report.failure->setupStopped &&
                 report.failure->operationOrigin && std::string_view(report.failure->operation) == "text.set";
  };
  const int code = ui::runApp(
    [] {
      return ui::label("ready");
    },
    [&](ui::Scene &scene) {
      scene.onError([&](const ManagedDiagnostic &diagnostic) {
        ++notifications;
        observerCause = treeCause(diagnostic.error, TreeError::CLOSED) && diagnostic.error.setupStopped;
      });
      allocationsBeforeFailure = 0;
      missing.text() = "unbound";
      ++tail;
    },
    options);
  allocationsBeforeFailure = -1;

  CHECK(code == 1);
  CHECK(tail == 1);
  CHECK(notifications == 1);
  CHECK(reports == 1);
  CHECK(observerCause);
  CHECK(finalCause);
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("r2 stationary setup delivers its first short failure before expected or exception cleanup") {
  for (const int ending : {0, 1, 2}) {
    ui::Ref<Label> missing;
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::RunAppOptions options;
    options.scene.clock = terminal.clock();
    options.terminal = &terminal;
    int notifications{};
    int reports{};
    int tail{};
    const auto origin = std::source_location::current();
    options.report = [&](const ui::SceneReport &report) {
      ++reports;
      REQUIRE(report.failure);
      CHECK(treeCause(*report.failure, TreeError::CLOSED));
      CHECK(report.failure->phase == ManagedPhase::TEXT);
      CHECK(report.failure->operationOrigin->line() == origin.line());
    };
    const int code = ui::runApp(
      [] {
        return ui::label("ready");
      },
      [&](ui::Scene &scene) -> std::expected<void, ManagedError> {
        scene.onError([&](const ManagedDiagnostic &diagnostic) {
          ++notifications;
          CHECK(treeCause(diagnostic.error, TreeError::CLOSED));
          CHECK(diagnostic.origin.line() == origin.line());
        });
        const auto failure = (missing.text({}, origin) = "unbound").result();
        ++tail;
        if (ending == 1) {
          throw std::bad_alloc();
        }
        if (ending == 2) {
          throw std::runtime_error("later setup throw");
        }
        return failure;
      },
      options);

    CHECK(code == 1);
    CHECK(tail == 1);
    CHECK(notifications == 1);
    CHECK(reports == 1);
    CHECK_FALSE(terminal.closed);
  }
}

TEST_CASE("r2 independent factory and final sink cannot inherit outer setup attribution") {
  for (const bool throwing : {false, true}) {
    ui::Ref<Label> missing;
    auto outer = ui::scene(ui::label("outer"));
    auto nested = ui::scene(ui::label("nested"));
    int later{};
    int reports{};
    outer
      .configure([&](ManagedScene &) {
        ui::RunAppOptions options;
        options.report = [&](const ui::SceneReport &report) {
          ++reports;
          REQUIRE(report.failure);
          CHECK(treeCause(*report.failure, throwing ? TreeError::CALLBACK_FAILED : TreeError::NODE_LIMIT));
          auto failure = missing.text() = "sink only";
          CHECK_FALSE(failure.result().error().setupStopped);
          nested.configure([&](ManagedScene &) {
            missing.text() = "nested setup";
          });
          CHECK_FALSE(nested.prepareChecked());
        };
        const int code = ui::runApp(
          [&]() -> std::expected<decltype(ui::label("inner")), ManagedError> {
            auto failure = missing.text() = "factory only";
            CHECK_FALSE(failure.result().error().setupStopped);
            if (throwing) {
              throw std::runtime_error("factory");
            }
            return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::NODE_LIMIT, {}, {}});
          },
          options);
        CHECK(code == 1);
      })
      .configure([&](ManagedScene &) {
        ++later;
      });

    CHECK(outer.prepareChecked());
    CHECK(later == 1);
    CHECK(reports == 1);
    CHECK_FALSE(nested.checked());
  }
}

TEST_CASE("r2 independent lifecycle and capture retirement isolate and then restore outer setup") {
  struct Retirement {
    ui::Ref<Label> &missing;
    int &retired;
    bool &attributed;

    ~Retirement() {
      auto result = missing.text() = "retirement";
      ++retired;
      attributed = attributed || result.result().error().setupStopped;
    }
  };

  struct RetiringWidget final : Widget {
    Retirement retirement;

    RetiringWidget(ui::Ref<Label> &missing, int &retired, bool &attributed) : retirement{missing, retired, attributed} {
    }
  };

  for (const int lifecycle : {0, 1, 2, 3, 4}) {
    for (const bool outerFailure : {false, true}) {
      ui::Ref<Label> missing;
      auto outer = ui::scene(ui::label("outer"));
      int retired{};
      int later{};
      bool attributed{};
      const auto origin = std::source_location::current();
      outer
        .configure([&](ManagedScene &) {
          {
            example::ReplayTerminal terminal({}, 100, {30, 4});
            ui::SceneOptions options;
            options.clock = terminal.clock();
            auto inner = ui::scene(
              ui::custom<RetiringWidget>(std::ref(missing), std::ref(retired), std::ref(attributed)), options);
            inner.reportOnError([](const ui::SceneReport &) {
            });
            inner.configure([capture = std::make_shared<Retirement>(missing, retired, attributed)](ManagedScene &) {
            });
            if (lifecycle < 3) {
              inner.configure([](ManagedScene &) -> std::expected<void, ManagedError> {
                return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::NODE_LIMIT, {}, {}});
              });
              if (lifecycle == 0) {
                CHECK_FALSE(inner.prepareChecked());
              }
              if (lifecycle == 1) {
                CHECK_FALSE(inner.run(terminal));
              }
              if (lifecycle == 2) {
                CHECK(inner.runApp(terminal) == 1);
              }
              REQUIRE_FALSE(inner.checked());
              CHECK(treeCause(inner.checked().error(), TreeError::NODE_LIMIT));
            } else {
              CHECK(inner.prepareChecked());
              if (lifecycle == 3) {
                CHECK(inner.close());
              }
            }
          }
          CHECK(outer.checked());
          if (outerFailure) {
            missing.text({}, origin) = "outer continuation";
          }
        })
        .configure([&](ManagedScene &) {
          ++later;
        });
      const auto result = outer.prepareChecked();

      CHECK(retired == 2);
      CHECK_FALSE(attributed);
      CHECK(later == (outerFailure ? 0 : 1));
      if (outerFailure) {
        REQUIRE_FALSE(result);
        CHECK(result.error().operationOrigin->line() == origin.line());
      } else {
        CHECK(result);
      }
    }
  }
}

TEST_CASE("r2 prepared configuration capture retirement cannot poison its caller") {
  ui::Ref<Label> missing;
  int retired{};
  bool attributed{};

  struct Capture {
    ui::Ref<Label> &missing;
    int &retired;
    bool &attributed;

    ~Capture() {
      auto failure = missing.text() = "retire";
      ++retired;
      attributed = failure.result().error().setupStopped;
    }
  };

  auto outer = ui::scene(ui::label("outer"));
  auto inner = ui::scene(ui::label("inner"));
  take(inner.prepareChecked());
  outer.configure([&](ManagedScene &) {
    inner.configure([capture = std::make_shared<Capture>(missing, retired, attributed)](ManagedScene &) {
    });
  });
  CHECK(outer.prepareChecked());
  CHECK(retired == 1);
  CHECK_FALSE(attributed);
}

TEST_CASE("r2 recipe-only short writes are isolated while actual inner configure retains its scope") {
  struct RecipeWriter final : Widget {
    RecipeWriter(ui::Ref<Label> &missing, bool &attributed) {
      auto failure = missing.text() = "recipe only";
      attributed = failure.result().error().setupStopped;
    }
  };

  for (const bool setupFailure : {false, true}) {
    ui::Ref<Label> missing;
    bool attributed{};
    int later{};
    auto outer = ui::scene(ui::label("outer"));
    auto inner = ui::scene(ui::custom<RecipeWriter>(std::ref(missing), std::ref(attributed)));
    outer
      .configure([&](ManagedScene &) {
        if (setupFailure) {
          inner.configure([&](ManagedScene &) {
            missing.text() = "actual setup";
          });
        }
        const auto prepared = inner.prepareChecked();
        CHECK(prepared.has_value() == !setupFailure);
        if (!prepared) {
          CHECK(prepared.error().setupStopped);
        }
      })
      .configure([&](ManagedScene &) {
        ++later;
      });
    CHECK(outer.prepareChecked());
    CHECK_FALSE(attributed);
    CHECK(later == 1);
  }
}

TEST_CASE("r2 independent observer and report capture replacement cannot poison its caller") {
  for (const bool finalReport : {false, true}) {
    ui::Ref<Label> missing;
    int retired{};
    bool attributed{};

    struct Capture {
      ui::Ref<Label> &missing;
      int &retired;
      bool &attributed;

      ~Capture() {
        auto failure = missing.text() = "retired observer";
        ++retired;
        attributed = failure.result().error().setupStopped;
      }
    };

    auto outer = ui::scene(ui::label("outer"));
    auto inner = ui::scene(ui::label("inner"));
    if (finalReport) {
      inner.reportOnError([capture = std::make_shared<Capture>(missing, retired, attributed)](const ui::SceneReport &) {
      });
    } else {
      inner.onError([capture = std::make_shared<Capture>(missing, retired, attributed)](const ManagedDiagnostic &) {
      });
    }
    outer.configure([&](ManagedScene &) {
      if (finalReport) {
        inner.reportOnError({});
      } else {
        inner.onError({});
      }
    });
    CHECK(outer.prepareChecked());
    CHECK(retired == 1);
    CHECK_FALSE(attributed);
  }
}

TEST_CASE("r3 incoming diagnostic captures retire under isolation after allocation failure") {
  for (const bool finalReport : {false, true}) {
    for (const bool outerFailure : {false, true}) {
      ui::Ref<Label> missing;
      int retired{};
      int later{};
      bool attributed{};

      struct Capture {
        ui::Ref<Label> &missing;
        int &retired;
        bool &attributed;

        ~Capture() {
          auto failure = missing.text() = "incoming capture";

          ++retired;
          attributed = failure.result().error().setupStopped;
        }
      };

      ManagedErrorHandler observer;
      ui::SceneReportHandler sink;

      if (finalReport) {
        sink = [capture = std::make_shared<Capture>(missing, retired, attributed)](const ui::SceneReport &) {
        };
      } else {
        observer = [capture = std::make_shared<Capture>(missing, retired, attributed)](const ManagedDiagnostic &) {
        };
      }

      auto outer = ui::scene(ui::label("outer"));
      auto inner = ui::scene(ui::label("inner"));
      const auto setterOrigin = std::source_location::current();
      const auto outerOrigin = std::source_location::current();

      outer
        .configure([&](ManagedScene &) {
          allocationsBeforeFailure = 0;

          if (finalReport) {
            if (outerFailure) {
              std::move(inner).reportOnError(std::move(sink), setterOrigin);
            } else {
              inner.reportOnError(std::move(sink), setterOrigin);
            }
          } else {
            if (outerFailure) {
              std::move(inner).onError(std::move(observer), setterOrigin);
            } else {
              inner.onError(std::move(observer), setterOrigin);
            }
          }

          allocationsBeforeFailure = -1;

          if (outerFailure) {
            missing.text().set("outer failure", outerOrigin);
          }
        })
        .configure([&](ManagedScene &) {
          ++later;
        });

      const auto prepared = outer.prepareChecked();
      const auto innerResult = inner.checked();

      REQUIRE_FALSE(innerResult);
      CHECK(treeCause(innerResult.error(), TreeError::NO_MEMORY));
      REQUIRE(innerResult.error().operationOrigin);
      CHECK(innerResult.error().operationOrigin->line() == setterOrigin.line());
      CHECK(retired == 1);
      CHECK_FALSE(attributed);
      CHECK(prepared.has_value() == !outerFailure);
      CHECK(later == (outerFailure ? 0 : 1));

      if (outerFailure) {
        REQUIRE_FALSE(prepared);
        REQUIRE(prepared.error().operationOrigin);
        CHECK(prepared.error().operationOrigin->line() == outerOrigin.line());
        CHECK(prepared.error().setupStopped);
      }
    }
  }
}

TEST_CASE("r3 rejected diagnostic setters retire incoming captures without acquiring caller setup") {
  for (const bool finalReport : {false, true}) {
    for (const bool movedOwner : {false, true}) {
      ui::Ref<Label> missing;
      int retired{};
      int later{};
      bool attributed{};

      struct Capture {
        ui::Ref<Label> &missing;
        int &retired;
        bool &attributed;

        ~Capture() {
          auto failure = missing.text() = "rejected incoming capture";

          ++retired;
          attributed = failure.result().error().setupStopped;
        }
      };

      ManagedErrorHandler observer;
      ui::SceneReportHandler sink;

      if (finalReport) {
        sink = [capture = std::make_shared<Capture>(missing, retired, attributed)](const ui::SceneReport &) {
        };
      } else {
        observer = [capture = std::make_shared<Capture>(missing, retired, attributed)](const ManagedDiagnostic &) {
        };
      }

      auto outer = ui::scene(ui::label("outer"));
      auto inner = ui::scene(ui::label("inner"));

      outer
        .configure([&](ManagedScene &) {
          if (finalReport) {
            if (movedOwner) {
              std::move(inner).reportOnError(std::move(sink));
            } else {
              inner.reportOnError(std::move(sink));
            }
          } else {
            if (movedOwner) {
              std::move(inner).onError(std::move(observer));
            } else {
              inner.onError(std::move(observer));
            }
          }
        })
        .configure([&](ManagedScene &) {
          ++later;
        });

      if (movedOwner) {
        auto owner = std::move(inner);

        CHECK(outer.prepareChecked());
        CHECK(owner.checked());
      } else {
        inner.configure([&](ManagedScene &) {
          CHECK(outer.prepareChecked());
        });

        const auto prepared = inner.prepareChecked();

        REQUIRE_FALSE(prepared);
        CHECK(treeCause(prepared.error(), TreeError::WOULD_BLOCK));
      }

      CHECK(retired == 1);
      CHECK_FALSE(attributed);
      CHECK(later == 1);
    }
  }
}

TEST_CASE("void configuration report remains nonfatal and late Ref checked failure keeps setup context") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int notifications{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options);

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++notifications;
    })
    .configure([](ManagedScene &owner) {
      take(owner.reportError({ManagedPhase::STATE, owner.root().id(), TreeError::NODE_LIMIT, {}, {}}));
    });

  take(scene.run(terminal));
  CHECK(notifications == 1);

  ui::Ref<Button> button;
  const auto origin = std::source_location::current();
  auto failed = ui::scene(ui::button(button, "ready", [] {
  }));

  failed.prepare().configure([&](ManagedScene &) {
    return button.onClick(
      [] {
      },
      origin);
  });

  REQUIRE_FALSE(failed.checked());
  const auto batch = take(failed.diagnostics());

  REQUIRE(batch.errors.size() == 2);
  CHECK(batch.errors[0].contexts.empty());
  CHECK(batch.errors[0].origin.line() == origin.line());
  CHECK(batch.errors[1].contexts[0].operation == "scene.configure");
}

TEST_CASE("exact command receipt and overflow notification remain tied to the returned primary") {
  for (const bool overflow : {false, true}) {
    example::ReplayTerminal terminal({{10, "\x1bOQ", {}}}, 100, {30, 4});
    ui::SceneOptions options;
    ui::Ref<LineEdit> field;
    int notifications{};
    bool nested{};

    options.clock = terminal.clock();
    options.managed.diagnostics.maxErrors = overflow ? 1 : 8;

    auto scene = ui::scene(ui::lineEdit(field, "initial"), options);

    scene.onError([&](const ManagedDiagnostic &) {
      ++notifications;

      if (!nested) {
        nested = true;
        auto &owner = take(scene.managed()).get();

        take(owner.reportError({ManagedPhase::STATE, owner.root().id(), TreeError::CALLBACK_FAILED, {}, {}}));
      }
    });

    if (overflow) {
      scene.configure([](ManagedScene &owner) {
        take(owner.reportError({ManagedPhase::STATE, owner.root().id(), TreeError::NODE_LIMIT, {}, {}}));
      });
    }

    if (overflow) {
      scene.onKey(ui::key(InputKey::F2), []() -> std::expected<void, TreeError> {
        return std::unexpected(TreeError::STALE_NODE);
      });
    } else {
      scene.onKey(ui::key(InputKey::F2), [&] {
        auto &owner = take(scene.managed()).get();

        take(owner.remove(field.node()));

        return field.undo();
      });
    }

    const auto result = scene.run(terminal);

    if (overflow) {
      REQUIRE_FALSE(result);
      CHECK(treeCause(result.error(), TreeError::STALE_NODE));
    } else {
      REQUIRE(result);
    }
    CHECK(notifications == (overflow ? 2 : 1));
    const auto batch = take(scene.diagnostics());

    CHECK(batch.errors.size() == (overflow ? 1 : 2));
    CHECK(batch.suppressedNotifications == 1);
  }
}

TEST_CASE("runtime primary latches before observer reentry and nested nonfatal reports restore the marker") {
  example::ReplayTerminal terminal({{10, "\x1bOQ", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  int notifications{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options);

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++notifications;
      const auto blocked = scene.close();

      CHECK_FALSE(blocked);
      CHECK(treeCause(blocked.error(), TreeError::WOULD_BLOCK));

      auto &owner = take(scene.managed()).get();

      take(owner.reportError({ManagedPhase::STATE, owner.root().id(), TreeError::CALLBACK_FAILED, {}, {}}));
      CHECK(treeCause(scene.checked().error(), TreeError::NODE_LIMIT));
      throw std::runtime_error("input observer");
    })
    .onKey(ui::key(InputKey::F2), []() -> std::expected<void, TreeError> {
      return std::unexpected(TreeError::NODE_LIMIT);
    });

  const auto result = scene.run(terminal);

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::NODE_LIMIT));
  CHECK(notifications == 1);

  const auto batch = take(scene.diagnostics());

  CHECK(batch.errors.size() == 2);
  CHECK(batch.observerFailures == 1);
  CHECK(batch.suppressedNotifications == 1);
}

TEST_CASE("late click self-removal retains owning captures through the admitted callback") {
  example::ReplayTerminal terminal({{10, "\r", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  int destroyed{}, clicked{};
  auto lifetime = std::make_shared<Lifetime>(Lifetime{destroyed});

  destroyed = 0;
  options.clock = terminal.clock();

  auto scene = ui::scene(ui::button(button, "ready"), options);

  scene.prepare().configure([&, lifetime](ManagedScene &owner) {
    return button.onClick([&, lifetime](const ClickEvent &event) {
      ++clicked;
      CHECK(event.target == button.node());
      take(owner.remove(event.target));
      CHECK(destroyed == 0);
    });
  });

  lifetime.reset();
  take(scene.run(terminal));
  CHECK(clicked == 1);
  CHECK(destroyed == 1);
}

TEST_CASE("owning initial text preserves validation, resource and finite state errors") {
  SUBCASE("invalid UTF-8 retains the original category and offset") {
    ui::Ref<LineEdit> field;
    auto scene = ui::scene(ui::lineEdit(field, std::string_view{"\xff", 1}));
    const auto result = scene.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().text);
    CHECK(result.error().text->kind == TextErrorKind::INVALID_UTF8);
    CHECK(result.error().text->byte_offset == 0);
    CHECK(field.node() == Node<LineEdit>{});
  }

  SUBCASE("missing resolver retains the original resource category") {
    auto scene = ui::scene(ui::lineEdit(i18n("field")));
    const auto result = scene.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(result.error().resource == TextResourceError::RESOLVER_REQUIRED);
  }

  SUBCASE("ordinary editors respect the existing finite state budget") {
    ui::SceneOptions options;
    ui::Ref<LineEdit> first, second;

    options.managed.maxStates = 1;

    auto scene = ui::scene(ui::column(ui::lineEdit(first, "first"), ui::lineEdit(second, "second")), options);
    const auto result = scene.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(treeCause(result.error(), TreeError::NODE_LIMIT));
    CHECK(first.node() == Node<LineEdit>{});
    CHECK(second.node() == Node<LineEdit>{});
  }
}

TEST_CASE("prepared configuration OOM notifies the original failure and closes live references") {
  constexpr std::size_t CAPTURE_INTS = 128;
  std::array<int, CAPTURE_INTS> payload{1};
  ui::Ref<LineEdit> field;
  auto scene = ui::scene(ui::lineEdit(field, "prepared"));
  int notifications{}, calls{};
  bool memoryCause{};

  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notifications;
    memoryCause = treeCause(diagnostic.error, TreeError::NO_MEMORY);
  });
  take(scene.prepareChecked());

  const auto previousTerminate = std::set_terminate(oomTerminate);

  allocationsBeforeFailure = 0;
  scene.configure([payload, &calls](ManagedScene &) {
    calls += payload.front();
  });
  allocationsBeforeFailure = -1;
  std::set_terminate(previousTerminate);

  REQUIRE_FALSE(scene.checked());
  CHECK(treeCause(scene.checked().error(), TreeError::NO_MEMORY));
  CHECK(notifications == 1);
  CHECK(memoryCause);
  CHECK(calls == 0);
  CHECK_FALSE(field.text().get());
  CHECK(take(scene.diagnostics()).errors.size() == 1);
}

TEST_CASE("pre-owner allocation failure still notifies without allocating diagnostic storage") {
  auto scene = ui::scene(ui::label("ready"));
  int notifications{};
  bool emptyNode{};

  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notifications;
    emptyNode = diagnostic.error.node == NodeId{};
  });

  const auto previousTerminate = std::set_terminate(oomTerminate);

  allocationsBeforeFailure = 0;

  const auto result = scene.prepareChecked();

  allocationsBeforeFailure = -1;
  std::set_terminate(previousTerminate);

  REQUIRE_FALSE(result);
  CHECK(notifications == 1);
  CHECK(emptyNode);
  CHECK(take(scene.diagnostics()).observerFailures == 0);
}

TEST_CASE("foreign-thread checked operations leave the UI owner healthy and move keeps stable storage") {
  auto original = ui::scene(ui::lineEdit("initial"));
  auto scene = std::move(original);
  bool rejected{};

  CHECK_FALSE(original.checked());
  std::thread worker([&] {
    scene.prepare().theme({});
    const auto prepared = scene.prepareChecked();
    const auto closed = scene.close();
    const auto run = scene.run();

    rejected = !prepared && treeCause(prepared.error(), TreeError::WRONG_THREAD) && !closed &&
               treeCause(closed.error(), TreeError::WRONG_THREAD) && !run &&
               treeCause(run.error(), TreeError::WRONG_THREAD) && !scene.diagnostics();
  });

  worker.join();
  CHECK(rejected);
  take(scene.prepareChecked());
  take(scene.checked());
  take(scene.close());
}

TEST_CASE("pending configuration budget rejects excess callbacks without executing them") {
  ui::SceneOptions options;
  int calls{};

  options.maxConfigurations = 1;

  auto scene = ui::scene(ui::label("ready"), options);

  scene
    .configure([&](ManagedScene &) {
      ++calls;
    })
    .onKey(ui::key(InputKey::F2), [] {
    });

  REQUIRE_FALSE(scene.checked());
  CHECK(treeCause(scene.checked().error(), TreeError::NODE_LIMIT));
  CHECK_FALSE(scene.prepareChecked());
  CHECK(calls == 0);
}

TEST_CASE("late configuration capture retirement rejects lifecycle reentry before cleanup") {
  struct Reentry {
    ui::Scene &scene;
    bool &blocked;

    ~Reentry() {
      const auto result = scene.close();

      blocked = !result && treeCause(result.error(), TreeError::WOULD_BLOCK);
    }
  };

  int alive{};
  bool blocked{};
  auto scene = ui::scene(ui::custom<Mounted>(std::ref(alive)));

  take(scene.prepareChecked());
  scene.configure([capture = std::make_shared<Reentry>(scene, blocked)](ManagedScene &) {
  });

  CHECK(blocked);
  REQUIRE_FALSE(scene.checked());
  CHECK(treeCause(scene.checked().error(), TreeError::WOULD_BLOCK));
  CHECK(alive == 0);
}

TEST_CASE("observer capture retirement reenters while the complete facade state is alive") {
  struct Reentry {
    ui::Scene &scene;
    bool &blocked;

    ~Reentry() {
      const auto result = scene.close();

      blocked = !result && treeCause(result.error(), TreeError::WOULD_BLOCK);
    }
  };

  bool blocked{};
  std::optional<ui::Scene> scene(ui::scene(ui::label("observer")));

  scene->onError([capture = std::make_shared<Reentry>(*scene, blocked)](const ManagedDiagnostic &) {
  });
  take(scene->prepareChecked());
  scene.reset();
  CHECK(blocked);
}

TEST_CASE("diagnostic snapshots translate allocation failure before and after ownership") {
  constexpr std::ptrdiff_t MAX_BUDGET = 8;

  enum class SnapshotPhase {
    BEFORE_OWNER,
    BEFORE_OWNER_FAILURE,
    PREPARED,
    CLOSED_FAILURE
  };

  for (const auto phase : {SnapshotPhase::BEFORE_OWNER, SnapshotPhase::BEFORE_OWNER_FAILURE, SnapshotPhase::PREPARED,
                           SnapshotPhase::CLOSED_FAILURE}) {
    ui::SceneOptions options;

    if (phase == SnapshotPhase::BEFORE_OWNER_FAILURE) {
      options.maxConfigurations = 0;
    }

    auto scene = ui::scene(ui::label("snapshot"), options);

    if (phase == SnapshotPhase::PREPARED) {
      take(scene.prepareChecked());
    } else if (phase == SnapshotPhase::CLOSED_FAILURE) {
      scene.configure([](ManagedScene &) -> std::expected<void, ManagedError> {
        return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::CALLBACK_FAILED, {}, {}});
      });
      REQUIRE_FALSE(scene.prepareChecked());
    } else if (phase == SnapshotPhase::BEFORE_OWNER_FAILURE) {
      REQUIRE_FALSE(scene.prepareChecked());
    }

    for (std::ptrdiff_t budget = 0; budget <= MAX_BUDGET; ++budget) {
      bool typed{}, threw{};
      const auto previousTerminate = std::set_terminate(oomTerminate);

      allocationsBeforeFailure = budget;

      try {
        const auto result = scene.diagnostics();

        typed = result.has_value() || result.error() == TreeError::NO_MEMORY;
      } catch (const std::bad_alloc &) {
        threw = true;
      }

      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);
      CHECK(typed);
      CHECK_FALSE(threw);
    }
  }
}

TEST_CASE("lazy facade construction propagates allocation failure without terminating") {
  constexpr std::ptrdiff_t MAX_BUDGET = 12;
  std::size_t rejected{}, created{};

  for (std::ptrdiff_t budget = 0; budget <= MAX_BUDGET; ++budget) {
    auto description = ui::label("construction");
    const auto previousTerminate = std::set_terminate(oomTerminate);

    allocationsBeforeFailure = budget;

    try {
      auto scene = ui::scene(std::move(description));

      allocationsBeforeFailure = -1;
      ++created;
    } catch (const std::bad_alloc &) {
      allocationsBeforeFailure = -1;
      ++rejected;
    }

    std::set_terminate(previousTerminate);
  }

  CHECK(rejected > 0);
  CHECK(created > 0);
}

TEST_CASE("prepare translates allocation failures and retires partial state") {
  constexpr std::ptrdiff_t BUDGETS[]{0, 1, 4, 16, 64, 128};
  std::size_t failures{};

  for (const auto budget : BUDGETS) {
    INFO("allocation budget: ", budget);
    int alive{};
    ui::Ref<LineEdit> field;
    auto scene = ui::scene(ui::column(ui::custom<Mounted>(std::ref(alive)), ui::lineEdit(field, "initial")));

    const auto previousTerminate = std::set_terminate(oomTerminate);

    allocationsBeforeFailure = budget;

    const auto result = scene.prepareChecked();

    allocationsBeforeFailure = -1;
    std::set_terminate(previousTerminate);

    if (!result) {
      ++failures;
      const auto *application = std::get_if<ApplicationError>(&result.error().cause);

      const bool applicationMemory = application && std::visit(
                                                      [](const auto &cause) {
                                                        using Cause = std::decay_t<decltype(cause)>;

                                                        if constexpr (std::same_as<Cause, ApplicationFailure>) {
                                                          return cause == ApplicationFailure::NO_MEMORY;
                                                        } else if constexpr (std::same_as<Cause, SchedulerError>) {
                                                          return cause == SchedulerError::NO_MEMORY;
                                                        } else if constexpr (std::same_as<Cause, TreeError>) {
                                                          return cause == TreeError::NO_MEMORY;
                                                        } else {
                                                          return false;
                                                        }
                                                      },
                                                      application->cause);

      CHECK((treeCause(result.error(), TreeError::NO_MEMORY) || applicationMemory));
      CHECK(alive == 0);
      CHECK(field.node() == Node<LineEdit>{});
    }

    take(scene.close());
    CHECK(alive == 0);
  }

  CHECK(failures >= 3);
}

TEST_CASE("idle facade does not poll or continuously redraw") {
  constexpr std::uint64_t IDLE_END_MS = 5000;
  example::ReplayTerminal terminal({}, IDLE_END_MS, {30, 4});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  const auto result = take(ui::scene(ui::label("idle"), options).run(terminal));

  CHECK(result.frames == 1);
}

TEST_CASE("low-level owning editor binding failure rolls back its attempted node") {
  SUBCASE("ordinary initial draft rejection preserves an earlier sibling") {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    const auto sibling = take(ui::mount(*owner, ui::label("earlier")));
    ui::Ref<LineEdit> field;
    ui::LineEditConfig config;

    config.options.limits.max_input_bytes = 1;

    const auto result = ui::mount(*owner, ui::lineEdit(field, "too long", config));

    REQUIRE_FALSE(result);
    CHECK(result.error().phase == ManagedPhase::BIND);
    CHECK(field.node() == Node<LineEdit>{});
    CHECK(take(app->tree().inspect(owner->root())).children.size() == 1);
    CHECK(app->tree().inspect(sibling));
    take(ui::mount(*owner, ui::lineEdit("retry")));
  }

  SUBCASE("allocation failures at mount and bind never strand an unpublished editor") {
    constexpr std::ptrdiff_t MAX_MOUNT_BUDGET = 160;
    std::size_t failures{}, successes{};

    for (std::ptrdiff_t budget = 0; budget <= MAX_MOUNT_BUDGET; ++budget) {
      auto app = take(Application::create());
      auto owner = take(ManagedScene::create(*app));
      const auto sibling = take(ui::mount(*owner, ui::label("earlier")));
      ui::Ref<LineEdit> field;
      auto recipe = ui::lineEdit(field, "hello");
      const auto previousTerminate = std::set_terminate(oomTerminate);

      allocationsBeforeFailure = budget;

      const auto result = ui::mount(*owner, std::move(recipe));

      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);

      if (!result) {
        ++failures;
        CHECK(treeCause(result.error(), TreeError::NO_MEMORY));
        CHECK(field.node() == Node<LineEdit>{});
        CHECK(take(app->tree().inspect(owner->root())).children.size() == 1);

        const auto consumed = ui::mount(*owner, std::move(recipe));

        REQUIRE_FALSE(consumed);
        CHECK(treeCause(consumed.error(), TreeError::INVALID_ARGUMENT));
      } else {
        ++successes;
        take(owner->remove(*result));
      }

      CHECK(app->tree().inspect(sibling));
      take(ui::mount(*owner, ui::lineEdit("retry")));
    }

    CHECK(failures > 0);
    CHECK(successes > 0);
  }
}

TEST_CASE("theme catches internal allocation transfers while caller construction stays separate") {
  constexpr std::ptrdiff_t CALIBRATION_BUDGET = 256;
  StyleSheet sheet;

  sheet.rules.resize(1);
  allocationsBeforeFailure = CALIBRATION_BUDGET;

  const auto parameterCopyCost = [](StyleSheet) {
    return CALIBRATION_BUDGET - allocationsBeforeFailure.load();
  }(sheet);

  allocationsBeforeFailure = -1;
  REQUIRE(parameterCopyCost > 0);

  for (const bool prepared : {false, true}) {
    for (const bool rvalue : {false, true}) {
      int alive{}, notifications{};
      auto scene = ui::scene(ui::custom<Mounted>(std::ref(alive)));

      scene.onError([&](const ManagedDiagnostic &) {
        ++notifications;
      });

      if (prepared) {
        take(scene.prepareChecked());
      }

      bool escaped{};
      const auto previousTerminate = std::set_terminate(oomTerminate);

      allocationsBeforeFailure = parameterCopyCost;

      try {
        if (rvalue) {
          std::move(scene).theme(sheet);
        } else {
          scene.theme(sheet);
        }
      } catch (const std::bad_alloc &) {
        escaped = true;
      }

      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);
      CHECK_FALSE(escaped);
      REQUIRE_FALSE(scene.checked());
      CHECK(treeCause(scene.checked().error(), TreeError::NO_MEMORY));
      CHECK(scene.checked().error().phase == ManagedPhase::STYLE);
      CHECK(notifications == 1);
      CHECK(alive == 0);
    }
  }

  auto scene = ui::scene(ui::label("pre-entry"));
  bool callerEscaped{};

  allocationsBeforeFailure = 0;

  try {
    scene.theme(sheet);
  } catch (const std::bad_alloc &) {
    callerEscaped = true;
  }

  allocationsBeforeFailure = -1;
  CHECK(callerEscaped);
  CHECK(scene.checked());

  for (const bool prepared : {false, true}) {
    auto owner = ui::scene(ui::label("theme stage"));
    auto stage = ui::theme(sheet);
    bool escaped{}, typed{};

    if (prepared) {
      take(owner.prepareChecked());
    }

    allocationsBeforeFailure = 0;

    try {
      auto result = std::move(owner) | std::move(stage);
      const auto status = result.checked();

      typed = !status && treeCause(status.error(), TreeError::NO_MEMORY) && status.error().phase == ManagedPhase::STYLE;
    } catch (const std::bad_alloc &) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;
    CHECK_FALSE(escaped);
    CHECK(typed);
  }
}

namespace {
struct ThrowingTransfer {
  bool &armed;
  bool memoryFailure;
  int &calls;

  ThrowingTransfer(bool &enabled, bool memory, int &invoked) : armed(enabled), memoryFailure(memory), calls(invoked) {
  }

  ThrowingTransfer(const ThrowingTransfer &) = default;

  ThrowingTransfer(ThrowingTransfer &&other)
      : armed(other.armed), memoryFailure(other.memoryFailure), calls(other.calls) {
    if (armed) {
      if (memoryFailure) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("callback transfer");
    }
  }

  void operator()(ManagedScene &) {
    ++calls;
  }

  void operator()() {
    ++calls;
  }
};
} // namespace

TEST_CASE("rvalue and pipe callback transfers retain typed sticky failures") {
  for (const bool prepared : {false, true}) {
    for (const bool memory : {false, true}) {
      for (const bool key : {false, true}) {
        int alive{}, calls{}, notifications{};
        bool armed{true}, escaped{};
        auto scene = ui::scene(ui::custom<Mounted>(std::ref(alive)));
        ThrowingTransfer callback(armed, memory, calls);
        const auto origin = std::source_location::current();

        scene.onError([&](const ManagedDiagnostic &) {
          ++notifications;
        });

        if (prepared) {
          take(scene.prepareChecked());
        }

        try {
          if (key) {
            std::move(scene).onKey(ui::key(InputKey::F2), callback, origin);
          } else {
            std::move(scene).configure(callback, origin);
          }
        } catch (...) {
          escaped = true;
        }

        CHECK_FALSE(escaped);
        REQUIRE_FALSE(scene.checked());
        CHECK(treeCause(scene.checked().error(), memory ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED));
        CHECK(notifications == 1);
        CHECK(calls == 0);
        CHECK(alive == 0);
        CHECK(take(scene.diagnostics()).errors.front().origin.line() == origin.line());
      }
    }
  }

  for (const bool memory : {false, true}) {
    for (const bool key : {false, true}) {
      int calls{};
      bool armed{}, escaped{}, typed{};
      ThrowingTransfer callback(armed, memory, calls);
      auto scene = ui::scene(ui::label("pipe"));

      take(scene.prepareChecked());

      const auto dispatch = [&](auto stage) {
        armed = true;

        try {
          auto result = std::move(scene) | std::move(stage);
          const auto status = result.checked();

          typed = !status && treeCause(status.error(), memory ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED);
        } catch (...) {
          escaped = true;
        }
      };

      if (key) {
        dispatch(ui::onKey(ui::key(InputKey::F2), callback));
      } else {
        dispatch(ui::configure(callback));
      }

      CHECK_FALSE(escaped);
      CHECK(typed);
      CHECK(calls == 0);
    }
  }
}

TEST_CASE("owned state and option deleters reenter before facade state teardown") {
  std::optional<ui::Scene> scene;
  ui::Scene *activeScene{};
  int releases{}, configurations{};
  bool closeBlocked{}, quitBlocked{}, prepareBlocked{}, snapshotSafe{}, managedClosed{};

  scene.emplace(ui::scene([&](ManagedScene &owner) {
    take(owner.state(std::shared_ptr<int>(new int(1), [&](int *value) {
      ++releases;

      const auto close = activeScene->close();
      const auto quit = activeScene->quit();
      const auto prepare = activeScene->prepareChecked();

      closeBlocked = !close && treeCause(close.error(), TreeError::WOULD_BLOCK);
      quitBlocked = !quit && treeCause(quit.error(), TreeError::WOULD_BLOCK);
      prepareBlocked = !prepare && treeCause(prepare.error(), TreeError::WOULD_BLOCK);
      activeScene->configure([&](ManagedScene &) {
        ++configurations;
      });

      const auto snapshot = activeScene->diagnostics();

      snapshotSafe = snapshot && snapshot->closed;
      managedClosed = !activeScene->managed() && treeCause(activeScene->managed().error(), TreeError::CLOSED);
      delete value;
    })));

    return ui::label("state");
  }));
  // Optional may disengage before destruction; the object remains live throughout its destructor body.
  activeScene = std::addressof(*scene);

  take(scene->prepareChecked());
  scene.reset();

  CHECK(releases == 1);
  CHECK(closeBlocked);
  CHECK(quitBlocked);
  CHECK(prepareBlocked);
  CHECK(snapshotSafe);
  CHECK(managedClosed);
  CHECK(configurations == 0);

  ui::SceneOptions options;
  int diagnosticsReleases{};
  bool diagnosticsBlocked{}, diagnosticsSafe{};

  options.application.scheduler.diagnostics = std::shared_ptr<Diagnostics>(new Diagnostics, [&](Diagnostics *value) {
    ++diagnosticsReleases;

    const auto close = activeScene->close();
    const auto snapshot = activeScene->diagnostics();

    diagnosticsBlocked = !close && treeCause(close.error(), TreeError::WOULD_BLOCK);
    diagnosticsSafe = snapshot && snapshot->closed;
    delete value;
  });
  scene.emplace(ui::scene(ui::label("owned options"), options));
  activeScene = std::addressof(*scene);

  options.application.scheduler.diagnostics.reset();
  take(scene->prepareChecked());
  scene.reset();

  CHECK(diagnosticsReleases == 1);
  CHECK(diagnosticsBlocked);
  CHECK(diagnosticsSafe);
}

TEST_CASE("runApp method function and final pipe share transport cleanup and exit policy") {
  for (int spelling : {0, 1, 2, 3}) {
    StderrCapture capture;
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    int alive{};

    options.clock = terminal.clock();

    auto scene = ui::scene(ui::custom<Mounted>(std::ref(alive)), options);
    const auto status = spelling == 0   ? scene.runApp(terminal)
                        : spelling == 1 ? ui::runApp(scene, terminal)
                        : spelling == 2 ? scene | ui::runApp(terminal)
                                        : std::move(scene) | ui::runApp(terminal);

    CHECK(status == 0);
    CHECK(terminal.closed);
    CHECK(alive == 0);
    CHECK(scene.checked()); // Even the rvalue final stage borrows instead of relocating the facade.
    CHECK(capture.contents().empty());
  }
}

TEST_CASE("onError reaction and final structured sink coexist independently of modifier order") {
  for (bool reportFirst : {false, true}) {
    StderrCapture capture;
    FailingTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    int observed{}, reported{}, alive{};
    std::optional<ui::SceneReport> delivered;

    options.clock = terminal.clock();

    auto reaction = [&](const ManagedDiagnostic &) {
      ++observed;
    };
    auto sink = [&](const ui::SceneReport &report) {
      ++reported;
      CHECK(terminal.closed);
      CHECK(alive == 0);
      delivered = report;
    };
    auto scene = [&] {
      auto owner = ui::scene(ui::custom<Mounted>(std::ref(alive)), options);

      if (reportFirst) {
        return std::move(owner) | ui::reportOnError(sink) | ui::onError(reaction);
      }

      return std::move(owner) | ui::onError(reaction) | ui::reportOnError(sink);
    }();

    CHECK(ui::runApp(scene, terminal) == 1);
    CHECK((scene | ui::runApp(terminal)) == 1);
    CHECK(observed == 1);
    CHECK(reported == 1);
    REQUIRE(delivered);
    REQUIRE(delivered->failure);
    REQUIRE(delivered->failure->application);
    REQUIRE(delivered->failure->application->restoration);
    CHECK(delivered->failure->application->restoration->native_code == 9);
    CHECK(delivered->failure->phase == scene.checked().error().phase);
    CHECK(delivered->failure->cause.index() == scene.checked().error().cause.index());
    CHECK(capture.contents().empty());
  }
}

TEST_CASE("reporting sink throws once with original primary and no recursive reaction") {
  StderrCapture capture;
  OrderedFailureTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int observed{}, reported{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("failure"), options);

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++observed;
    })
    .reportOnError([&](const ui::SceneReport &report) {
      ++reported;
      CHECK(terminal.closed);
      REQUIRE(report.failure);

      throw std::runtime_error("sink delivery");
    });

  CHECK(scene.runApp(terminal) == 1);
  CHECK(scene.runApp(terminal) == 1);
  CHECK(observed == 1);
  CHECK(reported == 1);
  CHECK(terminal.stderrAtClose == 0);
  CHECK(take(scene.diagnostics()).observerFailures == 0);

  const auto output = capture.contents();

  CHECK(output.find("ApplicationError(phase=") != std::string::npos);
  CHECK(output.find("TerminalError(kind=SYSTEM, native=7") != std::string::npos);
  CHECK(output.find("restoration=TerminalError(kind=SYSTEM, native=9") != std::string::npos);
  CHECK(output.find("sink delivery") == std::string::npos);
  CHECK(std::count(output.begin(), output.end(), '\n') == 1);
}

TEST_CASE("final sink retains supplied cleanup envelope and rejects quiet reentry") {
  StderrCapture capture;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  auto scene = ui::scene(ui::label("setup"));
  int observed{}, reported{};

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++observed;
    })
    .reportOnError([&](const ui::SceneReport &report) {
      ++reported;
      REQUIRE(report.failure);
      CHECK(treeCause(*report.failure, TreeError::INVALID_ARGUMENT));
      CHECK(report.failure->cleanup == TreeError::CALLBACK_FAILED);
      const auto blocked = scene.close();

      REQUIRE_FALSE(blocked);
      CHECK(treeCause(blocked.error(), TreeError::WOULD_BLOCK));
      CHECK(ui::runApp(scene, terminal) == 1);
      CHECK(treeCause(scene.quit().error(), TreeError::WOULD_BLOCK));
      CHECK(treeCause(scene.checked().error(), TreeError::INVALID_ARGUMENT));
    });
  scene.configure([](ManagedScene &) -> std::expected<void, ManagedError> {
    return std::unexpected(
      ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, TreeError::CALLBACK_FAILED});
  });

  CHECK(scene.runApp(terminal) == 1);
  CHECK(reported == 1);
  CHECK(observed == 1);
  CHECK_FALSE(terminal.closed); // Preparation failed before terminal acquisition.
  CHECK(capture.contents().empty());
}

TEST_CASE("custom summary delivery owns captures and clearing sink restores default") {
  for (bool clear : {false, true}) {
    StderrCapture capture;
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    int destroyed{}, reported{}, observed{};

    options.clock = terminal.clock();

    {
      auto scene = ui::scene(ui::label("summary"), options);
      auto lifetime = std::make_shared<Lifetime>(destroyed);

      scene
        .reportOnError([&, lifetime](const ui::SceneReport &report) {
          ++reported;
          CHECK(destroyed == 0);
          CHECK(terminal.closed);
          CHECK_FALSE(report.failure);
          CHECK(report.retainedErrors == 1);
          CHECK(report.droppedErrors == 0);
        })
        .onError([&](const ManagedDiagnostic &) {
          ++observed;
        });
      lifetime.reset();
      scene.prepare().configure([](ManagedScene &owner) {
        take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
      });

      if (clear) {
        scene.reportOnError({});
        CHECK(destroyed == 1);
      }

      CHECK(scene.runApp(terminal) == 0);
      CHECK(observed == 1);
      CHECK(reported == (clear ? 0 : 1));
      CHECK(take(scene.diagnostics()).errors.size() == 1);
    }

    CHECK(destroyed == 1);

    const auto output = capture.contents();

    CHECK(clear ? output.find("recoverable diagnostics: retained=1") != std::string::npos : output.empty());
  }
}

TEST_CASE("success-summary reporting is read-only during sink replacement and lifecycle reentry") {
  StderrCapture capture;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int reported{}, reacted{}, replacementCalls{}, destroyed{};

  options.clock = terminal.clock();

  {
    auto scene = ui::scene(ui::label("summary"), options);
    auto lifetime = std::make_shared<Lifetime>(destroyed);

    scene
      .onError([&](const ManagedDiagnostic &) {
        ++reacted;
      })
      .reportOnError([&, lifetime](const ui::SceneReport &report) {
        ++reported;
        CHECK_FALSE(report.failure);
        CHECK(terminal.closed);
        CHECK(destroyed == 0);
        CHECK(treeCause(scene.close().error(), TreeError::WOULD_BLOCK));
        CHECK(treeCause(scene.quit().error(), TreeError::WOULD_BLOCK));
        CHECK(treeCause(scene.run(terminal).error(), TreeError::WOULD_BLOCK));
        CHECK(ui::runApp(scene, terminal) == 1);
        scene.reportOnError(); // Rejected self-removal keeps the executing capture alive.
        scene.reportOnError([&](const ui::SceneReport &) {
          ++replacementCalls;
        });
        scene.onError({}).prepare().configure([](ManagedScene &) {
        });
        CHECK(scene.checked());
        CHECK(reacted == 1);
        CHECK(destroyed == 0);
      });
    lifetime.reset();
    scene.prepare().configure([](ManagedScene &owner) {
      take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
    });

    CHECK(scene.runApp(terminal) == 0);
    CHECK(scene.checked());
    CHECK(reported == 1);
    CHECK(reacted == 1);
    CHECK(replacementCalls == 0);
  }

  CHECK(destroyed == 1);
  CHECK(capture.contents().empty());
}

TEST_CASE("runtime terminal exceptions use existing typed boundaries before final reporting") {
  struct ThrowingTerminal : example::ReplayTerminal {
    bool memoryFailure{};

    explicit ThrowingTerminal(bool memory) : ReplayTerminal({}, 100, {30, 4}), memoryFailure(memory) {
    }

    std::expected<CellSize, TerminalError> size() const override {
      if (memoryFailure) {
        throw std::bad_alloc();
      }

      throw 7;
    }
  };

  for (bool memory : {false, true}) {
    StderrCapture capture;
    ThrowingTerminal terminal(memory);
    ui::SceneOptions options;
    std::optional<ui::SceneReport> delivered;

    options.clock = terminal.clock();

    auto scene = ui::scene(ui::label("runtime"), options);

    scene.reportOnError([&](const ui::SceneReport &report) {
      CHECK(terminal.closed);
      delivered = report;
    });

    CHECK(scene.runApp(terminal) == 1);
    REQUIRE(delivered);
    REQUIRE(delivered->failure);
    const auto *application = std::get_if<ApplicationError>(&delivered->failure->cause);

    REQUIRE(application);
    const auto *failure = std::get_if<ApplicationFailure>(&application->cause);

    REQUIRE(failure);
    CHECK(*failure == (memory ? ApplicationFailure::NO_MEMORY : ApplicationFailure::CALLBACK_FAILED));
    CHECK(capture.contents().empty());
  }
}

TEST_CASE("recoverable-only throwing sink keeps success and falls back once") {
  StderrCapture capture;
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int observed{}, reported{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("summary"), options);

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++observed;
    })
    .reportOnError([&](const ui::SceneReport &report) {
      ++reported;
      CHECK(terminal.closed);
      CHECK_FALSE(report.failure);

      throw 7;
    });
  scene.prepare().configure([](ManagedScene &owner) {
    take(owner.reportError({ManagedPhase::TEXT, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
  });

  CHECK(scene.runApp(terminal) == 0);
  CHECK(scene.checked());
  CHECK(scene.runApp(terminal) == 1);
  CHECK(observed == 1);
  CHECK(reported == 1);

  const auto output = capture.contents();

  CHECK(output.find("recoverable diagnostics: retained=1, dropped=0") != std::string::npos);
  CHECK(std::count(output.begin(), output.end(), '\n') == 1);
}

TEST_CASE("layer facade methods and pipes preserve owning overrides across theme changes") {
  for (bool pipe : {false, true}) {
    // Keep each transition beyond the frame pacing interval so each gets a preparation pass.
    example::ReplayTerminal terminal({{50, "\x1bOQ", {}}, {100, "\x1bOR", {}}}, 200, {30, 4});
    ui::SceneOptions options;
    ui::Ref<Label> label;
    const RgbColor foreground{255, 60, 80};
    StyleSheet overrideSheet;
    StyleRule overrideRule;
    std::size_t observations{};
    std::size_t themeChanges{};
    std::size_t liveChanges{};

    options.clock = terminal.clock();
    overrideRule.selector.id = "message";
    overrideRule.declarations.foreground = foreground;
    overrideSheet.rules.push_back(overrideRule);

    auto recipe = ui::label(label, "layered").id("message").onPrepare([&](Label &widget, Node<Widget>) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.foreground == foreground);
      ++observations;

      return std::expected<void, TreeError>{};
    });
    auto scene = pipe ? ui::scene(std::move(recipe), options) | ui::styleLayers() |
                          ui::styleLayer(StyleTier::USER, "contrast", overrideSheet)
                      : ui::scene(std::move(recipe), options);

    if (!pipe) {
      scene.styleLayers().styleLayer(StyleTier::USER, "contrast", overrideSheet);
    }

    CHECK(label.node() == Node<Label>{});
    REQUIRE(scene.checked());
    scene.prepare();
    REQUIRE(scene.checked());

    auto &owner = take(scene.managed()).get();
    constexpr auto THEME = style::ct::themeFromTokens(
      ThemeTokens{{220, 225, 232}, {24, 28, 36}, {75, 180, 220}, {125, 130, 140}, {}, {75, 180, 220}});
    auto live = take(style::ct::live(owner, THEME));

    scene.onKey(ui::key(InputKey::F2), [&] {
      scene.theme(turboTheme());
      CHECK(scene.checked());
      ++themeChanges;
    });
    scene.onKey(ui::key(InputKey::F3), [&] {
      live = take(style::ct::live(owner, THEME));
      take(live.set<"foreground">(RgbColor{1, 2, 3}));
      ++liveChanges;
    });
    REQUIRE(scene.run(terminal));
    CHECK(observations >= 3);
    CHECK(themeChanges == 1);
    CHECK(liveChanges == 1);
    CHECK(terminal.closed);
    CHECK_FALSE(label.text().get());
  }
}

TEST_CASE("layer facade keeps configuration failures and allocation errors in the collector") {
  SUBCASE("missing opt-in remains a typed setup error") {
    auto scene = ui::scene(ui::label("setup"));

    scene.styleLayer(StyleTier::USER, "contrast", {}).prepare();
    REQUIRE_FALSE(scene.checked());
    const auto result = scene.checked();
    const auto *cause = std::get_if<StyleError>(&result.error().cause);

    REQUIRE(cause);
    CHECK(cause->cause == TreeError::INVALID_ARGUMENT);
    CHECK_FALSE(scene.managed());
  }

  SUBCASE("pipe owning copy failure is captured after stage construction") {
    StyleSheet sheet;
    StyleRule rule;

    rule.selector.id = "message";
    rule.declarations.bold = true;
    sheet.rules.push_back(rule);

    auto scene = ui::scene(ui::label("setup"));
    auto stage = ui::styleLayer(StyleTier::USER, "contrast", sheet);

    const auto checkFailure = [&](ui::Scene owner, auto pendingStage, std::ptrdiff_t budget) {
      bool escaped{};
      bool typed{};
      bool succeeded{};
      const auto previousTerminate = std::set_terminate(oomTerminate);

      allocationsBeforeFailure = budget;

      try {
        auto result = std::move(owner) | std::move(pendingStage);
        const auto status = result.checked();

        succeeded = status.has_value();
        typed = !status && treeCause(status.error(), TreeError::NO_MEMORY);
      } catch (const std::bad_alloc &) {
        escaped = true;
      }

      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);
      CHECK_FALSE(escaped);
      CHECK((typed || succeeded));

      return succeeded;
    };

    CHECK_FALSE(checkFailure(std::move(scene), std::move(stage), 0));

    // Cover copies, callback storage and pending registration, not only the first allocation.
    constexpr std::ptrdiff_t MAX_ALLOCATION_PROBE = 64;
    bool reachedSuccess{};

    for (std::ptrdiff_t budget = 1; budget <= MAX_ALLOCATION_PROBE; ++budget) {
      reachedSuccess =
        checkFailure(ui::scene(ui::label("setup")), ui::styleLayer(StyleTier::USER, "contrast", sheet), budget);

      if (reachedSuccess) {
        break;
      }
    }

    CHECK(reachedSuccess);
  }
}

TEST_CASE("U125 short quit uses actual method and pipe callbacks with explicit API compatibility") {
  using ExplicitQuit = ui::QuitAction (*)(ui::SceneRef, std::source_location);
  ExplicitQuit explicitQuit = ui::quit;
  static_assert(std::same_as<decltype(ui::quit()), void>);
  std::array<std::string, 4> baselines;

  for (const bool shortForm : {false, true}) {
    for (const bool pipe : {false, true}) {
      for (const bool click : {false, true}) {
        example::ReplayTerminal terminal({{50, click ? "\r" : "\x1b[21~", {}}}, 100, {30, 4});
        ui::SceneOptions options;
        ui::Ref<Button> button;

        options.clock = terminal.clock();

        auto scene = ui::scene(ui::button(button, "Exit"), options);

        if (click) {
          if (pipe) {
            if (shortForm) {
              scene | ui::onClick(button, ui::quit);
            } else {
              scene | ui::onClick(button, explicitQuit(scene.ref(), std::source_location::current()));
            }
          } else {
            scene.configure([&](ManagedScene &) {
              return shortForm ? button.onClick([] {
                ui::quit();
              })
                               : button.onClick(ui::quit(scene.ref()));
            });
          }
        } else if (pipe) {
          if (shortForm) {
            scene | ui::onKey(key::F10, ui::quit);
          } else {
            scene | ui::onKey(key::F10, ui::quit(scene.ref()));
          }
        } else if (shortForm) {
          scene.onKey(key::F10, ui::quit);
        } else {
          scene.onKey(key::F10, ui::quit(scene.ref()));
        }

        const auto result = scene.run(terminal);

        CHECK(result);
        CHECK(terminal.closed);
        CHECK(options.clock.read(options.clock.context) == 50);
        CHECK_FALSE(terminal.output.empty());
        CHECK(take(scene.diagnostics()).errors.empty());

        auto &baseline = baselines[(pipe ? 2 : 0) + (click ? 1 : 0)];

        if (baseline.empty()) {
          baseline = terminal.output;
        } else {
          CHECK(terminal.output == baseline);
        }

        const auto unavailable = ui::detail::SceneAccess::currentRef().quit();

        REQUIRE_FALSE(unavailable);
        CHECK(unavailable.error().phase == ManagedPhase::RUN);
        CHECK(treeCause(unavailable.error(), TreeError::CLOSED));
        ui::quit();
      }
    }
  }
}

TEST_CASE("U125 prepared direct managed callbacks target their own facade and restore the outer scene") {
  enum class Dispatch {
    KEY,
    CLICK,
    INPUT
  };

  for (const auto dispatch : {Dispatch::KEY, Dispatch::CLICK, Dispatch::INPUT}) {
    for (const bool quitOuter : {false, true}) {
      example::ReplayTerminal outerTerminal({}, 100, {30, 4});
      example::ReplayTerminal innerTerminal({}, 100, {30, 4});
      ui::SceneOptions outerOptions;
      ui::SceneOptions innerOptions;
      ui::Ref<Button> button;
      int later{};

      outerOptions.clock = outerTerminal.clock();
      innerOptions.clock = innerTerminal.clock();

      auto inner = ui::scene(ui::button(button, "Inner"), innerOptions);

      inner.onKey(key::Q, ui::quit).configure([&](ManagedScene &owner) {
        if (dispatch == Dispatch::INPUT) {
          take(owner.onInput([](const InputEvent &) -> std::expected<bool, ManagedError> {
            ui::quit();

            return true;
          }));
        } else if (dispatch == Dispatch::CLICK) {
          take(button.onClick([] {
            ui::quit();
          }));
        }
      });
      take(inner.prepareChecked());

      auto moved = std::move(inner);

      CHECK_FALSE(inner.checked());

      auto &managed = take(moved.managed()).get();
      auto &application = ui::detail::SceneAccess::application(managed);
      auto outer = ui::scene(ui::label("Outer"), outerOptions);

      outer
        .configure([&](ManagedScene &) {
          if (dispatch == Dispatch::CLICK) {
            take(application.router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
          } else {
            CHECK(take(managed.input(application, KeyEvent{InputKey::CHARACTER, U'Q'})));
          }

          if (quitOuter) {
            ui::quit();
          }
        })
        .configure([&](ManagedScene &) {
          ++later;
        });

      CHECK(outer.prepareChecked());
      CHECK(later == 1);
      CHECK(moved.run(innerTerminal));
      CHECK(innerTerminal.output.empty());
      CHECK(outer.run(outerTerminal));
      CHECK(outerTerminal.output.empty() == quitOuter);
    }
  }
}

TEST_CASE("U125 deferred router capture retirement masks both current quit and outer setup failure") {
  for (const bool pipe : {false, true}) {
    example::ReplayTerminal outerTerminal({}, 100, {30, 4});
    example::ReplayTerminal innerTerminal({}, 100, {30, 4});
    ui::SceneOptions outerOptions;
    ui::SceneOptions innerOptions;
    ui::Ref<Button> button;
    ui::Ref<Label> missing;
    int retired{}, later{};
    bool attributed{}, currentUnavailable{};

    outerOptions.clock = outerTerminal.clock();
    innerOptions.clock = innerTerminal.clock();

    struct Capture {
      ui::Ref<Label> &missing;
      int &retired;
      bool &attributed;
      bool &currentUnavailable;

      ~Capture() {
        const auto current = ui::detail::SceneAccess::currentRef().quit();

        currentUnavailable = !current && treeCause(current.error(), TreeError::CLOSED);
        ui::quit();

        auto failure = missing.text() = "retired capture";

        attributed = failure.result().error().setupStopped;
        ++retired;
      }
    };

    auto capture = std::make_shared<Capture>(missing, retired, attributed, currentUnavailable);
    auto inner = ui::scene(ui::vbox(ui::label("Inner"), ui::button(button, "Remove")), innerOptions);
    ManagedScene *managed{};

    inner.configure([&](ManagedScene &owner) {
      managed = &owner;
    });

    if (pipe) {
      inner | ui::onClick(button, [&, capture] {
        take(managed->remove(button.node()));
      });
    } else {
      inner.configure([&](ManagedScene &) {
        return button.onClick([&, capture] {
          take(managed->remove(button.node()));
        });
      });
    }

    take(inner.prepareChecked());
    capture.reset();

    auto &application = ui::detail::SceneAccess::application(*managed);
    auto outer = ui::scene(ui::label("Outer"), outerOptions);

    outer
      .configure([&](ManagedScene &) {
        take(application.router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
        take(application.router().focused());
      })
      .configure([&](ManagedScene &) {
        ++later;
      });

    CHECK(outer.prepareChecked());
    CHECK(retired == 1);
    CHECK_FALSE(attributed);
    CHECK(currentUnavailable);
    CHECK(later == 1);
    CHECK(inner.run(innerTerminal));
    CHECK_FALSE(innerTerminal.output.empty());
    CHECK(outer.run(outerTerminal));
    CHECK_FALSE(outerTerminal.output.empty());
  }
}

TEST_CASE("U125 owned notification preserves its own setup and suspends another setup") {
  for (const bool ownSetup : {false, true}) {
    ui::Ref<LineEdit> field;
    ui::Ref<Label> missing;
    bool attributed{};
    int notifications{}, later{};
    ui::LineEditConfig config;

    config.result = [&](LineEditResult) -> std::expected<void, TreeError> {
      auto failure = missing.text() = "notification";

      attributed = failure.result().error().setupStopped;
      ++notifications;

      return {};
    };

    auto inner = ui::scene(ui::lineEdit(field, "initial", std::move(config)));

    take(inner.prepareChecked());

    const auto mutate = [&](ManagedScene &) {
      CHECK(field.text().set("changed").result());
    };

    if (ownSetup) {
      inner.configure(mutate).configure([&](ManagedScene &) {
        ++later;
      });
      CHECK_FALSE(inner.checked());
      CHECK(attributed);
      CHECK(later == 0);
    } else {
      auto outer = ui::scene(ui::label("outer"));

      outer.configure(mutate).configure([&](ManagedScene &) {
        ++later;
      });
      CHECK(outer.prepareChecked());
      CHECK_FALSE(attributed);
      CHECK(inner.checked());
      CHECK(later == 1);
    }

    CHECK(notifications == 1);
  }
}

TEST_CASE("U125 rejected standard managed callback parameters retire before both masks end") {
  for (const bool preparation : {false, true}) {
    example::ReplayTerminal terminal({}, 100, {30, 4});
    ui::SceneOptions options;
    ui::Ref<Label> missing;
    int retired{}, later{};
    bool attributed{}, unavailable{};

    options.clock = terminal.clock();

    struct Capture {
      ui::Ref<Label> &missing;
      int &retired;
      bool &attributed;
      bool &unavailable;

      ~Capture() {
        auto context = ui::detail::SceneAccess::currentRef().quit();

        unavailable = !context && treeCause(context.error(), TreeError::CLOSED);
        ui::quit();

        auto failure = missing.text() = "rejected registration";

        attributed = failure.result().error().setupStopped;
        ++retired;
      }
    };

    auto inner = ui::scene(ui::label("inner"));

    take(inner.prepareChecked());

    auto &owner = take(inner.managed()).get();
    auto capture = std::make_shared<Capture>(missing, retired, attributed, unavailable);
    ManagedEventHandler<Widget> event = [capture](Widget &,
                                                  const RoutedEvent &) -> std::expected<EventReply, TreeError> {
      return EventReply::IGNORED;
    };
    ManagedPrepareHandler<Widget> prepare = [capture](Widget &, Node<Widget>) -> std::expected<void, TreeError> {
      return {};
    };

    if (preparation) {
      event = {};
    } else {
      prepare = {};
    }

    capture.reset();

    auto outer = ui::scene(ui::label("outer"), options);

    outer
      .configure([&](ManagedScene &) {
        const auto rejected = preparation ? owner.onPrepare(Node<Widget>{}, std::move(prepare))
                                          : owner.onEvent(Node<Widget>{}, std::move(event));

        CHECK_FALSE(rejected);
      })
      .configure([&](ManagedScene &) {
        ++later;
      });

    CHECK(outer.prepareChecked());
    CHECK(retired == 1);
    CHECK(unavailable);
    CHECK_FALSE(attributed);
    CHECK(later == 1);
    CHECK(outer.run(terminal));
    CHECK_FALSE(terminal.output.empty());
  }
}

TEST_CASE("U125 no current and independent factory cleanup do not reuse an outer scene") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  int later{}, sinks{};
  bool workerClosed{};

  options.clock = terminal.clock();

  auto outer = ui::scene(ui::label("outer"), options);

  outer
    .configure([&](ManagedScene &) {
      std::thread worker([&] {
        const auto current = ui::detail::SceneAccess::currentRef().quit();

        workerClosed = !current && treeCause(current.error(), TreeError::CLOSED);
        ui::quit();
      });

      worker.join();

      ui::RunAppOptions failed;

      failed.report = [&](const ui::SceneReport &) {
        ui::quit();
        ++sinks;
      };

      const auto code = ui::runApp(
        []() -> std::expected<ui::Description<Label>, ManagedError> {
          ui::quit();

          return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::NODE_LIMIT});
        },
        [](ui::Scene &) {
        },
        failed);

      CHECK(code == 1);
    })
    .configure([&](ManagedScene &) {
      ++later;
    });

  CHECK(outer.prepareChecked());
  CHECK(workerClosed);
  CHECK(sinks == 1);
  CHECK(later == 1);
  CHECK(outer.run(terminal));
  CHECK_FALSE(terminal.output.empty());

  const auto current = ui::detail::SceneAccess::currentRef().quit();

  REQUIRE_FALSE(current);
  CHECK(current.error().phase == ManagedPhase::RUN);
  CHECK(treeCause(current.error(), TreeError::CLOSED));
  ui::quit();
}

TEST_CASE("U125 managed property user code keeps same-owner setup and masks unrelated setup") {
  for (const bool ownSetup : {false, true}) {
    for (const bool copiedValue : {false, true}) {
      ui::Ref<Label> missing;
      bool attributed{};
      int comparisons{}, later{};
      example::ReplayTerminal outerTerminal({}, 100, {30, 4});
      example::ReplayTerminal innerTerminal({}, 100, {30, 4});
      ui::SceneOptions innerOptions;
      ui::SceneOptions outerOptions;

      innerOptions.clock = innerTerminal.clock();
      outerOptions.clock = outerTerminal.clock();

      struct Value {
        int number;
        std::function<void()> compared;

        bool operator==(const Value &other) const {
          compared();

          return number == other.number;
        }
      };

      const auto compared = [&] {
        auto failure = missing.text() = "property comparison";

        attributed = failure.result().error().setupStopped;
        ++comparisons;
        ui::quit();
      };
      ManagedState<Value> state;
      auto inner = ui::scene(
        [&](ManagedScene &owner) {
          state = take(owner.state(Value{1, compared}));

          return ui::label("inner");
        },
        innerOptions);

      take(inner.prepareChecked());

      const auto change = [&](ManagedScene &) {
        if (copiedValue) {
          const Value value{2, compared};

          CHECK(state.set(value));
        } else {
          CHECK(state.set(Value{2, compared}));
        }
      };

      if (ownSetup) {
        inner.configure(change).configure([&](ManagedScene &) {
          ++later;
        });
        CHECK_FALSE(inner.checked());
        CHECK(attributed);
        CHECK(later == 0);
      } else {
        auto outer = ui::scene(ui::label("outer"), outerOptions);

        outer.configure(change).configure([&](ManagedScene &) {
          ++later;
        });
        CHECK(outer.prepareChecked());
        CHECK_FALSE(attributed);
        CHECK(later == 1);
        CHECK(inner.run(innerTerminal));
        CHECK(innerTerminal.output.empty());
        CHECK(outer.run(outerTerminal));
        CHECK_FALSE(outerTerminal.output.empty());
      }

      CHECK(comparisons == 1);
    }
  }
}

TEST_CASE("U125 rejected current quit reports once at the registered site without bypassing notify guards") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  int notifications{};
  Application *application{};
  const auto origin = std::source_location::current();

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::button(button, "Button"), options);

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++notifications;
      take(application->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
    })
    .configure([&](ManagedScene &owner) {
      application = &ui::detail::SceneAccess::application(owner);

      return button.onClick(ui::quit, origin);
    });
  take(scene.prepareChecked());

  auto &owner = take(scene.managed()).get();

  take(owner.reportError(ManagedError{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT}));

  const auto batch = take(scene.diagnostics());

  CHECK(notifications == 1);
  REQUIRE(batch.errors.size() == 2);
  CHECK(batch.suppressedNotifications == 1);
  CHECK(batch.errors[1].error.phase == ManagedPhase::RUN);
  CHECK(treeCause(batch.errors[1].error, TreeError::WOULD_BLOCK));
  CHECK(batch.errors[1].origin.line() == origin.line());
  CHECK(scene.checked());
  CHECK(scene.run(terminal));
  CHECK_FALSE(terminal.output.empty());
}

TEST_CASE("U125 failed input storage transfer retires its sole capture under both masks") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Label> missing;
  int retired{}, later{}, originalCalls{};
  bool attributed{}, unavailable{};

  options.clock = terminal.clock();

  struct Capture {
    ui::Ref<Label> &missing;
    int &retired;
    bool &attributed;
    bool &unavailable;

    ~Capture() {
      const auto current = ui::detail::SceneAccess::currentRef().quit();

      unavailable = !current && treeCause(current.error(), TreeError::CLOSED);
      ui::quit();

      auto failure = missing.text() = "failed transfer";

      attributed = failure.result().error().setupStopped;
      ++retired;
    }
  };

  auto inner = ui::scene(ui::label("inner"));

  take(inner.prepareChecked());

  auto &owner = take(inner.managed()).get();

  take(owner.onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++originalCalls;

    return true;
  }));

  std::function<std::expected<bool, ManagedError>(const InputEvent &)> callback =
    [capture = std::make_shared<Capture>(missing, retired, attributed, unavailable)](
      const InputEvent &) -> std::expected<bool, ManagedError> {
    return true;
  };
  auto outer = ui::scene(ui::label("outer"), options);

  outer
    .configure([&](ManagedScene &) {
      allocationsBeforeFailure = 0;

      const auto rejected = owner.onInput(std::move(callback));

      allocationsBeforeFailure = -1;
      REQUIRE_FALSE(rejected);
      CHECK(treeCause(rejected.error(), TreeError::NO_MEMORY));
      CHECK(take(owner.input(ui::detail::SceneAccess::application(owner), KeyEvent{InputKey::F2})));
    })
    .configure([&](ManagedScene &) {
      ++later;
    });

  CHECK(outer.prepareChecked());
  CHECK(retired == 1);
  CHECK(unavailable);
  CHECK_FALSE(attributed);
  CHECK(originalCalls == 1);
  CHECK(later == 1);
  CHECK(outer.run(terminal));
  CHECK_FALSE(terminal.output.empty());
}

TEST_CASE("U125 nested throwing setup restores outer context and later scenes start empty") {
  for (const bool quitOuter : {false, true}) {
    example::ReplayTerminal outerTerminal({}, 100, {30, 4});
    example::ReplayTerminal innerTerminal({}, 100, {30, 4});
    ui::SceneOptions outerOptions;
    ui::SceneOptions innerOptions;

    outerOptions.clock = outerTerminal.clock();
    innerOptions.clock = innerTerminal.clock();

    auto outer = ui::scene(ui::label("outer"), outerOptions);
    auto middle = ui::scene(ui::label("middle"));
    auto inner = ui::scene(ui::label("inner"), innerOptions);

    inner.configure([](ManagedScene &) {
      ui::quit();
    });
    middle.configure([&](ManagedScene &) {
      take(inner.prepareChecked());

      throw std::runtime_error("middle");
    });
    outer.configure([&](ManagedScene &) {
      CHECK_FALSE(middle.prepareChecked());

      if (quitOuter) {
        ui::quit();
      }
    });

    CHECK(outer.prepareChecked());
    CHECK(inner.run(innerTerminal));
    CHECK(innerTerminal.output.empty());
    CHECK(outer.run(outerTerminal));
    CHECK(outerTerminal.output.empty() == quitOuter);
  }

  StderrCapture stderrCapture;

  ui::quit();

  const auto checked = ui::detail::SceneAccess::currentRef().quit();

  REQUIRE_FALSE(checked);
  CHECK(checked.error().phase == ManagedPhase::RUN);
  CHECK(treeCause(checked.error(), TreeError::CLOSED));
  CHECK(stderrCapture.contents().empty());
}

TEST_CASE("U125 weak callback identity cannot bypass immutable foreign-thread admission") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  std::weak_ptr<ui::detail::SceneAnchor> weak;
  bool rejected{};
  int notifications{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("live"), options);

  scene
    .onError([&](const ManagedDiagnostic &) {
      ++notifications;
    })
    .configure([&](ManagedScene &) {
      weak = ui::detail::CurrentSceneScope::snapshot();
    });
  take(scene.prepareChecked());

  std::thread worker([&] {
    ui::detail::CurrentSceneScope scope(weak);

    const auto result = ui::detail::SceneAccess::currentRef().quit();

    rejected = !result && treeCause(result.error(), TreeError::WRONG_THREAD);
    ui::quit();
  });

  worker.join();
  CHECK(rejected);
  CHECK(notifications == 0);
  CHECK(scene.run(terminal));
  CHECK_FALSE(terminal.output.empty());
}

TEST_CASE("U125 prepared callback close revokes current publication before capture retirement") {
  example::ReplayTerminal terminal({}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  bool closed{};
  int later{};

  options.clock = terminal.clock();

  auto inner = ui::scene(ui::button(button, "Close"));

  inner | ui::onClick(button, [&] {
    CHECK(inner.close());

    const auto current = ui::detail::SceneAccess::currentRef().quit();

    closed = !current && treeCause(current.error(), TreeError::CLOSED);
    ui::quit();
  });
  take(inner.prepareChecked());

  auto &application = ui::detail::SceneAccess::application(take(inner.managed()).get());
  auto outer = ui::scene(ui::label("outer"), options);

  outer
    .configure([&](ManagedScene &) {
      take(application.router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
    })
    .configure([&](ManagedScene &) {
      ++later;
    });

  CHECK(outer.prepareChecked());
  CHECK(closed);
  CHECK(later == 1);
  CHECK(outer.run(terminal));
  CHECK_FALSE(terminal.output.empty());
}

TEST_CASE("unbound checkbox owns its state and commits before the business callback") {
  using namespace portals;

  for (const auto initial : {false, true}) {
    ui::Ref<CheckBox> box;
    int changes{};
    auto scene =
      ui::scene(ui::checkbox("Alerts", initial, [&](CheckBoxResult result) -> std::expected<void, TreeError> {
                  REQUIRE(result);
                  CHECK(result->changed);
                  CHECK(result->snapshot->value == !initial);
                  ++changes;

                  return {};
                }).reference(box));

    take(scene.prepareChecked());

    auto &owner = take(scene.managed()).get();
    auto &application = ui::detail::SceneAccess::application(owner);

    take(application.tree().read(box.node(), [initial](const CheckBox &value) {
      CHECK(value.checked() == initial);
    }));
    take(application.router().invoke(box.node(), ActionEvent{Action::ACTIVATE}));
    take(application.tree().read(box.node(), [initial](const CheckBox &value) {
      CHECK(value.checked() == !initial);
    }));
    CHECK(changes == 1);
    CHECK(scene.close());
  }
}

TEST_CASE("unbound checkbox partial mount retires unpublished refs and internal state") {
  using namespace portals;

  ui::SceneOptions policies;
  ui::Ref<CheckBox> first;
  ui::Ref<CheckBox> second;

  policies.managed.maxStates = 1;

  auto scene =
    ui::scene(ui::vbox(ui::checkbox("First").reference(first), ui::checkbox("Second").reference(second)), policies);
  const auto prepared = scene.prepareChecked();

  REQUIRE_FALSE(prepared);
  CHECK(prepared.error().phase == ManagedPhase::STATE);
  CHECK(treeCause(prepared.error(), TreeError::NODE_LIMIT));
  CHECK_FALSE(first.node().id().generation);
  CHECK_FALSE(second.node().id().generation);
  CHECK(scene.close());
}

TEST_CASE("unbound checkbox mount reports state allocation failure and retires the partial recipe") {
  using namespace portals;

  constexpr std::ptrdiff_t MAX_MOUNT_FAILURE_BUDGET = 256;
  bool stateFailure{};
  bool completed{};

  for (std::ptrdiff_t budget = 0; budget <= MAX_MOUNT_FAILURE_BUDGET; ++budget) {
    ui::Ref<CheckBox> box;
    auto scene = ui::scene(ui::checkbox("Alerts").reference(box));

    allocationsBeforeFailure = budget;

    const auto prepared = scene.prepareChecked();

    allocationsBeforeFailure = -1;

    if (prepared) {
      completed = true;

      break;
    }

    if (prepared.error().phase == ManagedPhase::STATE) {
      stateFailure = true;
      CHECK(treeCause(prepared.error(), TreeError::NO_MEMORY));
      CHECK(box.node() == Node<CheckBox>{});
    }

    CHECK(scene.close());
  }

  CHECK(stateFailure);
  CHECK(completed);
}

TEST_CASE("input decoder empty lifecycle remains allocation free before checkbox mount") {
  using namespace portals;

  static_assert(std::is_nothrow_move_constructible_v<InputDecoder>);
  static_assert(std::is_nothrow_move_assignable_v<InputDecoder>);
  bool created{};
  const auto previousTerminate = std::set_terminate(oomTerminate);

  try {
    allocationsBeforeFailure = 0;

    {
      auto decoder = InputDecoder::create();

      created = decoder.has_value();

      if (decoder) {
        InputDecoder moved(std::move(*decoder));

        decoder->reset();
        *decoder = std::move(moved);
        decoder->reset();
        moved.reset();
      }
    }

    allocationsBeforeFailure = -1;
    std::set_terminate(previousTerminate);
  } catch (...) {
    allocationsBeforeFailure = -1;
    std::set_terminate(previousTerminate);

    throw;
  }

  CHECK(created);
}

TEST_CASE("readonly process snapshots preserve allocation failures as typed errors") {
  using namespace portals;

  const char *arguments[]{"synthetic-test"};

  allocationsBeforeFailure = 0;

  const auto args = ui::app::Arguments::capture(1, arguments);

  allocationsBeforeFailure = -1;

  REQUIRE_FALSE(args);
  CHECK(args.error() == ui::app::ProcessContextError::NO_MEMORY);

#ifdef _WIN32
  const std::array entries{ui::app::Environment::Entry{"KNOWN", "value"}};
  const auto environment = ui::app::Environment::fromEntries(entries);

  REQUIRE(environment);

  allocationsBeforeFailure = 0;

  const auto queried = environment->get("A_SYNTHETIC_QUERY_LONG_ENOUGH_TO_REQUIRE_UTF16_STORAGE");

  allocationsBeforeFailure = -1;

  REQUIRE_FALSE(queried);
  CHECK(queried.error() == ui::app::ProcessContextError::NO_MEMORY);
#endif
}

TEST_CASE("CLI preflight sustained OOM reports before terminal acquisition") {
  using namespace portals;

  const char *arguments[]{"synthetic-cli-oom"};
  const auto environment = ui::app::Environment::fromEntries({});

  REQUIRE(environment);
  const auto beforeInitialization = ui::app::args();

  REQUIRE_FALSE(beforeInitialization);
  CHECK(beforeInitialization.error() == ui::app::ProcessContextError::UNINITIALIZED);

  struct UntouchedTerminal : example::ReplayTerminal {
    mutable int queries{};

    std::expected<CellSize, TerminalError> size() const override {
      ++queries;

      return example::ReplayTerminal::size();
    }
  };

  const auto checkPreflight = [&](bool expectedParser, bool allocateMessage = false) {
    UntouchedTerminal terminal;
    int mounted{}, reports{}, status{};
    bool parsed{}, typedFailure{}, emptyDiagnostic{}, managedFailure{}, contextFailure{};
    auto owner = ui::scene([&](ManagedScene &) {
      ++mounted;

      return ui::label("never mounted");
    });

    owner.reportOnError([&](const ui::SceneReport &report) {
      ++reports;
      typedFailure =
        report.commandLineFailure && report.commandLineFailure->code == ui::CommandLineErrorCode::NO_MEMORY;
      emptyDiagnostic = report.commandLineFailure && report.commandLineFailure->message.empty();
      contextFailure = report.commandLineFailure &&
                       report.commandLineFailure->processContext == ui::app::ProcessContextError::NO_MEMORY;
      managedFailure = report.failure.has_value();
    });

    ui::CommandLineParser parser =
      [&](const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
      parsed = true;

      if (allocateMessage) {
        return std::unexpected(ui::CommandLineError{ui::CommandLineErrorCode::INVALID_ARGUMENT,
                                                    "new nonempty diagnostic requires an allocation"});
      }

      throw std::bad_alloc();
    };
    const auto previousTerminate = std::set_terminate(oomTerminate);

    try {
      allocationsBeforeFailure = 0;
      status = owner.runApp(terminal, 1, arguments, std::move(parser));
      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);
    } catch (...) {
      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);

      throw;
    }

    CHECK(status == 2);
    CHECK(parsed == expectedParser);
    CHECK(typedFailure);
    CHECK(emptyDiagnostic);
    CHECK(contextFailure == !expectedParser);
    CHECK_FALSE(managedFailure);
    CHECK(reports == 1);
    CHECK(mounted == 0);
    CHECK(terminal.queries == 0);
    CHECK_FALSE(terminal.closed);
  };

  checkPreflight(false);
  REQUIRE(ui::app::initialize(1, arguments, *environment));
  checkPreflight(true);
  checkPreflight(true, true);

  static_assert(std::is_nothrow_default_constructible_v<ui::LaunchText>);
  static_assert(std::is_nothrow_copy_constructible_v<ui::LaunchText>);
  static_assert(std::is_nothrow_move_constructible_v<ui::LaunchText>);
  static_assert(std::is_nothrow_copy_assignable_v<ui::LaunchText>);
  static_assert(std::is_nothrow_move_assignable_v<ui::LaunchText>);
  static_assert(std::is_nothrow_copy_constructible_v<ui::CommandLineError>);
  static_assert(std::is_nothrow_move_constructible_v<ui::CommandLineError>);
  static_assert(std::is_nothrow_copy_constructible_v<ui::LaunchAction>);
  static_assert(std::is_nothrow_move_constructible_v<ui::LaunchAction>);

  constexpr const char *ORIGINAL_MESSAGE = "original CLI message retained after source and parser retirement";
  ui::CommandLineError original;

  {
    std::string source(ORIGINAL_MESSAGE);

    original = {ui::CommandLineErrorCode::INVALID_ARGUMENT, source};
    source.assign("changed source");
  }

  const ui::LaunchAction output{ui::LaunchDisposition::FINISH, std::string("synthetic help output\n")};
  UntouchedTerminal terminal;
  std::optional<ui::SceneReport> retained;
  int retired{}, nestedStatus{}, reportReentry{}, reports{}, status{};
  bool emptyText{}, ownedText{}, outputText{};
  auto owner = ui::scene(ui::label("never mounted"));

  struct Retirement {
    ui::Scene &owner;
    UntouchedTerminal &terminal;
    int &retired;
    int &status;

    ~Retirement() {
      ++retired;
      status = owner.runApp(terminal);
    }
  };

  owner.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    retained.emplace(report);
    reportReentry = owner.runApp(terminal);
  });

  ui::CommandLineParser parser =
    [failure = original,
     retirement = std::unique_ptr<Retirement>(new Retirement{owner, terminal, retired, nestedStatus})](
      const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
    return std::unexpected(failure);
  };
  const auto previousTerminate = std::set_terminate(oomTerminate);

  try {
    allocationsBeforeFailure = 0;

    ui::LaunchText empty;
    ui::LaunchText emptyView(std::string_view{});
    ui::LaunchText null(nullptr);
    ui::LaunchText copy(original.message);
    ui::LaunchText moved(std::move(copy));
    ui::LaunchText assigned;

    assigned = original.message;
    emptyText = empty.empty() && emptyView.empty() && null.empty() && copy.empty() && empty.data()[0] == '\0';
    ownedText = moved == ORIGINAL_MESSAGE && assigned == original.message && moved.data() == original.message.data();

    ui::LaunchAction copiedOutput(output);
    ui::LaunchAction movedOutput(std::move(copiedOutput));
    std::expected<ui::LaunchAction, ui::CommandLineError> outputResult(std::in_place, movedOutput);

    outputText = outputResult->output.view() == "synthetic help output\n" &&
                 outputResult->output.find("help") != std::string_view::npos &&
                 outputResult->output.size() == output.output.size() &&
                 outputResult->output.data() == output.output.data();
    status = owner.runApp(terminal, 1, arguments, std::move(parser));
    allocationsBeforeFailure = -1;
    std::set_terminate(previousTerminate);
  } catch (...) {
    allocationsBeforeFailure = -1;
    std::set_terminate(previousTerminate);

    throw;
  }

  CHECK(emptyText);
  CHECK(ownedText);
  CHECK(outputText);
  CHECK(status == 2);
  CHECK(retired == 1);
  CHECK(nestedStatus == 1);
  CHECK(reportReentry == 1);
  CHECK(reports == 1);
  REQUIRE(retained);
  REQUIRE(retained->commandLineFailure);
  CHECK(retained->commandLineFailure->code == ui::CommandLineErrorCode::INVALID_ARGUMENT);
  CHECK(retained->commandLineFailure->message == ORIGINAL_MESSAGE);
  REQUIRE(retained->failure);
  CHECK(treeCause(*retained->failure, TreeError::WOULD_BLOCK));
  CHECK(terminal.queries == 0);
  CHECK_FALSE(terminal.closed);
}
