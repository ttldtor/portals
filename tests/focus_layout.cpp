// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/test_terminal.hpp"

#include <portals/action.hpp>
#include <portals/layout.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace portals;
int failures{};

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    std::cerr << "integration fixture failed\n";
    std::abort();
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

CellRect rectangle(int x, int y, int width, int height) {
  return take(CellRect::from({x, y}, {width, height}));
}

SizePolicy fixed_height() {
  SizePolicy policy;

  policy.height = {1, 1, 1, SizeMode::fixed};

  return policy;
}

SizePolicy fixed_label_width() {
  auto policy = fixed_height();

  policy.width = {4, 4, 4, SizeMode::fixed};

  return policy;
}

struct Label : SizedWidget {
  GraphemeText text;
  CellSize desired;
  CellRect bounds;
  int activations{};
  int presses{};

  Label(std::string_view value, SizePolicy policy)
      : SizedWidget(policy), text(take(GraphemeText::from(value))), desired{static_cast<int>(value.size()), 1} {
  }

  std::expected<CellSize, TreeError> measure(CellSize) override {
    return desired;
  }

  std::expected<void, TreeError> arrange(CellRect local) override {
    bounds = take(CellRect::from({}, local.size()));

    return {};
  }

  std::expected<void, TreeError> paint(PaintContext &context) override {
    const RenderCommand command = TextCommand{{}, text, bounds};

    return context.submit(std::span(&command, 1));
  }
};

struct SurfaceSink : PaintContext {
  NodeGeometry geometry;
  std::vector<RenderCommand> &commands;

  SurfaceSink(NodeGeometry arranged, std::vector<RenderCommand> &scene) : geometry(arranged), commands(scene) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    for (const auto &value : values) {
      const auto &text = std::get<TextCommand>(value);
      const auto origin = take(translated(text.origin, geometry.surface.origin()));
      const auto clip = take(text.clip.translated(geometry.surface.origin()));

      commands.emplace_back(TextCommand{origin, text.text, clip.intersection(geometry.clip), text.style, text.z});
    }

    return {};
  }
};

EventReply receive(Label &label, const RoutedEvent &event) {
  if (const auto *action = std::get_if<ActionEvent>(&event); action && action->action == Action::activate) {
    ++label.activations;

    return EventReply::handled;
  }

  if (const auto *mouse = std::get_if<MouseInputEvent>(&event);
      mouse && mouse->button == MouseButton::left && mouse->action == MouseAction::press) {
    ++label.presses;

    return EventReply::handled;
  }

  return EventReply::ignored;
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  test_support::FakeClock clock;
  test_support::TestTerminal terminal{clock, {12, 5}};
  FrameWriter writer{terminal};

  void draw(CellSize size, std::span<const Node<Label>> labels) {
    const auto viewport = take(CellRect::from({}, size));
    std::optional<CellBuffer> rendered;

    terminal.resize(size);
    take(tree->invalidate(root, Invalidation::layout));
    take(tree->frame([&] {
      take(layout_tree(*tree, root, viewport, viewport));

      std::vector<RenderCommand> commands;

      for (const auto node : labels) {
        const auto geometry = take(tree->inspect(node)).geometry;

        if (!geometry.valid) {
          continue;
        }

        SurfaceSink sink(geometry, commands);

        take(tree->paint(node, sink));
      }

      // Every borrowed label survives through rendering in this same tree frame.
      rendered.emplace(take(render_full_frame(size, commands)));
    }));

    take(writer.submit(std::move(*rendered)));

    // The output owner flushes only after the layout/render frame has ended.
    const auto output = writer.flush(TerminalDeadline::max());

    check(output.status == TerminalStatus::progress && !output.error && !writer.pending(),
          "real renderer/writer completes the headless terminal frame");
  }

  DispatchResult input(std::string_view bytes) {
    const auto events = terminal.input(bytes);

    check(events.size() == 1, "integration input produces one decoded event");

    return take(router->dispatch(events.at(0)));
  }

  void expect_rows(std::span<const std::string_view> expected) {
    const auto snapshot = terminal.snapshot();

    check(expected.size() == static_cast<std::size_t>(snapshot.size.height), "snapshot row count matches viewport");

    for (int y = 0; y < snapshot.size.height; ++y) {
      std::string actual;

      for (int x = 0; x < snapshot.size.width; ++x) {
        const auto index = static_cast<std::size_t>(y * snapshot.size.width + x);

        actual += snapshot.cells.at(index).glyph;
      }

      check(actual == expected[static_cast<std::size_t>(y)], "independent terminal snapshot matches expected layout");
    }
  }

  void expect_painted_hits(Node<Label> first, Node<Label> second, Node<Label> footer) {
    const auto snapshot = terminal.snapshot();

    for (int y = 0; y < snapshot.size.height; ++y) {
      for (int x = 0; x < snapshot.size.width; ++x) {
        const auto index = static_cast<std::size_t>(y * snapshot.size.width + x);
        const auto &glyph = snapshot.cells.at(index).glyph;
        Node<Widget> expected;

        if (glyph == "A") {
          expected = first;
        } else if (glyph == "B") {
          expected = second;
        } else if (glyph == "F") {
          expected = footer;
        } else {
          continue;
        }

        check(take(router->hit_test({x, y})) == expected, "each painted label cell hits its actual layout node");
      }
    }
  }
};

void layout_render_input() {
  Fixture f;
  LinearOptions column_options;
  LinearOptions row_options;

  column_options.gap = 1;
  column_options.padding = {1, 1, 1, 1};
  row_options.gap = 1;

  const auto column = take(f.tree->emplace<Column>(f.root, column_options));
  const auto row = take(f.tree->emplace<Row>(column, row_options, fixed_height()));
  const auto first = take(f.tree->emplace<Label>(row, "AAAA", fixed_label_width()));
  const auto second = take(f.tree->emplace<Label>(row, "BBBB", fixed_label_width()));
  const auto footer = take(f.tree->emplace<Label>(column, "FFFFFFFFFF", fixed_height()));
  const std::array labels{first, second, footer};
  const std::array bindings{take(f.router->bind(first, receive)), take(f.router->bind(second, receive)),
                            take(f.router->bind(footer, receive))};
  constexpr std::array<std::string_view, 5> wide_rows{"            ", " AAAA BBBB  ", "            ", " FFFFFFFFFF ",
                                                      "            "};

  f.draw({12, 5}, labels);
  f.expect_rows(wide_rows);
  f.expect_painted_hits(first, second, footer);

  const auto original_second = take(f.tree->inspect(second)).geometry;

  check(take(f.tree->inspect(row)).geometry.surface == rectangle(1, 1, 10, 1), "Column places the real nested Row");
  check(original_second.surface == rectangle(6, 1, 4, 1), "Row places second fixed child after its gap");
  check(original_second.clip == original_second.surface, "wide viewport exposes the complete second child");
  check(f.input("\x1b[<0;7;2M").handler == second.id(), "decoded SGR press hits the arranged second child");
  check(take(f.router->focused()) == second, "mouse focus uses actual Row geometry");
  check(f.input("\r").handler == second.id(), "keyboard activation follows mouse focus");
  check(take(f.router->invoke(second, {Action::activate})).handler == second.id(),
        "programmatic activation enters the same typed handler");
  take(f.tree->read(second, [](const Label &label) {
    check(label.activations == 2 && label.presses == 1, "mouse, keyboard and programmatic events are delivered once");
  }));
  take(f.tree->invalidate(f.root, Invalidation::layout));
  check(f.input("\x1b[<0;7;2M").status == DispatchStatus::no_target, "resize invalidation rejects old hit rectangles");
  check(f.input("\r").handler == second.id(), "keyboard remains accessible before resized geometry is ready");

  constexpr std::array<std::string_view, 5> narrow_rows{"        ", " AAAA B ", "        ", " FFFFFF ", "        "};

  f.draw({8, 5}, labels);
  f.expect_rows(narrow_rows);
  f.expect_painted_hits(first, second, footer);

  const auto clipped_second = take(f.tree->inspect(second)).geometry;
  const auto padding_hit = f.input("\x1b[<0;8;2M");

  check(clipped_second.surface == original_second.surface, "fixed width survives viewport shortage");
  check(clipped_second.clip == rectangle(6, 1, 1, 1), "real Row clip leaves one visible child cell");
  check(original_second.clip == rectangle(6, 1, 4, 1), "saved geometry remains an owning pre-resize snapshot");
  check(padding_hit.target == column.id() && padding_hit.status == DispatchStatus::unhandled,
        "clipped child cannot receive mouse from adjacent Column padding");
  take(f.tree->read(second, [](const Label &label) {
    check(label.presses == 1, "clipped-away cells never invoke the child");
  }));
  check(f.input("\x1b[<0;7;2M").handler == second.id(), "the remaining painted child cell stays clickable");
  take(f.router->set_focus(first));
  take(f.tree->set_visible(first, false));
  check(take(f.router->focused()) == second, "hidden focused child returns focus to its live predecessor in history");
  check(f.input("\x1b[<0;2;2M").status == DispatchStatus::no_target, "hidden reflow requires fresh mouse geometry");

  constexpr std::array<std::string_view, 5> hidden_rows{"        ", " BBBB   ", "        ", " FFFFFF ", "        "};

  f.draw({8, 5}, labels);
  f.expect_rows(hidden_rows);
  f.expect_painted_hits(first, second, footer);

  const auto hidden_first = take(f.tree->inspect(first));
  const auto moved_second = take(f.tree->inspect(second)).geometry;

  check(!hidden_first.geometry.valid, "layout does not retain hittable geometry for hidden targets");
  check(moved_second.surface == rectangle(1, 1, 4, 1), "real Row removes hidden child and its gap");
  check(f.input("\x1b[<0;2;2M").handler == second.id(), "old first-child cell now routes to the reflowed second child");
  check(f.input("\t").target == footer.id(), "Tab skips hidden child and reaches the next Column item");
  check(f.input("\x1b[Z").target == second.id(), "Shift+Tab returns through actual nested layout order");
  check(take(f.router->invoke(first, {Action::activate})).status == DispatchStatus::no_target,
        "hidden explicit target is ineligible after reflow");
  take(f.tree->read(first, [](const Label &label) {
    check(label.activations == 0 && label.presses == 0, "hidden original target never receives rerouted input");
  }));
}

void bubbling_rechecks_ancestor_state() {
  for (const bool hide : {false, true}) {
    Fixture f;
    const auto row = take(f.tree->emplace<Row>(f.root));
    const auto child = take(f.tree->emplace<Label>(row, "AAAA", fixed_label_width()));
    const std::array labels{child};
    int parent_calls{}, root_calls{}, child_calls{};
    auto root_binding = take(f.router->bind(
      f.root,
      [&](Widget &, const RoutedEvent &) {
        ++root_calls;

        return EventReply::handled;
      },
      false));
    auto parent_binding = take(f.router->bind(
      row,
      [&](Row &, const RoutedEvent &) {
        ++parent_calls;

        return EventReply::handled;
      },
      false));
    auto child_binding = take(f.router->bind(child, [&](Label &, const RoutedEvent &) {
      ++child_calls;

      if (hide) {
        take(f.tree->set_visible(row, false));
      } else {
        take(f.tree->set_enabled(row, false));
      }

      return EventReply::ignored;
    }));

    f.draw({12, 5}, labels);

    const auto routed = f.input("\r");

    check(routed.target == child.id() && routed.handler == f.root.id(),
          "bubble retains its target and reaches the surviving eligible root");
    check(child_calls == 1 && parent_calls == 0 && root_calls == 1,
          "ancestor hidden or disabled by the lower handler is rechecked before delivery");
    check(take(f.router->focused()).id().session == 0, "focus recovery excludes a newly ineligible ancestor subtree");
    check(take(f.router->invoke(child, {Action::activate})).status == DispatchStatus::no_target,
          "later action cannot enter the newly ineligible child subtree");
    check(child_calls == 1 && parent_calls == 0 && root_calls == 1, "rejected later action invokes no handler");
  }
}
} // namespace

int main() {
  layout_render_input();
  bubbling_rechecks_ancestor_state();

  return failures == 0 ? 0 : 1;
}
