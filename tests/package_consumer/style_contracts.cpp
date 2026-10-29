// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_scene_resource.hpp>
#include <portals/ui_style.hpp>

#include <stdexcept>

namespace {
void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("installed style contract failed");
  }
}

template <class T> void requireWidth(portals::ui::Scene &scene, portals::ui::Ref<T> reference) {
  bool visited = false;

  scene.configure([&](portals::ManagedScene &, portals::Application &app) {
    require(app.tree()
              .read(reference.node(),
                    [&](const portals::Widget &widget) {
                      require(static_cast<bool>(widget.computedStyle()));
                      require(widget.computedStyle()->minWidth == 12);
                      require(widget.computedStyle()->normal.bold);
                      visited = true;
                    })
              .has_value());
  });
  require(scene.checked().has_value());
  require(visited);
}
} // namespace

int main() {
  using namespace portals;

  auto resource = ui::scene(ui::resource(R"(VBox{
    Label#caption{"Text"} Button#save{"Save"}
    CheckBox#choice{"Yes"} LineEdit#message{"initial"}
  })"));
  const auto caption = resource.get<Label>("caption");
  const auto save = resource.get<Button>("save");
  const auto choice = resource.get<CheckBox>("choice");
  const auto message = resource.get<LineEdit>("message");

  resource | ui::style("Control { bold:true; } Label#caption { min-width:12; } "
                       "Button#save { min-width:12; } CheckBox#choice { min-width:12; } "
                       "LineEdit#message { min-width:12; }");
  require(resource.prepareChecked().has_value());
  requireWidth(resource, caption);
  requireWidth(resource, save);
  requireWidth(resource, choice);
  requireWidth(resource, message);

  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> output;
  auto scene =
    ui::scene(ui::vbox(ui::lineEdit(input, "one two"),
                       ui::virtualList(ui::words(input.draftText())).reference(output).id("tokens").height(2)));
  const auto named = scene.get<VirtualList<TextSegment>>("tokens");

  scene | ui::style("List#tokens { min-width:12; bold:true; }");
  require(scene.prepareChecked().has_value());
  require(named.node() == output.node());
  requireWidth(scene, output);
}
