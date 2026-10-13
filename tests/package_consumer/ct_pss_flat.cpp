// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "ct_pss_support.hpp"
#include <portals/style_pss.hpp>

using namespace ctPssConsumer;

// DSL slice: numeric-capacity descriptors, with rules stored as data instead of a type pack.
inline constexpr auto APPLICATION = sc::compactSheet(
  std::array{
    sc::flatRule(sc::all(), sc::fg(sc::rgb(210, 215, 225))),
    sc::flatRule(sc::type<"Label">(), sc::italic(true)),
    sc::flatRule(sc::id<"action">(), sc::bold(true), sc::minWidth(std::int32_t{6})),
    sc::flatRule(sc::selector<"", "action">(StyleState::FOCUSED), sc::bg(sc::rgb(40, 60, 80)), sc::underline(true)),
  },
  "application.typed");
inline constexpr auto USER = sc::compactSheet(
  std::array{
    sc::flatRule(sc::id<"action">(), sc::fg(sc::rgb(255, 210, 70))),
    sc::flatRule(sc::id<"action">(), sc::bold(false)),
    sc::flatRule(sc::type<"Label">(), sc::italic(false), sc::underline(true)),
  },
  "user.typed");

int main(int argc, char **argv) try {
  auto application = sc::materializeDocument(APPLICATION);
  auto user = sc::materializeDocument(USER);

  if (!application || !user) {
    const auto &error = application ? user.error() : application.error();

    std::cerr << "PSS setup code=" << static_cast<int>(error.code) << " offset=" << error.location.offset << '\n';

    return 1;
  }

  return styleLayersConsumer::run(argc, argv, initialize, true, &application->sheet, &user->sheet);
} catch (const std::bad_alloc &) {
  std::cerr << "Stylesheet setup ran out of memory\n";

  return 1;
}
