// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui_scene_resource.hpp>
#include <portals/ui_style.hpp>

#include <array>

using namespace portals;
namespace sc = style::ct;

namespace {
template <class T> ComputedStyle computed(ui::Scene &scene, const ui::Ref<T> &reference) {
  std::optional<ComputedStyle> result;

  scene.configure([&](ManagedScene &, Application &app) {
    REQUIRE(app.tree().read(reference.node(), [&](const Widget &widget) {
      result = widget.computedStyle();
    }));
  });
  REQUIRE(scene.checked());
  REQUIRE(result);

  return *result;
}

struct CustomButton final : Button {
  CustomButton() : Button(*GraphemeText::from("Custom")) {
  }
};

constexpr auto STATIC_RULES = sc::sheet(sc::parameters(), sc::rule(sc::selector<"Button", "save">(), sc::minWidth(12)),
                                        sc::rule(sc::selector<"Control", "save">(), sc::bold(true)));
constexpr auto COMPACT_RULES = sc::pss("Button#save { min-width:12; } Control#save { bold:true; }");
} // namespace

TEST_CASE("standard factories retain natural identities and the Control alias after id assignment") {
  ui::Ref<Label> label;
  ui::Ref<Button> button;
  ui::Ref<Button> plain;
  ui::Ref<CheckBox> checkbox;
  ui::Ref<LineEdit> editor;
  ui::Ref<KeyHints> hints;
  ui::Ref<HelpView> help;
  auto scene = ui::scene(ui::column(
    ui::label(label, "Label").id("caption"), ui::button(button, "Button").id("save"),
    ui::plainButton("Plain").reference(plain).id("plain"), ui::checkbox("Check").reference(checkbox).id("choice"),
    ui::lineEdit(editor, "Text").id("message"), ui::keyHints(help).reference(hints).id("hints"), ui::helpView(help)));

  scene | ui::style("Control { bold:true; } Label#caption { min-width:11; } Button#save { min-width:12; } "
                    "Button#plain { min-width:13; } CheckBox#choice { min-width:14; } "
                    "LineEdit#message { min-width:15; } KeyHints#hints { min-width:16; } "
                    "Button#save:focused { italic:true; } Control#save:disabled { underline:true; }");
  REQUIRE(scene.prepareChecked());

  const std::array styles{computed(scene, label),    computed(scene, button), computed(scene, plain),
                          computed(scene, checkbox), computed(scene, editor), computed(scene, hints)};

  for (std::size_t index = 0; index < styles.size(); ++index) {
    CHECK(styles[index].minWidth == 11 + static_cast<std::int32_t>(index));
    CHECK(styles[index].normal.bold);
  }

  CHECK(styles[1].focused.italic);
  CHECK(styles[1].disabled.underline);
}

TEST_CASE("static and compact sources use the same natural type and Control matching") {
  for (bool compact : {false, true}) {
    ui::Ref<Button> button;
    auto scene = ui::scene(ui::button(button, "Save").id("save"));

    if (compact) {
      scene | ui::style(COMPACT_RULES);
    } else {
      scene | ui::liveTheme(STATIC_RULES);
    }

    REQUIRE(scene.prepareChecked());
    CHECK(computed(scene, button).minWidth == 12);
    CHECK(computed(scene, button).normal.bold);
  }
}

TEST_CASE("explicit targets preserve exact matching unless the Control alias is requested") {
  const auto sheet = parsePss("Control#save.primary { min-width:19; } "
                              "Control#save.primary:focused { bold:true; }");
  REQUIRE(sheet);

  const StyleTarget exact{.type = "Button", .id = "save", .classes = {"primary"}};
  auto compatible = exact;

  compatible.control = true;
  CHECK(resolveStyle(*sheet, exact).minWidth == 0);
  CHECK_FALSE(resolveStyle(*sheet, exact).focused.bold);
  CHECK(resolveStyle(*sheet, compatible).minWidth == 19);
  CHECK(resolveStyle(*sheet, compatible).focused.bold);
  CHECK_FALSE(resolveStyle(*sheet, compatible).normal.bold);
  CHECK(resolveStyle(*sheet, StyleTarget{.type = "Button", .id = "other", .classes = {"primary"}, .control = true})
          .minWidth == 0);
  CHECK(resolveStyle(*sheet, StyleTarget{.type = "Button", .id = "save", .control = true}).minWidth == 0);
  CHECK(resolveStyle(*sheet, StyleTarget{.type = "Control", .id = "save", .classes = {"primary"}}).minWidth == 19);

  const auto naturalLast = parsePss("Control { min-width:17; } Button { min-width:13; }");
  const auto aliasLast = parsePss("Button { min-width:13; } Control { min-width:17; }");

  REQUIRE(naturalLast);
  REQUIRE(aliasLast);
  CHECK(resolveStyle(*naturalLast, compatible).minWidth == 13);
  CHECK(resolveStyle(*aliasLast, compatible).minWidth == 17);

  ui::Ref<Button> replaced;
  ui::Ref<Button> optedIn;
  auto scene = ui::scene(
    ui::column(ui::button(replaced, "Exact").style(StyleTarget{.type = "Button"}).id("save"),
               ui::button(optedIn, "Alias").style(StyleTarget{.type = "Button", .control = true}).id("alias")));

  scene | ui::style("Button { min-width:13; } Control { min-width:17; }");
  REQUIRE(scene.prepareChecked());
  CHECK(computed(scene, replaced).minWidth == 13);
  CHECK(computed(scene, optedIn).minWidth == 17);
}

TEST_CASE("custom Control subtypes retain the existing generic type until explicitly replaced") {
  ui::Ref<CustomButton> generic;
  ui::Ref<CustomButton> named;
  auto scene = ui::scene(
    ui::column(ui::custom<CustomButton>().reference(generic).id("generic"),
               ui::custom<CustomButton>().reference(named).style(StyleTarget{.type = "CustomButton"}).id("named")));

  scene | ui::style("Control { min-width:11; } Button { min-width:19; } CustomButton { min-width:13; }");
  REQUIRE(scene.prepareChecked());
  CHECK(computed(scene, generic).minWidth == 11);
  CHECK(computed(scene, named).minWidth == 13);
}

TEST_CASE("cold text lists preserve names classes and explicit types through finalization") {
  for (bool graphemes : {false, true}) {
    for (unsigned identity = 0; identity != 3; ++identity) {
      ui::Ref<LineEdit> input;
      ui::Ref<VirtualList<TextSegment>> output;
      auto description = [&] {
        auto list = ui::virtualList(graphemes ? ui::graphemes(input.draftText()) : ui::words(input.draftText()));

        if (identity == 0) {
          return std::move(list).id("tokens");
        }

        if (identity == 1) {
          return std::move(list).style(StyleTarget{.classes = {"segments"}}).id("tokens");
        }

        return std::move(list).id("tokens").style(StyleTarget{.type = "TokenList", .classes = {"segments"}});
      }();

      auto scene =
        ui::scene(ui::column(ui::lineEdit(input, "one two"), std::move(description).reference(output).height(2)));
      const auto named = scene.get<VirtualList<TextSegment>>("tokens");

      scene | ui::style("List#tokens { min-width:11; } List#tokens.segments { min-width:12; } "
                        "TokenList#tokens.segments { min-width:13; }");
      REQUIRE(scene.prepareChecked());
      CHECK(named.node() == output.node());
      CHECK(computed(scene, output).minWidth == 11 + static_cast<std::int32_t>(identity));
    }
  }
}

TEST_CASE("resource text controls expose natural selectors and retain Control compatibility") {
  for (bool control : {false, true}) {
    auto scene = ui::scene(ui::resource(R"(VBox#root{width=32 gap=1
      Label#caption{"Text" width=12} HBox#row{height=2
      Button#save{"Save" flex=2} CheckBox#choice{"Yes" checked=true}}
      LineEdit#message{"initial" height=1}})"));
    const auto caption = scene.get<Label>("caption");
    const auto save = scene.get<Button>("save");
    const auto choice = scene.get<CheckBox>("choice");
    const auto message = scene.get<LineEdit>("message");

    scene | ui::style(control ? "Control { min-width:12; }"
                              : "Label#caption { min-width:12; } Button#save { min-width:12; } "
                                "CheckBox#choice { min-width:12; } LineEdit#message { min-width:12; }");
    REQUIRE(scene.prepareChecked());
    CHECK(computed(scene, caption).minWidth == 12);
    CHECK(computed(scene, save).minWidth == 12);
    CHECK(computed(scene, choice).minWidth == 12);
    CHECK(computed(scene, message).minWidth == 12);
    scene.configure([&](ManagedScene &, Application &app) {
      REQUIRE(app.tree().read(caption.node(), [](const Label &widget) {
        CHECK(widget.sizePolicy().width.mode == SizeMode::FIXED);
        CHECK(widget.sizePolicy().width.preferred == 12);
      }));
      REQUIRE(app.tree().read(save.node(), [](const Button &widget) {
        CHECK(widget.sizePolicy().width.mode == SizeMode::FLEX);
        CHECK(widget.sizePolicy().width.weight == 2);
      }));
    });
    REQUIRE(scene.checked());
  }
}

TEST_CASE("resource zero width remains exact and rejects conflicting style minima atomically") {
  auto scene = ui::scene(ui::resource(R"(Label#caption{"Text" width=0})"));
  const auto caption = scene.get<Label>("caption");

  scene | ui::style("Label#caption { min-width:0; }");
  REQUIRE(scene.prepareChecked());
  CHECK(computed(scene, caption).minWidth == 0);

  const auto conflicting = parsePssDocument("Label#caption { min-width:12; }");

  REQUIRE(conflicting);
  scene.configure([&](ManagedScene &managed, Application &app) {
    const auto rejected = managed.styleDocument(StyleTier::APPLICATION, "conflicting", *conflicting);

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().phase == ManagedPhase::STYLE);

    const auto *style = std::get_if<StyleError>(&rejected.error().cause);

    REQUIRE(style);
    CHECK(style->cause == TreeError::INVALID_ARGUMENT);
    REQUIRE(app.tree().read(caption.node(), [](const Label &widget) {
      CHECK(widget.sizePolicy().width.mode == SizeMode::FIXED);
      CHECK(widget.sizePolicy().width.minimum == 0);
      CHECK(widget.sizePolicy().width.preferred == 0);
      CHECK(widget.sizePolicy().width.maximum == 0);
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->minWidth == 0);
    }));
  });
  REQUIRE(scene.checked());

  auto cold = ui::scene(ui::resource(R"(Label#caption{"Text" width=0})"));

  cold | ui::style("Label#caption { min-width:12; }");

  const auto rejected = cold.prepareChecked();

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().phase == ManagedPhase::STYLE);

  const auto *style = std::get_if<StyleError>(&rejected.error().cause);

  REQUIRE(style);
  CHECK(style->cause == TreeError::INVALID_ARGUMENT);
}
