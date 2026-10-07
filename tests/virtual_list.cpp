// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/test_terminal.hpp"
#include <cstdlib>
#include <iostream>
#include <portals/virtual_list.hpp>

namespace allocation_test {
thread_local std::size_t count{};
thread_local std::size_t bytes{};
thread_local std::ptrdiff_t remaining{-1};
} // namespace allocation_test
void *operator new(std::size_t size) {
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  ++allocation_test::count;
  allocation_test::bytes += size;

  if (void *value = std::malloc(size ? size : 1)) {
    return value;
  }

  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

// Match nothrow allocation used by standard-library temporary buffers to this fixture's allocator.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}

namespace {
using namespace portals;
int failures{};
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    std::cerr << "fixture failed\n";
    std::abort();
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Page : ModelSnapshot<int> {
  std::vector<ModelRow<int>> rows;
  RowRange available;
  RangeState state;
  mutable std::size_t calls{};
  mutable std::size_t requested{};
  Page(std::uint64_t revision, std::uint64_t count, RowRange range, RangeState ready = RangeState::ready)
      : ModelSnapshot(revision, count), available(range), state(ready) {
    for (std::size_t i = 0; i < range.count; ++i) {
      rows.push_back({{range.first + i + 1, 1}, static_cast<int>(i)});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    ++calls;
    requested += range.count;

    if (state == RangeState::error) {
      return {state, {}, "failed"};
    }

    if (state == RangeState::ready && range.first >= available.first &&
        range.first - available.first <= available.count &&
        range.count <= available.count - (range.first - available.first)) {
      return {state, std::span(rows).subspan(static_cast<std::size_t>(range.first - available.first), range.count), {}};
    }

    return {};
  }
};
struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
};
void bounded_delegates() {
  for (const auto count : {100ULL, 1000000ULL}) {
    Fixture f;
    auto page = std::make_shared<Page>(1, count, RowRange{8, 7});
    auto model = take(Model<int>::create(*f.tree, f.root, page));
    int formatted{};
    auto node = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), [&](const ModelRow<int> &) {
      ++formatted;

      return take(GraphemeText::from("row"));
    }));
    take(set_virtual_list_first(*f.tree, node, 10));

    auto request = take(prepare_virtual_list(*f.tree, node, take(model->pin()), {8, 3}));

    check(!request && formatted == 3 && page->requested == 7, "viewport plus overscan, no invisible formatting");
    check(take(f.tree->inspect(node)).children.empty(), "no row widgets");
    take(f.tree->read(node, [&](const VirtualList<int> &list) {
      check(list.visible_range() == RowRange{10, 3}, "visible logical interval");
      check(list.prepared_range() == RowRange{8, 7}, "bounded prepared interval");
    }));
  }
}

struct SurfaceSink : PaintContext {
  NodeGeometry geometry;
  std::vector<RenderCommand> &commands;
  SurfaceSink(NodeGeometry value, std::vector<RenderCommand> &output) : geometry(value), commands(output) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    for (const auto &value : values) {
      std::visit(
        [&](const auto &command) {
          auto result = command;

          result.clip = take(command.clip.translated(geometry.surface.origin())).intersection(geometry.clip);

          if constexpr (std::is_same_v<std::decay_t<decltype(command)>, TextCommand>) {
            result.origin = take(translated(command.origin, geometry.surface.origin()));
          } else {
            result.area = take(command.area.translated(geometry.surface.origin()));
          }

          commands.emplace_back(result);
        },
        value);
    }

    return {};
  }
};
struct Screen {
  test_support::FakeClock clock;
  test_support::TestTerminal terminal{clock, {8, 3}};
  FrameWriter writer{terminal};
  std::expected<void, TreeError> draw(Fixture &f, Node<VirtualList<int>> node, CellSize size, CellSize clip_size = {},
                                      CellPoint origin = {}) {
    terminal.resize(size);

    const auto viewport = take(CellRect::from({}, clip_size == CellSize{} ? size : clip_size));
    const auto bounds = take(CellRect::from(origin, size));
    std::optional<CellBuffer> buffer;
    std::expected<void, TreeError> result;

    take(f.tree->invalidate(f.root, Invalidation::layout));
    take(f.tree->frame([&] {
      take(layout_tree(*f.tree, f.root, bounds, viewport));

      std::vector<RenderCommand> commands;
      SurfaceSink sink(take(f.tree->inspect(node)).geometry, commands);

      result = f.tree->paint(node, sink);

      if (result) {
        buffer.emplace(take(render_full_frame(size, commands)));
      }
    }));

    if (buffer) {
      take(writer.submit(std::move(*buffer)));

      const auto written = writer.flush(TerminalDeadline::max());

      check(!written.error && !writer.pending(), "writer finishes independently observed output");
    }

    return result;
  }

  std::string row(int y) const {
    const auto snapshot = terminal.snapshot();
    std::string result;

    for (int x = 0; x < snapshot.size.width; ++x) {
      result += snapshot.cells[static_cast<std::size_t>(y * snapshot.size.width + x)].glyph;
    }

    return result;
  }
};
auto label() {
  return [](const ModelRow<int> &row) {
    return take(GraphemeText::from(std::to_string(row.id.value)));
  };
}

std::optional<RowId> selected(Fixture &f, Node<VirtualList<int>> node) {
  std::optional<RowId> value;

  take(f.tree->read(node, [&](const auto &list) {
    value = list.selection();
  }));

  return value;
}

void render_geometry_and_input() {
  Fixture f;
  auto page = std::make_shared<Page>(1, 100, RowRange{0, 10});
  auto model = take(Model<int>::create(*f.tree, f.root, page));
  VirtualListOptions options;

  options.row_height = 2;
  options.selected.background = RgbColor{20, 40, 60};

  auto list = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), label(), options));
  auto router = take(EventRouter::create(*f.tree));
  auto binding = take(bind_virtual_list(*router, *f.tree, list));
  Screen screen;

  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 5}));
  take(screen.draw(f, list, {4, 5}));
  check(screen.row(0) == "1   " && screen.row(1) == "    " && screen.row(2) == "2   " && screen.row(4) == "3   ",
        "fixed row height includes final partial row");

  take(screen.draw(f, list, {4, 5}, {}, {1, 1}));
  check(screen.row(0) == "    " && screen.row(1) == " 1  ", "widget-local commands translate through real geometry");
  take(router->dispatch(InputEvent{MouseInputEvent{1, 3, MouseButton::left, MouseAction::press}}));
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 5}));
  check(selected(f, list) == RowId{2, 1}, "mouse subtracts surface origin before fixed-row lookup");

  const auto reads = page->calls;

  take(screen.draw(f, list, {4, 5}));
  check(page->calls == reads, "layout and paint never read or format model");

  auto clicked = take(router->dispatch(InputEvent{MouseInputEvent{1, 3, MouseButton::left, MouseAction::press}}));

  check(clicked.status == DispatchStatus::handled, "real geometry routes click to row");
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 5}));
  check(selected(f, list) == RowId{2, 1}, "click in second cell row selects stable ID");
  take(screen.draw(f, list, {4, 5}));
  check(screen.terminal.snapshot().cells[8].style.background == options.selected.background,
        "selected row style is rendered");
  take(router->invoke(list, {Action::move_down}));
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 5}));
  check(selected(f, list) == RowId{3, 1}, "down selects next stable row");
  take(screen.draw(f, list, {4, 5}));
  take(router->dispatch(InputEvent{MouseInputEvent{1, 1, MouseButton::wheel_down, MouseAction::scroll}}));
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 5}));
  take(screen.draw(f, list, {4, 5}));
  check(screen.row(0) == "2   " && selected(f, list) == RowId{3, 1},
        "wheel changes first row without changing selection");

  const auto stale = screen.draw(f, list, {3, 4});

  check(!stale && stale.error() == TreeError::invalid_geometry, "resize requires explicit preparation");
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {3, 4}));
  take(screen.draw(f, list, {3, 4}, {2, 2}));
  check(screen.row(0) == "2  " && screen.row(2) == "   ", "ancestor viewport clips real rendered rows");
  take(f.tree->frame([&] {
    auto rejected = prepare_virtual_list(*f.tree, list, take(model->pin()), {3, 4});

    check(!rejected && rejected.error() == TreeError::would_block, "preparation forbidden during frame");
  }));
}

void unicode_empty_and_limits() {
  Fixture f;
  auto page = std::make_shared<Page>(1, 100, RowRange{0, 10});
  auto model = take(Model<int>::create(*f.tree, f.root, page));
  auto list = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), [](const auto &) {
    return take(GraphemeText::from("\xE7\x95\x8C"
                                   "a"));
  }));

  Screen screen;

  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {1, 2}));
  take(screen.draw(f, list, {1, 2}));
  check(screen.row(0) == " ", "wide grapheme cannot leave an orphan in one cell");
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {2, 2}));
  take(screen.draw(f, list, {2, 2}));
  check(screen.terminal.snapshot().cells[0].glyph == "\xE7\x95\x8C" && screen.terminal.snapshot().cells[1].tail,
        "wide glyph keeps continuation");

  VirtualListOptions bad;

  bad.row_height = 0;

  auto rejected = set_virtual_list_options(*f.tree, list, bad);

  check(!rejected && rejected.error() == TreeError::invalid_argument, "zero row height explicit");
  bad.row_height = 1;
  bad.overscan_rows = std::numeric_limits<std::size_t>::max();
  take(set_virtual_list_options(*f.tree, list, bad));

  auto huge = std::make_shared<Page>(2, std::numeric_limits<std::uint64_t>::max(), RowRange{});

  take(model->publisher().publish(huge));
  take(f.scheduler->dispatch());
  take(set_virtual_list_first(*f.tree, list, std::numeric_limits<std::uint64_t>::max()));

  auto too_large = prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3});

  check(!too_large && too_large.error() == TreeError::invalid_argument, "oversized range never wraps or truncates");
  bad.overscan_rows = 2;
  take(set_virtual_list_options(*f.tree, list, bad));

  auto last = take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3}));

  check(last && last->range() == RowRange{std::numeric_limits<std::uint64_t>::max() - 3, 3},
        "last row and overscan checked at uint64 maximum");

  auto empty = std::make_shared<Page>(3, 0, RowRange{});

  take(model->publisher().publish(empty));
  take(f.scheduler->dispatch());
  check(!take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3})), "empty model schedules no work");
  take(screen.draw(f, list, {4, 3}));
  check(screen.row(0) == "    ", "empty model draws blank frame");

  const auto empty_reads = empty->calls;

  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {0, 3}));
  take(f.tree->consume_frame_request());
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {0, 3}));
  check(empty->calls == empty_reads && take(f.tree->consume_frame_request()) == Invalidation::none,
        "empty or zero-width preparation does no provider work and stays idle");
}

void pending_and_identity() {
  Fixture f;
  auto pending = std::make_shared<Page>(1, 100, RowRange{}, RangeState::pending);
  auto model = take(Model<int>::create(*f.tree, f.root, pending));
  auto a = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), label()));
  auto router = take(EventRouter::create(*f.tree));
  auto binding = take(bind_virtual_list(*router, *f.tree, a));
  auto pin = take(model->pin());
  auto old = take(prepare_virtual_list(*f.tree, a, pin, {4, 3}));

  check(old.has_value(), "pending returns one explicit dispatch receipt");
  check(!take(prepare_virtual_list(*f.tree, a, pin, {4, 3})), "same pin never redispatches pending work");

  Screen screen;

  take(screen.draw(f, a, {4, 3}));
  check(screen.row(0) == "... ", "pending placeholder renders through writer");
  take(router->invoke(a, {Action::move_end}));

  auto latest = take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));

  check(latest && latest->range() == RowRange{95, 5} && old->stop_requested(), "pending End scroll replaces request");

  auto late = old->complete(std::make_shared<Page>(1, 100, old->range()));

  check(!late && late.error() == SchedulerError::closed, "old completion cannot alter selection");
  take(latest->complete(std::make_shared<Page>(1, 100, latest->range())));
  take(f.scheduler->dispatch());

  auto complete_pin = take(model->pin());

  take(prepare_virtual_list(*f.tree, a, complete_pin, {4, 3}));
  check(selected(f, a) == RowId{100, 1}, "latest pending navigation selects its resulting ID");
  take(screen.draw(f, a, {4, 3}));
  check(screen.row(2) == "100 ", "only latest viewport data is painted");

  auto revised = std::make_shared<Page>(2, 101, RowRange{0, 101});

  revised->rows[0].id = {100, 1};
  revised->rows[99].id = {999, 1};
  take(model->publisher().publish(revised));
  take(f.scheduler->dispatch());
  take(set_virtual_list_first(*f.tree, a, 0));
  take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));
  check(selected(f, a) == RowId{100, 1}, "insertion and reorder preserve stable selection");
  take(router->invoke(a, {Action::move_down}));
  take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));
  check(selected(f, a) == RowId{2, 1}, "navigation locates selected identity in the bounded visible range");
}

void consumers_hide_retry_and_selection_intent() {
  Fixture f;
  auto page = std::make_shared<Page>(1, 100, RowRange{}, RangeState::pending);
  auto model = take(Model<int>::create(*f.tree, f.root, page));
  auto a = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), label()));
  auto b = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), label()));

  take(set_virtual_list_first(*f.tree, b, 20));

  auto pin = take(model->pin());
  auto ar = take(prepare_virtual_list(*f.tree, a, pin, {4, 3}));
  auto br = take(prepare_virtual_list(*f.tree, b, pin, {4, 3}));

  take(br->complete(std::make_shared<Page>(1, 100, br->range())));
  take(f.scheduler->dispatch());

  auto next = take(model->pin());

  take(prepare_virtual_list(*f.tree, a, next, {4, 3}));
  take(prepare_virtual_list(*f.tree, b, next, {4, 3}));
  take(f.tree->read(a, [](const auto &view) {
    check(view.range_state() == RangeState::pending, "other consumer remains pending");
  }));
  take(f.tree->read(b, [](const auto &view) {
    check(view.range_state() == RangeState::ready, "completion updates its consumer");
  }));

  auto router = take(EventRouter::create(*f.tree));
  auto binding = take(bind_virtual_list(*router, *f.tree, a));

  take(router->invoke(a, {Action::move_down}));
  take(set_virtual_list_selection(*f.tree, a, RowId{77, 1}));
  take(ar->complete(std::make_shared<Page>(1, 100, ar->range())));
  take(f.scheduler->dispatch());
  take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));
  check(selected(f, a) == RowId{77, 1}, "explicit selection cancels pending navigation intent");
  take(set_virtual_list_first(*f.tree, a, 40));

  auto hidden_work = take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));

  take(f.tree->set_visible(f.root, false));
  take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));
  check(hidden_work->stop_requested(), "hidden ancestor cancels pending work");
  take(f.tree->read(a, [](const auto &view) {
    check(view.visible_range().count == 0, "hidden view releases delegates");
  }));
  take(f.tree->set_visible(f.root, true));

  auto retry = take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));

  take(retry->complete(std::make_shared<Page>(1, 100, retry->range(), RangeState::error)));
  take(f.scheduler->dispatch());
  take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));
  take(f.tree->read(a, [](const auto &view) {
    check(view.range_state() == RangeState::error, "provider error is observable");
  }));
  take(f.tree->remove(b));

  Screen screen;

  take(screen.draw(f, a, {4, 3}));
  check(screen.row(0) == "!   ", "provider error placeholder renders explicitly");
  take(retry_virtual_list(*f.tree, a));

  auto retried = take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));

  check(retried.has_value(), "explicit provider retry gives fresh dispatch receipt");
  retried->cancel();

  auto again = take(prepare_virtual_list(*f.tree, a, take(model->pin()), {4, 3}));

  check(again.has_value(), "rejected worker dispatch is retryable after cancellation");
  take(f.tree->remove(a));
  check(again->stop_requested(), "removal cancels worker without joining");
}

void transactional_failures_and_lifetime() {
  Fixture f;
  auto initial = std::make_shared<Page>(1, 100, RowRange{0, 10});
  std::weak_ptr<const Page> lifetime = initial;
  auto model = take(Model<int>::create(*f.tree, f.root, initial));
  bool fail{};
  bool allocation{};
  int calls{};
  auto list = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), [&](const auto &row) {
    ++calls;

    if (fail && calls % 3 == 2) {
      if (allocation) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("delegate failed");
    }

    return take(GraphemeText::from(std::to_string(row.id.value)));
  }));
  take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3}));
  initial.reset();

  auto replacement = std::make_shared<Page>(2, 100, RowRange{0, 10});

  replacement->rows[0].id = {55, 1};
  take(model->publisher().publish(replacement));
  take(f.scheduler->dispatch());

  auto pin = take(model->pin());

  fail = true;

  Screen screen;

  for (bool bad_alloc : {false, true}) {
    calls = 0;
    allocation = bad_alloc;

    const auto result = prepare_virtual_list(*f.tree, list, pin, {4, 3});

    check(!result && result.error() == (bad_alloc ? TreeError::no_memory : TreeError::callback_failed),
          "delegate/allocation error returns without mixed presentation");
    take(screen.draw(f, list, {4, 3}));
    check(screen.row(0) == "1   " && screen.row(1) == "2   ", "failed prepare retains entire previous frame and text");
    check(!lifetime.expired(), "retained prepared frame owns old provider storage");
  }

  fail = false;
  take(prepare_virtual_list(*f.tree, list, pin, {4, 3}));
  check(lifetime.expired(), "successful prepare releases old frame owner");
  take(screen.draw(f, list, {4, 3}));
  check(screen.row(0) == "55  ", "successful retry commits new presentation");

  auto foreign = take(Model<int>::create(*f.tree, f.root, replacement));
  const auto wrong = prepare_virtual_list(*f.tree, list, take(foreign->pin()), {4, 3});

  check(!wrong && wrong.error() == TreeError::stale_node,
        "foreign frame is rejected even when cached revision matches");
  model.reset();
  take(screen.draw(f, list, {4, 3}));
  check(screen.row(0) == "55  ", "prepared owning text survives source destruction through real frame");
}

void allocation_transaction_and_idle() {
  Fixture f;
  auto first = std::make_shared<Page>(1, 100, RowRange{0, 10});
  auto model = take(Model<int>::create(*f.tree, f.root, first));
  auto list = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), label()));
  auto initial_pin = take(model->pin());

  take(prepare_virtual_list(*f.tree, list, initial_pin, {4, 3}));
  take(f.tree->consume_frame_request());
  take(prepare_virtual_list(*f.tree, list, initial_pin, {4, 3}));
  check(take(f.tree->consume_frame_request()) == Invalidation::none,
        "unchanged preparation does not schedule idle frames");

  auto second = std::make_shared<Page>(2, 100, RowRange{0, 10});

  second->rows[0].id = {22, 1};
  take(model->publisher().publish(second));
  take(f.scheduler->dispatch());

  auto pin = take(model->pin());
  Screen screen;
  bool completed{};
  int rejected{};

  for (std::ptrdiff_t budget = 0; budget < 256 && !completed; ++budget) {
    allocation_test::remaining = budget;

    auto result = prepare_virtual_list(*f.tree, list, pin, {4, 3});
    allocation_test::remaining = -1;

    if (result) {
      completed = true;
    } else {
      ++rejected;
      check(result.error() == TreeError::no_memory, "real allocation failure is explicit no_memory");
      take(screen.draw(f, list, {4, 3}));
      check(screen.row(0) == "1   ", "real allocation failure preserves previous complete presentation");
    }
  }

  check(completed && rejected > 0, "allocation failpoints eventually commit");
  take(screen.draw(f, list, {4, 3}));
  check(screen.row(0) == "22  ", "successful allocation retry publishes new text");
}

void single_budget_retry_and_affinity() {
  Fixture f;
  auto page = std::make_shared<Page>(1, 100, RowRange{}, RangeState::pending);
  auto model = take(Model<int>::create(*f.tree, f.root, page, ModelOptions{16, 1, 1}));
  auto list = take(f.tree->emplace<VirtualList<int>>(f.root, take(model->open_range()), label()));
  auto first = take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3}));

  take(set_virtual_list_first(*f.tree, list, 10));

  auto full = prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3});

  check(!full && full.error() == TreeError::would_block, "cancelled caller token retains finite work budget");
  first.reset();

  auto next = take(prepare_virtual_list(*f.tree, list, take(model->pin()), {4, 3}));

  check(next && next->range() == RowRange{8, 7}, "releasing cancelled receipt restores single-slot progress");

  auto pin = take(model->pin());
  std::expected<std::optional<ModelRequest<int>>, TreeError> foreign;
  std::thread worker([&] {
    foreign = prepare_virtual_list(*f.tree, list, pin, {4, 3});
  });

  worker.join();
  check(!foreign && foreign.error() == TreeError::wrong_thread,
        "preparation rejects foreign thread before provider read");
}
} // namespace
int main() {
  single_budget_retry_and_affinity();
  allocation_transaction_and_idle();
  bounded_delegates();
  render_geometry_and_input();
  unicode_empty_and_limits();
  pending_and_identity();
  consumers_hide_retry_and_selection_intent();
  transactional_failures_and_lifetime();

  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
