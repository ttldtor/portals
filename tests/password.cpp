// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/password.hpp>

#include <thread>
#include <type_traits>

using namespace portals;

namespace {
template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool equals(const PasswordValue &value, std::string_view expected) {
  bool equal = false;

  take(value.withPlaintext([&](SecretView bytes) {
    equal = std::string_view(bytes.data(), bytes.size()) == expected;
  }));

  return equal;
}

struct ClipboardProbe : Clipboard {
  std::string bytes;
  std::function<void()> duringRead;
  unsigned reads{};
  unsigned writes{};

  std::expected<std::string, TreeError> read(std::size_t) override {
    ++reads;

    if (duringRead) {
      duringRead();
    }

    return bytes;
  }

  std::expected<void, TreeError> write(std::string_view) override {
    ++writes;

    return {};
  }
};

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create({.data_capacity = 1}));
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  Node<Column> parent = take(tree->emplace<Column>(root));
  Node<Password> node;
  std::shared_ptr<Property<PasswordValue>> model;
  PasswordResult result;
  unsigned results{};
  std::function<void(PasswordResult)> callback;
  std::optional<PasswordBinding> binding;

  Fixture(std::string_view initial = {}, PasswordOptions options = {}, std::shared_ptr<Clipboard> clipboard = {},
          std::function<bool(SecretView)> validate = {}) {
    node = take(tree->emplace<Password>(parent, options));
    model =
      take(Property<PasswordValue>::create(*tree, root, take(PasswordValue::create(initial)), Invalidation::LAYOUT));
    binding.emplace(take(bindPassword(
      *tree, *router, node, model,
      [&](PasswordResult value) {
        result = value;
        ++results;

        if (callback) {
          callback(std::move(value));
        }
      },
      std::move(clipboard), std::move(validate))));
    take(router->setFocus(node));
  }

  void input(char32_t scalar) {
    take(router->dispatch(TextInputEvent{scalar, {}}));
  }

  void action(Action command, InputModifiers modifiers = {}) {
    take(router->invoke(node, {command, modifiers}));
  }

  void key(char32_t scalar) {
    take(router->dispatch(KeyEvent{InputKey::CHARACTER, scalar, {.control = true}}));
  }

  auto snapshot() {
    return take(model->snapshot());
  }

  std::pair<std::size_t, std::size_t> selection() {
    std::pair<std::size_t, std::size_t> selected;

    take(tree->read(node, [&](const Password &password) {
      selected = {password.cursor(), password.anchor()};
    }));

    return selected;
  }

  bool dirty() {
    bool changed = false;

    take(tree->read(node, [&](const Password &password) {
      changed = password.dirty();
    }));

    return changed;
  }
};

template <class T>
concept HasTextGetter = requires(const T &value) { value.text(); };
template <class T>
concept HasUndo = requires(T &value) { value.undo(); };
static_assert(!HasTextGetter<Password>);
static_assert(!HasUndo<PasswordBinding>);
static_assert(!std::is_base_of_v<Control, Password>);
static_assert(std::is_nothrow_move_constructible_v<PasswordResult>);
} // namespace

TEST_CASE("Password immediately publishes protected immutable values and exact partial receipts") {
  Fixture fixture("seed");
  const auto original = fixture.snapshot();

  fixture.input(U'x');

  CHECK(fixture.result.status == PasswordStatus::COMMITTED);
  REQUIRE(fixture.result.commit);
  CHECK(fixture.result.commit->changed);
  CHECK(equals(fixture.result.commit->snapshot->value, "seedx"));
  CHECK(equals(original->value, "seed"));
  CHECK_FALSE(fixture.dirty());

  fixture.input(U'y');

  REQUIRE(fixture.result.commit);
  CHECK(fixture.result.commit->rejected == 1);
  CHECK(fixture.result.commit->notification_error.has_value());
  CHECK(equals(fixture.snapshot()->value, "seedxy"));

  take(fixture.scheduler->dispatch());
  CHECK(fixture.selection().first == 6);

  take(fixture.binding->cancel());
  CHECK(equals(fixture.snapshot()->value, "seedxy"));
}

TEST_CASE("Password grapheme edits never split combining or ZWJ sequences") {
  Fixture fixture("", {.editMode = EditMode::EXPLICIT, .pastePolicy = PasswordPastePolicy::ENABLED});
  constexpr auto COMBINING = "e\xcc\x81";
  constexpr auto EMOJI = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
  constexpr auto CJK = "\xe7\x95\x8c";
  const std::string initial = std::string(COMBINING) + EMOJI + CJK;

  take(fixture.router->dispatch(PasteInputEvent{initial}));
  CHECK(fixture.snapshot()->revision == 0);
  fixture.action(Action::MOVE_HOME);
  fixture.action(Action::MOVE_RIGHT, {.shift = true});
  fixture.action(Action::MOVE_RIGHT, {.shift = true});
  CHECK(fixture.selection() == std::pair<std::size_t, std::size_t>{14, 0});

  fixture.input(U'X');
  take(fixture.binding->confirm());
  CHECK(equals(fixture.snapshot()->value, std::string("X") + CJK));

  fixture.action(Action::MOVE_END);
  fixture.action(Action::ERASE_BACKWARD);
  take(fixture.binding->confirm());
  CHECK(equals(fixture.snapshot()->value, "X"));

  // A new combining scalar joins its preceding cluster; caret snaps to the new end.
  fixture.input(U'\u0301');
  CHECK(fixture.selection().first == 3);
  fixture.action(Action::ERASE_BACKWARD);
  take(fixture.binding->confirm());
  CHECK(fixture.snapshot()->value.empty());
}

TEST_CASE("Password word navigation uses numeric whole-grapheme boundaries") {
  Fixture fixture("one two!", {.editMode = EditMode::EXPLICIT});

  fixture.action(Action::MOVE_LEFT, {.control = true});
  CHECK(fixture.selection().first == 4);
  fixture.action(Action::MOVE_LEFT, {.control = true});
  CHECK(fixture.selection().first == 0);
  fixture.action(Action::MOVE_RIGHT, {.shift = true, .control = true});
  CHECK(fixture.selection() == std::pair<std::size_t, std::size_t>{4, 0});
  fixture.input(U'X');
  take(fixture.binding->confirm());
  CHECK(equals(fixture.snapshot()->value, "Xtwo!"));
}

TEST_CASE("Password deferred conflicts preserve drafts and cancel adopts the latest checked value") {
  Fixture fixture("old", {.editMode = EditMode::EXPLICIT});

  take(fixture.model->set(take(PasswordValue::create("fresh"))));
  fixture.input(U'!');
  CHECK(fixture.selection().first == 6);
  take(fixture.scheduler->dispatch());
  CHECK(fixture.dirty());
  take(fixture.model->set(take(PasswordValue::create("external"))));
  take(fixture.model->set(take(PasswordValue::create("latest"))));
  take(fixture.binding->sync(take(fixture.router->focused())));
  CHECK(take(fixture.binding->confirm()).status == PasswordStatus::CONFLICT);
  CHECK(fixture.selection().first == 6);
  CHECK(equals(fixture.snapshot()->value, "latest"));
  CHECK(take(fixture.binding->cancel()).status == PasswordStatus::CANCELLED);
  CHECK_FALSE(fixture.dirty());
  fixture.input(U'?');
  take(fixture.binding->confirm());
  CHECK(equals(fixture.snapshot()->value, "latest?"));
}

TEST_CASE("Password completion and explicit modes use their own focus-loss publication policies") {
  for (auto mode : {EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture fixture("", {.editMode = mode});
    fixture.input(U'x');

    auto other = take(fixture.tree->emplace<Password>(fixture.parent));
    auto otherModel = take(Property<PasswordValue>::create(*fixture.tree, fixture.root, take(PasswordValue::create()),
                                                           Invalidation::LAYOUT));
    auto otherBinding =
      take(bindPassword(*fixture.tree, *fixture.router, other, std::move(otherModel), [](PasswordResult) {
      }));
    take(fixture.router->setFocus(other));

    CHECK(fixture.dirty() == (mode == EditMode::EXPLICIT));
    CHECK(fixture.snapshot()->value.empty() == (mode == EditMode::EXPLICIT));

    if (mode == EditMode::ON_COMPLETION) {
      CHECK(fixture.result.trigger == LineEditTrigger::BLUR);
    }
  }
}

TEST_CASE("Password semantic rejection retains its draft while invalid Unicode is rejected in full") {
  unsigned validations = 0;
  Fixture fixture("", {.pastePolicy = PasswordPastePolicy::ENABLED}, {}, [&](SecretView bytes) {
    ++validations;

    return bytes.size() >= 2;
  });

  fixture.input(U'a');
  CHECK(fixture.result.status == PasswordStatus::VALIDATION_FAILED);
  CHECK(fixture.dirty());
  CHECK(fixture.snapshot()->revision == 0);
  fixture.input(U'b');
  CHECK(fixture.result.status == PasswordStatus::COMMITTED);
  CHECK(equals(fixture.snapshot()->value, "ab"));

  const auto priorSelection = fixture.selection();
  const auto priorRevision = fixture.snapshot()->revision;
  const auto priorValidations = validations;

  for (std::string_view bytes : {"\xff", "a\nb", "a\rb", "\xe2\x80\xa8", "\xe2\x80\xa9"}) {
    take(fixture.router->dispatch(PasteInputEvent{std::string(bytes)}));
    CHECK(fixture.result.status == PasswordStatus::INVALID_TEXT);
    CHECK(fixture.result.failure.has_value());
    CHECK(fixture.selection() == priorSelection);
    CHECK(fixture.snapshot()->revision == priorRevision);
    CHECK(validations == priorValidations);
  }
}

TEST_CASE("Password whole-value limits and programmatic Unicode rejection preserve the current draft") {
  Fixture fixture("ab", {.limits = {4, 3, 2, 4}, .pastePolicy = PasswordPastePolicy::ENABLED});
  const auto revision = fixture.snapshot()->revision;
  const auto selection = fixture.selection();

  fixture.input(U'c');
  CHECK(fixture.result.status == PasswordStatus::LIMIT_EXCEEDED);
  CHECK(fixture.selection() == selection);
  CHECK(fixture.snapshot()->revision == revision);

  auto invalid = take(fixture.binding->setValue(take(PasswordValue::create("\xff"))));
  CHECK(invalid.status == PasswordStatus::INVALID_TEXT);
  REQUIRE(invalid.failure);
  CHECK(std::holds_alternative<PasswordValueAccessFailure<TextError>>(*invalid.failure));

  auto line = take(fixture.binding->setValue(take(PasswordValue::create("a\n"))));
  CHECK(line.status == PasswordStatus::INVALID_TEXT);
  REQUIRE(line.failure);
  CHECK(std::get<PasswordInputError>(*line.failure) == PasswordInputError::LINE_SEPARATOR);
  CHECK(fixture.selection() == selection);
  CHECK(equals(fixture.snapshot()->value, "ab"));
}

TEST_CASE("Password clipboard is opt-in and never exports bytes; read-only allows explicit setValue") {
  auto clipboard = std::make_shared<ClipboardProbe>();
  clipboard->bytes = "paste";
  Fixture fixture("seed", {}, clipboard);

  fixture.key(U'a');
  fixture.key(U'c');
  fixture.key(U'x');
  fixture.key(U'v');
  take(fixture.router->dispatch(PasteInputEvent{"paste"}));
  CHECK(clipboard->reads == 0);
  CHECK(clipboard->writes == 0);
  CHECK(equals(fixture.snapshot()->value, "seed"));

  Fixture enabled("", {.pastePolicy = PasswordPastePolicy::ENABLED}, clipboard);
  enabled.key(U'v');
  CHECK(clipboard->reads == 1);
  CHECK(equals(enabled.snapshot()->value, "paste"));

  Fixture readOnly("seed", {.readOnly = true, .pastePolicy = PasswordPastePolicy::ENABLED}, clipboard);
  readOnly.input(U'x');
  readOnly.action(Action::ERASE_BACKWARD);
  readOnly.key(U'v');
  CHECK(equals(readOnly.snapshot()->value, "seed"));
  CHECK(clipboard->reads == 1);
  take(readOnly.binding->setValue(take(PasswordValue::create("replacement"))));
  CHECK(equals(readOnly.snapshot()->value, "replacement"));
}

TEST_CASE("Password no-op identity and empty edits preserve revisions and work flags") {
  Fixture fixture;
  const auto original = fixture.snapshot();

  take(fixture.tree->consumeFrameRequest());
  take(fixture.binding->setValue(original->value));
  fixture.action(Action::ERASE_BACKWARD);
  fixture.action(Action::MOVE_HOME);

  CHECK(fixture.snapshot() == original);
  CHECK(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE);

  auto equalBytesNewIdentity = take(PasswordValue::create());
  take(fixture.binding->setValue(equalBytesNewIdentity));
  CHECK(fixture.snapshot()->revision == original->revision + 1);

  take(fixture.binding->setValue(take(PasswordValue::create("abc"))));
  fixture.action(Action::MOVE_HOME);
  take(fixture.tree->consumeFrameRequest());
  take(fixture.binding->setValue(fixture.snapshot()->value));
  CHECK(fixture.selection().first == 0);
  CHECK(take(fixture.tree->consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("Password callbacks safely remove self, ancestors and bindings") {
  for (bool ancestor : {false, true}) {
    Fixture fixture;
    fixture.callback = [&](PasswordResult) {
      take(fixture.tree->remove(ancestor ? Node<Widget>(fixture.parent) : Node<Widget>(fixture.node)));
      fixture.binding.reset();
    };

    fixture.input(U'x');
    CHECK(equals(fixture.snapshot()->value, "x"));
    CHECK(fixture.result.status == PasswordStatus::COMMITTED);
  }

  auto clipboard = std::make_shared<ClipboardProbe>();
  clipboard->bytes = "secret";
  Fixture fixture("", {.pastePolicy = PasswordPastePolicy::ENABLED}, clipboard);
  clipboard->duringRead = [&] {
    take(fixture.tree->remove(fixture.parent));
  };
  fixture.key(U'v');
  CHECK(fixture.result.status == PasswordStatus::FAILED);
  CHECK(fixture.snapshot()->revision == 0);
}

TEST_CASE("Password validator removal and exceptions preserve protected transaction state") {
  Fixture *current = nullptr;
  Fixture fixture("base", {}, {}, [&](SecretView) {
    take(current->tree->remove(current->parent));

    return true;
  });
  current = &fixture;
  fixture.input(U'x');
  CHECK(fixture.result.status == PasswordStatus::FAILED);
  CHECK(equals(fixture.snapshot()->value, "base"));

  Fixture throwing("base", {}, {}, [](SecretView) -> bool {
    throw 1;
  });
  throwing.input(U'x');
  CHECK(throwing.result.status == PasswordStatus::FAILED);
  REQUIRE(throwing.result.failure);
  CHECK(std::holds_alternative<PasswordValueAccessFailure<std::monostate>>(*throwing.result.failure));
  CHECK(equals(throwing.snapshot()->value, "base"));
  CHECK(throwing.selection().first == 4);
}

TEST_CASE("Password public binding rejects foreign-thread access without callbacks") {
  Fixture fixture;
  std::optional<PasswordFailure> failure;

  std::thread worker([&] {
    auto synced = fixture.binding->sync({});

    REQUIRE_FALSE(synced);
    failure = std::move(synced.error());
  });
  worker.join();

  REQUIRE(failure);
  CHECK(std::get<TreeError>(*failure) == TreeError::WRONG_THREAD);
  CHECK(fixture.results == 0);
}

TEST_CASE("Password busy source edits and model adoption preserve the existing presentation") {
  Fixture fixture("old");
  const auto original = fixture.snapshot();
  const auto selection = fixture.selection();

  take(original->value.withPlaintext([&](SecretView) {
    fixture.input(U'x');
  }));
  CHECK(fixture.result.status == PasswordStatus::FAILED);
  REQUIRE(fixture.result.failure);
  const auto &editFailure = std::get<PasswordValueAccessFailure<SecureStringFailure>>(*fixture.result.failure);
  REQUIRE(editFailure.access);
  REQUIRE(editFailure.access->primary);
  CHECK(editFailure.access->primary->code == SecureStringErrorCode::BUSY);
  CHECK(fixture.snapshot() == original);
  CHECK(fixture.selection() == selection);

  auto replacement = take(PasswordValue::create("replacement"));
  take(fixture.model->set(replacement));
  take(replacement.withPlaintext([&](SecretView) {
    auto synced = fixture.binding->sync(take(fixture.router->focused()));

    REQUIRE_FALSE(synced);
    const auto &adoptionFailure = std::get<PasswordValueAccessFailure<TextError>>(synced.error());
    REQUIRE(adoptionFailure.access);
    REQUIRE(adoptionFailure.access->primary);
    CHECK(adoptionFailure.access->primary->code == SecureStringErrorCode::BUSY);
    CHECK(fixture.selection() == selection);

    take(fixture.scheduler->dispatch());
    CHECK(fixture.result.status == PasswordStatus::FAILED);
    REQUIRE(fixture.result.failure);
    CHECK(std::holds_alternative<PasswordValueAccessFailure<TextError>>(*fixture.result.failure));
  }));

  take(fixture.binding->sync(take(fixture.router->focused())));
  CHECK(fixture.selection().first == 11);
}

TEST_CASE("Password delivery failure preserves successful receipts and an independent primary failure") {
  Fixture committed;
  committed.callback = [](PasswordResult) {
    throw 1;
  };
  auto accepted = take(committed.binding->setValue(take(PasswordValue::create("new"))));

  CHECK(accepted.status == PasswordStatus::COMMITTED);
  CHECK(accepted.callbackError == TreeError::CALLBACK_FAILED);
  REQUIRE(accepted.commit);
  CHECK(equals(accepted.commit->snapshot->value, "new"));

  Fixture rejected("old", {}, {}, [](SecretView) -> bool {
    throw 1;
  });
  rejected.callback = [](PasswordResult) {
    throw 2;
  };
  auto failed = take(rejected.binding->setValue(take(PasswordValue::create("new"))));

  CHECK(failed.status == PasswordStatus::FAILED);
  CHECK(failed.callbackError == TreeError::CALLBACK_FAILED);
  REQUIRE(failed.failure);
  CHECK(std::holds_alternative<PasswordValueAccessFailure<std::monostate>>(*failed.failure));
  CHECK(equals(rejected.snapshot()->value, "old"));
}

TEST_CASE("Password frames contain only constant masks and no-echo has fixed geometry") {
  struct Sink : PaintContext {
    unsigned textCommands{};
    bool onlyMasks{true};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        if (const auto *text = std::get_if<TextCommand>(&command)) {
          ++textCommands;
          onlyMasks = onlyMasks && text->text.bytes().find_first_not_of('*') == std::string_view::npos;
        }
      }

      return {};
    }
  };

  for (auto display : {PasswordDisplayMode::PASSWORD, PasswordDisplayMode::NO_ECHO}) {
    Fixture fixture("e\xcc\x81\xe7\x95\x8c", {.displayMode = display});
    Sink sink;
    const auto rectangle = take(CellRect::from({}, {20, 1}));
    CellSize intrinsic{};

    take(fixture.tree->frame([&] {
      intrinsic = take(fixture.tree->measure(fixture.node, {20, 1}));
      take(layoutTree(*fixture.tree, fixture.root, rectangle, rectangle));
      take(fixture.tree->paint(fixture.node, sink));
    }));

    CHECK(sink.onlyMasks);
    CHECK(intrinsic.width == (display == PasswordDisplayMode::NO_ECHO ? 1 : 3));
    CHECK((sink.textCommands == 0) == (display == PasswordDisplayMode::NO_ECHO));
  }
}
