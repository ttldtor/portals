// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../examples/replay/terminal.hpp"
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
