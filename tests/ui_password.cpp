// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui_password.hpp>
#include <portals/ui_style.hpp>

#include "support/examples/replay/terminal.hpp"

#include <fstream>
#include <stdexcept>
#include <type_traits>

using namespace portals;

namespace {
template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

template <class T>
concept HasText = requires(T value) { value.text(); };
template <class T>
concept HasUndo = requires(T value) { value.undo(); };
template <class T>
concept HasRedo = requires(T value) { value.redo(); };
static_assert(std::same_as<ui::PasswordRef, ui::Ref<Password>>);
static_assert(!HasText<ui::PasswordRef>);
static_assert(!HasUndo<ui::PasswordRef>);
static_assert(!HasRedo<ui::PasswordRef>);
static_assert(!HasText<PasswordValue>);

bool equals(const PasswordValue &value, std::string_view expected) {
  bool equal{};

  take(value.withPlaintext([&](SecretView bytes) {
    equal = std::string_view(bytes.data(), bytes.size()) == expected;
  }));

  return equal;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> owner = take(ManagedScene::create(*app));

  ~Fixture() {
    CHECK(owner->close());
  }

  void input(ui::PasswordRef ref, char32_t scalar) {
    take(app->router().setFocus(ref.node()));
    take(app->router().dispatch(TextInputEvent{scalar, {}}));
  }
};

struct PaintProbe : PaintContext {
  std::string bytes;
  std::vector<std::string> runs;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *text = std::get_if<TextCommand>(&command); text && !text->clip.empty()) {
        bytes = text->text.bytes();
        runs.push_back(bytes);
      }
    }

    return {};
  }
};

std::string paint(Fixture &fixture, ui::PasswordRef ref) {
  PaintProbe output;

  take(fixture.owner->prepare(*fixture.app, {20, 4}));
  take(fixture.app->tree().frame([&] {
    const auto viewport = take(CellRect::from({}, {20, 4}));

    take(layoutTree(fixture.app->tree(), take(fixture.app->tree().root()), viewport, viewport));
    take(fixture.app->tree().paint(ref.node(), output));
  }));

  for (const auto &run : output.runs) {
    CHECK(run == output.bytes);
    CHECK(run.find_first_not_of('*') == std::string::npos);
  }

  return output.bytes;
}

ManagedError reject(std::string_view source, ui::SceneResourceOptions options = {}) {
  auto owner = ui::scene(source, std::move(options));
  auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  REQUIRE(result.error().sceneResource);
  REQUIRE(result.error().sceneResource->source);
  CHECK(*result.error().sceneResource->source == "<string resource>");

  return result.error();
}

struct TemporaryFile {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-password-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");

  ~TemporaryFile() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
  }
};
} // namespace

TEST_CASE("ref first Password factory owns protected state and inherits native Password style") {
  Fixture fixture;
  ui::PasswordRef password;
  unsigned providers{};
  ui::PasswordConfig config;

  config.initial = [&]() -> std::expected<PasswordValue, PasswordFailure> {
    ++providers;

    return take(PasswordValue::create("seed"));
  };
  take(ui::mount(*fixture.owner, ui::password(password, config).placeholder("Password")));
  take(fixture.owner->theme(take(parsePss("Password { bold:true; foreground:#010203; }"))));
  CHECK(providers == 1);
  CHECK(equals(take(password.value()), "seed"));
  CHECK(paint(fixture, password) == "****");
  take(fixture.app->tree().read(password.node(), [](const Password &widget) {
    REQUIRE(widget.computedStyle());
    CHECK(widget.computedStyle()->normal.bold);
  }));
  fixture.input(password, U'x');
  CHECK(equals(take(password.value()), "seedx"));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}));
  CHECK(equals(take(password.value()), "seedx"));
  CHECK(providers == 1);
}

TEST_CASE("Password method and pipe noEcho hide the same nonempty protected draft") {
  Fixture fixture;
  ui::PasswordRef method, pipe;

  take(ui::mount(*fixture.owner, ui::vbox(ui::password(method).noEcho(), ui::password(pipe) | ui::noEcho())));
  take(method.value(take(PasswordValue::create("first"))));
  take(pipe.value(take(PasswordValue::create("longer second"))));
  CHECK(paint(fixture, method).empty());
  CHECK(paint(fixture, pipe).empty());
}

TEST_CASE("readonly blocks input but explicit protected values follow the configured edit boundary") {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture fixture;
    auto model = take(fixture.owner->state(take(PasswordValue::create("seed"))));
    ui::PasswordRef password;

    take(ui::mount(*fixture.owner, ui::password(password, model, {.options = {.editMode = mode, .readOnly = true}})));
    fixture.input(password, U'x');
    CHECK(equals(take(password.value()), "seed"));
    take(password.value(take(PasswordValue::create("replacement"))));
    CHECK(equals(take(password.value()), "replacement"));
    CHECK(equals(take(model.snapshot())->value, mode == EditMode::IMMEDIATE ? "replacement" : "seed"));
    take(fixture.app->router().invoke(password.node(), {Action::ACTIVATE}));
    CHECK(equals(take(model.snapshot())->value, "replacement"));
  }
}

TEST_CASE("external protected model conflict preserves draft and cancel adopts latest identity") {
  Fixture fixture;
  auto model = take(fixture.owner->state(take(PasswordValue::create("seed"))));
  ui::PasswordRef password;
  std::vector<PasswordStatus> results;
  ui::PasswordConfig config;

  config.options.editMode = EditMode::EXPLICIT;
  config.result = [&](PasswordResult result) -> std::expected<void, TreeError> {
    results.push_back(result.status);

    return {};
  };
  take(ui::mount(*fixture.owner, ui::password(password, model, config)));
  take(password.value(take(PasswordValue::create("draft"))));
  take(model.set(take(PasswordValue::create("external"))));
  take(fixture.app->router().invoke(password.node(), {Action::ACTIVATE}));
  CHECK(results.back() == PasswordStatus::CONFLICT);
  CHECK(equals(take(password.value()), "draft"));
  CHECK(equals(take(model.snapshot())->value, "external"));
  take(fixture.app->router().invoke(password.node(), {Action::CANCEL}));
  CHECK(equals(take(password.value()), "external"));
}

TEST_CASE("short form handler borrows SecretView across self removal") {
  Fixture fixture;
  ui::PasswordRef password;
  ui::Ref<Button> submit;
  bool readAfterRemoval{};

  take(ui::mount(*fixture.owner, ui::vbox(ui::password(password), ui::button(submit, "Submit", [&] {
                                            return password.withPlaintext([&](SecretView bytes) {
                                              take(fixture.owner->remove(password.node()));
                                              readAfterRemoval =
                                                std::string_view(bytes.data(), bytes.size()) == "secret";
                                            });
                                          }))));
  take(password.value(take(PasswordValue::create("secret"))));
  take(fixture.app->router().invoke(submit.node(), {Action::ACTIVATE}));
  CHECK(readAfterRemoval);
  CHECK_FALSE(password.value());
}

TEST_CASE("typed protected Ref failures are collected once at void action boundaries") {
  Fixture fixture;
  ui::PasswordRef password;
  ui::Ref<Button> submit;
  std::optional<ManagedError> failure;

  take(ui::mount(*fixture.owner, ui::vbox(ui::password(password), ui::button(submit, "Set invalid", [&] {
                                            auto result = password.value(PasswordValue{});

                                            REQUIRE_FALSE(result);
                                            failure = result.error();
                                          }))));
  take(fixture.app->router().invoke(submit.node(), {Action::ACTIVATE}));
  REQUIRE(failure);
  CHECK(failure->password.has_value());
  auto diagnostics = take(fixture.owner->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.errors.front().error.password.has_value());
}

TEST_CASE("failed protected borrow preserves typed access failure without duplicate collection") {
  Fixture fixture;
  ui::PasswordRef password;
  ui::Ref<Button> submit;

  take(ui::mount(*fixture.owner, ui::vbox(ui::password(password), ui::button(submit, "Borrow", [password] {
                                            return password.withPlaintext([](SecretView) {
                                              throw std::runtime_error("callback failed");
                                            });
                                          }))));
  take(fixture.app->router().invoke(submit.node(), {Action::ACTIVATE}));
  const auto diagnostics = take(fixture.owner->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  REQUIRE(diagnostics.errors.front().error.password);
  CHECK(std::holds_alternative<PasswordValueAccessFailure<std::monostate>>(*diagnostics.errors.front().error.password));
  CHECK(take(password.value()).available());
}

TEST_CASE("completion validator exceptions on blur keep the typed secure cause and report once") {
  Fixture fixture;
  auto model = take(fixture.owner->state(take(PasswordValue::create())));
  ui::PasswordRef password;
  ui::Ref<Button> target;
  unsigned validations{};
  bool throwOnValidate{};
  ui::PasswordConfig config;

  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [&](SecretView) -> bool {
    ++validations;
    if (throwOnValidate) {
      throw std::runtime_error("validation failed");
    }

    return true;
  };
  take(ui::mount(*fixture.owner, ui::vbox(ui::password(password, model, config), ui::button(target, "Target", [] {
                                          }))));
  fixture.input(password, U'x');
  const auto baselineValidations = validations;
  bool dirty{};

  take(fixture.app->tree().read(password.node(), [&](const Password &widget) {
    dirty = widget.dirty();
  }));
  CHECK(dirty);
  CHECK(take(model.snapshot())->value.empty());
  throwOnValidate = true;
  const auto before = take(fixture.owner->diagnostics()).errors.size();
  auto focused = target.focus();

  REQUIRE_FALSE(focused);
  INFO("focus phase=", static_cast<int>(focused.error().phase), " validations=", validations);
  if (const auto *error = std::get_if<TreeError>(&focused.error().cause)) {
    INFO("focus tree error=", static_cast<int>(*error));
    REQUIRE(focused.error().password);
  }
  REQUIRE(focused.error().password);
  const auto *failure = std::get_if<PasswordValueAccessFailure<std::monostate>>(&*focused.error().password);

  REQUIRE(failure);
  REQUIRE(failure->access);
  REQUIRE(failure->access->primary);
  CHECK(failure->access->primary->code == SecureStringErrorCode::CALLBACK_FAILED);
  CHECK(validations == baselineValidations + 1);
  const auto diagnostics = take(fixture.owner->diagnostics());

  REQUIRE(diagnostics.errors.size() == before + 1);
  REQUIRE(diagnostics.errors.back().error.password);
  CHECK(std::holds_alternative<PasswordValueAccessFailure<std::monostate>>(*diagnostics.errors.back().error.password));
}

TEST_CASE("Password initial provider failure prevents runner terminal activity") {
  unsigned calls{};
  ui::PasswordConfig config;

  config.initial = [&]() -> std::expected<PasswordValue, PasswordFailure> {
    ++calls;

    return std::unexpected(PasswordFailure{TreeError::INVALID_ARGUMENT});
  };
  auto owner = ui::scene(ui::password(config));
  auto prepared = owner.prepareChecked();
  example::ReplayTerminal terminal;

  REQUIRE_FALSE(prepared);
  REQUIRE(prepared.error().password);
  CHECK(std::get<TreeError>(*prepared.error().password) == TreeError::INVALID_ARGUMENT);
  CHECK(owner.runApp(terminal) == 1);
  CHECK(calls == 1);
  CHECK(terminal.output.empty());
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("legacy Password resource accepts empty text and public placeholder alias") {
  auto owner = ui::scene(R"(Password#psw{text="" placeHolder="Password"})");
  auto password = owner.get<Password>("psw");

  take(owner.prepareChecked());
  CHECK(take(password.value()).empty());
  take(password.value(take(PasswordValue::create("safe"))));
  CHECK(equals(take(password.value()), "safe"));
  take(owner.close());

  for (const auto group : {"securePolicy{maxBytes=16}", "securePolicy={maxBytes=16}"}) {
    INFO("legacy protected settings=", std::string_view(group));
    auto configured = ui::scene("Password#psw{text=\"\" " + std::string(group) + "}");
    auto configuredPassword = configured.get<Password>("psw");

    take(configured.prepareChecked());
    CHECK(take(configuredPassword.value()).options().maxBytes == 16);
    take(configured.close());
  }
}

TEST_CASE("configured Password security settings match typed configuration") {
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  for (const auto leaf :
       {"text=\"\"", "placeholder=\"Password\"", "editMode=EXPLICIT", "readOnly=true", "displayMode=NO_ECHO",
        "pastePolicy=DISABLED", "securePolicy{maxBytes=16}", "securePolicy={maxBytes=16}",
        "securePolicy{storage=NATIVE_GUARDED}", "securePolicy{lockPages=false}", "securePolicy{excludeFromDumps=false}",
        "securePolicy{encryptWhileIdle=true}"}) {
    INFO("individual setting=", std::string_view(leaf));
    auto probe = ui::scene("scene{Password{" + std::string(leaf) + "}}", options);
    auto result = probe.prepareChecked();

    CHECK(result);
    if (result) {
      take(probe.close());
    }
  }
  auto typed = ui::scene(ui::password().settings(ui::ControlSettings{.securePolicy = {.maxBytes = 16}}));
  auto typedPrepared = typed.prepareChecked();

  INFO("typed securePolicy maxBytes");
  CHECK(typedPrepared);
  if (typedPrepared) {
    take(typed.close());
  }
  auto owner = ui::scene(R"(scene { Password#psw {
    text="" placeholder="Password" editMode=EXPLICIT readOnly=true displayMode=NO_ECHO
    pastePolicy=DISABLED securePolicy { maxBytes=16 storage=NATIVE_GUARDED lockPages=false
      excludeFromDumps=false encryptWhileIdle=true }
  } })",
                         options);
  auto password = owner.get<Password>("psw");

  auto prepared = owner.prepareChecked();

  INFO("configuration mount");
  if (!prepared) {
    INFO("phase=", static_cast<int>(prepared.error().phase), " cause variant=", prepared.error().cause.index(),
         " operation=", std::string_view(prepared.error().operation ? prepared.error().operation : "<none>"),
         " execution=", std::string_view(prepared.error().execution ? prepared.error().execution : "<none>"));
    if (const auto *error = std::get_if<TreeError>(&prepared.error().cause)) {
      INFO("tree error=", static_cast<int>(*error));
      REQUIRE(prepared);
    }
  }
  REQUIRE(prepared);
  auto draft = password.value();

  INFO("configuration protected draft");
  REQUIRE(draft);
  const auto initial = *draft;

  CHECK(initial.options().maxBytes == 16);
  CHECK(initial.options().storage == SecureStringStorage::NATIVE_GUARDED);
  CHECK_FALSE(initial.options().lockPages);
  CHECK_FALSE(initial.options().excludeFromDumps);
  CHECK(initial.options().encryptWhileIdle);
  auto candidate = PasswordValue::create("replacement", {.maxBytes = 16});

  REQUIRE(candidate);
  auto changed = password.value(std::move(*candidate));

  INFO("configuration programmatic edit");
  REQUIRE(changed);
  CHECK(equals(take(password.value()), "replacement"));
  take(owner.close());
}

TEST_CASE("Password secret literals expressions and malformed input are redacted before callbacks") {
  constexpr std::string_view SECRET = "secret-never-in-diagnostics";

  for (const auto source :
       {R"(VBox{ Label{text=i18n("caption")} Private{} Password{text="secret-never-in-diagnostics"} })",
        R"(VBox{ Label{text=i18n("caption")} Private{} Password{text=i18n("secret-never-in-diagnostics")} })",
        R"(VBox{ Label{text=i18n("caption")} Private{} Password{text="secret-never-in-diagnostics})",
        R"(VBox{ Label{text=i18n("caption")} Private{} Password{text="secret-never-in-diagnostics\q"} })"}) {
    unsigned resolutions{}, factories{}, providers{};
    ui::SceneResourceOptions options;

    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "caption";
    };
    REQUIRE(options.registry.add<Password>("Private", [&](ui::SceneResourceArguments) {
      ++factories;
      ui::PasswordConfig config;

      config.initial = [&]() -> std::expected<PasswordValue, PasswordFailure> {
        ++providers;

        return take(PasswordValue::create());
      };

      return ui::password(config);
    }));
    const auto error = reject(source, options);

    CHECK(error.sceneResource->source->find(SECRET) == std::string::npos);
    CHECK(resolutions == 0);
    CHECK(factories == 0);
    CHECK(providers == 0);
  }
}

TEST_CASE("configured aliases defaults and custom Password factories reject inherited secrets before i18n") {
  for (const auto source :
       {R"(definitions { Hidden=Password{text="secret"} } scene { VBox { Label{text=i18n("caption")} Hidden{} } })",
        R"(defaults { Password{text=i18n("secret")} } scene { VBox { Label{text=i18n("caption")} Password{} } })",
        R"(definitions { Hidden=Private{text="secret"} } scene { VBox { Label{text=i18n("caption")} Hidden{} } })",
        R"(defaults { Private{text="secret"} } scene { VBox { Label{text=i18n("caption")} Private{} } })"}) {
    unsigned resolutions{}, factories{}, providers{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "caption";
    };
    REQUIRE(options.registry.add<Password>("Private", [&](ui::SceneResourceArguments) {
      ++factories;
      ui::PasswordConfig config;

      config.initial = [&]() -> std::expected<PasswordValue, PasswordFailure> {
        ++providers;

        return take(PasswordValue::create());
      };

      return ui::password(config);
    }));
    reject(source, options);
    CHECK(resolutions == 0);
    CHECK(factories == 0);
    CHECK(providers == 0);
  }
}

TEST_CASE("Password resource source limits and file failures retain only safe source identity") {
  constexpr std::string_view SOURCE = R"(Password{text="secret-never-in-diagnostics"})";
  ui::SceneResourceOptions options;

  options.limits.maxSourceBytes = 5;
  const auto limited = reject(SOURCE, options);

  CHECK(limited.sceneResource->code == SceneResourceErrorCode::SOURCE_LIMIT);
  CHECK(
    reject("Password{text=\"secret-never-in-diagnostics").sceneResource->source->find("secret-never-in-diagnostics") ==
    std::string::npos);
  TemporaryFile file;
  std::ofstream output(file.path, std::ios::binary);

  REQUIRE(output);
  output << SOURCE;
  output.close();
  auto owner = ui::scene(ui::SceneResourceFile{file.path});
  auto failed = owner.prepareChecked();

  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().sceneResource);
  REQUIRE(failed.error().sceneResource->source);
  CHECK(*failed.error().sceneResource->source == file.path.generic_string());
}

TEST_CASE("typed inherited Password defaults reject secrets before unrelated resource i18n") {
  for (const bool expression : {false, true}) {
    unsigned resolutions{}, factories{};
    ui::SceneResourceOptions options;

    options.controls.emplace();
    options.controls->define("Hidden", "Password")
      .defaults("Hidden", {.text = expression ? i18n("secret-key") : text("secret-literal")});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "caption";
    };
    REQUIRE(options.registry.add<Password>("Private", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::password();
    }));
    reject(R"(VBox{Label{text=i18n("caption")} Private{} Hidden{}})", options);
    CHECK(resolutions == 0);
    CHECK(factories == 0);
  }
}

TEST_CASE("fluent protected publication failures retain TEXT phase and collect once") {
  Fixture fixture;
  ui::PasswordRef password;
  bool reject{};
  ui::PasswordConfig config;

  config.validate = [&](SecretView) -> bool {
    if (reject) {
      throw std::runtime_error("private validation message");
    }
    return true;
  };
  take(ui::mount(*fixture.owner, ui::password(password, config)));
  const auto candidate = take(PasswordValue::create("candidate"));
  const auto count = take(fixture.owner->diagnostics()).errors.size();
  reject = true;

  auto changed = password.value(candidate);

  REQUIRE_FALSE(changed);
  CHECK(changed.error().phase == ManagedPhase::TEXT);
  REQUIRE(changed.error().password);
  const auto *access = std::get_if<PasswordValueAccessFailure<std::monostate>>(&*changed.error().password);

  REQUIRE(access);
  REQUIRE(access->access);
  REQUIRE(access->access->primary);
  CHECK(access->access->primary->code == SecureStringErrorCode::CALLBACK_FAILED);
  CHECK(std::get<TreeError>(changed.error().cause) == TreeError::CALLBACK_FAILED);
  REQUIRE(changed.error().operation);
  CHECK(std::string_view(changed.error().operation) == "password.fluent");
  const auto diagnostics = take(fixture.owner->diagnostics());

  REQUIRE(diagnostics.errors.size() == count + 1);
  CHECK(diagnostics.errors.back().error.phase == ManagedPhase::TEXT);
  REQUIRE(diagnostics.errors.back().error.password);
  CHECK(std::holds_alternative<PasswordValueAccessFailure<std::monostate>>(*diagnostics.errors.back().error.password));
}
