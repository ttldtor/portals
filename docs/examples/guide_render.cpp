#include <portals/render.hpp>

#include <array>

int main() {
  using namespace portals;

  const CellSize viewport{20, 2};
  auto text = GraphemeText::from("Score: 10");
  auto clip = CellRect::from({0, 0}, viewport);

  if (!text || !clip) {
    return 1;
  }

  const std::array<RenderCommand, 1> commands{
    TextCommand{{1, 0}, *text, *clip}};
  auto frame = renderFullFrame(viewport, commands);

  if (!frame) {
    return 1;
  }

  // The owning frame is ready for the host's presentation code.
  return 0;
}
