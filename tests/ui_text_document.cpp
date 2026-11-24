// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_text_document.hpp>

#include <thread>

using namespace portals;

namespace {
template <class T, class E> T checked(std::expected<T, E> value) {
  REQUIRE(value.has_value());
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

std::string describe(const ui::TextDocumentValue &value) {
  return value.text;
}

void settle(Application &app, ManagedScene &scene) {
  checked(app.scheduler().dispatch());
  checked(scene.afterDispatch(app));
  checked(scene.prepare(app, {80, 20}));
}
} // namespace

TEST_CASE("finite document source updates two views and survives receiver replacement") {
  [[maybe_unused]] auto emptyLabel = ui::label({});
  auto document = ui::finiteTextDocument("Hello world");
  auto word = document.bookmark(6);
  REQUIRE(word);
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  ui::Ref<Label> first;
  ui::Ref<Label> second;
  checked(ui::mount(
    *scene, ui::vbox(ui::label(first, document.source(), describe), ui::label(second, document.source(), describe))));
  checked(scene->prepare(*app, {80, 20}));
  checked(document.insert(0, "Hi, "));
  settle(*app, *scene);
  CHECK(checked(first.text().get()) == "Hi, Hello world");
  CHECK(checked(second.text().get()) == "Hi, Hello world");
  checked(document.restore(word));
  CHECK(checked(document.source().value().snapshot())->value.selection.cursor == 10);
  checked(scene->remove(first.node()));
  checked(document.undo());
  settle(*app, *scene);
  CHECK(checked(second.text().get()) == "Hello world");
  ui::Ref<Label> replacement;
  checked(ui::mount(*scene, ui::label(replacement, document.source(), describe)));
  CHECK(checked(replacement.text().get()) == "Hello world");
  checked(document.redo());
  settle(*app, *scene);
  CHECK(checked(replacement.text().get()) == "Hi, Hello world");
  auto unauthorized = (document.source().field(&ui::TextDocumentValue::text) = std::string("wrong")).result();
  REQUIRE_FALSE(unauthorized);
  CHECK(std::get<TreeError>(unauthorized.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(checked(second.text().get()) == "Hi, Hello world");
}

TEST_CASE("finite UI document keeps newer dirty edits after old acknowledgement and save-as keeps caret") {
  auto document = ui::finiteTextDocument("Hello world");
  auto oldSave = document.beginSave("memory:original");
  REQUIRE(oldSave);
  checked(document.insert(0, "Hi, "));
  checked(document.acknowledgeSave(oldSave));
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  checked(ui::mount(*scene, ui::label(document)));
  auto before = checked(document.source().value().snapshot());
  CHECK(before->value.dirty);
  checked(document.savepoint("memory:copy"));
  auto after = checked(document.source().value().snapshot());
  CHECK_FALSE(after->value.dirty);
  CHECK(after->value.target == "memory:copy");
  CHECK(after->value.selection == before->value.selection);
  CHECK(after->value.canUndo);
  auto stale = document.acknowledgeSave(oldSave);
  REQUIRE_FALSE(stale);
  CHECK(std::get<TreeError>(stale.error().cause) == TreeError::STALE_NODE);
  CHECK(document.lastError() == TextDocumentError::STALE);
  CHECK(checked(document.source().value().snapshot())->value.target == "memory:copy");
}

TEST_CASE("finite UI document rejects foreign-thread edits before changing core and reports closed source") {
  auto document = ui::finiteTextDocument("Hello");
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  checked(ui::mount(*scene, ui::label(document)));
  std::optional<ManagedError> failure;
  std::thread other([&] {
    auto result = document.insert(0, "wrong");
    if (!result) {
      failure = result.error();
    }
  });
  other.join();
  REQUIRE(failure);
  CHECK(std::get<TreeError>(failure->cause) == TreeError::WRONG_THREAD);
  CHECK(checked(document.source().value().snapshot())->value.text == "Hello");
  scene.reset();
  auto closed = document.insert(0, "wrong");
  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}

TEST_CASE("finite document presentation reentry preserves the accepted nested update") {
  auto document = ui::finiteTextDocument("Hello");
  bool entered{};
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  ui::Ref<Label> view;
  checked(ui::mount(*scene, ui::label(view, document.source(), [&](const ui::TextDocumentValue &value) {
    if (value.revision == 2 && !entered) {
      entered = true;
      checked(document.insert(0, "Nested "));
    }
    return value.text;
  })));
  auto outer = document.insert(0, "Outer ");
  settle(*app, *scene);
  CHECK(entered);
  CHECK(checked(document.source().value().snapshot())->value.text == "Nested Outer Hello");
  CHECK(checked(view.text().get()) == "Nested Outer Hello");
  if (!outer) {
    CHECK(std::get<TreeError>(outer.error().cause) == TreeError::WOULD_BLOCK);
  }
}

static_assert(!std::default_initializable<ui::FiniteTextDocument>);

TEST_CASE("finite document reports rejected presentation delivery and explicit refresh retries it") {
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  auto app = checked(Application::create(options));
  auto scene = checked(ManagedScene::create(*app));
  auto document = ui::finiteTextDocument("Hello");
  ui::Ref<Label> view;
  checked(ui::mount(*scene, ui::label(view, document.source(), describe)));
  auto noise = checked(scene->state(0));
  checked(scene->observe(noise, [](const auto &) {
  }));
  checked(noise.set(1));
  auto rejected = document.insert(0, "New ");
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().application);
  CHECK(std::get<SchedulerError>(rejected.error().application->cause) == SchedulerError::WOULD_BLOCK);
  CHECK(checked(document.source().value().snapshot())->value.text == "New Hello");
  CHECK(checked(view.text().get()) == "Hello");
  settle(*app, *scene);
  checked(document.refresh());
  settle(*app, *scene);
  CHECK(checked(view.text().get()) == "New Hello");
}

TEST_CASE("default finite UI document rejects oversized text through the ordinary mount boundary") {
  std::string oversized(4097, 'x');
  auto document = ui::finiteTextDocument(oversized);
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  auto mounted = ui::mount(*scene, ui::label(document));
  REQUIRE_FALSE(mounted);
  CHECK(std::get<TreeError>(mounted.error().cause) == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("finite document rejects commands after explicit scene close before changing core") {
  auto document = ui::finiteTextDocument("Hello");
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  checked(ui::mount(*scene, ui::label(document)));
  checked(document.insert(0, "New "));
  checked(document.savepoint("memory:note"));
  auto accepted = checked(document.beginSave("memory:pending"));
  auto value = checked(document.source().value().snapshot());
  checked(scene->close());
  auto expectClosed = [](const auto &result) {
    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::CLOSED);
  };
  expectClosed(document.insert(0, "Rejected "));
  expectClosed(document.undo());
  expectClosed(document.refresh());
  expectClosed(document.beginSave("memory:rejected"));
  expectClosed(document.acknowledgeSave(accepted));
  CHECK(document.lastError() == std::nullopt);
  CHECK(checked(document.bookmark(0)).position().revision == value->value.revision);
  // The retained immutable value proves the accepted presentation is unchanged.
  CHECK(value->value.text == "New Hello");
  CHECK_FALSE(value->value.dirty);
  CHECK(value->value.canUndo);
  CHECK(value->value.target == "memory:note");
}

TEST_CASE("failed finite document mount rolls back its cold source and permits a later mount") {
  auto document = ui::finiteTextDocument("Hello");
  auto app = checked(Application::create());
  auto failedScene = checked(ManagedScene::create(*app));
  auto failed =
    ui::mount(*failedScene,
              ui::label(document.source(), [](const ui::TextDocumentValue &) -> std::expected<std::string, TreeError> {
                return std::unexpected(TreeError::INVALID_ARGUMENT);
              }));
  REQUIRE_FALSE(failed);
  auto scene = checked(ManagedScene::create(*app));
  ui::Ref<Label> view;
  checked(ui::mount(*scene, ui::label(view, document.source(), describe)));
  checked(document.insert(0, "New "));
  settle(*app, *scene);
  CHECK(checked(view.text().get()) == "New Hello");
}