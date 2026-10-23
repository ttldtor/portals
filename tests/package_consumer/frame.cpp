// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/frame.hpp>
#include <portals/ui.hpp>

#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace portals;

constexpr BorderOptions LEGACY_BORDER{PrimitiveGlyphs::ASCII, 512};
constexpr ScrollAreaOptions LEGACY_SCROLL{.frame = LEGACY_BORDER};
static_assert(LEGACY_BORDER.maxPaintCells == 512 && LEGACY_SCROLL.frame->maxPaintCells == 512);
static_assert(std::is_aggregate_v<BorderOptions> && std::is_aggregate_v<ScrollAreaOptions>);
static_assert(FramePattern::PLAIN == FramePattern::SINGLE);

template <class T, class E>
T checked(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("Installed frame operation failed at line " + std::to_string(origin.line()));
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("Installed frame contract failed");
  }
}
} // namespace

int main() try {
  using namespace portals;
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  const auto draft = checked(scene->state(std::string("draft")));
  ui::Ref<Button> save;
  ui::Ref<LineEdit> editor;
  ui::Ref<Border> saveFrame, editorFrame;
  int clicks{};
  auto recipe = ui::vbox(
    ui::button(save, "Save").width(10).framed(saveFrame, "rounded", "Actions").height(4).onClick([&] {
      ++clicks;
    }),
    ui::lineEdit(draft).reference(editor).width(14) | ui::framed(editorFrame, "ascii", "Draft") | ui::height(4));

  checked(ui::mount(*scene, std::move(recipe)));
  checked(scene->prepare(*app, {24, 10}));
  const auto viewport = checked(CellRect::from({}, {24, 10}));

  checked(app->tree().frame([&] {
    checked(layoutTree(app->tree(), app->root(), viewport, viewport));
  }));
  checked(scene->completedLayout(*app));
  require(checked(saveFrame.geometry()).local.size() == CellSize{10, 4});
  require(checked(editorFrame.geometry()).local.size() == CellSize{14, 4});
  require(checked(save.geometry()).local.origin() == CellPoint{1, 1});
  require(checked(save.geometry()).local.size() == CellSize{8, 1});
  require(checked(app->tree().inspect(save.node())).parent == saveFrame.node());
  require(checked(app->tree().inspect(editor.node())).parent == editorFrame.node());
  checked(app->router().invoke(save.node(), {Action::ACTIVATE}));
  require(clicks == 1);
  auto edit = editor.text() = "updated";

  checked(edit.result());
  require(checked(editor.text().get()) == "updated");
  const auto saveNode = save.node();
  const auto saveFrameNode = saveFrame.node();

  checked(scene->remove(saveFrameNode));
  require(!app->tree().inspect(saveNode) && !app->tree().inspect(saveFrameNode));
  checked(scene->close());
  auto scrollApp = checked(Application::create());
  auto scrollScene = checked(ManagedScene::create(*scrollApp));
  ui::Ref<ScrollArea> area;

  checked(ui::mount(*scrollScene,
                    ui::vbox(ui::scrollArea(area, ui::label("content").width(30).height(20)) |
                             ui::framed("rounded", "Sections") | ui::scrollOnFrame() | ui::width(16) | ui::height(6))));
  checked(scrollScene->prepare(*scrollApp, {24, 16}));
  const auto scrollViewport = checked(CellRect::from({}, {24, 16}));

  checked(scrollApp->tree().frame([&] {
    checked(layoutTree(scrollApp->tree(), scrollApp->root(), scrollViewport, scrollViewport));
  }));
  checked(scrollScene->completedLayout(*scrollApp));
  const auto scrollLayout = checked(scrollAreaLayout(scrollApp->tree(), area.node()));

  require(checked(area.geometry()).local.size() == CellSize{16, 6});
  require(scrollLayout.placement.viewport == CellSize{14, 4});
  require(scrollLayout.placement.viewportOrigin == CellPoint{1, 1});
  require(scrollLayout.placement.horizontalBar && scrollLayout.placement.verticalBar);
  const auto missingFrame = ui::mount(*scrollScene, ui::scrollArea(ui::label("content")).scrollOnFrame());

  require(!missingFrame && std::get<TreeError>(missingFrame.error().cause) == TreeError::INVALID_ARGUMENT);
  checked(scrollScene->close());

  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';

  return 1;
}
