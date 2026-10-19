// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace portals;

void check(bool value, std::string_view message) {
  if (!value) {
    throw std::runtime_error(std::string(message));
  }
}

template <class T, class E> T checked(std::expected<T, E> result) {
  check(result.has_value(), "checked SDK operation failed");

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

DataPresentation presentation(Application &app, Node<DataStateView> node) {
  DataPresentation value;

  checked(app.tree().read(node, [&](const DataStateView &view) {
    value = view.presentation();
  }));

  return value;
}

void prepare(ManagedScene &owner, Application &app) {
  constexpr CellSize SIZE{30, 8};
  constexpr int MAX_LAYOUT_PASSES = 3;
  const auto bounds = checked(CellRect::from({}, SIZE));

  checked(owner.prepare(app, SIZE));

  for (int pass = 0; pass < MAX_LAYOUT_PASSES; ++pass) {
    checked(app.tree().consumeFrameRequest());

    std::optional<LayoutSnapshot> snapshot;

    checked(app.tree().frame([&] {
      checked(layoutTree(app.tree(), app.root(), bounds, bounds));
      snapshot.emplace(checked(LayoutSnapshot::capture(app.tree(), app.root())));
    }));
    checked(owner.prepareViews(app, *snapshot));

    if (checked(app.tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      checked(owner.completedLayout(app));

      return;
    }
  }

  check(false, "installed view layout did not settle");
}

struct Fixture {
  SchedulerTime now{};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> owner;

  Fixture() {
    app = checked(Application::create({}, {[](void *context) noexcept {
                                             return *static_cast<SchedulerTime *>(context);
                                           },
                                           &now}));
    owner = checked(ManagedScene::create(*app));
  }

  SchedulerDispatch advance(SchedulerTime time) {
    now = time;

    auto dispatch = checked(app->scheduler().dispatch());

    checked(owner->afterDispatch(*app));

    return dispatch;
  }

  void prepare() {
    ::prepare(*owner, *app);
  }
};

void explicitBehavior(ManagedScene &owner, Application &app, ui::Ref<DataStateView> view) {
  for (auto phase : {DataPhase::LOADING, DataPhase::EMPTY, DataPhase::FAILED, DataPhase::OFFLINE, DataPhase::REFRESHING,
                     DataPhase::READY}) {
    checked(view.display({phase, 2}));
    prepare(owner, app);
    check(presentation(app, view.node()) == DataPresentation{phase, 2}, "explicit phase missing");
  }

  checked(view.display({DataPhase::LOADING, 1}));
  check(presentation(app, view.node()) == DataPresentation{DataPhase::READY, 2}, "old generation replaced state");

  checked(app.tree().consumeFrameRequest());
  checked(view.display({DataPhase::READY, 2}));
  check(checked(app.tree().consumeFrameRequest()) == Invalidation::NONE, "equal value requested redraw");
}

void ordinaryAndPipe() {
  {
    Fixture f;
    ui::Ref<DataStateView> view;

    checked(ui::mount(*f.owner, ui::dataStateView(DataPresentation{}, ui::label("body")).reference(view)));
    explicitBehavior(*f.owner, *f.app, view);
    checked(f.owner->close());

    auto closed = view.display({DataPhase::LOADING, 3});

    check(!closed && closed.error() == TreeError::STALE_NODE, "retired presentation Ref lost stale-node cause");
    f.owner.reset();

    auto expired = view.display({DataPhase::LOADING, 3});

    check(!expired && expired.error() == TreeError::CLOSED, "destroyed owner Ref lost closed cause");
  }

  {
    ui::Ref<DataStateView> view;
    auto scene = ui::scene(ui::dataStateView(DataPresentation{}, ui::label("body")).reference(view));

    scene | ui::prepare();
    scene.configure([&](ManagedScene &owner, Application &app) {
      explicitBehavior(owner, app, view);
    });
    checked(scene.checked());
    checked(scene.close());
  }
}

void readonlyBinding() {
  Fixture f;
  auto state = checked(f.owner->state(DataPresentation{DataPhase::LOADING}));
  auto source = checked(f.owner->derive(state, [](const DataPresentation &value) {
    return value;
  }));
  auto node = checked(ui::mount(*f.owner, ui::dataStateView(source, ui::label("bound"))));

  f.prepare();
  checked(state.set({DataPhase::EMPTY, 2}));
  f.prepare();
  check(presentation(*f.app, node) == DataPresentation{DataPhase::EMPTY, 2}, "readonly source did not update");

  auto rejected = setDataStateView(f.app->tree(), node, {DataPhase::READY, 3});

  check(!rejected && rejected.error() == TreeError::INVALID_ARGUMENT, "bound view allowed writes");
  checked(f.owner->close());
}

struct Page final : ModelSnapshot<int> {
  RangeState state;
  std::array<ModelRow<int>, 2> rows{{{{1, 1}, 10}, {{2, 1}, 20}}};

  explicit Page(RangeState value, std::uint64_t revision = 7) : ModelSnapshot(revision, 2), state(value) {
  }

  ModelRows<int> read(RowRange range) const override {
    if (state != RangeState::READY) {
      return {state, {}, {}};
    }

    return {state, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }
};

GraphemeText displayNumber(const int &value) {
  return checked(GraphemeText::from(std::to_string(value)));
}

template <bool TABLE> void automaticCompletionAndCleanup() {
  using View = std::conditional_t<TABLE, VirtualTable<int>, VirtualList<int>>;
  Fixture f;
  auto model = checked(f.owner->model<int>(std::make_shared<Page>(RangeState::FAILED)));
  ui::Ref<View> body;
  std::optional<ModelRequest<int>> work;
  auto prepared = [&](const ModelFrame<int> &,
                      const std::optional<ModelRequest<int>> &request) -> std::expected<void, TreeError> {
    if (request) {
      work = *request;
    }

    return {};
  };
  auto content = [&] {
    if constexpr (TABLE) {
      std::array<TableColumn<int>, 1> columns{{{{1, 1}, checked(GraphemeText::from("Value")), 8, displayNumber}}};

      return ui::virtualTable(model, std::span<const TableColumn<int>>(columns), {}, {}, prepared).reference(body);
    } else {
      return ui::virtualList(
               model,
               [](const ModelRow<int> &row) {
                 return displayNumber(row.value);
               },
               {}, {}, prepared)
        .reference(body);
    }
  }();
  int retries{};
  DataStateConfig config;

  config.retry = [&]() -> std::expected<void, TreeError> {
    ++retries;

    if constexpr (TABLE) {
      return retryVirtualTable(f.app->tree(), body.node());
    } else {
      return retryVirtualList(f.app->tree(), body.node());
    }
  };

  auto node = checked(ui::mount(*f.owner, ui::dataStateView(std::move(content), config)));

  f.prepare();
  check(presentation(*f.app, node).phase == DataPhase::FAILED && !work, "failed base did not remain static");

  auto rejected = setDataStateView(f.app->tree(), node, {DataPhase::READY, 2});

  check(!rejected && rejected.error() == TreeError::INVALID_ARGUMENT, "automatic view allowed writes");

  const auto strip = checked(f.app->tree().inspect(node)).children.front();
  const auto retry = checked(f.app->tree().inspect(strip)).children.back();

  checked(f.app->router().invoke(retry, {Action::ACTIVATE}));
  checked(f.app->router().invoke(retry, {Action::ACTIVATE}));
  f.prepare();
  check(retries == 1 && work && !work->stopRequested(), "explicit retry did not retain one request");
  check(presentation(*f.app, node).phase == DataPhase::LOADING, "retry did not show pending state");
  check(f.advance(100).timers == 1, "visible pending indicator did not tick");
  f.prepare();
  check(!work->stopRequested(), "indicator preparation cancelled failed-base retry");

  checked(work->complete(std::make_shared<Page>(RangeState::READY)));
  f.advance(250);
  f.prepare();
  check(presentation(*f.app, node).phase == DataPhase::READY, "provider completion did not display ready");
  check(f.advance(500).timers == 0, "ready state retained animation");

  // A retry completion is consumer-local; publish ready base data before requesting a refresh.
  checked(checked(model.publisher()).publish(std::make_shared<Page>(RangeState::READY, work->dataRevision() + 1)));
  f.advance(500);
  f.prepare();

  if constexpr (TABLE) {
    checked(retryVirtualTable(f.app->tree(), body.node()));
  } else {
    checked(retryVirtualList(f.app->tree(), body.node()));
  }

  work.reset();
  f.prepare();
  check(work && presentation(*f.app, node).phase == DataPhase::REFRESHING, "refresh lost ready rows");
  checked(f.app->tree().setVisible(node, false));
  f.prepare();
  check(work->stopRequested(), "hidden automatic body retained work");
  f.advance(600);
  check(f.advance(1000).timers == 0, "hidden animation rearmed");
  checked(f.owner->remove(node));
  checked(f.owner->close());

  auto late = work->complete(std::make_shared<Page>(RangeState::READY));

  check(!late && late.error() == SchedulerError::CLOSED, "retired request accepted completion");
  check(f.advance(1200).timers == 0, "closed scene retained animation");
}

void originalTechnicalError() {
  Fixture f;
  auto model = checked(f.owner->model<int>(std::make_shared<Page>(RangeState::PENDING)));
  ui::Ref<VirtualList<int>> body;
  auto node = checked(ui::mount(
    *f.owner, ui::dataStateView(ui::virtualList(
                                  model,
                                  [](const ModelRow<int> &row) {
                                    return displayNumber(row.value);
                                  },
                                  {}, {},
                                  [](const ModelFrame<int> &,
                                     const std::optional<ModelRequest<int>> &) -> std::expected<void, TreeError> {
                                    return std::unexpected(TreeError::NODE_LIMIT);
                                  })
                                  .reference(body))));

  checked(f.owner->prepare(*f.app, {30, 8}));

  const auto bounds = checked(CellRect::from({}, {30, 8}));
  std::optional<LayoutSnapshot> snapshot;

  checked(f.app->tree().frame([&] {
    checked(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    snapshot.emplace(checked(LayoutSnapshot::capture(f.app->tree(), f.app->root())));
  }));

  auto failed = f.owner->prepareViews(*f.app, *snapshot);

  check(!failed && failed.error() == TreeError::NODE_LIMIT, "technical cause was masked");

  const auto errors = checked(f.owner->diagnostics());
  const auto &error = errors.errors.back().error;

  check(error.phase == ManagedPhase::PREPARE && error.node == body.node().id() &&
          std::get<TreeError>(error.cause) == TreeError::NODE_LIMIT,
        "technical phase, target or cause changed");
  check(presentation(*f.app, node).phase == DataPhase::LOADING, "technical error became a domain phase");
  checked(f.owner->close());
}
} // namespace

/** @brief Verify data presentation through public installed headers and library only. */
int main() {
  static_assert(dataPhase({true, RangeState::READY, 0, 0, false}) == DataPhase::EMPTY);
  static_assert(dataPhase({true, RangeState::READY, 2, 0, true}) == DataPhase::REFRESHING);

  try {
    ordinaryAndPipe();
    readonlyBinding();
    automaticCompletionAndCleanup<false>();
    automaticCompletionAndCleanup<true>();
    originalTechnicalError();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
