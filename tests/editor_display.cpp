// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui.hpp>

#include <array>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

namespace allocationTest {
thread_local std::ptrdiff_t remaining{-1};

// GCC otherwise inlines the malloc-backed replacement delete and diagnoses a
// false allocation/deallocation mismatch at ordinary new expressions.
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void release(void *value) noexcept {
  std::free(value);
}
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }

  if (auto *value = std::malloc(size == 0 ? 1 : size)) {
    return value;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *value) noexcept {
  allocationTest::release(value);
}

void operator delete(void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

constexpr std::string_view UNICODE = "e\xcc\x81\xe7\x95\x8c\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";

struct ClipboardProbe : Clipboard {
  int reads{};
  int writes{};
  std::string bytes{"paste"};

  std::expected<std::string, TreeError> read(std::size_t) override {
    ++reads;
    return bytes;
  }

  std::expected<void, TreeError> write(std::string_view value) override {
    ++writes;
    bytes = value;
    return {};
  }
};

struct PaintProbe : PaintContext {
  std::string bytes;
  std::vector<CellRect> fills;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> input) override {
    for (const auto &command : input) {
      if (const auto *text = std::get_if<TextCommand>(&command)) {
        bytes.append(text->text.bytes());
      }
      if (const auto *fill = std::get_if<FillCommand>(&command)) {
        fills.push_back(fill->area);
      }
    }
    return {};
  }
};

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<LineEdit> edit;
  ui::Ref<Button> clearButton;
  ManagedState<std::string> model = take(scene->state(std::string(UNICODE)));
  std::vector<LineEditResult> outcomes = std::vector<LineEditResult>(0);
  std::function<void()> afterResult;

  explicit Fixture(ui::LineEditConfig config = {}, ApplicationOptions options = {})
      : app(take(Application::create(std::move(options)))) {
    outcomes.reserve(128);
    config.result = [&](LineEditResult outcome) -> std::expected<void, TreeError> {
      outcomes.push_back(outcome);
      if (afterResult) {
        afterResult();
      }
      return {};
    };
    take(ui::mount(*scene, ui::column(ui::lineEdit(edit, model, std::move(config)), ui::button("Clear", [field = edit] {
                                                                                      return field.clear();
                                                                                    }).reference(clearButton))));
    take(app->router().setFocus(edit.node()));
  }

  std::string draft() const {
    return take(edit.text().get());
  }

  std::string value() const {
    return take(model.snapshot())->value;
  }

  void key(char32_t scalar) {
    take(app->router().dispatch(KeyEvent{InputKey::CHARACTER, scalar, {.control = true}}));
  }

  std::string inspectPaint(CellSize size = {3, 1}) {
    PaintProbe probe;
    take(app->tree().frame([&] {
      const auto viewport = take(CellRect::from({}, {size.width, std::max(size.height, 3)}));
      take(layoutTree(app->tree(), take(app->tree().root()), viewport, viewport));
      take(app->tree().paint(edit.node(), probe));
    }));
    return probe.bytes;
  }
};
} // namespace

TEST_CASE("password paint and geometry use one ASCII star per Unicode grapheme") {
  Fixture fixture({.options = {.displayMode = LineEditDisplayMode::PASSWORD}});
  CHECK(fixture.draft() == UNICODE);
  CHECK(fixture.value() == UNICODE);
  take(fixture.app->tree().edit(fixture.edit.node(), [&](LineEdit &widget) {
    CHECK(take(widget.measure({100, 1})).width == 4);
  }));
  const auto paint = fixture.inspectPaint({2, 1});
  CHECK_FALSE(paint.empty());
  CHECK(paint.find_first_not_of('*') == std::string::npos);
  fixture.key(U'a');
  CHECK(fixture.inspectPaint().find_first_not_of('*') == std::string::npos);
  take(fixture.edit.clear().result());
  CHECK(fixture.inspectPaint().empty());
  take(fixture.edit.undo().result());
  CHECK(fixture.inspectPaint().find_first_not_of('*') == std::string::npos);
}

TEST_CASE("no echo has constant intrinsic width and submits no secret text or selection") {
  Fixture fixture({.options = {.displayMode = LineEditDisplayMode::NO_ECHO}});
  for (const auto &value : {std::string("x"), std::string(UNICODE), std::string(512, 'q')}) {
    take(fixture.edit.text().set(value).result());
    fixture.key(U'a');
    CHECK(fixture.inspectPaint().empty());
    take(fixture.app->tree().edit(fixture.edit.node(), [&](LineEdit &widget) {
      CHECK(take(widget.measure({100, 1})) == CellSize{1, 1});
    }));
  }
}

TEST_CASE("no echo caret remains at the origin after navigation and selection") {
  Fixture fixture({.options = {.displayMode = LineEditDisplayMode::NO_ECHO}});
  for (const auto key : {InputKey::HOME, InputKey::END, InputKey::LEFT}) {
    take(fixture.app->router().dispatch(KeyEvent{key}));
    PaintProbe probe;
    take(fixture.app->tree().frame([&] {
      const auto viewport = take(CellRect::from({}, {3, 3}));
      take(layoutTree(fixture.app->tree(), take(fixture.app->tree().root()), viewport, viewport));
      take(fixture.app->tree().paint(fixture.edit.node(), probe));
    }));
    CHECK(probe.bytes.empty());
    REQUIRE(probe.fills.size() == 2);
    CHECK(probe.fills.back() == take(CellRect::from({}, {1, 1})));
  }
}

TEST_CASE("protected copy and cut are consumed before any service result or invalidation") {
  for (auto mode : {LineEditDisplayMode::PASSWORD, LineEditDisplayMode::NO_ECHO}) {
    auto clipboard = std::make_shared<ClipboardProbe>();
    Fixture fixture({.options = {.displayMode = mode}, .clipboard = clipboard});
    fixture.key(U'a');
    const auto before = fixture.outcomes.size();
    take(fixture.app->tree().consumeFrameRequest());
    fixture.key(U'c');
    fixture.key(U'x');
    CHECK(clipboard->writes == 0);
    CHECK(clipboard->reads == 0);
    CHECK(fixture.outcomes.size() == before);
    CHECK(fixture.draft() == UNICODE);
    CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
    fixture.key(U'v');
    CHECK(clipboard->reads == 1);
    CHECK(fixture.value() == "paste");
  }
}

TEST_CASE("normal clipboard and programmatic text in read only remain compatible") {
  auto clipboard = std::make_shared<ClipboardProbe>();
  Fixture normal({.clipboard = clipboard});
  normal.key(U'a');
  normal.key(U'c');
  CHECK(clipboard->bytes == UNICODE);
  CHECK(clipboard->writes == 1);
  normal.key(U'x');
  CHECK(normal.draft().empty());
  Fixture readOnly(
    {.options = {.readOnly = true, .displayMode = LineEditDisplayMode::PASSWORD}, .clipboard = clipboard});
  const auto reads = clipboard->reads;
  readOnly.key(U'v');
  CHECK(clipboard->reads == reads);
  take(readOnly.edit.text().set("loaded").result());
  CHECK(readOnly.value() == "loaded");
  CHECK(readOnly.edit.clear().result().error() == TreeError::WOULD_BLOCK);
}

TEST_CASE("placeholder is presentation only and checked before internal state creation") {
  Fixture fixture({.options = {.displayMode = LineEditDisplayMode::PASSWORD, .placeholder = std::string(UNICODE)}});
  take(fixture.edit.clear().result());
  const auto revision = take(fixture.model.snapshot())->revision;
  const auto count = fixture.outcomes.size();
  CHECK(fixture.inspectPaint().find(UNICODE) != std::string::npos);
  CHECK(fixture.inspectPaint({0, 0}).empty());
  CHECK(take(fixture.model.snapshot())->revision == revision);
  CHECK(fixture.outcomes.size() == count);
  for (const auto &placeholder : {std::string("\xff"), std::string("bad\nline"), std::string(5000, 'x')}) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app, {.maxStates = 1}));
    ui::Ref<LineEdit> ref;
    auto mounted = ui::mount(*scene, ui::lineEdit(ref, "", {.options = {.placeholder = placeholder}}));
    REQUIRE_FALSE(mounted);
    CHECK(ref.node().id() == NodeId{});
    CHECK(scene->state(7).has_value());
  }
}

TEST_CASE("clear is one independent history operation in every edit mode") {
  for (auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture fixture({.options = {.editMode = mode,
                                 .programmaticChange = ProgrammaticChange::RESET_BASELINE,
                                 .displayMode = LineEditDisplayMode::PASSWORD}});
    fixture.key(U'a');
    const auto before = fixture.outcomes.size();
    take(fixture.app->router().invoke(fixture.clearButton.node(), {Action::ACTIVATE}));
    CHECK(fixture.draft().empty());
    CHECK(fixture.value() == (mode == EditMode::IMMEDIATE ? "" : UNICODE));
    CHECK(fixture.outcomes.size() == before + 1);
    take(fixture.app->tree().read(fixture.edit.node(), [&](const LineEdit &edit) {
      CHECK(edit.cursor() == 0);
      CHECK(edit.anchor() == 0);
    }));
    take(fixture.edit.undo().result());
    CHECK(fixture.draft() == UNICODE);
    take(fixture.app->tree().read(fixture.edit.node(), [&](const LineEdit &edit) {
      CHECK(edit.cursor() == UNICODE.size());
      CHECK(edit.anchor() == 0);
    }));
    take(fixture.edit.redo().result());
    CHECK(fixture.draft().empty());
    const auto emptyCount = fixture.outcomes.size();
    take(fixture.app->tree().consumeFrameRequest());
    take(fixture.edit.clear().result());
    CHECK(fixture.outcomes.size() == emptyCount);
    CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  }
}

TEST_CASE("clear keeps semantic rejection distinct and never adopts an external model") {
  Fixture rejected({.validate = [](std::string_view value) {
    return !value.empty();
  }});
  take(rejected.edit.clear().result());
  CHECK(rejected.outcomes.back().status == LineEditStatus::VALIDATION_FAILED);
  CHECK(rejected.value() == UNICODE);
  CHECK(rejected.draft().empty());
  take(rejected.edit.undo().result());
  CHECK(rejected.draft() == UNICODE);

  Fixture conflict;
  take(conflict.model.set("external"));
  take(conflict.edit.clear().result());
  CHECK(conflict.outcomes.back().status == LineEditStatus::CONFLICT);
  CHECK(conflict.value() == "external");
  CHECK(conflict.draft().empty());
}

TEST_CASE("disabled and empty read only clear reject without changing eligibility semantics") {
  Fixture fixture;
  take(fixture.edit.clear().result());
  take(fixture.app->tree().setEnabled(fixture.scene->root(), false));
  CHECK(fixture.edit.clear().result().error() == TreeError::WOULD_BLOCK);
  ui::Ref<LineEdit> empty;
  CHECK(empty.clear().result().error() == TreeError::CLOSED);
  CHECK_FALSE(fixture.edit.canUndo());
}

TEST_CASE("a complete clear over the history budget reports technical failure and preserves the draft") {
  Fixture fixture({.options = {.history = {.maxBytes = 1}}});
  const auto command = fixture.edit.clear();
  REQUIRE_FALSE(command);
  CHECK(command.result().error() == TreeError::WOULD_BLOCK);
  CHECK(fixture.draft() == UNICODE);
  REQUIRE(fixture.outcomes.size() == 1);
  CHECK(fixture.outcomes.back().status == LineEditStatus::FAILED);
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
}

TEST_CASE("unknown display modes fail checked mount before internal model publication") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app, {.maxStates = 1}));
  auto mounted =
    ui::mount(*scene, ui::lineEdit("", {.options = {.displayMode = static_cast<LineEditDisplayMode>(99)}}));
  REQUIRE_FALSE(mounted);
  CHECK(std::get<TreeError>(mounted.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(scene->state(7).has_value());
}

TEST_CASE("placeholder preparation OOM never publishes the internal model") {
  const auto malformed = std::string(1024, 'p') + '\xff';
  bool sawAllocationFailure = false;
  bool reachedTextValidation = false;
  for (std::ptrdiff_t allowance = 0; allowance != 100 && !reachedTextValidation; ++allowance) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app, {.maxStates = 1}));
    ui::Ref<LineEdit> ref;
    auto description = ui::lineEdit(ref, "", {.options = {.placeholder = malformed}});
    allocationTest::remaining = allowance;
    auto mounted = ui::mount(*scene, std::move(description));
    allocationTest::remaining = -1;
    REQUIRE_FALSE(mounted);
    const auto cause = std::get<TreeError>(mounted.error().cause);
    if (cause == TreeError::NO_MEMORY) {
      sawAllocationFailure = true;
    } else {
      CHECK(cause == TreeError::INVALID_ARGUMENT);
      REQUIRE(mounted.error().text);
      CHECK(mounted.error().text->kind == TextErrorKind::INVALID_UTF8);
      reachedTextValidation = true;
    }
    CHECK(ref.node().id() == NodeId{});
    CHECK(scene->state(7).has_value());
  }
  CHECK(sawAllocationFailure);
  CHECK(reachedTextValidation);
}

TEST_CASE("protected presentation omits the marker from built in diagnostic exports and managed contexts") {
  constexpr std::string_view MARKER = "uniqueNonsecretMarker89173";
  auto diagnostics = std::make_shared<Diagnostics>();
  take(diagnostics->enable({512}));
  Fixture fixture({.options = {.displayMode = LineEditDisplayMode::PASSWORD},
                   .validate =
                     [](std::string_view) {
                       return false;
                     }},
                  {.scheduler = {.diagnostics = diagnostics}});
  take(fixture.edit.text().set(MARKER).result());
  take(fixture.app->router().dispatch(PasteInputEvent{std::string(MARKER)}));
  fixture.key(U'a');
  fixture.key(U'c');
  fixture.key(U'x');
  fixture.key(U'v'); // No service: a clipboard semantic failure, without recording its draft.
  take(fixture.app->tree().setEnabled(fixture.scene->root(), false));
  CHECK_FALSE(fixture.edit.clear());
  const auto exported = exportDiagnostics(diagnostics->snapshot());
  CHECK(exported.find(MARKER) == std::string::npos);
  const auto batch = take(fixture.scene->diagnostics());
  REQUIRE_FALSE(batch.errors.empty());
  for (const auto &error : batch.errors) {
    for (const auto &context : error.contexts) {
      CHECK(context.operation.find(MARKER) == std::string::npos);
      CHECK(context.name.find(MARKER) == std::string::npos);
      CHECK(context.scope.find(MARKER) == std::string::npos);
      CHECK(context.expectedType.find(MARKER) == std::string::npos);
    }
  }
  CHECK(fixture.draft().find(MARKER) != std::string::npos); // Plaintext getters remain intentionally available.
}

TEST_CASE("history disabled clear stays disabled while command affinity and frame guards remain checked") {
  Fixture fixture({.options = {.history = {.enabled = false}}});
  take(fixture.edit.clear().result());
  CHECK_FALSE(fixture.edit.canUndo());
  std::optional<TreeError> foreign;
  std::thread worker([&] {
    foreign = fixture.edit.clear().result().error();
  });
  worker.join();
  CHECK(foreign == TreeError::WRONG_THREAD);
  take(fixture.app->tree().frame([&] {
    CHECK(fixture.edit.clear().result().error() == TreeError::WOULD_BLOCK);
  }));
}

TEST_CASE("clear captures survive result self removal and same editor reentry is blocked") {
  Fixture fixture;
  fixture.afterResult = [&] {
    CHECK(fixture.edit.clear().result().error() == TreeError::WOULD_BLOCK);
    take(fixture.scene->remove(fixture.edit.node()));
  };
  take(fixture.edit.clear().result());
  CHECK(fixture.value().empty());
  CHECK(fixture.outcomes.size() == 1);
  CHECK(fixture.edit.clear().result().error() == TreeError::STALE_NODE);
}

TEST_CASE("clear allocation failures preserve open typing group before acceptance") {
  bool rejected = false;
  bool completed = false;
  for (std::ptrdiff_t allowance = 0; allowance != 150 && !completed; ++allowance) {
    Fixture fixture({.options = {.editMode = EditMode::EXPLICIT, .displayMode = LineEditDisplayMode::PASSWORD}});
    take(fixture.app->router().dispatch(TextInputEvent{U'a'}));
    const auto old = fixture.draft();
    allocationTest::remaining = allowance;
    const auto command = fixture.edit.clear();
    allocationTest::remaining = -1;
    if (!command) {
      CHECK(command.result().error() == TreeError::NO_MEMORY);
      CHECK(fixture.draft() == old);
      CHECK(fixture.inspectPaint().find_first_not_of('*') == std::string::npos);
      take(fixture.app->router().dispatch(TextInputEvent{U'b'}));
      take(fixture.edit.undo().result());
      CHECK(fixture.draft() == UNICODE);
      rejected = true;
    } else {
      CHECK(fixture.draft().empty());
      completed = true;
    }
  }
  CHECK(rejected);
  CHECK(completed);
}

TEST_CASE("post accept publication OOM keeps matching dirty draft history and exact failure") {
  bool observed = false;
  for (std::ptrdiff_t allowance = 0; allowance != 160 && !observed; ++allowance) {
    Fixture fixture({.options = {.displayMode = LineEditDisplayMode::PASSWORD}});
    allocationTest::remaining = allowance;
    const auto command = fixture.edit.clear();
    allocationTest::remaining = -1;
    if (!command && fixture.draft().empty()) {
      CHECK(command.result().error() == TreeError::NO_MEMORY);
      CHECK(fixture.value() == UNICODE);
      REQUIRE(fixture.outcomes.size() == 1);
      CHECK(fixture.outcomes.back().error == TreeError::NO_MEMORY);
      take(fixture.app->tree().read(fixture.edit.node(), [&](const LineEdit &edit) {
        CHECK(edit.dirty());
      }));
      CHECK(fixture.inspectPaint().empty());
      take(fixture.edit.undo().result());
      CHECK(fixture.draft() == UNICODE);
      observed = true;
    }
  }
  CHECK(observed);
}

TEST_CASE("nonempty password mask allocation is staged with fluent draft and history") {
  const std::string candidate(512, 's');
  bool failed = false;
  bool succeeded = false;
  for (std::ptrdiff_t allowance = 0; allowance != 180 && !succeeded; ++allowance) {
    Fixture fixture({.options = {.editMode = EditMode::EXPLICIT, .displayMode = LineEditDisplayMode::PASSWORD}});
    allocationTest::remaining = allowance;
    const auto result = fixture.edit.text().set(candidate).result();
    allocationTest::remaining = -1;
    if (!result) {
      CHECK(std::get<TreeError>(result.error().cause) == TreeError::NO_MEMORY);
      CHECK(fixture.draft() == UNICODE);
      take(fixture.app->tree().edit(fixture.edit.node(), [&](LineEdit &widget) {
        CHECK(take(widget.measure({100, 1})).width == 4);
      }));
      CHECK_FALSE(fixture.edit.canUndo());
      failed = true;
    } else {
      CHECK(fixture.draft() == candidate);
      take(fixture.app->tree().edit(fixture.edit.node(), [&](LineEdit &widget) {
        CHECK(take(widget.measure({100, 1})).width == 513);
      }));
      take(fixture.edit.undo().result());
      CHECK(fixture.draft() == UNICODE);
      succeeded = true;
    }
  }
  CHECK(failed);
  CHECK(succeeded);
}
