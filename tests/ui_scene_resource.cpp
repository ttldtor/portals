// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include "../examples/scene_resource/form.hpp"
#include "support/examples/replay/terminal.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <type_traits>

using namespace portals;

static_assert(std::is_aggregate_v<ManagedError>);
static_assert(std::is_aggregate_v<ui::SceneResourceOptions>);
static_assert(std::is_nothrow_copy_constructible_v<ManagedError>);
static_assert(std::is_nothrow_copy_assignable_v<ManagedError>);

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
std::ptrdiff_t activeBudget{-1};

void oomTerminate() {
  allocationsBeforeFailure = -1;
  std::fprintf(stderr, "resource OOM terminate at budget=%td\n", activeBudget);
  std::abort();
}
} // namespace

// Fault injection is local to this test process and armed only around checked operations.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure == 0) {
    throw std::bad_alloc();
  }

  if (allocationsBeforeFailure > 0) {
    --allocationsBeforeFailure;
  }

  auto *memory = std::malloc(size ? size : 1);

  if (!memory) {
    throw std::bad_alloc();
  }

  return memory;
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return ::operator new(size, std::nothrow);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

namespace {
using Code = SceneResourceErrorCode;

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == expected;
}

ManagedError rejected(std::string_view source, ui::SceneResourceOptions options = {}) {
  auto owner = ui::scene(source, std::move(options));
  auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  REQUIRE(result.error().sceneResource);

  return result.error();
}

struct TemporaryFile {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-resource-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");

  ~TemporaryFile() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
  }

  void write(std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);

    REQUIRE(output);
    output << bytes;
    REQUIRE(output);
  }
};

struct Custom final : SizedWidget {
  static inline int alive{};
  std::string text;

  explicit Custom(std::string_view value) : text(value) {
    ++alive;
  }

  ~Custom() override {
    --alive;
  }
};
} // namespace

TEST_CASE("runtime resource retains temporary source and publishes typed named refs") {
  auto owner = portals::ui::scene(std::string("VBox{ LineEdit#message{\"\"} Button#add{\"Add\"} }"));
  auto add = owner.get<portals::Button>("add");

  REQUIRE(owner.prepareChecked());
  REQUIRE(add.text().get());
  CHECK(*add.text().get() == "Add");
  REQUIRE(owner.close());
  CHECK_FALSE(add.text().get());
}

TEST_CASE("resource properties use existing controls layout and PSS through named refs") {
  auto owner = ui::scene(R"(VBox#root{ gap = 1; width: 32
    Label#caption{ text = "Hello" }
    HBox{ Button#add{"Add" flex=2} CheckBox#choice{"Yes" checked=true} }
    LineEdit#message{text="initial" height=1}
  })") | ui::style("Label { bold: true; }");
  auto label = owner.get<Label>("caption");
  auto message = owner.get<LineEdit>("message");
  auto choice = owner.get<CheckBox>("choice");
  bool configured{};

  owner.configure([&](ManagedScene &managed, Application &application) {
    configured = true;
    REQUIRE(label.text().get());
    CHECK(*label.text().get() == "Hello");
    REQUIRE(message.text().get());
    CHECK(*message.text().get() == "initial");
    REQUIRE(application.tree().read(choice.node(), [](const CheckBox &widget) {
      CHECK(widget.checked());
    }));
    CHECK(managed.root() != Node<Widget>{});
  });

  REQUIRE(owner.prepareChecked());
  CHECK(configured);
  REQUIRE(owner.close());
}

TEST_CASE("complete schema and duplicate validation precede custom factories") {
  struct Scenario {
    std::string_view source;
    Code expected;
  };

  const Scenario scenarios[]{
    {"VBox{ Probe{} Missing{} }", Code::UNKNOWN_TYPE},
    {"VBox{ Probe{} Button{unknown=1} }", Code::UNKNOWN_PROPERTY},
    {"VBox{ Probe#same{} Label#same{} }", Code::DUPLICATE_ID},
    {"VBox{ Probe{} Button{text=\"a\" text:\"b\"} }", Code::DUPLICATE_PROPERTY},
    {"VBox{ Probe{} Button{\"a\" text=\"b\"} }", Code::DUPLICATE_PROPERTY},
    {"VBox{ Probe{} Label{Button{}} }", Code::CHILDREN_FORBIDDEN},
    {"VBox{ Probe{} Label{width=2+3} }", Code::SYNTAX},
    {"VBox{ Probe{} Label{style=someExpression()} }", Code::UNKNOWN_PROPERTY},
    {"VBox{ Probe{} Label{width=-1} }", Code::INVALID_VALUE},
    {"VBox{ Probe{} Label{flex=0} }", Code::INVALID_VALUE},
    {"VBox{ Probe{} Label{height=2147483648} }", Code::INVALID_VALUE},
    {"VBox{ Probe{} CheckBox{checked=1} }", Code::INVALID_VALUE},
    {"VBox{ Probe{}", Code::SYNTAX},
  };

  for (const auto &scenario : scenarios) {
    CAPTURE(scenario.source);

    int calls{};
    ui::SceneResourceOptions options;

    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments arguments) {
      ++calls;

      return ui::custom<Custom>(std::move(arguments.text));
    }));

    const auto failure = rejected(scenario.source, std::move(options));

    CHECK(failure.sceneResource->code == scenario.expected);
    CHECK(calls == 0);
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("resource failures own exact source and byte based position after owner destruction") {
  std::string source = "VBox{\n  Label{\"Привет\"}\n  Unknown{}\n}";
  const auto expected = source;
  const auto failure = rejected(source);

  source.assign("replaced");
  REQUIRE(failure.sceneResource->source);
  CHECK(*failure.sceneResource->source == expected);
  CHECK(failure.sceneResource->offset == expected.find("Unknown"));
  CHECK(failure.sceneResource->line == 3);
  CHECK(failure.sceneResource->column == 3);
  CHECK(cause(failure, TreeError::INVALID_ARGUMENT));
}

TEST_CASE("source UTF8 is checked including comments and escaped text is owning") {
  for (const std::string invalid : {std::string("Label{\"\xC0\xAF\"}"), std::string("Label{} // \xF4\x90\x80\x80")}) {
    CHECK(rejected(invalid).sceneResource->code == Code::INVALID_UTF8);
  }

  CHECK(rejected("Label{\"\\q\"}").sceneResource->code == Code::SYNTAX);
  CHECK(rejected("Label{\"unfinished}").sceneResource->code == Code::SYNTAX);

  auto owner = ui::scene(R"(Label#text{"say \"hello\"\\"})");
  auto text = owner.get<Label>("text");

  REQUIRE(owner.prepareChecked());
  REQUIRE(text.text().get());
  CHECK(*text.text().get() == "say \"hello\"\\");
}

TEST_CASE("all parser limits reject before factories and exact boundaries remain usable") {
  ui::SceneResourceOptions options;

  options.limits.maxSourceBytes = 7;
  CHECK(rejected("Label{\"\"}", options).sceneResource->code == Code::SOURCE_LIMIT);
  options.limits = {};
  options.limits.maxNodes = 1;
  CHECK(rejected("VBox{Label{}}", options).sceneResource->code == Code::NODE_LIMIT);
  options.limits = {};
  options.limits.maxDepth = 1;
  CHECK(rejected("VBox{Label{}}", options).sceneResource->code == Code::DEPTH_LIMIT);
  options.limits = {};
  options.limits.maxStringBytes = 2;
  CHECK(rejected("Label{\"abc\"}", options).sceneResource->code == Code::STRING_LIMIT);
  options.limits = {};
  options.limits.maxProperties = 1;
  CHECK(rejected("Label{text=\"a\" width=1}", options).sceneResource->code == Code::PROPERTY_LIMIT);
  options.limits = {};
  options.limits.maxDepth = 257;
  CHECK(rejected("Label{}", options).sceneResource->code == Code::INVALID_LIMIT);
  options.limits = {};
  options.limits.maxNodes = 0;
  CHECK(rejected("Label{}", options).sceneResource->code == Code::INVALID_LIMIT);
  options.limits = {.maxSourceBytes = 10, .maxNodes = 1, .maxDepth = 1, .maxStringBytes = 1, .maxProperties = 1};

  auto owner = ui::scene("Label{\"a\"}", options);

  REQUIRE(owner.prepareChecked());
}

TEST_CASE("resource depth limits include exact default and configured boundaries") {
  for (const std::size_t depth : {std::size_t{16}, std::size_t{32}, std::size_t{64}}) {
    std::string source;

    for (std::size_t index = 0; index < depth; ++index) {
      source += "VBox{";
    }

    source.append(depth, '}');

    ui::SceneResourceOptions options;

    options.limits.maxDepth = depth;

    example::ReplayTerminal terminal({}, 100, {24, 8});
    ui::SceneOptions sceneOptions;

    sceneOptions.clock = terminal.clock();

    auto owner = ui::scene(source, options, sceneOptions);
    auto prepared = owner.prepareChecked();

    REQUIRE(prepared);

    auto rendered = owner.run(terminal);

    REQUIRE(rendered);
    CHECK(rendered->frames == 1);
    CHECK(terminal.closed);

    options.limits.maxDepth = depth - 1;
    CHECK(rejected(source, options).sceneResource->code == Code::DEPTH_LIMIT);
  }

  ui::SceneResourceOptions unsupported;

  unsupported.limits.maxDepth = 256;
  CHECK(rejected("VBox{}", unsupported).sceneResource->code == Code::INVALID_LIMIT);

  std::string overlong;

  for (std::size_t index = 0; index < 256; ++index) {
    overlong += "VBox{";
  }

  overlong.append(256, '}');
  CHECK(rejected(overlong).sceneResource->code == Code::DEPTH_LIMIT);
}

TEST_CASE("resource files are explicit lazy bounded and retain file identity on failure") {
  TemporaryFile file;
  auto owner = ui::scene(ui::SceneResourceFile{file.path});

  file.write("Button#loaded{\"from file\"}");

  auto loaded = owner.get<Button>("loaded");

  REQUIRE(owner.prepareChecked());
  REQUIRE(loaded.text().get());
  CHECK(*loaded.text().get() == "from file");

  ui::SceneResourceOptions small;

  small.limits.maxSourceBytes = 5;

  auto limited = ui::scene(ui::SceneResourceFile{file.path}, small);
  auto limit = limited.prepareChecked();

  REQUIRE_FALSE(limit);
  REQUIRE(limit.error().sceneResource);
  CHECK(limit.error().sceneResource->code == Code::SOURCE_LIMIT);
  CHECK(*limit.error().sceneResource->source == file.path.generic_string());

  std::error_code ignored;

  std::filesystem::remove(file.path, ignored);

  example::ReplayTerminal terminal;
  auto absent = ui::scene(ui::SceneResourceFile{file.path});
  auto failure = absent.prepareChecked();

  REQUIRE_FALSE(failure);
  REQUIRE(failure.error().sceneResource);
  CHECK(failure.error().sceneResource->code == Code::FILE_IO);
  CHECK(failure.error().sceneResource->system);
  CHECK(absent.runApp(terminal) == 1);
  CHECK(terminal.output.empty());
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("explicit resolver localizes every text before invoking custom factories") {
  int resolved{}, factories{};
  ui::SceneResourceOptions options;

  options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++resolved;

    return key == "greeting" ? "Здравствуйте" : "Добавить";
  };
  REQUIRE(options.registry.add<Custom>("Localized", [&](ui::SceneResourceArguments arguments) {
    ++factories;
    CHECK(resolved == 2);
    CHECK(arguments.text == "Здравствуйте");

    return ui::custom<Custom>(std::move(arguments.text));
  }));

  auto owner = ui::scene("VBox{ Localized{i18n(\"greeting\")} Button#add{text=i18n(\"add\")} }", options);
  auto add = owner.get<Button>("add");

  REQUIRE(owner.prepareChecked());
  CHECK(factories == 1);
  REQUIRE(add.text().get());
  CHECK(*add.text().get() == "Добавить");
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);

  const auto missing = rejected("Label{i18n(\"absent\")}");

  REQUIRE(missing.resource);
  CHECK(*missing.resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK(missing.sceneResource->code == Code::TEXT);

  options.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  };

  const auto notFound = rejected("VBox{ Localized{} Label{i18n(\"absent\")} }", options);

  CHECK(factories == 1);
  REQUIRE(notFound.resource);
  CHECK(*notFound.resource == TextResourceError::MISSING_RESOURCE);
}

TEST_CASE("custom registry rejects replacement and retains factory captures until prepare") {
  ui::SceneResourceOptions options;
  auto text = std::make_shared<std::string>("custom text");
  auto weak = std::weak_ptr(text);
  ui::Ref<Custom> reference;

  REQUIRE(options.registry.add<Custom>("Custom", [text, reference](ui::SceneResourceArguments) {
    return ui::custom<Custom>(reference, *text);
  }));
  CHECK_FALSE(options.registry.add<Custom>("Button", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>("");
  }));
  CHECK_FALSE(options.registry.add<Custom>("bad-name", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>("");
  }));

  auto owner = ui::scene("Custom#custom{width=8}", std::move(options));

  text.reset();
  CHECK_FALSE(weak.expired());
  REQUIRE(owner.prepareChecked());
  CHECK(reference.node() != Node<Custom>{});
  CHECK(Custom::alive == 1);
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("custom mount failure rolls back complete resource without publishing earlier references") {
  ui::Ref<Custom> reference;
  ui::SceneResourceOptions options;

  REQUIRE(options.registry.add<Custom>("Good", [reference](ui::SceneResourceArguments) {
    return ui::custom<Custom>(reference, "alive");
  }));
  REQUIRE(options.registry.add<Custom>("Fail", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>("failure").setup([](ManagedScene &, Node<Custom>) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WOULD_BLOCK});
    });
  }));

  auto owner = ui::scene("VBox{ Good{}\n Fail{} }", std::move(options));
  auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::WOULD_BLOCK));
  CHECK(result.error().phase == ManagedPhase::BIND);
  REQUIRE(result.error().sceneResource);
  CHECK(result.error().sceneResource->line == 2);
  CHECK(result.error().sceneResource->column == 2);
  CHECK(Custom::alive == 0);
  CHECK(reference.node() == Node<Custom>{});
}

TEST_CASE("factory throw and typed rejection preserve exact cause and resource location") {
  for (const bool throwing : {false, true}) {
    ui::SceneResourceOptions options;

    REQUIRE(options.registry.add<Custom>(
      "Fail", [throwing](ui::SceneResourceArguments) -> std::expected<ui::Description<Custom>, ManagedError> {
        if (throwing) {
          throw std::runtime_error("factory");
        }

        return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT});
      }));

    const auto failure = rejected("VBox{\n Fail{} }", std::move(options));

    CHECK(cause(failure, throwing ? TreeError::CALLBACK_FAILED : TreeError::NODE_LIMIT));
    CHECK(failure.sceneResource->code == Code::FACTORY);
    CHECK(failure.sceneResource->line == 2);
  }
}

TEST_CASE("resource error copies share immutable context without allocating") {
  const auto original = rejected("VBox{\n Unknown{} }");
  ManagedError assigned{};

  allocationsBeforeFailure = 0;
  const ManagedError copied(original);
  assigned = original;
  allocationsBeforeFailure = -1;

  CHECK(copied.sceneResource == original.sceneResource);
  CHECK(assigned.sceneResource == original.sceneResource);
  REQUIRE(copied.sceneResource);
  CHECK(copied.sceneResource->code == Code::UNKNOWN_TYPE);
  CHECK(copied.sceneResource->line == 2);
  CHECK(cause(copied, TreeError::INVALID_ARGUMENT));
  CHECK(cause(assigned, TreeError::INVALID_ARGUMENT));
}

TEST_CASE("resource attribution allocation failure preserves original typed factory error") {
  ui::SceneResourceOptions options;
  REQUIRE(options.registry.add<Custom>(
    "Fail", [](ui::SceneResourceArguments) -> std::expected<ui::Description<Custom>, ManagedError> {
      allocationsBeforeFailure = 0;
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT});
    }));
  auto owner = ui::scene("VBox{\n Fail{} }", std::move(options));

  const auto result = owner.prepareChecked();
  allocationsBeforeFailure = -1;

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::NODE_LIMIT));
  CHECK(result.error().phase == ManagedPhase::STATE);
  CHECK_FALSE(result.error().sceneResource);
  CHECK(Custom::alive == 0);
  REQUIRE(owner.close());
}

TEST_CASE("prepare reentry and throwing notification cannot replace the first resource failure") {
  ui::SceneResourceOptions options;
  ui::Scene *active{};

  REQUIRE(options.registry.add<Custom>("Reenter", [&](ui::SceneResourceArguments) {
    auto nested = active->prepareChecked();

    REQUIRE_FALSE(nested);
    CHECK(cause(nested.error(), TreeError::WOULD_BLOCK));

    return ui::custom<Custom>("reentry");
  }));

  auto owner = ui::scene("Reenter{}", std::move(options));

  active = &owner;

  auto failure = owner.prepareChecked();

  REQUIRE_FALSE(failure);
  CHECK(cause(failure.error(), TreeError::WOULD_BLOCK));
  CHECK(Custom::alive == 0);

  auto invalid = ui::scene("Unknown{}");
  int notified{}, reported{};

  invalid.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notified;
    REQUIRE(diagnostic.error.sceneResource);
    CHECK(diagnostic.error.sceneResource->code == Code::UNKNOWN_TYPE);
    throw std::runtime_error("rejected notification");
  });
  invalid.reportOnError([&](const ui::SceneReport &report) {
    ++reported;
    REQUIRE(report.failure);
    REQUIRE(report.failure->sceneResource);
    CHECK(report.failure->sceneResource->code == Code::UNKNOWN_TYPE);
  });

  example::ReplayTerminal terminal;

  CHECK(invalid.runApp(terminal) == 1);
  CHECK(notified == 1);
  CHECK(reported == 1);
  CHECK(terminal.output.empty());
}

TEST_CASE("self removal during mount leaves no published resource identity") {
  ui::Ref<Custom> reference;
  ui::SceneResourceOptions options;

  REQUIRE(options.registry.add<Custom>("Gone", [reference](ui::SceneResourceArguments) {
    return ui::custom<Custom>(reference, "gone").setup([](ManagedScene &managed, Node<Custom> node) {
      return managed.remove(node);
    });
  }));

  auto owner = ui::scene("VBox{Gone{}}", std::move(options));
  auto failure = owner.prepareChecked();

  REQUIRE_FALSE(failure);
  CHECK(cause(failure.error(), TreeError::STALE_NODE));
  CHECK(reference.node() == Node<Custom>{});
  CHECK(Custom::alive == 0);
}

TEST_CASE("resource composes with a late managed model and the enclosing C++ transaction") {
  ui::Ref<Label> sibling;
  auto owner = ui::scene([sibling](ManagedScene &managed) -> std::expected<ui::Description<Column>, ManagedError> {
    auto state = managed.state(std::string("late model"));

    if (!state) {
      return std::unexpected(state.error());
    }

    ui::SceneResourceOptions options;
    const auto registered = options.registry.add<LineEdit>("BoundField", [state = *state](ui::SceneResourceArguments) {
      return ui::lineEdit(state);
    });

    REQUIRE(registered);

    return ui::vbox(ui::label(sibling, "sibling"),
                    ui::resource("VBox{BoundField#field{}}", std::move(options)).id("resource"));
  });
  auto field = owner.get<LineEdit>("field");
  auto root = owner.get<Column>("resource");

  REQUIRE(owner.prepareChecked());
  CHECK(root.node() != Node<Column>{});
  REQUIRE(field.text().get());
  CHECK(*field.text().get() == "late model");
  CHECK(sibling.node() != Node<Label>{});
  REQUIRE(owner.close());

  ui::Ref<Label> unpublished;
  auto invalid = ui::scene(ui::vbox(ui::label(unpublished, "sibling"), ui::resource("Missing{}")));

  REQUIRE_FALSE(invalid.prepareChecked());
  CHECK(unpublished.node() == Node<Label>{});
}

TEST_CASE("resource preparation allocation failures remain checked and retire partial ownership") {
  constexpr std::ptrdiff_t MAX_BUDGET = 2048;
  for (const bool localized : {false, true}) {
    std::size_t failed{}, succeeded{};

    for (std::ptrdiff_t budget = 0; budget <= MAX_BUDGET; ++budget) {
      ui::Ref<Custom> reference;
      ui::SceneResourceOptions options;

      options.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
        return "translated";
      };

      REQUIRE(options.registry.add<Custom>("Custom", [reference](ui::SceneResourceArguments arguments) {
        return ui::custom<Custom>(reference, std::move(arguments.text));
      }));

      const auto source =
        localized
          ? "VBox{Custom{i18n(\"own\")} LineEdit#field{i18n(\"initial\")} HBox{Label#caption{i18n(\"caption\")} "
            "Button#button{i18n(\"button\")} CheckBox#check{i18n(\"check\") checked=true}}}"
          : "VBox{Custom{\"owning temporary\"} LineEdit#field{\"initial\"} HBox{Label#caption{\"label\"} "
            "Button#button{\"button\"} CheckBox#check{\"check\" checked=true}}}";
      auto owner = ui::scene(source, std::move(options));
      std::expected<void, ManagedError> result;
      bool escaped{};

      const auto previousTerminate = std::set_terminate(oomTerminate);

      activeBudget = budget;
      allocationsBeforeFailure = budget;

      try {
        result = owner.prepareChecked();
      } catch (...) {
        escaped = true;
      }

      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);

      CAPTURE(budget);
      REQUIRE_FALSE(escaped);

      if (!result) {
        ++failed;

        const auto *application = std::get_if<ApplicationError>(&result.error().cause);

        CHECK((cause(result.error(), TreeError::NO_MEMORY) || application));
        CHECK(Custom::alive == 0);
        CHECK(reference.node() == Node<Custom>{});

        // Persistent allocation rejection also prevents best-effort diagnostic context.
        CHECK_FALSE(result.error().sceneResource);
      } else {
        ++succeeded;
      }

      REQUIRE(owner.close());
      CHECK(Custom::alive == 0);

      if (succeeded) {
        break;
      }
    }

    CHECK(failed > 0);
    CHECK(succeeded == 1);
    std::fprintf(stderr, "resource OOM sweep localized=%d failures=%zu first-success-budget=%td\n", localized, failed,
                 activeBudget);
  }
}

TEST_CASE("custom recipe preserves nonstring move-only arguments with and without owning text") {
  struct Owned final : Widget {
    std::string text;
    std::unique_ptr<int> value;

    explicit Owned(std::unique_ptr<int> input) : value(std::move(input)) {
    }

    Owned(std::string_view label, std::unique_ptr<int> input) : text(label), value(std::move(input)) {
    }
  };

  ui::Ref<Owned> first, second;
  auto one = std::make_unique<int>(41);
  auto two = std::make_unique<int>(42);
  auto owner = ui::scene(ui::vbox(ui::custom<Owned>(first, std::move(one)),
                                  ui::custom<Owned>(second, std::string("owned label"), std::move(two))));
  bool verified{};

  owner.configure([&](ManagedScene &, Application &application) {
    REQUIRE(application.tree().read(first.node(), [](const Owned &widget) {
      CHECK(*widget.value == 41);
    }));
    REQUIRE(application.tree().read(second.node(), [](const Owned &widget) {
      CHECK(*widget.value == 42);
      CHECK(widget.text == "owned label");
    }));
    verified = true;
  });

  CHECK_FALSE(one);
  CHECK_FALSE(two);
  REQUIRE(owner.prepareChecked());
  CHECK(verified);
}

TEST_CASE("complete public resource form handles Add Hello and F10 with existing input routing") {
  example::ReplayTerminal terminal({{20, "\t\r", {}}, {60, "\x1b[21~", {}}}, 100, {40, 8});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  auto owner = scene_resource_example::form(options);
  auto message = owner.get<LineEdit>("message");
  std::string final;

  owner.onStop([&] {
    auto value = message.text().get();

    REQUIRE(value);
    final = *value;
  });

  const auto result = owner.run(terminal);

  REQUIRE(result);
  CHECK(final == "Hello!");
  CHECK(terminal.closed);
  CHECK(result->frames >= 2);
  CHECK_FALSE(message.text().get());
}

TEST_CASE("fluent resource controls retain lvalue identity and consume temporary chains") {
  int created{};
  ui::SceneResourceOptions options;
  auto factory = [&](ui::SceneResourceArguments arguments) {
    ++created;

    return ui::custom<Custom>(arguments.text);
  };

  CHECK(&options.control<Custom>("First", factory).control<Custom>("Second", factory) == &options);
  CHECK(created == 0);

  auto owner = ui::scene("VBox{First{} Second{}}", options);

  REQUIRE(owner.prepareChecked());
  CHECK(created == 2);
  CHECK(Custom::alive == 2);
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);

  auto temporary = ui::scene("Group{Leaf{}}", ui::SceneResourceOptions{}
                                                .control<Column>(
                                                  "Group",
                                                  [](ui::SceneResourceArguments) {
                                                    return ui::column();
                                                  },
                                                  true)
                                                .control<Custom>("Leaf", factory));

  REQUIRE(temporary.prepareChecked());
  CHECK(created == 3);
  REQUIRE(temporary.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("fluent resource registration rejects before parser resolver factories and terminal") {
  for (const auto name : {"Label", "bad-name"}) {
    int created{}, resolved{}, notified{};
    ui::SceneResourceOptions options;

    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolved;

      return "unused";
    };
    auto factory = [&](ui::SceneResourceArguments) {
      ++created;

      return ui::custom<Custom>("unused");
    };

    options.control<Custom>("Good", factory).control<Custom>(name, factory);

    auto owner = ui::scene("invalid syntax i18n(\"unused\")", options);
    const auto expected = name == std::string_view("Label") ? Code::DUPLICATE_TYPE : Code::UNKNOWN_TYPE;

    owner.onError([&](const ManagedDiagnostic &diagnostic) {
      ++notified;
      REQUIRE(diagnostic.error.sceneResource);
      CHECK(diagnostic.error.sceneResource->code == expected);
      CHECK(diagnostic.error.sceneResource->line == 0);
      CHECK(diagnostic.error.sceneResource->column == 0);
      CHECK(cause(diagnostic.error, TreeError::INVALID_ARGUMENT));
    });

    example::ReplayTerminal terminal;

    CHECK(owner.runApp(terminal) == 1);
    CHECK(notified == 1);
    CHECK(created == 0);
    CHECK(resolved == 0);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);

    CHECK(rejected("Good{i18n(\"unused\")}", options).sceneResource->code == expected);
    CHECK(created == 0);
    CHECK(resolved == 0);

    TemporaryFile absent;
    auto fileOwner = ui::scene(ui::SceneResourceFile{absent.path}, std::move(options));
    auto fileResult = fileOwner.prepareChecked();

    REQUIRE_FALSE(fileResult);
    REQUIRE(fileResult.error().sceneResource);
    CHECK(fileResult.error().sceneResource->code == expected);
    CHECK(fileResult.error().sceneResource->line == 0);
    CHECK(created == 0);
    CHECK(resolved == 0);
  }
}

namespace {
struct RegistrationFactory {
  std::shared_ptr<int> copies;
  bool *reject{};
  bool allocationFailure{};

  RegistrationFactory(std::shared_ptr<int> counter, bool &rejection, bool allocation = false)
      : copies(std::move(counter)), reject(&rejection), allocationFailure(allocation) {
  }

  RegistrationFactory(const RegistrationFactory &other)
      : copies(other.copies), reject(other.reject), allocationFailure(other.allocationFailure) {
    ++*copies;

    if (*reject) {
      if (allocationFailure) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("registration copy");
    }
  }

  RegistrationFactory(RegistrationFactory &&) = default;

  ui::Description<Custom> operator()(ui::SceneResourceArguments arguments) {
    return ui::custom<Custom>(arguments.text);
  }
};
} // namespace

TEST_CASE("fluent registration first failure stops later capture work and survives copies") {
  bool reject{};
  auto copies = std::make_shared<int>();
  RegistrationFactory factory(copies, reject);
  ui::SceneResourceOptions options;

  options.control<Custom>("bad-name", factory);

  const auto before = *copies;

  reject = true;
  options.control<Custom>("Label", factory);
  CHECK(*copies == before);

  auto copy = options;
  auto moved = std::move(copy);

  options = {};
  CHECK(rejected("Label{}", std::move(moved)).sceneResource->code == Code::UNKNOWN_TYPE);
}

TEST_CASE("fluent resource callbacks and options copies retain owning captures") {
  auto lifetime = std::make_shared<std::string>("retained");
  std::weak_ptr<std::string> observed = lifetime;
  ui::SceneResourceOptions options;

  options.control<Custom>("Owned", [lifetime](ui::SceneResourceArguments) {
    return ui::custom<Custom>(*lifetime);
  });

  auto copy = options;
  auto moved = std::move(copy);

  lifetime.reset();
  options = {};

  {
    auto owner = ui::scene("Owned{}", std::move(moved));

    REQUIRE(owner.prepareChecked());
    CHECK_FALSE(observed.expired());
    REQUIRE(owner.close());
  }

  // Registry has a fallible copy transfer on checked MSVC; release any source captures too.
  copy = {};
  moved = {};
  CHECK(observed.expired());
  CHECK(Custom::alive == 0);
}

TEST_CASE("checked registry failure remains recoverable before fluent registration") {
  ui::SceneResourceOptions options;
  auto factory = [](ui::SceneResourceArguments arguments) {
    return ui::custom<Custom>(arguments.text);
  };

  CHECK_FALSE(options.registry.add<Custom>("Label", factory));
  CHECK_FALSE(options.registry.add<Custom>("bad-name", factory));
  options.control<Custom>("Recovered", factory);

  auto owner = ui::scene("Recovered{}", std::move(options));

  REQUIRE(owner.prepareChecked());
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("fluent registration captures internal callback copy exceptions as checked failures") {
  for (const bool allocation : {false, true}) {
    bool reject = true;
    RegistrationFactory factory(std::make_shared<int>(), reject, allocation);
    ui::SceneResourceOptions options;

    CHECK_NOTHROW(options.control<Custom>("Throwing", factory));

    reject = false;
    options.control<Custom>("Label", factory);

    const auto failure = rejected("Throwing{}", std::move(options));

    CHECK(cause(failure, allocation ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED));
    CHECK(failure.sceneResource->code == (allocation ? Code::NO_MEMORY : Code::FACTORY));
    CHECK(failure.sceneResource->line == 0);
  }
}

TEST_CASE("fluent registration allocation rejection retains an allocation-free first failure") {
  std::size_t failures{};
  bool succeeded{};

  for (std::ptrdiff_t budget = 0; budget < 128 && !succeeded; ++budget) {
    ui::SceneResourceOptions options;
    auto factory = [](ui::SceneResourceArguments arguments) {
      return ui::custom<Custom>(arguments.text);
    };
    bool escaped{};

    allocationsBeforeFailure = budget;

    try {
      options.control<Custom>("AllocationProbeWithAnOwningRegistryName", factory);
    } catch (...) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;
    CHECK_FALSE(escaped);

    auto owner = ui::scene("AllocationProbeWithAnOwningRegistryName{}", std::move(options));
    auto result = owner.prepareChecked();

    if (!result) {
      ++failures;
      CHECK(cause(result.error(), TreeError::NO_MEMORY));
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == Code::NO_MEMORY);
      CHECK(result.error().sceneResource->line == 0);
    } else {
      succeeded = true;
    }

    REQUIRE(owner.close());
    CHECK(Custom::alive == 0);
  }

  CHECK(failures > 0);
  CHECK(succeeded);
}
