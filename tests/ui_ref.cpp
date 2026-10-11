// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui.hpp>

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
}

// Process-local failure injection is armed only around the public operation under test.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure.load() == 0) {
    throw std::bad_alloc();
  }

  if (allocationsBeforeFailure.load() > 0) {
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

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
};

struct ThrowingMoveClick {
  TreeError cause;

  explicit ThrowingMoveClick(TreeError value) : cause(value) {
  }

  ThrowingMoveClick(const ThrowingMoveClick &) = default;

  ThrowingMoveClick(ThrowingMoveClick &&other) : cause(other.cause) {
    if (cause == TreeError::NO_MEMORY) {
      throw std::bad_alloc();
    }

    throw std::runtime_error("callback transfer failed");
  }

  void operator()(const ClickEvent &) const {
  }
};

bool hasCause(const ManagedError &error, TreeError expected) {
  const auto *cause = std::get_if<TreeError>(&error.cause);

  return cause && *cause == expected;
}

template <class T>
concept HasText = requires(T ref) { ref.text(); };
static_assert(HasText<ui::Ref<Label>>);
static_assert(HasText<ui::Ref<Button>>);
static_assert(HasText<ui::Ref<CheckBox>>);
static_assert(HasText<ui::Ref<LineEdit>>);
static_assert(!HasText<ui::Ref<Widget>>);
static_assert(std::same_as<decltype(std::declval<ui::TextProperty>() += "x"), ui::TextProperty>);
} // namespace

TEST_CASE("late click internal callback transfer preserves typed errors and diagnostics") {
  Fixture fixture;
  ui::Ref<Button> button;

  take(ui::mount(*fixture.scene, ui::button(button, "late")));

  for (const auto cause : {TreeError::NO_MEMORY, TreeError::CALLBACK_FAILED}) {
    // Passing an lvalue copies at API entry; only its internal forwarding move throws.
    ThrowingMoveClick action(cause);
    std::expected<void, ManagedError> result;

    CHECK_NOTHROW(result = button.onClick(action));
    REQUIRE_FALSE(result);
    CHECK(hasCause(result.error(), cause));
  }

  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 2);
  CHECK(hasCause(diagnostics.errors[0].error, TreeError::NO_MEMORY));
  CHECK(hasCause(diagnostics.errors[1].error, TreeError::CALLBACK_FAILED));

  int clicks = 0;

  take(button.onClick([&] {
    ++clicks;
  }));
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(clicks == 1);
}

TEST_CASE("text references append owned expressions and retain chain results") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("start").reference(message)));

  auto property = message.text();
  auto chain = property.append(" ").append(text("{}", 123.0));

  REQUIRE(chain.result());
  CHECK(take(chain.get()) == "start 123");
  chain += "!";
  CHECK(take(message.text().get()) == "start 123!");
}

TEST_CASE("assignment and plus-equals work with every requested expression") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("old").reference(message)));

  auto assigned = message.text() = text("{}", 12);
  message.text() += " ";
  message.text() += text("{}", 123.0);
  auto localized = message.text([](std::string_view key) -> std::expected<std::string, TextResourceError> {
    return key == "currency" ? std::expected<std::string, TextResourceError>{"USD"}
                             : std::unexpected(TextResourceError::MISSING_RESOURCE);
  });
  localized += i18n("currency");

  REQUIRE(assigned.result());
  REQUIRE(localized.result());
  CHECK(take(message.text().get()) == "12 123USD");
  CHECK(take(assigned.get()) == "12 123USD");
}

TEST_CASE("caption and draft equal operations do not request another frame") {
  Fixture fixture;
  ui::Ref<Label> message;
  ui::Ref<LineEdit> edit;
  const auto model = take(fixture.scene->state(std::string("draft")));
  take(
    ui::mount(*fixture.scene, ui::column(ui::label("same").reference(message), ui::lineEdit(model).reference(edit))));
  take(fixture.app->tree().consumeFrameRequest());

  auto captionResult = message.text() = "same";
  auto editorResult = edit.text() += "";

  REQUIRE(captionResult.result());
  REQUIRE(editorResult.result());
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.app->tree().read(edit.node(), [](const LineEdit &widget) {
    CHECK_FALSE(widget.dirty());
  }));
}

TEST_CASE("caption changes preserve checkbox value and button activation") {
  Fixture fixture;
  ui::Ref<Button> button;
  ui::Ref<CheckBox> checkbox;
  int clicks = 0;
  const auto checked = take(fixture.scene->state(true));
  take(ui::mount(*fixture.scene, ui::column(ui::button("old",
                                                       [&]() -> std::expected<void, TreeError> {
                                                         ++clicks;
                                                         return {};
                                                       })
                                              .reference(button),
                                            ui::checkbox("old", checked).reference(checkbox))));
  take((button.text() = "Run").result());
  take((checkbox.text() += " caption").result());
  take(fixture.app->router().setFocus(button.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));

  CHECK(clicks == 1);
  CHECK(take(checked.snapshot())->value);
  take(fixture.app->tree().read(checkbox.node(), [](const CheckBox &widget) {
    CHECK(widget.checked());
    CHECK(widget.text().bytes() == "old caption");
  }));
}

TEST_CASE("failed mutations preserve text and collect original typed details") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("safe").reference(message)));
  const std::string invalid("\xff", 1);
  const auto expected = GraphemeText::from(invalid).error();
  auto failed = message.text() = text(invalid);

  REQUIRE_FALSE(failed.result());
  CHECK(failed.result().error().text == expected);
  CHECK(failed.result().error().phase == ManagedPhase::TEXT);
  CHECK(failed.result().error().node == message.node().id());
  CHECK(take(message.text().get()) == "safe");
  auto missing = message.text() += i18n("currency");

  REQUIRE_FALSE(missing.result());
  CHECK(missing.result().error().resource == TextResourceError::RESOLVER_REQUIRED);
  const auto errors = take(fixture.scene->diagnostics());

  REQUIRE(errors.errors.size() == 2);
  CHECK(errors.errors[0].error.text == expected);
  REQUIRE(errors.errors[1].contexts.size() == 1);
  CHECK(errors.errors[1].contexts[0].operation == "text.append");
  CHECK(errors.errors[1].contexts[0].name == "currency");
  CHECK(errors.errors[1].contexts[0].expectedType == "Label");
}

TEST_CASE("resource failures and callback allocation exceptions leave text unchanged") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("safe").reference(message)));

  auto absent = message.text([](std::string_view) -> std::expected<std::string, TextResourceError> {
    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  }) = i18n("absent");
  auto thrown = message.text([](std::string_view) -> std::expected<std::string, TextResourceError> {
    throw std::runtime_error("resolver failed");
  }) += i18n("thrown");
  auto allocation = message.text([](std::string_view) -> std::expected<std::string, TextResourceError> {
    throw std::bad_alloc();
  }) = i18n("allocation");

  REQUIRE_FALSE(absent.result());
  REQUIRE_FALSE(thrown.result());
  REQUIRE_FALSE(allocation.result());
  CHECK(absent.result().error().resource == TextResourceError::MISSING_RESOURCE);
  CHECK(thrown.result().error().resource == TextResourceError::RESOLVER_FAILED);
  CHECK(hasCause(allocation.result().error(), TreeError::NO_MEMORY));
  CHECK(take(message.text().get()) == "safe");
}

TEST_CASE("saved proxies and snapshots own their observations and returned bytes") {
  Fixture fixture;
  auto property = [&] {
    ui::Ref<Label> local;
    take(ui::mount(*fixture.scene, ui::label("owned").reference(local)));

    return local.text();
  }();
  auto snapshot = take(property.get());
  take(property.append("!").result());

  CHECK(snapshot == "owned");
  CHECK(take(property.get()) == "owned!");
  fixture.scene.reset();
  auto failed = property = "after destruction";

  REQUIRE_FALSE(failed.result());
  CHECK(hasCause(failed.result().error(), TreeError::CLOSED));
  REQUIRE_FALSE(property.get());
}

TEST_CASE("empty moved stale closed and foreign-thread references fail safely") {
  Fixture fixture;
  ui::Ref<Label> empty;
  auto emptyResult = empty.text() += "x";

  REQUIRE_FALSE(emptyResult.result());
  CHECK(hasCause(emptyResult.result().error(), TreeError::CLOSED));
  ui::Ref<Label> moved;
  auto message = std::move(moved);
  auto movedResult = moved.text() = "x";

  CHECK(hasCause(movedResult.result().error(), TreeError::CLOSED));
  take(ui::mount(*fixture.scene, ui::label("live").reference(message)));
  std::optional<ManagedError> foreign;
  std::thread worker([&] {
    auto result = message.text() += "foreign";
    foreign = result.result().error();
  });
  worker.join();

  REQUIRE(foreign);
  CHECK(hasCause(*foreign, TreeError::WRONG_THREAD));
  CHECK(take(message.text().get()) == "live");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  take(fixture.scene->remove(message.node()));
  auto stale = message.text() = "stale";

  REQUIRE_FALSE(stale.result());
  CHECK(hasCause(stale.result().error(), TreeError::STALE_NODE));
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
  take(fixture.scene->close());
  auto closed = message.text() = "closed";

  CHECK(hasCause(closed.result().error(), TreeError::CLOSED));
}

TEST_CASE("failed description publication leaves both Ref and legacy NodeRef empty") {
  Fixture fixture;
  ui::Ref<Label> first;
  ui::NodeRef<Label> legacy;
  auto mounted = ui::mount(
    *fixture.scene, ui::column(ui::label("a").reference(first).reference(legacy), ui::label("b").reference(first)));

  REQUIRE_FALSE(mounted);
  CHECK(first.node() == Node<Label>{});
  CHECK(legacy.node() == Node<Label>{});
  CHECK(hasCause(first.text().get().error(), TreeError::CLOSED));
  take(ui::mount(*fixture.scene, ui::label("retry").reference(first).reference(legacy)));
  CHECK(first.node() == legacy.node());
}

TEST_CASE("resolver reentry through aliases rejects nested mutation without corrupting the outer result") {
  Fixture fixture;
  ui::Ref<Label> first;
  ui::Ref<Label> second;
  take(ui::mount(*fixture.scene, ui::label("old").reference(first).reference(second)));
  std::optional<ManagedError> nested;
  auto result = first.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
    auto attempt = second.text() = "nested";
    nested = attempt.result().error();

    return "resolved";
  }) = i18n("key");

  REQUIRE(result.result());
  REQUIRE(nested);
  CHECK(hasCause(*nested, TreeError::WOULD_BLOCK));
  CHECK(take(first.text().get()) == "resolved");
}

TEST_CASE("resolver retirement close and destruction are rechecked after user code") {
  SUBCASE("retired node") {
    Fixture fixture;
    ui::Ref<Label> message;
    take(ui::mount(*fixture.scene, ui::label("old").reference(message)));
    auto result = message.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
      take(fixture.scene->remove(message.node()));

      return "new";
    }) = i18n("key");

    REQUIRE_FALSE(result.result());
    CHECK(hasCause(result.result().error(), TreeError::STALE_NODE));
  }
  SUBCASE("closed scene") {
    Fixture fixture;
    ui::Ref<Label> message;
    take(ui::mount(*fixture.scene, ui::label("old").reference(message)));
    auto result = message.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
      take(fixture.scene->close());

      return "new";
    }) = i18n("key");

    CHECK(hasCause(result.result().error(), TreeError::CLOSED));
  }
  SUBCASE("destroyed scene and caller expression") {
    Fixture fixture;
    ui::Ref<Label> message;
    take(ui::mount(*fixture.scene, ui::label("old").reference(message)));
    auto expression = std::make_unique<TextValue>(i18n("key"));
    auto property = message.text([&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      fixture.scene.reset();
      expression.reset();
      CHECK(key == "key");

      return "new";
    });
    auto result = property.set(*expression);

    CHECK(hasCause(result.result().error(), TreeError::CLOSED));
  }
}

TEST_CASE("frame and error-observer reentry preserve the previous caption") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("old").reference(message)));
  int resolverCalls = 0;
  std::optional<ManagedError> nested;
  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    auto result = message.text() = "nested";
    nested = result.result().error();
  }));
  take(fixture.app->tree().frame([&] {
    auto result = message.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolverCalls;

      return "new";
    }) = i18n("key");

    CHECK(hasCause(result.result().error(), TreeError::WOULD_BLOCK));
  }));

  CHECK(resolverCalls == 0);
  REQUIRE(nested);
  CHECK(hasCause(*nested, TreeError::WOULD_BLOCK));
  CHECK(take(message.text().get()) == "old");
  const auto errors = take(fixture.scene->diagnostics());

  CHECK(errors.errors.size() == 2);
  CHECK(errors.suppressedNotifications == 1);
}

TEST_CASE("fluent editor changes are drafts with unchanged Enter Escape blur and validation") {
  Fixture fixture;
  ui::Ref<LineEdit> edit;
  ui::Ref<Button> button;
  const auto model = take(fixture.scene->state(std::string("initial")));
  std::vector<LineEditStatus> statuses;
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
    statuses.push_back(result.status);

    return {};
  };
  config.validate = [](std::string_view value) {
    return value != "rejected";
  };
  take(ui::mount(*fixture.scene, ui::column(ui::lineEdit(model, std::move(config)).reference(edit),
                                            ui::button("other", []() -> std::expected<void, TreeError> {
                                              return {};
                                            }).reference(button))));
  take(fixture.app->router().setFocus(edit.node()));
  take((edit.text() = "draft").result());

  CHECK(take(model.snapshot())->value == "initial");
  CHECK(statuses == std::vector{LineEditStatus::EDITED});
  take(fixture.app->router().setFocus(button.node()));
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  CHECK(take(edit.text().get()) == "draft");
  CHECK(take(model.snapshot())->value == "initial");
  take(fixture.app->router().setFocus(edit.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(statuses.back() == LineEditStatus::COMMITTED);
  CHECK(take(model.snapshot())->value == "draft");
  take((edit.text() = "rejected").result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(statuses.back() == LineEditStatus::VALIDATION_FAILED);
  CHECK(take(edit.text().get()) == "rejected");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  CHECK(statuses.back() == LineEditStatus::CANCELLED);
  CHECK(take(edit.text().get()) == "draft");
  take((edit.text() += " pending").result());
  take(model.set("external"));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(statuses.back() == LineEditStatus::CONFLICT);
  CHECK(take(edit.text().get()) == "draft pending");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  CHECK(take(edit.text().get()) == "external");
}

TEST_CASE("editor candidate errors preserve text selection dirty flag and model revision") {
  Fixture fixture;
  ui::Ref<LineEdit> edit;
  const auto model = take(fixture.scene->state(std::string("safe")));
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.options.limits.max_input_bytes = 5;
  take(ui::mount(*fixture.scene, ui::lineEdit(model, std::move(config)).reference(edit)));
  const auto revision = take(model.snapshot())->revision;
  auto tooLong = edit.text() += "xx";
  auto newline = edit.text() = "\n";
  auto invalid = edit.text() = std::string_view("\xff", 1);

  REQUIRE_FALSE(tooLong.result());
  CHECK(tooLong.result().error().text == TextError{TextErrorKind::INPUT_LIMIT, 0});
  REQUIRE_FALSE(newline.result());
  CHECK(hasCause(newline.result().error(), TreeError::INVALID_ARGUMENT));
  CHECK_FALSE(newline.result().error().text);
  REQUIRE_FALSE(invalid.result());
  CHECK(invalid.result().error().text == TextError{TextErrorKind::INVALID_UTF8, 0});
  CHECK(take(model.snapshot())->revision == revision);
  take(fixture.app->tree().read(edit.node(), [](const LineEdit &widget) {
    CHECK(widget.text().bytes() == "safe");
    CHECK(widget.cursor() == 4);
    CHECK(widget.anchor() == 4);
    CHECK_FALSE(widget.dirty());
  }));
}

TEST_CASE("resolver model reentry preserves the editor's original conflict baseline") {
  Fixture fixture;
  ui::Ref<LineEdit> edit;
  const auto model = take(fixture.scene->state(std::string("base")));
  LineEditStatus status{};
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
    status = result.status;

    return {};
  };
  take(ui::mount(*fixture.scene, ui::lineEdit(model, std::move(config)).reference(edit)));
  take(fixture.app->router().setFocus(edit.node()));
  auto result = edit.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
    take(model.set("external"));
    take(fixture.scene->prepare(*fixture.app, {40, 10}));

    return "suffix";
  }) += i18n("key");

  REQUIRE(result.result());
  CHECK(take(edit.text().get()) == "basesuffix");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(status == LineEditStatus::CONFLICT);
  CHECK(take(model.snapshot())->value == "external");
}

TEST_CASE("equal editor candidate retains the original revision without requesting redraw") {
  Fixture fixture;
  ui::Ref<LineEdit> edit;
  const auto model = take(fixture.scene->state(std::string("base")));
  LineEditStatus status{};
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
    status = result.status;

    return {};
  };
  take(ui::mount(*fixture.scene, ui::lineEdit(model, std::move(config)).reference(edit)));
  take(fixture.app->router().setFocus(edit.node()));
  auto result = edit.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
    take(model.set("basesuffix"));
    take(fixture.scene->prepare(*fixture.app, {40, 10}));
    take(fixture.app->tree().consumeFrameRequest());

    return "suffix";
  }) += i18n("key");

  REQUIRE(result.result());
  CHECK(take(edit.text().get()) == "basesuffix");
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.app->tree().read(edit.node(), [](const LineEdit &widget) {
    CHECK(widget.dirty());
    CHECK(widget.cursor() == 10);
    CHECK(widget.anchor() == 10);
  }));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(status == LineEditStatus::CONFLICT);
  CHECK(take(model.snapshot())->value == "basesuffix");
}

TEST_CASE("allocation budget sweep keeps get failures typed and leaves the value intact") {
  Fixture fixture;
  ui::Ref<Label> message;
  // Exceed small-string storage so the owning read allocates on every supported standard library.
  const std::string LONG_TEXT(256, 'x');
  take(ui::mount(*fixture.scene, ui::label(LONG_TEXT).reference(message)));
  auto property = message.text();
  bool sawFailure = false;
  bool sawSuccess = false;

  for (std::ptrdiff_t budget = 0; budget < 32; ++budget) {
    std::optional<std::expected<std::string, ManagedError>> outcome;
    bool escaped = false;

    allocationsBeforeFailure = budget;

    try {
      const auto snapshot = property.get();

      allocationsBeforeFailure = -1;
      outcome.emplace(snapshot);
    } catch (const std::bad_alloc &) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;

    REQUIRE_FALSE(escaped);
    REQUIRE(outcome);

    if (*outcome) {
      sawSuccess = true;
      CHECK(**outcome == LONG_TEXT);
    } else {
      sawFailure = true;
      CHECK(hasCause(outcome->error(), TreeError::NO_MEMORY));
    }

    CHECK(take(message.text().get()) == LONG_TEXT);
  }

  CHECK(sawFailure);
  CHECK(sawSuccess);
}

TEST_CASE("operator plus owns joined graphemes without changing any source or requesting frames") {
  Fixture fixture;
  ui::Ref<Label> message;
  ui::Ref<Label> message2;
  ui::Ref<Label> message3;
  take(ui::mount(*fixture.scene, ui::column(ui::label("target").reference(message), ui::label("e").reference(message2),
                                            ui::label("\xcc\x81").reference(message3))));
  take(fixture.app->tree().consumeFrameRequest());
  auto combined = message2.text() + message3.text();
  const auto result = take(combined.result());

  CHECK(result.bytes() == "e\xcc\x81");
  CHECK(result.graphemes().size() == 1);
  CHECK(take(message2.text().get()) == "e");
  CHECK(take(message3.text().get()) == "\xcc\x81");
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  auto assigned = message.text() = message2.text() + message3.text();

  REQUIRE(assigned.result());
  CHECK(take(message.text().get()) == "e\xcc\x81");
  take(fixture.scene->remove(message2.node()));
  take(fixture.scene->remove(message3.node()));
  take((message.text() = combined + "!").result());
  CHECK(take(message.text().get()) == "e\xcc\x81!");
}

TEST_CASE("operator plus supports strings formatted resource and owning-result chains") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("Price ").reference(message)));
  auto property = message.text([](std::string_view) -> std::expected<std::string, TextResourceError> {
    return "USD";
  });
  auto value = property + text("{}", 123.0) + " " + i18n("currency");

  CHECK(take(value.result()).bytes() == "Price 123 USD");
  CHECK(take(message.text().get()) == "Price ");
  take((message.text() = value).result());
  take((message.text() += value + "!").result());
  CHECK(take(message.text().get()) == "Price 123 USDPrice 123 USD!");
}

TEST_CASE("failed right-hand property read preserves source identity and the receiving value") {
  Fixture fixture;
  ui::Ref<Label> target;
  ui::Ref<Label> left;
  ui::Ref<Label> right;
  take(ui::mount(*fixture.scene, ui::column(ui::label("target").reference(target), ui::label("left").reference(left),
                                            ui::label("right").reference(right))));
  const auto sourceNode = right.node().id();
  take(fixture.scene->remove(right.node()));
  take(fixture.app->tree().consumeFrameRequest());
  auto failed = target.text() = left.text() + right.text();

  REQUIRE_FALSE(failed.result());
  CHECK(hasCause(failed.result().error(), TreeError::STALE_NODE));
  CHECK(failed.result().error().node == sourceNode);
  CHECK(take(left.text().get()) == "left");
  CHECK(take(target.text().get()) == "target");
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  const auto errors = take(fixture.scene->diagnostics());

  REQUIRE(errors.errors.size() == 1);
  CHECK(errors.errors[0].error.node == sourceNode);
  REQUIRE(errors.errors[0].contexts.size() == 1);
  CHECK(errors.errors[0].contexts[0].operation == "text.assign.concat");
}

TEST_CASE("append resegments combining and emoji boundaries before enforcing whole-draft limits") {
  Fixture fixture;
  ui::Ref<LineEdit> edit;
  const auto model = take(fixture.scene->state(std::string("\xf0\x9f\x91\xa9")));
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.options.limits.max_graphemes = 1;
  config.options.limits.max_cells = 2;
  take(ui::mount(*fixture.scene, ui::lineEdit(model, std::move(config)).reference(edit)));
  auto joined = edit.text() += "\xe2\x80\x8d\xf0\x9f\x92\xbb";

  REQUIRE(joined.result());
  take(fixture.app->tree().read(edit.node(), [](const LineEdit &widget) {
    CHECK(widget.text().graphemes().size() == 1);
    CHECK(widget.text().measure() == 2);
  }));
  take((edit.text() = "e").result());
  take((edit.text() += "\xcc\x81").result());
  CHECK(take(edit.text().get()) == "e\xcc\x81");
}

TEST_CASE("concatenation uses left policy and receiving assignment reapplies its own policy and limits") {
  ApplicationOptions wideOptions;
  wideOptions.widthPolicy.ambiguous = AmbiguousWidth::WIDE;
  auto wideApp = take(Application::create(wideOptions));
  auto wideScene = take(ManagedScene::create(*wideApp));
  Fixture narrow;
  ui::Ref<Label> source;
  ui::Ref<LineEdit> target;
  take(ui::mount(*wideScene, ui::label("\xc2\xa1").reference(source)));
  const auto model = take(narrow.scene->state(std::string{}));
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.options.limits.max_cells = 1;
  take(ui::mount(*narrow.scene, ui::lineEdit(model, std::move(config)).reference(target)));
  const auto joined = source.text() + "";

  CHECK(take(joined.result()).measure() == 2);
  take((target.text() = joined).result());
  take(narrow.app->tree().read(target.node(), [](const LineEdit &widget) {
    CHECK(widget.text().measure() == 1);
  }));
  const auto rejected = target.text() = joined + "x";

  REQUIRE_FALSE(rejected.result());
  CHECK(rejected.result().error().text->kind == TextErrorKind::CELL_LIMIT);
  CHECK(take(target.text().get()) == "\xc2\xa1");
}

TEST_CASE("allocation budget sweeps preserve failed mutation and original diagnostic causes") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("old").reference(message)));
  const std::string replacement(128, 'r');
  auto property = message.text();
  bool sawFailure = false;
  bool sawSuccess = false;

  for (std::ptrdiff_t budget = 0; budget < 96; ++budget) {
    take((message.text() = "old").result());
    take(fixture.scene->clearErrors());
    std::optional<ui::TextProperty> outcome;
    bool escaped = false;

    allocationsBeforeFailure = budget;

    try {
      outcome.emplace(property.set(replacement));
    } catch (const std::bad_alloc &) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;

    REQUIRE_FALSE(escaped);
    REQUIRE(outcome);

    if (outcome->result()) {
      sawSuccess = true;
      CHECK(take(message.text().get()) == replacement);
    } else {
      sawFailure = true;
      CHECK(hasCause(outcome->result().error(), TreeError::NO_MEMORY));
      CHECK(take(message.text().get()) == "old");
      const auto diagnostics = take(fixture.scene->diagnostics());

      REQUIRE(diagnostics.errors.size() == 1);
      CHECK(hasCause(diagnostics.errors[0].error, TreeError::NO_MEMORY));
    }
  }

  CHECK(sawFailure);
  CHECK(sawSuccess);
}

TEST_CASE("allocation budget sweeps keep read-only concatenation observable without changing sources") {
  Fixture fixture;
  ui::Ref<Label> left;
  ui::Ref<Label> right;
  take(ui::mount(*fixture.scene, ui::column(ui::label("left").reference(left), ui::label("right").reference(right))));
  auto lhs = left.text();
  auto rhs = right.text();
  bool sawFailure = false;
  bool sawSuccess = false;

  for (std::ptrdiff_t budget = 0; budget < 96; ++budget) {
    std::optional<ui::TextResult> result;
    bool escaped = false;

    allocationsBeforeFailure = budget;

    try {
      const auto computed = lhs + rhs;

      allocationsBeforeFailure = -1;
      result.emplace(computed);
    } catch (const std::bad_alloc &) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;

    INFO("concatenation allocation budget: ", budget);
    REQUIRE_FALSE(escaped);
    REQUIRE(result);
    auto value = result->result();

    if (value) {
      sawSuccess = true;
      CHECK(value->bytes() == "leftright");
    } else {
      sawFailure = true;
      CHECK(hasCause(value.error(), TreeError::NO_MEMORY));
    }

    CHECK(take(left.text().get()) == "left");
    CHECK(take(right.text().get()) == "right");
  }

  CHECK(sawFailure);
  CHECK(sawSuccess);
}

TEST_CASE("property assignment copies value while proxy construction copies observation") {
  Fixture fixture;
  ui::Ref<Label> target;
  ui::Ref<Label> source;
  take(ui::mount(*fixture.scene,
                 ui::column(ui::label("target").reference(target), ui::label("source").reference(source))));
  auto proxy = target.text();
  auto copiedObservation = proxy;
  auto assigned = target.text() = source.text();

  REQUIRE(assigned.result());
  CHECK(take(target.text().get()) == "source");
  CHECK(take(source.text().get()) == "source");
  take((copiedObservation = "new").result());
  CHECK(take(target.text().get()) == "new");
  take((target.text() = target.text()).result());
  CHECK(take(target.text().get()) == "new");
  take(fixture.scene->remove(source.node()));
  auto failed = target.text() = source.text();

  REQUIRE_FALSE(failed.result());
  CHECK(hasCause(failed.result().error(), TreeError::STALE_NODE));
  CHECK(take(target.text().get()) == "new");
}

TEST_CASE("error observer self-removal and reuse of the same proxy preserve outer error receipts") {
  Fixture fixture;
  ui::Ref<Label> message;
  take(ui::mount(*fixture.scene, ui::label("old").reference(message)));
  auto property = message.text();
  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    auto nested = property = "nested";

    CHECK(hasCause(nested.result().error(), TreeError::WOULD_BLOCK));
    take(fixture.app->tree().remove(message.node()));
  }));
  const auto result = property = std::string_view("\xff", 1);

  REQUIRE_FALSE(result.result());
  CHECK(result.result().error().text == TextError{TextErrorKind::INVALID_UTF8, 0});
  CHECK(property.result().error().text == TextError{TextErrorKind::INVALID_UTF8, 0});
}

TEST_CASE("compact ref-first factories publish complete observations and void clicks") {
  Fixture fixture;
  ui::Ref<Label> caption;
  ui::Ref<Button> button;
  ui::Ref<LineEdit> editor;
  const auto model = take(fixture.scene->state(std::string("value")));
  int clicks = 0;
  take(ui::mount(*fixture.scene, ui::column(ui::label(caption, "caption").id("caption-id"),
                                            ui::button(button, "go",
                                                       [&] {
                                                         ++clicks;
                                                       }),
                                            ui::lineEdit(editor, model))));
  CHECK(take(caption.text().get()) == "caption");
  CHECK(take(editor.text().get()) == "value");
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(clicks == 1);
}

TEST_CASE("late ref click owns routed parameters and rejects duplicate binding") {
  Fixture fixture;
  ui::Ref<Button> button;
  take(ui::mount(*fixture.scene, ui::button(button, "late")));
  std::optional<ClickEvent> observed;
  take(button.onClick([&](const ClickEvent &event) {
    observed = event;
  }));
  const InputModifiers modifiers{.shift = true};
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE, modifiers}));
  REQUIRE(observed);
  CHECK(observed->target == button.node());
  CHECK(observed->modifiers == modifiers);
  CHECK(std::get<ActionEvent>(observed->event).action == Action::ACTIVATE);
  auto duplicate = button.onClick([] {
  });
  REQUIRE_FALSE(duplicate);
  CHECK(hasCause(duplicate.error(), TreeError::INVALID_ARGUMENT));
}

TEST_CASE("late click checks publication affinity liveness and pre-run ownership") {
  Fixture fixture;
  ui::Ref<Button> button;
  auto unpublished = button.onClick([] {
  });
  REQUIRE_FALSE(unpublished);
  CHECK(hasCause(unpublished.error(), TreeError::CLOSED));
  take(ui::mount(*fixture.scene, ui::button(button, "late")));
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  int clicks = 0;
  take(button.onClick([&] {
    ++clicks;
  }));
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(clicks == 1);
  std::optional<ManagedError> foreign;
  std::thread worker([&] {
    auto result = button.onClick([] {
    });
    if (!result) {
      foreign = result.error();
    }
  });
  worker.join();
  REQUIRE(foreign);
  CHECK(hasCause(*foreign, TreeError::WRONG_THREAD));
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  take(fixture.scene->remove(button.node()));
  auto stale = button.onClick([] {
  });
  REQUIRE_FALSE(stale);
  CHECK(hasCause(stale.error(), TreeError::STALE_NODE));
  fixture.scene.reset();
  auto destroyed = button.onClick([] {
  });
  REQUIRE_FALSE(destroyed);
  CHECK(hasCause(destroyed.error(), TreeError::CLOSED));
}

TEST_CASE("ref-first mount failure does not publish callbacks or observations") {
  Fixture fixture;
  ui::Ref<Button> first;
  ui::Ref<Button> second;
  int clicks = 0;
  auto mounted = ui::mount(
    *fixture.scene,
    ui::column(
      ui::button(first, "first",
                 [&] {
                   ++clicks;
                 }),
      ui::button(second, "second").setup([](ManagedScene &, Node<Button>) -> std::expected<void, ManagedError> {
        return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
      })));
  REQUIRE_FALSE(mounted);
  CHECK(first.node() == Node<Button>{});
  CHECK(second.node() == Node<Button>{});
  CHECK(clicks == 0);
  CHECK(hasCause(first
                   .onClick([] {
                   })
                   .error(),
                 TreeError::CLOSED));
  take(ui::mount(*fixture.scene, ui::button(first, "retry").onClick([&] {
    ++clicks;
  })));
  take(fixture.app->router().invoke(first.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(clicks == 1);
}

TEST_CASE("late click keeps admitted captures through self-removal") {
  Fixture fixture;
  ui::Ref<Button> button;
  take(ui::mount(*fixture.scene, ui::button(button, "remove")));
  auto lifetime = std::make_shared<int>(7);
  std::weak_ptr<int> observer = lifetime;
  take(button.onClick([&, lifetime](const ClickEvent &event) {
    CHECK(event.target == button.node());
    take(fixture.scene->remove(event.target));
    CHECK_FALSE(observer.expired());
    CHECK(*lifetime == 7);
  }));
  lifetime.reset();
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  take(fixture.app->router().focused());
  CHECK(observer.expired());
}

TEST_CASE("late click parameter adapters preserve exact editor command diagnostics") {
  Fixture fixture;
  ui::Ref<Button> button;
  ui::Ref<LineEdit> editor;
  auto model = take(fixture.scene->state(std::string("value")));
  take(ui::mount(*fixture.scene, ui::column(ui::button(button, "undo"), ui::lineEdit(editor, model))));
  take(button.onClick([&](const ClickEvent &) {
    return editor.undo();
  }));
  take(fixture.scene->remove(editor.node()));
  take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
  auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(hasCause(diagnostics.errors[0].error, TreeError::STALE_NODE));
  CHECK(diagnostics.errors[0].error.node == editor.node().id());
}

TEST_CASE("typed click errors and exceptions report their original cause once") {
  for (const auto cause : {TreeError::NODE_LIMIT, TreeError::NO_MEMORY, TreeError::CALLBACK_FAILED}) {
    Fixture fixture;
    ui::Ref<Button> button;
    take(ui::mount(*fixture.scene, ui::button(button, "error")));
    take(button.onClick([cause](const ClickEvent &) -> std::expected<void, TreeError> {
      if (cause == TreeError::NO_MEMORY) {
        throw std::bad_alloc();
      }
      if (cause == TreeError::CALLBACK_FAILED) {
        throw std::runtime_error("test failure");
      }
      return std::unexpected(cause);
    }));
    take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
    auto diagnostics = take(fixture.scene->diagnostics());
    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(hasCause(diagnostics.errors[0].error, cause));
  }
}

TEST_CASE("failed late click allocation leaves the button available for a retry") {
  constexpr std::ptrdiff_t ALLOCATION_BUDGETS = 12;
  std::size_t failures = 0;
  std::size_t successes = 0;
  for (std::ptrdiff_t budget = 0; budget < ALLOCATION_BUDGETS; ++budget) {
    Fixture fixture;
    ui::Ref<Button> button;
    take(ui::mount(*fixture.scene, ui::button(button, "allocation")));
    take(fixture.app->tree().consumeFrameRequest());
    auto capture = std::make_shared<int>(0);
    auto action = [capture](const ClickEvent &) {
      ++*capture;
    };
    allocationsBeforeFailure = budget;
    auto registered = button.onClick(action);
    allocationsBeforeFailure = -1;
    if (!registered) {
      ++failures;
      CHECK(hasCause(registered.error(), TreeError::NO_MEMORY));
      CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
      take(button.onClick(action));
    } else {
      ++successes;
    }
    take(fixture.app->router().invoke(button.node(), ActionEvent{Action::ACTIVATE}));
    CHECK(*capture == 1);
  }
  CHECK(failures > 0);
  CHECK(successes > 0);
}

TEST_CASE("compact ids precede styles preserve classes and survive theme switching") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene,
                 ui::label(label, "id").id("message").style(StyleTarget{.type = "Control", .classes = {"accent"}})));
  ui::Ref<Label> legacy;
  take(ui::mount(*fixture.scene, ui::label(legacy, "legacy")
                                   .style(StyleTarget{.type = "Control", .id = "message", .classes = {"accent"}})
                                   .style(StyleTarget{.type = "Control", .classes = {"accent"}})));
  auto checkColor = [&](std::optional<RgbColor> expected) {
    take(fixture.scene->prepare(*fixture.app, {40, 10}));
    take(fixture.app->tree().read(label.node(), [&](const Label &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.foreground == expected);
    }));
  };
  take(fixture.scene->theme(take(parsePss("Control#message.accent { foreground: #010203; }"))));
  checkColor(RgbColor{1, 2, 3});
  take(fixture.app->tree().read(legacy.node(), [](const Label &widget) {
    REQUIRE(widget.computedStyle());
    CHECK_FALSE(widget.computedStyle()->normal.foreground);
  }));
  take(fixture.scene->theme(take(parsePss("Control#message.accent { foreground: #040506; }"))));
  checkColor(RgbColor{4, 5, 6});
  take(fixture.app->tree().consumeFrameRequest());
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.scene->style(label.node(), StyleTarget{.type = "Control", .classes = {"accent"}}));
  checkColor({});
}

TEST_CASE("short key handlers compose before routing and direct onInput replaces the chain") {
  Fixture fixture;
  int fallback = 0;
  std::vector<InputKey> seen;
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++fallback;
    return false;
  }));
  take(fixture.scene->onKey(ui::key(InputKey::F2), [&] {
    seen.push_back(InputKey::F2);
  }));
  take(fixture.scene->onKey(ui::key(InputKey::F3), [&](const KeyEvent &event) {
    seen.push_back(event.key);
  }));
  take(fixture.scene->onKey(ui::key(InputKey::F10), [&] {
    seen.push_back(InputKey::F10);
  }));
  for (const auto key : {InputKey::F2, InputKey::F3, InputKey::F10}) {
    CHECK(take(fixture.scene->input(*fixture.app, KeyEvent{key})));
  }
  CHECK(seen == std::vector<InputKey>{InputKey::F2, InputKey::F3, InputKey::F10});
  CHECK(fallback == 0);
  CHECK_FALSE(take(fixture.scene->input(*fixture.app, KeyEvent{InputKey::F2, {}, {}, KeyAction::RELEASE})));
  CHECK_FALSE(take(fixture.scene->input(*fixture.app, ui::key(InputKey::F2, {.shift = true}))));
  CHECK(fallback == 2);
  take(fixture.scene->onKey(ui::key(InputKey::F2), [&] {
    seen.push_back(InputKey::F1);
  }));
  CHECK(take(fixture.scene->input(*fixture.app, KeyEvent{InputKey::F2, {}, {}, KeyAction::REPEAT})));
  CHECK(seen.back() == InputKey::F1);
  take(fixture.scene->onInput({}));
  CHECK_FALSE(take(fixture.scene->input(*fixture.app, ui::key(InputKey::F2))));
  CHECK(seen.size() == 4);
}

TEST_CASE("short character key preserves modifiers and editor command reporting") {
  Fixture fixture;
  ui::Ref<LineEdit> editor;
  auto model = take(fixture.scene->state(std::string("value")));
  take(ui::mount(*fixture.scene, ui::lineEdit(editor, model)));
  take(fixture.scene->onKey(ui::character(U'z', {.control = true}), [&](const KeyEvent &event) {
    CHECK(event.modifiers.control);
    return editor.undo();
  }));
  CHECK_FALSE(take(fixture.scene->input(*fixture.app, ui::character(U'z'))));
  take(fixture.scene->remove(editor.node()));
  CHECK(take(fixture.scene->input(*fixture.app, ui::character(U'z', {.control = true}))));
  auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(hasCause(diagnostics.errors[0].error, TreeError::STALE_NODE));
}

TEST_CASE("short key allocation and capacity failures preserve the existing chain") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app, ManagedOptions{.maxKeyHandlers = 2}));
  int first = 0;
  int second = 0;
  take(scene->onKey(ui::key(InputKey::F2), [&] {
    ++first;
  }));
  allocationsBeforeFailure = 0;
  auto failed = scene->onKey(ui::key(InputKey::F3), [&] {
    ++second;
  });
  allocationsBeforeFailure = -1;
  REQUIRE_FALSE(failed);
  CHECK(hasCause(failed.error(), TreeError::NO_MEMORY));
  CHECK(take(scene->input(*app, ui::key(InputKey::F2))));
  CHECK(first == 1);
  CHECK_FALSE(take(scene->input(*app, ui::key(InputKey::F3))));
  take(scene->onKey(ui::key(InputKey::F3), [&] {
    ++second;
  }));
  auto full = scene->onKey(ui::key(InputKey::F10), [] {
  });
  REQUIRE_FALSE(full);
  CHECK(hasCause(full.error(), TreeError::NODE_LIMIT));
  CHECK(take(scene->input(*app, ui::key(InputKey::F3))));
  CHECK(second == 1);
  take(scene->onInput({}));
  take(scene->onKey(ui::key(InputKey::F10), [] {
  }));
  CHECK(take(scene->input(*app, ui::key(InputKey::F10))));
}

TEST_CASE("parameter clicks preserve keyboard space and mouse source values") {
  Fixture fixture;
  ui::Ref<Button> button;
  std::vector<ClickEvent> clicks;
  take(ui::mount(*fixture.scene, ui::button(button, "source", [&](const ClickEvent &event) {
    clicks.push_back(event);
  })));
  const auto viewport = take(CellRect::from({}, {40, 10}));
  take(fixture.app->tree().arrange(fixture.app->root(), viewport, viewport));
  take(fixture.app->tree().arrange(fixture.scene->root(), viewport, viewport));
  take(fixture.app->tree().arrange(button.node(), take(CellRect::from({2, 3}, {12, 1})), viewport));
  take(fixture.app->router().setFocus(button.node()));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER, {}, {.control = true}}));
  take(fixture.app->router().dispatch(TextInputEvent{U' ', {.alt = true}}));
  const MouseInputEvent mouse{3, 3, MouseButton::LEFT, MouseAction::PRESS, {.shift = true}};
  take(fixture.app->router().dispatch(mouse));
  take(fixture.app->router().dispatch(MouseInputEvent{3, 3, MouseButton::LEFT, MouseAction::RELEASE, {}}));
  REQUIRE(clicks.size() == 3);
  CHECK(clicks[0].modifiers.control);
  CHECK(std::get<ActionEvent>(clicks[0].event).action == Action::ACTIVATE);
  CHECK(clicks[1].modifiers.alt);
  CHECK(std::get<TextInputEvent>(clicks[1].event).scalar == U' ');
  CHECK(clicks[2].target == button.node());
  CHECK(std::get<MouseInputEvent>(clicks[2].event) == mouse);
}

TEST_CASE("legacy empty button callback remains an unambiguous rejected recipe") {
  Fixture fixture;
  auto mounted = ui::mount(*fixture.scene, ui::button("empty", {}));
  REQUIRE_FALSE(mounted);
  CHECK(hasCause(mounted.error(), TreeError::INVALID_ARGUMENT));
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());
}

TEST_CASE("failed compact id preparation preserves metadata and frame requests") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "id").id("original")));
  take(fixture.scene->theme(take(parsePss("Control#original { foreground: #010203; }"))));
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  take(fixture.app->tree().consumeFrameRequest());
  allocationsBeforeFailure = 0;
  auto failed = fixture.scene->id(label.node(), "a new identity whose storage requires allocation");
  allocationsBeforeFailure = -1;
  REQUIRE_FALSE(failed);
  CHECK(hasCause(failed.error(), TreeError::NO_MEMORY));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  take(fixture.app->tree().read(label.node(), [](const Label &widget) {
    REQUIRE(widget.computedStyle());
    CHECK(widget.computedStyle()->normal.foreground == RgbColor{1, 2, 3});
  }));
}

TEST_CASE("input replacement publishes callback and key budget before retiring old captures") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app, ManagedOptions{.maxKeyHandlers = 1}));
  int clicks = 0;

  struct Reenter {
    std::function<void()> retire;

    ~Reenter() {
      retire();
    }
  };

  auto capture = std::make_shared<Reenter>();
  capture->retire = [&] {
    take(scene->onKey(ui::key(InputKey::F2), [&] {
      ++clicks;
    }));
  };
  take(scene->onKey(ui::key(InputKey::F3), [capture] {
  }));
  capture.reset();
  take(scene->onInput({}));
  CHECK(take(scene->input(*app, ui::key(InputKey::F2))));
  CHECK(clicks == 1);
  auto full = scene->onKey(ui::key(InputKey::F10), [] {
  });
  REQUIRE_FALSE(full);
  CHECK(hasCause(full.error(), TreeError::NODE_LIMIT));
}
