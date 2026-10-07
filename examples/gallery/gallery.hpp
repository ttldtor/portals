// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/controls.hpp>

#include <optional>
#include <stdexcept>

/** @brief Finite control-gallery fixture shared by the example and semantic replay tests. */
namespace gallery {
using namespace portals;

/** @brief Unwrap an owning expected result or stop this finite example with an exception.
 * @tparam T Success type accepted by std::expected: void, or a movable value returned by ownership transfer.
 * @tparam E Error type accepted by std::expected; the helper does not inspect or format its value.
 * @param result Owning expected passed by value; argument copying/moving occurs before helper entry.
 * @return The success value moved out of result, or no value when T is void.
 * @throws std::runtime_error If result is unexpected; its original error detail is not included.
 * @throws std::bad_alloc If allocating the diagnostic or moving T requires unavailable storage.
 * Other exceptions from T's move construction or argument construction also propagate.
 */
template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("gallery operation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

/** @brief Create owning gallery text with the default width policy.
 * @param value Borrowed UTF-8 input; the returned text owns its decoded content.
 * @return Validated text using the default WidthPolicy.
 * @throws std::runtime_error If text validation fails.
 * @throws std::bad_alloc If text or diagnostic storage cannot be allocated.
 */
inline GraphemeText text(std::string_view value) {
  return take(GraphemeText::from(value));
}

// Translate local control commands into the same surface geometry used by hit-testing.
// The destination vector borrows widget text and is consumed by the renderer inside tree.frame.
class SurfaceSink : public PaintContext {
public:
  SurfaceSink(NodeGeometry geometry, std::vector<RenderCommand> &commands) : geometry_(geometry), commands_(commands) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      auto status = std::visit(
        [&](const auto &value) -> std::expected<void, TreeError> {
          // Apply the node transform, then intersect with the layout-owned ancestor clip.
          auto clip = value.clip.translated(geometry_.surface.origin());

          if (!clip) {
            return std::unexpected(TreeError::INVALID_GEOMETRY);
          }

          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            auto origin = translated(value.origin, geometry_.surface.origin());

            if (!origin) {
              return std::unexpected(TreeError::INVALID_GEOMETRY);
            }

            commands_.emplace_back(
              TextCommand{*origin, value.text, clip->intersection(geometry_.clip), value.style, value.z});
          } else {
            auto area = value.area.translated(geometry_.surface.origin());

            if (!area) {
              return std::unexpected(TreeError::INVALID_GEOMETRY);
            }

            commands_.emplace_back(FillCommand{*area, clip->intersection(geometry_.clip), value.style, value.z});
          }

          return {};
        },
        command);

      if (!status) {
        return status;
      }
    }

    return {};
  }

private:
  NodeGeometry geometry_;
  std::vector<RenderCommand> &commands_;
};

struct Scene {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  Node<Column> column = take(tree->emplace<Column>(root));
  Node<Label> title = take(tree->emplace<Label>(column, text("Portals controls")));
  Node<Label> label = take(tree->emplace<Label>(column, text("Unicode: \xe7\x95\x8c e\xcc\x81")));
  Node<Button> button = take(tree->emplace<Button>(column, text("Run")));
  Node<CheckBox> checkbox = take(tree->emplace<CheckBox>(column, text("Alerts")));
  Node<Column> disabled_group = take(tree->emplace<Column>(column));
  Node<Label> disabled_label = take(tree->emplace<Label>(disabled_group, text("Disabled label")));
  Node<Button> disabled_button = take(tree->emplace<Button>(disabled_group, text("Disabled button")));
  Node<CheckBox> disabled_checkbox = take(tree->emplace<CheckBox>(disabled_group, text("Disabled check")));
  Node<Label> status = take(tree->emplace<Label>(column, text("Initial focus: Run")));
  std::vector<Node<Control>> controls{
    title, label, button, checkbox, disabled_label, disabled_button, disabled_checkbox, status};
  std::shared_ptr<Property<bool>> model = take(Property<bool>::create(*tree, root, false, Invalidation::PAINT));
  int calls{};
  int changes{};
  int disabled_calls{};
  ActionBinding button_binding = take(bindButton(*router, button, [&] {
    ++calls;
  }));
  ActionBinding disabled_binding = take(bindButton(*router, disabled_button, [&] {
    ++disabled_calls;
  }));
  CheckBoxBinding check_binding = take(bindCheckbox(*tree, *router, checkbox, model, [&](CheckBoxResult result) {
    take(std::move(result));
    ++changes;
  }));
  CheckBoxBinding disabled_check_binding =
    take(bindCheckbox(*tree, *router, disabled_checkbox, model, [&](CheckBoxResult result) {
      take(std::move(result));
      ++disabled_calls;
    }));
  InputDecoder decoder = take(InputDecoder::create());

  Scene() {
    take(tree->setEnabled(disabled_group, false));
  }

  DispatchResult input(std::string_view bytes) {
    DispatchResult result;

    do {
      auto decoded = take(decoder.next(bytes, 0));

      bytes.remove_prefix(decoded.consumed);

      if (decoded.event) {
        result = take(router->dispatch(*decoded.event));
      }

      if (bytes.empty() && !decoded.event) {
        break;
      }
    } while (true);

    return result;
  }

  CellBuffer render(CellSize size = {30, 9}) {
    // Apply model and focus presentation before entering the immutable render frame.
    take(check_binding.sync());
    take(disabled_check_binding.sync());

    const auto focused = take(router->focused());

    for (const auto control : controls) {
      take(syncControl(*tree, control, focused));
    }

    const auto viewport = take(CellRect::from({}, size));
    std::optional<CellBuffer> buffer;

    take(tree->frame([&] {
      take(layoutTree(*tree, root, viewport, viewport));

      std::vector<RenderCommand> commands;

      for (const auto control : controls) {
        const auto snapshot = take(tree->inspect(control));

        if (!snapshot.geometry.valid) {
          continue;
        }

        SurfaceSink sink(snapshot.geometry, commands);

        take(tree->paint(control, sink));
      }

      // Finish consuming every borrowed command while the frame still retains the widgets.
      buffer.emplace(take(renderFullFrame(size, commands)));
    }));

    return std::move(*buffer);
  }

  void stage(std::string_view name) {
    if (name == "initial") {
      return;
    }

    input("\r\t ");
    take(setControlText(*tree, status, text("Keyboard: Run + Alerts")));

    if (name == "keyboard") {
      return;
    }

    auto keyboard_frame = render();

    input("\x1b[<0;2;3M\x1b[<0;2;3m");
    take(setControlText(*tree, status, text("Mouse: Run pressed once")));

    if (name == "mouse") {
      return;
    }

    take(tree->setEnabled(button, false));
    take(setControlText(*tree, status, text("Run disabled; focus: Alerts")));
  }
};
} // namespace gallery
