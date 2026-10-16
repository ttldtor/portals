// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <stdexcept>

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
  ui::Ref<LineEdit> edit;
  ui::Ref<Button> other;
  ManagedState<std::string> model = take(scene->state(std::string("base")));
  std::vector<LineEditResult> outcomes;
  std::function<void()> afterResult;

  explicit Fixture(ui::LineEditConfig config = {}, ApplicationOptions options = {})
      : app(take(Application::create(options))) {
    config.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
      outcomes.push_back(std::move(result));

      if (afterResult) {
        afterResult();
      }

      return {};
    };
    take(ui::mount(*scene, ui::column(ui::lineEdit(model, std::move(config)).reference(edit),
                                      ui::button("other", []() -> std::expected<void, TreeError> {
                                        return {};
                                      }).reference(other))));
    take(app->router().setFocus(edit.node()));
  }

  std::string value() {
    return take(model.snapshot())->value;
  }

  std::string draft() {
    return take(edit.text().get());
  }
};
} // namespace

TEST_CASE("default mode publishes each accepted text edit without Enter") {
  Fixture fixture;
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));

  CHECK(fixture.draft() == "basex");
  CHECK(fixture.value() == "basex");
  REQUIRE(fixture.outcomes.size() == 1);
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
  CHECK(fixture.outcomes.back().trigger == LineEditTrigger::INPUT);
  REQUIRE(fixture.outcomes.back().commit);
  CHECK(fixture.outcomes.back().commit->changed);
  take((fixture.edit.text() += "y").result());
  CHECK(fixture.value() == "basexy");
  CHECK(fixture.outcomes.size() == 2);
  CHECK(fixture.outcomes.back().trigger == LineEditTrigger::FLUENT);
}

TEST_CASE("semantic rejection accepts visible fluent text without publishing the model") {
  ui::LineEditConfig config;
  config.validate = [](std::string_view candidate) {
    return candidate != "-";
  };
  Fixture fixture(std::move(config));
  auto accepted = fixture.edit.text() = "-";

  REQUIRE(accepted.result());
  CHECK(fixture.draft() == "-");
  CHECK(fixture.value() == "base");
  REQUIRE(fixture.outcomes.size() == 1);
  CHECK(fixture.outcomes.back().status == LineEditStatus::VALIDATION_FAILED);
  CHECK_FALSE(fixture.outcomes.back().commit);
}

TEST_CASE("completion mode publishes every blur before the next frame") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "draft").result());

  CHECK(fixture.value() == "base");
  take(fixture.app->router().setFocus(fixture.other.node()));
  CHECK(fixture.value() == "draft");
  REQUIRE(fixture.outcomes.size() == 2);
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
  CHECK(fixture.outcomes.back().trigger == LineEditTrigger::BLUR);
  take(fixture.app->router().setFocus(fixture.edit.node()));
  take(fixture.app->router().setFocus(fixture.other.node()));
  CHECK(fixture.outcomes.size() == 2);
}

TEST_CASE("explicit mode preserves a draft across blur and accepts programmatic confirmation") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "draft").result());
  take(fixture.app->router().setFocus(fixture.other.node()));

  CHECK(fixture.value() == "base");
  CHECK(fixture.draft() == "draft");
  CHECK(fixture.outcomes.size() == 1);
  take(fixture.app->router().invoke(fixture.edit.node(), {Action::ACTIVATE}));
  CHECK(fixture.value() == "draft");
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
}

TEST_CASE("immediate Escape adopts the latest successful model without undoing it") {
  ui::LineEditConfig config;
  config.validate = [](std::string_view candidate) {
    return candidate != "-";
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "applied").result());
  take((fixture.edit.text() = "-").result());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));

  CHECK(fixture.value() == "applied");
  CHECK(fixture.draft() == "applied");
  CHECK(fixture.outcomes.back().status == LineEditStatus::CANCELLED);
}

TEST_CASE("external revisions conflict with pending candidates in every mode") {
  constexpr std::array MODES{EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT};

  for (auto mode : MODES) {
    CAPTURE(mode);
    ui::LineEditConfig config;
    config.options.editMode = mode;
    config.validate = [](std::string_view candidate) {
      return candidate != "-";
    };
    Fixture fixture(std::move(config));
    take((fixture.edit.text() = "-").result());
    take(fixture.model.set("external"));
    take(fixture.scene->prepare(*fixture.app, {40, 10}));

    CHECK(fixture.draft() == "-");
    take((fixture.edit.text() = "next").result());
    take(fixture.app->router().dispatch(KeyEvent{InputKey::ENTER}));
    CHECK(fixture.outcomes.back().status == LineEditStatus::CONFLICT);
    CHECK(fixture.value() == "external");
    CHECK(fixture.draft() == "next");
    take(fixture.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    CHECK(fixture.draft() == "external");
  }
}

TEST_CASE("validator reentry cannot overwrite a newer model revision") {
  Fixture *owner = nullptr;
  ui::LineEditConfig config;
  config.validate = [&](std::string_view candidate) {
    CHECK(candidate == "candidate");
    take(owner->model.set("external"));
    auto nested = owner->edit.text() = "nested";

    REQUIRE_FALSE(nested.result());
    CHECK(std::get<TreeError>(nested.result().error().cause) == TreeError::WOULD_BLOCK);

    return true;
  };
  Fixture fixture(std::move(config));
  owner = &fixture;
  take((fixture.edit.text() = "candidate").result());

  CHECK(fixture.draft() == "candidate");
  CHECK(fixture.value() == "external");
  CHECK(fixture.outcomes.back().status == LineEditStatus::CONFLICT);
}

TEST_CASE("validator exceptions retain the accepted candidate and report typed failure") {
  ui::LineEditConfig config;
  config.validate = [](std::string_view) -> bool {
    throw std::bad_alloc();
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "candidate").result());

  CHECK(fixture.draft() == "candidate");
  CHECK(fixture.value() == "base");
  REQUIRE(fixture.outcomes.size() == 1);
  CHECK(fixture.outcomes.back().status == LineEditStatus::FAILED);
  CHECK(fixture.outcomes.back().error == TreeError::NO_MEMORY);
  const auto errors = take(fixture.scene->diagnostics());

  REQUIRE(errors.errors.size() == 1);
  CHECK(std::get<TreeError>(errors.errors[0].error.cause) == TreeError::NO_MEMORY);
}

TEST_CASE("accepted paste shares the confirmation policy while invalid paste changes nothing") {
  Fixture fixture;
  take(fixture.app->router().dispatch(PasteInputEvent{"paste"}));

  CHECK(fixture.value() == "basepaste");
  const auto revision = take(fixture.model.snapshot())->revision;
  take(fixture.app->tree().consumeFrameRequest());
  take(fixture.app->router().dispatch(PasteInputEvent{"\r\n"}));
  CHECK(fixture.outcomes.back().status == LineEditStatus::INVALID_TEXT);
  CHECK(fixture.value() == "basepaste");
  CHECK(fixture.draft() == "basepaste");
  CHECK(take(fixture.model.snapshot())->revision == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("equal fluent text does not repeat validation publication or redraw") {
  int validations = 0;
  ui::LineEditConfig config;
  config.validate = [&](std::string_view) {
    ++validations;

    return true;
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "candidate").result());
  const auto revision = take(fixture.model.snapshot())->revision;
  take(fixture.app->tree().consumeFrameRequest());
  take((fixture.edit.text() = "candidate").result());
  take((fixture.edit.text() += "").result());

  CHECK(validations == 1);
  CHECK(fixture.outcomes.size() == 1);
  CHECK(take(fixture.model.snapshot())->revision == revision);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("result callback may destroy the managed scene after successful publication") {
  Fixture fixture;
  fixture.afterResult = [&] {
    fixture.scene.reset();
    throw std::runtime_error("after destruction");
  };
  const auto node = fixture.edit.node();
  const auto accepted = fixture.edit.text() = "applied";

  REQUIRE(accepted.result());
  REQUIRE(fixture.outcomes.size() == 1);
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
  REQUIRE(fixture.outcomes.back().commit);
  CHECK(fixture.outcomes.back().commit->snapshot->value == "applied");
  CHECK(fixture.app->tree().inspect(node).error() == TreeError::STALE_NODE);
}

TEST_CASE("focus delivery is direct scoped and rejects reentry without changing RoutedEvent") {
  Fixture fixture;
  const auto root = take(fixture.app->tree().root());
  const auto first = take(fixture.app->tree().emplace<Widget>(root));
  const auto second = take(fixture.app->tree().emplace<Widget>(root));
  int losses = 0;
  int gains = 0;
  auto input = [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  };
  auto firstBinding = take(fixture.app->router().bind(first, input, [&](Widget &, bool focused) {
    if (!focused) {
      ++losses;
      CHECK(take(fixture.app->router().focused()) == second);
      CHECK(fixture.app->router().setFocus(first).error() == TreeError::WOULD_BLOCK);
      CHECK(fixture.app->router().invoke(first, {Action::ACTIVATE}).error() == TreeError::WOULD_BLOCK);
      take(fixture.app->tree().remove(second));
    }

    return std::expected<void, TreeError>{};
  }));
  auto secondBinding = take(fixture.app->router().bind(second, input, [&](Widget &, bool focused) {
    gains += focused;

    return std::expected<void, TreeError>{};
  }));
  take(fixture.app->router().setFocus(first));
  take(fixture.app->router().setFocus(first));
  take(fixture.app->router().setFocus(second));

  CHECK(losses == 1);
  CHECK(gains == 0);
}

TEST_CASE("focus callback error preserves the published focus and returns its typed cause") {
  Fixture fixture;
  const auto root = take(fixture.app->tree().root());
  const auto target = take(fixture.app->tree().emplace<Widget>(root));
  auto registration = take(fixture.app->router().bind(
    target,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [](Widget &, bool) -> std::expected<void, TreeError> {
      return std::unexpected(TreeError::NO_MEMORY);
    }));
  const auto changed = fixture.app->router().setFocus(target);

  REQUIRE_FALSE(changed);
  CHECK(changed.error() == TreeError::NO_MEMORY);
  CHECK(take(fixture.app->router().focused()) == target);
}

TEST_CASE("hidden editor receives focus loss and confirms its pending value") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "pending").result());
  take(fixture.app->tree().setVisible(fixture.edit.node(), false));
  CHECK(take(fixture.app->router().focused()) == fixture.other.node());
  CHECK(fixture.value() == "pending");
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
}

TEST_CASE("Tab blur commits before returning from the routed input") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  Fixture fixture(std::move(config));
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  CHECK(fixture.value() == "base");
  take(fixture.app->router().dispatch(KeyEvent{InputKey::TAB}));
  CHECK(fixture.value() == "basex");
  CHECK(take(fixture.app->router().focused()) == fixture.other.node());
}

TEST_CASE("immediate resolver model reentry retains the captured conflict baseline") {
  Fixture fixture;
  const auto accepted = fixture.edit.text([&](std::string_view) -> std::expected<std::string, TextResourceError> {
    take(fixture.model.set("basesuffix"));
    take(fixture.scene->prepare(*fixture.app, {40, 10}));

    return "suffix";
  }) += i18n("key");

  REQUIRE(accepted.result());
  CHECK(fixture.draft() == "basesuffix");
  CHECK(fixture.value() == "basesuffix");
  CHECK(fixture.outcomes.back().status == LineEditStatus::CONFLICT);
}

TEST_CASE("blur validation failure is typed without restoring focus or discarding the candidate") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator failed");
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "candidate").result());
  const auto changed = fixture.app->router().setFocus(fixture.other.node());

  REQUIRE_FALSE(changed);
  CHECK(changed.error() == TreeError::CALLBACK_FAILED);
  CHECK(take(fixture.app->router().focused()) == fixture.other.node());
  CHECK(fixture.draft() == "candidate");
  CHECK(fixture.value() == "base");
  CHECK(fixture.outcomes.back().error == TreeError::CALLBACK_FAILED);
}

TEST_CASE("notification rejection does not undo immediate publication or repeat the result callback") {
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  Fixture fixture({}, options);
  take((fixture.edit.text() = "first").result());
  take((fixture.edit.text() = "second").result());

  REQUIRE(fixture.outcomes.size() == 2);
  REQUIRE(fixture.outcomes.back().commit);
  CHECK(fixture.outcomes.back().commit->changed);
  CHECK(fixture.outcomes.back().commit->rejected == 1);
  CHECK(fixture.value() == "second");
  take(fixture.scene->prepare(*fixture.app, {40, 10}));
  take(fixture.app->scheduler().dispatch());
  CHECK(fixture.draft() == "second");
  CHECK(fixture.outcomes.size() == 2);
}

TEST_CASE("model notifications and presentation sync are not blur events") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  Fixture fixture(std::move(config));
  take(fixture.model.set("external"));
  take(fixture.app->scheduler().dispatch());
  take((fixture.edit.text() += " draft").result());
  take(fixture.scene->prepare(*fixture.app, {40, 10}));

  CHECK(fixture.value() == "external");
  CHECK(fixture.draft() == "external draft");
  CHECK(fixture.outcomes.size() == 1);
  take(fixture.app->router().setFocus(fixture.other.node()));
  CHECK(fixture.value() == "external draft");
}

TEST_CASE("every mode applies the same policy to text paste and fluent candidates") {
  constexpr std::array MODES{EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT};

  for (auto mode : MODES) {
    CAPTURE(mode);
    ui::LineEditConfig config;
    config.options.editMode = mode;
    Fixture fixture(std::move(config));
    take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
    take(fixture.app->router().dispatch(PasteInputEvent{"y"}));
    take((fixture.edit.text() += "z").result());
    CHECK(fixture.draft() == "basexyz");
    CHECK(fixture.value() == (mode == EditMode::IMMEDIATE ? "basexyz" : "base"));
    take(fixture.app->router().invoke(fixture.edit.node(), {Action::ACTIVATE}));
    CHECK(fixture.value() == "basexyz");
    REQUIRE(fixture.outcomes.back().commit);
    CHECK(fixture.outcomes.back().commit->changed == (mode != EditMode::IMMEDIATE));
  }
}

TEST_CASE("clipboard cut and paste use the same immediate policy as text input") {
  class MemoryClipboard final : public Clipboard {
  public:
    std::string bytes;

    std::expected<std::string, TreeError> read(std::size_t) override {
      return bytes;
    }

    std::expected<void, TreeError> write(std::string_view selection) override {
      bytes = selection;

      return {};
    }
  };

  auto clipboard = std::make_shared<MemoryClipboard>();
  ui::LineEditConfig config;
  config.clipboard = clipboard;
  Fixture fixture(std::move(config));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'a', {.control = true}}));
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'x', {.control = true}}));
  CHECK(clipboard->bytes == "base");
  CHECK(fixture.value().empty());
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'v', {.control = true}}));
  CHECK(fixture.value() == "base");
  CHECK(fixture.outcomes.back().status == LineEditStatus::COMMITTED);
}

TEST_CASE("an unknown edit mode is rejected during binding") {
  ui::LineEditConfig config;
  config.options.editMode = static_cast<EditMode>(-1);
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  const auto model = take(scene->state(std::string("initial")));
  const auto mounted = ui::mount(*scene, ui::lineEdit(model, std::move(config)));

  REQUIRE_FALSE(mounted);
  CHECK(std::get<TreeError>(mounted.error().cause) == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("allocation budget distinguishes rejected text from failed publication of accepted text") {
  const std::string LONG_TEXT(256, 'x');
  bool rejectedText = false;
  bool rejectedPublish = false;
  bool committed = false;

  for (std::ptrdiff_t budget = 0; budget < 128; ++budget) {
    Fixture fixture;
    fixture.outcomes.reserve(1);
    auto property = fixture.edit.text();
    std::optional<ui::TextProperty> accepted;
    bool escaped = false;
    allocationsBeforeFailure = budget;

    try {
      const auto operation = property.set(LONG_TEXT);

      allocationsBeforeFailure = -1;
      accepted.emplace(operation);
    } catch (const std::bad_alloc &) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;

    REQUIRE_FALSE(escaped);
    REQUIRE(accepted);

    if (!accepted->result()) {
      rejectedText = true;
      CHECK(fixture.draft() == "base");
      CHECK(fixture.value() == "base");
      CHECK(std::get<TreeError>(accepted->result().error().cause) == TreeError::NO_MEMORY);
    } else {
      CHECK(fixture.draft() == LONG_TEXT);
      REQUIRE(fixture.outcomes.size() == 1);

      if (fixture.outcomes.back().commit) {
        committed = true;
        CHECK(fixture.value() == LONG_TEXT);
      } else {
        rejectedPublish = true;
        CHECK(fixture.value() == "base");
        CHECK(fixture.outcomes.back().error == TreeError::NO_MEMORY);
      }
    }
  }

  CHECK(rejectedText);
  CHECK(rejectedPublish);
  CHECK(committed);
}

TEST_CASE("review R1 loss failure still delivers gain and preserves the first error") {
  auto app = take(Application::create());
  const auto root = take(app->tree().root());
  const auto first = take(app->tree().emplace<Widget>(root));
  const auto second = take(app->tree().emplace<Widget>(root));
  int gains = 0;
  auto input = [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  };
  auto firstBinding = take(app->router().bind(first, input, [](Widget &, bool focused) {
    return focused ? std::expected<void, TreeError>{} : std::unexpected(TreeError::NO_MEMORY);
  }));
  auto secondBinding = take(app->router().bind(second, input, [&](Widget &, bool focused) {
    gains += focused;

    return std::expected<void, TreeError>(std::unexpected(TreeError::CALLBACK_FAILED));
  }));
  take(app->router().setFocus(first));
  const auto changed = app->router().setFocus(second);

  REQUIRE_FALSE(changed);
  CHECK(changed.error() == TreeError::NO_MEMORY);
  CHECK(gains == 1);
  CHECK(take(app->router().focused()) == second);
  take(app->router().setFocus(second));
  CHECK(gains == 1);
}

TEST_CASE("review R2 recovery and traversal never return a callback-retired target") {
  enum class Change {
    REMOVE,
    HIDE,
    DISABLE
  };

  enum class Path {
    TRAVERSE,
    FOCUSED,
    KEY,
    TEXT
  };

  constexpr std::array CHANGES{Change::REMOVE, Change::HIDE, Change::DISABLE};
  constexpr std::array PATHS{Path::TRAVERSE, Path::FOCUSED, Path::KEY, Path::TEXT};

  for (auto change : CHANGES) {
    for (auto path : PATHS) {
      const bool recovery = path != Path::TRAVERSE;

      for (bool onGain : {false, true}) {
        CAPTURE(change);
        CAPTURE(recovery);
        CAPTURE(path);
        CAPTURE(onGain);

        auto app = take(Application::create());
        const auto root = take(app->tree().root());
        const auto first = take(app->tree().emplace<Widget>(root));
        const auto second = take(app->tree().emplace<Widget>(root));
        bool armed = false;
        int inputCalls = 0;
        const auto mutate = [&] {
          if (change == Change::REMOVE) {
            take(app->tree().remove(second));
          } else if (change == Change::HIDE) {
            take(app->tree().setVisible(second, false));
          } else {
            take(app->tree().setEnabled(second, false));
          }
        };
        auto input = [&](Widget &, const RoutedEvent &) {
          ++inputCalls;

          return EventReply::IGNORED;
        };
        auto firstBinding = take(app->router().bind(first, input, [&](Widget &, bool focused) {
          if (armed && !focused && !onGain) {
            mutate();
          }

          return std::expected<void, TreeError>{};
        }));
        auto secondBinding = take(app->router().bind(second, input, [&](Widget &, bool focused) {
          if (armed && focused && onGain) {
            mutate();
          }

          return std::expected<void, TreeError>{};
        }));
        take(app->router().setFocus(second));
        take(app->router().setFocus(first));
        armed = true;

        if (recovery) {
          take(app->tree().setVisible(first, false));
        }

        if (path == Path::TRAVERSE || path == Path::FOCUSED) {
          const auto selected = recovery ? app->router().focused() : app->router().traverse();

          REQUIRE(selected);
          CHECK(selected->id().session == 0);
        } else {
          const auto delivered = path == Path::KEY ? app->router().dispatch(KeyEvent{InputKey::LEFT})
                                                   : app->router().dispatch(TextInputEvent{U'x'});

          REQUIRE(delivered);
          CHECK(delivered->status == DispatchStatus::NO_TARGET);
          CHECK(inputCalls == 0);
        }
      }
    }
  }
}

TEST_CASE("review R2 mouse suppresses a hit removed by focus loss without rerouting") {
  for (bool removeAncestor : {false, true}) {
    CAPTURE(removeAncestor);
    auto app = take(Application::create());
    const auto root = take(app->tree().root());
    const auto first = take(app->tree().emplace<Widget>(root));
    const auto ancestor = take(app->tree().emplace<Widget>(root));
    const auto second = take(app->tree().emplace<Widget>(removeAncestor ? ancestor : root));
    int inputCalls = 0;
    auto input = [&](Widget &, const RoutedEvent &) {
      ++inputCalls;

      return EventReply::HANDLED;
    };
    auto firstBinding = take(app->router().bind(first, input, [&](Widget &, bool focused) {
      if (!focused) {
        take(app->tree().remove(removeAncestor ? ancestor : second));
      }

      return std::expected<void, TreeError>{};
    }));
    auto secondBinding = take(app->router().bind(second, input));
    take(app->router().setFocus(first));
    const auto viewport = take(CellRect::from({0, 0}, {20, 8}));
    take(app->tree().arrange(root, viewport, viewport));
    take(app->tree().arrange(first, take(CellRect::from({0, 0}, {4, 1})), viewport));
    take(app->tree().arrange(ancestor, take(CellRect::from({5, 0}, {4, 1})), viewport));
    take(app->tree().arrange(second, take(CellRect::from({removeAncestor ? 0 : 5, 0}, {4, 1})), viewport));
    CHECK(take(app->router().hitTest({5, 0})) == second);

    MouseInputEvent click;
    click.button = MouseButton::LEFT;
    click.action = MouseAction::PRESS;
    click.column = 5;
    click.row = 0;
    const auto delivered = app->router().dispatch(click);

    REQUIRE(delivered);
    CHECK(delivered->status == DispatchStatus::NO_TARGET);
    CHECK(inputCalls == 0);
  }
}

TEST_CASE("review R3 recovery blur failure reports once with its editor and trigger") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator failed");
  };
  Fixture fixture(std::move(config));
  int notifications = 0;
  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  take((fixture.edit.text() = "candidate").result());
  take(fixture.app->tree().setVisible(fixture.edit.node(), false));
  const auto prepared = fixture.scene->prepare(*fixture.app, {40, 10});

  REQUIRE_FALSE(prepared);
  CHECK(prepared.error() == TreeError::CALLBACK_FAILED);
  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(notifications == 1);
  CHECK(diagnostics.errors[0].error.node == fixture.edit.node().id());
  REQUIRE(diagnostics.errors[0].contexts.size() == 1);
  CHECK(diagnostics.errors[0].contexts[0].operation == "lineEdit.blur");
  CHECK(diagnostics.errors[0].contexts[0].expectedType == "LineEdit");
  CHECK(diagnostics.errors[0].error.phase == ManagedPhase::INPUT);
  CHECK(fixture.outcomes.back().trigger == LineEditTrigger::BLUR);
}

TEST_CASE("review R3 input and fluent failures retain their entry path") {
  for (bool fluent : {false, true}) {
    CAPTURE(fluent);
    ui::LineEditConfig config;
    config.validate = [](std::string_view) -> bool {
      throw std::runtime_error("validator failed");
    };
    Fixture fixture(std::move(config));

    if (fluent) {
      take((fixture.edit.text() = "candidate").result());
    } else {
      take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
    }

    const auto diagnostics = take(fixture.scene->diagnostics());

    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(diagnostics.errors[0].error.node == fixture.edit.node().id());
    CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) == TreeError::CALLBACK_FAILED);
    CHECK(diagnostics.errors[0].error.phase == (fluent ? ManagedPhase::TEXT : ManagedPhase::INPUT));
    REQUIRE(diagnostics.errors[0].contexts.size() == 1);
    CHECK(diagnostics.errors[0].contexts[0].operation == (fluent ? "lineEdit.fluent" : "lineEdit.input"));
    CHECK(fixture.outcomes.back().trigger == (fluent ? LineEditTrigger::FLUENT : LineEditTrigger::INPUT));
  }
}

TEST_CASE("review R3 a separate prepare failure with the same cause is not suppressed") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator failed");
  };
  Fixture fixture(std::move(config));
  take((fixture.edit.text() = "candidate").result());
  const auto blurred = fixture.app->router().setFocus(fixture.other.node());

  REQUIRE_FALSE(blurred);
  CHECK(blurred.error() == TreeError::CALLBACK_FAILED);
  take(fixture.scene->onPrepare(fixture.other.node(), [](Button &, Node<Widget>) -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }));
  const auto prepared = fixture.scene->prepare(*fixture.app, {40, 10});

  REQUIRE_FALSE(prepared);
  CHECK(prepared.error() == TreeError::CALLBACK_FAILED);
  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 2);
  CHECK(diagnostics.errors[0].error.node == fixture.edit.node().id());
  CHECK(diagnostics.errors[1].error.node == fixture.other.node().id());
  CHECK(diagnostics.errors[1].error.phase == ManagedPhase::PREPARE);
}

TEST_CASE("review R3 result callback failure cannot suppress an independent gain failure") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = take(scene->state(std::string("base")));
  ui::Ref<LineEdit> edit;
  ui::LineEditConfig config;

  config.options.editMode = EditMode::ON_COMPLETION;
  config.result = [](LineEditResult receipt) -> std::expected<void, TreeError> {
    return receipt.status == LineEditStatus::COMMITTED ? std::unexpected(TreeError::NO_MEMORY)
                                                       : std::expected<void, TreeError>{};
  };
  take(ui::mount(*scene, ui::lineEdit(model, std::move(config)).reference(edit)));

  const auto other = take(app->tree().emplace<Widget>(scene->root()));
  bool armed = false;
  int gains = 0;
  int notifications = 0;
  auto otherBinding = take(app->router().bind(
    other,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool focused) -> std::expected<void, TreeError> {
      if (armed && focused) {
        ++gains;

        return std::unexpected(TreeError::NO_MEMORY);
      }

      return {};
    }));
  take(scene->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  take(app->router().setFocus(other));
  take(app->router().setFocus(edit.node()));
  take((edit.text() = "candidate").result());
  take(app->tree().setVisible(edit.node(), false));
  armed = true;

  const auto prepared = scene->prepare(*app, {40, 10});

  REQUIRE_FALSE(prepared);
  CHECK(prepared.error() == TreeError::NO_MEMORY);
  CHECK(gains == 1);
  CHECK(take(model.snapshot())->value == "candidate");

  const auto diagnostics = take(scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 2);
  CHECK(notifications == 2);
  CHECK(diagnostics.errors[0].error.node == edit.node().id());
  CHECK(diagnostics.errors[0].error.phase == ManagedPhase::INPUT);
  CHECK(diagnostics.errors[1].error.node == scene->root().id());
  CHECK(diagnostics.errors[1].error.phase == ManagedPhase::PREPARE);
  CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) == TreeError::NO_MEMORY);
  CHECK(std::get<TreeError>(diagnostics.errors[1].error.cause) == TreeError::NO_MEMORY);
}

TEST_CASE("review R3 blur validator may destroy the scene before failed receipt delivery") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = take(scene->state(std::string("base")));
  ui::Ref<LineEdit> edit;
  std::optional<LineEditResult> outcome;
  ui::LineEditConfig config;

  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [&](std::string_view) {
    scene.reset();

    return true;
  };
  config.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
    outcome = std::move(receipt);

    return {};
  };
  take(ui::mount(*scene, ui::lineEdit(model, std::move(config)).reference(edit)));

  const auto root = take(app->tree().root());
  const auto other = take(app->tree().emplace<Widget>(root));
  auto otherBinding = take(app->router().bind(other, [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  }));
  take(app->router().setFocus(edit.node()));
  take((edit.text() = "candidate").result());
  outcome.reset();

  const auto blurred = app->router().setFocus(other);

  CHECK_FALSE(scene);
  REQUIRE_FALSE(blurred);
  CHECK(blurred.error() == TreeError::STALE_NODE);
  REQUIRE(outcome);
  CHECK(outcome->status == LineEditStatus::FAILED);
  CHECK(outcome->trigger == LineEditTrigger::BLUR);
  CHECK(outcome->error == TreeError::STALE_NODE);
  CHECK_FALSE(outcome->commit);
  CHECK(take(app->router().focused()) == other);
}

TEST_CASE("focus-return scopes preserve mode-specific drafts and blur publication") {
  constexpr std::array MODES{EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT};
  for (auto mode : MODES) {
    CAPTURE(mode);
    ui::LineEditConfig config;
    config.options.editMode = mode;
    Fixture fixture(std::move(config));
    take(fixture.scene->focusScope(fixture.other.node()));
    take((fixture.edit.text() = "candidate").result());
    take(fixture.other.enterFocusScope());
    CHECK(fixture.draft() == "candidate");
    CHECK(fixture.value() == (mode == EditMode::EXPLICIT ? "base" : "candidate"));
    take(fixture.other.leaveFocusScope());
    CHECK(take(fixture.app->router().focused()) == fixture.edit.node());
    CHECK(fixture.draft() == "candidate");
  }
}

TEST_CASE("focus-return entry retains dirty candidate and record after validator failure") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator failed");
  };
  Fixture fixture(std::move(config));
  take(fixture.scene->focusScope(fixture.other.node()));
  take((fixture.edit.text() = "candidate").result());
  const auto entered = fixture.other.enterFocusScope();
  REQUIRE_FALSE(entered);
  CHECK(std::get<TreeError>(entered.error().cause) == TreeError::CALLBACK_FAILED);
  CHECK(take(fixture.app->router().focused()) == fixture.other.node());
  CHECK(fixture.draft() == "candidate");
  CHECK(fixture.value() == "base");
  take(fixture.other.leaveFocusScope());
  CHECK(take(fixture.app->router().focused()) == fixture.edit.node());
  CHECK(fixture.draft() == "candidate");
  CHECK(fixture.value() == "base");
}

TEST_CASE("externally hidden focus-return root recovers without discarding an explicit draft") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  Fixture fixture(std::move(config));
  take(fixture.scene->focusScope(fixture.edit.node()));
  take(fixture.other.focus());
  take(fixture.edit.enterFocusScope());
  take((fixture.edit.text() = "survives").result());
  take(fixture.app->tree().setVisible(fixture.edit.node(), false));
  CHECK(take(fixture.app->router().focused()) == fixture.other.node());
  CHECK(fixture.draft() == "survives");
  CHECK(fixture.value() == "base");
  take(fixture.app->tree().setVisible(fixture.edit.node(), true));
  take(fixture.edit.focus());
  CHECK(fixture.draft() == "survives");
}

TEST_CASE("automatic scope return publishes once when hidden editor blur validation throws") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator failed");
  };
  Fixture fixture(std::move(config));
  take(fixture.scene->focusScope(fixture.edit.node()));
  take(fixture.other.focus());
  take(fixture.edit.enterFocusScope());
  take((fixture.edit.text() = "candidate").result());
  take(fixture.app->tree().setVisible(fixture.edit.node(), false));
  const auto returned = fixture.app->router().focused();
  REQUIRE_FALSE(returned);
  CHECK(returned.error() == TreeError::CALLBACK_FAILED);
  const auto count = fixture.outcomes.size();
  CHECK(take(fixture.app->router().focused()) == fixture.other.node());
  CHECK(fixture.outcomes.size() == count);
  CHECK(fixture.draft() == "candidate");
  CHECK(fixture.value() == "base");
}

TEST_CASE("focus-return entry preserves completion publication despite rejected notification") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  Fixture fixture(std::move(config), options);
  take(fixture.scene->focusScope(fixture.other.node()));
  take((fixture.edit.text() = "first").result());
  take(fixture.other.enterFocusScope());
  take(fixture.other.leaveFocusScope());
  take((fixture.edit.text() = "second").result());
  take(fixture.other.enterFocusScope());
  REQUIRE(fixture.outcomes.back().commit);
  CHECK(fixture.outcomes.back().commit->changed);
  CHECK(fixture.outcomes.back().commit->rejected == 1);
  CHECK(fixture.value() == "second");
  CHECK(fixture.draft() == "second");
  take(fixture.other.leaveFocusScope());
}

TEST_CASE("focus request propagates one exact reported blur failure") {
  enum class Request {
    FOCUS,
    ENTER,
    LEAVE
  };
  constexpr std::array REQUESTS{Request::FOCUS, Request::ENTER, Request::LEAVE};
  for (auto request : REQUESTS) {
    CAPTURE(request);
    ui::LineEditConfig config;
    config.options.editMode = EditMode::ON_COMPLETION;
    config.validate = [](std::string_view) -> bool {
      throw std::runtime_error("validator failed");
    };
    Fixture fixture(std::move(config));
    if (request == Request::LEAVE) {
      take(fixture.scene->focusScope(fixture.edit.node()));
      take(fixture.other.focus());
      take(fixture.edit.enterFocusScope());
    } else if (request == Request::ENTER) {
      take(fixture.scene->focusScope(fixture.other.node()));
    }
    take((fixture.edit.text() = "candidate").result());
    int notifications{};
    take(fixture.scene->onError([&](const ManagedDiagnostic &) {
      ++notifications;
    }));
    const auto result = request == Request::FOCUS   ? fixture.other.focus()
                        : request == Request::ENTER ? fixture.other.enterFocusScope()
                                                    : fixture.edit.leaveFocusScope();
    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::CALLBACK_FAILED);
    CHECK(result.error().node == fixture.edit.node().id());
    const auto diagnostics = take(fixture.scene->diagnostics());
    CHECK(diagnostics.errors.size() == 1);
    CHECK(notifications == 1);
    REQUIRE_FALSE(diagnostics.errors.empty());
    CHECK(diagnostics.errors[0].error.node == fixture.edit.node().id());
    REQUIRE(diagnostics.errors[0].contexts.size() == 1);
    CHECK(diagnostics.errors[0].contexts[0].operation == "lineEdit.blur");
    CHECK(fixture.draft() == "candidate");
    CHECK(fixture.value() == "base");
    if (request == Request::ENTER) {
      take(fixture.other.leaveFocusScope());
    }
    if (request == Request::LEAVE) {
      CHECK_FALSE(fixture.edit.leaveFocusScope());
    }
  }
}

TEST_CASE("focus entry keeps independent same-cause result and gain diagnostics") {
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.result = [](LineEditResult receipt) -> std::expected<void, TreeError> {
    return receipt.status == LineEditStatus::COMMITTED ? std::unexpected(TreeError::NO_MEMORY)
                                                       : std::expected<void, TreeError>{};
  };
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = take(scene->state(std::string("base")));
  ui::Ref<LineEdit> edit;
  ui::Ref<Column> destination;
  take(ui::mount(
    *scene, ui::column(ui::lineEdit(model, std::move(config)).reference(edit), ui::column(destination).focusScope())));
  auto binding = take(app->router().bind(
    destination.node(),
    [](Column &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [](Column &, bool gained) -> std::expected<void, TreeError> {
      return gained ? std::unexpected(TreeError::NO_MEMORY) : std::expected<void, TreeError>{};
    }));
  take(edit.focus());
  take((edit.text() = "candidate").result());
  int notifications{};
  take(scene->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  const auto entered = destination.enterFocusScope();
  REQUIRE_FALSE(entered);
  CHECK(std::get<TreeError>(entered.error().cause) == TreeError::NO_MEMORY);
  const auto diagnostics = take(scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 2);
  CHECK(notifications == 2);
  CHECK(diagnostics.errors[0].error.node == edit.node().id());
  CHECK(diagnostics.errors[1].error.node == destination.node().id());
  CHECK(take(model.snapshot())->value == "candidate");
}

TEST_CASE("checked focus-return request survives scene destruction by its blur validator") {
  for (bool leaving : {false, true}) {
    CAPTURE(leaving);
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->state(std::string("base")));
    ui::Ref<LineEdit> edit;
    ui::Ref<Button> other;
    ui::LineEditConfig config;
    config.options.editMode = EditMode::ON_COMPLETION;
    config.validate = [&](std::string_view) {
      scene.reset();
      return true;
    };
    take(ui::mount(*scene,
                   ui::column(ui::lineEdit(model, std::move(config)).reference(edit), ui::button(other, "other", [] {
                              }))));
    if (leaving) {
      take(scene->focusScope(edit.node()));
      take(other.focus());
      take(edit.enterFocusScope());
    } else {
      take(scene->focusScope(other.node()));
      take(edit.focus());
    }
    take((edit.text() = "candidate").result());
    const auto result = leaving ? edit.leaveFocusScope() : other.enterFocusScope();
    CHECK_FALSE(scene);
    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::STALE_NODE);
    CHECK_FALSE(edit.focus());
  }
}

TEST_CASE("cross-scene focus request retains the original reported editor failure") {
  enum class Request {
    FOCUS,
    ENTER,
    LEAVE
  };
  constexpr std::array REQUESTS{Request::FOCUS, Request::ENTER, Request::LEAVE};
  for (auto request : REQUESTS) {
    CAPTURE(request);
    auto app = take(Application::create());
    auto source = take(ManagedScene::create(*app));
    auto destination = take(ManagedScene::create(*app));
    auto model = take(source->state(std::string("base")));
    ui::Ref<LineEdit> edit;
    ui::Ref<Button> target, previous;
    ui::LineEditConfig config;
    config.options.editMode = EditMode::ON_COMPLETION;
    config.validate = [](std::string_view) -> bool {
      throw std::runtime_error("validator failed");
    };
    take(ui::mount(*source, ui::lineEdit(model, std::move(config)).reference(edit)));
    take(ui::mount(*destination, ui::column(ui::button(target, "target",
                                                       [] {
                                                       })
                                              .focusScope(),
                                            ui::button(previous, "previous", [] {
                                            }))));
    if (request == Request::LEAVE) {
      take(previous.focus());
      take(target.enterFocusScope());
    }
    take(edit.focus());
    take((edit.text() = "candidate").result());
    int notifications{};
    take(source->onError([&](const ManagedDiagnostic &) {
      ++notifications;
    }));
    take(destination->onError([&](const ManagedDiagnostic &) {
      ++notifications;
    }));
    const auto result = request == Request::FOCUS   ? target.focus()
                        : request == Request::ENTER ? target.enterFocusScope()
                                                    : target.leaveFocusScope();
    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::CALLBACK_FAILED);
    CHECK(result.error().node == edit.node().id());
    CHECK(take(source->diagnostics()).errors.size() == 1);
    CHECK(take(destination->diagnostics()).errors.empty());
    CHECK(notifications == 1);
    CHECK(take(edit.text().get()) == "candidate");
    CHECK(take(model.snapshot())->value == "base");
  }
}

TEST_CASE("focus receipt boundary ignores an unrelated Application blur failure") {
  Fixture unrelated([] {
    ui::LineEditConfig config;
    config.options.editMode = EditMode::ON_COMPLETION;
    config.validate = [](std::string_view) -> bool {
      throw std::runtime_error("unrelated validator");
    };
    return config;
  }());
  take((unrelated.edit.text() = "unrelated").result());
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = take(scene->state(std::string("base")));
  ui::Ref<LineEdit> edit;
  ui::Ref<Column> destination;
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [&](std::string_view) {
    const auto other = unrelated.app->router().setFocus(unrelated.other.node());
    CHECK_FALSE(other);
    return true;
  };
  take(ui::mount(
    *scene, ui::column(ui::lineEdit(model, std::move(config)).reference(edit), ui::column(destination).focusScope())));
  auto binding = take(app->router().bind(
    destination.node(),
    [](Column &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [](Column &, bool gained) -> std::expected<void, TreeError> {
      return gained ? std::unexpected(TreeError::CALLBACK_FAILED) : std::expected<void, TreeError>{};
    }));
  take(edit.focus());
  take((edit.text() = "candidate").result());
  const auto entered = destination.enterFocusScope();
  REQUIRE_FALSE(entered);
  CHECK(entered.error().node == destination.node().id());
  const auto own = take(scene->diagnostics());
  REQUIRE(own.errors.size() == 1);
  CHECK(own.errors[0].error.node == destination.node().id());
  const auto other = take(unrelated.scene->diagnostics());
  REQUIRE(other.errors.size() == 1);
  CHECK(other.errors[0].error.node == unrelated.edit.node().id());
  CHECK(take(model.snapshot())->value == "candidate");
}

TEST_CASE("cross-scene reported blur survives source destruction without another notification") {
  auto app = take(Application::create());
  auto source = take(ManagedScene::create(*app));
  auto destination = take(ManagedScene::create(*app));
  auto model = take(source->state(std::string("base")));
  ui::Ref<LineEdit> edit;
  ui::Ref<Button> target;
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator failed");
  };
  config.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
    if (receipt.trigger == LineEditTrigger::BLUR) {
      source.reset();
    }
    return {};
  };
  take(ui::mount(*source, ui::lineEdit(model, std::move(config)).reference(edit)));
  take(ui::mount(*destination, ui::button(target, "target", [] {
                               }).focusScope()));
  const auto original = edit.node().id();
  take(edit.focus());
  take((edit.text() = "candidate").result());
  int notifications{};
  take(source->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  take(destination->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  const auto entered = target.enterFocusScope();
  CHECK_FALSE(source);
  REQUIRE_FALSE(entered);
  CHECK(entered.error().node == original);
  CHECK(notifications == 1);
  CHECK(take(destination->diagnostics()).errors.empty());
  CHECK(take(app->router().focused()) == target.node());
}

TEST_CASE("automatic focus return preserves one original cross-scene blur diagnostic") {
  enum class Path {
    PREPARE,
    TAB,
    SHIFT_TAB
  };
  constexpr std::array PATHS{Path::PREPARE, Path::TAB, Path::SHIFT_TAB};
  for (auto path : PATHS) {
    for (bool destroySource : {false, true}) {
      for (bool disable : {false, true}) {
        CAPTURE(path);
        CAPTURE(destroySource);
        CAPTURE(disable);
        auto app = take(Application::create());
        auto source = take(ManagedScene::create(*app));
        auto destination = take(ManagedScene::create(*app));
        auto model = take(source->state(std::string("base")));
        ui::Ref<LineEdit> edit;
        ui::Ref<Button> other;
        ui::LineEditConfig config;
        config.options.editMode = EditMode::ON_COMPLETION;
        config.validate = [](std::string_view) -> bool {
          throw std::runtime_error("validator failed");
        };
        config.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
          if (destroySource && receipt.trigger == LineEditTrigger::BLUR) {
            source.reset();
          }
          return {};
        };
        take(ui::mount(*source, ui::lineEdit(model, std::move(config)).reference(edit).focusScope()));
        take(ui::mount(*destination, ui::button(other, "other", [] {
        })));
        const auto original = edit.node().id();
        take(other.focus());
        take(edit.enterFocusScope());
        take((edit.text() = "candidate").result());
        int notifications{};
        std::optional<ManagedError> reported;
        const auto observe = [&](const ManagedDiagnostic &diagnostic) {
          ++notifications;
          reported = diagnostic.error;
        };
        take(source->onError(observe));
        take(destination->onError(observe));
        if (disable) {
          take(app->tree().setEnabled(edit.node(), false));
        } else {
          take(app->tree().setVisible(edit.node(), false));
        }
        const auto result = [&]() -> std::expected<void, TreeError> {
          if (path == Path::PREPARE) {
            return destination->prepare(*app, {40, 10});
          }
          KeyEvent event{InputKey::TAB};
          event.modifiers.shift = path == Path::SHIFT_TAB;
          const auto dispatched = app->router().dispatch(event);
          return dispatched ? std::expected<void, TreeError>{} : std::unexpected(dispatched.error());
        }();
        REQUIRE_FALSE(result);
        CHECK(result.error() == TreeError::CALLBACK_FAILED);
        CHECK(notifications == 1);
        REQUIRE(reported);
        CHECK(reported->node == original);
        CHECK(reported->phase == ManagedPhase::INPUT);
        CHECK(std::string_view(reported->operation ? reported->operation : "") == "lineEdit.blur");
        CHECK(take(destination->diagnostics()).errors.empty());
        if (destroySource) {
          CHECK_FALSE(source);
        } else {
          REQUIRE(source);
          CHECK(take(source->diagnostics()).errors.size() == 1);
          CHECK(take(edit.text().get()) == "candidate");
          CHECK(take(model.snapshot())->value == "base");
        }
        CHECK(take(app->router().focused()) == other.node());
        CHECK(notifications == 1);
      }
    }
  }
}

TEST_CASE("automatic prepare keeps independent same-cause result and gain failures across scenes") {
  auto app = take(Application::create());
  auto source = take(ManagedScene::create(*app));
  auto destination = take(ManagedScene::create(*app));
  auto model = take(source->state(std::string("base")));
  ui::Ref<LineEdit> edit;
  ui::Ref<Column> other;
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  config.result = [](LineEditResult receipt) -> std::expected<void, TreeError> {
    return receipt.status == LineEditStatus::COMMITTED ? std::unexpected(TreeError::NO_MEMORY)
                                                       : std::expected<void, TreeError>{};
  };
  take(ui::mount(*source, ui::lineEdit(model, std::move(config)).reference(edit).focusScope()));
  take(ui::mount(*destination, ui::column(other)));
  bool armed{};
  auto binding = take(app->router().bind(
    other.node(),
    [](Column &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Column &, bool gained) -> std::expected<void, TreeError> {
      return gained && armed ? std::unexpected(TreeError::NO_MEMORY) : std::expected<void, TreeError>{};
    }));
  take(other.focus());
  take(edit.enterFocusScope());
  take((edit.text() = "candidate").result());
  take(app->tree().setVisible(edit.node(), false));
  int notifications{};
  take(source->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  take(destination->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  armed = true;
  const auto result = destination->prepare(*app, {40, 10});
  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::NO_MEMORY);
  CHECK(notifications == 2);
  const auto sourceErrors = take(source->diagnostics());
  const auto destinationErrors = take(destination->diagnostics());
  REQUIRE(sourceErrors.errors.size() == 1);
  REQUIRE(destinationErrors.errors.size() == 1);
  CHECK(sourceErrors.errors[0].error.node == edit.node().id());
  CHECK(destinationErrors.errors[0].error.node == destination->root().id());
  CHECK(destinationErrors.errors[0].error.phase == ManagedPhase::PREPARE);
  CHECK(take(model.snapshot())->value == "candidate");
}

TEST_CASE("nested Application prepare restores the outer automatic blur receipt boundary") {
  ui::LineEditConfig innerConfig;
  innerConfig.options.editMode = EditMode::ON_COMPLETION;
  innerConfig.validate = [](std::string_view) -> bool {
    throw std::runtime_error("inner validator");
  };
  Fixture inner(std::move(innerConfig));
  take(inner.scene->focusScope(inner.edit.node()));
  take(inner.other.focus());
  take(inner.edit.enterFocusScope());
  take((inner.edit.text() = "inner candidate").result());
  take(inner.app->tree().setVisible(inner.edit.node(), false));
  ui::LineEditConfig outerConfig;
  outerConfig.options.editMode = EditMode::ON_COMPLETION;
  outerConfig.validate = [&](std::string_view) -> bool {
    const auto nested = inner.scene->prepare(*inner.app, {40, 10});
    CHECK_FALSE(nested);
    throw std::runtime_error("outer validator");
  };
  Fixture outer(std::move(outerConfig));
  take(outer.scene->focusScope(outer.edit.node()));
  take(outer.other.focus());
  take(outer.edit.enterFocusScope());
  take((outer.edit.text() = "outer candidate").result());
  take(outer.app->tree().setEnabled(outer.edit.node(), false));
  const auto result = outer.scene->prepare(*outer.app, {40, 10});
  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::CALLBACK_FAILED);
  const auto innerErrors = take(inner.scene->diagnostics());
  const auto outerErrors = take(outer.scene->diagnostics());
  REQUIRE(innerErrors.errors.size() == 1);
  REQUIRE(outerErrors.errors.size() == 1);
  CHECK(innerErrors.errors[0].error.node == inner.edit.node().id());
  CHECK(outerErrors.errors[0].error.node == outer.edit.node().id());
  CHECK(take(inner.app->router().focused()) == inner.other.node());
  CHECK(take(outer.app->router().focused()) == outer.other.node());
}
