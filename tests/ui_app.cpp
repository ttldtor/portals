// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>

using namespace portals;

namespace {
const auto STARTUP_ARGUMENTS = ui::app::args();
const auto STARTUP_ENVIRONMENT = ui::app::environment();
constexpr const char *ARGUMENTS[] = {"ui-app-test", "--synthetic"};

void initializeContext() {
  const std::array<ui::app::Environment::Entry, 3> entries{
    {{"PORTALS_SYNTHETIC", "owned"}, {"EMPTY", ""}, {"Mixed", "case"}}};
  const auto environment = ui::app::Environment::fromEntries(entries);

  REQUIRE(environment);
  REQUIRE(ui::app::initialize(2, ARGUMENTS, *environment));
}

struct CountingTerminal : example::ReplayTerminal {
  mutable int touches{};

  std::expected<CellSize, TerminalError> size() const override {
    ++touches;

    return example::ReplayTerminal::size();
  }
};

template <class T>
concept AcceptsOptions = requires(T &&value) { ui::opts(std::forward<T>(value)); };

struct Options {
  bool ansi16{};
};

static_assert(AcceptsOptions<Options &>);
static_assert(AcceptsOptions<const Options &>);
static_assert(!AcceptsOptions<Options>);
static_assert(!AcceptsOptions<const Options>);
static_assert(!std::is_constructible_v<ui::OptionsRef<const Options>, const Options &&>);
static_assert(std::same_as<decltype(ui::opts(std::declval<Options &>()).get()), const Options &>);
} // namespace

TEST_CASE("process context is checked before initialization and copies immutable arguments") {
  REQUIRE_FALSE(STARTUP_ARGUMENTS);
  CHECK(STARTUP_ARGUMENTS.error() == ui::app::ProcessContextError::UNINITIALIZED);
  REQUIRE_FALSE(STARTUP_ENVIRONMENT);
  CHECK(STARTUP_ENVIRONMENT.error() == ui::app::ProcessContextError::UNINITIALIZED);

  initializeContext();

  char executable[] = "copied-program";
  char argument[] = "value";
  const char *values[] = {executable, argument};
  const auto snapshot = ui::app::Arguments::capture(2, values);

  REQUIRE(snapshot);
  executable[0] = 'X';
  argument[0] = 'X';
  CHECK(snapshot->argc() == 2);
  CHECK(snapshot->size() == 2);
  CHECK((*snapshot)[0] == "copied-program");
  CHECK((*snapshot)[1] == "value");
  CHECK(std::string_view(snapshot->argv()[0]) == "copied-program");
  CHECK(snapshot->argv()[2] == nullptr);
  CHECK(std::distance(snapshot->begin(), snapshot->end()) == 2);
  CHECK_THROWS_AS((*snapshot)[2], std::out_of_range);
  CHECK_FALSE(ui::app::Arguments::capture(-1, values));
  CHECK_FALSE(ui::app::Arguments::capture(1, nullptr));

  const auto empty = ui::app::Arguments::capture(0, nullptr);

  REQUIRE(empty);
  CHECK(empty->begin() == empty->end());
  CHECK(empty->argv()[0] == nullptr);
  CHECK(ui::app::args()->argc() == 2);
  CHECK((*ui::app::args())[0] == "ui-app-test");

  const char *contradictory[] = {"different"};
  const auto reinitialized = ui::app::initialize(1, contradictory);

  REQUIRE_FALSE(reinitialized);
  CHECK(reinitialized.error() == ui::app::ProcessContextError::ALREADY_INITIALIZED);
}

TEST_CASE("environment snapshots distinguish missing empty case and support concurrent immutable readers") {
  initializeContext();

  auto environment = ui::app::environment();

  REQUIRE(environment);
  const auto empty = environment->get("EMPTY");
  const auto missing = environment->get("MISSING");

  REQUIRE(empty);
  REQUIRE(*empty);
  CHECK((**empty).empty());
  REQUIRE(missing);
  CHECK_FALSE(*missing);
  const auto emptyName = environment->get("");
  const auto nulName = environment->get(std::string_view("NUL\0NAME", 8));

  REQUIRE_FALSE(emptyName);
  REQUIRE_FALSE(nulName);
  CHECK(emptyName.error() == ui::app::ProcessContextError::INVALID_ARGUMENT);
  CHECK(nulName.error() == ui::app::ProcessContextError::INVALID_ARGUMENT);
#ifdef _WIN32
  const auto caseInsensitive = environment->get("mixed");
  const auto invalidUtf8 = environment->get(std::string_view("\xff", 1));

  REQUIRE(caseInsensitive);
  REQUIRE(*caseInsensitive);
  CHECK(**caseInsensitive == "case");
  REQUIRE_FALSE(invalidUtf8);
  CHECK(invalidUtf8.error() == ui::app::ProcessContextError::INVALID_ARGUMENT);
  const std::array<ui::app::Environment::Entry, 1> unicode{{{"\xc3\x84", "unicode"}}};
  const auto unicodeEnvironment = ui::app::Environment::fromEntries(unicode);

  REQUIRE(unicodeEnvironment);

  const auto unicodeValue = unicodeEnvironment->get("\xc3\xa4");

  REQUIRE(unicodeValue);
  REQUIRE(*unicodeValue);
  CHECK(**unicodeValue == "unicode");
#else
  const auto differentCase = environment->get("mixed");

  REQUIRE(differentCase);
  CHECK_FALSE(*differentCase);
  const std::array<ui::app::Environment::Entry, 1> bytes{{{"NATIVE", std::string(1, '\xff')}}};
  const auto byteEnvironment = ui::app::Environment::fromEntries(bytes);

  REQUIRE(byteEnvironment);

  const auto nativeBytes = byteEnvironment->get("NATIVE");

  REQUIRE(nativeBytes);
  REQUIRE(*nativeBytes);
  CHECK(**nativeBytes == bytes[0].second);
#endif

  const std::array<ui::app::Environment::Entry, 1> replacement{{{"PORTALS_SYNTHETIC", "replacement"}}};
  const auto other = ui::app::Environment::fromEntries(replacement);

  REQUIRE(other);

  const auto reinitialized = ui::app::initialize(2, ARGUMENTS, *other);

  REQUIRE_FALSE(reinitialized);
  CHECK(reinitialized.error() == ui::app::ProcessContextError::ALREADY_INITIALIZED);

  std::atomic<bool> valid{true};
  std::array<std::jthread, 4> readers;

  for (auto &reader : readers) {
    reader = std::jthread([snapshot = *environment, &valid] {
      for (int index = 0; index < 100; ++index) {
        const auto owned = snapshot.get("PORTALS_SYNTHETIC");
        const auto registered = ui::app::env();

        if (!owned || !registered) {
          valid = false;

          return;
        }

        const auto emptyValue = registered->get("EMPTY");

        if (!emptyValue || !*owned || !*emptyValue) {
          valid = false;

          return;
        }

        if (**owned != "owned" || !(**emptyValue).empty()) {
          valid = false;
        }
      }
    });
  }

  for (auto &reader : readers) {
    reader.join();
  }

  CHECK(valid);

  const auto retained = environment->get("PORTALS_SYNTHETIC");

  REQUIRE(retained);
  REQUIRE(*retained);
  CHECK(**retained == "owned");
}

TEST_CASE("help preflight retires recipe and options without touching the terminal") {
  initializeContext();

  CountingTerminal terminal;
  int mounted{}, parsed{}, providers{}, stopped{}, reports{};
  auto scene = ui::scene([&](ManagedScene &) {
    ++mounted;

    return ui::label("unmounted");
  });

  scene.applicationOptions([&] {
    ++providers;

    return ApplicationOptions{};
  });
  scene.onStop([&] {
    ++stopped;
  });
  scene.reportOnError([&](const ui::SceneReport &) {
    ++reports;
  });

  CHECK(scene.runApp(terminal, 2, ARGUMENTS,
                     [&](const ui::app::Arguments &arguments) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
                       ++parsed;
                       CHECK(arguments[0] == "ui-app-test");
                       CHECK_FALSE(terminal.closed);

                       return ui::LaunchAction{ui::LaunchDisposition::FINISH, {}};
                     }) == 0);
  CHECK(scene.runApp(terminal) == 0);
  CHECK(parsed == 1);
  CHECK(mounted == 0);
  CHECK(providers == 0);
  CHECK(stopped == 1);
  CHECK(reports == 0);
  CHECK(terminal.touches == 0);
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("CLI errors and parser exceptions use one typed report before application creation") {
  initializeContext();

  for (const auto code : {ui::CommandLineErrorCode::INVALID_ARGUMENT, ui::CommandLineErrorCode::CALLBACK_FAILED,
                          ui::CommandLineErrorCode::NO_MEMORY}) {
    CountingTerminal terminal;
    int reports{}, observed{};
    auto scene = ui::scene(ui::label("never mounted"));

    scene.onError([&](const ManagedDiagnostic &) {
      ++observed;
    });
    scene.reportOnError([&](const ui::SceneReport &report) {
      ++reports;
      CHECK(report.commandLineFailure);
      REQUIRE(report.commandLineFailure);
      CHECK(report.commandLineFailure->code == code);
      CHECK_FALSE(report.failure);
      CHECK(scene.runApp(terminal) == 1);
      CHECK_FALSE(scene.close());
    });

    CHECK(scene.runApp(terminal, 2, ARGUMENTS,
                       [code](const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
                         if (code == ui::CommandLineErrorCode::CALLBACK_FAILED) {
                           throw std::runtime_error("synthetic parser throw");
                         }

                         if (code == ui::CommandLineErrorCode::NO_MEMORY) {
                           throw std::bad_alloc();
                         }

                         return std::unexpected(ui::CommandLineError{code, "synthetic error"});
                       }) == 2);
    CHECK(scene.runApp(terminal) == 2);
    CHECK(reports == 1);
    CHECK(observed == 0);
    CHECK(terminal.touches == 0);
    CHECK_FALSE(terminal.closed);
  }
}

TEST_CASE("CLI preflight enforces owning UI thread and rejects parser reentry") {
  initializeContext();

  CountingTerminal terminal;
  int parsed{}, reports{}, workerStatus{};
  auto scene = ui::scene(ui::label("lazy"));

  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    CHECK(report.failure);
    CHECK_FALSE(report.commandLineFailure);
  });

  std::jthread worker([&] {
    workerStatus = scene.runApp(terminal, 2, ARGUMENTS, [&](const ui::app::Arguments &) {
      ++parsed;

      return std::expected<ui::LaunchAction, ui::CommandLineError>{};
    });
  });

  worker.join();
  CHECK(workerStatus == 1);
  CHECK(parsed == 0);
  CHECK(reports == 0);
  CHECK(scene.runApp(terminal, 2, ARGUMENTS,
                     [&](const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
                       ++parsed;
                       CHECK(scene.runApp(terminal) == 1);

                       return ui::LaunchAction{};
                     }) == 1);
  CHECK(parsed == 1);
  CHECK(reports == 1);
  CHECK(terminal.touches == 0);
}

TEST_CASE("parsed options are applied before Application create and old runApp still works") {
  initializeContext();

  CountingTerminal terminal;
  ui::SceneOptions options;

  options.clock = terminal.clock();

  Options settings;
  auto borrowed = ui::opts(settings);
  int providers{}, parsed{}, mounted{};
  auto scene = ui::scene(
    [&](ManagedScene &) {
      ++mounted;
      CHECK(parsed == 1);
      CHECK(providers == 1);

      return ui::label("mounted");
    },
    options);

  scene.applicationOptions([&] {
    ++providers;
    CHECK(borrowed.get().ansi16);

    ApplicationOptions result;

    return result;
  });

  CHECK(scene.runApp(terminal, 2, ARGUMENTS,
                     [&](const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
                       ++parsed;
                       settings.ansi16 = true;

                       return ui::LaunchAction{};
                     }) == 0);
  CHECK(parsed == 1);
  CHECK(providers == 1);
  CHECK(mounted == 1);
  CHECK(terminal.closed);

  CountingTerminal legacyTerminal;
  ui::SceneOptions legacyOptions;

  legacyOptions.clock = legacyTerminal.clock();

  auto legacy = ui::scene(ui::label("legacy"), legacyOptions);

  CHECK(legacy.runApp(legacyTerminal) == 0);
  CHECK(legacyTerminal.closed);
}

TEST_CASE("argc run rejects already prepared scenes without parsing or terminal access") {
  initializeContext();

  CountingTerminal terminal;
  auto scene = ui::scene(ui::label("prepared"));
  int parsed{}, reports{};

  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    REQUIRE(std::holds_alternative<TreeError>(report.failure->cause));
    CHECK(std::get<TreeError>(report.failure->cause) == TreeError::WOULD_BLOCK);
  });
  scene.prepare();
  REQUIRE(scene.checked());
  CHECK(scene.runApp(terminal, 2, ARGUMENTS, [&](const ui::app::Arguments &) {
    ++parsed;

    return std::expected<ui::LaunchAction, ui::CommandLineError>{};
  }) == 1);
  CHECK(parsed == 0);
  CHECK(reports == 1);
  CHECK(terminal.touches == 0);
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("application policy callback failures retain exact typed diagnostics before mount") {
  initializeContext();

  for (const auto cause : {TreeError::CALLBACK_FAILED, TreeError::NO_MEMORY}) {
    CountingTerminal terminal;
    int reports{}, mounted{};
    auto scene = ui::scene([&](ManagedScene &) {
      ++mounted;

      return ui::label("never mounted");
    });

    scene.applicationOptions([cause]() -> ApplicationOptions {
      if (cause == TreeError::NO_MEMORY) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("synthetic application policy failure");
    });
    scene.reportOnError([&](const ui::SceneReport &report) {
      ++reports;
      REQUIRE(report.failure);
      REQUIRE(std::holds_alternative<TreeError>(report.failure->cause));
      CHECK(std::get<TreeError>(report.failure->cause) == cause);
      CHECK(report.failure->phase == ManagedPhase::PREPARE);
      REQUIRE(report.failure->operation);
      CHECK(std::string_view(report.failure->operation) == "scene.applicationOptions");
    });

    CHECK(scene.runApp(terminal, 2, ARGUMENTS, {}) == 1);
    CHECK(mounted == 0);
    CHECK(reports == 1);
    CHECK(terminal.touches == 0);
  }
}

TEST_CASE("parser owning captures retire inside lifecycle admission") {
  initializeContext();

  CountingTerminal terminal;
  auto scene = ui::scene(ui::label("never mounted"));
  int retired{}, nestedStatus{}, reports{};

  struct Capture {
    ui::Scene &scene;
    CountingTerminal &terminal;
    int &retired;
    int &status;

    ~Capture() {
      ++retired;
      status = scene.runApp(terminal);
    }
  };

  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    REQUIRE(std::holds_alternative<TreeError>(report.failure->cause));
    CHECK(std::get<TreeError>(report.failure->cause) == TreeError::WOULD_BLOCK);
  });

  ui::CommandLineParser parser =
    [capture = std::unique_ptr<Capture>(new Capture{scene, terminal, retired, nestedStatus})](
      const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
    return ui::LaunchAction{ui::LaunchDisposition::FINISH, {}};
  };

  CHECK(scene.runApp(terminal, 2, ARGUMENTS, std::move(parser)) == 1);
  CHECK(retired == 1);
  CHECK(nestedStatus == 1);
  CHECK(reports == 1);
  CHECK(terminal.touches == 0);
}

TEST_CASE("contradictory process registration fails before parser and terminal") {
  initializeContext();

  CountingTerminal terminal;
  auto scene = ui::scene(ui::label("never mounted"));
  int parsed{}, reports{};
  const char *different[] = {"contradictory"};

  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.commandLineFailure);
    CHECK(report.commandLineFailure->code == ui::CommandLineErrorCode::PROCESS_CONTEXT);
    CHECK(report.commandLineFailure->processContext == ui::app::ProcessContextError::ALREADY_INITIALIZED);
  });

  CHECK(scene.runApp(terminal, 1, different, [&](const ui::app::Arguments &) {
    ++parsed;

    return std::expected<ui::LaunchAction, ui::CommandLineError>{};
  }) == 2);
  CHECK(parsed == 0);
  CHECK(reports == 1);
  CHECK(terminal.touches == 0);
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("CLI failure survives independent parser capture retirement reentry") {
  initializeContext();

  CountingTerminal terminal;
  auto scene = ui::scene(ui::label("never mounted"));
  int retired{}, nestedStatus{}, reports{};

  struct Capture {
    ui::Scene &scene;
    CountingTerminal &terminal;
    int &retired;
    int &status;

    ~Capture() {
      ++retired;
      status = scene.runApp(terminal);
    }
  };

  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.commandLineFailure);
    CHECK(report.commandLineFailure->code == ui::CommandLineErrorCode::INVALID_ARGUMENT);
    CHECK(report.commandLineFailure->message == "original CLI failure");
    REQUIRE(report.failure);
    REQUIRE(std::holds_alternative<TreeError>(report.failure->cause));
    CHECK(std::get<TreeError>(report.failure->cause) == TreeError::WOULD_BLOCK);
  });

  ui::CommandLineParser parser =
    [capture = std::unique_ptr<Capture>(new Capture{scene, terminal, retired, nestedStatus})](
      const ui::app::Arguments &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
    return std::unexpected(ui::CommandLineError{ui::CommandLineErrorCode::INVALID_ARGUMENT, "original CLI failure"});
  };

  CHECK(scene.runApp(terminal, 2, ARGUMENTS, std::move(parser)) == 2);
  CHECK(scene.runApp(terminal) == 2);
  CHECK(retired == 1);
  CHECK(nestedStatus == 1);
  CHECK(reports == 1);
  CHECK(terminal.touches == 0);
  CHECK_FALSE(terminal.closed);
}
