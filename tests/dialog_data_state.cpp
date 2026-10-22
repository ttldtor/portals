// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "package_consumer/virtual_support.hpp"
#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>
#include <portals/ui_scene_resource.hpp>

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

  explicit Fixture(ApplicationOptions options = {}) {
    app = take(Application::create(options, {[](void *value) noexcept {
                                               return *static_cast<SchedulerTime *>(value);
                                             },
                                             &now}));
    scene = take(ManagedScene::create(*app));
  }

  void prepare(CellSize viewport = VIEWPORT) {
    take(scene->prepare(*app, viewport));

    const auto bounds = take(CellRect::from({}, viewport));

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

static_assert(std::same_as<decltype(ui::dialog("Caption", ui::label("body")) | ui::frame()), ui::Description<Border>>);
static_assert(!std::derived_from<Dialog, SizedWidget>);

std::string dialogCaption(Fixture &f, Node<Dialog> node) {
  const auto panel = take(f.app->tree().inspect(node)).children.front();
  const auto column = take(f.app->tree().inspect(panel)).children.front();
  const auto title = take(f.app->tree().inspect(column)).children.front();
  std::string result;

  take(f.app->tree().read(title, [&](const Widget &widget) {
    const auto &label = dynamic_cast<const Label &>(widget);

    check(label.text().policy() == f.scene->widthPolicy(), "dialog caption lost the complete receiving width policy");
    result = label.text().bytes();
  }));

  return result;
}

void dialogScalarParity() {
  std::optional<CellRect> baseline;

  for (int path = 0; path < 5; ++path) {
    Fixture f({.widthPolicy = {.ambiguous = AmbiguousWidth::WIDE, .emoji = EmojiWidth::ONE, .revision = 19}});
    ui::Ref<OverlayHost> host;
    ui::Ref<Dialog> first, second;
    ui::Ref<LineEdit> background;
    DialogOptions actual;
    const DialogOptions expected{.mode = DialogMode::NON_MODAL,
                                 .placement = DialogPlacement::CENTER,
                                 .maximumPanelSize = {14, 6},
                                 .viewportMargin = 0,
                                 .anchorGap = 0,
                                 .closeOnEscape = false,
                                 .frame = {.glyphs = PrimitiveGlyphs::ASCII, .maxPaintCells = 1000}};
    int resolutions{};

    actual.placement = DialogPlacement::ABOVE_START;
    actual.maximumPanelSize = {29, 11};
    actual.viewportMargin = 2;
    actual.anchorGap = 4;
    actual.frame.maxPaintCells = 4096;
    if (path == 4) {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resolutions;

        return "Caption";
      };
      take(options.registry.add<LineEdit>("Background", [&](ui::SceneResourceArguments) {
        return ui::lineEdit(background, "outside");
      }));
      const auto root = take(ui::mount(*f.scene, ui::resource(R"(
        definitions{Shell=OverlayHost{maxLayers=2} Popup=Dialog{title=i18n("caption") dialog{mode=NON_MODAL maximumPanelSize{width=14} frame{glyphs=ASCII}}}}
        defaults{Dialog{dialog{placement=CENTER maximumPanelSize{height=6} viewportMargin=0 anchorGap=0 closeOnEscape=false frame{maxPaintCells=1000}}}}
        scene{Shell{Background{} Popup{Label{"very wide retained panel content"}} Dialog{title="Second" dialog={mode=MODAL closeOnEscape=true} Label{"secondary"}}}}
      )",
                                                              options)));
      const auto children = take(f.app->tree().inspect(root)).children;

      take(ui::detail::NamedAccess::publish(host, *f.scene, root));
      take(ui::detail::NamedAccess::publish(first, *f.scene, children[1]));
      take(ui::detail::NamedAccess::publish(second, *f.scene, children[2]));
    } else {
      auto layer = ui::dialog(path == 0 ? "Caption" : "Actual", ui::label("very wide retained panel content"),
                              path == 0 ? expected : actual);

      if (path == 1) {
        layer = std::move(layer)
                  .title("Caption")
                  .mode(DialogMode::NON_MODAL)
                  .placement(DialogPlacement::CENTER)
                  .maximumPanelSize({14, 6})
                  .viewportMargin(0)
                  .anchorGap(0)
                  .closeOnEscape(false)
                  .frame({.glyphs = PrimitiveGlyphs::ASCII, .maxPaintCells = 1000});
      } else if (path == 2) {
        layer = std::move(layer) | ui::title("Caption") | ui::mode(DialogMode::NON_MODAL) |
                ui::placement(DialogPlacement::CENTER) | ui::maximumPanelSize({14, 6}) | ui::viewportMargin(0) |
                ui::anchorGap(0) | ui::closeOnEscape(false) |
                ui::dialogSettings({.frame = {.glyphs = PrimitiveGlyphs::ASCII, .maxPaintCells = 1000}});
      } else if (path == 3) {
        layer = std::move(layer).variant("Popup");
      }
      auto recipe =
        ui::overlayHost(OverlayHostOptions{.maxLayers = path == 0 ? 2U : 3U}, ui::lineEdit(background, "outside"),
                        std::move(layer).reference(first), ui::dialog(second, "Second", ui::label("secondary")))
          .reference(host);
      ui::ControlContext context;

      if (path == 1) {
        recipe = std::move(recipe).maxLayers(2);
      } else if (path == 2) {
        recipe = std::move(recipe) | ui::maxLayers(2);
      } else if (path == 3) {
        recipe = std::move(recipe).variant("Shell");
        context.define("Shell", "OverlayHost", {.maxLayers = 2});
        context.define("Popup", "Dialog",
                       {.title = text("Caption"),
                        .dialog = {.mode = DialogMode::NON_MODAL,
                                   .placement = DialogPlacement::CENTER,
                                   .maximumPanelSize = {.width = 14, .height = 6},
                                   .viewportMargin = 0,
                                   .anchorGap = 0,
                                   .closeOnEscape = false,
                                   .frame = {.glyphs = PrimitiveGlyphs::ASCII, .maxPaintCells = 1000}}});
      }
      take(ui::mount(*f.scene, std::move(recipe), context));
    }
    take(f.scene->theme(take(parsePss(
      ".dialog.nonmodal { bold: true; } .overlay-host { italic: true; } .dialog-title { underline: true; }"))));
    check(!take(first.opened()) && !take(second.opened()), "resource/native layers did not begin closed");
    check(take(f.app->tree().inspect(host.node())).children.size() == 3,
          "host did not retain exactly one ordinary body followed by both layers");
    take(f.app->tree().read(host.node(), [](const OverlayHost &widget) {
      check(widget.hostOptions().maxLayers == 2, "actual host cap was ignored or replaced by ordinary Overlay options");
    }));
    take(f.app->tree().read(first.node(), [&](const Dialog &widget) {
      const auto options = widget.options();

      check(options.mode == expected.mode && options.placement == expected.placement &&
              options.maximumPanelSize == expected.maximumPanelSize &&
              options.viewportMargin == expected.viewportMargin && options.anchorGap == expected.anchorGap &&
              options.closeOnEscape == expected.closeOnEscape && options.frame.glyphs == expected.frame.glyphs &&
              options.frame.maxPaintCells == expected.frame.maxPaintCells,
            "native/options/method/pipe/resource actual dialog options diverged");
    }));
    check(dialogCaption(f, first.node()) == "Caption", "actual or overridden caption was not retained");
    check(resolutions == (path == 4 ? 1 : 0), "resource title resolution was duplicated or leaked to code paths");
    f.prepare();
    take(background.focus());
    take(first.open());
    check(take(f.app->router().focused()) == background.node(), "NON_MODAL sparse mode stole ordinary body focus");
    f.prepare();
    take(f.app->tree().read(first.node(), [](const Dialog &widget) {
      check(widget.computedStyle() && widget.computedStyle()->normal.bold,
            "sparse dialog identity lost native dialog/nonmodal classes");
    }));
    take(f.app->tree().read(host.node(), [](const OverlayHost &widget) {
      check(widget.computedStyle() && widget.computedStyle()->normal.italic,
            "host settings lost the native overlay-host class");
    }));
    const auto panel = take(f.app->tree().inspect(first.node())).children.front();
    const auto titleColumn = take(f.app->tree().inspect(panel)).children.front();
    const auto heading = take(f.app->tree().inspect(titleColumn)).children.front();

    take(f.app->tree().read(heading, [](const Widget &widget) {
      check(widget.computedStyle() && widget.computedStyle()->normal.underline,
            "derived title preparation lost the native dialog-title class");
    }));
    const auto geometry = take(f.scene->geometry(panel)).local;

    check(take(f.scene->geometry(first.node())).local.size() == VIEWPORT,
          "panel caps narrowed the full-host raw Dialog root");
    check(geometry.size().width <= 14 && geometry.size().height <= 6,
          "independent maximum panel caps were not applied to native geometry");
    if (!baseline) {
      baseline = geometry;
    }
    check(geometry == *baseline, "native/options/method/pipe/resource panel geometry diverged");
    take(f.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    take(f.app->router().dispatch(KeyEvent{.key = InputKey::ESCAPE, .action = KeyAction::RELEASE}));
    check(take(first.opened()), "closeOnEscape=false did not retain the layer");
    take(first.dismiss());
    take(second.open());
    const auto outside = f.app->router().setFocus(background.node());

    check(!outside && outside.error() == TreeError::INVALID_ARGUMENT, "MODAL secondary layer lost outside focus guard");
    take(f.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    take(f.app->router().dispatch(KeyEvent{.key = InputKey::ESCAPE, .action = KeyAction::RELEASE}));
    check(!take(second.opened()), "default/explicit Escape cancellation failed");
    take(first.open());
    f.prepare({1, 1});
    check(take(f.scene->geometry(first.node())).local.size() == CellSize{1, 1},
          "tiny viewport changed raw structural-root extent");
    take(f.scene->remove(host.node()));
    check(!first.opened() && !background.text().get(), "host cleanup retained published child capabilities");
  }
}

void resourceDialogCompositionAndLateCapabilities() {
  Fixture f;
  ui::SceneResourceOptions options;
  ui::Ref<Widget> host, layer;
  ui::Ref<Label> body;
  std::vector<std::string> factories, mounts;
  bool realDialog{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  take(options.registry.add<Label>("Mark", [&](ui::SceneResourceArguments arguments) {
    factories.push_back(arguments.id);
    auto recipe = ui::label(arguments.text);

    if (arguments.id == "body") {
      recipe = std::move(recipe).reference(body);
    }

    return std::move(recipe).setup(
      [&, id = arguments.id](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
        mounts.push_back(id);

        return {};
      });
  }));
  take(options.registry.add<Widget>("Opaque", [&](ui::SceneResourceArguments) {
    factories.push_back("opaque");
    auto recipe = realDialog ? ui::detail::Access::erase(ui::dialog("Actual", ui::label("real managed body")))
                             : ui::detail::Access::erase(ui::label("not a managed dialog"));

    return std::move(recipe).reference(layer);
  }));
  const auto source =
    R"(scene{OverlayHost{Grid{columns=2 Mark#body{"body"} Mark#right{"right"}} Dialog{title="Nested" Grid{columns=2 Mark#one{"one"} DataStateView{showStatus=false Mark#two{"two"}}}} Opaque{}}})";
  auto result = ui::mount(*f.scene, ui::resource(source, options).reference(host));

  check(!result && result.error().sceneResource &&
          result.error().sceneResource->offset == std::string_view(source).find("Opaque"),
        "erased non-dialog layer was accepted or lost owning source attribution");
  check(host.node() == Node<Widget>{} && layer.node() == Node<Widget>{} && body.node() == Node<Label>{} &&
          take(f.app->tree().inspect(f.scene->root())).children.empty(),
        "late capability rejection published partial host/Grid/dialog references");
  check(factories == std::vector<std::string>{"body", "right", "one", "two", "opaque"} &&
          mounts == std::vector<std::string>{"body", "right", "one", "two"},
        "resource composite changed exact factory postorder or mounted a source more than once");
  realDialog = true;
  factories.clear();
  mounts.clear();
  take(ui::mount(*f.scene, ui::resource(source, options).reference(host)));
  f.prepare();
  const auto children = take(f.app->tree().inspect(host.node())).children;
  const auto gridChildren = take(f.app->tree().inspect(children.front())).children;
  const auto left = take(f.scene->geometry(gridChildren.front())).local;
  const auto right = take(f.scene->geometry(gridChildren.back())).local;

  check(right.origin().x >= left.right(), "complete host Grid body lost final source-index binding");
  check(!take(f.scene->dialogOpened(children[1])) && !take(f.scene->dialogOpened(layer.node())),
        "native or erased managed layers did not register as initially closed");
  take(f.scene->openDialog(children[1]));
  f.prepare();
  take(f.scene->dismissDialog(children[1]));
  take(f.scene->remove(host.node()));
  check(!f.scene->dialogOpened(layer.node()), "cleanup retained an erased managed layer record");
}

void lateDialogLayerRemovalAndDeclaredCapability() {
  for (const bool removeHost : {false, true}) {
    Fixture f;
    ui::SceneResourceOptions options;
    ui::Ref<Widget> root;
    int factories{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    if (removeHost) {
      take(options.registry.add<Dialog>("Layer", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::dialog("Actual", ui::label("body"))
          .setup([&](ManagedScene &scene, Node<Dialog> node) -> std::expected<void, ManagedError> {
            const auto snapshot = f.app->tree().inspect(node);

            if (!snapshot) {
              return std::unexpected(ui::detail::error(node, snapshot.error()));
            }

            return scene.remove(snapshot->parent);
          });
      }));
    } else {
      struct UnregisteredDialog final : Dialog {};

      take(options.registry.add<UnregisteredDialog>("Layer", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::custom<UnregisteredDialog>();
      }));
    }
    const auto source = "scene{OverlayHost{Label{} Layer{}}}";
    const auto result = ui::mount(*f.scene, ui::resource(source, options).reference(root));

    check(!result && factories == 1 && result.error().sceneResource &&
            result.error().sceneResource->offset == std::string_view(source).find("Layer"),
          "declared or late-removed dialog layer bypassed checked managed capability/source attribution");
    check(root.node() == Node<Widget>{} && take(f.app->tree().inspect(f.scene->root())).children.empty(),
          "declared or removed layer retained a partial unpublished host");
  }
}

class DialogMeasureProbe final : public Widget {
public:
  explicit DialogMeasureProbe(std::size_t *measures) : measures_(measures) {
  }

  std::expected<CellSize, TreeError> measure(CellSize) override {
    ++*measures_;

    return CellSize{12, 2};
  }

private:
  std::size_t *measures_;
};

void resourceDialogIdle(bool opened) {
  virtualConsumer::ReplayTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  ui::SceneResourceOptions options;
  std::size_t measures{}, settledMeasures{}, settledSubmissions{}, checkpoints{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  take(options.registry.add<DialogMeasureProbe>("Probe", [&](ui::SceneResourceArguments) {
    return ui::custom<DialogMeasureProbe>(&measures);
  }));
  const auto root = take(ui::mount(*scene, ui::resource("scene{OverlayHost{maxLayers=1 Probe{} Dialog{title=\"Idle\" "
                                                        "dialog{maximumPanelSize{width=15 height=6}} Probe{}}}}",
                                                        options)));
  const auto layer = take(app->tree().inspect(root)).children[1];

  if (opened) {
    take(scene->openDialog(layer));
  }
  terminal.checkpoint = [&](std::size_t next, TerminalRead &) {
    if (next == 0) {
      settledMeasures = measures;
      settledSubmissions = terminal.submissions;
    } else {
      check(measures == settledMeasures && terminal.submissions == settledSubmissions,
            "idle dialog composite remeasured or submitted another frame");
    }

    ++checkpoints;
  };
  take(scene->run(terminal));
  check(terminal.closed && checkpoints == 3 && settledMeasures > 0 && settledSubmissions == 1,
        "real dialog idle loop skipped initial preparation/presentation or cleanup");
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
    dialogScalarParity();
    resourceDialogCompositionAndLateCapabilities();
    lateDialogLayerRemovalAndDeclaredCapability();
    resourceDialogIdle(false);
    resourceDialogIdle(true);
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
