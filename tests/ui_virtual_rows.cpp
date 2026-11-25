// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cstdlib>
#include <doctest.h>
#include <new>
#include <portals/ui.hpp>
#include <portals/ui_virtual_rows.hpp>
#include <thread>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
}

void *operator new(std::size_t size) {
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  if (void *p = std::malloc(size ? size : 1)) {
    return p;
  }

  throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept {
  std::free(p);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *p) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t) noexcept {
  ::operator delete(p);
}

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

template <class T, class E>
T take(std::expected<T, E> value, std::source_location where = std::source_location::current()) {
  INFO(where.file_name(), ":", where.line());
  if (!value) {
    if constexpr (std::same_as<E, TreeError>) {
      INFO("TreeError=", static_cast<int>(value.error()));
      REQUIRE(value);
    }
    if constexpr (std::same_as<E, ManagedError>) {
      INFO("ManagedPhase=", static_cast<int>(value.error().phase),
           " TreeError=", static_cast<int>(std::get<TreeError>(value.error().cause)));
      REQUIRE(value);
    }
  }
  REQUIRE(value.has_value());
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

TEST_CASE("row subtree visibility publication allocates nothing after validation") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  const auto host = take(scene->emplace<SizedWidget>(scene->root()));
  const auto a = take(scene->emplace<SizedWidget>(host));
  const auto b = take(scene->emplace<SizedWidget>(host));
  std::array<Node<Widget>, 1> visible{b};

  struct FailAllocations {
    FailAllocations() {
      allocation_test::remaining = 0;
    }

    ~FailAllocations() {
      allocation_test::remaining = -1;
    }
  };

  auto published = [&] {
    FailAllocations guard;
    return portals::detail::RowTreeAccess::publish(app->tree(), host, visible);
  }();
  take(published);
  CHECK_FALSE(take(app->tree().inspect(a)).visible);
  CHECK(take(app->tree().inspect(b)).visible);
  const auto outsider = take(scene->emplace<SizedWidget>(scene->root()));
  visible[0] = outsider;
  auto rejected = [&] {
    FailAllocations guard;
    return portals::detail::RowTreeAccess::publish(app->tree(), host, visible);
  }();
  REQUIRE_FALSE(rejected);
  CHECK(take(app->tree().inspect(b)).visible);
}

struct LayoutOnlyRows final : ui::detail::VirtualRowsState {
  std::expected<void, TreeError> requestFirst(std::uint64_t) override {
    return {};
  }

  std::expected<void, TreeError> requestMove(std::int64_t) override {
    return {};
  }

  std::expected<void, TreeError> requestPage(bool) override {
    return {};
  }
};

TEST_CASE("row layout result allocation failure is typed including MSVC checked vector proxy") {
  auto state = std::make_shared<LayoutOnlyRows>();
  state->placements.push_back({{}, 0});
  VirtualRows<int> view(state);
  const std::array<MeasuredChild, 1> children{MeasuredChild{{4, 1}, {}, {}}};

  struct FailResultAllocation {
    FailResultAllocation() {
#if defined(_MSVC_STL_VERSION) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
      // The local vector proxy and payload precede the expected result's checked proxy.
      allocation_test::remaining = 2;
#else
      // The local payload precedes the expected result's payload copy.
      allocation_test::remaining = 1;
#endif
    }

    ~FailResultAllocation() {
      allocation_test::remaining = -1;
    }
  };

  auto result = [&] {
    FailResultAllocation guard;
    return view.arrangeChildren(children, {4, 1});
  }();
  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::NO_MEMORY);
}

struct Task {
  bool done{};
  std::string caption;
  bool operator==(const Task &) const = default;
};

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene;
  CellSize available{40, 2};

  explicit Fixture(ManagedOptions options = {}) : scene(take(ManagedScene::create(*app, options))) {
  }

  void prepare() {
    take(scene->afterDispatch(*app));
    take(scene->prepare(*app, available));
    const auto bounds = take(CellRect::from({}, available));
    std::optional<LayoutSnapshot> snapshot;
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      snapshot = take(LayoutSnapshot::capture(app->tree(), app->root()));
    }));
    take(scene->prepareViews(*app, *snapshot));
  }

  void settle() {
    prepare();
    prepare();
    prepare();
  }
};

TEST_CASE("row navigation is one outer tab stop and arrows retain host focus") {
  Fixture fixture;
  fixture.available = {40, 4};
  ui::Ref<Button> before;
  ui::Ref<Button> after;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*fixture.scene, ui::vbox(ui::button("Before")
                                            .onClick([] {
                                            })
                                            .reference(before),
                                          ui::virtualRows(model,
                                                          [](auto row) {
                                                            return ui::hbox(ui::checkbox("", row.field(&Task::done)),
                                                                            ui::label(row.value().caption));
                                                          })
                                            .reference(host),
                                          ui::button("After")
                                            .onClick([] {
                                            })
                                            .reference(after))));
  fixture.settle();
  auto position = [&] {
    VirtualRowsPosition result;
    take(fixture.app->tree().read(host.node(), [&](const auto &view) {
      INFO("host failure=", view.lastError() ? static_cast<int>(*view.lastError()) : -1);
      CHECK_FALSE(view.lastError());
      result = view.position();
    }));
    return result;
  };
  CHECK(position().currentIndex == 0);
  CHECK(position().firstIndex == 0);
  CHECK(position().count == 4);
  CHECK(position().visible == 2);
  take(fixture.app->router().setFocus(before.node()));
  CHECK(take(fixture.app->router().traverse()) == host.node());
  for (int step = 0; step != 8; ++step) {
    take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
    fixture.settle();
    CHECK(take(fixture.app->router().focused()) == host.node());
  }
  CHECK(position().currentIndex == 3);
  CHECK(position().firstIndex == 2);
  CHECK(position().hasBefore == true);
  CHECK(position().hasAfter == false);
  CHECK(take(fixture.app->router().traverse()) == after.node());
  CHECK(take(fixture.app->router().traverse(true)) == host.node());
  CHECK(take(fixture.app->router().traverse(true)) == before.node());
  Node<ScrollBar> scrollbar;
  take(fixture.app->tree().read(host.node(), [&](const auto &view) {
    scrollbar = view.state()->scrollbar;
  }));
  take(fixture.app->router().setFocus(scrollbar));
  CHECK(take(fixture.app->router().traverse()) == after.node());
}

TEST_CASE("current row controls use a local Tab cycle and Escape retains column") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}});
  ui::Ref<VirtualRows<Task>> host;

  struct Controls {
    RowId id;
    ui::Ref<CheckBox> box;
    ui::Ref<LineEdit> editor;
  };

  std::vector<Controls> controls;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   Controls current{row.id()};
                                   controls.push_back(current);
                                   return ui::hbox(ui::checkbox("", row.field(&Task::done)).reference(current.box),
                                                   ui::lineEdit(row.field(&Task::caption)).reference(current.editor));
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(take(fixture.app->router().focused()) == controls[0].box.node());
  CHECK(take(fixture.app->router().traverse()) == controls[0].editor.node());
  CHECK(take(fixture.app->router().traverse()) == controls[0].box.node());
  CHECK(take(fixture.app->router().traverse(true)) == controls[0].editor.node());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::UP}));
  CHECK(take(fixture.app->router().focused()) == controls[0].editor.node());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  CHECK(take(fixture.app->router().focused()) == host.node());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  fixture.settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::F2}));
  CHECK(take(fixture.app->router().focused()) == controls[1].editor.node());
}

TEST_CASE("Escape row navigation admission does not cancel completion drafts") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<ui::Ref<LineEdit>> editors;
  std::vector<ui::RowContext<Task>> rows;
  take(ui::mount(*fixture.scene, ui::virtualRows(model,
                                                 [&](auto row) {
                                                   ui::Ref<LineEdit> editor;
                                                   rows.push_back(row);
                                                   editors.push_back(editor);
                                                   return ui::lineEdit(row.field(&Task::caption)).reference(editor);
                                                 },
                                                 {.editing = {.mode = EditMode::ON_COMPLETION}})
                                   .reference(host)));
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(take(rows[0].field(&Task::caption).set("unfinished")) == EditStatus::DIRTY);
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  CHECK(take(fixture.app->router().focused()) == editors[0].node());
  CHECK(*take(rows[0].field(&Task::caption).snapshot()).draft == "unfinished");
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.lastError() == TreeError::WOULD_BLOCK);
  }));
  auto refused = setVirtualRowsFirst(fixture.app->tree(), host.node(), 2);
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == TreeError::WOULD_BLOCK);
  take(rows[0].commit());
  fixture.settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  CHECK(take(fixture.app->router().focused()) == host.node());
}

TEST_CASE("row position is reactive read-only and qualified by the original host lifetime") {
  Fixture fixture;
  fixture.available = {40, 3};
  auto model = ui::model(std::array{0, 1, 2, 3});
  ui::Ref<VirtualRows<int>> host;
  auto position = host.position();
  take(ui::mount(*fixture.scene, ui::vbox(ui::virtualRows(model,
                                                          [](auto row) {
                                                            return ui::label(std::to_string(row.value()));
                                                          })
                                            .reference(host),
                                          ui::label(position, [](const VirtualRowsPosition &value) {
                                            return std::to_string(value.currentIndex.value_or(99));
                                          }))));
  fixture.settle();
  CHECK(take(position.value().snapshot())->value.currentIndex == 0);
  take(fixture.app->router().setFocus(host.node()));
  for (int step = 0; step != 3; ++step) {
    take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  }
  fixture.settle();
  CHECK(take(position.value().snapshot())->value.currentIndex == 3);
  auto denied = position.value().set(take(position.value().snapshot())->value);
  REQUIRE_FALSE(denied);
  CHECK(std::get<TreeError>(denied.error().cause) == TreeError::INVALID_ARGUMENT);
  take(fixture.scene->remove(host.node()));
  CHECK_FALSE(position.value().snapshot());
}

TEST_CASE("wheel eviction of an editing row retains current and recovers to its host") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<ui::Ref<LineEdit>> editors;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   ui::Ref<LineEdit> editor;
                                   editors.push_back(editor);
                                   return ui::lineEdit(row.field(&Task::caption)).reference(editor);
                                 }).reference(host)));
  fixture.settle();
  // Direct focus has the same complete order as mouse focus, independent of collapsed outer Tab.
  take(fixture.app->router().setFocus(editors[0].node()));
  CHECK(take(fixture.app->router().traverse()) == editors[0].node());
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  CHECK(take(fixture.app->router().focused()) == host.node());
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.position().currentIndex == 0);
    CHECK(view.position().firstIndex == 2);
    CHECK_FALSE(view.position().editing);
  }));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.position().currentIndex == 1);
    CHECK(view.position().firstIndex == 1);
  }));
}

TEST_CASE("semantic current survives source round trip and current removal selects nearest") {
  Fixture fixture;
  auto a = ui::model(std::array{0, 1, 2, 3, 4});
  auto b = ui::model(std::array{10, 11, 12, 13});
  auto navigation = ui::navigation(a);
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(navigation, [](auto row) {
                                   return ui::label(std::to_string(row.value()));
                                 }).reference(host)));
  auto position = [&] {
    VirtualRowsPosition result;
    take(fixture.app->tree().read(host.node(), [&](const auto &view) {
      INFO("host failure=", view.lastError() ? static_cast<int>(*view.lastError()) : -1);
      CHECK_FALSE(view.lastError());
      result = view.position();
    }));
    return result;
  };
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  for (int index = 0; index != 3; ++index) {
    take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  }
  fixture.settle();
  const auto remembered = position();
  CHECK(remembered.currentIndex == 3);
  CHECK(take(navigation.currentRow()) == remembered.current);
  take(navigation.setModel(b));
  fixture.settle();
  CHECK(position().resource != remembered.resource);
  CHECK(position().currentIndex == 0);
  take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  fixture.settle();
  take(navigation.setModel(a));
  fixture.settle();
  CHECK(position().current == remembered.current);
  CHECK(position().currentIndex == remembered.currentIndex);
  CHECK(position().firstIndex == remembered.firstIndex);
  take(a.erase(3));
  fixture.settle();
  CHECK(position().currentIndex == 3);
  CHECK(position().current != remembered.current);
  take(a.clear());
  fixture.settle();
  CHECK(position().count == 0);
  CHECK(position().visible == 0);
  CHECK_FALSE(position().current);
  for (const auto key : {InputKey::UP, InputKey::DOWN, InputKey::HOME, InputKey::END}) {
    take(fixture.app->router().dispatch(KeyEvent{key}));
    fixture.settle();
    CHECK(take(fixture.app->router().focused()) == host.node());
  }
}

TEST_CASE("persistent field identity takes precedence over a changed row column order") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "A"}, Task{true, "B"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<ui::Ref<LineEdit>> editors;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   ui::Ref<LineEdit> editor;
                                   editors.push_back(editor);
                                   auto field =
                                     ui::lineEdit(row.field("caption", &Task::caption, {})).reference(editor);
                                   auto box = ui::checkbox("", row.field(&Task::done));
                                   return row.value().done ? ui::hbox(std::move(field), std::move(box))
                                                           : ui::hbox(std::move(box), std::move(field));
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->router().setFocus(editors[0].node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  fixture.settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(take(fixture.app->router().focused()) == editors[1].node());
}

TEST_CASE("offscreen finite current survives source history and resolves deletion independently of viewport") {
  for (const bool shared : {false, true}) {
    INFO("shared navigation=", shared);
    Fixture fixture;
    auto a = ui::model(std::array{0, 1, 2, 3, 4, 5});
    auto b = ui::model(std::array{10, 11, 12});
    auto navigation = ui::navigation(a);
    ui::Ref<VirtualRows<int>> host;
    auto factory = [](auto row) {
      return ui::label(std::to_string(row.value()));
    };
    auto view = shared ? ui::virtualRows(navigation, factory) : ui::virtualRows(a, factory);
    take(ui::mount(*fixture.scene, std::move(view).reference(host)));
    auto position = [&] {
      VirtualRowsPosition value;
      take(fixture.app->tree().read(host.node(), [&](const auto &widget) {
        CHECK_FALSE(widget.lastError());
        value = widget.position();
      }));
      return value;
    };
    auto change = [&](ui::Model<int> target) {
      if (shared) {
        take(navigation.setModel(std::move(target)));
      } else {
        take(host.setModel(std::move(target)));
      }
      fixture.settle();
    };
    fixture.settle();
    take(scrollVirtualRows(fixture.app->tree(), host.node(), 3));
    fixture.settle();
    CHECK(position().current == RowId{1, 1});
    CHECK(position().currentIndex == 0);
    change(b);
    change(a);
    CHECK(position().current == RowId{1, 1});
    CHECK(position().currentIndex == 0);
    CHECK(position().firstIndex == 3);
    take(a.erase(0));
    fixture.settle();
    CHECK(position().current == RowId{2, 1});
    CHECK(position().currentIndex == 0);
    if (shared) {
      CHECK(take(navigation.currentRow()) == position().current);
    } else {
      CHECK(position().firstIndex == 2);
    }
  }
}

TEST_CASE("enter reveals an offscreen current and delayed handoff does not steal outside focus") {
  Fixture fixture;
  fixture.available = {40, 3};
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
  ui::Ref<VirtualRows<Task>> host;
  ui::Ref<Button> after;
  std::vector<std::pair<RowId, ui::Ref<LineEdit>>> editors;
  take(ui::mount(*fixture.scene,
                 ui::vbox(ui::virtualRows(model,
                                          [&](auto row) {
                                            ui::Ref<LineEdit> editor;
                                            editors.emplace_back(row.id(), editor);
                                            return ui::lineEdit(row.field(&Task::caption)).reference(editor);
                                          })
                            .reference(host),
                          ui::button("After")
                            .onClick([] {
                            })
                            .reference(after))));
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  fixture.settle();
  auto current = std::ranges::find_if(editors.rbegin(), editors.rend(), [](const auto &entry) {
    return entry.first == RowId{1, 1};
  });
  REQUIRE(current != editors.rend());
  CHECK(take(fixture.app->router().focused()) == current->second.node());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::F2}));
  take(fixture.app->router().setFocus(after.node()));
  fixture.settle();
  CHECK(take(fixture.app->router().focused()) == after.node());
  take(fixture.app->router().setFocus(host.node()));
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::F2}));
  take(fixture.app->router().setFocus(after.node()));
  take(fixture.app->router().setFocus(host.node()));
  fixture.settle();
  CHECK(take(fixture.app->router().focused()) == host.node());
}

TEST_CASE("Page in a row editor scrolls its viewport while retaining semantic current") {
  Fixture fixture;
  auto source = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [](auto row) {
                                   return ui::lineEdit(row.field(&Task::caption));
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::PAGE_DOWN}));
  fixture.settle();
  CHECK(take(fixture.app->router().focused()) == host.node());
  take(fixture.app->tree().read(host.node(), [](const auto &widget) {
    CHECK_FALSE(widget.lastError());
    CHECK(widget.position().current == RowId{1, 1});
    CHECK(widget.position().currentIndex == 0);
    CHECK(widget.position().firstIndex == 2);
  }));
}

TEST_CASE("real delegated checkbox writes exact model record and survives eviction and reorder") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<std::pair<RowId, ui::Ref<CheckBox>>> boxes(0);
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   ui::Ref<CheckBox> box;
                                   boxes.emplace_back(row.id(), box);
                                   return ui::hbox(ui::checkbox("", row.field(&Task::done)).reference(box),
                                                   ui::label(row.value().caption), ui::button("Remove").onClick([row] {
                                                     return row.erase();
                                                   }));
                                 }).reference(host)));
  fixture.settle();
  REQUIRE(boxes.size() == 2);
  take(fixture.scene->focus(boxes[0].second.node()));
  take(fixture.app->router().invoke(boxes[0].second.node(), ActionEvent{Action::ACTIVATE}));
  take(model.move(0, 1, 3));
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  auto found = std::ranges::find_if(boxes.rbegin(), boxes.rend(), [](const auto &box) {
    return box.first == RowId{1, 1};
  });
  REQUIRE(found != boxes.rend());
  take(fixture.app->tree().read(found->second.node(), [](const CheckBox &box) {
    CHECK(box.checked());
  }));
}

TEST_CASE("row-owned ordinary checkbox states reclaim after hundreds of scroll transitions") {
  Fixture fixture({.maxStates = 5});
  auto model = ui::model(std::array{0, 1, 2, 3, 4, 5, 6, 7});
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [](auto row) {
                                   return ui::checkbox(std::to_string(row.value()), false);
                                 }).reference(host)));
  fixture.settle();
  for (std::uint64_t step = 0; step < 300; ++step) {
    take(setVirtualRowsFirst(fixture.app->tree(), host.node(), step % 7));
    fixture.settle();
    take(fixture.app->tree().read(host.node(), [](const auto &view) {
      CHECK_FALSE(view.lastError());
      CHECK(view.mountedRows() <= 2);
    }));
  }
}

TEST_CASE("failed row construction keeps the previous committed rows") {
  Fixture fixture;
  auto model = ui::model(std::array{0, 1, 2, 3});
  ui::Ref<VirtualRows<int>> host;
  bool reject{};
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   if (reject) {
                                     throw std::runtime_error("injected row factory failure");
                                   }
                                   return ui::label(std::to_string(row.value()));
                                 }).reference(host)));
  fixture.settle();
  reject = true;
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.firstRow() == 0);
    CHECK(view.mountedRows() == 2);
    CHECK(view.lastError() == TreeError::CALLBACK_FAILED);
  }));
}

TEST_CASE("completion drafts veto scroll and explicit drafts keep exactly one row pin") {
  for (const auto mode : {EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    INFO("mode=", static_cast<int>(mode));
    Fixture fixture;
    auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
    ui::Ref<VirtualRows<Task>> host;
    std::vector<ui::RowContext<Task>> handles(0);
    take(ui::mount(*fixture.scene, ui::virtualRows(
                                     model,
                                     [&](auto row) {
                                       handles.push_back(row);
                                       return ui::lineEdit(row.field(&Task::caption));
                                     },
                                     VirtualRowsOptions{.editing = {.mode = mode}})
                                     .reference(host)));
    fixture.settle();
    REQUIRE(handles.size() == 2);
    auto first = handles[0];
    auto second = handles[1];
    CHECK(take(first.field(&Task::caption).set("draft A")) == EditStatus::DIRTY);
    auto rejected = second.field(&Task::caption).set("draft B");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == TreeError::WOULD_BLOCK);
    CHECK_FALSE(take(second.field(&Task::caption).snapshot()).draft);
    auto moved = setVirtualRowsFirst(fixture.app->tree(), host.node(), 2);
    if (mode == EditMode::ON_COMPLETION) {
      REQUIRE_FALSE(moved);
      CHECK(moved.error() == TreeError::WOULD_BLOCK);
      CHECK((take(first.field(&Task::caption).snapshot()).accepted &&
             *take(first.field(&Task::caption).snapshot()).accepted == "A"));
    } else {
      take(moved);
      fixture.settle();
      CHECK((take(first.field(&Task::caption).snapshot()).draft &&
             *take(first.field(&Task::caption).snapshot()).draft == "draft A"));
      REQUIRE(handles.size() == 4);
      auto another = handles[2].field(&Task::caption).set("second pin");
      REQUIRE_FALSE(another);
      CHECK(another.error() == TreeError::WOULD_BLOCK);
      take(first.commit());
      fixture.settle();
      CHECK_FALSE(first.field(&Task::caption).snapshot());
      take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 0));
      fixture.settle();
      auto restored = std::ranges::find_if(handles.rbegin(), handles.rend(), [](const auto &row) {
        return row.id() == RowId{1, 1};
      });
      REQUIRE(restored != handles.rend());
      CHECK(*take(restored->field(&Task::caption).snapshot()).accepted == "draft A");
    }
  }
}

TEST_CASE("accepted mounted editor keeps Undo while completed history does not pin eviction") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<ui::Ref<LineEdit>> editors(0);
  std::vector<ui::RowContext<Task>> rows(0);
  take(ui::mount(*fixture.scene, ui::virtualRows(model,
                                                 [&](auto row) {
                                                   ui::Ref<LineEdit> editor;
                                                   editors.push_back(editor);
                                                   rows.push_back(row);
                                                   return ui::lineEdit(row.field(&Task::caption)).reference(editor);
                                                 },
                                                 {.editing = {.mode = EditMode::EXPLICIT}})
                                   .reference(host)));
  fixture.settle();
  const auto first = rows.front();
  const auto editor = editors.front();
  take((editor.text() = "edited").result());
  take(first.commit());
  fixture.settle();
  REQUIRE(first.snapshot());
  take(editor.undo().result());
  CHECK(take(first.field(&Task::caption).snapshot()).draft);
  take(first.cancel());
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  CHECK_FALSE(first.snapshot());
}

TEST_CASE("saved row actions and fields close while explicit immutable snapshot stays leased") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "old A"}, Task{false, "old B"}, Task{false, "new C"}});
  std::vector<ui::RowContext<Task>> handles(0);
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   handles.push_back(row);
                                   return ui::checkbox(row.value().caption, row.field(&Task::done));
                                 }).reference(host)));
  fixture.settle();
  auto old = handles.front();
  auto field = old.field(&Task::done);
  auto lease = take(old.snapshot());
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  auto erased = old.erase();
  REQUIRE_FALSE(erased);
  CHECK(erased.error() == TreeError::CLOSED);
  auto changed = field.set(true);
  REQUIRE_FALSE(changed);
  CHECK(changed.error() == TreeError::CLOSED);
  std::optional<TreeError> foreignError;
  std::jthread foreign([&] {
    auto result = old.snapshot();
    if (!result) {
      foreignError = result.error();
    }
  });
  foreign.join();
  CHECK(foreignError == TreeError::WRONG_THREAD);
  CHECK(lease.value().caption == "old A");
  take(fixture.scene->remove(host.node()));
  CHECK_FALSE(handles.back().erase());
}

class FiniteRows final : public ModelSnapshot<int> {
public:
  explicit FiniteRows(bool ready) : ModelSnapshot(1, 8), ready_(ready) {
    for (std::uint64_t i = 0; i < rows_.size(); ++i) {
      rows_[i] = {{i + 1, 1}, static_cast<int>(i)};
    }
  }

  ModelRows<int> read(RowRange range) const override {
    return ready_ ? ModelRows<int>{RangeState::READY, std::span(rows_).subspan(range.first, range.count), {}}
                  : ModelRows<int>{};
  }

private:
  bool ready_;
  std::array<ModelRow<int>, 8> rows_;
};

class LargeFiniteRows final : public ModelSnapshot<int> {
public:
  static constexpr std::uint64_t COUNT = (std::uint64_t{1} << 54) + 7;

  LargeFiniteRows() : ModelSnapshot(1, COUNT) {
  }

  ModelRows<int> read(RowRange range) const override {
    ++reads;
    if (range.first <= 1 && range.count <= 2 - range.first) {
      return {RangeState::READY, std::span(first_).subspan(range.first, range.count), {}};
    }
    if (range.first >= COUNT - 2 && range.count <= COUNT - range.first) {
      return {RangeState::READY, std::span(last_).subspan(range.first - (COUNT - 2), range.count), {}};
    }
    return {};
  }

  RowLookup locateRow(RowId id) const override {
    return id.generation == 1 && id.value > 0 && id.value <= COUNT ? RowLookup{RowLookupState::FOUND, id.value - 1}
                                                                   : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

  mutable std::size_t reads{};

private:
  std::array<ModelRow<int>, 2> first_{{{{1, 1}, 0}, {{2, 1}, 1}}};
  std::array<ModelRow<int>, 2> last_{{{{COUNT - 1, 1}, 2}, {{COUNT, 1}, 3}}};
};

TEST_CASE("finite current and scrollbar preserve exact uint64 positions above double precision") {
  Fixture fixture;
  auto provider = std::make_shared<LargeFiniteRows>();
  auto source = ui::model<int>(provider);
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [](auto row) {
                                   return ui::label(std::to_string(row.value()));
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::END}));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK_FALSE(view.lastError());
    CHECK(view.position().count == LargeFiniteRows::COUNT);
    CHECK(view.position().currentIndex == LargeFiniteRows::COUNT - 1);
    CHECK(view.position().firstIndex == LargeFiniteRows::COUNT - 2);
  }));
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), LargeFiniteRows::COUNT - 1));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK_FALSE(view.lastError());
    CHECK(view.position().firstIndex == LargeFiniteRows::COUNT - 1);
    CHECK(view.position().visible == 1);
  }));
  CHECK(provider->reads <= 4);
}

TEST_CASE("finite pending sources retain one qualified request and consume its completion") {
  Fixture fixture;
  std::optional<ModelRequest<int>> work;
  std::size_t requests{};
  auto model = ui::model<int>(std::make_shared<FiniteRows>(false), {},
                              [&](const auto &, const auto &request) -> std::expected<void, TreeError> {
                                if (request) {
                                  work = *request;
                                  ++requests;
                                }
                                return {};
                              });
  std::vector<int> built(0);
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
    built.push_back(row.value());
    return ui::label(std::to_string(row.value()));
  })));
  fixture.settle();
  REQUIRE(work);
  CHECK(requests == 1);
  CHECK(built.empty());
  std::jthread worker([&] {
    take(work->complete(std::make_shared<FiniteRows>(true)));
  });
  worker.join();
  take(fixture.app->scheduler().dispatch());
  fixture.settle();
  CHECK(built == std::vector<int>{0, 1});
  CHECK(requests == 1);
}

TEST_CASE("finite pending offscreen current shares one consumer without replacing the ready viewport") {
  Fixture fixture;
  std::optional<ModelRequest<int>> work;
  std::vector<RowRange> requests;
  auto source = ui::model<int>(std::make_shared<FiniteRows>(false), {},
                               [&](const auto &, const auto &request) -> std::expected<void, TreeError> {
                                 if (request) {
                                   work = *request;
                                   requests.push_back(request->range());
                                 }
                                 return {};
                               });
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [](auto row) {
                                   return ui::label(std::to_string(row.value()));
                                 }).reference(host)));
  auto deliver = [&] {
    auto request = std::exchange(work, {});
    REQUIRE(request);
    std::jthread worker([&] {
      take(request->complete(std::make_shared<FiniteRows>(true)));
    });
    worker.join();
    take(fixture.app->scheduler().dispatch());
    fixture.settle();
  };
  fixture.settle();
  deliver();
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  REQUIRE(requests.size() == 2);
  CHECK(requests.back().first == 2);
  deliver();
  REQUIRE(requests.size() == 3);
  CHECK(requests.back().first == 0);
  CHECK(requests.back().count == 1);
  take(fixture.app->tree().read(host.node(), [](const auto &widget) {
    CHECK(widget.position().firstIndex == 0);
  }));
  deliver();
  CHECK(requests.size() == 3);
  take(fixture.app->tree().read(host.node(), [](const auto &widget) {
    CHECK_FALSE(widget.lastError());
    CHECK(widget.position().firstIndex == 2);
    CHECK(widget.position().currentIndex == 0);
    CHECK(widget.position().current == RowId{1, 1});
  }));
}

TEST_CASE("cold row-local declarations stay closed until publication without dereferencing a null scene") {
  auto checkClosed = [](const auto &result) {
    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::CLOSED);
  };
  auto outside = ui::model(std::array{1});
  auto outsideScalar = ui::model(42);
  checkClosed(outside.managed());
  checkClosed(outsideScalar.value().snapshot());
  checkClosed((outsideScalar.value() = 43).result());
  CHECK_FALSE(outside.row({1, 1}, 2));
  Fixture fixture;
  auto outer = ui::model(std::array{0, 1});
  std::size_t checked{};
  take(ui::mount(*fixture.scene, ui::virtualRows(outer, [&](auto) {
    auto local = ui::model(std::array{1});
    auto scalar = ui::model(42);
    checkClosed(local.managed());
    checkClosed(scalar.value().snapshot());
    checkClosed((scalar.value() = 43).result());
    checkClosed(local.update({1, 1}, 2));
    CHECK_FALSE(local.row({1, 1}, 2));
    ++checked;
    return ui::hbox(ui::virtualList(local), ui::label(scalar));
  })));
  fixture.settle();
  CHECK(checked == 2);
}

TEST_CASE("external cold declarations first resolved in a row survive that rows eviction") {
  Fixture fixture;
  auto outer = ui::model(std::array{0, 1, 2, 3});
  auto shared = ui::model<int>(std::make_shared<FiniteRows>(true));
  auto scalar = ui::model(42);
  auto navigation = ui::navigation(shared);
  auto state = take(fixture.scene->state(1));
  auto derived = take(fixture.scene->derive(state, [](int value) {
    return std::to_string(value * 2);
  }));
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(outer, [navigation, scalar, state, derived](auto) {
                                   return ui::hbox(ui::virtualList(navigation), ui::label(scalar),
                                                   ui::label(derived).observe(state, [](int) {
                                                   }));
                                 }).reference(host)));
  fixture.settle();
  auto handle = take(shared.managed());
  CHECK(handle.publisher());
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 1));
  fixture.settle();
  CHECK(handle.publisher());
  CHECK(take(navigation.managed()).current());
  take((scalar.value() = 43).result());
  take(state.set(3));
  take(fixture.app->scheduler().dispatch());
  fixture.settle();
  CHECK(take(derived.snapshot())->value == "6");
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK_FALSE(view.lastError());
  }));
}

class ScopedRowActivity final : public ui::detail::SceneActivity {
public:
  ScopedRowActivity(ManagedState<int> source, int &count) : state(source), live(count) {
    ++live;
  }

  ~ScopedRowActivity() override {
    --live;
  }

  bool active() const noexcept override {
    return state.snapshot().has_value();
  }

  void service() noexcept override {
  }

  void cancel() noexcept override {
  }

  ManagedState<int> state;
  int &live;
};

TEST_CASE("row resource scopes reclaim derived models subscriptions and activities") {
  Fixture fixture({.maxStates = 24, .maxModels = 6, .maxObservers = 12});
  auto source = ui::model(std::array{0, 1, 2, 3, 4, 5, 6, 7});
  ui::Ref<VirtualRows<int>> host;
  std::vector<ManagedSource<int>> saved(0);
  int live{};
  std::optional<ui::Navigation<int>> firstNavigation;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [&](auto row) {
                                   auto state = take(fixture.scene->state(row.value()));
                                   auto derived = take(fixture.scene->derive(state, [](int value) {
                                     return value * 2;
                                   }));
                                   saved.push_back(derived);
                                   auto model = ui::model<int>(std::make_shared<FiniteRows>(true));
                                   auto navigation = ui::navigation(model);
                                   if (!firstNavigation) {
                                     firstNavigation = navigation;
                                   }
                                   auto activity = std::make_shared<ScopedRowActivity>(state, live);
                                   take(ui::detail::RowAccess::admit(*fixture.scene, activity));
                                   return ui::hbox(
                                     ui::detail::Access::controlSetup(
                                       ui::button("Scoped").onClick([activity, model] {
                                       }),
                                       [derived](ManagedScene &scene, Node<Button> node, const ui::ControlSettings &) {
                                         return scene.observe(node, derived, [](int) {
                                         });
                                       }),
                                     ui::virtualList(navigation));
                                 }).reference(host)));
  fixture.settle();
  auto old = saved.front();
  auto oldNavigation = std::weak_ptr(take(ui::detail::EditAccess::context(take(firstNavigation->managed()))));
  for (std::uint64_t step = 1; step <= 300; ++step) {
    take(setVirtualRowsFirst(fixture.app->tree(), host.node(), step % 7));
    fixture.settle();
    CHECK(live <= 2);
    take(fixture.app->tree().read(host.node(), [](const auto &view) {
      CHECK_FALSE(view.lastError());
    }));
  }
  CHECK_FALSE(old.snapshot());
  CHECK(oldNavigation.expired());
  take(fixture.scene->remove(host.node()));
  fixture.settle();
  CHECK(live == 0);
}

class ControlledExecutor final : public BackgroundExecutor {
public:
  std::vector<std::shared_ptr<BackgroundJob>> jobs = std::vector<std::shared_ptr<BackgroundJob>>(0);
  std::uint64_t epoch{};
  std::size_t closedPublications{};

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    auto owned = std::shared_ptr<BackgroundJob>(std::move(job));
    jobs.push_back(owned);
    return BackgroundTicket(owned);
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }

  void run() {
    auto pending = std::exchange(jobs, std::vector<std::shared_ptr<BackgroundJob>>(0));
    for (auto &job : pending) {
      std::expected<void, AsyncError> result;
      std::jthread worker([&] {
        job->run({});
        result = job->publish();
      });
      worker.join();
      if (!result && result.error() == AsyncError::CLOSED) {
        ++closedPublications;
      } else {
        take(result);
      }
      ++epoch;
    }
  }
};

struct ArenaTask {
  bool done{};
  std::string_view caption;
  bool alternative{};
};

class CursorTasks final : public WindowProvider<ArenaTask> {
public:
  SourceCapabilities capabilities() const noexcept override {
    return caps;
  }

  std::expected<SourceReadReply<ArenaTask>, SourceError> load(const SourceReadCommand &command,
                                                              PageBuilder<ArenaTask> &out, std::stop_token) override {
    ++loads;
    CHECK(std::this_thread::get_id() != uiThread);
    const auto total = std::uint64_t{1000000};
    const auto position = command.anchor.kind == SourceAnchorKind::TAIL     ? total
                          : command.anchor.kind == SourceAnchorKind::ROW    ? command.anchor.row.value - 1
                          : command.anchor.kind == SourceAnchorKind::CURSOR ? command.anchor.cursor.token[0]
                                                                            : 0;
    const bool backwards = command.direction == SourceDirection::BACKWARD;
    const auto count = std::min<std::uint64_t>(command.maxRows, backwards ? position : total - position);
    const auto first = backwards ? position - count : position;
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto text = std::to_string(first + i);
      auto *bytes = static_cast<char *>(out.resource()->allocate(text.size(), alignof(char)));
      std::copy(text.begin(), text.end(), bytes);
      auto added =
        out.emplace({first + i + 1, 1}, command.dataRevision, ArenaTask{false, std::string_view(bytes, text.size())});
      if (!added) {
        return std::unexpected(added.error());
      }
    }
    return out.ready({command.source, {first, 0}}, {command.source, {first + count, 0}},
                     (backwards ? first == 0 : first + count == total) ? SourceEdge::FINAL_END : SourceEdge::MORE);
  }

  SourceCapabilities caps{true, true, true};
  std::thread::id uiThread = std::this_thread::get_id();
  std::size_t loads{};
};

template <class T> struct NativeFixture {
  ControlledExecutor executor;
  std::unique_ptr<Application> app = take(Application::create(ApplicationOptions{.backgroundExecutor = &executor}));
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));

  void cycle() {
    take(scene->afterDispatch(*app));
    executor.run();
    take(app->scheduler().dispatch());
    take(scene->prepare(*app, {40, 2}));
    const auto bounds = take(CellRect::from({}, {40, 2}));
    std::optional<LayoutSnapshot> snapshot;
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      snapshot = take(LayoutSnapshot::capture(app->tree(), app->root()));
    }));
    take(scene->prepareViews(*app, *snapshot));
    take(scene->completedLayout(*app));
  }
};

TEST_CASE("native keyboard current is cursor qualified and forward-only boundaries retain host focus") {
  NativeFixture<ArenaTask> fixture;
  auto provider = std::make_shared<CursorTasks>();
  provider->caps = {};
  auto source = ui::model<ArenaTask>(provider);
  ui::Ref<VirtualRows<ArenaTask>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [](auto row) {
                                   return ui::label(row.value().caption);
                                 }).reference(host)));
  auto settle = [&] {
    for (int step = 0; step != 4; ++step) {
      fixture.cycle();
    }
  };
  auto position = [&] {
    VirtualRowsPosition value;
    take(fixture.app->tree().read(host.node(), [&](const auto &view) {
      value = view.position();
      CHECK_FALSE(view.lastError());
      CHECK_FALSE(view.state()->scrollbarVisible);
    }));
    return value;
  };
  settle();
  take(fixture.app->router().setFocus(host.node()));
  CHECK(position().current == RowId{1, 1});
  CHECK_FALSE(position().count);
  CHECK_FALSE(position().firstIndex);
  CHECK_FALSE(position().currentIndex);
  CHECK(position().hasBefore == false);
  CHECK(position().hasAfter == true);
  for (const auto key : {InputKey::UP, InputKey::END}) {
    take(fixture.app->router().dispatch(KeyEvent{key}));
    settle();
    CHECK(take(fixture.app->router().focused()) == host.node());
    CHECK(position().current == RowId{1, 1});
  }
  for (int step = 0; step != 5; ++step) {
    take(fixture.app->router().dispatch(KeyEvent{InputKey::DOWN}));
    settle();
    CHECK(take(fixture.app->router().focused()) == host.node());
  }
  CHECK(position().current == RowId{6, 1});
  CHECK(provider->loads < 10);
  CHECK_FALSE(position().count);
}

TEST_CASE("native current reveals by stable row after eviction and survives source round trip") {
  NativeFixture<ArenaTask> fixture;
  auto a = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  auto b = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  ui::Ref<VirtualRows<ArenaTask>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(a, [](auto row) {
                                   return ui::label(row.value().caption);
                                 }).reference(host)));
  auto settle = [&] {
    for (int step = 0; step != 6; ++step) {
      fixture.cycle();
    }
  };
  auto position = [&] {
    VirtualRowsPosition value;
    take(fixture.app->tree().read(host.node(), [&](const auto &view) {
      INFO("host failure=", view.lastError() ? static_cast<int>(*view.lastError()) : -1);
      CHECK_FALSE(view.lastError());
      value = view.position();
    }));
    return value;
  };
  auto key = [&](InputKey key) {
    take(fixture.app->router().dispatch(KeyEvent{key}));
    settle();
    CHECK(take(fixture.app->router().focused()) == host.node());
  };
  settle();
  take(fixture.app->router().setFocus(host.node()));
  for (int step = 0; step != 3; ++step) {
    key(InputKey::DOWN);
  }
  CHECK(position().current == RowId{4, 1});
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 6));
  settle();
  CHECK(position().current == RowId{4, 1});

  struct PaintCommands final : PaintContext {
    std::size_t count{};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      count += commands.size();
      return {};
    }
  };

  auto markerCount = [&] {
    PaintCommands sink;
    take(fixture.app->tree().frame([&] {
      take(fixture.app->tree().paint(host.node(), sink));
    }));
    return sink.count;
  };
  CHECK(markerCount() == 0);
  key(InputKey::DOWN);
  CHECK(position().current == RowId{5, 1});
  CHECK(markerCount() == 1);
  take(host.setModel(b));
  settle();
  CHECK(position().current == RowId{1, 1});
  take(host.setModel(a));
  settle();
  CHECK(position().current == RowId{5, 1});
  key(InputKey::END);
  CHECK(position().current == RowId{1000000, 1});
  CHECK(position().hasAfter == false);
  key(InputKey::HOME);
  CHECK(position().current == RowId{1, 1});
  CHECK(position().hasBefore == false);
  CHECK_FALSE(position().currentIndex);
  CHECK_FALSE(position().count);
}

TEST_CASE("rejected native window preserves the entire committed orientation") {
  NativeFixture<ArenaTask> fixture;
  auto source = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  ui::Ref<VirtualRows<ArenaTask>> host;
  bool reject{};
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [&](auto row) {
                                   if (reject) {
                                     throw std::runtime_error("reject newly staged row");
                                   }
                                   return ui::label(row.value().caption);
                                 }).reference(host)));
  auto settle = [&] {
    for (int step = 0; step != 4; ++step) {
      fixture.cycle();
    }
  };
  settle();
  VirtualRowsPosition before;
  take(fixture.app->tree().read(host.node(), [&](const auto &widget) {
    before = widget.position();
  }));
  reject = true;
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  settle();
  take(fixture.app->tree().read(host.node(), [&](const auto &widget) {
    CHECK(widget.position() == before);
    CHECK(widget.lastError() == TreeError::CALLBACK_FAILED);
    CHECK(widget.mountedRows() == before.visible);
  }));
}

TEST_CASE("native Enter reveals the accepted current before entering its real control") {
  NativeFixture<ArenaTask> fixture;
  auto source = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  ui::Ref<VirtualRows<ArenaTask>> host;
  std::vector<std::pair<RowId, ui::Ref<Button>>> buttons;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [&](auto row) {
                                   ui::Ref<Button> button;
                                   buttons.emplace_back(row.id(), button);
                                   return ui::button(row.value().caption)
                                     .onClick([] {
                                     })
                                     .reference(button);
                                 }).reference(host)));
  auto settle = [&] {
    for (int step = 0; step != 6; ++step) {
      fixture.cycle();
    }
  };
  settle();
  take(fixture.app->router().setFocus(host.node()));
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 6));
  settle();
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  settle();
  auto current = std::ranges::find_if(buttons.rbegin(), buttons.rend(), [](const auto &entry) {
    return entry.first == RowId{1, 1};
  });
  REQUIRE(current != buttons.rend());
  CHECK(take(fixture.app->router().focused()) == current->second.node());
}

TEST_CASE("native conflicting persistent field keys fail checked row staging") {
  NativeFixture<ArenaTask> fixture;
  auto source = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  ui::Ref<VirtualRows<ArenaTask>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(source, [](auto row) {
                                   return ui::hbox(ui::checkbox("", row.field("same", &ArenaTask::done, {})),
                                                   ui::checkbox("", row.field("same", &ArenaTask::alternative, {})));
                                 }).reference(host)));
  for (int i = 0; i < 4; ++i) {
    fixture.cycle();
  }
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.mountedRows() == 0);
    CHECK(view.lastError() == TreeError::INVALID_ARGUMENT);
  }));
}

struct RetiredRowHook {
  std::function<void()> callback;

  ~RetiredRowHook() {
    if (callback) {
      callback();
    }
  }
};

TEST_CASE("deferred Enter does not focus a superseded source accepted by a retired row callback") {
  Fixture fixture;
  auto a = ui::model(std::array{Task{false, "A0"}, Task{false, "A1"}, Task{false, "A2"}, Task{false, "A3"}});
  auto b = ui::model(std::array{Task{false, "B0"}, Task{false, "B1"}, Task{false, "B2"}, Task{false, "B3"}});
  ui::Ref<VirtualRows<Task>> host;
  bool armed{};
  bool switched{};
  std::optional<ManagedError> failure;
  take(ui::mount(*fixture.scene, ui::virtualRows(a, [&](auto row) {
                                   auto hook = std::make_shared<RetiredRowHook>();
                                   if (row.value().caption == "A2") {
                                     hook->callback = [&] {
                                       if (armed && !switched) {
                                         switched = true;
                                         auto changed = host.setModel(b);
                                         if (!changed) {
                                           failure = changed.error();
                                         }
                                       }
                                     };
                                   }
                                   return ui::hbox(ui::lineEdit(row.field(&Task::caption)),
                                                   ui::button("Keep").onClick([hook] {
                                                   }));
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->router().setFocus(host.node()));
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  const auto generation = portals::detail::RowTreeAccess::focusGeneration(fixture.app->router());
  armed = true;
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  fixture.settle();
  CHECK(switched);
  CHECK_FALSE(failure);
  CHECK(take(fixture.app->router().focused()) == host.node());
  CHECK(portals::detail::RowTreeAccess::focusGeneration(fixture.app->router()) == generation);
}

TEST_CASE("native publication metadata precedes destructor source swaps and remembers the correct anchor") {
  NativeFixture<ArenaTask> fixture;
  auto a = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  auto bProvider = std::make_shared<CursorTasks>();
  bProvider->caps.seekRow = false;
  auto b = ui::model<ArenaTask>(bProvider);
  auto c = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  ui::Ref<VirtualRows<ArenaTask>> host;
  bool armed{}, switched{};
  std::optional<ManagedError> error;
  std::vector<ui::RowContext<ArenaTask>> rows(0);
  take(ui::mount(*fixture.scene, ui::virtualRows(a, [&](auto row) {
                                   rows.push_back(row);
                                   auto hook = std::make_shared<RetiredRowHook>();
                                   if (row.id() == RowId{1, 1}) {
                                     hook->callback = [&] {
                                       if (armed && !switched) {
                                         switched = true;
                                         auto changed = host.setModel(b);
                                         if (!changed) {
                                           error = changed.error();
                                         }
                                       }
                                     };
                                   }
                                   return ui::hbox(ui::label(row.value().caption), ui::button("Keep").onClick([hook] {
                                   }));
                                 }).reference(host)));
  for (int i = 0; i < 4; ++i) {
    fixture.cycle();
  }
  armed = true;
  take(scrollVirtualRows(fixture.app->tree(), host.node(), 1));
  fixture.cycle();
  REQUIRE(switched);
  REQUIRE_FALSE(error);
  // B has never delivered rows. Its missing seek capability must not invent saved B history.
  take(host.setModel(c));
  take(host.setModel(b));
  for (int i = 0; i < 4; ++i) {
    fixture.cycle();
  }
  take(host.setModel(a));
  for (int i = 0; i < 4; ++i) {
    fixture.cycle();
  }
  const auto restored = std::ranges::find_if(rows.rbegin(), rows.rend(), [](const auto &row) {
    return row.id() == RowId{2, 1} && row.snapshot().has_value();
  });
  REQUIRE(restored != rows.rend());
  take(fixture.scene->close());
  fixture.executor.run();
}

TEST_CASE("native unknown extent uses worker pages and leases arena borrowed data") {
  ControlledExecutor executor;
  auto app = take(Application::create(ApplicationOptions{.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  auto provider = std::make_shared<CursorTasks>();
  auto source = ui::model<ArenaTask>(std::shared_ptr<WindowProvider<ArenaTask>>(provider));
  ui::Ref<VirtualRows<ArenaTask>> host;
  std::vector<ui::RowContext<ArenaTask>> handles(0);
  take(ui::mount(*scene, ui::virtualRows(source, [&](auto row) {
                           handles.push_back(row);
                           return ui::hbox(ui::checkbox("", row.field(&ArenaTask::done)),
                                           ui::label(row.value().caption));
                         }).reference(host)));
  auto cycle = [&] {
    take(scene->afterDispatch(*app));
    executor.run();
    take(app->scheduler().dispatch());
    take(scene->prepare(*app, {40, 2}));
    const auto bounds = take(CellRect::from({}, {40, 2}));
    std::optional<LayoutSnapshot> snapshot;
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      snapshot = take(LayoutSnapshot::capture(app->tree(), app->root()));
    }));
    take(scene->prepareViews(*app, *snapshot));
    take(scene->completedLayout(*app));
  };
  cycle();
  cycle();
  cycle();
  REQUIRE(handles.size() == 2);
  auto lease = take(handles[0].snapshot());
  CHECK(lease.value().caption == "0");
  take(app->tree().read(host.node(), [](const auto &view) {
    CHECK_FALSE(view.rowCount());
    REQUIRE(view.capabilities());
    CHECK(view.capabilities()->backward);
    CHECK(view.mountedRows() == 2);
  }));
  auto fakeFirst = setVirtualRowsFirst(app->tree(), host.node(), 100);
  REQUIRE_FALSE(fakeFirst);
  take(pageVirtualRows(app->tree(), host.node()));
  cycle();
  cycle();
  cycle();
  REQUIRE(handles.size() == 4);
  CHECK(take(handles[2].snapshot()).value().caption == "2");
  CHECK(lease.value().caption == "0");
  CHECK_FALSE(handles[0].snapshot());
  take(pageVirtualRows(app->tree(), host.node(), true));
  cycle();
  cycle();
  cycle();
  CHECK(take(handles.back().snapshot()).value().caption == "1");
  // Relative row intents accumulate before the outstanding worker delivery.
  take(scrollVirtualRows(app->tree(), host.node(), 1));
  take(scrollVirtualRows(app->tree(), host.node(), 1));
  take(scrollVirtualRows(app->tree(), host.node(), 1));
  for (int i = 0; i < 8; ++i) {
    cycle();
  }
  auto rowThree = std::ranges::find_if(handles.rbegin(), handles.rend(), [](const auto &row) {
    return row.id() == RowId{4, 1} && row.snapshot().has_value();
  });
  REQUIRE(rowThree != handles.rend());
  CHECK(take(rowThree->snapshot()).value().caption == "3");
  const auto oldRow = *rowThree;
  auto providerB = std::make_shared<CursorTasks>();
  auto sourceB = ui::model<ArenaTask>(providerB);
  take(host.setModel(sourceB));
  for (int i = 0; i < 4; ++i) {
    cycle();
  }
  CHECK_FALSE(oldRow.snapshot());
  CHECK(take(handles.back().snapshot()).id() == RowId{2, 1});
  take(host.setModel(source));
  for (int i = 0; i < 4; ++i) {
    cycle();
  }
  auto restored = std::ranges::find_if(handles.rbegin(), handles.rend(), [](const auto &row) {
    return row.id() == RowId{4, 1} && row.snapshot().has_value();
  });
  REQUIRE(restored != handles.rend());
  auto forwardProvider = std::make_shared<CursorTasks>();
  forwardProvider->caps = {};
  auto forwardSource = ui::model<ArenaTask>(forwardProvider);
  take(host.setModel(forwardSource));
  for (int i = 0; i < 4; ++i) {
    cycle();
  }
  take(host.setModel(sourceB));
  for (int i = 0; i < 4; ++i) {
    cycle();
  }
  auto stillB = handles.back();
  auto unsupported = host.setModel(forwardSource);
  REQUIRE_FALSE(unsupported);
  CHECK(take(stillB.snapshot()).id() == RowId{2, 1});
  take(host.setModel(forwardSource, {.restore = ModelRestorePolicy::RESET}));
  for (int i = 0; i < 4; ++i) {
    cycle();
  }
  auto unavailableBack = navigateVirtualRows(app->tree(), host.node(), {}, SourceDirection::BACKWARD);
  REQUIRE_FALSE(unavailableBack);
  take(app->router().dispatch(MouseInputEvent{0, 0, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, {}, {}, 2.0}));
  for (int i = 0; i < 4; ++i) {
    cycle();
  }
  auto wheelRow = std::ranges::find_if(handles.rbegin(), handles.rend(), [](const auto &row) {
    return row.id() == RowId{3, 1} && row.snapshot().has_value();
  });
  REQUIRE(wheelRow != handles.rend());
  take(scene->close());
  executor.run();
}

struct AccountedTask {
  std::pmr::string caption;
  bool done{};
};

class AccountedTasks final : public WindowProvider<AccountedTask> {
public:
  SourceCapabilities capabilities() const noexcept override {
    return {true, true, true};
  }

  std::expected<SourceReadReply<AccountedTask>, SourceError>
  load(const SourceReadCommand &command, PageBuilder<AccountedTask> &out, std::stop_token) override {
    CHECK(std::this_thread::get_id() != uiThread);
    auto added = out.emplace({1, 1}, command.dataRevision,
                             AccountedTask{std::pmr::string("accounted native caption", out.resource())});
    if (!added) {
      return std::unexpected(added.error());
    }
    return out.ready({command.source, {0, 0}}, {command.source, {1, 0}}, SourceEdge::FINAL_END);
  }

  std::thread::id uiThread = std::this_thread::get_id();
};

class AccountedEndpoint final : public EditEndpoint<std::pmr::string> {
public:
  explicit AccountedEndpoint(const bool &framing) : inFrame(framing) {
  }

  EditCapabilities capabilities() const noexcept override {
    return {true, true, true};
  }

  std::expected<EditRecord<std::pmr::string>, TreeError> read(ModelResource, RowId) override {
    CHECK_FALSE(inFrame);
    ++reads;
    return EditRecord<std::pmr::string>{std::make_shared<const std::pmr::string>(value), version};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<std::pmr::string> request) override {
    CHECK_FALSE(inFrame);
    CHECK(request.target.row == RowId{1, 1});
    if (request.target.baseVersion != version) {
      return EditReceipt{request.target, EditStatus::CONFLICT};
    }
    value = *request.value;
    return EditReceipt{request.target, EditStatus::ACCEPTED, ++version};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const std::pmr::string &candidate) const override {
    return sizeof(candidate) + candidate.capacity() + 1;
  }

  const bool &inFrame;
  std::pmr::string value{"accounted native caption"};
  std::uint64_t version{1};
  std::size_t reads{};
};

class GuardedBoolean final : public EditEndpoint<bool> {
public:
  explicit GuardedBoolean(const bool &framing) : inFrame(framing) {
  }

  EditCapabilities capabilities() const noexcept override {
    return {true, true, true};
  }

  std::expected<EditRecord<bool>, TreeError> read(ModelResource, RowId) override {
    CHECK_FALSE(inFrame);
    ++reads;
    return EditRecord<bool>{std::make_shared<const bool>(value), version};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<bool> request) override {
    CHECK_FALSE(inFrame);
    if (terminal != EditStatus::ACCEPTED) {
      return EditReceipt{request.target, terminal};
    }
    if (defer) {
      pending = request;
      return EditReceipt{request.target, EditStatus::PENDING};
    }
    value = *request.value;
    return EditReceipt{request.target, EditStatus::ACCEPTED, ++version};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const bool &) const override {
    return sizeof(bool);
  }

  const bool &inFrame;
  bool value{};
  std::size_t reads{};
  std::uint64_t version{1};
  bool defer{};
  EditStatus terminal{EditStatus::ACCEPTED};
  std::optional<EditRequest<bool>> pending;
};

TEST_CASE("native rejected checkbox clicks preserve accepted presentation after prepare") {
  for (const auto status : {EditStatus::REJECTED, EditStatus::CONFLICT}) {
    NativeFixture<ArenaTask> fixture;
    const bool framing{};
    auto endpoint = std::make_shared<GuardedBoolean>(framing);
    endpoint->terminal = status;
    auto source = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
    std::vector<ui::Ref<CheckBox>> boxes(0);
    std::vector<ui::RowContext<ArenaTask>> rows(0);
    take(ui::mount(*fixture.scene, ui::virtualRows(source, [&](auto row) {
      ui::Ref<CheckBox> box;
      boxes.push_back(box);
      rows.push_back(row);
      return ui::checkbox("", row.field("done", &ArenaTask::done, endpoint)).reference(box);
    })));
    for (int i = 0; i < 4; ++i) {
      fixture.cycle();
    }
    REQUIRE_FALSE(boxes.empty());
    (void)fixture.app->router().invoke(boxes.front().node(), ActionEvent{Action::ACTIVATE});
    fixture.cycle();
    take(fixture.app->tree().read(boxes.front().node(), [](const CheckBox &control) {
      CHECK_FALSE(control.checked());
    }));
    auto snapshot = take(rows.front().field("done", &ArenaTask::done, endpoint).snapshot());
    CHECK(snapshot.status == status);
    REQUIRE(snapshot.draft);
    CHECK(*snapshot.draft);
    CHECK_FALSE(*snapshot.accepted);
    take(fixture.scene->close());
  }
}

TEST_CASE("pending and conflict drafts retain one pin and late closed receipts cannot retarget") {
  Fixture fixture;
  const bool framing{};
  auto endpoint = std::make_shared<GuardedBoolean>(framing);
  endpoint->defer = true;
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<ui::RowContext<Task>> rows(0);
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   rows.push_back(row);
                                   return ui::checkbox("", row.field("done", &Task::done, endpoint));
                                 }).reference(host)));
  fixture.settle();
  const auto first = rows.front();
  auto field = first.field("done", &Task::done, endpoint);
  CHECK(take(field.set(true)) == EditStatus::PENDING);
  REQUIRE(endpoint->pending);
  const auto request = *endpoint->pending;
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  CHECK(take(field.snapshot()).status == EditStatus::PENDING);
  auto second = rows.back().field("done", &Task::done, endpoint);
  auto refused = second.set(true);
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == TreeError::WOULD_BLOCK);
  CHECK(take(first.applyReply({request.target, EditStatus::CONFLICT})));
  fixture.settle();
  CHECK(take(field.snapshot()).status == EditStatus::CONFLICT);
  CHECK_FALSE(second.set(true));
  take(first.cancel());
  fixture.settle();
  CHECK_FALSE(first.snapshot());
  CHECK_FALSE(take(first.applyReply({request.target, EditStatus::ACCEPTED, 2})));
  CHECK(take(second.set(true)) == EditStatus::PENDING);
}

TEST_CASE("native accounted pmr text uses common codec editing without endpoint reads during frame") {
  ControlledExecutor executor;
  auto app = take(Application::create(ApplicationOptions{.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  auto provider = std::make_shared<AccountedTasks>();
  auto source = ui::model<AccountedTask>(provider);
  bool framing{};
  auto endpoint = std::make_shared<AccountedEndpoint>(framing);
  auto boolean = std::make_shared<GuardedBoolean>(framing);
  auto codec = std::make_shared<ui::EditTextCodec<std::pmr::string>>();
  codec->format = [](const std::pmr::string &value) -> std::expected<std::string, TreeError> {
    return std::string(value);
  };
  codec->parse = [](std::string_view text) -> std::expected<std::pmr::string, TreeError> {
    return std::pmr::string(text);
  };
  ui::Ref<LineEdit> editor;
  ui::Ref<CheckBox> box;
  std::vector<ui::RowContext<AccountedTask>> rows(0);
  take(ui::mount(*scene, ui::virtualRows(source, [&](auto row) {
    rows.push_back(row);
    return ui::hbox(ui::checkbox("", row.field("done", &AccountedTask::done, boolean)).reference(box),
                    ui::lineEdit(row.field("caption", &AccountedTask::caption, endpoint), codec).reference(editor));
  })));
  for (int iteration = 0; iteration < 5; ++iteration) {
    take(scene->afterDispatch(*app));
    executor.run();
    take(app->scheduler().dispatch());
    take(scene->prepare(*app, {40, 2}));
    const auto bounds = take(CellRect::from({}, {40, 2}));
    std::optional<LayoutSnapshot> layout;
    take(app->tree().frame([&] {
      framing = true;
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      layout = take(LayoutSnapshot::capture(app->tree(), app->root()));
      framing = false;
    }));
    take(scene->prepareViews(*app, *layout));
    take(scene->completedLayout(*app));
  }
  REQUIRE(rows.size() == 1);
  const auto readsBefore = boolean->reads;
  take(app->tree().frame([&] {
    framing = true;
    take(app->tree().read(box.node(), [](const CheckBox &control) {
      CHECK_FALSE(control.checked());
    }));
    framing = false;
  }));
  CHECK(boolean->reads == readsBefore);
  take(app->router().invoke(box.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(boolean->value);
  take(scene->focus(editor.node()));
  take(app->router().dispatch(TextInputEvent{U'X'}));
  take(scene->afterDispatch(*app));
  CHECK(endpoint->value.find('X') != std::pmr::string::npos);
  CHECK(endpoint->version == 2);
  take(scene->close());
  executor.run();
}

class LateChildTerminal final : public ApplicationTerminal {
public:
  explicit LateChildTerminal(Application &owner) : app(owner) {
  }

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (stop.stop_requested()) {
      return {TerminalStatus::STOPPED};
    }
    if (std::chrono::steady_clock::now() >= cutoff) {
      return {TerminalStatus::END_OF_INPUT};
    }
    std::this_thread::sleep_until(std::min(deadline, cutoff));
    return {TerminalStatus::DEADLINE};
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{40, 2};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    output += bytes;
    pending = bytes.size();
    if (output.find("accounted native caption") != std::string::npos) {
      app.requestStop();
    }
    return {};
  }

  TerminalWrite flush(TerminalDeadline, std::stop_token) override {
    return {TerminalStatus::PROGRESS, std::exchange(pending, 0), 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    closed = true;
    return {};
  }

  Application &app;
  TerminalDeadline cutoff = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::string output;
  std::size_t pending{};
  bool closed{};
};

TEST_CASE("first late native child starts the application owned pool without another input event") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto outer = ui::model(std::array{1});
  auto provider = std::make_shared<AccountedTasks>();
  auto native = ui::model<AccountedTask>(provider);
  take(ui::mount(*scene, ui::virtualRows(outer,
                                         [native](auto) {
                                           return ui::virtualRows(native, [](auto row) {
                                             return ui::label(row.value().caption);
                                           });
                                         },
                                         {.rowHeight = 2})));
  CHECK(app->background() == nullptr);
  CHECK_FALSE(scene->requiresBackgroundExecutor());
  LateChildTerminal terminal(*app);
  take(app->run(*scene, terminal));
  CHECK(terminal.output.find("accounted native caption") != std::string::npos);
  CHECK(terminal.closed);
  CHECK(app->background() == nullptr);
}

struct ReadyRowsTerminal final : ApplicationReadyTerminal {
  std::uint64_t time{};
  std::optional<InputEvent> input;
  std::string inputBytes;
  std::string pending;
  std::string output;
  std::size_t submissions{};
  bool closed{};

  SchedulerClock clock() noexcept {
    return {[](void *context) noexcept {
              return static_cast<ReadyRowsTerminal *>(context)->time;
            },
            this};
  }

  TerminalRead poll(std::stop_token) override {
    if (!inputBytes.empty()) {
      return {TerminalStatus::PROGRESS, std::exchange(inputBytes, {})};
    }
    TerminalRead result;
    if (input) {
      result.events.push_back(std::move(*input));
      input.reset();
    } else {
      result.status = TerminalStatus::DEADLINE;
    }
    return result;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{40, 4};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    if (!pending.empty()) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_PENDING});
    }
    pending = std::move(bytes);
    ++submissions;
    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    const auto count = pending.size();
    output += pending;
    pending.clear();
    return {TerminalStatus::PROGRESS, count, 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    closed = true;
    return {};
  }
};

struct ReadyRowsCleanup {
  Application &app;
  ManagedScene &scene;

  ~ReadyRowsCleanup() {
    app.requestStop();
    (void)scene.step();
  }
};

TEST_CASE("shared completion validation refuses row navigation through the production ready loop") {
  ReadyRowsTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::ON_COMPLETION});
  auto field = edit.field(&Task::caption);
  ui::Ref<LineEdit> editor;
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*scene, ui::vbox(ui::virtualRows(navigation,
                                                  [](auto row) {
                                                    return ui::label(row.value().caption);
                                                  })
                                    .reference(host),
                                  ui::lineEdit(editor, field, {.validate = [](std::string_view value) {
                                                 return !value.ends_with('x');
                                               }}))));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int step = 0; step != 5; ++step) {
      terminal.time += 20;
      take(scene->step());
    }
  };
  steps();
  take(app->router().setFocus(editor.node()));
  terminal.input = TextInputEvent{U'x'};
  steps();
  const auto original = take(field.snapshot());
  REQUIRE(original.draft);
  CHECK(original.draft->ends_with('x'));
  take(app->router().setFocus(host.node()));
  terminal.input = KeyEvent{InputKey::DOWN};
  steps();
  CHECK(take(navigation.currentRow()) == RowId{1, 1});
  take(app->tree().read(host.node(), [](const auto &widget) {
    CHECK(widget.position().current == RowId{1, 1});
    CHECK(widget.position().currentIndex == 0);
    CHECK(widget.position().firstIndex == 0);
    CHECK(widget.lastError() == TreeError::WOULD_BLOCK);
  }));
  CHECK(take(app->router().focused()) == host.node());
  const auto retained = take(field.snapshot());
  REQUIRE(retained.draft);
  CHECK(*retained.draft == *original.draft);
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("production row navigation retains focus through arrows wheel and refused Escape") {
  ReadyRowsTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}, Task{false, "D"},
                                    Task{false, "E"}, Task{false, "F"}, Task{false, "G"}, Task{false, "H"}});
  ui::Ref<Button> before;
  ui::Ref<Button> after;
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*scene, ui::vbox(ui::button("Before")
                                    .onClick([] {
                                    })
                                    .reference(before),
                                  ui::virtualRows(model,
                                                  [](auto row) {
                                                    return ui::hbox(ui::checkbox("", row.field(&Task::done)),
                                                                    ui::lineEdit(row.field(&Task::caption)));
                                                  },
                                                  {.editing = {.mode = EditMode::ON_COMPLETION}})
                                    .reference(host),
                                  ui::button("After")
                                    .onClick([] {
                                    })
                                    .reference(after))));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int step = 0; step != 5; ++step) {
      terminal.time += 20;
      take(scene->step());
    }
  };
  auto key = [&](InputKey value, InputModifiers modifiers = {}) {
    terminal.input = KeyEvent{.key = value, .modifiers = modifiers};
    steps();
  };
  auto position = [&] {
    VirtualRowsPosition value;
    take(app->tree().read(host.node(), [&](const auto &view) {
      value = view.position();
    }));
    return value;
  };
  steps();
  CHECK(take(app->router().focused()) == before.node());
  key(InputKey::TAB);
  CHECK(take(app->router().focused()) == host.node());
  for (int index = 0; index != 12; ++index) {
    key(InputKey::DOWN);
    CHECK(take(app->router().focused()) == host.node());
  }
  CHECK(position().currentIndex == 7);
  CHECK(position().firstIndex == 6);
  terminal.input = MouseInputEvent{8, 1, MouseButton::LEFT, MouseAction::PRESS};
  steps();
  CHECK(position().currentIndex == 6);
  CHECK(position().editing);
  key(InputKey::TAB);
  key(InputKey::ESCAPE);
  CHECK(take(app->router().focused()) == host.node());
  key(InputKey::DOWN);
  CHECK(position().currentIndex == 7);
  key(InputKey::ENTER);
  key(InputKey::TAB);
  const auto editor = take(app->router().focused());
  CHECK(editor != host.node());
  terminal.input = TextInputEvent{U'x'};
  steps();
  key(InputKey::ESCAPE);
  CHECK(take(app->router().focused()) == editor);
  CHECK(position().editing);
  take(app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.lastError() == TreeError::WOULD_BLOCK);
  }));
  key(InputKey::ENTER);
  key(InputKey::ESCAPE);
  CHECK(take(app->router().focused()) == host.node());
  key(InputKey::TAB);
  CHECK(take(app->router().focused()) == after.node());
  key(InputKey::TAB, {.shift = true});
  CHECK(take(app->router().focused()) == host.node());
  key(InputKey::ENTER);
  terminal.input = MouseInputEvent{2, 1, MouseButton::WHEEL_UP, MouseAction::SCROLL, {}, {}, 4.0};
  steps();
  CHECK(take(app->router().focused()) == host.node());
  CHECK(position().currentIndex == 7);
  CHECK(position().firstIndex == 2);
  key(InputKey::DOWN);
  CHECK(take(app->router().focused()) == host.node());
  CHECK(position().firstIndex == 6);
  const auto settled = terminal.submissions;
  steps();
  CHECK(terminal.submissions == settled);
}

TEST_CASE("production ready loop mounts rows and wakes for model changes without forced prepare") {
  ReadyRowsTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{Task{false, "Alpha"}, Task{false, "Beta"}, Task{false, "Gamma"}});
  ui::Ref<VirtualRows<Task>> host;
  std::vector<ui::RowContext<Task>> handles(0);
  take(ui::mount(*scene, ui::virtualRows(model,
                                         [&](auto row) {
                                           handles.push_back(row);
                                           return ui::hbox(ui::checkbox("", row.field(&Task::done)),
                                                           ui::label(row.field(&Task::caption)));
                                         })
                           .reference(host)
                           .height(4)
                           .framed("rounded", "Rows")));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int i = 0; i < 8; ++i) {
      terminal.time += 20;
      take(scene->step());
    }
  };
  steps();
  CHECK(terminal.output.find("Alpha") != std::string::npos);
  CHECK(terminal.submissions >= 2);
  const auto stable = terminal.submissions;
  steps();
  CHECK(terminal.submissions == stable);
  REQUIRE_FALSE(handles.empty());
  auto written = take(handles[0].field(&Task::done).set(true));
  CHECK(written == EditStatus::ACCEPTED);
  auto accepted = take(handles[0].field(&Task::done).snapshot());
  CHECK(accepted.status == EditStatus::ACCEPTED);
  CHECK(accepted.target.resource.identity.instance != 0);
  CHECK(accepted.target.resource.lineage != 0);
  CHECK(accepted.target.sourceSession.instance != 0);
  CHECK(*accepted.accepted);
  take(model.update({1, 1}, Task{true, "Changed"}));
  steps();
  CHECK(terminal.output.find("Changed") != std::string::npos);
}

TEST_CASE("production character edits retain focused row identity and live accepted caption") {
  ReadyRowsTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{Task{false, "A"}, Task{false, "B"}, Task{false, "C"}});
  std::vector<ui::Ref<LineEdit>> editors(0);
  std::size_t firstConstructions{};
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*scene, ui::virtualRows(model, [&](auto row) {
                           if (row.id() == RowId{1, 1}) {
                             ++firstConstructions;
                           }
                           ui::Ref<LineEdit> editor;
                           editors.push_back(editor);
                           return ui::hbox(ui::lineEdit(row.field(&Task::caption)).reference(editor).width(10),
                                           ui::label(row.field(&Task::caption)));
                         }).reference(host)));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int i = 0; i < 5; ++i) {
      terminal.time += 20;
      take(scene->step());
    }
  };
  steps();
  REQUIRE(editors.size() == 3);
  const auto editor = editors.front().node();
  take(scene->focus(editor));
  for (const auto character : {U'x', U'y', U'z'}) {
    terminal.input = TextInputEvent{character};
    steps();
    CHECK(take(app->router().focused()) == editor);
    CHECK(firstConstructions == 1);
  }
  CHECK(terminal.output.find("Axyz") != std::string::npos);
  const auto idle = terminal.submissions;
  steps();
  CHECK(terminal.submissions == idle);
}

TEST_CASE("production coalesced and fractional wheel detents preserve their magnitude") {
  ReadyRowsTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{0, 1, 2, 3, 4, 5, 6, 7});
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*scene, ui::virtualRows(model, [](auto row) {
                           return ui::label(std::to_string(row.value()));
                         }).reference(host)));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int i = 0; i < 5; ++i) {
      terminal.time += 20;
      take(scene->step());
    }
  };
  steps();
  auto first = [&] {
    std::uint64_t value{};
    take(app->tree().read(host.node(), [&](const auto &view) {
      value = view.firstRow();
    }));
    return value;
  };
  terminal.inputBytes = "\x1b[<65;1;1M\x1b[<65;1;1M";
  steps();
  CHECK(first() == 2);
  take(setVirtualRowsFirst(app->tree(), host.node(), 0));
  steps();
  terminal.input = MouseInputEvent{0, 0, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, {}, {}, 2.0};
  steps();
  CHECK(first() == 2);
  for (const auto delta : {0.25, 0.25, 0.5}) {
    terminal.input = MouseInputEvent{0, 0, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, {}, {}, delta};
    steps();
  }
  CHECK(first() == 3);
  terminal.input = MouseInputEvent{0, 0, MouseButton::WHEEL_UP, MouseAction::SCROLL, {}, {}, 2.0};
  steps();
  CHECK(first() == 1);
}

TEST_CASE("native production batches two raw SGR wheel events before row publication") {
  ControlledExecutor executor;
  ReadyRowsTerminal terminal;
  auto app = take(Application::create(ApplicationOptions{.backgroundExecutor = &executor}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto source = ui::model<ArenaTask>(std::make_shared<CursorTasks>());
  std::vector<ui::RowContext<ArenaTask>> rows(0);
  take(ui::mount(*scene, ui::virtualRows(source, [&](auto row) {
    rows.push_back(row);
    return ui::label("Record " + std::string(row.value().caption));
  })));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int i = 0; i < 8; ++i) {
      executor.run();
      terminal.time += 20;
      take(scene->step());
    }
  };
  constexpr int MAX_STARTUP_STEPS = 64;
  for (int attempt = 0; attempt < MAX_STARTUP_STEPS && terminal.output.find("Record 0") == std::string::npos;
       ++attempt) {
    executor.run();
    terminal.time += 20;
    take(scene->step());
  }
  REQUIRE(terminal.output.find("Record 0") != std::string::npos);
  REQUIRE(rows.size() == 4);
  terminal.inputBytes = "\x1b[<65;1;1M\x1b[<65;1;1M";
  steps();
  const auto active = [&](RowId id) {
    return std::ranges::any_of(rows, [&](const auto &row) {
      return row.id() == id && row.snapshot().has_value();
    });
  };
  CHECK_FALSE(active({2, 1}));
  CHECK(active({3, 1}));
  CHECK(active({6, 1}));
}

TEST_CASE("stable factory failure and hidden viewport do not request unbounded frames") {
  ReadyRowsTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{1, 2});
  std::size_t calls{};
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*scene, ui::virtualRows(model, [&](auto) -> ui::Description<Label> {
                           ++calls;
                           throw std::runtime_error("stable factory failure");
                         }).reference(host)));
  take(scene->start(terminal));
  ReadyRowsCleanup cleanup{*app, *scene};
  auto steps = [&] {
    for (int i = 0; i < 12; ++i) {
      terminal.time += 20;
      take(scene->step());
    }
  };
  steps();
  CHECK(calls == 1);
  const auto stable = terminal.submissions;
  steps();
  CHECK(calls == 1);
  CHECK(terminal.submissions == stable);
  take(app->tree().setVisible(host.node(), false));
  steps();
  const auto hidden = terminal.submissions;
  steps();
  CHECK(terminal.submissions == hidden);
  CHECK(calls == 1);
}

TEST_CASE("row action retains admitted data while callback removes its entire host") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "admitted"}});
  ui::Ref<VirtualRows<Task>> host;
  ui::Ref<Button> button;
  bool returned{};
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [&](auto row) {
                                   return ui::button("Remove host").reference(button).onClick([&, row] {
                                     auto lease = take(row.snapshot());
                                     take(fixture.scene->remove(host.node()));
                                     CHECK(lease.value().caption == "admitted");
                                     CHECK_FALSE(row.erase());
                                     returned = true;
                                   });
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(returned);
  fixture.settle();
}

TEST_CASE("fixed-height delegates contain real multiline compositions and reject overflow") {
  Fixture fixture;
  auto model = ui::model(std::array{Task{false, "Caption"}});
  ui::Ref<VirtualRows<Task>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(
                                   model,
                                   [](auto row) {
                                     return ui::vbox(ui::checkbox("Done", row.field(&Task::done)),
                                                     ui::label(row.value().caption));
                                   },
                                   VirtualRowsOptions{.rowHeight = 2})
                                   .reference(host)));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.mountedRows() == 1);
  }));
}

TEST_CASE("fixed row height reports composition overflow with a typed layout error") {
  Fixture fixture;
  auto model = ui::model(std::array{1});
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [](auto) {
    return ui::vbox(ui::label("top"), ui::label("bottom"));
  })));
  fixture.prepare();
  take(fixture.scene->afterDispatch(*fixture.app));
  const auto bounds = take(CellRect::from({}, {40, 2}));
  std::expected<void, TreeError> layout;
  take(fixture.app->tree().frame([&] {
    layout = layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds);
  }));
  REQUIRE_FALSE(layout);
  CHECK(layout.error() == TreeError::INVALID_GEOMETRY);
}

TEST_CASE("row binding admission failure preserves the previous complete window") {
  Fixture fixture;
  auto model = ui::model(std::array{0, 1, 2, 3});
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [](auto row) {
                                   return ui::button(std::to_string(row.value()))
                                     .onClick([] {
                                     })
                                     .setup([reject = row.value() >= 2](
                                              ManagedScene &, Node<Button> node) -> std::expected<void, ManagedError> {
                                       if (reject) {
                                         return std::unexpected(ui::detail::error(node, TreeError::WOULD_BLOCK));
                                       }
                                       return {};
                                     });
                                 }).reference(host)));
  fixture.settle();
  take(setVirtualRowsFirst(fixture.app->tree(), host.node(), 2));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.firstRow() == 0);
    CHECK(view.mountedRows() == 2);
    CHECK(view.lastError().has_value());
  }));
}

TEST_CASE("unqualified ordinary editors require explicit row edit participation") {
  Fixture fixture;
  auto model = ui::model(std::array{1, 2});
  ui::Ref<VirtualRows<int>> host;
  take(ui::mount(*fixture.scene, ui::virtualRows(model, [](auto) {
                                   return ui::lineEdit("unsafe local draft");
                                 }).reference(host)));
  fixture.settle();
  take(fixture.app->tree().read(host.node(), [](const auto &view) {
    CHECK(view.mountedRows() == 0);
    CHECK(view.lastError() == TreeError::INVALID_ARGUMENT);
  }));
}
} // namespace
