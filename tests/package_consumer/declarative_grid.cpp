// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_scene_resource.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace portals;

template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("Installed declarative Grid operation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("Installed declarative Grid contract failed");
  }
}
}

int main() try {
  example::ReplayTerminal terminal({}, 50, {30, 6});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Label> value;
  int observed{};
  auto scene = ui::scene(ui::grid(ui::label("Name"), ui::label(value, "Ada")).columns(2).columnGap(2), options)
    | ui::onLayout(value, [&](const NodeGeometry &geometry) {
        ++observed;
        require(geometry.local.origin() == CellPoint{6, 0});
      });

  const auto native = checked(scene.run(terminal));

  require(native.frames == 1 && observed == 1 && terminal.closed);
  require(!value.geometry());

  example::ReplayTerminal loadedTerminal({}, 50, {30, 6});
  options.clock = loadedTerminal.clock();
  auto loaded = ui::scene(R"(
    Grid { columns=2 columnGap=2 scroll=true
      header { sticky=true Label#heading { "Name" } Label { "Role" } }
      body { Label { "Ada" } Label#role { "Programmer" } }
    })", {}, options);
  auto role = loaded.get<Label>("role");
  int loadedObserved{};

  loaded | ui::onLayout(role, [&](const NodeGeometry &geometry) {
    ++loadedObserved;
    require(geometry.local.origin().x == 6);
    require(geometry.local.origin().y == 1);
  });
  const auto resource = checked(loaded.run(loadedTerminal));

  require(resource.frames == 1 && loadedObserved == 1 && loadedTerminal.closed);

  int effects{};
  auto invalid = ui::scene(ui::grid(
    ui::label("first").row(0).column(0), ui::label("second").row(0).column(0)).columns(1)
      .setup([&](ManagedScene &, Node<Grid>) -> std::expected<void, ManagedError> {
        ++effects;
        return {};
      }));
  const auto rejected = invalid.prepareChecked();

  require(!rejected && effects == 0);
  const auto *cause = std::get_if<TreeError>(&rejected.error().cause);
  require(cause && *cause == TreeError::INVALID_ARGUMENT);
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
