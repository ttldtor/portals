// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/examples/replay/terminal.hpp"
#include <doctest.h>
#include <portals/ui.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<std::ptrdiff_t> failAllocation{-1};
}

// One-shot allocation failure is armed only inside a known validation operation.
void *operator new(std::size_t size) {
  auto remaining = failAllocation.load();
  if (remaining == 0) {
    failAllocation = -1;
    throw std::bad_alloc();
  }
  if (remaining > 0) {
    --failAllocation;
  }
  if (auto memory = std::malloc(size ? size : 1)) {
    return memory;
  }
  throw std::bad_alloc();
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
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

using namespace portals;

namespace {
template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);
  return value && *value == expected;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<FindBar> find;
  ui::Ref<Button> other;
  std::vector<FindMatch> notifications;

  explicit Fixture(std::string_view document = "alpha beta alpha", std::string_view query = "alpha",
                   ui::FindBarConfig config = {}) {
    auto callback = std::make_shared<std::function<void(FindMatch)>>(std::move(config.selected));
    config.selected = [this, callback](FindMatch match) {
      notifications.push_back(match);
      if (*callback) {
        (*callback)(std::move(match));
      }
    };
    take(ui::mount(*scene, ui::vbox(ui::findBar(find, document, query, std::move(config)), ui::button("Other")
                                                                                             .onClick([] {
                                                                                             })
                                                                                             .reference(other))));
  }

  std::string summary() {
    const auto children = take(app->tree().inspect(find.node())).children;
    std::string result;
    take(app->tree().read(children.back(), [&](const Widget &widget) {
      result = dynamic_cast<const Label &>(widget).text().bytes();
    }));
    return result;
  }

  std::string query() {
    return take(find.text().get());
  }
};
} // namespace

TEST_CASE("default search finds real matches, overlaps, wrap and empty ordinary states") {
  Fixture fixture;
  auto first = take(fixture.find.match());
  REQUIRE(first);
  CHECK(first.text() == "alpha");
  CHECK(first.begin() == 0);
  CHECK(first.end() == 5);
  CHECK(first.ordinal() == 1);
  CHECK(first.storedCount() == 2);
  CHECK_FALSE(first.truncated());
  CHECK(fixture.notifications.empty());
  CHECK(fixture.find.next());
  CHECK(take(fixture.find.match()).begin() == 11);
  CHECK(take(fixture.find.current(first)));
  CHECK(fixture.find.next());
  CHECK(take(fixture.find.match()).ordinal() == 1);
  CHECK(fixture.find.previous());
  CHECK(take(fixture.find.match()).ordinal() == 2);
  take(fixture.find.replaceDocument("aaaa"));
  take((fixture.find.text() = "aa").result());
  CHECK(take(fixture.find.match()).storedCount() == 3);
  CHECK(take(fixture.find.navigate(FindDirection::NEXT)).begin() == 1);
  take((fixture.find.text() = "missing").result());
  CHECK_FALSE(take(fixture.find.match()));
  CHECK_FALSE(fixture.find.next());
  CHECK(fixture.summary().find("No matches") != std::string::npos);
  take((fixture.find.text() = "").result());
  CHECK_FALSE(take(fixture.find.match()));
}

TEST_CASE("literal matches preserve combining, wide, emoji and CRLF boundaries without normalization") {
  const std::string document = "e\u0301 e 界 👩‍💻 👩 🇺🇸\r\n";
  Fixture fixture(document, "e");
  auto match = take(fixture.find.match());
  CHECK(match.storedCount() == 1);
  CHECK(match.begin() == document.find("e "));
  take((fixture.find.text() = "\u0301").result());
  CHECK_FALSE(take(fixture.find.match()));
  take((fixture.find.text() = "é").result());
  CHECK_FALSE(take(fixture.find.match()));
  take((fixture.find.text() = "e\u0301").result());
  CHECK(take(fixture.find.match()).begin() == 0);
  take((fixture.find.text() = "👩").result());
  CHECK(take(fixture.find.match()).storedCount() == 1);
  CHECK(take(fixture.find.match()).begin() == document.find("👩 🇺🇸"));
  take((fixture.find.text() = "界").result());
  CHECK(take(fixture.find.match()).text() == "界");
  CHECK(take(GraphemeText::from(take(fixture.find.match()).context())).bytes() == document);
}

TEST_CASE("stored result caps are visible and wrap only their prefix") {
  Fixture fixture("aaaaa", "a", {.limits = {.maxMatches = 2}});
  auto match = take(fixture.find.match());
  CHECK(match.storedCount() == 2);
  CHECK(match.truncated());
  CHECK(fixture.summary().find("2+") != std::string::npos);
  CHECK(fixture.find.previous());
  CHECK(take(fixture.find.match()).begin() == 1);
  CHECK(fixture.find.next());
  CHECK(take(fixture.find.match()).begin() == 0);
}

TEST_CASE("source replacement changes identity even for equal bytes and rejects bad data coherently") {
  Fixture fixture;
  auto old = take(fixture.find.match());
  take(fixture.find.replaceDocument("alpha beta alpha"));
  CHECK_FALSE(take(fixture.find.current(old)));
  CHECK(old.text() == "alpha");
  CHECK(old.context() == "alpha beta alpha");
  auto current = take(fixture.find.match());
  const auto before = fixture.summary();
  const auto failure = fixture.find.replaceDocument(std::string(1, static_cast<char>(0xff)));
  REQUIRE_FALSE(failure);
  REQUIRE(failure.error().text);
  CHECK(failure.error().text->kind == TextErrorKind::INVALID_UTF8);
  CHECK(take(fixture.find.current(current)));
  CHECK(fixture.summary() == before);
  CHECK(fixture.query() == "alpha");
  auto oversized = fixture.find.replaceDocument(std::string(65537, 'a'));
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().text);
  CHECK(oversized.error().text->kind == TextErrorKind::INPUT_LIMIT);
  CHECK(take(fixture.find.current(current)));
}

TEST_CASE("query publication keeps its existing edit mode and explicit Clear is one undoable operation") {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture fixture("alpha beta alpha", "alpha", {.editor = {.options = {.editMode = mode}}});
    auto before = take(fixture.find.match());
    take((fixture.find.text() = "beta").result());
    if (mode == EditMode::IMMEDIATE) {
      CHECK(take(fixture.find.match()).text() == "beta");
    } else {
      CHECK(take(fixture.find.current(before)));
      CHECK(fixture.summary().find("Previous search") != std::string::npos);
      take(fixture.app->router().invoke(fixture.find.editor().node(), {portals::Action::ACTIVATE}));
      CHECK(take(fixture.find.match()).text() == "beta");
    }
    take(fixture.find.clearQuery().result());
    CHECK(fixture.query().empty());
    CHECK_FALSE(take(fixture.find.match()));
    take(fixture.find.editor().undo().result());
    CHECK(fixture.query() == "beta");
    if (mode != EditMode::IMMEDIATE) {
      CHECK_FALSE(take(fixture.find.match()));
      take(fixture.app->router().invoke(fixture.find.editor().node(), {portals::Action::ACTIVATE}));
    }
    CHECK(take(fixture.find.match()).text() == "beta");
  }
}

TEST_CASE("search allocation failure retains the original typed receipt and one collector entry") {
  std::vector<LineEditResult> outcomes;
  ui::FindBarConfig config;
  config.editor.validate = [](std::string_view query) {
    if (query == "candidate") {
      failAllocation = 0;
    }
    return true;
  };
  config.editor.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
    outcomes.push_back(std::move(receipt));
    return {};
  };
  Fixture fixture("alpha beta alpha", "alpha", std::move(config));
  auto before = take(fixture.find.match());
  const auto summary = fixture.summary();
  take((fixture.find.text() = "candidate").result());
  CHECK(fixture.query() == "candidate");
  CHECK(take(fixture.find.current(before)));
  CHECK(fixture.summary() == summary);
  REQUIRE(outcomes.size() == 1);
  CHECK(outcomes[0].status == LineEditStatus::FAILED);
  CHECK(outcomes[0].error == TreeError::NO_MEMORY);
  const auto errors = take(fixture.scene->diagnostics());
  REQUIRE(errors.errors.size() == 1);
  CHECK(cause(errors.errors[0].error, TreeError::NO_MEMORY));
}

TEST_CASE("Clear allocation failure preserves last-good results and exactly one undo entry and error") {
  std::vector<LineEditResult> outcomes;
  ui::FindBarConfig config;
  config.editor.validate = [](std::string_view query) {
    if (query.empty()) {
      failAllocation = 0;
    }
    return true;
  };
  config.editor.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
    outcomes.push_back(std::move(receipt));
    return {};
  };
  Fixture fixture("alpha beta alpha", "alpha", std::move(config));
  const auto before = take(fixture.find.match());
  const auto summary = fixture.summary();
  auto cleared = fixture.find.clearQuery();
  REQUIRE_FALSE(cleared);
  CHECK(cleared.result().error() == TreeError::NO_MEMORY);
  CHECK(fixture.query().empty());
  CHECK(take(fixture.find.current(before)));
  CHECK(fixture.summary() == summary);
  REQUIRE(outcomes.size() == 1);
  CHECK(outcomes[0].status == LineEditStatus::FAILED);
  CHECK(outcomes[0].error == TreeError::NO_MEMORY);
  const auto errors = take(fixture.scene->diagnostics());
  REQUIRE(errors.errors.size() == 1);
  CHECK(cause(errors.errors[0].error, TreeError::NO_MEMORY));
  take(fixture.find.editor().undo().result());
  CHECK(fixture.query() == "alpha");
  CHECK_FALSE(fixture.find.editor().canUndo());
  CHECK(fixture.find.editor().canRedo());
  CHECK(fixture.notifications.empty());
}

TEST_CASE("navigation callback may remove the field while retaining its own callable and match") {
  Fixture fixture;
  FindMatch retained;
  bool completed{};
  // Source callback uses the same guarded publication path as navigation.
  ui::FindBarConfig config;
  config.selected = [&](FindMatch match) {
    retained = match;
    take(fixture.scene->remove(fixture.find.node()));
    completed = match.text() == "alpha";
  };
  auto separate = take(ManagedScene::create(*fixture.app));
  ui::Ref<FindBar> target;
  config.selected = [&](FindMatch match) {
    retained = match;
    take(separate->remove(target.node()));
    completed = match.text() == "alpha";
  };
  take(ui::mount(*separate, ui::findBar(target, "alpha alpha", "alpha", std::move(config))));
  auto moved = target.navigate(FindDirection::NEXT);
  REQUIRE(moved);
  CHECK(completed);
  CHECK(retained.text() == "alpha");
  CHECK_FALSE(target.match());
}

TEST_CASE("source callback may retire its scene and reentry is a checked WOULD_BLOCK") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FindBar> target;
  FindMatch retained;
  bool blocked{};
  bool retire{};
  ui::FindBarConfig config;
  config.selected = [&](FindMatch match) {
    retained = match;
    auto nested = target.replaceDocument("nested");
    blocked = !nested && cause(nested.error(), TreeError::WOULD_BLOCK);
    if (retire) {
      scene.reset();
    }
  };
  take(ui::mount(*scene, ui::findBar(target, "alpha", "alpha", std::move(config))));
  take(target.replaceDocument("alpha alpha"));
  CHECK(blocked);
  retire = true;
  take(target.replaceDocument("alpha"));
  CHECK_FALSE(scene);
  CHECK(retained.text() == "alpha");
  CHECK_FALSE(target.match());
}

TEST_CASE("readonly and disabled or hidden state preserve programmatic source updates") {
  Fixture fixture("alpha alpha", "alpha", {.editor = {.options = {.readOnly = true}}});
  take(fixture.find.focus());
  CHECK(take(fixture.app->router().focused()) == fixture.find.editor().node());
  auto clear = fixture.find.clearQuery();
  REQUIRE_FALSE(clear);
  CHECK(clear.result().error() == TreeError::WOULD_BLOCK);
  CHECK(fixture.find.next());
  take((fixture.find.text() = "beta").result());
  CHECK(fixture.query() == "beta");
  take(fixture.app->tree().setEnabled(fixture.find.node(), false));
  take(fixture.find.replaceDocument("beta beta"));
  CHECK(take(fixture.find.match()).storedCount() == 2);
  auto disabled = fixture.find.navigate(FindDirection::NEXT);
  REQUIRE_FALSE(disabled);
  CHECK(cause(disabled.error(), TreeError::WOULD_BLOCK));
  take(fixture.app->tree().setEnabled(fixture.find.node(), true));
  take(fixture.app->tree().setVisible(fixture.find.node(), false));
  take(fixture.find.replaceDocument("beta"));
  CHECK(take(fixture.find.match()).storedCount() == 1);
  CHECK_FALSE(fixture.find.navigate(FindDirection::NEXT));
}

TEST_CASE("pre-mount query proxies resolve lazily and single-match navigation stays idle") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FindBar> find;
  ui::Ref<Label> caption;
  auto draft = find.text();
  take(ui::mount(*scene, ui::vbox(ui::findBar(find, "alpha", "alpha"), ui::label(caption, draft))));
  take(scene->prepare(*app, {40, 8}));
  CHECK(take(caption.text().get()) == "alpha");
  take(app->tree().consumeFrameRequest());
  CHECK(find.next());
  CHECK(find.previous());
  take((find.text() = "alpha").result());
  CHECK(take(app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take((draft = "missing").result());
  take(scene->prepare(*app, {40, 8}));
  CHECK(take(caption.text().get()) == "missing");
}

TEST_CASE("frames foreign threads and retired owners reject before search or callbacks") {
  Fixture fixture;
  take(fixture.app->tree().frame([&] {
    auto changed = fixture.find.replaceDocument("alpha alpha");
    REQUIRE_FALSE(changed);
    CHECK(cause(changed.error(), TreeError::WOULD_BLOCK));
  }));
  bool rejected{};
  std::thread other([&] {
    auto changed = fixture.find.replaceDocument("foreign");
    rejected = !changed && cause(changed.error(), TreeError::WRONG_THREAD);
  });
  other.join();
  CHECK(rejected);
  CHECK(fixture.notifications.empty());
  fixture.scene.reset();
  auto retired = fixture.find.replaceDocument("retired");
  REQUIRE_FALSE(retired);
  CHECK(cause(retired.error(), TreeError::CLOSED));
}

TEST_CASE("small zero hidden and theme preparation never invokes query validation") {
  int calls{};
  ui::FindBarConfig config;
  config.editor.validate = [&](std::string_view) {
    ++calls;
    return true;
  };
  Fixture fixture("界 alpha 👩‍💻", "alpha", std::move(config));
  const auto match = take(fixture.find.match());
  for (const auto extent : {CellSize{2, 2}, CellSize{0, 0}, CellSize{40, 8}}) {
    take(fixture.scene->prepare(*fixture.app, extent));
    const auto bounds = take(CellRect::from({}, extent));
    take(fixture.app->tree().frame([&] {
      take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
    }));
    take(fixture.scene->theme({}));
  }
  take(fixture.app->tree().setVisible(fixture.find.node(), false));
  take(fixture.scene->prepare(*fixture.app, {40, 8}));
  CHECK(calls == 0);
  CHECK(take(fixture.find.current(match)));
}

TEST_CASE("retained default FindBar does not schedule perpetual idle frames") {
  example::ReplayTerminal terminal({{20, "\x1b[21~", {}}}, 80, {40, 8});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  auto owner = ui::scene(ui::findBar("alpha alpha", "alpha"), options);
  owner.onKey(key::F10, [&] {
    (void)owner.quit();
  });
  const auto result = owner.run(terminal);
  REQUIRE(result);
  CHECK(result->frames == 1);
  CHECK(terminal.closed);
}

TEST_CASE("fully prepared query remains unpublished when the editor model allocation fails") {
  bool observed{};
  const std::string candidate(180, 'b');
  for (std::ptrdiff_t budget = 0; budget < 64 && !observed; ++budget) {
    CAPTURE(budget);
    std::fprintf(stderr, "staged publication probe budget=%td\n", budget);
    std::vector<LineEditResult> outcomes;
    outcomes.reserve(1);
    ui::FindBarConfig config;
    config.editor.validate = [budget](std::string_view) {
      failAllocation = budget;
      return true;
    };
    config.editor.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
      failAllocation = -1;
      outcomes.push_back(std::move(result));
      return {};
    };
    Fixture fixture(std::string(500, 'b') + "alpha", "alpha", std::move(config));
    const auto before = take(fixture.find.match());
    const auto caption = fixture.summary();
    const auto prepared = take(ui::detail::Access::findPreparedQueries(*fixture.scene, fixture.find.node()));
    auto changed = fixture.find.text() = candidate;
    failAllocation = -1;
    take(changed.result());
    REQUIRE(outcomes.size() == 1);
    const bool staged = take(ui::detail::Access::findPreparedQueries(*fixture.scene, fixture.find.node())) > prepared;
    if (staged && outcomes.front().status == LineEditStatus::FAILED) {
      observed = true;
      CHECK(outcomes.front().error == TreeError::NO_MEMORY);
      CHECK(take(fixture.find.current(before)));
      CHECK(fixture.summary() == caption);
      CHECK(fixture.query() == candidate);
      CHECK(fixture.notifications.empty());
      const auto diagnostics = take(fixture.scene->diagnostics());
      REQUIRE(diagnostics.errors.size() == 1);
      CHECK(cause(diagnostics.errors.front().error, TreeError::NO_MEMORY));
    }
  }
  CHECK(observed);
}

TEST_CASE("repetitive search remains exact with large configured queries and bounded previews") {
  const std::string document(12000, 'a');
  const std::string query = std::string(2047, 'a') + 'b';
  ui::FindBarConfig config;
  config.editor.options.limits = {4096, 4096, 4096, 8192};
  Fixture fixture(document, query, config);
  CHECK_FALSE(take(fixture.find.match()));
  take((fixture.find.text() = std::string(2048, 'a')).result());
  auto found = take(fixture.find.match());
  REQUIRE(found);
  CHECK(found.storedCount() == 4096);
  CHECK(found.truncated());
  CHECK(found.context().empty());
  CHECK(found.context(2048).size() == 2048);
  CHECK(fixture.summary().find("preview budget") != std::string::npos);
}

TEST_CASE("configuration ceilings are distinct from typed input admission errors") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  for (const auto limits :
       {FindLimits{.maxMatches = 0}, FindLimits{.maxDocumentBytes = FindLimits::MAX_DOCUMENT_BYTES + 1},
        FindLimits{.maxMatches = FindLimits::MAX_MATCHES + 1}}) {
    auto mounted = ui::mount(*scene, ui::findBar("", "", {.limits = limits}));
    REQUIRE_FALSE(mounted);
    CHECK(cause(mounted.error(), TreeError::INVALID_ARGUMENT));
    CHECK_FALSE(mounted.error().text);
  }
  auto tooLarge = ui::mount(*scene, ui::findBar("x", "", {.limits = {.maxDocumentBytes = 0}}));
  REQUIRE_FALSE(tooLarge);
  REQUIRE(tooLarge.error().text);
  CHECK(tooLarge.error().text->kind == TextErrorKind::INPUT_LIMIT);
  take(ui::mount(*scene, ui::findBar("", "", {.limits = {.maxDocumentBytes = 0}})));
}

TEST_CASE("built-in loaded FindBar forwards source limits labels query and existing editor settings") {
  auto owner = ui::scene(ui::resource(R"(
    defaults { FindBar { maxDocumentBytes=64 maxMatches=2 editMode=EXPLICIT
      previousLabel="Назад" nextLabel="Вперёд" clearLabel="Сброс" } }
    scene { FindBar#find { document="alpha alpha alpha" text="alpha" readOnly=true } }
  )",
                                      {.format = ui::SceneResourceFormat::CONFIGURATION}));
  auto find = owner.get<FindBar>("find");
  owner.configure([&](ManagedScene &, Application &app) {
    const auto outer = take(app.tree().inspect(find.node())).children;
    const auto toolbar = take(app.tree().inspect(outer.front())).children;
    const std::array<std::string_view, 3> captions{"Назад", "Вперёд", "Сброс"};
    for (std::size_t index = 0; index < captions.size(); ++index) {
      take(app.tree().read(toolbar[index + 1], [&](const Widget &widget) {
        CHECK(dynamic_cast<const Button &>(widget).text().bytes() == captions[index]);
      }));
    }
  });
  take(owner.prepareChecked());
  CHECK(take(find.match()).storedCount() == 2);
  CHECK(take(find.match()).truncated());
  CHECK(take(find.text().get()) == "alpha");
  CHECK(find.next());
  auto cleared = find.clearQuery();
  CHECK_FALSE(cleared);
  CHECK(cleared.result().error() == TreeError::WOULD_BLOCK);
  take((find.text() = "other").result());
  CHECK(take(find.match()).text() == "alpha");
  const auto oversize = find.replaceDocument(std::string(65, 'a'));
  REQUIRE_FALSE(oversize);
  REQUIRE(oversize.error().text);
  CHECK(oversize.error().text->kind == TextErrorKind::INPUT_LIMIT);
  take(owner.close());

  const std::string query(2048, 'a');
  const std::string source =
    "scene { FindBar#large { document=\"" + query + "\" text=\"" + query +
    "\" limits { maxInputBytes=4096 maxGraphemes=4096 maxClusterBytes=4096 maxCells=8192 } } }";
  auto large = ui::scene(ui::resource(source, {.format = ui::SceneResourceFormat::CONFIGURATION}));
  auto target = large.get<FindBar>("large");
  take(large.prepareChecked());
  CHECK(take(target.match()).text() == query);
  take(large.close());
}

TEST_CASE("late partial mount failures roll back the complete FindBar and publish no refs") {
  for (bool nested : {false, true}) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    ui::Ref<Label> preserved;
    ui::Ref<FindBar> field;
    take(ui::mount(*scene, ui::label(preserved, "Preserved")));
    const auto before = take(app->tree().inspect(scene->root())).children;
    ui::FindBarConfig config;
    config.nextLabel = TextValue(std::string(1, static_cast<char>(0xff)));
    auto recipe = ui::findBar(field, "alpha", "alpha", std::move(config));
    auto mounted = nested ? ui::mount(*scene, ui::vbox(std::move(recipe))) : ui::mount(*scene, std::move(recipe));
    REQUIRE_FALSE(mounted);
    CHECK(mounted.error().text.has_value());
    CHECK(take(app->tree().inspect(scene->root())).children == before);
    CHECK(field.node() == Node<FindBar>{});
    CHECK(take(preserved.text().get()) == "Preserved");
    CHECK(take(app->router().traverse()) == Node<Widget>{});
  }
}

TEST_CASE("query publication pins the selected callback when it removes the executing field") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FindBar> find;
  auto pin = std::make_shared<int>(7);
  std::weak_ptr<int> lifetime = pin;
  FindMatch retained;
  ui::FindBarConfig config;
  config.selected = [&, pin](FindMatch match) {
    retained = match;
    take(scene->remove(find.node()));
    CHECK_FALSE(lifetime.expired());
    CHECK(*pin == 7);
    CHECK(match.text() == "beta");
  };
  take(ui::mount(*scene, ui::findBar(find, "alpha beta", "alpha", std::move(config))));
  // The owning aggregate intentionally has fallible copy transfers; release this caller's callback copy.
  config.selected = {};
  pin.reset();
  take((find.text() = "beta").result());
  take(scene->prepare(*app, {40, 8}));
  CHECK(lifetime.expired());
  CHECK(retained.text() == "beta");
  CHECK_FALSE(find.match());
}

TEST_CASE("callback failure keeps the published selection and semantic rejection retains prior results") {
  ui::FindBarConfig config;
  config.selected = [](FindMatch) {
    throw std::runtime_error("selection failed");
  };
  Fixture fixture("alpha alpha", "alpha", config);
  auto moved = fixture.find.navigate(FindDirection::NEXT);
  REQUIRE_FALSE(moved);
  CHECK(cause(moved.error(), TreeError::CALLBACK_FAILED));
  CHECK(take(fixture.find.match()).ordinal() == 2);
  auto replaced = fixture.find.replaceDocument("alpha");
  REQUIRE_FALSE(replaced);
  CHECK(cause(replaced.error(), TreeError::CALLBACK_FAILED));
  CHECK(take(fixture.find.match()).storedCount() == 1);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());

  ui::FindBarConfig rejected;
  rejected.editor.validate = [](std::string_view value) {
    return value != "blocked";
  };
  Fixture draft("alpha beta", "alpha", rejected);
  const auto accepted = take(draft.find.match());
  take((draft.find.text() = "blocked").result());
  CHECK(draft.query() == "blocked");
  CHECK(take(draft.find.current(accepted)));
  CHECK(draft.notifications.empty());
  take((draft.find.text() = "beta").result());
  CHECK(take(draft.find.match()).text() == "beta");
}

TEST_CASE("accepted editor query and search remain coherent through the first post-commit allocation budget") {
  bool committed{};
  const std::string candidate(180, 'b');
  for (std::ptrdiff_t budget = 0; budget < 96 && !committed; ++budget) {
    CAPTURE(budget);
    std::fprintf(stderr, "post-commit publication probe budget=%td\n", budget);
    std::optional<LineEditStatus> status;
    ui::FindBarConfig config;
    config.editor.validate = [budget](std::string_view) {
      failAllocation = budget;
      return true;
    };
    config.editor.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
      failAllocation = -1;
      status = receipt.status;
      return {};
    };
    Fixture fixture(std::string(500, 'b') + "alpha", "alpha", std::move(config));
    auto changed = fixture.find.text() = candidate;
    failAllocation = -1;
    take(changed.result());
    REQUIRE(status);
    if (*status != LineEditStatus::COMMITTED) {
      continue;
    }
    committed = true;
    CHECK(fixture.query() == candidate);
    CHECK(take(fixture.find.match()).text() == candidate);
    CHECK(take(fixture.scene->diagnostics()).errors.empty());
    take(fixture.find.replaceDocument(candidate));
    CHECK(take(fixture.find.match()).text() == candidate);
    CHECK(take(fixture.find.match()).storedCount() == 1);
  }
  CHECK(committed);
}

TEST_CASE("accepted query retains its new results when the private summary was removed") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<FindBar> find;
  std::optional<LineEditStatus> status;
  int selected{};
  ui::FindBarConfig config;
  config.editor.validate = [&](std::string_view) {
    const auto children = take(app->tree().inspect(find.node())).children;
    take(app->tree().remove(children.back()));
    return true;
  };
  config.editor.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
    status = receipt.status;
    return {};
  };
  config.selected = [&](FindMatch) {
    ++selected;
  };
  take(ui::mount(*scene, ui::findBar(find, "alpha beta", "alpha", config)));
  const auto old = take(find.match());
  take((find.text() = "beta").result());
  CHECK(status == LineEditStatus::COMMITTED);
  CHECK(take(find.text().get()) == "beta");
  CHECK(take(find.match()).text() == "beta");
  CHECK_FALSE(take(find.current(old)));
  CHECK(selected == 0);
  auto diagnostics = take(scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(cause(diagnostics.errors.front().error, TreeError::STALE_NODE));
  // A new source cannot be published without its summary; it retains the accepted-query search.
  auto replaced = find.replaceDocument("beta beta");
  REQUIRE_FALSE(replaced);
  CHECK(cause(replaced.error(), TreeError::STALE_NODE));
  CHECK(take(find.match()).text() == "beta");
  CHECK(take(find.match()).storedCount() == 1);
}

TEST_CASE("loaded FindBar source and button captions use the existing resource resolver") {
  std::vector<std::string> resolved;
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    resolved.emplace_back(key);
    if (key == "source") {
      return "alpha alpha";
    }
    if (key == "previous") {
      return "Назад";
    }
    if (key == "next") {
      return "Далее";
    }
    if (key == "clear") {
      return "Сброс";
    }
    return "alpha";
  };
  auto owner = ui::scene(ui::resource(R"(
    scene { FindBar#find { document=i18n("source") text=i18n("query")
      previousLabel=i18n("previous") nextLabel=i18n("next") clearLabel=i18n("clear") } }
  )",
                                      options));
  auto find = owner.get<FindBar>("find");
  owner.configure([&](ManagedScene &, Application &app) {
    const auto outer = take(app.tree().inspect(find.node())).children;
    const auto children = take(app.tree().inspect(outer.front())).children;
    const std::array<std::string_view, 3> captions{"Назад", "Далее", "Сброс"};
    for (std::size_t i = 0; i < captions.size(); ++i) {
      take(app.tree().read(children[i + 1], [&](const Widget &widget) {
        CHECK(dynamic_cast<const Button &>(widget).text().bytes() == captions[i]);
      }));
    }
  });
  take(owner.prepareChecked());
  CHECK(take(find.match()).storedCount() == 2);
  CHECK(take(find.text().get()) == "alpha");
  CHECK(resolved.size() == 5);
  for (auto key : {"source", "query", "previous", "next", "clear"}) {
    CHECK(std::ranges::count(resolved, key) == 1);
  }
  take(owner.close());
}
