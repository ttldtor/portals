// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <cstdlib>
#include <iostream>
#include <new>
#include <portals/ui.hpp>
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
          result.styles[i] = widget.computedStyle();
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

} // namespace

int main() {
  try {
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
