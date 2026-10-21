// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace portals;

constexpr std::string_view SOURCE = R"(
definitions { Edit = LineEdit { history { maxOperations=2 } } }
defaults { LineEdit { history { groupTyping=false } } }
scene { VBox {
  Edit#first { text="" }
  Edit#second { text="", history { maxOperations=1 } }
  ThreeState#flag { text="Selection", state=MIXED }
} }
)";

struct SourceFile {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("portals-control-defaults-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");

  ~SourceFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

bool verify(ui::Scene scene) {
  auto first = scene.get<LineEdit>("first");
  auto second = scene.get<LineEdit>("second");
  auto flag = scene.get<ThreeState>("flag");
  Application *application{};

  scene.configure([&](ManagedScene &, Application &app) {
    application = &app;
  });

  if (!scene.prepareChecked() || !application) {
    return false;
  }

  for (const auto value : {"a", "b"}) {
    if (!(first.text() = value).result() || !(second.text() = value).result()) {
      return false;
    }
  }

  if (!first.undo().result() || !first.undo().result() || !second.undo().result()) {
    return false;
  }

  const auto firstValue = first.text().get();
  const auto secondValue = second.text().get();

  if (!firstValue || *firstValue != "" || !secondValue || *secondValue != "a" || first.canUndo() || second.canUndo() ||
      !first.canRedo()) {
    return false;
  }

  for (const auto state : {CheckState::UNCHECKED, CheckState::CHECKED, CheckState::MIXED}) {
    if (!application->router().invoke(flag.node(), {Action::ACTIVATE})) {
      return false;
    }

    bool matches{};
    const auto inspected = application->tree().read(flag.node(), [&](const ThreeState &widget) {
      matches = widget.checkState() == state;
    });

    if (!inspected || !matches) {
      return false;
    }
  }

  return scene.close().has_value() && !first.text().get() && !flag.text().get();
}
} // namespace

int main() {
  using namespace portals;

  ui::ControlContext controls;
  controls.defaults("LineEdit", {.history = {.groupTyping = false}})
    .define("Edit", "LineEdit", {.history = {.maxOperations = 2}});

  if (!verify(
        ui::scene(ui::vbox(ui::lineEdit("").variant("Edit").id("first"),
                           ui::lineEdit("").variant("Edit").settings({.history = {.maxOperations = 1}}).id("second"),
                           ui::threeState("Selection", CheckState::MIXED).id("flag")),
                  {.controls = controls}))) {
    return 1;
  }

  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;

  if (!verify(ui::scene(SOURCE, options))) {
    return 2;
  }

  SourceFile source;
  {
    std::ofstream file(source.path, std::ios::binary);
    file << SOURCE;

    if (!file) {
      return 3;
    }
  }

  if (!verify(ui::scene(ui::SceneResourceFile{source.path}, options))) {
    return 4;
  }

  return 0;
}
