// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui_lyra.hpp>

#include <stdexcept>
#include <vector>

namespace {
namespace ui = portals::ui;

ui::app::Arguments arguments(std::initializer_list<const char *> values) {
  const std::vector<const char *> argv(values);
  auto snapshot = ui::app::Arguments::capture(static_cast<int>(argv.size()), argv.data());

  REQUIRE(snapshot.has_value());

  return *snapshot;
}

struct Options {
  int count{12};
  std::string theme{"modern"};
  bool ansi16{};
};

::lyra::cli declaration(Options &options) {
  return ::lyra::cli() | ::lyra::opt(options.count, "count")["-n"]["--count"]("Row count.") |
         ::lyra::opt(options.theme, "theme")["-t"]["--theme"]("Palette.").choices("modern", "classic") |
         ::lyra::opt(options.ansi16)["-a"]["--ansi16"]("Use ANSI colors.");
}
} // namespace

TEST_CASE("Lyra adapter owns a lazy parser declaration and preserves typed defaults") {
  Options options;
  auto parser = ui::lyra::cli(declaration(options));

  CHECK(options.count == 12);
  CHECK(options.theme == "modern");
  CHECK_FALSE(options.ansi16);

  const auto result = parser(arguments({"sample"}));

  REQUIRE(result.has_value());
  CHECK(result->disposition == ui::LaunchDisposition::CONTINUE);
  CHECK(result->output.empty());
  CHECK(options.count == 12);
  CHECK(options.theme == "modern");
  CHECK_FALSE(options.ansi16);
}

TEST_CASE("Lyra adapter accepts native short and long declarations") {
  for (const auto &input : {arguments({"sample", "-n", "7", "-t", "classic", "-a"}),
                            arguments({"sample", "--count", "7", "--theme", "classic", "--ansi16"})}) {
    Options options;
    auto parser = ui::lyra::cli(declaration(options));
    const auto result = parser(input);

    REQUIRE(result.has_value());
    CHECK(result->disposition == ui::LaunchDisposition::CONTINUE);
    CHECK(options.count == 7);
    CHECK(options.theme == "classic");
    CHECK(options.ansi16);
  }
}

TEST_CASE("Lyra diagnostics are typed and owned after parser destruction") {
  for (const auto &input : {arguments({"sample", "--conut", "1"}), arguments({"sample", "--count"}),
                            arguments({"sample", "--count", "many"}), arguments({"sample", "--theme", "typo"})}) {
    const auto error = [&] {
      Options options;
      auto parser = ui::lyra::cli(declaration(options));
      auto result = parser(input);

      REQUIRE_FALSE(result.has_value());

      return result.error();
    }();

    CHECK(error.code == ui::CommandLineErrorCode::INVALID_ARGUMENT);
    CHECK_FALSE(error.message.empty());
  }
}

TEST_CASE("Lyra built-in help yields owning output and a successful early exit") {
  for (const auto spelling : {"-?", "-h", "--help"}) {
    Options options;
    auto parser = ui::lyra::cli(declaration(options));
    const auto result = parser(arguments({"sample", spelling}));

    REQUIRE(result.has_value());
    CHECK(result->disposition == ui::LaunchDisposition::FINISH);
    CHECK(result->output.find("sample") != std::string::npos);
    CHECK(result->output.find("--count") != std::string::npos);
    CHECK(result->output.find("--theme") != std::string::npos);
    CHECK(options.count == 12);
  }
}

TEST_CASE("Lyra version is opt-in and uses native short circuit behavior") {
  Options options;
  auto parser = ui::lyra::cli(declaration(options), {.version = "sample 1.0"});
  const auto result = parser(arguments({"sample", "--version", "--unknown"}));

  REQUIRE(result.has_value());
  CHECK(result->disposition == ui::LaunchDisposition::FINISH);
  CHECK(result->output == "sample 1.0\n");

  auto noVersion = ui::lyra::cli(declaration(options));

  CHECK_FALSE(noVersion(arguments({"sample", "--version"})).has_value());
}

TEST_CASE("Lyra custom help uses an explicit borrowed observer") {
  bool help{};
  auto parser = ui::lyra::cli(::lyra::cli() | ::lyra::help(help), {.automaticHelp = false, .helpRequested = &help});
  const auto result = parser(arguments({"sample", "--help"}));

  REQUIRE(result.has_value());
  CHECK(help);
  CHECK(result->disposition == ui::LaunchDisposition::FINISH);
  INFO(result->output);
  CHECK(result->output.find("USAGE:") != std::string::npos);
  CHECK(result->output.find("sample") != std::string::npos);
  CHECK(result->output.find("--help") != std::string::npos);

  auto noHelp = ui::lyra::cli(::lyra::cli(), {.automaticHelp = false});

  CHECK_FALSE(noHelp(arguments({"sample", "--help"})).has_value());
}

TEST_CASE("Lyra callback exceptions reach the generic launch boundary unchanged") {
  auto native = ::lyra::cli() | ::lyra::opt([](bool) -> ::lyra::parser_result {
                  throw std::runtime_error("callback failed");
                })["--fail"];
  auto parser = ui::lyra::cli(native);

  CHECK_THROWS_WITH_AS(static_cast<void>(parser(arguments({"sample", "--fail"}))), "callback failed",
                       std::runtime_error);
}

TEST_CASE("Lyra retains upstream partial update behavior on a later error") {
  Options options;
  auto parser = ui::lyra::cli(declaration(options));
  const auto result = parser(arguments({"sample", "--count", "9", "--unknown"}));

  CHECK_FALSE(result.has_value());
  CHECK(options.count == 9);
}

TEST_CASE("Lyra accepts an empty standalone argument snapshot") {
  auto parser = ui::lyra::cli(::lyra::cli());
  const auto result = parser(arguments({}));

  REQUIRE(result.has_value());
  CHECK(result->disposition == ui::LaunchDisposition::CONTINUE);
}
