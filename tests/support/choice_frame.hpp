// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui.hpp>

#include <stdexcept>

// Internal headless renderer used by portals_combo_box_example_tests and combo-box behavior tests.
// Input: prepared application/managed scene and finite viewport. Output: owning CellBuffer/text;
// any checked tree/layout/render failure throws into the surrounding doctest case.
namespace choice_test {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  if (!value) {
    throw std::runtime_error("choice frame operation failed");
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

inline void pump(Application &app, ManagedScene &scene, BackgroundExecutor &executor, CellSize size) {
  take(app.scheduler().dispatch());
  executor.service();
  take(scene.afterDispatch(app));
  take(scene.prepare(app, size));
  const auto bounds = take(CellRect::from({}, size));
  for (unsigned pass = 0; pass < 4; ++pass) {
    take(app.tree().consumeFrameRequest());
    std::optional<LayoutSnapshot> layout;
    take(app.tree().frame([&] {
      take(layoutTree(app.tree(), app.root(), bounds, bounds));
      layout.emplace(take(LayoutSnapshot::capture(app.tree(), app.root())));
    }));
    take(scene.prepareViews(app, *layout));
    if (take(app.tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      break;
    }
  }
  take(scene.completedLayout(app));
  executor.service();
}

class Surface final : public PaintContext {
public:
  Surface(NodeGeometry geometry, std::vector<RenderCommand> &commands) : geometry_(geometry), commands_(commands) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      std::visit(
        [&](const auto &value) {
          auto output = value;
          output.clip = take(value.clip.translated(geometry_.surface.origin())).intersection(geometry_.clip);
          if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, TextCommand>) {
            output.origin = take(translated(value.origin, geometry_.surface.origin()));
          } else {
            output.area = take(value.area.translated(geometry_.surface.origin()));
          }
          commands_.emplace_back(output);
        },
        command);
    }
    return {};
  }

private:
  NodeGeometry geometry_;
  std::vector<RenderCommand> &commands_;
};

inline CellBuffer paint(Application &app, CellSize size) {
  std::optional<CellBuffer> output;
  take(app.tree().frame([&] {
    std::vector<RenderCommand> commands;
    std::vector<Node<Widget>> pending{app.root()};
    while (!pending.empty()) {
      const auto node = pending.back();
      pending.pop_back();
      const auto snapshot = take(app.tree().inspect(node));
      if (!snapshot.visible || !snapshot.geometry.valid) {
        continue;
      }
      Surface surface(snapshot.geometry, commands);
      take(app.tree().paint(node, surface));
      pending.insert(pending.end(), snapshot.children.rbegin(), snapshot.children.rend());
    }
    output.emplace(take(renderFullFrame(size, commands, app.widthPolicy())));
  }));
  return std::move(*output);
}

inline std::string text(const CellBuffer &buffer) {
  std::string output;
  for (std::int32_t y = 0; y < buffer.size().height; ++y) {
    for (std::int32_t x = 0; x < buffer.size().width; ++x) {
      auto cell = take(buffer.at({x, y}));
      if (cell.cell.kind != CellKind::CONTINUATION) {
        output.append(cell.glyph);
      }
    }
    output.push_back('\n');
  }
  return output;
}
} // namespace choice_test
