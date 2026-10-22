// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace portals;

constexpr std::string_view SOURCE = R"(
definitions { Edit = LineEdit { history { maxOperations=2 } } }
defaults { LineEdit { history { groupTyping=false } } }
scene { VBox {
  Edit#first { text="" }
  Edit#second { text="", history { maxOperations=1 } }
  ThreeState#flag { text="Selection", state=MIXED }
} }
)";

struct SourceFile {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("portals-control-defaults-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");

  ~SourceFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

bool verify(ui::Scene scene) {
  auto first = scene.get<LineEdit>("first");
  auto second = scene.get<LineEdit>("second");
  auto flag = scene.get<ThreeState>("flag");
  Application *application{};

  scene.configure([&](ManagedScene &, Application &app) {
    application = &app;
  });

  if (!scene.prepareChecked() || !application) {
    return false;
  }

  for (const auto value : {"a", "b"}) {
    if (!(first.text() = value).result() || !(second.text() = value).result()) {
      return false;
    }
  }

  if (!first.undo().result() || !first.undo().result() || !second.undo().result()) {
    return false;
  }

  const auto firstValue = first.text().get();
  const auto secondValue = second.text().get();

  if (!firstValue || *firstValue != "" || !secondValue || *secondValue != "a" || first.canUndo() || second.canUndo() ||
      !first.canRedo()) {
    return false;
  }

  for (const auto state : {CheckState::UNCHECKED, CheckState::CHECKED, CheckState::MIXED}) {
    if (!application->router().invoke(flag.node(), {Action::ACTIVATE})) {
      return false;
    }

    bool matches{};
    const auto inspected = application->tree().read(flag.node(), [&](const ThreeState &widget) {
      matches = widget.checkState() == state;
    });

    if (!inspected || !matches) {
      return false;
    }
  }

  return scene.close().has_value() && !first.text().get() && !flag.text().get();
}

bool verifyHelp(ui::Scene scene) {
  auto help = scene.get<HelpView>("guide");
  auto hints = scene.get<KeyHints>("hints");

  if (!scene.prepareChecked() || help.node() == Node<HelpView>{} || hints.node() == Node<KeyHints>{}) {
    return false;
  }

  if (!help.close() || !help.open() || !help.close()) {
    return false;
  }

  const auto helpIdentity = help.node();
  const auto hintsIdentity = hints.node();

  return scene.close().has_value() && help.node() == helpIdentity && hints.node() == hintsIdentity && !help.open();
}

bool verifyDataState(ui::Scene scene) {
  auto state = scene.get<DataStateView>("state");
  auto body = scene.get<Label>("content");
  Application *application{};

  scene.configure([&](ManagedScene &, Application &app) {
    application = &app;
  });

  if (!scene.prepareChecked() || !application || !body.text().get()) {
    return false;
  }

  const auto root = application->tree().inspect(state.node());
  bool ready{};
  const auto inspected = application->tree().read(state.node(), [&](const DataStateView &view) {
    ready = view.presentation() == DataPresentation{};
  });

  if (!root || root->children.size() != 1 || root->children.front().id() != body.node().id() || !inspected || !ready) {
    return false;
  }

  const auto stateIdentity = state.node();

  return scene.close().has_value() && state.node() == stateIdentity && !body.text().get();
}

bool verifyNotifications(ui::Scene scene, ui::NotificationCenter center) {
  auto surface = scene.get<Overlay>("notice-surface");
  auto content = scene.get<Label>("notice-content");

  if (!scene.prepareChecked() || surface.node() == Node<Overlay>{} || !content.text().get()) {
    return false;
  }

  const auto first = center.post(ui::toast("First", {.duration = std::nullopt}));
  const auto second = center.post(ui::toast("Second", {.duration = std::nullopt}));
  const auto third = center.post(ui::toast("Third", {.duration = std::nullopt}));
  const auto snapshot = center.snapshot();

  if (!first || first->status != ToastPostStatus::ACCEPTED_VISIBLE || !second ||
      second->status != ToastPostStatus::ACCEPTED_QUEUED || !third || third->status != ToastPostStatus::DROPPED ||
      !snapshot || (*snapshot)->items.size() != 2 || (*snapshot)->dropped != 1 || !(*snapshot)->reducedMotion) {
    return false;
  }

  const auto surfaceIdentity = surface.node();

  return center.clear() && scene.close().has_value() && !center.snapshot() && surface.node() == surfaceIdentity &&
         !content.text().get() && (*snapshot)->items.size() == 2;
}

bool prepareGeometry(ManagedScene &scene, Application &app) {
  constexpr CellSize VIEWPORT{80, 24};

  if (!scene.prepare(app, VIEWPORT)) {
    return false;
  }

  for (std::size_t pass = 0; pass < ApplicationOptions{}.maxPreparationPasses; ++pass) {
    std::optional<LayoutSnapshot> layout;
    const auto frame = app.tree().frame([&] {
      const auto bounds = CellRect::from({0, 0}, VIEWPORT);
      const auto root = app.tree().root();

      if (!bounds || !root || !layoutTree(app.tree(), *root, *bounds, *bounds)) {
        return;
      }

      auto captured = LayoutSnapshot::capture(app.tree(), scene.root(), LayoutSnapshotProfile::GEOMETRY);

      if (captured) {
        layout = std::move(*captured);
      }
    });

    if (!frame || !layout || !app.tree().consumeFrameRequest() || !scene.prepareViews(app, *layout)) {
      return false;
    }

    const auto requested = app.tree().consumeFrameRequest();

    if (!requested) {
      return false;
    }

    if (*requested != Invalidation::LAYOUT) {
      return scene.completedLayout(app).has_value();
    }
  }

  return false;
}

bool verifyThemeSelector(ui::Scene scene, ui::Ref<ThemeSelector> selector, bool initiallyCommitted = false) {
  auto panel = scene.get<Dialog>("theme-panel");
  auto content = scene.get<Label>("theme-content");
  Application *application{};
  ManagedScene *managed{};

  scene.configure([&](ManagedScene &owner, Application &app) {
    managed = &owner;
    application = &app;
  });

  if (!scene.prepareChecked() || !application || !managed || !prepareGeometry(*managed, *application) ||
      !content.text().get()) {
    return false;
  }

  const auto initial = selector.snapshot();

  if (!initial || (*initial)->catalog->choices.size() != 2 ||
      (initiallyCommitted ? (*initial)->lastCommittedId != "nord" : (*initial)->lastCommittedId.has_value())) {
    return false;
  }

  bool optionsMatch{};
  const auto inspected = application->tree().read(panel.node(), [&](const Dialog &view) {
    optionsMatch = view.options().mode == DialogMode::MODAL && !view.options().closeOnEscape &&
                   view.options().maximumPanelSize == CellSize{56, 18};
  });

  const auto previewedId = initiallyCommitted ? "latte" : "nord";

  if (!inspected || !optionsMatch || !selector.open() || !selector.preview(previewedId)) {
    return false;
  }

  const auto active = selector.snapshot();

  if (!active || (*active)->status != ui::ThemeSelectionStatus::ACTIVE || (*active)->previewedId != previewedId ||
      !selector.cancel()) {
    return false;
  }

  const auto restored = selector.snapshot();
  const auto panelIdentity = panel.node();

  return restored && (!initiallyCommitted || (*restored)->lastCommittedId == "nord") &&
         (*restored)->status == ui::ThemeSelectionStatus::IDLE && scene.close().has_value() && !selector.open() &&
         !content.text().get() && panel.node() == panelIdentity &&
         (*active)->status == ui::ThemeSelectionStatus::ACTIVE && (*active)->catalog->choices.size() == 2;
}

bool verifyDialog(ui::Scene scene) {
  auto host = scene.get<OverlayHost>("host");
  auto details = scene.get<Dialog>("panel");
  auto content = scene.get<Label>("inside");
  Application *application{};
  ManagedScene *managed{};

  scene.configure([&](ManagedScene &owner, Application &app) {
    managed = &owner;
    application = &app;
  });

  if (!scene.prepareChecked() || !application || !managed || !prepareGeometry(*managed, *application) ||
      !content.text().get()) {
    return false;
  }

  bool hostMatches{}, dialogMatches{};
  const auto hostRead = application->tree().read(host.node(), [&](const OverlayHost &view) {
    hostMatches = view.hostOptions().maxLayers == 2;
  });
  const auto dialogRead = application->tree().read(details.node(), [&](const Dialog &view) {
    const auto options = view.options();

    dialogMatches = options.mode == DialogMode::NON_MODAL && options.maximumPanelSize == CellSize{42, 9} &&
                    options.viewportMargin == 2 && !options.closeOnEscape;
  });
  const auto initiallyOpen = details.opened();

  if (!hostRead || !dialogRead || !hostMatches || !dialogMatches || !initiallyOpen || *initiallyOpen ||
      !details.open()) {
    return false;
  }

  const auto opened = details.opened();

  if (!opened || !*opened || !details.dismiss()) {
    return false;
  }

  const auto dismissed = details.opened();

  const auto hostIdentity = host.node();
  const auto dialogIdentity = details.node();

  return dismissed && !*dismissed && scene.close().has_value() && host.node() == hostIdentity &&
         details.node() == dialogIdentity && !details.opened() && !content.text().get();
}

bool verifyMessage(ui::Scene scene, ui::Ref<MessageDialog> question) {
  auto panel = scene.get<Dialog>("message-panel");
  Application *application{};
  ManagedScene *managed{};
  auto answer = std::make_shared<std::optional<MessageResult>>();

  scene.configure([&](ManagedScene &owner, Application &app) {
    managed = &owner;
    application = &app;
  });

  if (!scene.prepareChecked() || !application || !managed || !prepareGeometry(*managed, *application) ||
      panel.node() == Node<Dialog>{}) {
    return false;
  }

  bool matches{};
  const auto inspected = application->tree().read(panel.node(), [&](const Dialog &view) {
    matches = view.options().mode == DialogMode::MODAL && view.options().closeOnEscape &&
              view.options().maximumPanelSize == CellSize{42, 9};
  });

  if (!inspected || !matches ||
      !question
         .open([answer](const MessageResult &result) {
           *answer = result;
         })
         .result() ||
      !question.cancel().result()) {
    return false;
  }

  if (!managed->afterDispatch(*application) || !application->scheduler().dispatch() ||
      !managed->afterDispatch(*application) || !*answer || !**answer || ***answer != MessageChoice::CANCEL) {
    return false;
  }

  const auto latched = question.lastResult();

  const auto panelIdentity = panel.node();

  return latched && **latched && ***latched == MessageChoice::CANCEL && scene.close().has_value() &&
         panel.node() == panelIdentity && !question.lastResult() && **latched && ***latched == MessageChoice::CANCEL;
}
} // namespace

int main() {
  using namespace portals;

  ui::ControlContext controls;
  controls.defaults("LineEdit", {.history = {.groupTyping = false}})
    .define("Edit", "LineEdit", {.history = {.maxOperations = 2}});

  if (!verify(
        ui::scene(ui::vbox(ui::lineEdit("").variant("Edit").id("first"),
                           ui::lineEdit("").variant("Edit").settings({.history = {.maxOperations = 1}}).id("second"),
                           ui::threeState("Selection", CheckState::MIXED).id("flag")),
                  {.controls = controls}))) {
    return 1;
  }

  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;

  if (!verify(ui::scene(SOURCE, options))) {
    return 2;
  }

  SourceFile source;
  {
    std::ofstream file(source.path, std::ios::binary);
    file << SOURCE;

    if (!file) {
      return 3;
    }
  }

  if (!verify(ui::scene(ui::SceneResourceFile{source.path}, options))) {
    return 4;
  }

  ui::Ref<HelpView> help;

  if (!verifyHelp(ui::scene(ui::vbox(ui::keyHints(help).entrySeparator(" | ").overflowMarker("more").id("hints"),
                                     ui::helpView(help)
                                       .initiallyOpen()
                                       .format({.unknownMarker = "[unknown]"})
                                       .emptyText("No commands")
                                       .id("guide"))))) {
    return 5;
  }

  if (!verifyHelp(ui::scene(R"(
scene { VBox {
  KeyHints#hints { help="guide", entrySeparator=" | ", overflowMarker="more" }
  HelpView#guide { initiallyOpen=true, emptyText="No commands", format { unknownMarker="[unknown]" } }
} }
)",
                            options))) {
    return 6;
  }

  if (!verifyDataState(ui::scene(ui::dataStateView(DataPresentation{}, ui::label("body").id("content"))
                                   .showStatus(false)
                                   .statusRows(2)
                                   .spinnerInterval(250)
                                   .labels({.loading = i18n("unused-status")})
                                   .id("state")))) {
    return 7;
  }

  if (!verifyDataState(ui::scene(R"(
scene { DataStateView#state {
  showStatus=false, statusRows=2, spinnerInterval=250
  labels { loading=i18n("unused-status") }
  Label#content { "body" }
} }
)",
                                 options))) {
    return 8;
  }

  if (!verifyDialog(
        ui::scene(ui::overlayHost(
                    ui::button("body").id("body"),
                    ui::dialog("Connection settings", ui::vbox(ui::label("settings").id("inside"), ui::button("Close")))
                      .mode(DialogMode::NON_MODAL)
                      .maximumPanelSize({42, 9})
                      .viewportMargin(2)
                      .closeOnEscape(false)
                      .id("panel"))
                    .maxLayers(2)
                    .id("host")))) {
    return 9;
  }

  if (!verifyDialog(ui::scene(R"(
scene { OverlayHost#host {
  maxLayers=2
  Button#body { "body" }
  Dialog#panel {
    title="Connection settings"
    dialog { mode=NON_MODAL, viewportMargin=2, closeOnEscape=false
             maximumPanelSize { width=42, height=9 } }
    VBox { Label#inside { "settings" } Button { "Close" } }
  }
} }
)",
                              options))) {
    return 10;
  }

  ui::Ref<MessageDialog> nativeQuestion;

  if (!verifyMessage(ui::scene(ui::overlayHost(
                       ui::button("body"), ui::confirmDialog(nativeQuestion, "Continue?", "Use the active connection?")
                                             .maximumPanelSize({42, 9})
                                             .messageSettings({.labels = {.cancel = text("Stay")}, .maxBodyLines = 512})
                                             .id("message-panel"))),
                     nativeQuestion)) {
    return 11;
  }

  ui::Ref<MessageDialog> resourceQuestion;
  ui::SceneResourceOptions messageOptions;
  messageOptions.format = ui::SceneResourceFormat::CONFIGURATION;

  if (!messageOptions.registry.add<Dialog>(
        "Prompt",
        [resourceQuestion](ui::SceneResourceArguments) {
          return ui::confirmDialog(resourceQuestion, "Captured title", "Captured explanation");
        },
        ui::controlPropertySchema("MessageBox"))) {
    return 12;
  }

  if (!verifyMessage(ui::scene(R"(
scene { OverlayHost {
  Button { "body" }
  Prompt#message-panel {
    title="Continue?", body="Use the active connection?"
    dialog { maximumPanelSize { width=42, height=9 } }
    message { labels { cancel="Stay" } maxBodyLines=512 }
  }
} }
)",
                               messageOptions),
                     resourceQuestion)) {
    return 13;
  }

  ui::NotificationCenter nativeNotices;

  if (!verifyNotifications(ui::scene(ui::notifications(nativeNotices, ui::label("Body").id("notice-content"))
                                       .notificationSettings({.capacity = 2,
                                                              .maxVisible = 1,
                                                              .maximumWidth = 32,
                                                              .reducedMotion = true,
                                                              .actionLabelBytes = 64,
                                                              .dedupKeyBytes = 32,
                                                              .maxPaintCells = 512})
                                       .id("notice-surface")),
                           nativeNotices)) {
    return 14;
  }

  ui::NotificationCenter resourceNotices;
  ui::SceneResourceOptions notificationOptions;
  notificationOptions.format = ui::SceneResourceFormat::CONFIGURATION;
  notificationOptions.control<Overlay>(
    "NoticeArea",
    [resourceNotices](ui::SceneResourceArguments) {
      return ui::notifications(resourceNotices, ui::label("Body").id("notice-content"));
    },
    ui::controlPropertySchema("Notifications"));

  if (!verifyNotifications(ui::scene(R"(
scene { NoticeArea#notice-surface {
  notification {
    capacity=2, maxVisible=1, maximumWidth=32, reducedMotion=true
    actionLabelBytes=64, dedupKeyBytes=32, maxPaintCells=512
  }
} }
)",
                                     notificationOptions),
                           resourceNotices)) {
    return 15;
  }

  ui::Ref<ThemeSelector> nativeSelector;

  if (!verifyThemeSelector(
        ui::scene(
          ui::label("Body").id("theme-content"),
          ui::themeSelector(nativeSelector, {{"nord", "Nord", nordTheme}, {"latte", "Latte", catppuccinLatteTheme}})
            .themeCatalogSettings({.maxChoices = 2})
            .title("Appearance")
            .maximumPanelSize({56, 18})
            .themeSelectorSettings({.labels = {.apply = text("Use")}, .openingShortcut = {.enabled = false}})
            .id("theme-panel")),
        nativeSelector)) {
    return 16;
  }

  ui::Ref<ThemeSelector> resourceSelector;
  ui::SceneResourceOptions selectorOptions;
  selectorOptions.format = ui::SceneResourceFormat::CONFIGURATION;
  selectorOptions.control<Dialog>(
    "AppearanceView",
    [resourceSelector](ui::SceneResourceArguments) {
      return ui::themeSelector(resourceSelector,
                               {{"nord", "Nord", nordTheme}, {"latte", "Latte", catppuccinLatteTheme}});
    },
    ui::controlPropertySchema("ThemeSelector"));

  if (!verifyThemeSelector(ui::scene(R"(
scene { OverlayHost {
  Label#theme-content { "Body" }
  AppearanceView#theme-panel {
    title="Appearance"
    dialog { maximumPanelSize { width=56, height=18 } }
    themeSelector { labels { apply="Use" } openingShortcut { enabled=false } }
  }
} }
)",
                                     selectorOptions),
                           resourceSelector)) {
    return 17;
  }

  ui::Ref<ThemeSelector> catalogSelector;
  ui::SceneResourceOptions catalogOptions;
  catalogOptions.format = ui::SceneResourceFormat::CONFIGURATION;
  catalogOptions.control<Dialog>(
    "CatalogPicker",
    [catalogSelector](ui::SceneResourceArguments) {
      return ui::themeSelector(catalogSelector,
                               {{"nord", "Nord", nordTheme}, {"latte", "Latte", catppuccinLatteTheme}});
    },
    ui::controlPropertySchema("ThemeSelectorCatalog"));

  if (!verifyThemeSelector(ui::scene(R"(
scene { OverlayHost {
  Label#theme-content { "Body" }
  CatalogPicker#theme-panel {
    title="Appearance"
    dialog { maximumPanelSize { width=56, height=18 } }
    themeSelector { labels { apply="Use" } openingShortcut { enabled=false } }
    themeCatalog { maxChoices=2 initialId { value="nord" } }
  }
} }
)",
                                     catalogOptions),
                           catalogSelector, true)) {
    return 18;
  }

  return 0;
}
