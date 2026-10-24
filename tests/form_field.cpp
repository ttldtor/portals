// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>

#include <array>
#include <stdexcept>
#include <thread>

using namespace portals;

namespace {
template <class T, class E>
T take(std::expected<T, E> value, std::source_location origin = std::source_location::current()) {
  if (!value) {
    MESSAGE(std::string_view(origin.file_name()) << ':' << origin.line());

    if constexpr (std::same_as<E, ManagedError>) {
      ui::SceneReport report;

      report.failure = value.error();
      ui::reportToStderr(report);
    } else if constexpr (std::same_as<E, TreeError>) {
      MESSAGE("TreeError: " << static_cast<int>(value.error()));
    }
  }

  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == expected;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<FormField> field;
  ui::Ref<Button> other;

  explicit Fixture(ui::FormFieldConfig config = {}, std::string_view initial = {}) {
    take(ui::mount(*scene, ui::vbox(ui::formField(field, "Name", initial, std::move(config)), ui::button("Other")
                                                                                                .onClick([] {
                                                                                                })
                                                                                                .reference(other))));
  }

  FieldValidation validation() {
    return take(field.validation());
  }

  std::string message() {
    auto children = take(app->tree().inspect(field.node())).children;
    std::string result;

    take(app->tree().read(children.back(), [&](const Widget &widget) {
      result = dynamic_cast<const Label &>(widget).text().bytes();
    }));

    return result;
  }
};
} // namespace

TEST_CASE("required is visible initially; empty validation retains an editable draft and readable error") {
  Fixture fixture({.help = "Public name", .required = true});
  auto children = take(fixture.app->tree().inspect(fixture.field.node())).children;
  std::string caption;

  take(fixture.app->tree().read(children.front(), [&](const Widget &widget) {
    caption = dynamic_cast<const Label &>(widget).text().bytes();
  }));
  CHECK(caption == "Name (required)");
  CHECK(fixture.validation().status == FieldStatus::UNCHECKED);
  CHECK_FALSE(take(fixture.field.validate()));
  CHECK(fixture.validation().status == FieldStatus::INVALID);
  CHECK(fixture.message() == "Error: A value is required");
  take(fixture.field.focus());
  CHECK(take(fixture.app->router().focused()) == fixture.field.editor().node());
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  CHECK(fixture.validation().status == FieldStatus::VALID);
  CHECK(fixture.message() == "Valid");
  CHECK(take(fixture.field.text().get()) == "x");
}

TEST_CASE("field publication rules follow all ordinary editor modes without a second validation") {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    CAPTURE(mode);
    int calls{};
    Fixture fixture({.validate =
                       [&](std::string_view value) {
                         ++calls;
                         return value == "ok" ? FieldValidation::valid() : FieldValidation::invalid("Use ok");
                       },
                     .editor = {.options = {.editMode = mode}}});

    take(fixture.field.focus());
    take((fixture.field.text() = "bad").result());
    CHECK(calls == (mode == EditMode::IMMEDIATE ? 1 : 0));
    take(fixture.other.focus());
    CHECK(calls == (mode == EditMode::EXPLICIT ? 0 : 1));

    if (mode == EditMode::EXPLICIT) {
      take(fixture.app->router().invoke(fixture.field.editor().node(), {Action::ACTIVATE}));
    }

    CHECK(calls == 1);
    CHECK(fixture.validation().status == FieldStatus::INVALID);
    CHECK(take(fixture.field.text().get()) == "bad");
    take((fixture.field.text() = "ok").result());
    CHECK(take(fixture.field.validate()));
    CHECK(fixture.validation().status == FieldStatus::VALID);
  }
}

TEST_CASE("pending is explicit text presentation and does not disable the editor or schedule frames") {
  Fixture fixture;

  take(fixture.field.display(FieldValidation::pending("Checking the directory")));
  CHECK(fixture.message() == "Checking: Checking the directory");
  take(fixture.field.focus());
  take(fixture.app->router().dispatch(TextInputEvent{U'я'}));
  CHECK(take(fixture.field.text().get()) == "я");
  CHECK(fixture.validation().status == FieldStatus::VALID);
  take(fixture.app->tree().consumeFrameRequest());
  CHECK_FALSE(take(fixture.app->tree().consumeFrameRequest()));
}

TEST_CASE("validation is rejected in frames before calling user code; explicit reentry is checked") {
  int calls{};
  ui::Ref<FormField> reference;
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));

  take(ui::mount(*scene, ui::formField(reference, "Name", "x", {.validate = [&](std::string_view) {
                                         ++calls;
                                         auto nested = reference.validate();
                                         REQUIRE_FALSE(nested);
                                         CHECK(cause(nested.error(), TreeError::WOULD_BLOCK));

                                         return FieldValidation::valid();
                                       }})));
  take(app->tree().frame([&] {
    auto rejected = reference.validate();
    REQUIRE_FALSE(rejected);
    CHECK(cause(rejected.error(), TreeError::WOULD_BLOCK));
  }));
  CHECK(calls == 0);
  CHECK(take(reference.validate()));
  CHECK(calls == 1);
}

TEST_CASE("validator exceptions retain exact checked errors; a self-removing validator cannot publish") {
  Fixture throwing({.validate = [](std::string_view) -> FieldValidation {
    throw std::runtime_error("rule failed");
  }});
  auto failure = throwing.field.validate();

  REQUIRE_FALSE(failure);
  CHECK(cause(failure.error(), TreeError::CALLBACK_FAILED));
  CHECK(throwing.validation().status == FieldStatus::UNCHECKED);

  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FormField> removed;

  take(ui::mount(*scene, ui::formField(removed, "Name", "x", {.validate = [&](std::string_view) {
                                         take(app->tree().remove(removed.node()));

                                         return FieldValidation::invalid("Too late");
                                       }})));

  auto stale = removed.validate();

  REQUIRE_FALSE(stale);
  CHECK(cause(stale.error(), TreeError::STALE_NODE));
  CHECK_FALSE(removed.validation());
}

TEST_CASE("custom editor retains the same focus composition and rejects implicit text validation") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FormField> field;
  ui::Ref<CheckBox> editor;

  take(ui::mount(*scene,
                 ui::formField("Updates", ui::checkbox("Enable", false).reference(editor), {.help = "Custom editor"})
                   .reference(field)));
  take(field.focus());
  CHECK(take(app->router().focused()) == editor.node());
  auto failure = field.validate();

  REQUIRE_FALSE(failure);
  CHECK(cause(failure.error(), TreeError::INVALID_ARGUMENT));
  auto text = field.text().get();

  REQUIRE_FALSE(text);
  CHECK(cause(text.error(), TreeError::INVALID_ARGUMENT));
  take(field.display(FieldValidation::invalid("Choose an option")));
  CHECK(take(field.validation()).status == FieldStatus::INVALID);
}

TEST_CASE("Unicode narrow and zero layouts and theme changes do not validate or replace the draft") {
  int calls{};
  Fixture fixture({.help = "Long help text",
                   .validate =
                     [&](std::string_view) {
                       ++calls;

                       return FieldValidation::valid();
                     }},
                  "界e\u0301");

  for (const auto extent : {CellSize{2, 2}, CellSize{0, 0}, CellSize{40, 8}}) {
    take(fixture.scene->prepare(*fixture.app, extent));
    auto bounds = take(CellRect::from({}, extent));

    take(fixture.app->tree().frame([&] {
      take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
    }));
    take(fixture.scene->theme({}));
  }

  CHECK(calls == 0);
  CHECK(take(fixture.field.text().get()) == "界e\u0301");
  CHECK(take(fixture.field.validate()));
  CHECK(calls == 1);
}

TEST_CASE("field references reject foreign threads and retired owners") {
  Fixture fixture;
  bool rejected{};
  std::thread other([&] {
    auto result = fixture.field.validate();
    rejected = !result && cause(result.error(), TreeError::WRONG_THREAD);
  });

  other.join();
  CHECK(rejected);
  fixture.scene.reset();
  auto result = fixture.field.validation();

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::CLOSED));
}

TEST_CASE("standard loaded fields share required help initial and editor mode settings without registration") {
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;

  auto owner = ui::scene(ui::resource(R"(
    defaults { FormField { required=true help="Public account name" editMode=EXPLICIT } }
    scene { VBox {
      FormField#name { label="Имя пользователя" text="Алекс" requiredMessage="Укажите имя" }
      FormField#optional { label="Optional" text="" required=false }
      Button { "Other" }
    } }
  )",
                                      options));
  auto field = owner.get<FormField>("name");
  auto optional = owner.get<FormField>("optional");

  take(owner.prepareChecked());
  CHECK(take(field.validation()).status == FieldStatus::UNCHECKED);
  CHECK(take(field.text().get()) == "Алекс");
  take((field.text() = "").result());
  CHECK(take(field.validation()).status == FieldStatus::UNCHECKED);
  CHECK_FALSE(take(field.validate()));
  CHECK(take(field.validation()).message == "Укажите имя");
  CHECK(take(optional.validate()));
  take(field.focus());
  CHECK(field.editor().node() != Node<LineEdit>{});
  take(owner.close());
}

TEST_CASE("field text proxies captured before mount remain lazy and update live sibling captions") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FormField> field;
  ui::Ref<Label> mirror;
  auto proxy = field.text();

  take(ui::mount(*scene, ui::vbox(ui::formField(field, "Name", "Alex"), ui::label(mirror, proxy))));
  CHECK(take(mirror.text().get()) == "Alex");
  take((proxy = "Bob").result());
  take(scene->prepare(*app, {30, 8}));
  CHECK(take(mirror.text().get()) == "Bob");
}

TEST_CASE("label mouse activation follows the field editor focus route") {
  Fixture fixture;
  auto bounds = take(CellRect::from({}, {30, 8}));

  take(fixture.scene->prepare(*fixture.app, {30, 8}));
  take(fixture.app->tree().frame([&] {
    take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
  }));
  take(fixture.other.focus());
  take(fixture.app->router().dispatch(MouseInputEvent{0, 0, MouseButton::LEFT, MouseAction::PRESS, {}}));
  CHECK(take(fixture.app->router().focused()) == fixture.field.editor().node());
  CHECK(take(fixture.app->router().traverse()) == fixture.other.node());
}

TEST_CASE("one owning rule instance is shared by explicit validation and editor publication") {
  Fixture fixture({.validate = [count = 0](std::string_view) mutable {
    ++count;

    return FieldValidation::valid(std::to_string(count));
  }});

  CHECK(take(fixture.field.validate()));
  CHECK(fixture.validation().message == "1");
  take((fixture.field.text() = "next").result());
  CHECK(fixture.validation().message == "2");
}

TEST_CASE("invalid explicit presentation preserves the last accepted text and status") {
  Fixture fixture;

  take(fixture.field.display(FieldValidation::invalid("Readable error")));

  const auto before = fixture.message();
  auto malformed = fixture.field.display(FieldValidation::invalid(std::string(1, static_cast<char>(0xff))));

  REQUIRE_FALSE(malformed);
  CHECK(cause(malformed.error(), TreeError::INVALID_ARGUMENT));
  CHECK(fixture.message() == before);
  CHECK(fixture.validation().message == "Readable error");
}

TEST_CASE("a throwing allocation rule is reported as NO_MEMORY without replacing presentation") {
  Fixture fixture({.validate = [](std::string_view) -> FieldValidation {
    throw std::bad_alloc();
  }});
  auto result = fixture.field.validate();

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::NO_MEMORY));
  CHECK(fixture.validation().status == FieldStatus::UNCHECKED);
}

TEST_CASE("original editor outcomes are delivered once and history remains reversible") {
  std::vector<LineEditResult> outcomes;
  Fixture fixture({.required = true, .editor = {.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
                                       outcomes.push_back(std::move(result));

                                       return {};
                                     }}});

  take((fixture.field.text() = "first").result());
  take((fixture.field.text() = "second").result());
  take(fixture.field.editor().undo().result());
  CHECK(take(fixture.field.text().get()) == "first");
  REQUIRE(outcomes.size() == 3);
  CHECK(outcomes.back().status == LineEditStatus::COMMITTED);
  CHECK(outcomes.back().commit.has_value());
  CHECK(fixture.validation().status == FieldStatus::VALID);
}

TEST_CASE("partial field mount rolls back its complete standalone or nested subtree without publishing refs") {
  for (const bool nested : {false, true}) {
    for (const bool failAfterEditor : {false, true}) {
      CAPTURE(nested);
      CAPTURE(failAfterEditor);
      auto app = take(Application::create());
      auto scene = take(ManagedScene::create(*app));
      ui::Ref<Label> preserved;
      ui::Ref<FormField> field;
      ui::Ref<LineEdit> editor;

      take(ui::mount(*scene, ui::label(preserved, "Preserved sibling")));

      const auto before = take(app->tree().inspect(scene->root())).children;
      const std::string malformed(1, static_cast<char>(0xff));
      auto recipe = failAfterEditor
                      ? ui::formField("Name", ui::lineEdit(editor, "value"), {.help = malformed}).reference(field)
                      : ui::formField(field, "Name", malformed);
      auto mounted = nested ? ui::mount(*scene, ui::vbox(std::move(recipe))) : ui::mount(*scene, std::move(recipe));

      REQUIRE_FALSE(mounted);
      CHECK(mounted.error().text.has_value());
      CHECK(take(app->tree().inspect(scene->root())).children == before);
      CHECK(field.node() == Node<FormField>{});
      CHECK(editor.node() == Node<LineEdit>{});
      CHECK(take(preserved.text().get()) == "Preserved sibling");
      CHECK(take(app->router().traverse()) == Node<Widget>{});
    }
  }
}

TEST_CASE("publication-triggered field removal pins the executing rule and preserves the original rejection") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FormField> field;
  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> observed = lifetime;
  std::vector<LineEditStatus> outcomes;

  take(
    ui::mount(*scene, ui::formField(field, "Name", "",
                                    {.validate =
                                       [&, retained = lifetime](std::string_view) {
                                         take(app->tree().remove(field.node()));
                                         CHECK_FALSE(observed.expired());
                                         CHECK(*retained == 1);

                                         return FieldValidation::invalid("Removed during publication");
                                       },
                                     .editor = {.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
                                       outcomes.push_back(result.status);

                                       return {};
                                     }}})));
  lifetime.reset();

  auto changed = field.text() = "remove";

  CHECK(changed.result());
  REQUIRE(outcomes.size() == 1);
  CHECK(outcomes.front() == LineEditStatus::VALIDATION_FAILED);
  CHECK_FALSE(field.validation());
  take(scene->prepare(*app, {30, 8}));
  CHECK(observed.expired());
}
