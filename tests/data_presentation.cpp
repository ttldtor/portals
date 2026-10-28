// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <cstdlib>
#include <iostream>
#include <new>
#include <portals/ui.hpp>
#include <portals/ui_scene_resource.hpp>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local std::size_t count{};
thread_local void (*hook)(void *){};
thread_local void *context{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (auto callback = std::exchange(allocationTest::hook, nullptr)) {
    callback(allocationTest::context);
  }
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }
  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }
  ++allocationTest::count;
  if (auto *result = std::malloc(size ? size : 1)) {
    return result;
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

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

using namespace portals;

namespace {
void check(bool value, std::string_view message, std::source_location at = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error(std::string(message) + " line " + std::to_string(at.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location at = std::source_location::current()) {
  if (!result) {
    if constexpr (std::same_as<E, TreeError>) {
      std::cerr << "tree=" << static_cast<int>(result.error()) << '\n';
    }
    if constexpr (std::same_as<E, ManagedError>) {
      std::cerr << "managed phase=" << static_cast<int>(result.error().phase);
      if (auto cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << " cause=" << static_cast<int>(*cause);
      }
      std::cerr << '\n';
    }
  }
  check(result.has_value(), "API failure", at);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Fixture {
  SchedulerTime now{};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ApplicationOptions options = {}) {
    app = take(Application::create(options, {[](void *value) noexcept {
                                               return *static_cast<SchedulerTime *>(value);
                                             },
                                             &now}));
    scene = take(ManagedScene::create(*app));
  }

  void prepare(CellSize size = {30, 6}) {
    take(scene->prepare(*app, size));
    const auto bounds = take(CellRect::from({}, size));
    bool stable{};
    for (int pass = 0; pass < 3; ++pass) {
      take(app->tree().consumeFrameRequest());
      std::optional<LayoutSnapshot> snapshot;
      take(app->tree().frame([&] {
        take(layoutTree(app->tree(), app->root(), bounds, bounds));
        snapshot.emplace(take(LayoutSnapshot::capture(
          app->tree(), app->root(),
          scene->usesGeometrySnapshot() ? LayoutSnapshotProfile::GEOMETRY : LayoutSnapshotProfile::DIMENSIONS)));
      }));
      take(scene->prepareViews(*app, *snapshot));
      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        stable = true;
        break;
      }
    }
    check(stable, "preparation did not settle in existing three passes");
    take(scene->completedLayout(*app));
  }

  SchedulerDispatch advance(SchedulerTime time) {
    now = time;
    auto result = take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
    return result;
  }

  DataPresentation value(Node<DataStateView> node) {
    DataPresentation result;
    take(app->tree().read(node, [&](const DataStateView &view) {
      result = view.presentation();
    }));
    return result;
  }
};

struct Page final : ModelSnapshot<int> {
  RangeState state;
  std::array<ModelRow<int>, 2> rows{{{{1, 1}, 1}, {{2, 1}, 2}}};

  explicit Page(RangeState value) : ModelSnapshot(7, 2), state(value) {
  }

  ModelRows<int> read(RowRange range) const override {
    if (state != RangeState::READY) {
      return {state, {}, {}};
    }
    return {state, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }
};

void mapping() {
  static_assert(dataPhase({}) == DataPhase::LOADING);
  static_assert(dataPhase({true, RangeState::READY, 0, 0, false}) == DataPhase::EMPTY);
  static_assert(dataPhase({true, RangeState::READY, 2, 0, true}) == DataPhase::REFRESHING);
  static_assert(dataPhase({true, RangeState::FAILED, 0, 7, false}) == DataPhase::FAILED);
  static_assert(dataPhase({true, RangeState::FAILED, 2, 7, true}) == DataPhase::LOADING);
  static_assert(!isDataPending(DataPhase::FAILED));
  check(!dataPhase({true, static_cast<RangeState>(99), 0, 0, false}), "invalid readiness accepted");
}

void ownedTimerAndLifecycle() {
  Fixture f;
  auto node = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{DataPhase::LOADING}, ui::label("content"))));
  check(f.scene->usesPreparedLayout(), "explicit-only scene did not opt into prepared layout");
  f.prepare();
  auto tick = f.advance(100);
  check(tick.timers == 1, "visible pending timer missing");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT, "spinner tick invalidated layout");
  take(setDataStateView(f.app->tree(), node, {DataPhase::READY, 2}));
  f.prepare();
  check(f.advance(500).timers == 0, "READY retained timer");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "READY redraws while idle");
  take(setDataStateView(f.app->tree(), node, {DataPhase::LOADING, 1}));
  check(f.value(node) == DataPresentation{DataPhase::READY, 2}, "stale generation replaced presentation");
  take(setDataStateView(f.app->tree(), node, {DataPhase::REFRESHING, 3}));
  f.prepare();
  take(f.app->tree().setVisible(node, false));
  take(f.app->tree().consumeFrameRequest());
  f.advance(600);
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "hidden due callback requested redraw");
  check(f.advance(1000).timers == 0, "hidden timer rearmed");
  take(f.app->tree().setVisible(node, true));
  f.prepare();
  check(f.advance(1100).timers == 1, "reshown pending did not arm");
  take(f.app->tree().remove(node));
  take(f.app->tree().consumeFrameRequest());
  f.advance(1200);
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "removed callback redrew");
  check(f.advance(1500).timers == 0, "removed callback rearmed");
}

void affinityAndBinding() {
  Fixture f;
  auto state = take(f.scene->state(DataPresentation{DataPhase::LOADING}));
  auto readonly = take(f.scene->derive(state, [](const DataPresentation &value) {
    return value;
  }));
  ui::NodeRef<DataStateView> writableRef;
  ui::NodeRef<DataStateView> readonlyRef;
  take(ui::mount(*f.scene, ui::column(ui::dataStateView(state, ui::label("content")).reference(writableRef),
                                      ui::dataStateView(readonly, ui::label("derived")).reference(readonlyRef))));
  const auto node = writableRef.node();
  const auto derived = readonlyRef.node();
  f.prepare();
  auto illegal = setDataStateView(f.app->tree(), node, {DataPhase::READY});
  check(!illegal && illegal.error() == TreeError::INVALID_ARGUMENT, "bound writable escape");
  take(state.set({DataPhase::READY, 2}));
  f.prepare();
  check(f.value(node).phase == DataPhase::READY, "bound update missing before paint");
  check(f.value(derived).phase == DataPhase::READY, "readonly source update missing before paint");
  auto readonlyWrite = setDataStateView(f.app->tree(), derived, {DataPhase::OFFLINE});
  check(!readonlyWrite && readonlyWrite.error() == TreeError::INVALID_ARGUMENT,
        "readonly source gained a writable escape");
  TreeError cause{};
  std::thread foreign([&] {
    auto result = setDataStateView(f.app->tree(), node, {DataPhase::FAILED});
    cause = result.error();
  });
  foreign.join();
  check(cause == TreeError::WRONG_THREAD, "display lost UI affinity");
}

void automaticSamePin() {
  Fixture f;
  auto model = take(f.scene->model<int>(std::make_shared<Page>(RangeState::READY)));
  auto node = take(ui::mount(*f.scene, ui::dataStateView(ui::virtualList(model, [](const ModelRow<int> &row) {
    return take(GraphemeText::from(std::to_string(row.value)));
  }))));
  f.prepare();
  check(f.value(node).phase == DataPhase::READY, "automatic ready mapping missing");
  check(f.advance(200).timers == 0, "automatic READY polls");
}

void automaticRetiredTextSource() {
  Fixture f;
  ui::Ref<LineEdit> input;
  ui::Ref<DataStateView> state;
  ui::Ref<VirtualList<TextSegment>> list;
  take(ui::mount(
    *f.scene,
    ui::vbox(
      ui::dataStateView(ui::virtualList(ui::words(input.draftText())).reference(list)).reference(state).height(3),
      ui::lineEdit(input, "one two"))));
  f.prepare();
  check(f.value(state.node()).phase == DataPhase::READY, "cold text model presentation missing");
  take(f.scene->remove(input.node()));
  f.prepare();
  check(f.value(state.node()).phase == DataPhase::READY, "retired text source lost its last prepared rows");
  take(f.app->tree().read(list.node(), [](const VirtualList<TextSegment> &view) {
    check(view.dataStatus().prepared && view.dataStatus().rowCount == 2,
          "retired text source discarded its owning prepared facts");
  }));
  check(f.advance(300).timers == 0, "retired ready text source starts polling");
}

void coldTextStateRollback() {
  Fixture f;
  ui::Ref<LineEdit> input;
  ui::Ref<DataStateView> state;
  auto rejected = ui::mount(
    *f.scene,
    ui::vbox(ui::dataStateView(ui::virtualList(ui::words(input.draftText(), TextSplitOptions{.maxTokens = 1})))
               .reference(state),
             ui::lineEdit(input, "one two")));
  const auto *cause = rejected ? nullptr : std::get_if<TreeError>(&rejected.error().cause);

  check(cause && *cause == TreeError::NODE_LIMIT, "cold transform failure lost its original error");
  check(input.node() == Node<LineEdit>{} && state.node() == Node<DataStateView>{},
        "failed cold presentation mount published a reference");
  check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "failed cold presentation retained a subtree");
  check(!f.scene->usesPreparedLayout(), "failed cold presentation retained a prepared registration");
  f.prepare();
  check(f.advance(300).timers == 0, "failed cold presentation retained an animation timer");
  take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("recovered"))));
  f.prepare();
}

void scrollClipStopsAnimation() {
  Fixture f;
  ui::Ref<ScrollArea> area;
  ui::Ref<DataStateView> state;
  ScrollAreaOptions options;
  options.horizontal = options.vertical = ScrollBarPolicy::NEVER;
  take(ui::mount(
    *f.scene,
    ui::scrollArea(
      area, ui::dataStateView(DataPresentation{DataPhase::LOADING}, ui::label("body")).reference(state).height(10),
      options)));
  f.prepare();
  check(f.scene->usesGeometrySnapshot(), "scroll composition lost owning geometry preparation");
  check(f.advance(100).timers == 1, "visible scroll status did not animate");
  take(area.setOffset(ScrollAxis::VERTICAL, {1, 0}));
  f.prepare();
  check(f.advance(200).timers == 0, "clipped status strip retained an animation timer");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "clipped status strip redrew while idle");
  take(area.setOffset(ScrollAxis::VERTICAL, {0, 0}));
  f.prepare();
  check(f.advance(300).timers == 1, "revealed status strip did not resume animation");
  take(state.display({DataPhase::READY, 2}));
  f.prepare();
  check(f.advance(600).timers == 0, "ready scroll composition retained animation");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "ready scroll composition redrew while idle");
}

void rollback() {
  Fixture f;
  ui::NodeRef<DataStateView> ref;
  DataStateConfig invalid;
  invalid.spinnerInterval = 0;
  auto failed = ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body"), invalid).reference(ref));
  check(!failed && ref.node() == Node<DataStateView>{}, "invalid mount published Ref");
  check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "invalid mount retained nodes");
  take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body"))));
  f.prepare();
}

void retryAdmissionAndRemoval() {
  Fixture f;
  Node<DataStateView> root;
  int calls{};
  auto capture = std::make_shared<int>(42);
  std::weak_ptr<int> weak = capture;
  DataStateConfig config;
  config.retry = [&, capture]() -> std::expected<void, TreeError> {
    ++calls;
    take(f.scene->remove(root));
    check(!weak.expired(), "self-removal destroyed admitted retry capture");
    return {};
  };
  auto recipe = ui::dataStateView(DataPresentation{DataPhase::FAILED}, ui::label("body"), config);
  config.retry = {};
  capture.reset();
  root = take(ui::mount(*f.scene, std::move(recipe)));
  f.prepare();
  auto children = take(f.app->tree().inspect(root)).children;
  auto strip = take(f.app->tree().inspect(children.front())).children;
  auto button = strip.back();
  take(f.app->router().invoke(button, {Action::ACTIVATE}));
  check(calls == 1 && weak.expired(), "retired retry capture not reclaimed after return");
  auto repeated = f.app->router().invoke(button, {Action::ACTIVATE});
  check(!repeated && repeated.error() == TreeError::STALE_NODE, "retired retry admitted again");
}

void disabledZeroAndStop() {
  Fixture f;
  auto root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{DataPhase::LOADING}, ui::label("body"))));
  take(f.app->tree().setEnabled(root, false));
  f.prepare();
  check(f.advance(100).timers == 0, "disabled animation runs");
  take(f.app->tree().setEnabled(root, true));
  f.prepare({0, 0});
  check(f.advance(200).timers == 0, "zero viewport animation runs");
  f.prepare();
  take(f.scene->close());
  take(f.app->tree().consumeFrameRequest());
  check(f.advance(300).timers == 0, "closed scene animation runs");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "closed scene redraws");
}

void rejectedNotification() {
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  Fixture f(options);
  auto state = take(f.scene->state(DataPresentation{DataPhase::LOADING}));
  take(f.scene->observe(state, [](const DataPresentation &) {
  }));
  auto root = take(ui::mount(*f.scene, ui::dataStateView(state, ui::label("body"))));
  f.prepare();
  auto changed = take(state.set({DataPhase::READY, 2}));
  check(changed.changed && changed.rejected == 1, "notification rejection fixture missed full queue");
  f.prepare();
  check(f.value(root) == DataPresentation{DataPhase::READY, 2}, "rejected notification was not recovered before paint");
  f.advance(500);
  f.prepare();
  check(f.value(root) == DataPresentation{DataPhase::READY, 2}, "late old notification replaced recovered value");
}

void allocationRollback() {
  std::size_t allocations{};
  {
    Fixture f;
    ui::NodeRef<DataStateView> reference;
    auto recipe = ui::dataStateView(DataPresentation{DataPhase::LOADING}, ui::label("body")).reference(reference);
    const auto before = allocationTest::count;
    take(ui::mount(*f.scene, std::move(recipe)));
    allocations = allocationTest::count - before;
  }
  check(allocations > 0, "mount allocation calibration empty");
  for (std::size_t failAt = 0; failAt < allocations; ++failAt) {
    Fixture f;
    ui::NodeRef<DataStateView> reference;
    auto recipe = ui::dataStateView(DataPresentation{DataPhase::LOADING}, ui::label("body")).reference(reference);
    allocationTest::remaining = static_cast<std::ptrdiff_t>(failAt);
    auto mounted = ui::mount(*f.scene, std::move(recipe));
    allocationTest::remaining = -1;
    if (mounted) {
      take(f.scene->remove(*mounted));
    } else {
      const auto cause = std::get_if<TreeError>(&mounted.error().cause);
      check(cause && *cause == TreeError::NO_MEMORY, "allocation failure lost typed error");
      check(reference.node() == Node<DataStateView>{}, "failed mount published Ref");
    }
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "allocation rollback leaked subtree");
    take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("retry"))));
    f.prepare();
  }
  std::cout << "mount allocation points=" << allocations << '\n';
}

struct LargePage final : ModelSnapshot<int> {
  RangeState state;
  std::array<ModelRow<int>, 64> rows;

  explicit LargePage(RangeState value, std::uint64_t revision = 7) : ModelSnapshot(revision, 64), state(value) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      rows[i] = {{i + 1, 1}, static_cast<int>(i)};
    }
  }

  ModelRows<int> read(RowRange range) const override {
    if (state != RangeState::READY) {
      return {state, {}, {}};
    }
    return {state, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }
};

template <bool TABLE> void delayedRetryAndCancellation() {
  using View = std::conditional_t<TABLE, VirtualTable<int>, VirtualList<int>>;
  for (int negative = -1; negative < 4; ++negative) {
    Fixture f;
    auto model = take(f.scene->model<int>(std::make_shared<LargePage>(RangeState::FAILED)));
    std::optional<ModelRequest<int>> work;
    auto prepared = [&](const ModelFrame<int> &,
                        const std::optional<ModelRequest<int>> &request) -> std::expected<void, TreeError> {
      if (request) {
        work = *request;
      }
      return {};
    };
    ui::NodeRef<View> view;
    auto content = [&] {
      if constexpr (TABLE) {
        std::array<TableColumn<int>, 1> columns{{{{1, 1}, take(GraphemeText::from("Value")), 8, [](const int &value) {
                                                    return take(GraphemeText::from(std::to_string(value)));
                                                  }}}};
        return ui::virtualTable(model, std::span<const TableColumn<int>>(columns), {}, {}, prepared).reference(view);
      } else {
        return ui::virtualList(
                 model,
                 [](const ModelRow<int> &row) {
                   return take(GraphemeText::from(std::to_string(row.value)));
                 },
                 {}, {}, prepared)
          .reference(view);
      }
    }();
    int retries{};
    DataStateConfig config;
    config.retry = [&]() -> std::expected<void, TreeError> {
      ++retries;
      if constexpr (TABLE) {
        return retryVirtualTable(f.app->tree(), view.node());
      } else {
        return retryVirtualList(f.app->tree(), view.node());
      }
    };
    auto root = take(ui::mount(*f.scene, ui::dataStateView(std::move(content), config)));
    config.retry = {};
    f.prepare();
    check(f.value(root).phase == DataPhase::FAILED && !work, "FAILED base state/request wrong");
    const auto strip = take(f.app->tree().inspect(root)).children.front();
    const auto retry = take(f.app->tree().inspect(strip)).children.back();
    take(f.app->router().setFocus(view.node()));
    check(take(f.app->router().focused()) == view.node(), "error phase stole body focus");
    take(f.app->router().setFocus(retry));
    take(f.app->router().invoke(retry, {Action::ACTIVATE}));
    take(f.app->router().invoke(retry, {Action::ACTIVATE}));
    check(retries == 1, "repeated activation started another pending job");
    f.prepare();
    check(take(f.app->router().focused()) == view.node(), "hidden retry focus was not repaired in the same frame");
    check(work.has_value() && !work->stopRequested(), "explicit FAILED retry missing");
    auto active = *work;
    check(f.value(root).phase == DataPhase::LOADING, "pending retry still presents FAILED");
    f.advance(100);
    f.prepare();
    check(!active.stopRequested(), "PAINT preparation cancelled delayed retry");
    if (negative == -1) {
      f.now = 250;
      take(active.complete(std::make_shared<LargePage>(RangeState::READY)));
      f.advance(250);
      f.prepare();
      check(f.value(root).phase == DataPhase::READY, "delayed valid completion not displayed");
      continue;
    }
    if (negative == 0) {
      take(f.app->tree().setVisible(root, false));
      f.prepare();
    }
    if (negative == 1) {
      take(f.scene->remove(root));
    }
    if (negative == 2) {
      take(take(model.publisher()).publish(std::make_shared<LargePage>(RangeState::FAILED, 8)));
      f.advance(150);
      f.prepare();
    }
    if (negative == 3) {
      if constexpr (TABLE) {
        take(setVirtualTableFirst(f.app->tree(), view.node(), 32));
      } else {
        take(setVirtualListFirst(f.app->tree(), view.node(), 32));
      }
      f.prepare();
    }
    check(active.stopRequested(), "obsolete FAILED retry work was retained");
    auto late = active.complete(std::make_shared<LargePage>(RangeState::READY));
    check(!late && late.error() == SchedulerError::CLOSED, "cancelled retry accepted late completion");
  }
}

template <bool TABLE> void refreshingKeepsReadyRows() {
  Fixture f;
  auto model = take(f.scene->model<int>(std::make_shared<LargePage>(RangeState::READY)));
  std::optional<ModelRequest<int>> active;
  auto prepared = [&](const ModelFrame<int> &,
                      const std::optional<ModelRequest<int>> &request) -> std::expected<void, TreeError> {
    if (request) {
      active = *request;
    }
    return {};
  };
  using View = std::conditional_t<TABLE, VirtualTable<int>, VirtualList<int>>;
  ui::NodeRef<View> view;
  auto content = [&] {
    if constexpr (TABLE) {
      std::array<TableColumn<int>, 1> columns{{{{1, 1}, take(GraphemeText::from("Value")), 8, [](const int &value) {
                                                  return take(GraphemeText::from(std::to_string(value)));
                                                }}}};
      return ui::virtualTable(model, std::span<const TableColumn<int>>(columns), {}, {}, prepared).reference(view);
    } else {
      return ui::virtualList(
               model,
               [](const ModelRow<int> &row) {
                 return take(GraphemeText::from(std::to_string(row.value)));
               },
               {}, {}, prepared)
        .reference(view);
    }
  }();
  auto root = take(ui::mount(*f.scene, ui::dataStateView(std::move(content))));
  f.prepare();
  if constexpr (TABLE) {
    take(retryVirtualTable(f.app->tree(), view.node()));
  } else {
    take(retryVirtualList(f.app->tree(), view.node()));
  }
  f.prepare();
  check(active && f.value(root).phase == DataPhase::REFRESHING, "READY retry lacks last-good presentation");
  f.advance(100);
  f.prepare();
  DataRangeStatus status;
  take(f.app->tree().read(view.node(), [&](const View &value) {
    status = value.dataStatus();
  }));
  check(status.state == RangeState::READY && status.requestPending && f.value(root).phase == DataPhase::REFRESHING,
        "next pin discarded current-revision last-good rows");
  take(active->complete(std::make_shared<LargePage>(RangeState::READY)));
  f.advance(250);
  f.prepare();
  check(f.value(root).phase == DataPhase::READY, "refresh completion did not retire pending presentation");
}

void runtimeAllocationStaging() {
  std::size_t allocations{};
  {
    Fixture f;
    auto root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body"))));
    f.prepare();
    const auto before = allocationTest::count;
    take(setDataStateView(f.app->tree(), root, {DataPhase::LOADING}));
    allocations = allocationTest::count - before;
  }
  for (std::size_t failAt = 0; failAt < allocations; ++failAt) {
    Fixture f;
    auto root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body"))));
    f.prepare();
    allocationTest::remaining = static_cast<std::ptrdiff_t>(failAt);
    auto updated = setDataStateView(f.app->tree(), root, {DataPhase::LOADING});
    allocationTest::remaining = -1;
    check(!updated && updated.error() == TreeError::NO_MEMORY, "runtime allocation failure lost cause");
    check(f.value(root).phase == DataPhase::READY, "failed staging published partial phase");
    take(setDataStateView(f.app->tree(), root, {DataPhase::LOADING}));
    f.prepare();
    check(f.advance(100).timers == 1, "failed staging leaked timer budget or poisoned retry");
  }
  std::cout << "runtime allocation points=" << allocations << '\n';
}

struct IndicatorPaint final : PaintContext {
  std::size_t indicators{};
  std::optional<RgbColor> color;
  std::string glyph;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *text = std::get_if<TextCommand>(&command)) {
        const auto bytes = text->text.bytes();
        if (bytes == "|" || bytes == "/" || bytes == "-" || bytes == "\\") {
          ++indicators;
          color = text->style.foreground;
          glyph = bytes;
          check(text->text.graphemes().size() == 1 && text->text.measure() == 1,
                "spinner frame is not one cell/grapheme");
        }
      }
    }
    return {};
  }
};

void indicatorThemeAndEqual() {
  Fixture f;
  auto root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{DataPhase::LOADING}, ui::label("body"))));
  take(f.scene->theme(take(parsePss(".data-spinner.loading { foreground: #123456; }"))));
  f.prepare();
  const auto strip = take(f.app->tree().inspect(root)).children.front();
  const auto indicator = take(f.app->tree().inspect(strip)).children.front();
  std::string previousGlyph;
  for (SchedulerTime time = 100; time <= 400; time += 100) {
    check(f.advance(time).timers == 1, "visible animation missed a frame");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT, "animation invalidated geometry");
    IndicatorPaint painted;
    take(f.app->tree().frame([&] {
      take(f.app->tree().paint(indicator, painted));
    }));
    check(painted.indicators == 1 && painted.color == RgbColor{0x12, 0x34, 0x56}, "indicator bypassed resolved theme");
    check(painted.glyph != previousGlyph, "timer redrew an unchanged spinner glyph");
    previousGlyph = painted.glyph;
  }
  take(setDataStateView(f.app->tree(), root, {DataPhase::READY}));
  f.prepare();
  const auto before = allocationTest::count;
  take(setDataStateView(f.app->tree(), root, {DataPhase::READY}));
  check(allocationTest::count == before, "equal state allocated");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "equal state redrew");
}

void originalTechnicalError() {
  Fixture f;
  auto model = take(f.scene->model<int>(std::make_shared<Page>(RangeState::PENDING)));
  ui::NodeRef<VirtualList<int>> body;
  auto root = take(ui::mount(
    *f.scene, ui::dataStateView(ui::virtualList(
                                  model,
                                  [](const ModelRow<int> &) {
                                    return take(GraphemeText::from("body"));
                                  },
                                  {}, {},
                                  [](const ModelFrame<int> &,
                                     const std::optional<ModelRequest<int>> &) -> std::expected<void, TreeError> {
                                    return std::unexpected(TreeError::NODE_LIMIT);
                                  })
                                  .reference(body))));
  take(f.scene->prepare(*f.app, {30, 6}));
  const auto bounds = take(CellRect::from({}, CellSize{30, 6}));
  std::optional<LayoutSnapshot> snapshot;
  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    snapshot.emplace(take(LayoutSnapshot::capture(f.app->tree(), f.app->root())));
  }));
  auto failed = f.scene->prepareViews(*f.app, *snapshot);
  check(!failed && failed.error() == TreeError::NODE_LIMIT, "wrapper masked provider technical error");
  const auto history = take(f.scene->diagnostics());
  const auto &error = history.errors.back().error;
  check(error.phase == ManagedPhase::PREPARE && error.node == body.node().id() &&
          std::get<TreeError>(error.cause) == TreeError::NODE_LIMIT,
        "wrapper lost original failure phase/target/cause");
  check(f.value(root).phase == DataPhase::LOADING, "technical error became a domain failure phase");
}

void allocationReentryRetiresIdentity() {
  Fixture f;
  auto root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body"))));
  f.prepare();

  struct Removal {
    Fixture &fixture;
    Node<DataStateView> root;
    bool called{};
  } removal{f, root};

  allocationTest::context = &removal;
  allocationTest::hook = [](void *value) {
    auto &entered = *static_cast<Removal *>(value);
    entered.called = true;
    take(entered.fixture.scene->remove(entered.root));
  };
  // The first target-copy allocation occurs before scheduler registration, outside its mutex.
  auto updated = setDataStateView(f.app->tree(), root, {DataPhase::LOADING});
  allocationTest::hook = nullptr;
  allocationTest::context = nullptr;
  check(removal.called && !updated &&
          (updated.error() == TreeError::STALE_NODE || updated.error() == TreeError::CLOSED),
        "post-allocation publication ignored retired identity");
  take(f.app->tree().consumeFrameRequest());
  check(f.advance(500).timers == 0, "retired identity published a timer");
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "retired publication requested redraw");
}

void modelApplicationFailureIsTechnical() {
  Fixture f;
  auto trigger = take(f.scene->state(0));
  take(f.scene->observe(trigger, [](const int &value) {
    if (value == 1) {
      allocationTest::remaining = 0;
    }
  }));
  auto model = take(f.scene->model<int>(std::make_shared<LargePage>(RangeState::READY)));
  auto root = take(ui::mount(*f.scene, ui::dataStateView(ui::virtualList(model, [](const ModelRow<int> &row) {
    return take(GraphemeText::from(std::to_string(row.value)));
  }))));
  f.prepare();
  auto publisher = take(model.publisher());
  auto replacement = std::make_shared<LargePage>(RangeState::READY, 8);
  take(trigger.set(1));
  take(publisher.publish(replacement));
  auto dispatched = f.app->scheduler().dispatch();
  allocationTest::remaining = -1;
  take(dispatched);
  take(f.scene->prepare(*f.app, {30, 6}));
  const auto bounds = take(CellRect::from({}, CellSize{30, 6}));
  std::optional<LayoutSnapshot> snapshot;
  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    snapshot.emplace(take(LayoutSnapshot::capture(f.app->tree(), f.app->root())));
  }));
  auto failed = f.scene->prepareViews(*f.app, *snapshot);
  check(!failed && failed.error() == TreeError::NO_MEMORY,
        "queued UI model failure became a domain phase or pending loop");
  auto history = take(f.scene->diagnostics());
  check(history.errors.back().error.phase == ManagedPhase::PREPARE &&
          std::get<TreeError>(history.errors.back().error.cause) == TreeError::NO_MEMORY,
        "sticky model failure lost phase/cause");
  check(f.value(root).phase == DataPhase::READY, "technical model failure replaced last-good presentation");
}

void publicRefDisplayAndValidation() {
  Fixture f;
  ui::Ref<DataStateView> reference;
  auto root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body")).reference(reference)));
  take(reference.display({DataPhase::OFFLINE}));
  check(f.value(root).phase == DataPhase::OFFLINE, "checked Ref display did not reach configured composite");
  auto failed = reference.display({static_cast<DataPhase>(99)});
  check(!failed && failed.error() == TreeError::INVALID_ARGUMENT, "Ref invalid phase lost cause");
  auto errors = take(f.scene->diagnostics()).errors;
  check(errors.size() == 1 && errors.front().error.node == root.id() && errors.front().error.operation &&
          std::string_view(errors.front().error.operation) == "dataState.display",
        "Ref failure lost target or was reported more than once");
  take(f.scene->clearErrors());
  f.prepare();
  check(f.advance(100).timers == 0, "OFFLINE animates");
}

void incompleteMountDoesNotPublish() {
  Fixture f;
  ui::NodeRef<DataStateView> reference;
  int setupCalls{};
  auto recipe = ui::dataStateView(DataPresentation{}, ui::label("body"))
                  .setup([&](ManagedScene &scene, Node<DataStateView> root) -> std::expected<void, ManagedError> {
                    ++setupCalls;
                    auto premature = setDataStateView(f.app->tree(), root, {DataPhase::LOADING});
                    check(!premature && premature.error() == TreeError::INVALID_ARGUMENT,
                          "incomplete composite accepted presentation write");
                    auto prepared = scene.prepare(*f.app, {30, 6});
                    check(!prepared && prepared.error() == TreeError::WOULD_BLOCK,
                          "incomplete own-value mount entered preparation");
                    check(reference.node() == Node<DataStateView>{}, "partial composite published Ref");
                    return {};
                  })
                  .reference(reference);
  auto root = take(ui::mount(*f.scene, std::move(recipe)));
  check(setupCalls == 1 && reference.node() == root && f.value(root).phase == DataPhase::READY,
        "cold guard changed successful initial publication");
  f.prepare();
  check(f.advance(100).timers == 0, "rejected cold write started a timer");
}

struct PresentationImage {
  DataPresentation value;
  std::string caption;
  std::array<bool, 5> visible;
  std::array<std::optional<ComputedStyle>, 5> styles;
  bool operator==(const PresentationImage &) const = default;
};

struct BodyRemoval {
  Fixture &fixture;
  Node<Label> body;
  std::size_t delay{};
  bool called{};
};

void removeBodyOnAllocation(void *context) {
  auto &entered = *static_cast<BodyRemoval *>(context);
  if (entered.delay) {
    --entered.delay;
    allocationTest::hook = removeBodyOnAllocation;
    return;
  }
  entered.called = true;
  take(entered.fixture.scene->remove(entered.body));
  take(entered.fixture.app->tree().consumeFrameRequest());
}

void bodyRemovalAtomicity() {
  const auto sheet = take(parsePss(".ready { foreground: #123456; } .loading { foreground: #654321; }"));
  const auto exercise = [&](bool during, bool disabled, std::size_t delay, bool calibrate) -> std::size_t {
    ApplicationOptions options;
    options.scheduler.subscription_capacity = 1;
    Fixture f(options);
    ui::Ref<Label> body;
    int retries{};
    DataStateConfig config;
    config.contentPolicy = DataContentPolicy::REPLACE;
    config.retry = [&]() -> std::expected<void, TreeError> {
      ++retries;
      return {};
    };
    auto root =
      take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body").reference(body), config)));
    config.retry = {};
    take(f.scene->theme(sheet));
    if (disabled) {
      take(f.app->tree().setEnabled(root, false));
    }
    f.prepare();
    const auto strip = take(f.app->tree().inspect(root)).children.front();
    const auto children = take(f.app->tree().inspect(strip)).children;
    const auto indicator = children[0];
    const auto message = children[1];
    const auto retry = children[2];
    const std::array<Node<Widget>, 5> nodes{root, strip, indicator, message, retry};
    const auto image = [&] {
      PresentationImage result;
      result.value = f.value(root);
      take(f.app->tree().read(message, [&](const Widget &widget) {
        result.caption = dynamic_cast<const Label &>(widget).text().bytes();
      }));
      for (std::size_t i = 0; i < nodes.size(); ++i) {
        result.visible[i] = take(f.app->tree().inspect(nodes[i])).visible;
        take(f.app->tree().read(nodes[i], [&](const Widget &widget) {
          result.styles[i] =
            widget.computedStyle() ? std::optional<ComputedStyle>(widget.computedStyle()->flatten()) : std::nullopt;
        }));
      }
      return result;
    };
    const auto before = image();
    if (calibrate) {
      const auto count = allocationTest::count;
      take(setDataStateView(f.app->tree(), root, {DataPhase::LOADING}));
      return allocationTest::count - count;
    }
    BodyRemoval removal{f, body.node(), delay};
    if (during) {
      allocationTest::context = &removal;
      allocationTest::hook = removeBodyOnAllocation;
    } else {
      take(f.scene->remove(body.node()));
      take(f.app->tree().consumeFrameRequest());
    }
    const auto updated = setDataStateView(f.app->tree(), root, {DataPhase::LOADING});
    allocationTest::hook = nullptr;
    allocationTest::context = nullptr;
    const auto invalidation = take(f.app->tree().consumeFrameRequest());
    const auto after = image();
    if (!disabled) {
      std::cout << "body-removal " << (during ? "allocation" : "before")
                << " before phase=" << static_cast<int>(before.value.phase) << " caption='" << before.caption
                << "' indicator=" << before.visible[2] << " retry=" << before.visible[4]
                << " after phase=" << static_cast<int>(after.value.phase) << " caption='" << after.caption
                << "' indicator=" << after.visible[2] << " retry=" << after.visible[4]
                << " invalidation=" << static_cast<int>(invalidation) << '\n';
    }
    check(!during || removal.called, "body-removal allocation admission was not reached");
    check(!updated && updated.error() == TreeError::STALE_NODE, "removed body did not preserve typed refusal");
    check(after == before, "rejected body update partially published presentation/visibility/style");
    check(invalidation == Invalidation::NONE, "rejected body update added partial invalidation");
    check(!f.app->tree().inspect(body.node()), "presenter restored a removed user child");
    // Flush metadata through the existing theme path: a wrongly published phase class must also be observable.
    take(f.scene->theme(sheet));
    check(image() == before, "rejected update published a phase class into style metadata");
    auto equal = setDataStateView(f.app->tree(), root, {});
    check(!equal && equal.error() == TreeError::STALE_NODE, "equal shortcut admitted an incomplete composite");
    Subscription probe;
    take(f.app->tree().withScope(root, [&](const UiScope &scope) {
      probe = take(f.app->scheduler().at(scope, 2000, [] {
      }));
    }));
    probe = {};
    take(f.app->tree().consumeFrameRequest());
    check(f.advance(1000).timers == 0 && retries == 0, "rejected update retained timer work or invoked retry");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "rejected update caused later redraw");
    return 0;
  };
  exercise(false, false, 0, false);
  exercise(true, false, 0, false);
  // Disabled layout keeps registration out of this sweep, so every text/target allocation can safely reenter UI.
  const auto allocations = exercise(false, true, 0, true);
  for (std::size_t i = 0; i < allocations; ++i) {
    exercise(true, true, i, false);
  }
  std::cout << "body-removal materialization points=" << allocations << '\n';
}

Node<DataStateView> checkedDataState(Fixture &f, Node<Widget> node) {
  ui::Ref<DataStateView> reference;

  take(ui::detail::NamedAccess::publish(reference, *f.scene, node));

  return reference.node();
}

std::string statusCaption(Fixture &f, Node<DataStateView> node) {
  const auto strip = take(f.app->tree().inspect(node)).children.front();
  const auto message = take(f.app->tree().inspect(strip)).children[1];
  std::string result;

  take(f.app->tree().read(message, [&](const Widget &widget) {
    result = dynamic_cast<const Label &>(widget).text().bytes();
  }));

  return result;
}

void scalarAndLabelParity() {
  for (int path = 0; path < 5; ++path) {
    Fixture f;
    ui::Ref<Label> body;
    Node<DataStateView> root;
    int resolutions{};
    DataStateConfig actual;
    const ui::TextResolver resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      if (key == "busy") {
        return "Working";
      }
      if (key == "failure") {
        return "Oops";
      }
      throw std::runtime_error("unused retry label was resolved");
    };
    const ui::ControlDataStateLabelSettings labels{.loading = i18n("busy"),
                                                   .empty = text("Nada"),
                                                   .error = i18n("failure"),
                                                   .offline = text("Disconnected"),
                                                   .refreshing = text("Again"),
                                                   .retry = i18n("unused retry")};
    ui::ControlSettings patch{.showStatus = true,
                              .statusRows = 2,
                              .contentPolicy = DataContentPolicy::REPLACE,
                              .spinnerInterval = 200,
                              .labels = labels};

    actual.resolver = resolver;
    actual.statusRows = 3;
    actual.spinnerInterval = 999;
    if (path == 0) {
      actual.showStatus = true;
      actual.statusRows = 2;
      actual.contentPolicy = DataContentPolicy::REPLACE;
      actual.spinnerInterval = 200;
      actual.labels = {.loading = i18n("busy"),
                       .empty = text("Nada"),
                       .error = i18n("failure"),
                       .offline = text("Disconnected"),
                       .refreshing = text("Again"),
                       .retry = i18n("unused retry")};
      root =
        take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body").reference(body), actual)));
    } else if (path == 1) {
      root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body").reference(body), actual)
                                        .showStatus()
                                        .statusRows(2)
                                        .contentPolicy(DataContentPolicy::REPLACE)
                                        .spinnerInterval(200)
                                        .labels(labels)));
    } else if (path == 2) {
      root = take(ui::mount(*f.scene, ui::dataStateView(DataPresentation{}, ui::label("body").reference(body), actual) |
                                        ui::showStatus() | ui::statusRows(2) |
                                        ui::contentPolicy(DataContentPolicy::REPLACE) | ui::spinnerInterval(200) |
                                        ui::labels(labels)));
    } else if (path == 3) {
      ui::ControlContext context;

      context.defaults("DataStateView", patch).define("Result", "DataStateView");
      root = take(ui::mount(
        *f.scene, ui::dataStateView(DataPresentation{}, ui::label("body").reference(body), actual).variant("Result"),
        context));
    } else {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.resolver = resolver;
      take(options.registry.add<Label>("Body", [&](ui::SceneResourceArguments) {
        return ui::label("body").reference(body);
      }));
      const auto mounted = take(ui::mount(*f.scene, ui::resource(R"(
        definitions{Result=DataStateView{showStatus=true contentPolicy=REPLACE labels{loading=i18n("busy") error=i18n("failure")}}}
        defaults{DataStateView{statusRows=2 spinnerInterval=200 labels{empty="Nada" offline="Disconnected" refreshing="Again" retry=i18n("unused retry")}}}
        scene{Result{labels={refreshing="Again"} Body{}}}
      )",
                                                                 options)));

      root = checkedDataState(f, mounted);
    }
    check(f.value(root) == DataPresentation{},
          "resource fallback must use explicit READY/generation1 without inference");
    check(resolutions == 2, "sparse label expressions were duplicated or unused retry caption was resolved");
    take(setDataStateView(f.app->tree(), root, {DataPhase::LOADING}));
    f.prepare();
    const auto strip = take(f.app->tree().inspect(root)).children.front();

    check(statusCaption(f, root) == "Working", "loading label did not match receiving native configuration");
    check(take(f.scene->geometry(strip)).local.size().height == 2, "statusRows did not reach real strip geometry");
    check(!take(f.app->tree().inspect(body.node())).visible,
          "REPLACE policy failed to hide the independently owned body");
    check(f.advance(199).timers == 0 && f.advance(200).timers == 1, "spinnerInterval did not retain native cadence");
    for (const auto &[phase, text] :
         std::array<std::pair<DataPhase, std::string_view>, 4>{{{DataPhase::EMPTY, "Nada"},
                                                                {DataPhase::FAILED, "Oops"},
                                                                {DataPhase::OFFLINE, "Disconnected"},
                                                                {DataPhase::REFRESHING, "Again"}}}) {
      take(setDataStateView(f.app->tree(), root, {phase}));
      f.prepare();
      check(statusCaption(f, root) == text, "sparse status label paths diverged from native options");
    }
    take(f.app->tree().setVisible(root, false));
    take(f.app->tree().consumeFrameRequest());
    f.advance(600);
    check(f.advance(1000).timers == 0 && take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE,
          "hidden scalar status retained timer/redraw work");
    take(f.app->tree().setVisible(root, true));
    f.prepare();
    check(f.advance(1200).timers == 1, "reshown scalar status lost native timer cadence");
    take(setDataStateView(f.app->tree(), root, {DataPhase::READY}));
    f.prepare();
    check(f.advance(2000).timers == 0, "READY data-state scalar view retained spinner work");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "READY data-state view redraws while idle");
    check(resolutions == 2, "display changes re-resolved construction-only label expressions");
  }
}

void unusedLabelsAndActualServices() {
  for (int mode = 0; mode < 4; ++mode) {
    Fixture f;
    ui::SceneResourceOptions options;
    int calls{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++calls;
      if (mode == 1) {
        throw std::runtime_error("unused resolver");
      }
      if (mode == 2) {
        return std::string(1, static_cast<char>(0xff));
      }

      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    };
    const auto mounted =
      take(ui::mount(*f.scene, ui::resource("scene{DataStateView{showStatus=false labels{loading=i18n(\"unused\") "
                                            "retry=i18n(\"unused retry\")} Label{\"body\"}}}",
                                            options)));
    const auto root = checkedDataState(f, mounted);

    take(setDataStateView(f.app->tree(), root, {DataPhase::LOADING}));
    f.prepare();
    check(calls == 0, "disabled status resolved an unused label or retry expression");
    check(take(f.app->tree().inspect(root)).children.size() == 1, "disabled status mounted status widgets");
    check(f.advance(1000).timers == 0, "disabled status allocated/armed spinner work");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "disabled status redraws while idle");
  }
  {
    Fixture f;
    DataStateConfig actual;
    int local{}, fallback{}, retries{};

    actual.statusRows = 2;
    actual.labels.loading = i18n("local");
    actual.labels.retry = i18n("retry");
    actual.messageStyle.normal.bold = true;
    actual.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      ++local;

      return key == "retry" ? "Try again" : "Local";
    };
    actual.retry = [&]() -> std::expected<void, TreeError> {
      ++retries;
      return {};
    };
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++fallback;
      throw std::runtime_error("custom factory resolver must win");
    };
    ui::Ref<DataStateView> reference;

    take(options.registry.add<DataStateView>("Custom", [&](ui::SceneResourceArguments) {
      return ui::dataStateView(DataPresentation{DataPhase::FAILED}, ui::label("body"), actual)
        .labels({.empty = text("Patched")})
        .reference(reference);
    }));
    take(ui::mount(*f.scene, ui::resource("scene{Custom{}}", options)));
    f.prepare();
    check(local == 2 && fallback == 0, "actual factory resolver/retry services were replaced or eagerly duplicated");
    const auto strip = take(f.app->tree().inspect(reference.node())).children.front();
    const auto widgets = take(f.app->tree().inspect(strip)).children;

    check(widgets.size() == 3, "sparse label patch dropped explicit retry service");
    take(f.app->tree().read(widgets[1], [](const Widget &widget) {
      check(dynamic_cast<const Label &>(widget).styleDefaults().normal.bold,
            "sparse labels replaced the actual native message style");
    }));
    take(f.app->router().invoke(widgets.back(), {Action::ACTIVATE}));
    check(retries == 1, "actual factory retry action was not retained");
  }
  {
    Fixture f;
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    const auto node = checkedDataState(
      f, take(ui::mount(*f.scene,
                        ui::resource("scene{DataStateView{spinnerInterval=4294967396 Label{\"body\"}}}", options))));

    take(setDataStateView(f.app->tree(), node, {DataPhase::LOADING}));
    f.prepare();
    check(f.advance(100).timers == 0, "full uint64 interval was narrowed to size_t/uint32");
  }
}

struct ResolverCopyState {
  bool armed{};
  int copies{};
  int calls{};
};

struct CopySensitiveResolver {
  std::shared_ptr<ResolverCopyState> state;
  std::shared_ptr<int> lifetime;

  CopySensitiveResolver(std::shared_ptr<ResolverCopyState> value, std::shared_ptr<int> token)
      : state(std::move(value)), lifetime(std::move(token)) {
  }

  CopySensitiveResolver(const CopySensitiveResolver &other) : state(other.state), lifetime(other.lifetime) {
    ++state->copies;
    if (state->armed) {
      throw std::bad_alloc();
    }
  }

  std::expected<std::string, TextResourceError> operator()(std::string_view) const {
    ++state->calls;

    return "Translated";
  }
};

void resolverCopyAndBorrowLifetime() {
  for (const bool showStatus : {false, true}) {
    Fixture f;
    ui::SceneResourceOptions options;
    ui::Ref<Widget> reference;
    auto state = std::make_shared<ResolverCopyState>();
    std::weak_ptr<int> lifetime;
    bool armInFactory = true;
    int bodyMounts{}, restored{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    {
      auto token = std::make_shared<int>(1);

      lifetime = token;
      options.resolver = CopySensitiveResolver(state, std::move(token));
    }
    take(options.registry.add<Label>("Body", [&](ui::SceneResourceArguments) {
      // Arm after resource options have been copied: only the new constructor-only fallback copy is targeted.
      state->armed = armInFactory;

      return ui::detail::Access::cold<Label>(
        [&](ManagedScene &scene, Node<Widget> parent,
            ui::detail::MountContext &context) -> std::expected<Node<Label>, ManagedError> {
          check(context.dataStateResolver == nullptr, "constructor resolver borrow leaked into body creation");
          ++bodyMounts;

          return ui::detail::Access::mount(ui::label("body"), scene, parent, context);
        });
    }));
    const auto source = std::string("scene{DataStateView{showStatus=") + (showStatus ? "true" : "false") +
                        " labels{loading=i18n(\"work\")} Body{}}}";
    const auto mount = [&]() {
      auto recipe = ui::resource(source, options).reference(reference);
      auto guarded = ui::detail::Access::cold<Widget>(
        [&, recipe = std::move(recipe)](
          ManagedScene &scene, Node<Widget> parent,
          ui::detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
          try {
            auto result = ui::detail::Access::mount(std::move(recipe), scene, parent, context);

            check(context.dataStateResolver == nullptr, "checked constructor exit retained borrowed resolver");
            ++restored;

            return result;
          } catch (...) {
            check(context.dataStateResolver == nullptr, "throwing constructor exit retained borrowed resolver");
            ++restored;
            throw;
          }
        });

      return ui::mount(*f.scene, std::move(guarded));
    };
    const auto copiesBefore = state->copies;
    auto result = mount();

    check(state->copies > copiesBefore && restored == 1, "targeted resolver copy/restore path was not reached");
    if (showStatus) {
      const auto *cause = result ? nullptr : std::get_if<TreeError>(&result.error().cause);

      check(!result && cause && *cause == TreeError::NO_MEMORY && bodyMounts == 0 && state->calls == 0,
            "fallback resolver copy failure did not preserve the checked allocation boundary");
      check(result.error().sceneResource && !result.error().sceneResource->propertyPath &&
              result.error().sceneResource->offset == source.find("DataStateView"),
            "fallback resolver copy failure lost its owning source-node attribution");
      check(reference.node() == Node<Widget>{} && take(f.app->tree().inspect(f.scene->root())).children.empty(),
            "fallback resolver copy failure published partial nodes/references");
      state->armed = false;
      armInFactory = false;
      result = mount();
      check(result.has_value() && restored == 2 && state->calls == 1,
            "constructor copy failure poisoned retry or duplicated label resolution");
    } else {
      check(result.has_value() && state->calls == 0,
            "disabled status copied or called the armed constructor fallback resolver");
    }
    check(bodyMounts == 1, "body was not mounted exactly once after constructor resolver restoration");
    options.resolver = {};
    check(lifetime.expired(), "mounted data-state records retained the resource resolver service");
    f.prepare();
    check(f.advance(1000).timers == 0, "READY/disabled constructor fallback created timer work");
  }
  {
    Fixture f;
    ui::SceneResourceOptions options;
    int body{}, status{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      key == "body" ? ++body : ++status;

      return "Body translated";
    };
    take(ui::mount(*f.scene, ui::resource("scene{DataStateView{showStatus=false labels{loading=i18n(\"unused\")} "
                                          "Label{text=i18n(\"body\")}}}",
                                          options)));
    check(body == 1 && status == 0, "status suppression changed existing eager body text resolution");
  }
}

void boundAndInferredScalarPaths() {
  {
    Fixture f;
    auto state = take(f.scene->state(DataPresentation{DataPhase::LOADING}));
    const auto node = take(ui::mount(*f.scene, ui::dataStateView(state, ui::label("body"))
                                                 .statusRows(2)
                                                 .spinnerInterval(200)
                                                 .labels({.loading = text("Bound")})));

    f.prepare();
    check(statusCaption(f, node) == "Bound", "bound factory did not consume local sparse labels");
    check(f.advance(199).timers == 0 && f.advance(200).timers == 1, "bound factory lost declarative spinner interval");
    take(state.set({DataPhase::READY, 2}));
    f.prepare();
    check(f.value(node).phase == DataPhase::READY && f.advance(1000).timers == 0,
          "bound factory lost native phase writer or timer retirement");
  }
  {
    Fixture f;
    auto model = take(f.scene->model<int>(std::make_shared<Page>(RangeState::READY)));
    const auto node = take(ui::mount(
      *f.scene, ui::dataStateView(ui::virtualList(model,
                                                  [](const ModelRow<int> &row) {
                                                    return take(GraphemeText::from(std::to_string(row.value)));
                                                  })) |
                  ui::statusRows(2) | ui::spinnerInterval(200) | ui::labels({.empty = text("No rows")})));

    f.prepare();
    check(f.value(node).phase == DataPhase::READY && f.advance(200).timers == 0,
          "inferred scalar factory replaced the real model presentation or started polling");
  }
}

void resourceResolverReentryRetiresParent() {
  Fixture f;
  ui::SceneResourceOptions options;
  ui::Ref<Widget> reference;
  int resolutions{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    const auto children = take(f.app->tree().inspect(f.scene->root())).children;

    check(children.size() == 1, "reentry fixture did not reach a real unpublished source parent");
    take(f.scene->remove(children.front()));

    return "Reentered";
  };
  const auto result = ui::mount(
    *f.scene, ui::resource("scene{VBox{DataStateView{labels{loading=i18n(\"reenter\")} Label{\"body\"}}}}", options)
                .reference(reference));
  const auto *cause = result ? nullptr : std::get_if<TreeError>(&result.error().cause);

  check(!result && resolutions == 1 && cause && (*cause == TreeError::STALE_NODE || *cause == TreeError::CLOSED),
        "resource constructor callback ignored the native post-resolver lifetime recheck");
  check(result.error().sceneResource && !result.error().sceneResource->propertyPath &&
          reference.node() == Node<Widget>{} && take(f.app->tree().inspect(f.scene->root())).children.empty(),
        "reentrant resource construction retained a partial body/reference or lost source-node attribution");
  check(f.advance(1000).timers == 0, "reentrant failed construction retained spinner work");
}

void completeResourceBodyAndOverrideOrder() {
  Fixture f;
  ui::SceneResourceOptions options;
  ui::Ref<Label> one, two, three;
  ui::Ref<Column> box;
  std::vector<std::string> factories, mounts;

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  take(options.registry.add<Label>("Mark", [&](ui::SceneResourceArguments arguments) {
    factories.push_back(arguments.id);
    const auto id = arguments.id;
    const auto reference = id == "one" ? one : id == "two" ? two : three;

    return ui::label(arguments.text)
      .reference(reference)
      .setup([&, id](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
        mounts.push_back(id);

        return {};
      });
  }));
  take(options.registry.add<Column>(
    "Box",
    [&](ui::SceneResourceArguments) {
      factories.push_back("box");

      return ui::column().reference(box).setup([&](ManagedScene &, Node<Column>) -> std::expected<void, ManagedError> {
        mounts.push_back("box");

        return {};
      });
    },
    true));
  const auto root = take(ui::mount(*f.scene, ui::resource(R"(
    scene{DataStateView{showStatus=false Grid#grid{columns=2 Mark#one{"one"} Box{Mark#two{"two"} DataStateView{showStatus=false Mark#three{"three"}}}}}}
  )",
                                                          options)));

  check(factories == std::vector<std::string>{"one", "two", "three", "box"},
        "resource code factories lost exact postorder invocation");
  check(mounts == std::vector<std::string>{"one", "box", "two", "three"},
        "complete data-state body mounted descendants out of order or more than once");
  check(one.node() != Node<Label>{} && two.node() != Node<Label>{} && three.node() != Node<Label>{},
        "nested body references were not published");
  f.prepare();
  const auto left = take(f.scene->geometry(one.node())).local;
  const auto right = take(f.scene->geometry(box.node())).local;

  check(right.origin().x >= left.right(), "late Grid binding lost composed source-index child placements");
  take(f.scene->remove(root));
  check(f.advance(1000).timers == 0, "removed complete body retained presentation work");
  factories.clear();
  mounts.clear();
  ui::SceneResourceOptions overridden;

  overridden.format = ui::SceneResourceFormat::CONFIGURATION;
  take(overridden.registry.add<Label>("Mark", [&](ui::SceneResourceArguments arguments) {
    factories.push_back(arguments.text);

    return ui::label(arguments.text);
  }));
  take(overridden.registry.add<Column>(
    "DataStateView",
    [&](ui::SceneResourceArguments) {
      factories.push_back("override");

      return ui::column();
    },
    true));
  take(ui::mount(*f.scene, ui::resource("scene{DataStateView{Mark{\"first\"} Mark{\"second\"}}}", overridden)));
  check(factories == std::vector<std::string>{"first", "second", "override"},
        "caller override inherited fallback one-body or factory order behavior");
}

void labelsChildNameScope() {
  for (int spelling = 0; spelling < 3; ++spelling) {
    Fixture f;
    ui::SceneResourceOptions options;
    int labelsChildren{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    if (spelling) {
      take(options.registry.add<Label>("labels", [&](ui::SceneResourceArguments) {
        ++labelsChildren;

        return ui::label("child");
      }));
    }
    const std::string contents = spelling == 1   ? "labels{}"
                                 : spelling == 2 ? "labels={loading=\"Waiting\"} Label{\"body\"}"
                                                 : "labels{loading=\"Waiting\"} Label{\"body\"}";
    const auto root = take(ui::mount(*f.scene, ui::resource("scene{DataStateView{" + contents + "}}", options)));

    check(labelsChildren == (spelling == 1 ? 1 : 0),
          "native labels group stole or swallowed a registered empty body child");
    check(take(f.app->tree().inspect(root)).children.size() == 2, "scoped labels spelling lost the one real body");
    if (spelling) {
      take(ui::mount(*f.scene, ui::resource("scene{VBox{labels{}}}", options)));
      check(labelsChildren == (spelling == 1 ? 2 : 1), "labels became globally reserved beneath unrelated VBox");
    }
  }
}

void lateLabelFailureAndFinalizerRollback() {
  Fixture f;
  ui::Ref<Widget> root;
  ui::Ref<Label> body;
  ui::SceneResourceOptions options;
  int calls{};
  bool repaired{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  take(options.registry.add<Label>("Body", [&](ui::SceneResourceArguments) {
    return ui::label("body").reference(body);
  }));
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++calls;
    if (!repaired) {
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    }

    return "Repaired";
  };
  const auto source = "scene{DataStateView{labels{loading=i18n(\"secret key\")} Body{}}}";
  auto result = ui::mount(*f.scene, ui::resource(source, options).reference(root));

  check(!result && result.error().resource == TextResourceError::MISSING_RESOURCE && calls == 1,
        "late label failure lost its original typed resource cause or repeated resolution");
  check(result.error().sceneResource && !result.error().sceneResource->propertyPath &&
          result.error().sceneResource->offset == std::string_view(source).find("DataStateView"),
        "native label failure invented a leaf or lost owning source-node attribution");
  check(root.node() == Node<Widget>{} && body.node() == Node<Label>{} &&
          take(f.app->tree().inspect(f.scene->root())).children.empty(),
        "late label failure published partial references/nodes");
  repaired = true;
  take(ui::mount(*f.scene, ui::resource(source, options).reference(root)));
  check(body.node() != Node<Label>{}, "late label failure poisoned retry of pending references");
  take(f.scene->remove(root.node()));
  options.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return std::string(1, static_cast<char>(0xff));
  };
  const auto invalidText = ui::mount(*f.scene, ui::resource(source, options));
  const auto nativeTextError = GraphemeText::from(std::string(1, static_cast<char>(0xff))).error();

  check(!invalidText && invalidText.error().text && invalidText.error().text->kind == nativeTextError.kind &&
          invalidText.error().sceneResource && !invalidText.error().sceneResource->propertyPath,
        "late label failure lost the native typed text error or invented a leaf");
  check(take(f.app->tree().inspect(f.scene->root())).children.empty(),
        "late invalid label text retained a partial native body");
  ui::Ref<Widget> composite;
  ui::Ref<Label> first;
  bool actualHelp{};
  ui::SceneResourceOptions late;

  late.format = ui::SceneResourceFormat::CONFIGURATION;
  take(late.registry.add<Label>("Mark", [&](ui::SceneResourceArguments) {
    return ui::label("first").reference(first);
  }));
  take(late.registry.add<Widget>("Support", [&](ui::SceneResourceArguments) {
    return actualHelp ? ui::detail::Access::erase(ui::helpView(ui::Ref<HelpView>{}))
                      : ui::detail::Access::erase(ui::label("wrong capability"));
  }));
  const auto nested =
    "scene{DataStateView{showStatus=false Grid{columns=2 Mark{} KeyHints{help=\"support\"} Support#support{}}}}";
  auto failed = ui::mount(*f.scene, ui::resource(nested, late).reference(composite));

  check(!failed && failed.error().sceneResource && failed.error().sceneResource->propertyPath &&
          *failed.error().sceneResource->propertyPath == "KeyHints.help",
        "composed child finalizer lost its owning help source");
  check(first.node() == Node<Label>{} && composite.node() == Node<Widget>{} &&
          take(f.app->tree().inspect(f.scene->root())).children.empty(),
        "late composed finalizer did not roll back Grid/body pending references");
  actualHelp = true;
  take(ui::mount(*f.scene, ui::resource(nested, late).reference(composite)));
  f.prepare();
  check(first.node() != Node<Label>{}, "failed composed finalizer poisoned real Grid-body retry");
}
} // namespace

int main() {
  try {
    scalarAndLabelParity();
    unusedLabelsAndActualServices();
    resolverCopyAndBorrowLifetime();
    boundAndInferredScalarPaths();
    resourceResolverReentryRetiresParent();
    completeResourceBodyAndOverrideOrder();
    labelsChildNameScope();
    lateLabelFailureAndFinalizerRollback();
    mapping();
    ownedTimerAndLifecycle();
    affinityAndBinding();
    automaticSamePin();
    automaticRetiredTextSource();
    coldTextStateRollback();
    scrollClipStopsAnimation();
    rollback();
    retryAdmissionAndRemoval();
    disabledZeroAndStop();
    rejectedNotification();
    delayedRetryAndCancellation<false>();
    delayedRetryAndCancellation<true>();
    refreshingKeepsReadyRows<false>();
    refreshingKeepsReadyRows<true>();
    runtimeAllocationStaging();
    indicatorThemeAndEqual();
    originalTechnicalError();
    allocationReentryRetiresIdentity();
    modelApplicationFailureIsTechnical();
    publicRefDisplayAndValidation();
    incompleteMountDoesNotPublish();
    bodyRemovalAtomicity();
    allocationRollback();
    std::cout << "data-presentation focused checks PASS\n";
    return 0;
  } catch (const std::exception &error) {
    allocationTest::remaining = -1;
    allocationTest::hook = nullptr;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
