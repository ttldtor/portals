// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/key_help_state.hpp"
#include "support/managed_text_fixture.hpp"
#include <algorithm>
#include <portals/key_hints.hpp>
#include <portals/style.hpp>
#include <portals/ui_scene_resource.hpp>

namespace portals::detail {
struct ManagedScrollTestAccess {
  static std::uint64_t activation(ManagedScene &scene, Node<Widget> node) {
    return scene.helpActivation(node);
  }

  static std::shared_ptr<KeyHelpRecord> record(ManagedScene &scene, Node<Widget> node) {
    return scene.find(node)->keyHelp;
  }

  static std::expected<void, TreeError> prepare(ManagedScene &scene, const std::shared_ptr<KeyHelpRecord> &record,
                                                const LayoutSnapshot &layout) {
    return scene.prepareKeyHelp(record, layout);
  }
};
} // namespace portals::detail

namespace {
void prepareHelp(Fixture &f, CellSize size) {
  take(f.scene->prepare(*f.app, size));
  for (int pass{}; pass < 3; ++pass) {
    std::optional<LayoutSnapshot> layout;
    take(f.app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, size));
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
      layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
    }));
    (void)take(f.app->tree().consumeFrameRequest());
    take(f.scene->prepareViews(*f.app, *layout));
    if (take(f.app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      break;
    }
  }
  take(f.scene->completedLayout(*f.app));
}

struct StyleSink final : PaintContext {
  std::vector<CellStyle> text;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *value = std::get_if<TextCommand>(&command)) {
        text.push_back(value->style);
      }
    }
    return {};
  }
};

void checkPaintStyle(Fixture &f, Node<Widget> node, CellStyle expected,
                     std::source_location where = std::source_location::current()) {
  StyleSink sink;
  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(node, sink));
  }));
  check(!sink.text.empty(), "styled key help emits text", where);
  check(std::ranges::all_of(sink.text,
                            [&](const CellStyle &style) {
                              return style == expected;
                            }),
        "every key help text command uses the expected style", where);
}

void scalarPresentationParity() {
  std::string baseline;
  std::optional<CellRect> baselineHints, baselineHelp;
  for (int mode{}; mode < 4; ++mode) {
    Fixture f;
    ui::Ref<HelpView> help;
    ui::Ref<KeyHints> hints;
    Node<Widget> helpNode, hintsNode;
    KeyFormatOptions formatter;
    formatter.strokeSeparator = " -> ";
    formatter.alternativeSeparator = " | ";
    formatter.unknownMarker = " [unknown]";
    if (mode == 0) {
      take(ui::mount(*f.scene, ui::column(ui::keyHints(help, {.format = formatter,
                                                              .entrySeparator = " / ",
                                                              .overflowMarker = "!",
                                                              .emptyText = "none",
                                                              .unavailableText = "lost"})
                                            .reference(hints),
                                          ui::helpView(help, {.initiallyOpen = true,
                                                              .format = formatter,
                                                              .emptyText = "none",
                                                              .unavailableText = "lost"}))));
    } else if (mode == 1) {
      take(ui::mount(
        *f.scene,
        ui::column(ui::keyHints(help)
                     .format({.strokeSeparator = " -> ", .alternativeSeparator = " | ", .unknownMarker = " [unknown]"})
                     .entrySeparator(" / ")
                     .overflowMarker("!")
                     .emptyText("none")
                     .unavailableText("lost")
                     .reference(hints),
                   ui::helpView(help)
                     .initiallyOpen()
                     .format({.strokeSeparator = " -> ", .alternativeSeparator = " | ", .unknownMarker = " [unknown]"})
                     .emptyText("none")
                     .unavailableText("lost"))));
    } else if (mode == 2) {
      auto full =
        ui::helpView(help) | ui::initiallyOpen() |
        ui::format({.strokeSeparator = " -> ", .alternativeSeparator = " | ", .unknownMarker = " [unknown]"}) |
        ui::emptyText("none") | ui::unavailableText("lost");
      auto compact =
        ui::keyHints(help) |
        ui::format({.strokeSeparator = " -> ", .alternativeSeparator = " | ", .unknownMarker = " [unknown]"}) |
        ui::entrySeparator(" / ") | ui::overflowMarker("!") | ui::emptyText("none") | ui::unavailableText("lost");
      take(ui::mount(*f.scene, ui::column(std::move(compact).reference(hints), std::move(full))));
    } else {
      auto root = take(ui::mount(*f.scene, ui::resource(R"(
        defaults{ HelpView{format{strokeSeparator=" -> " alternativeSeparator=" | " unknownMarker=" [unknown]"}
          emptyText="none" unavailableText="lost"} KeyHints{format{strokeSeparator=" -> " alternativeSeparator=" | " unknownMarker=" [unknown]"}
          emptyText="none" unavailableText="lost" entrySeparator=" / " overflowMarker="!"} }
        definitions{Guide=HelpView{initiallyOpen=true}}
        scene{VBox{KeyHints{help="guide"} Guide#guide{}}}
      )",
                                                        {.format = ui::SceneResourceFormat::CONFIGURATION})));
      const auto children = take(f.app->tree().inspect(root)).children;
      check(children.size() == 2, "resource key-help nodes remain leaves in their source parent");
      hintsNode = children[0];
      helpNode = children[1];
    }
    if (mode != 3) {
      helpNode = help.node();
      hintsNode = hints.node();
    }
    auto full = portals::detail::ManagedScrollTestAccess::record(*f.scene, helpNode);
    auto compact = portals::detail::ManagedScrollTestAccess::record(*f.scene, hintsNode);
    check(full && full->full && compact && compact->hints && compact->help == helpNode,
          "all input paths retain native full and compact records and the exact target");
    check(full->full->initiallyOpen && take(f.app->tree().inspect(helpNode)).visible,
          "initial-open passes through the native finalizer");
    take(f.scene->onKey(ui::press("Ctrl+K, C"), KeyInfo(TextValue("Comment")), [] {
    }));
    take(f.scene->onKey(ui::press("F2"), KeyInfo(TextValue("Rename")), [] {
    }));
    prepareHelp(f, {100, 12});
    check(compact->presentation->current && compact->presentation->text && compact->presentation->text->caption,
          "configured compact view builds a real canonical binding projection");
    const auto caption = std::string(compact->presentation->text->caption->bytes());
    check(caption.find(" -> ") != std::string::npos && caption.find(" / ") != std::string::npos,
          "owning formatter and entry separator reach rendered projection");
    if (!mode) {
      baseline = caption;
    } else {
      check(caption == baseline, "options methods pipes and CFG project the same caption");
    }
    take(f.app->tree().frame([&] {
      auto layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
      const auto hintsBounds = take(layout.geometry(hintsNode)).geometry.local;
      const auto helpBounds = take(layout.geometry(helpNode)).geometry.local;
      if (!mode) {
        baselineHints = hintsBounds;
        baselineHelp = helpBounds;
      } else {
        check(hintsBounds == *baselineHints && helpBounds == *baselineHelp,
              "options methods pipes and CFG retain the same actual native layout rectangles");
      }
    }));
    auto text = compact->presentation->text;
    auto projection = compact->presentation->projection;
    prepareHelp(f, {100, 12});
    check(compact->presentation->text == text && compact->presentation->projection == projection,
          "unchanged configured views retain their checked projection without idle formatting");
    check(full->full->emptyText == "none" && compact->hints->overflowMarker == "!",
          "configured options remain owning native settings");
  }
}

void scalarDefaultsAndLimits() {
  {
    Fixture f;
    ui::Ref<HelpView> help;
    ui::Ref<KeyHints> hints;
    ui::ControlContext controls;
    controls.defaults("HelpView", {.format = {.strokeSeparator = "context"}})
      .defaults("KeyHints", {.format = {.strokeSeparator = "context"}})
      .define("Guide", "HelpView", {.initiallyOpen = false});
    take(ui::mount(*f.scene,
                   ui::column(ui::keyHints(help, {.entrySeparator = "actual"}).reference(hints),
                              ui::helpView(help, {.format = {.maxBytesPerBinding = 123}}).variant("Guide")),
                   controls));
    auto full = portals::detail::ManagedScrollTestAccess::record(*f.scene, help.node());
    auto compact = portals::detail::ManagedScrollTestAccess::record(*f.scene, hints.node());
    check(full->full->emptyText == "No key bindings" && full->full->unavailableText == "Help unavailable" &&
            compact->hints->emptyText.empty() && compact->hints->unavailableText == "?",
          "omitted labels retain distinct native defaults");
    check(full->full->format.maxBytesPerBinding == 123 && compact->hints->entrySeparator == "actual" &&
            full->full->format.strokeSeparator == "context",
          "sparse defaults overlay actual receiving constructor options");
  }
  for (int mode{}; mode < 3; ++mode) {
    Fixture f({.maxKeyProjectionBytes = 8});
    ui::Ref<HelpView> help;
    const auto before = take(f.app->tree().inspect(f.scene->root())).children;
    auto recipe = mode == 0   ? ui::helpView(help, {.emptyText = "ninebytes"})
                  : mode == 1 ? ui::helpView(help).emptyText("ninebytes")
                              : ui::helpView(help) | ui::emptyText("ninebytes");
    auto result = ui::mount(*f.scene, std::move(recipe));
    check(!result && treeCause(result.error(), TreeError::NODE_LIMIT) && help.node() == Node<HelpView>{} &&
            take(f.app->tree().inspect(f.scene->root())).children == before,
          "native cap rejects before retaining nodes or publishing refs");
  }
  for (const bool compact : {false, true}) {
    Fixture f;
    ui::Ref<HelpView> help;
    auto invalid = std::string(1, static_cast<char>(0xff));
    auto result =
      compact
        ? ui::mount(*f.scene, ui::detail::Access::erase(ui::keyHints(help, {.format = {.unknownMarker = invalid}})))
        : ui::mount(*f.scene, ui::detail::Access::erase(ui::helpView(help, {.format = {.unknownMarker = invalid}})));
    check(!result && treeCause(result.error(), TreeError::INVALID_ARGUMENT) && help.node() == Node<HelpView>{},
          "native owning formatter literals validate UTF-8 before retained allocation");
  }
  {
    Fixture f;
    ui::Ref<HelpView> help;
    auto result = ui::mount(*f.scene, ui::helpView(help, {.unavailableText = std::string(1, static_cast<char>(0xff))}));
    check(!result && result.error().text && help.node() == Node<HelpView>{},
          "direct HelpView invalid unavailable marker retains its original low-level text cause");
  }
}

void formatterCapRemainsNative() {
  Fixture f;
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> hints;
  take(ui::mount(
    *f.scene, ui::column(ui::keyHints(help).format({.maxBytesPerBinding = 1}).reference(hints), ui::helpView(help))));
  take(f.scene->onKey(ui::press("Ctrl+S"), KeyInfo(TextValue("Save")), [] {
  }));
  int diagnostics{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++diagnostics;
  }));
  prepareHelp(f, {40, 8});
  auto record = portals::detail::ManagedScrollTestAccess::record(*f.scene, hints.node());
  check(!record->presentation->current && record->presentation->unavailable.bytes() == "?" && diagnostics == 1,
        "positive row cap is enforced by the native formatter and existing unavailable presentation");
  prepareHelp(f, {40, 8});
  check(diagnostics == 1, "unchanged row-cap failure does not repeat idle diagnostics");
}

void materializedHelpHandoff() {
  for (const std::size_t budget : {2U, 3U}) {
    for (const bool addLeaf : {false, true}) {
      Fixture f;
      ui::Ref<HelpView> help;
      ui::ControlSettings source{.format = {.strokeSeparator = "precharged"}, .emptyText = "source"};
      auto staged = ui::detail::Access::cold<Widget>(
        [=](ManagedScene &scene, Node<Widget> parent,
            ui::detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
          context.controlLeaves += 2;
          auto patch = source;
          if (addLeaf) {
            patch.unavailableText = "added";
          }
          auto recipe = ui::helpView(help).settings(std::move(patch));
          ui::detail::Access::preparedControlPresence(recipe, ui::detail::controlSettingsPresenceSet(source));
          auto result = ui::detail::Access::mount(std::move(recipe), scene, parent, context);
          if (!result) {
            return std::unexpected(result.error());
          }
          return Node<Widget>(*result);
        });
      const auto before = take(f.app->tree().inspect(f.scene->root())).children;
      auto result = ui::mount(*f.scene, std::move(staged), ui::ControlContext({.maxMaterializedLeaves = budget}));
      if (addLeaf && budget == 2) {
        check(!result && result.error().sceneResource &&
                result.error().sceneResource->code == SceneResourceErrorCode::MATERIALIZATION_LIMIT,
              "a genuinely new high-word leaf exhausts the precharged aggregate budget");
        check(help.node() == Node<HelpView>{} && take(f.app->tree().inspect(f.scene->root())).children == before,
              "failed high-word handoff publishes no reference and retains no subtree");
      } else {
        take(std::move(result));
        auto record = portals::detail::ManagedScrollTestAccess::record(*f.scene, help.node());
        check(record->full->emptyText == "source" && record->full->format.strokeSeparator == "precharged" &&
                record->full->unavailableText == (addLeaf ? "added" : "Help unavailable"),
              "materialized leaves in both words are consumed once while a new leaf is applied");
      }
    }
  }
}

void localHelpReferences() {
  for (const bool good : {false, true}) {
    Fixture f;
    ui::Ref<HelpView> help;
    ui::Ref<Label> label;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    int callbacks{};
    take(options.registry.add<Widget>("Erased", [&](ui::SceneResourceArguments) {
      ++callbacks;
      if (good) {
        return ui::detail::Access::erase(ui::helpView(help));
      }
      return ui::detail::Access::erase(ui::label(label, "wrong"));
    }));
    const auto before = take(f.app->tree().inspect(f.scene->root())).children;
    const std::string source = "scene{VBox{KeyHints{help=\"target\"} Erased#target{}}}";
    auto result = ui::mount(*f.scene, ui::resource(source, options));
    check(callbacks == 1, "explicit unknown target executes exactly one real code factory");
    if (good) {
      auto children = take(f.app->tree().inspect(take(std::move(result)))).children;
      auto hints = portals::detail::ManagedScrollTestAccess::record(*f.scene, children[0]);
      check(help.node() != Node<HelpView>{} && hints->help == help.node(),
            "erased help target succeeds through the native checked-record finalizer");
    } else {
      check(!result && treeCause(result.error(), TreeError::INVALID_ARGUMENT) && result.error().sceneResource &&
              result.error().sceneResource->propertyPath &&
              *result.error().sceneResource->propertyPath == "KeyHints.help" &&
              result.error().sceneResource->offset == source.find("help=") && !result.error().cleanup,
            "late wrong-capability failure retains owning help source and its primary error");
      check(label.node() == Node<Label>{} && take(f.app->tree().inspect(f.scene->root())).children == before,
            "late target failure rolls back all actual nodes and pending typed observations");
      take(ui::mount(*f.scene, ui::label(label, "reused")));
      check(label.node() != Node<Label>{}, "rolled-back observer can be reused immediately");
    }
  }
  {
    Fixture f;
    ui::Ref<Button> stable;
    ui::Ref<Label> unpublished;
    take(ui::mount(*f.scene, ui::button(stable, "Existing", [] {
    })));
    take(f.scene->focus(stable.node()));
    const auto before = take(f.app->tree().inspect(f.scene->root())).children;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Widget>("Wrong", [unpublished](ui::SceneResourceArguments) {
      return ui::detail::Access::erase(ui::label(unpublished, "wrong"));
    }));
    auto result = ui::mount(
      *f.scene,
      ui::resource("scene{VBox{HelpView{initiallyOpen=true} KeyHints{help=\"wrong\"} Wrong#wrong{}}}", options));
    check(!result && result.error().sceneResource && !result.error().cleanup && unpublished.node() == Node<Label>{} &&
            take(f.app->tree().inspect(f.scene->root())).children == before &&
            take(f.app->router().focused()) == stable.node(),
          "late target rejection after initial-open unwinds the help focus scope and preserves the earlier invoker");
  }
  {
    Fixture f;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Widget>("Unknown", [](ui::SceneResourceArguments) {
      return ui::detail::Access::erase(ui::label("ordinary"));
    }));
    auto root = take(ui::mount(*f.scene, ui::resource("scene{VBox{KeyHints{} Unknown{} HelpView{}}}", options)));
    auto children = take(f.app->tree().inspect(root)).children;
    check(portals::detail::ManagedScrollTestAccess::record(*f.scene, children[0])->help == children[2],
          "one declared help plus an unknown factory is not implicit ambiguity");
  }
}

void helpBodyStyles() {
  ControlStyle manual;
  manual.normal.foreground = RgbColor{1, 2, 3};
  manual.normal.bold = true;
  {
    Fixture f;
    auto presentation = std::make_shared<portals::detail::KeyHelpPresentation>(take(GraphemeText::from("Unavailable")));
    auto body = take(f.scene->emplace<portals::detail::HelpBody>(f.scene->root(), presentation, manual));
    take(f.app->tree().read(body, [&](const Widget &widget) {
      check(!widget.computedStyle(), "unregistered help body exercises manual fallback");
    }));
    take(f.app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, {30, 8}));
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    }));
    checkPaintStyle(f, body, manual.normal);
  }
  {
    Fixture f;
    ui::Ref<HelpView> help;
    take(ui::mount(*f.scene, ui::helpView(help, {.initiallyOpen = true}, manual)));
    prepareHelp(f, {30, 8});
    const auto body = portals::detail::ManagedScrollTestAccess::record(*f.scene, help.node())->body;
    take(f.app->tree().read(body, [&](const Widget &widget) {
      check(widget.computedStyle() && widget.computedStyle()->normal == manual.normal,
            "managed help body registers its manual defaults without a theme");
    }));
    checkPaintStyle(f, body, manual.normal);
    for (const auto source :
         {"* { foreground: #123456; bold: false; }", "HelpView.help-body { foreground: #123456; bold: false; }"}) {
      take(f.scene->theme(take(parsePss(source))));
      prepareHelp(f, {30, 8});
      auto expected = manual.normal;
      expected.foreground = RgbColor{0x12, 0x34, 0x56};
      expected.bold = false;
      take(f.app->tree().read(body, [&](const Widget &widget) {
        check(widget.computedStyle() && widget.computedStyle()->normal == expected,
              "wildcard and typed help-body themes resolve through managed styles");
      }));
      checkPaintStyle(f, body, expected);
    }
  }
}

void helpBodyMountRollback() {
  Fixture f;
  ui::Ref<HelpView> help;
  Node<Widget> retiredBody;
  const auto previous = take(f.app->tree().inspect(f.scene->root())).children;
  auto failed = ui::mount(
    *f.scene,
    ui::helpView(help).setup([&](ManagedScene &scene, Node<HelpView> node) -> std::expected<void, ManagedError> {
      retiredBody = portals::detail::ManagedScrollTestAccess::record(scene, node)->body;
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::CALLBACK_FAILED, {}, {}});
    }));
  check(!failed && treeCause(failed.error(), TreeError::CALLBACK_FAILED) && !failed.error().cleanup,
        "failure after body style registration preserves the original mount error");
  check(help.node() == Node<HelpView>{}, "failed help mount publishes no reference");
  check(take(f.app->tree().inspect(f.scene->root())).children == previous,
        "failed help mount restores the earlier scene children");
  check(!f.app->tree().inspect(retiredBody), "failed help mount retires the styled body");
  take(f.scene->theme(take(parsePss("HelpView.help-body { bold: true; }"))));
  take(ui::mount(*f.scene, ui::helpView(help, {.initiallyOpen = true})));
  prepareHelp(f, {30, 8});
  const auto body = portals::detail::ManagedScrollTestAccess::record(*f.scene, help.node())->body;
  CellStyle expected;
  expected.bold = true;
  checkPaintStyle(f, body, expected);
}

void disabledHintsStyles() {
  for (const bool themed : {false, true}) {
    Fixture f;
    ui::Ref<HelpView> help;
    ui::Ref<KeyHints> hints;
    ui::Ref<Column> ancestor;
    ControlStyle manual;
    manual.normal.foreground = RgbColor{1, 2, 3};
    manual.focused.foreground = RgbColor{4, 5, 6};
    manual.disabled.foreground = RgbColor{7, 8, 9};
    take(ui::mount(*f.scene, ui::column(ui::column(ui::keyHints(help, {}, manual).reference(hints)).reference(ancestor),
                                        ui::helpView(help))));
    for (const auto key : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
      take(f.scene->onKey(ui::press(key), KeyInfo(TextValue("A long named action")), [] {
      }));
    }
    if (themed) {
      take(f.scene->theme(take(parsePss("KeyHints { bold: false; } KeyHints:disabled { bold: true; }"))));
    }
    prepareHelp(f, {1, 6});
    const auto record = portals::detail::ManagedScrollTestAccess::record(*f.scene, hints.node());
    check(record->presentation->text->overflow && take(f.app->tree().inspect(hints.node())).enabled,
          "overflow strip has a locally enabled action before ancestor disabling");
    take(f.scene->focus(hints.node()));
    prepareHelp(f, {1, 6});
    auto focusedStyle = manual.focused;
    if (themed) {
      focusedStyle.bold = false;
    }
    checkPaintStyle(f, hints.node(), focusedStyle);
    take(f.app->tree().setEnabled(ancestor.node(), false));
    prepareHelp(f, {1, 6});
    auto expected = manual.disabled;
    expected.bold = themed;
    take(f.app->tree().read(hints.node(), [&](const KeyHints &widget) {
      check(!widget.enabled() && widget.computedStyle() && widget.computedStyle()->disabled == expected,
            "disabled ancestor synchronizes effective eligibility and disabled style");
    }));
    check(take(f.app->tree().inspect(hints.node())).enabled && record->presentation->text->overflow,
          "ancestor disabling preserves local overflow action state");
    checkPaintStyle(f, hints.node(), expected);
    take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    check(!take(f.app->tree().inspect(help.node())).visible, "disabled overflow cannot open help");
  }
}

void lifecycle() {
  Fixture f;
  ui::Ref<Button> invoker;
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> hints;
  take(ui::mount(*f.scene, ui::column(ui::button(invoker, "Document",
                                                 [] {
                                                 }),
                                      ui::keyHints(help).reference(hints), ui::helpView(help))));
  int saves{};
  take(f.scene->onKey(ui::press("Ctrl+S"), KeyInfo(TextValue("Save e\xcc\x81 wide \xe7\x95\x8c")), [&] {
    ++saves;
  }));
  take(f.scene->onKey(ui::press("F1"), KeyInfo(TextValue("Help")), [help] {
    return help.open();
  }));
  KeyInfo close(TextValue("Close help"));
  close.scope = help.node();
  take(f.scene->onKey(ui::press("Escape"), std::move(close), [help] {
    return help.close();
  }));
  take(f.scene->focus(invoker.node()));
  check(!take(f.app->tree().inspect(help.node())).visible, "help initially hidden with no implicit opening key");
  for (const auto size : {CellSize{}, CellSize{1, 1}, CellSize{20, 8}}) {
    prepareHelp(f, size);
    take(f.scene->input(*f.app, KeyEvent{InputKey::F1, 0, {}, KeyAction::PRESS}));
    check(take(f.app->tree().inspect(help.node())).visible, "application help key works independently of strip width");
    take(help.open());
    const auto focus = take(f.app->router().focused());
    take(help.open());
    check(take(f.app->router().focused()) == focus, "duplicate open never steals focus or pushes a return record");
    prepareHelp(f, size);
    take(help.close());
    check(!take(f.app->tree().inspect(help.node())).visible && take(f.app->router().focused()) == invoker.node(),
          "ordinary close returns exact surviving invoker");
  }
  take(help.open());
  const auto first = portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node());
  take(f.app->tree().setVisible(help.node(), false));
  (void)take(f.app->router().focused());
  take(f.app->tree().setVisible(help.node(), true));
  take(f.app->router().enterFocusScope(help.node()));
  const auto other = portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node());
  check(first && other > first, "lazy unwind and re-enter publish a distinct activation receipt");
  take(help.close());
  check(take(f.app->tree().inspect(help.node())).visible &&
          portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()) == other,
        "old help close never consumes or hides a new same-root activation (ABA)");
  take(f.app->router().leaveFocusScope(help.node()));
  take(help.open());
  take(help.close());
  check(saves == 0, "help rows are projections and never execute commands");
}

void exhaustion() {
  Fixture f;
  ui::Ref<Button> invoker;
  ui::Ref<Column> ordinary;
  ui::Ref<HelpView> help;
  take(ui::mount(*f.scene, ui::column(ui::column(ui::button(invoker, "Ordinary",
                                                            [] {
                                                            }))
                                        .reference(ordinary)
                                        .focusScope(),
                                      ui::helpView(help))));
  take(f.scene->focus(invoker.node()));
  portals::detail::EventRouterAccess::exhaustFocusReturnSerial(f.app->router());
  auto failed = help.open();
  check(!failed && treeCause(failed.error(), TreeError::IDENTITY_EXHAUSTED) &&
          !take(f.app->tree().inspect(help.node())).visible,
        "HelpView rejects unavailable receipt before visibility/focus publication");
  take(f.app->router().enterFocusScope(ordinary.node()));
  take(f.app->router().leaveFocusScope(ordinary.node()));
  check(take(f.app->router().focused()) == invoker.node(),
        "receipt exhaustion leaves legacy low-level focus behavior intact");
}

void overflowAndRetry() {
  Fixture f;
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> hints;
  ui::Ref<Button> stable;
  take(ui::mount(*f.scene, ui::column(ui::button(stable, "Stable",
                                                 [] {
                                                 }),
                                      ui::keyHints(help).reference(hints), ui::helpView(help))));
  for (const auto key : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
    KeyInfo info(TextValue("Combining e\xcc\x81 and wide \xe7\x95\x8c"));
    info.description = TextValue("First line\nSecond line\nThird line");
    take(f.scene->onKey(ui::press(key), std::move(info), [] {
    }));
  }
  prepareHelp(f, {1, 6});
  auto compact = portals::detail::ManagedScrollTestAccess::record(*f.scene, hints.node());
  auto full = portals::detail::ManagedScrollTestAccess::record(*f.scene, help.node());
  check(compact->presentation->current && compact->presentation->text->caption->bytes() == "+" &&
          compact->presentation->text->overflow == 8 && take(f.app->tree().inspect(hints.node())).enabled,
        "one-cell overflow reserves a whole visible marker with exact omitted row count");
  check(!full->presentation->projection && !full->presentation->text,
        "hidden full help performs no projection or formatting");
  take(f.scene->focus(hints.node()));
  take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  check(take(f.app->tree().inspect(help.node())).visible, "focused overflow Enter opens the checked help target");
  prepareHelp(f, {20, 6});
  check(take(f.app->router().dispatch(KeyEvent{InputKey::PAGE_DOWN})).status == DispatchStatus::HANDLED,
        "focusable full body bubbles page navigation into its existing ScrollArea");
  take(help.close());
  check(take(f.app->router().focused()) == hints.node(), "overflow remains the exact focus-return invoker");
  auto previous = compact->presentation->projection;
  auto previousText = compact->presentation->text;
  take(f.scene->focus(stable.node()));
  take(f.scene->setKeyCapabilities({KeySupport::SUPPORTED, KeySupport::UNKNOWN, KeySupport::UNKNOWN}));
  take(f.app->tree().setEnabled(hints.node(), false));
  int notifications{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  // Fail the new owning projection in view preparation, outside the allocating layout fixture.
  std::optional<LayoutSnapshot> layout;
  take(f.app->tree().frame([&] {
    const auto bounds = take(CellRect::from({}, {20, 6}));
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
  }));
  allocationTest::remaining = 0;
  auto failed = portals::detail::ManagedScrollTestAccess::prepare(*f.scene, compact, *layout);
  allocationTest::remaining = -1;
  take(failed);
  check(!compact->presentation->current && compact->presentation->projection == previous &&
          compact->presentation->text == previousText && notifications == 1,
        "failed fresh projection keeps the coherent last checked pair behind one unavailable marker");
  prepareHelp(f, {20, 6});
  const auto notified = notifications;
  prepareHelp(f, {20, 6});
  check(!compact->presentation->current && notifications == notified,
        "same failed input does not repeat a diagnostic or idle rebuild");
  (void)take(f.scene->keyBindings());
  prepareHelp(f, {20, 6});
  check(compact->presentation->current, "explicit checked query permits retry of the failed view revision");
}

Fixture *receiptFixture{};
Node<Widget> receiptHelp;
bool exhaustedDuringStage{};
bool receiptAlreadyIssued{};

void exhaustDuringStage() {
  if (portals::detail::EventRouterAccess::requiredFocusReceipt(receiptFixture->app->router()) != Node<Widget>{}) {
    exhaustedDuringStage = true;
    receiptAlreadyIssued =
      portals::detail::ManagedScrollTestAccess::activation(*receiptFixture->scene, receiptHelp) != 0;
    portals::detail::EventRouterAccess::exhaustFocusReturnSerial(receiptFixture->app->router());
  }
}

void retainedProjectionBudget() {
  const auto oneRow = sizeof(KeyBindingView) + std::string_view("Save").size() + sizeof(key::Chord) + sizeof(KeyAction);
  ManagedOptions options;
  options.maxKeyProjectionBytes = 2 * oneRow + 128;
  Fixture f(options);
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> first, second;
  take(ui::mount(*f.scene, ui::column(ui::keyHints(help).reference(first), ui::keyHints(help).reference(second),
                                      ui::helpView(help))));
  auto binding = take(f.scene->onKey(ui::press("Ctrl+S"), KeyInfo(TextValue("Save")), [] {
  }));
  prepareHelp(f, {80, 6});
  auto a = portals::detail::ManagedScrollTestAccess::record(*f.scene, first.node());
  auto b = portals::detail::ManagedScrollTestAccess::record(*f.scene, second.node());
  if (!a->presentation->current || !b->presentation->current ||
      a->presentation->projection != b->presentation->projection) {
    std::cerr << "resident initial: current=" << a->presentation->current << ',' << b->presentation->current
              << " bytes=" << a->projectionBytes << ',' << b->projectionBytes << " text=" << a->textBytes << ','
              << b->textBytes << " same=" << (a->presentation->projection == b->presentation->projection)
              << " budget=" << options.maxKeyProjectionBytes << '\n';
  }
  check(a->presentation->current && b->presentation->current &&
          a->presentation->projection == b->presentation->projection,
        "two projections sharing one owning DTO charge that DTO only once");
  take(binding.setText(TextValue("Save v2"), TextValue("")));
  (void)take(f.scene->keyBindings());
  std::optional<LayoutSnapshot> layout;
  take(f.app->tree().frame([&] {
    const auto bounds = take(CellRect::from({}, {80, 6}));
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
  }));
  take(portals::detail::ManagedScrollTestAccess::prepare(*f.scene, a, *layout));
  check(a->presentation->projection != b->presentation->projection,
        "one updated view retains the other last checked DTO");
  const auto previousA = a->presentation->projection;
  const auto previousB = b->presentation->projection;
  take(binding.setText(TextValue("Save v3"), TextValue("")));
  auto third = f.scene->keyBindings();
  check(!third && treeCause(third.error(), TreeError::NODE_LIMIT) && a->presentation->projection == previousA &&
          b->presentation->projection == previousB,
        "aggregate resident budget includes every distinct historical DTO retained by existing views");
}

void receiptGuardAndFocusErrors() {
  bool allocationSuccess{};
  bool allocationFailure{};
  for (std::ptrdiff_t point{}; point < 256; ++point) {
    Fixture f;
    ui::Ref<HelpView> help;
    take(ui::mount(*f.scene, ui::helpView(help)));
    allocationTest::remaining = point;
    auto opened = help.open();
    allocationTest::remaining = -1;
    if (opened) {
      allocationSuccess = true;
      break;
    }
    allocationFailure = true;
    check(treeCause(opened.error(), TreeError::NO_MEMORY) && !take(f.app->tree().inspect(help.node())).visible &&
            !portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()) &&
            portals::detail::EventRouterAccess::requiredFocusReceipt(f.app->router()) == Node<Widget>{},
          "prepublication allocation failure hides help and restores the receipt guard without a return record");
  }
  check(allocationSuccess && allocationFailure,
        "finite HelpView publication allocation sweep reaches failure and success");
  bool reachedGuard{};
  bool reachedPublished{};
  bool completed{};
  for (std::ptrdiff_t point{}; point < 256; ++point) {
    Fixture f;
    ui::Ref<HelpView> help;
    take(ui::mount(*f.scene, ui::helpView(help)));
    receiptFixture = &f;
    receiptHelp = help.node();
    exhaustedDuringStage = false;
    receiptAlreadyIssued = false;
    allocationTest::hookAfter = point;
    allocationTest::hook = exhaustDuringStage;
    auto opened = help.open();
    const bool hookRemaining = allocationTest::hook != nullptr;
    allocationTest::hook = nullptr;
    check(portals::detail::EventRouterAccess::requiredFocusReceipt(f.app->router()) == Node<Widget>{},
          "checked open restores its dynamic receipt requirement on every exit");
    if (exhaustedDuringStage) {
      if (receiptAlreadyIssued) {
        reachedPublished = true;
        check(opened.has_value() && take(f.app->tree().inspect(help.node())).visible &&
                portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
              "later exhaustion never revokes an already issued exact activation receipt");
      } else {
        reachedGuard = true;
        check(!opened && treeCause(opened.error(), TreeError::IDENTITY_EXHAUSTED) &&
                !take(f.app->tree().inspect(help.node())).visible &&
                !portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
              "exhaustion after staging still rejects before exact return publication");
      }
    }
    if (hookRemaining) {
      completed = true;
      break;
    }
  }
  check(completed && reachedGuard && reachedPublished,
        "finite exhaustion sweep reaches both sides of exact receipt publication and completion");

  Fixture f;
  ui::Ref<HelpView> help;
  take(ui::mount(*f.scene, ui::helpView(help)));
  auto invoker = f.node();
  bool failLoss{}, failGain{}, blocked{};
  auto binding = take(f.app->router().bind(
    invoker,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool gain) -> std::expected<void, TreeError> {
      if (!gain && failLoss) {
        auto nested = help.close();
        blocked = !nested && treeCause(nested.error(), TreeError::WOULD_BLOCK);
        return std::unexpected(TreeError::NO_MEMORY);
      }
      return gain && failGain ? std::expected<void, TreeError>(std::unexpected(TreeError::CALLBACK_FAILED))
                              : std::expected<void, TreeError>{};
    }));
  take(f.scene->focus(invoker));
  failLoss = true;
  auto opened = help.open();
  check(!opened && treeCause(opened.error(), TreeError::NO_MEMORY) && blocked &&
          take(f.app->tree().inspect(help.node())).visible &&
          portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
        "published focus callback NO_MEMORY retains open help and rejects lifecycle reentry");
  failLoss = false;
  failGain = true;
  auto closed = help.close();
  check(!closed && treeCause(closed.error(), TreeError::CALLBACK_FAILED) &&
          !take(f.app->tree().inspect(help.node())).visible && take(f.app->router().focused()) == invoker &&
          !portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
        "published return callback failure still completes close with its original error");
}
} // namespace
int main() try {
  scalarPresentationParity();
  scalarDefaultsAndLimits();
  formatterCapRemainsNative();
  materializedHelpHandoff();
  localHelpReferences();
  helpBodyStyles();
  helpBodyMountRollback();
  disabledHintsStyles();
  lifecycle();
  exhaustion();
  overflowAndRetry();
  retainedProjectionBudget();
  receiptGuardAndFocusErrors();
  std::cout << "key-help: styles/disabled/tiny/Unicode/open/close/return/activation-ABA/exhaustion PASS\n";
} catch (const std::exception &error) {
  allocationTest::remaining = -1;
  allocationTest::hook = nullptr;
  std::cerr << error.what() << '\n';
  return 1;
}
