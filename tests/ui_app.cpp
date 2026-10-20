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

struct ParserCapture {
  bool &retired;

  ~ParserCapture() {
    retired = true;
  }
};

struct Options {
  bool ansi16{};
};

struct OwningSceneFactory {
  ui::Scene operator()() const;
};

struct CheckedSceneFactory {
  std::expected<ui::Scene, ManagedError> operator()() const;
};

struct BorrowedSceneFactory {
  ui::Scene &operator()() const;
};

struct WrongErrorSceneFactory {
  std::expected<ui::Scene, TreeError> operator()() const;
};

template <class Factory>
concept AppFactoryEntry = requires(Factory &&factory) { ui::runApp(std::forward<Factory>(factory)); };

template <class Factory>
concept AppFactoryPipe = requires(Factory &&factory) { std::forward<Factory>(factory) | ui::runApp(); };

static_assert(AppFactoryEntry<OwningSceneFactory>);
static_assert(AppFactoryEntry<CheckedSceneFactory>);
static_assert(AppFactoryPipe<OwningSceneFactory>);
static_assert(AppFactoryPipe<CheckedSceneFactory>);
static_assert(!AppFactoryEntry<BorrowedSceneFactory>);
static_assert(!AppFactoryPipe<BorrowedSceneFactory>);
static_assert(!AppFactoryEntry<WrongErrorSceneFactory>);
static_assert(!AppFactoryPipe<WrongErrorSceneFactory>);

using LegacyRunStage = decltype(ui::runApp({}));
using LegacyOriginRunStage = decltype(ui::runApp({}, std::source_location::current()));
using LegacyCommandLineStage = decltype(ui::runApp(0, nullptr, ui::CommandLineParser{}, {}));

static_assert(std::same_as<LegacyRunStage, decltype(ui::runApp())>);
static_assert(std::same_as<LegacyOriginRunStage, LegacyRunStage>);
static_assert(std::same_as<LegacyRunStage, decltype(ui::runApp(ui::RunAppOptions{}))>);
static_assert(std::same_as<LegacyCommandLineStage, decltype(ui::runApp(0, nullptr, ui::CommandLineParser{}))>);
static_assert(
  std::same_as<LegacyCommandLineStage, decltype(ui::runApp(0, nullptr, ui::CommandLineParser{}, ui::RunAppOptions{}))>);
static_assert(
  std::same_as<decltype(ui::runApp(std::declval<OwningSceneFactory>(), std::declval<const ui::RunAppOptions &>())),
               int>);

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

TEST_CASE("owning Scene factories use one lifetime and preserve their policies in function and pipe forms") {
  for (const bool pipe : {false, true}) {
    for (const bool checked : {false, true}) {
      CountingTerminal terminal;
      ui::Ref<Label> label;
      ui::SceneOptions sceneOptions;
      ui::RunAppOptions options;
      int factories{}, configured{}, stopped{};

      sceneOptions.clock = terminal.clock();
      options.terminal = &terminal;
      options.scene.maxConfigurations = 0; // An owning Scene keeps the policies chosen by its factory.

      auto factory = [&, capture = std::make_unique<int>(42)]() mutable -> std::expected<ui::Scene, ManagedError> {
        ++factories;

        auto owner = ui::scene(ui::label(label, "owned"), sceneOptions);
        auto reference = owner.ref();

        owner.configure([&, reference](ManagedScene &) {
          ++configured;
          CHECK(label.text().get());
          CHECK(reference.quit());
          ui::quit();
        });
        owner.onStop([&, capture = std::move(capture)] {
          ++stopped;
          CHECK(*capture == 42);
          CHECK(label.text().get());
        });

        return owner;
      };
      auto launch = [&](auto &&create) {
        return pipe ? (std::forward<decltype(create)>(create) | ui::runApp(options))
                    : ui::runApp(std::forward<decltype(create)>(create), options);
      };
      const int status = checked ? launch(factory) : launch([&] {
        return std::move(*factory());
      });

      CHECK(status == 0);
      CHECK(factories == 1);
      CHECK(configured == 1);
      CHECK(stopped == 1);
      CHECK_FALSE(label.text().get());
    }
  }
}

TEST_CASE("factory CLI help and typed parser failures retire captures without invoking a factory or terminal") {
  initializeContext();

  for (const bool pipe : {false, true}) {
    for (const int outcome : {0, 1, 2, 3}) {
      CountingTerminal terminal;
      ui::RunAppOptions options;
      int factories{}, parsed{}, reports{};
      bool retired{};

      options.terminal = &terminal;
      options.report = [&](const ui::SceneReport &report) {
        ++reports;
        CHECK(retired);
        REQUIRE(report.commandLineFailure);
        CHECK_FALSE(report.failure);
        const auto expected = outcome == 1   ? ui::CommandLineErrorCode::INVALID_ARGUMENT
                              : outcome == 2 ? ui::CommandLineErrorCode::CALLBACK_FAILED
                                             : ui::CommandLineErrorCode::NO_MEMORY;

        CHECK(report.commandLineFailure->code == expected);
      };

      auto factory = [&]() -> ui::Scene {
        ++factories;

        return ui::scene(ui::label("unreachable"));
      };
      ui::CommandLineParser parser = [&, capture = std::unique_ptr<ParserCapture>(new ParserCapture{retired})](
                                       const auto &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
        ++parsed;
        CHECK_FALSE(retired);

        if (outcome == 0) {
          return ui::LaunchAction{ui::LaunchDisposition::FINISH, "factory help\n"};
        }

        if (outcome == 1) {
          return std::unexpected(ui::CommandLineError{ui::CommandLineErrorCode::INVALID_ARGUMENT, "invalid option"});
        }

        if (outcome == 2) {
          throw std::runtime_error("parser failed");
        }

        throw std::bad_alloc();
      };
      const int status = pipe ? (factory | ui::runApp(2, ARGUMENTS, std::move(parser), options))
                              : ui::runApp(factory, 2, ARGUMENTS, std::move(parser), options);

      CHECK(status == (outcome == 0 ? 0 : 2));
      CHECK(factories == 0);
      CHECK(parsed == 1);
      CHECK(retired);
      CHECK(reports == (outcome == 0 ? 0 : 1));
      CHECK(terminal.touches == 0);
      CHECK_FALSE(terminal.closed);
    }
  }
}

TEST_CASE("factory CLI continuation precedes owning Scene construction and retains Description compatibility") {
  initializeContext();

  for (const bool pipe : {false, true}) {
    for (const bool description : {false, true}) {
      CountingTerminal terminal;
      ui::RunAppOptions options;
      int parsed{}, factories{}, mounted{};
      bool retired{};

      options.terminal = &terminal;
      options.scene.clock = terminal.clock();
      ui::CommandLineParser parser = [&, capture =
                                           std::unique_ptr<ParserCapture>(new ParserCapture{retired})](const auto &) {
        ++parsed;

        return std::expected<ui::LaunchAction, ui::CommandLineError>{};
      };
      auto launch = [&](auto &&factory) {
        return pipe ? (std::forward<decltype(factory)>(factory) | ui::runApp(2, ARGUMENTS, std::move(parser), options))
                    : ui::runApp(std::forward<decltype(factory)>(factory), 2, ARGUMENTS, std::move(parser), options);
      };
      auto recipe = [&] {
        ++factories;
        CHECK(parsed == 1);
        CHECK(retired);

        return ui::label("after CLI");
      };
      const int status = description ? launch(recipe) : launch([&] {
        auto owner = ui::scene(recipe(), options.scene);

        owner.configure([&](ManagedScene &) {
          ++mounted;
          ui::quit();
        });

        return owner;
      });

      CHECK(status == 0);
      CHECK(parsed == 1);
      CHECK(factories == 1);
      CHECK(mounted == (description ? 0 : 1));
      CHECK(terminal.closed);
    }
  }
}

TEST_CASE("owning Scene factory typed failures and exceptions report once after local owners retire") {
  for (const bool pipe : {false, true}) {
    for (const int ending : {0, 1, 2}) {
      CountingTerminal terminal;
      ui::Ref<Label> label;
      ui::RunAppOptions options;
      int factories{}, stopped{}, reports{};
      const auto origin = std::source_location::current();

      options.terminal = &terminal;
      options.report = [&](const ui::SceneReport &report) {
        ++reports;
        CHECK(stopped == 1);
        CHECK_FALSE(label.text().get());
        REQUIRE(report.failure);
        CHECK_FALSE(report.commandLineFailure);
        REQUIRE(std::holds_alternative<TreeError>(report.failure->cause));
        CHECK(std::get<TreeError>(report.failure->cause) == (ending == 0   ? TreeError::NODE_LIMIT
                                                             : ending == 1 ? TreeError::NO_MEMORY
                                                                           : TreeError::CALLBACK_FAILED));
        CHECK(report.failure->phase == (ending == 0 ? ManagedPhase::STATE : ManagedPhase::MOUNT));
        CHECK(std::string_view(report.failure->execution) == "scene.factory");

        if (ending == 0) {
          CHECK(report.failure->cleanup == TreeError::INVALID_ARGUMENT);
          CHECK(std::string_view(report.failure->operation) == "test.factory");
          CHECK(report.origin.line() == origin.line());
        }
      };
      auto factory = [&]() -> std::expected<ui::Scene, ManagedError> {
        ++factories;

        auto owner = ui::scene(ui::label(label, "partial"));

        owner.onStop([&] {
          ++stopped;
        });
        REQUIRE(owner.prepareChecked());

        if (ending == 1) {
          throw std::bad_alloc();
        }

        if (ending == 2) {
          throw std::runtime_error("factory failed");
        }

        ManagedError failure{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, TreeError::INVALID_ARGUMENT};

        failure.operation = "test.factory";
        failure.operationOrigin = origin;

        return std::unexpected(failure);
      };
      const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

      CHECK(status == 1);
      CHECK(factories == 1);
      CHECK(stopped == 1);
      CHECK(reports == 1);
      CHECK(terminal.touches == 0);
      CHECK_FALSE(terminal.closed);
    }
  }
}

TEST_CASE("owning Scene reporter precedence preserves runtime restoration and independent cleanup errors") {
  struct FailingTerminal : CountingTerminal {
    std::expected<CellSize, TerminalError> size() const override {
      ++touches;

      return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 7});
    }

    std::expected<void, TerminalError> close() noexcept override {
      (void)CountingTerminal::close();

      return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 9});
    }
  };

  for (const bool upper : {false, true}) {
    for (const bool pipe : {false, true}) {
      FailingTerminal terminal;
      ui::RunAppOptions options;
      ui::Ref<Label> label;
      int factoryReports{}, upperReports{}, stopped{};
      std::optional<ui::SceneReport> delivered;

      options.terminal = &terminal;
      auto report = [&](const ui::SceneReport &value) {
        CHECK(terminal.closed);
        CHECK(stopped == 1);
        CHECK_FALSE(label.text().get());
        REQUIRE(value.failure);
        REQUIRE(value.failure->application);
        REQUIRE(value.failure->application->restoration);
        CHECK(value.failure->application->restoration->native_code == 9);
        CHECK(value.failure->cleanup == TreeError::INVALID_ARGUMENT);
        delivered = value;
      };

      if (upper) {
        options.report = [&](const ui::SceneReport &value) {
          ++upperReports;
          report(value);

          throw std::runtime_error("upper reporter failed");
        };
      }

      auto factory = [&] {
        ui::SceneOptions sceneOptions;

        sceneOptions.clock = terminal.clock();

        auto owner = ui::scene(ui::label(label, "mounted"), sceneOptions);

        owner.reportOnError([&](const ui::SceneReport &value) {
          ++factoryReports;
          report(value);
        });
        owner.onStop([&]() -> std::expected<void, ManagedError> {
          ++stopped;

          return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
        });

        return owner;
      };
      const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

      CHECK(status == 1);
      CHECK(upperReports == (upper ? 1 : 0));
      CHECK(factoryReports == (upper ? 0 : 1));
      CHECK(delivered);
    }
  }
}

TEST_CASE("upper factory reporter stays pinned through self replacement and rejects lifecycle reentry") {
  CountingTerminal terminal;
  ui::RunAppOptions options;
  ui::Scene *stationary{};
  int reports{}, stopped{};
  auto capture = std::make_shared<int>(42);
  auto weak = std::weak_ptr(capture);

  options.terminal = &terminal;
  options.report = [&, capture](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    CHECK(stopped == 1);
    options.report = {};
    CHECK_FALSE(weak.expired());
    CHECK(*capture == 42);
    REQUIRE(stationary);
    CHECK(stationary->runApp(terminal) == 1);
    const auto closed = stationary->close();

    REQUIRE_FALSE(closed);
    CHECK(std::get<TreeError>(closed.error().cause) == TreeError::WOULD_BLOCK);
  };
  capture.reset();

  auto factory = [&] {
    auto owner = ui::scene(ui::label("failure"));

    owner.onStop([&] {
      ++stopped;
    });

    return owner;
  };
  auto setup = [&](ui::Scene &owner) -> std::expected<void, ManagedError> {
    stationary = &owner;

    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
  };
  // The setup overload exposes the stationary facade only within this synchronous test run.
  const int status = ui::runApp(factory, setup, options);

  CHECK(status == 1);
  CHECK(reports == 1);
  CHECK(stopped == 1);
  CHECK(weak.expired());
}

TEST_CASE("abandoned cold Scene cleanup survives factory exceptions and checked errors") {
  for (const bool pipe : {false, true}) {
    for (const int ending : {0, 1, 2, 3}) {
      CountingTerminal terminal;
      ui::RunAppOptions options;
      int factories{}, stopped{}, reports{};

      options.terminal = &terminal;
      options.report = [&](const ui::SceneReport &report) {
        ++reports;
        CHECK(stopped == 2);
        REQUIRE(report.failure);
        REQUIRE(std::holds_alternative<TreeError>(report.failure->cause));
        CHECK(std::get<TreeError>(report.failure->cause) == (ending == 0   ? TreeError::CALLBACK_FAILED
                                                             : ending == 1 ? TreeError::NO_MEMORY
                                                                           : TreeError::WRONG_THREAD));
        CHECK(report.failure->cleanup == (ending == 3 ? TreeError::WOULD_BLOCK : TreeError::INVALID_ARGUMENT));
        CHECK(report.droppedErrors == (ending == 3 ? 2 : 1));
      };
      auto factory = [&]() -> std::expected<ui::Scene, ManagedError> {
        ++factories;

        auto first = ui::scene(ui::label("first local"));
        auto last = ui::scene(ui::label("last local"));

        first.onStop([&]() -> std::expected<void, ManagedError> {
          ++stopped;

          return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::NODE_LIMIT, {}, {}});
        });
        last.onStop([&]() -> std::expected<void, ManagedError> {
          ++stopped;

          return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
        });

        if (ending == 0) {
          throw std::runtime_error("factory failed after creating cold owners");
        }

        if (ending == 1) {
          throw std::bad_alloc();
        }

        return std::unexpected(ManagedError{ManagedPhase::STATE,
                                            {},
                                            TreeError::WRONG_THREAD,
                                            {},
                                            ending == 3 ? std::optional{TreeError::WOULD_BLOCK} : std::nullopt});
      };
      const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

      CHECK(status == 1);
      CHECK(factories == 1);
      CHECK(stopped == 2);
      CHECK(reports == 1);
      CHECK(terminal.touches == 0);
      CHECK_FALSE(terminal.closed);
    }
  }
}

TEST_CASE("successful factories fail before terminal acquisition when abandoned Scene cleanup failed") {
  for (const bool pipe : {false, true}) {
    CountingTerminal terminal;
    ui::RunAppOptions options;
    int stopped{}, configured{}, reports{};

    options.terminal = &terminal;
    options.report = [&](const ui::SceneReport &report) {
      ++reports;
      CHECK(stopped == 3);
      REQUIRE(report.failure);
      CHECK(report.failure->phase == ManagedPhase::CLEANUP);
      CHECK(std::get<TreeError>(report.failure->cause) == TreeError::INVALID_ARGUMENT);
      CHECK(report.failure->cleanup == TreeError::NODE_LIMIT);
      CHECK(report.droppedErrors == 1);
    };
    auto factory = [&] {
      auto first = ui::scene(ui::label("first abandoned"));
      auto last = ui::scene(ui::label("last abandoned"));
      auto returned = ui::scene(ui::label("returned owner"));

      first.onStop([&]() -> std::expected<void, ManagedError> {
        ++stopped;

        return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::WRONG_THREAD, {}, {}});
      });
      last.onStop([&]() -> std::expected<void, ManagedError> {
        ++stopped;

        return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
      });
      returned.onStop([&]() -> std::expected<void, ManagedError> {
        ++stopped;

        return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::NODE_LIMIT, {}, {}});
      });
      returned.configure([&](ManagedScene &) {
        ++configured;
      });

      return returned;
    };
    const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

    CHECK(status == 1);
    CHECK(reports == 1);
    CHECK(configured == 0);
    CHECK(terminal.touches == 0);
    CHECK_FALSE(terminal.closed);
  }
}

TEST_CASE("nested runner parser reporter and setup cleanup cannot enter an outer factory collector") {
  initializeContext();

  CountingTerminal terminal;
  CountingTerminal nestedTerminal;
  ui::RunAppOptions options;
  ui::RunAppOptions nestedOptions;
  int stopped{}, reports{}, nestedReports{};

  options.terminal = &terminal;
  nestedOptions.terminal = &nestedTerminal;
  nestedOptions.scene.clock = nestedTerminal.clock();
  auto temporary = [&] {
    auto owner = ui::scene(ui::label("independent temporary"));

    owner.onStop([&]() -> std::expected<void, ManagedError> {
      ++stopped;

      return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    });

    return owner;
  };

  nestedOptions.report = [&](const ui::SceneReport &report) {
    ++nestedReports;
    REQUIRE(report.commandLineFailure);
    auto local = temporary();
  };
  options.report = [&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    CHECK(std::get<TreeError>(report.failure->cause) == TreeError::CALLBACK_FAILED);
    CHECK_FALSE(report.failure->cleanup);
    CHECK(report.droppedErrors == 0);
    CHECK(stopped == 3);
  };

  auto factory = [&]() -> ui::Scene {
    CHECK(ui::runApp(
            [] {
              return ui::label("unreachable");
            },
            2, ARGUMENTS,
            [&](const auto &) -> std::expected<ui::LaunchAction, ui::CommandLineError> {
              auto local = temporary();

              return std::unexpected(ui::CommandLineError{ui::CommandLineErrorCode::INVALID_ARGUMENT, {}});
            },
            nestedOptions) == 2);
    CHECK(ui::runApp(
            [] {
              return ui::label("nested");
            },
            [&](ui::Scene &) {
              auto local = temporary();
            },
            nestedOptions) == 0);

    throw std::runtime_error("outer factory failure");
  };

  CHECK(ui::runApp(factory, options) == 1);
  CHECK(reports == 1);
  CHECK(nestedReports == 1);
  CHECK(stopped == 3);
  CHECK(terminal.touches == 0);
}

TEST_CASE("factory Scene results admit prepared owners and reject completed closed and moved-from facades") {
  for (const bool pipe : {false, true}) {
    for (const int state : {0, 1, 2, 3, 4}) {
      CountingTerminal terminal;
      ui::RunAppOptions options;
      int configured{}, stopped{}, reports{}, touchesAtReturn{};

      options.terminal = &terminal;
      options.report = [&](const ui::SceneReport &report) {
        ++reports;
        CHECK(stopped == 1);
        REQUIRE(report.failure);
        CHECK(std::get<TreeError>(report.failure->cause) == (state == 4 ? TreeError::NODE_LIMIT : TreeError::CLOSED));
      };
      auto factory = [&] {
        ui::SceneOptions sceneOptions;

        sceneOptions.clock = terminal.clock();

        auto owner = ui::scene(ui::label("returned state"), sceneOptions);

        owner.onStop([&] {
          ++stopped;
        });
        owner.configure([&](ManagedScene &) -> std::expected<void, ManagedError> {
          ++configured;

          if (state == 4) {
            return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::NODE_LIMIT, {}, {}});
          }

          return {};
        });

        if (state == 0) {
          REQUIRE(owner.prepareChecked());
        } else if (state == 1) {
          CHECK(owner.close());
        } else if (state == 2 || state == 4) {
          CHECK(bool(owner.run(terminal)) == (state == 2));
        } else {
          auto moved = std::move(owner);
        }

        touchesAtReturn = terminal.touches;

        return owner;
      };
      const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

      CHECK(status == (state == 0 ? 0 : 1));
      CHECK(reports == (state == 0 ? 0 : 1));
      CHECK(stopped == 1);
      CHECK(configured == (state == 1 || state == 3 ? 0 : 1));

      if (state == 0) {
        CHECK(terminal.touches > 0);
      } else {
        CHECK(terminal.touches == touchesAtReturn);
      }
    }
  }
}

TEST_CASE("factory cleanup merges with an existing cold Scene failure and counts equal-valued close errors") {
  for (const bool pipe : {false, true}) {
    for (const bool failedClose : {false, true}) {
      CountingTerminal terminal;
      ui::RunAppOptions options;
      int stopped{}, reports{}, configured{};

      options.terminal = &terminal;
      options.report = [&](const ui::SceneReport &report) {
        ++reports;
        REQUIRE(report.failure);
        CHECK(report.failure->phase == ManagedPhase::BIND);
        CHECK(std::get<TreeError>(report.failure->cause) == TreeError::NODE_LIMIT);
        CHECK(report.failure->cleanup == TreeError::INVALID_ARGUMENT);
        CHECK(report.droppedErrors == (failedClose ? 1 : 0));
        CHECK(stopped == 2);
      };
      auto factory = [&] {
        ui::SceneOptions sceneOptions;

        sceneOptions.maxConfigurations = 1;

        auto sibling = ui::scene(ui::label("abandoned sibling"));
        auto owner = ui::scene(ui::label("prefailed result"), sceneOptions);
        auto stop = [&]() -> std::expected<void, ManagedError> {
          ++stopped;

          return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
        };

        sibling.onStop(stop);
        owner.onStop([&]() -> std::expected<void, ManagedError> {
          ++stopped;

          if (failedClose) {
            return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
          }

          return {};
        });
        owner.configure([&](ManagedScene &) {
          ++configured;
        });
        owner.configure([](ManagedScene &) {
        });

        REQUIRE_FALSE(owner.checked());

        return owner;
      };
      const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

      CHECK(status == 1);
      CHECK(reports == 1);
      CHECK(stopped == 2);
      CHECK(configured == 0);
      CHECK(terminal.touches == 0);
    }
  }
}

TEST_CASE("factory rejection retains the returned Scene sink unless an upper sink explicitly overrides it") {
  for (const bool pipe : {false, true}) {
    for (const bool previousRun : {false, true}) {
      for (const bool upper : {false, true}) {
        CountingTerminal terminal;
        ui::RunAppOptions options;
        int factoryReports{}, upperReports{}, stopped{}, touchesAtReturn{};

        options.terminal = &terminal;
        auto report = [&](const ui::SceneReport &value) {
          REQUIRE(value.failure);
          CHECK(std::get<TreeError>(value.failure->cause) == TreeError::CLOSED);
          CHECK(stopped == 1);
          CHECK(terminal.touches == touchesAtReturn);
        };

        if (upper) {
          options.report = [&](const ui::SceneReport &value) {
            ++upperReports;
            report(value);
          };
        }

        auto factory = [&] {
          ui::SceneOptions sceneOptions;

          sceneOptions.clock = terminal.clock();

          auto owner = ui::scene(ui::label("retired result"), sceneOptions);

          owner.onStop([&] {
            ++stopped;
          });
          owner.reportOnError([&](const ui::SceneReport &value) {
            ++factoryReports;
            report(value);
          });

          if (previousRun) {
            CHECK(owner.run(terminal));
          } else {
            CHECK(owner.close());
          }

          touchesAtReturn = terminal.touches;

          return owner;
        };
        const int status = pipe ? (factory | ui::runApp(options)) : ui::runApp(factory, options);

        CHECK(status == 1);
        CHECK(factoryReports == (upper ? 0 : 1));
        CHECK(upperReports == (upper ? 1 : 0));
        CHECK(stopped == 1);
        CHECK(terminal.touches == touchesAtReturn);
      }
    }
  }
}

TEST_CASE("abandoned complete cleanup error keeps its occupied slot when returned owner close also fails") {
  CountingTerminal terminal;
  ui::RunAppOptions options;
  int stopped{}, reports{};
  const auto origin = std::source_location::current();

  options.terminal = &terminal;
  options.report = [&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    CHECK(report.failure->phase == ManagedPhase::CLEANUP);
    CHECK(std::get<TreeError>(report.failure->cause) == TreeError::NODE_LIMIT);
    CHECK(report.failure->cleanup == TreeError::WOULD_BLOCK);
    CHECK(report.failure->operationOrigin->line() == origin.line());
    CHECK(std::string_view(report.failure->operation) == "test.stop");
    CHECK(report.droppedErrors == 1);
    CHECK(stopped == 2);
  };
  auto factory = [&] {
    auto abandoned = ui::scene(ui::label("abandoned"));
    auto owner = ui::scene(ui::label("returned"));

    abandoned.onStop([&]() -> std::expected<void, ManagedError> {
      ++stopped;

      ManagedError failure{ManagedPhase::CLEANUP, {}, TreeError::NODE_LIMIT, {}, TreeError::WOULD_BLOCK};

      failure.operation = "test.stop";
      failure.operationOrigin = origin;

      return std::unexpected(failure);
    });
    owner.onStop([&]() -> std::expected<void, ManagedError> {
      ++stopped;

      return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    });

    return owner;
  };

  CHECK(ui::runApp(factory, options) == 1);
  CHECK(reports == 1);
  CHECK(stopped == 2);
  CHECK(terminal.touches == 0);
}
