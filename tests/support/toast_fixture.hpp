// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/ui.hpp>

#include <iostream>
#include <source_location>
#include <stdexcept>

namespace toastTest {
using namespace portals;

inline void check(bool value, const char *message, std::source_location at = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error(std::string(message) + " at " + at.file_name() + ":" + std::to_string(at.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location at = std::source_location::current()) {
  if (!result) {
    if constexpr (std::same_as<E, ManagedError>) {
      if (const auto *cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << "phase=" << int(result.error().phase) << " cause=" << int(*cause) << '\n';
      }
    } else if constexpr (std::same_as<E, TreeError>) {
      std::cerr << "tree cause=" << int(result.error()) << '\n';
    }
  }
  check(bool(result), "unexpected checked failure", at);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

inline void take(ui::DialogCommandResult result, std::source_location at = std::source_location::current()) {
  take(result.result(), at);
}

inline bool treeCause(const ManagedError &failure, TreeError expected) {
  const auto value = std::get_if<TreeError>(&failure.cause);
  return value && *value == expected;
}

// Test-only local-to-surface sink. Commands are rendered while the original frame borrow is live.
class Sink : public PaintContext {
public:
  Sink(NodeGeometry geometry, std::vector<RenderCommand> &commands) : geometry_(geometry), commands_(commands) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      std::visit(
        [&](const auto &value) {
          const auto clip = take(value.clip.translated(geometry_.surface.origin())).intersection(geometry_.clip);
          if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, TextCommand>) {
            commands_.emplace_back(TextCommand{take(translated(value.origin, geometry_.surface.origin())), value.text,
                                               clip, value.style, value.z});
          } else {
            commands_.emplace_back(
              FillCommand{take(value.area.translated(geometry_.surface.origin())), clip, value.style, value.z});
          }
        },
        command);
    }
    return {};
  }

private:
  NodeGeometry geometry_;
  std::vector<RenderCommand> &commands_;
};

inline void paint(WidgetTree &tree, Node<Widget> node, std::vector<RenderCommand> &commands) {
  const auto snapshot = take(tree.inspect(node));
  if (!snapshot.visible || !snapshot.geometry.valid) {
    return;
  }
  Sink sink(snapshot.geometry, commands);
  take(tree.paint(node, sink));
  for (const auto child : snapshot.children) {
    paint(tree, child, commands);
  }
}

struct Fixture {
  SchedulerTime now{};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;
  ui::NotificationCenter center;
  ui::Ref<Overlay> wrapper;

  explicit Fixture(ApplicationOptions options = {}) {
    app = take(Application::create(options, {[](void *context) noexcept {
                                               return *static_cast<SchedulerTime *>(context);
                                             },
                                             &now}));
    scene = take(ManagedScene::create(*app));
  }

  void mount(NotificationOptions options = {}) {
    take(ui::mount(*scene, ui::notifications(center, ui::label("Body"), options).reference(wrapper)));
  }

  void frame(CellSize size = {50, 12}, bool complete = true,
             std::source_location at = std::source_location::current()) {
    take(scene->prepare(*app, size));
    const auto bounds = take(CellRect::from({}, size));
    bool converged{};
    constexpr int MAX_PASSES = 3;
    for (int pass = 0; pass != MAX_PASSES; ++pass) {
      (void)take(app->tree().consumeFrameRequest());
      std::optional<LayoutSnapshot> snapshot;
      take(app->tree().frame([&] {
        take(layoutTree(app->tree(), app->root(), bounds, bounds));
        snapshot.emplace(take(scene->usesGeometrySnapshot()
                                ? LayoutSnapshot::capture(app->tree(), app->root(), LayoutSnapshotProfile::GEOMETRY)
                                : LayoutSnapshot::capture(app->tree(), app->root())));
      }));
      (void)take(app->tree().consumeFrameRequest());
      take(scene->prepareViews(*app, *snapshot), at);
      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        converged = true;
        break;
      }
    }
    check(converged, "notification layout uses the existing three-pass bound");
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      std::vector<RenderCommand> commands(std::initializer_list<RenderCommand>{});
      paint(app->tree(), app->root(), commands);
      (void)take(renderFullFrame(size, commands, app->widthPolicy()));
    }));
    (void)take(app->tree().consumeFrameRequest());
    if (complete) {
      take(scene->completedLayout(*app));
    }
  }

  SchedulerDispatch advance(SchedulerTime value) {
    now = value;
    const auto dispatched = take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
    return dispatched;
  }

  std::shared_ptr<const NotificationSnapshot> snapshot() {
    return take(center.snapshot());
  }
};
} // namespace toastTest
