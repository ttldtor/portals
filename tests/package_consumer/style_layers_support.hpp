// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "static_styles_support.hpp"
#include <portals/style_static.hpp>

namespace styleLayersConsumer {
using namespace portals;
namespace sc = style::ct;
inline constexpr auto THEME =
  sc::sheet(sc::parameters(sc::parameter<"background">(RgbColor{24, 28, 36}), sc::parameter<"width">(std::int32_t{2})),
            sc::rule(sc::selector(), sc::background(sc::token<"background">())),
            sc::rule(sc::selector<"", "action">(), sc::minWidth(sc::token<"width">())));

inline StyleSheet applicationSheet() {
  return {{{{.id = "action"}, {.bold = true}}}};
}

inline StyleSheet userSheet() {
  return {{{{.id = "action"}, {.foreground = RgbColor{255, 210, 70}}}}};
}

// Both complete consumers include this helper and the replay terminal, including errors and cleanup.
/** @tparam Factory Callable attaching a checked controller to a stable managed owner.
 * @param argc Number of command-line arguments, including the executable name.
 * @param argv Arguments accepting only the optional intentional --failure scenario.
 * @param initialize Controller factory invoked after mount; errors retain managed cleanup.
 * @param layered Whether to enable the existing application/user layer cascade.
 * @param application Optional override borrowed for this synchronous run; null uses the original sheet.
 * @param user Optional override borrowed for this synchronous run; null uses the original sheet.
 * @return Zero when replay, typed failure and lifetime checks match the selected scenario.
 */
template <class Factory>
int run(int argc, char **argv, Factory initialize, bool layered = false, const StyleSheet *application = nullptr,
        const StyleSheet *user = nullptr) {
  const bool failure = argc == 2 && std::string_view(argv[1]) == "--failure";

  if (argc > 2 || (argc == 2 && !failure)) {
    std::cerr << "Usage: style layers consumer [--failure]\n";

    return 1;
  }

  example::ReplayTerminal terminal({{50, "\r", {}}, {100, "\r", {}}, {150, "\r", CellSize{38, 8}}}, 200, {40, 10});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  using Controller = typename decltype(initialize(std::declval<ManagedScene &>()))::value_type;
  std::optional<Controller> controller;
  std::optional<ManagedError> actionError;
  ui::Ref<Button> button;
  std::size_t changes{};
  auto action = [&]() -> std::expected<void, TreeError> {
    auto result = changes == 0   ? controller->switchTheme()
                  : changes == 1 ? controller->attachLive()
                                 : controller->width(12);

    if (!result) {
      actionError = result.error();

      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    ++changes;

    return {};
  };
  auto scene = ui::scene(ui::column({.gap = 1, .padding = {1, 1, 1, 1}}, ui::label("Style layers"),
                                    ui::button("Change").id("action").reference(button).onClick(action)),
                         options);
  if (layered) {
    scene.styleLayers()
      .styleLayer(StyleTier::APPLICATION, "application", application ? *application : applicationSheet())
      .styleLayer(StyleTier::USER, "user", user ? *user : userSheet());
  }

  scene.configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
    auto created = initialize(owner);

    if (!created) {
      return std::unexpected(created.error());
    }

    controller = std::move(*created);

    if (failure) {
      const auto invalid = controller->width(-1);

      return invalid ? std::expected<void, ManagedError>{} : std::unexpected(invalid.error());
    }

    return {};
  });
  const auto result = scene.run(terminal);

  if (failure) {
    const auto *cause = result ? nullptr : std::get_if<StyleError>(&result.error().cause);

    if (!cause || cause->cause != TreeError::INVALID_ARGUMENT || result.error().phase != ManagedPhase::STYLE ||
        button.text().get()) {
      std::cerr << "Typed style failure or cleanup differed\n";

      return 1;
    }

    return 0;
  }

  if (!result || actionError) {
    staticStylesConsumer::report(actionError ? *actionError : result.error());

    return 1;
  }

  if (!terminal.closed || changes != 3 || button.text().get()) {
    std::cerr << "Theme update or lifetime differed\n";

    return 1;
  }

  std::cout << "frames=" << result->frames << " changes=" << changes << '\n' << terminal.output;

  return 0;
}
} // namespace styleLayersConsumer
