// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{40, 12};
constexpr SchedulerTime SPINNER_INTERVAL = 25;
constexpr int MAX_PREPARATION_PASSES = 3;

void check(bool value, const char *message, std::source_location at = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error(std::string(message) + " at line " + std::to_string(at.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location at = std::source_location::current()) {
  if (!result) {
    if constexpr (std::same_as<E, ManagedError>) {
      if (const auto *cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << "managed phase=" << static_cast<int>(result.error().phase)
                  << " tree cause=" << static_cast<int>(*cause) << '\n';
      }
    } else if constexpr (std::same_as<E, TreeError>) {
      std::cerr << "tree=" << static_cast<int>(result.error()) << '\n';
    }
  }

  check(result.has_value(), "unexpected API failure", at);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void take(ui::DialogCommandResult result, std::source_location at = std::source_location::current()) {
  take(result.result(), at);
}

bool treeCause(const ManagedError &error, TreeError expected) {
  const auto *actual = std::get_if<TreeError>(&error.cause);

  return actual && *actual == expected;
}

struct Fixture {
  SchedulerTime now{};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  Fixture() {
    app = take(Application::create({}, {[](void *value) noexcept {
                                          return *static_cast<SchedulerTime *>(value);
                                        },
                                        &now}));
    scene = take(ManagedScene::create(*app));
  }

  void prepare() {
    take(scene->prepare(*app, VIEWPORT));

    const auto bounds = take(CellRect::from({}, VIEWPORT));

    for (int pass = 0; pass != MAX_PREPARATION_PASSES; ++pass) {
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
        take(scene->completedLayout(*app));

        return;
      }
    }

    check(false, "combined preparation exceeded the existing three-pass bound");
  }

  SchedulerDispatch advance() {
    now += SPINNER_INTERVAL;

    const auto dispatched = take(app->scheduler().dispatch());

    take(scene->afterDispatch(*app));

    return dispatched;
  }

  void idle(const char *message) {
    check(advance().timers == 0, message);
    check(take(app->tree().consumeFrameRequest()) == Invalidation::NONE, "idle state requested another frame");
  }

  DataPresentation value(Node<DataStateView> node) {
    DataPresentation result;

    take(app->tree().read(node, [&](const DataStateView &view) {
      result = view.presentation();
    }));

    return result;
  }
};

struct ColdRefs {
  ui::Ref<OverlayHost> host;
  ui::Ref<LineEdit> background, input;
  ui::Ref<Label> anchor;
  ui::Ref<Dialog> dialog;
  ui::Ref<DataStateView> state;
  ui::Ref<VirtualList<TextSegment>> list;

  bool empty() const {
    return host.node() == Node<OverlayHost>{} && background.node() == Node<LineEdit>{} &&
           input.node() == Node<LineEdit>{} && anchor.node() == Node<Label>{} && dialog.node() == Node<Dialog>{} &&
           state.node() == Node<DataStateView>{} && list.node() == Node<VirtualList<TextSegment>>{};
  }
};

auto coldRecipe(ColdRefs &refs, TextSplitOptions split = {}, ui::VirtualListConfig<TextSegment> config = {}) {
  // The ordinary body mounts first. The actual source deliberately follows its consumer inside the dialog.
  auto content = ui::column(
    ui::dataStateView(
      ui::virtualList(ui::words(refs.input.draftText(), split), std::move(config)).reference(refs.list).height(2))
      .reference(refs.state)
      .height(2),
    ui::lineEdit(refs.input, "one two")
      .setup([&refs](ManagedScene &, Node<LineEdit>) -> std::expected<void, ManagedError> {
        check(refs.empty(), "cold setup observed premature public Ref publication");

        return {};
      }));

  return ui::overlayHost(
    refs.host, ui::column(ui::label("Anchor").reference(refs.anchor), ui::lineEdit(refs.background, "outside")),
    ui::dialog(refs.dialog, "Words", std::move(content)).anchor(refs.anchor));
}

void coldForwardAnchorAndRetainedIdentity() {
  Fixture f;
  ColdRefs refs;
  std::optional<ModelFrame<TextSegment>> frame;
  ui::VirtualListConfig<TextSegment> config;

  config.prepared = [&](const ModelFrame<TextSegment> &prepared, const std::optional<ModelRequest<TextSegment>> &) {
    frame = prepared;

    return std::expected<void, TreeError>{};
  };

  take(ui::mount(*f.scene, coldRecipe(refs, {}, std::move(config))));
  check(refs.host.node() != Node<OverlayHost>{} && refs.background.node() != Node<LineEdit>{} &&
          refs.input.node() != Node<LineEdit>{} && refs.anchor.node() != Node<Label>{} &&
          refs.dialog.node() != Node<Dialog>{} && refs.state.node() != Node<DataStateView>{} &&
          refs.list.node() != Node<VirtualList<TextSegment>>{},
        "complete cold mount did not publish every Ref");
  check(!take(refs.dialog.opened()), "cold dialog started open");
  f.prepare();
  f.idle("closed automatic state scheduled animation");
  take(refs.background.focus());
  take(refs.dialog.open());
  f.prepare();
  check(f.value(refs.state.node()).phase == DataPhase::READY, "opened automatic state did not become READY");
  take(f.app->tree().read(refs.list.node(), [](const VirtualList<TextSegment> &view) {
    check(view.dataStatus().prepared && view.dataStatus().rowCount == 2, "prepared owning two-row facts missing");
  }));
  check(frame.has_value(), "visible list did not expose its existing prepared callback");

  const auto retained = *frame;
  const auto host = refs.host.node();
  const auto dialog = refs.dialog.node();
  const auto state = refs.state.node();
  const auto list = refs.list.node();
  const auto input = refs.input.node();
  const auto anchor = refs.anchor.node();

  take((refs.input.text() = "three four five").result());
  f.prepare();
  check(f.value(state).phase == DataPhase::READY && frame->rowCount() == 3, "edited text did not prepare three rows");

  const auto oldRows = take(retained.read({0, 2}));
  const auto newRows = take(frame->read({0, 3}));

  check(oldRows.state() == RangeState::READY && oldRows.rows().size() == 2 && newRows.state() == RangeState::READY &&
          newRows.rows().size() == 3,
        "owning frames did not provide the complete ready row ranges");
  check(oldRows.rows()[0].value.bytes() == "one" && oldRows.rows()[1].value.bytes() == "two",
        "retained earlier frame lost owning text after edit");
  check(newRows.rows()[0].value.bytes() == "three" && newRows.rows()[2].value.bytes() == "five",
        "new prepared frame retained previous text");
  take(refs.dialog.dismiss());
  f.prepare();
  check(take(f.app->router().focused()) == refs.background.node(), "dismissal did not restore the ordinary editor");
  f.idle("dismissed READY state scheduled animation");
  take(refs.dialog.open());
  f.prepare();
  check(refs.host.node() == host && refs.dialog.node() == dialog && refs.state.node() == state &&
          refs.list.node() == list && refs.input.node() == input && refs.anchor.node() == anchor,
        "reopen replaced a retained widget identity");
  check(take(refs.input.draftText().snapshot()).bytes() == "three four five" && frame->rowCount() == 3,
        "reopen reconstructed the source or discarded edited rows");
  f.idle("reopened READY state scheduled animation");
}

void coldTransformRollbackAndRecovery() {
  Fixture f;
  ColdRefs refs;

  const auto rejected = ui::mount(*f.scene, coldRecipe(refs, TextSplitOptions{.maxTokens = 1}));

  check(!rejected && treeCause(rejected.error(), TreeError::NODE_LIMIT), "cold transform lost original NODE_LIMIT");
  check(refs.empty(), "failed combined mount published a Ref");
  check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "failed combined mount retained children");
  check(!f.scene->usesPreparedLayout(), "failed combined mount retained dialog/data registration");
  f.prepare();
  f.idle("rolled-back cold graph retained timer work");
  f.idle("rolled-back cold graph rearmed timer work");
  take(ui::mount(*f.scene, coldRecipe(refs)));
  f.prepare();
  take(refs.dialog.open());
  f.prepare();
  check(f.value(refs.state.node()).phase == DataPhase::READY, "valid retry did not reclaim overlay/data budgets");
}

void invalidConfigurationRollback(bool invalidData) {
  Fixture f;
  ui::Ref<OverlayHost> host;
  ui::Ref<Dialog> dialog;
  ui::Ref<DataStateView> state;
  DataStateConfig data;
  DialogOptions options;

  if (invalidData) {
    data.spinnerInterval = 0;
  } else {
    options.maximumPanelSize = {0, 1};
  }

  const auto failed = ui::mount(
    *f.scene, ui::overlayHost(
                host, ui::label("body"),
                ui::dialog(dialog, "Invalid",
                           ui::dataStateView(DataPresentation{}, ui::label("data"), data).reference(state), options)));

  check(!failed && treeCause(failed.error(), TreeError::INVALID_ARGUMENT), "invalid settings lost INVALID_ARGUMENT");
  check(host.node() == Node<OverlayHost>{} && dialog.node() == Node<Dialog>{} && state.node() == Node<DataStateView>{},
        "invalid settings published a partial Ref");
  check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "invalid settings retained a subtree");
  check(!f.scene->usesPreparedLayout(), "invalid settings retained prepared registration");
  f.prepare();
  f.idle("invalid settings retained animation");
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("recovered"), ui::dialog("Valid", ui::label("body")))));
  f.prepare();
}

void loadingCadenceAcrossDialogAndClip() {
  Fixture f;
  ui::Ref<Dialog> dialog;
  ui::Ref<ScrollArea> area;
  ui::Ref<DataStateView> state;
  DataStateConfig data;
  ScrollAreaOptions scroll;

  data.spinnerInterval = SPINNER_INTERVAL;
  scroll.horizontal = scroll.vertical = ScrollBarPolicy::NEVER;

  take(ui::mount(*f.scene,
                 ui::overlayHost(ui::label("outside"),
                                 ui::dialog(dialog, "Pending",
                                            ui::scrollArea(area,
                                                           ui::dataStateView(DataPresentation{DataPhase::LOADING},
                                                                             ui::label("retained content"), data)
                                                             .reference(state)
                                                             .height(10),
                                                           scroll)
                                              .height(3)
                                              .width(24)))));
  f.prepare();
  f.idle("initially closed LOADING state scheduled a timer");
  take(dialog.open());
  f.prepare();

  const auto tick = [&] {
    check(f.advance().timers == 1, "visible LOADING state did not have exactly one due timer");
    check(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT, "spinner tick requested layout");
  };

  tick();
  tick();
  take(dialog.dismiss());
  f.prepare();
  f.idle("dismissed LOADING state retained a due timer");
  f.idle("dismissed LOADING state rearmed a timer");
  take(dialog.open());
  f.prepare();
  tick();
  tick();
  take(area.setOffset(ScrollAxis::VERTICAL, {1, 0}));
  f.prepare();
  f.idle("fully clipped status strip retained a timer");
  f.idle("fully clipped status strip rearmed a timer");
  take(area.setOffset(ScrollAxis::VERTICAL, {0, 0}));
  f.prepare();
  tick();
  take(state.display({DataPhase::READY, 1}));
  f.prepare();
  f.idle("READY retained a pending timer");
  f.idle("READY resumed animation while idle");
}

void retryCaptureSurvivesDialogDismissal() {
  Fixture f;
  ui::Ref<Dialog> dialog;
  ui::Ref<DataStateView> state;
  int retries{};
  auto capture = std::make_shared<int>(42);
  std::weak_ptr<int> weak = capture;
  DataStateConfig config;

  config.retry = [&, capture]() -> std::expected<void, TreeError> {
    ++retries;
    take(dialog.dismiss());
    take(f.scene->remove(state.node()));
    check(!weak.expired(), "admitted retry lost its capture during dismissal/removal");

    return {};
  };

  auto recipe = ui::overlayHost(
    ui::label("outside"),
    ui::dialog(dialog, "Retry",
               ui::dataStateView(DataPresentation{DataPhase::FAILED}, ui::label("data"), config).reference(state)));

  config.retry = {};
  capture.reset();
  take(ui::mount(*f.scene, std::move(recipe)));
  f.prepare();
  take(dialog.open());
  f.prepare();

  const auto strip = take(f.app->tree().inspect(state.node())).children.front();
  const auto retry = take(f.app->tree().inspect(strip)).children.back();

  take(f.app->router().invoke(retry, {Action::ACTIVATE}));
  check(retries == 1 && !take(dialog.opened()) && weak.expired(), "retry lifecycle did not complete exactly once");

  const auto repeated = f.app->router().invoke(retry, {Action::ACTIVATE});

  check(!repeated && repeated.error() == TreeError::STALE_NODE, "removed retry was admitted a second time");
  f.prepare();
  f.idle("retired retry composite retained timer work");
}

void terminalIdleHasOneFrame(bool openReady) {
  constexpr std::uint64_t REPLAY_END_MS = 500;
  example::ReplayTerminal terminal{{}, REPLAY_END_MS, VIEWPORT};
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Dialog> dialog;

  take(ui::mount(*scene, ui::overlayHost(ui::label("ordinary body"),
                                         ui::dialog(dialog, "Idle",
                                                    ui::dataStateView(DataPresentation{openReady ? DataPhase::READY
                                                                                                 : DataPhase::LOADING},
                                                                      ui::label("retained body"))))));

  if (openReady) {
    take(dialog.open());
  }

  const auto stats = take(scene->run(terminal));

  check(stats.frames == 1, "closed LOADING or open READY dialog produced idle output frames");
}
} // namespace

int main() {
  try {
    coldForwardAnchorAndRetainedIdentity();
    coldTransformRollbackAndRecovery();
    invalidConfigurationRollback(true);
    invalidConfigurationRollback(false);
    loadingCadenceAcrossDialogAndClip();
    retryCaptureSurvivesDialogDismissal();
    terminalIdleHasOneFrame(false);
    terminalIdleHasOneFrame(true);
    std::cout << "dialog/data-state combined checks PASS\n";

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
