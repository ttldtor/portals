// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/examples/replay/terminal.hpp"
#include <doctest.h>
#include <portals/ui_style.hpp>

#include <fstream>
#include <thread>

using namespace portals;

namespace {
bool cause(const ManagedError &error, TreeError expected) {
  if (const auto *tree = std::get_if<TreeError>(&error.cause)) {
    return *tree == expected;
  }

  const auto *style = std::get_if<StyleError>(&error.cause);

  return style && style->cause == expected;
}

std::string textOf(const auto &reference) {
  auto result = reference.text().get();

  REQUIRE(result);

  return *result;
}

ManagedError textFailure(const auto &reference) {
  auto result = reference.text().get();

  REQUIRE_FALSE(result);

  return result.error();
}

ManagedError failureOf(ui::Scene &owner) {
  auto result = owner.checked();

  REQUIRE_FALSE(result);

  return result.error();
}

std::size_t diagnosticsCount(ui::Scene &owner) {
  auto result = owner.diagnostics();

  REQUIRE(result);

  return result->errors.size();
}

std::size_t styleCount(ManagedScene &scene) {
  auto result = scene.styleSources();

  REQUIRE(result);

  return result->size();
}

ComputedStyle computed(ui::Scene &owner, Node<Widget> node) {
  ComputedStyle result;

  owner.configure([&](ManagedScene &, Application &app) {
    REQUIRE(app.tree().read(node, [&](const Widget &widget) {
      REQUIRE(widget.computedStyle());
      result = widget.computedStyle()->flatten();
    }));
  });

  return result;
}

struct File {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-lookup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pss");

  ~File() {
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
} // namespace

TEST_CASE("named refs publish after full mount before pending handlers and survive facade move") {
  auto owner = ui::scene(ui::vbox(ui::button("Add").id("add"), ui::lineEdit("").id("message")));
  auto dynamic = owner.get("add");
  ui::Ref<Button> add = dynamic;
  auto message = owner.get<LineEdit>("message");
  bool configured{};

  owner.configure([&](ManagedScene &) {
    configured = add.node() != Node<Button>{} && message.node() != Node<LineEdit>{};
    message.text() = "ready";
  });

  CHECK(add.node() == Node<Button>{});
  CHECK_FALSE(owner.managed());

  auto moved = std::move(owner);

  REQUIRE(moved.prepareChecked());
  CHECK(configured);
  CHECK(textOf(dynamic) == "Add");
  CHECK(textOf(message) == "ready");
  REQUIRE(moved.close());
  CHECK(cause(textFailure(message), TreeError::CLOSED));
}

TEST_CASE("missing wrong empty and ambiguous lookup preserve owned name and typed source before terminal") {
  for (const int mode : {0, 1, 2, 3}) {
    example::ReplayTerminal terminal;
    auto owner = ui::scene(
      ui::vbox(ui::button("A").id("duplicate"), ui::button("B").id("duplicate"), ui::label("label").id("label")));
    int notifications{}, reports{};
    bool configured{};

    owner.onError([&](const ManagedDiagnostic &diagnostic) {
      ++notifications;
      CHECK(cause(diagnostic.error, TreeError::INVALID_ARGUMENT));
    });
    owner.reportOnError([&](const ui::SceneReport &report) {
      ++reports;
      REQUIRE(report.failure);
      CHECK(cause(*report.failure, TreeError::INVALID_ARGUMENT));
    });

    std::string name = mode == 0 ? "missing" : mode == 1 ? "label" : mode == 2 ? "" : "duplicate";
    const auto expectedName = name;
    const auto origin = std::source_location::current();
    auto target = owner.get<Button>(name, origin);

    name.assign("source buffer replaced");
    owner.configure([&](ManagedScene &) {
      configured = true;
    });
    CHECK(owner.runApp(terminal) == 1);
    CHECK_FALSE(configured);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);
    CHECK(notifications == 1);
    CHECK(reports == 1);
    CHECK(target.node() == Node<Button>{});
    CHECK(cause(textFailure(target), TreeError::INVALID_ARGUMENT));

    auto errors = owner.diagnostics();

    REQUIRE(errors);
    REQUIRE(errors->errors.size() == 1);
    REQUIRE_FALSE(errors->errors.front().contexts.empty());
    CHECK(errors->errors.front().contexts.front().name == expectedName);
    CHECK(errors->errors.front().origin.line() == origin.line());
  }
}

TEST_CASE("immediate dynamic capabilities and stale identity use checked existing boundaries") {
  auto owner = ui::scene(ui::vbox(ui::button("A").id("a"), ui::column().id("group")));

  REQUIRE(owner.prepareChecked());

  auto a = owner.get("a");
  auto group = owner.get("group");

  CHECK(cause(textFailure(group), TreeError::INVALID_ARGUMENT));
  REQUIRE(a.text().set("changed").result());
  CHECK(textOf(a) == "changed");

  auto managed = owner.managed();

  REQUIRE(managed);
  REQUIRE(managed->get().id(a.node(), "renamed"));
  CHECK(textOf(a) == "changed");
  REQUIRE(managed->get().remove(a.node()));
  CHECK(cause(textFailure(a), TreeError::STALE_NODE));

  auto replacement = ui::mount(managed->get(), ui::button("new").id("a"));

  REQUIRE(replacement);
  CHECK(cause(textFailure(a), TreeError::STALE_NODE));
  CHECK(textOf(owner.get("a")) == "new");
}

TEST_CASE("named refs reject foreign thread before publication state and retain no owner") {
  auto owner = ui::scene(ui::button("A").id("a"));
  auto target = owner.get<Button>("a");

  REQUIRE(owner.prepareChecked());

  bool rejected{};
  std::thread foreign([target, &rejected] {
    auto result = target.text().get();

    rejected = !result && cause(result.error(), TreeError::WRONG_THREAD);
  });

  foreign.join();
  CHECK(rejected);
  REQUIRE(owner.close());
  CHECK(cause(textFailure(target), TreeError::CLOSED));
}

TEST_CASE("scoped style uses one resolver preserving selectors tiers source and node identity") {
  ui::Ref<Button> first, second;
  auto owner = ui::scene(ui::vbox(ui::button(first, "A").id("a"), ui::button(second, "B").id("b")));
  auto named = owner.get("a");
  const auto scoped = ui::style(named, "Control { bold: true; }", {.sourceName = "scoped.pss"});

  owner | ui::style("Control { italic: true; }") | scoped;
  REQUIRE(owner.prepareChecked());
  CHECK(computed(owner, first.node()).normal.bold);
  CHECK_FALSE(computed(owner, second.node()).normal.bold);
  CHECK(computed(owner, first.node()).normal.italic);
  CHECK(computed(owner, second.node()).normal.italic);

  auto managed = owner.managed();

  REQUIRE(managed);

  auto sources = managed->get().styleSources();

  REQUIRE(sources);
  REQUIRE(sources->size() == 4);
  REQUIRE(sources->back().document);
  CHECK(sources->back().document->sourceName == "scoped.pss");
  CHECK(sources->back().document->sheet.scope == first.node().id());
  CHECK(sources->back().document->sheet.rules.front().selector.type == "Control");
  owner | scoped;
  CHECK(styleCount(managed->get()) == 4);
  owner.theme(modernTheme());
  REQUIRE(owner.checked());
  CHECK(computed(owner, first.node()).normal.bold);
  CHECK_FALSE(computed(owner, second.node()).normal.bold);

  owner | ui::style(first, "Control { bold: false; }", {.tier = StyleTier::USER});
  REQUIRE(owner.checked());
  CHECK_FALSE(computed(owner, first.node()).normal.bold);
}

TEST_CASE("scoped style excludes descendants foreign scenes and retired targets") {
  ui::Ref<Column> group;
  ui::Ref<Button> child;
  auto owner = ui::scene(ui::vbox(group, ui::button(child, "child")));

  owner | ui::style(group, "* { bold: true; }");
  REQUIRE(owner.prepareChecked());
  CHECK(computed(owner, group.node()).normal.bold);
  CHECK_FALSE(computed(owner, child.node()).normal.bold);

  auto foreign = ui::scene(ui::button("foreign"));

  foreign | ui::style(child, "* { italic: true; }");
  auto rejected = foreign.prepareChecked();

  REQUIRE_FALSE(rejected);
  CHECK(cause(rejected.error(), TreeError::WRONG_SESSION));

  auto managed = owner.managed();

  REQUIRE(managed);
  REQUIRE(managed->get().remove(child.node()));
  owner | ui::style(child, "* { underline: true; }");
  CHECK(cause(failureOf(owner), TreeError::STALE_NODE));
}

TEST_CASE("styleFile reads lazily and rejects missing or invalid input before terminal activity") {
  File file;
  auto owner = ui::scene(ui::button("A").id("a"));
  auto target = owner.get<Button>("a");
  auto stage = ui::styleFile(target, file.path);

  owner | stage;
  CHECK(owner.checked());
  file.write("* { bold: true; }");
  REQUIRE(owner.prepareChecked());
  CHECK(computed(owner, target.node()).normal.bold);

  for (const bool invalid : {false, true}) {
    File failure;
    example::ReplayTerminal terminal;
    auto rejected = ui::scene(ui::button("B").id("b"));
    auto reference = rejected.get("b");
    std::optional<ManagedError> error;

    if (invalid) {
      failure.write("* { bold: perhaps; }");
    }

    rejected | ui::styleFile(reference, failure.path);
    rejected.reportOnError([&](const ui::SceneReport &report) {
      error = report.failure;
    });
    CHECK(rejected.runApp(terminal) == 1);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);
    REQUIRE(error);
    CHECK(error->phase == ManagedPhase::STYLE);
    CHECK(error->styleSource);
    CHECK((invalid ? error->pss.has_value() : error->file.has_value()));
  }
}

TEST_CASE("lookup queue budget prevents startup without a partially bound request") {
  ui::SceneOptions options;

  options.maxConfigurations = 1;

  auto owner = ui::scene(ui::button("A").id("a"), options);
  auto first = owner.get<Button>("a");
  auto second = owner.get<Button>("a");

  CHECK(cause(failureOf(owner), TreeError::NODE_LIMIT));
  CHECK_FALSE(owner.prepareChecked());
  CHECK(first.node() == Node<Button>{});
  CHECK(second.node() == Node<Button>{});
}

TEST_CASE("implicit conversion of missing dynamic lookup retains one primary error in the typed slot") {
  auto owner = ui::scene(ui::button("A"));
  ui::Ref<Button> missing = owner.get("missing");
  int notifications{};

  owner.onError([&](const ManagedDiagnostic &) {
    ++notifications;
  });
  CHECK_FALSE(owner.prepareChecked());
  CHECK(cause(textFailure(missing), TreeError::INVALID_ARGUMENT));
  CHECK(notifications == 1);
  CHECK(diagnosticsCount(owner) == 1);
}

TEST_CASE("scoped sized strings preserve embedded NUL and moved refs never become global styles") {
  for (const bool moved : {false, true}) {
    ui::Ref<Button> target;
    auto owner = ui::scene(ui::button(target, "A"));

    if (moved) {
      auto retained = std::move(target);

      owner | ui::style(target, "* { bold: true; }");
      auto prepared = owner.prepareChecked();

      REQUIRE_FALSE(prepared);
      CHECK(cause(prepared.error(), TreeError::CLOSED));
    } else {
      owner | ui::style(target, "* { bold: true; }\0* { italic: true; }");
      auto prepared = owner.prepareChecked();

      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().pss);
      CHECK(prepared.error().pss->code == PssErrorCode::NUL_BYTE);
    }
  }
}

TEST_CASE("dynamic conversion and click preserve original identity across rename and ID reuse") {
  auto owner = ui::scene(ui::button("original").id("x"));
  auto original = owner.get("x");
  ui::Ref<Button> deferred = original;

  REQUIRE(owner.prepareChecked());

  auto managed = owner.managed();

  REQUIRE(managed);
  REQUIRE(managed->get().id(original.node(), "renamed"));

  auto replacement = ui::mount(managed->get(), ui::button("replacement").id("x"));

  REQUIRE(replacement);

  ui::Ref<Button> converted = original;

  CHECK(converted.node() == deferred.node());
  CHECK(converted.node() != *replacement);
  CHECK(textOf(converted) == "original");

  int calls{};

  REQUIRE(original.onClick([&] {
    ++calls;
  }));
  REQUIRE(managed->get().remove(original.node()));

  ui::Ref<Button> stale = original;
  auto rejected = original.onClick([&] {
    ++calls;
  });

  CHECK(cause(textFailure(stale), TreeError::STALE_NODE));
  REQUIRE_FALSE(rejected);
  CHECK(cause(rejected.error(), TreeError::STALE_NODE));
  CHECK(stale.node() == Node<Button>{});
  CHECK(calls == 0);
  CHECK(textOf(owner.get("x")) == "replacement");
  REQUIRE(owner.checked());
}

TEST_CASE("runtime lookup observer reentry is rejected without a second report or sticky failure") {
  auto owner = ui::scene(ui::button("A").id("a"));

  REQUIRE(owner.prepareChecked());

  int notifications{};
  bool blocked{};

  owner.onError([&](const ManagedDiagnostic &) {
    ++notifications;

    auto nested = owner.get<Button>("a");
    auto read = nested.text().get();

    blocked = !read && cause(read.error(), TreeError::WOULD_BLOCK);
  });

  auto missing = owner.get<Button>("missing");

  CHECK(cause(textFailure(missing), TreeError::INVALID_ARGUMENT));
  CHECK(blocked);
  CHECK(notifications == 1);
  CHECK(diagnosticsCount(owner) == 1);
  REQUIRE(owner.checked());

  auto valid = owner.get<Button>("a");

  CHECK(textOf(valid) == "A");
}
