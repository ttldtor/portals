// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include "examples/check_state/group.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

namespace {
using namespace portals;
constexpr std::array EMPTY = std::array<CheckState, 0>{};
constexpr std::array ALL_CHECKED{CheckState::CHECKED, CheckState::CHECKED};
constexpr std::array ALL_UNCHECKED{CheckState::UNCHECKED, CheckState::UNCHECKED};
constexpr std::array PARTIAL{CheckState::CHECKED, CheckState::UNCHECKED};
constexpr std::array MIXED{CheckState::MIXED};
constexpr std::array INVALID{static_cast<CheckState>(99)};
static_assert(aggregateCheckState(EMPTY) == CheckState::UNCHECKED);
static_assert(aggregateCheckState(ALL_CHECKED) == CheckState::CHECKED);
static_assert(aggregateCheckState(ALL_UNCHECKED) == CheckState::UNCHECKED);
static_assert(aggregateCheckState(PARTIAL) == CheckState::MIXED);
static_assert(aggregateCheckState(MIXED) == CheckState::MIXED);
static_assert(aggregateCheckState(INVALID).error() == TreeError::INVALID_ARGUMENT);

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <class T, class E> T take(std::expected<T, E> value) {
  check(bool(value), "unexpected operation failure");

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  Node<Widget> root;
  std::unique_ptr<EventRouter> router;
  Node<CheckBox> node;
  std::shared_ptr<Property<CheckState>> model;

  explicit Fixture(CheckState initial = CheckState::UNCHECKED) {
    SchedulerOptions options;

    options.data_capacity = 1;
    scheduler = take(Scheduler::create(options));
    tree = take(WidgetTree::create(*scheduler));
    root = take(tree->root());
    router = take(EventRouter::create(*tree));
    node = take(tree->emplace<CheckBox>(root, take(GraphemeText::from("Flag"))));
    model = take(Property<CheckState>::create(*tree, root, initial, Invalidation::PAINT));
  }

  CheckState value() {
    CheckState state{};

    take(tree->read(node, [&](const CheckBox &box) {
      state = box.checkState();
      check(box.checked() == (state == CheckState::CHECKED), "bool projection excludes mixed");
    }));

    return state;
  }

  void activate() {
    take(router->invoke(node, {Action::ACTIVATE}));
  }
};

void transitionsAndRecovery() {
  for (const auto cycle : {CheckCycle::TWO_STATE, CheckCycle::THREE_STATE}) {
    Fixture f(CheckState::MIXED);
    int calls{};
    auto binding = take(bindCheckbox(*f.tree, *f.router, f.node, f.model,
                                     [&](CheckStateResult receipt) {
                                       check(receipt && receipt->changed, "activation supplies exact changed receipt");
                                       ++calls;
                                     },
                                     {.cycle = cycle}));

    check(f.value() == CheckState::MIXED, "initial mixed synchronizes");
    take(f.tree->consumeFrameRequest());
    take(binding.sync());
    check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "equal sync has no redraw");
    f.activate();
    check(f.value() == (cycle == CheckCycle::TWO_STATE ? CheckState::CHECKED : CheckState::UNCHECKED),
          "mixed activation follows explicit cycle");
    f.activate();
    f.activate();
    check(calls == 3, "one callback per activation despite rejected notifications");
    take(f.model->set(CheckState::MIXED));
    auto update = take(f.model->set(CheckState::UNCHECKED));

    check(update.rejected == 1, "bounded queue exposes rejection");
    take(binding.sync());
    take(f.scheduler->dispatch());
    check(f.value() == CheckState::UNCHECKED && calls == 3, "latest snapshot beats stale delivery");
    take(f.tree->setEnabled(f.node, false));
    f.activate();
    check(calls == 3, "disabled routing does not activate");
    take(f.tree->setEnabled(f.node, true));
    take(f.tree->setVisible(f.node, false));
    f.activate();
    check(calls == 3, "hidden routing does not activate");
    bool wrongThread{};
    std::thread worker([&] {
      const auto status = binding.sync();
      wrongThread = !status && status.error() == TreeError::WRONG_THREAD;
    });

    worker.join();
    check(wrongThread, "typed binding stays UI affine");
  }
}

void inputAdmission() {
  Fixture f;
  int calls{};
  auto binding = take(bindCheckbox(*f.tree, *f.router, f.node, f.model,
                                   [&](CheckStateResult result) {
                                     take(std::move(result));
                                     ++calls;
                                   },
                                   {.cycle = CheckCycle::THREE_STATE}));
  const auto viewport = take(CellRect::from({}, {12, 1}));

  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, viewport, viewport));
    take(f.tree->arrange(f.node, viewport, viewport));
  }));
  take(f.router->setFocus(f.node));
  take(f.router->dispatch(TextInputEvent{U' ', {}}));
  check(f.value() == CheckState::CHECKED, "Space uses selected cycle");
  take(f.router->dispatch(MouseInputEvent{1, 0, MouseButton::LEFT, MouseAction::PRESS, {}}));
  check(f.value() == CheckState::MIXED, "mouse press uses same selected cycle");
  take(f.router->dispatch(MouseInputEvent{1, 0, MouseButton::LEFT, MouseAction::RELEASE, {}}));
  take(f.router->dispatch(MouseInputEvent{1, 0, MouseButton::LEFT, MouseAction::MOVE, {}}));
  take(f.router->dispatch(TextInputEvent{U' ', {.alt = true}}));
  check(calls == 2 && f.value() == CheckState::MIXED, "release/motion/modified input have no extra events");
}

void invalidAndBoolRebind() {
  Fixture f(CheckState::MIXED);
  std::optional<CheckStateBinding> binding;
  int errors{};

  binding.emplace(take(bindCheckbox(*f.tree, *f.router, f.node, f.model, [&](CheckStateResult receipt) {
    check(!receipt && receipt.error() == TreeError::INVALID_ARGUMENT, "invalid model input reports typed failure");
    ++errors;
  })));
  take(f.model->set(static_cast<CheckState>(99)));
  auto failed = binding->sync();

  check(!failed && failed.error() == TreeError::INVALID_ARGUMENT && f.value() == CheckState::MIXED,
        "invalid committed model cannot alter glyph");
  f.activate();
  check(errors == 1 && take(f.model->snapshot())->value == static_cast<CheckState>(99),
        "invalid raw commit is retained without silent normalization");
  binding.reset();
  auto boolean =
    std::shared_ptr<Property<bool>>(take(Property<bool>::create(*f.tree, f.root, false, Invalidation::PAINT)));
  auto legacy = take(bindCheckbox(*f.tree, *f.router, f.node, boolean, [](CheckBoxResult) {
  }));

  check(f.value() == CheckState::UNCHECKED, "bool false resets former mixed presentation");
  f.activate();
  check(f.value() == CheckState::CHECKED, "legacy bool cycle remains intact");
}

void lifetimeAndErrors() {
  Fixture f;
  std::optional<CheckStateBinding> binding;
  int calls{};

  binding.emplace(take(bindCheckbox(*f.tree, *f.router, f.node, f.model, [&](CheckStateResult receipt) {
    check(receipt && receipt->snapshot->value == CheckState::CHECKED, "self removal retains commit receipt");
    ++calls;
    take(f.tree->remove(f.node));
    binding.reset();
  })));
  f.activate();
  check(calls == 1 && take(f.model->snapshot())->value == CheckState::CHECKED, "admitted capture survives teardown");

  Fixture other;
  auto invalid = bindCheckbox(*other.tree, *other.router, other.node, other.model,
                              [](CheckStateResult) {
                              },
                              {.cycle = static_cast<CheckCycle>(99)});
  check(!invalid && invalid.error() == TreeError::INVALID_ARGUMENT, "invalid policy rejects registration");
  auto valid = take(bindCheckbox(*other.tree, *other.router, other.node, other.model, [](CheckStateResult) {
  }));
  auto moved = std::move(valid);
  auto empty = valid.sync();

  check(!empty && empty.error() == TreeError::INVALID_ARGUMENT, "moved-from binding is empty");
  take(moved.sync());
}

void partialFailureAndReentry() {
  Fixture f;
  const auto removed = take(f.tree->emplace<Widget>(f.root));
  auto stale = std::shared_ptr<Property<CheckState>>(
    take(Property<CheckState>::create(*f.tree, removed, CheckState::UNCHECKED, Invalidation::PAINT)));

  take(f.tree->remove(removed));
  const std::array models{stale, f.model};
  auto observer = take(f.model->observe(f.node, [](CheckBox &, auto) {
  }));

  take(f.model->set(CheckState::MIXED));
  const auto result = take(checkbox_example::updateSelection(models, CheckState::CHECKED));
  std::ostringstream report;

  checkbox_example::reportGroupUpdate(result, report);
  check(result.committed == 1 && result.failed == 1 && result.rejectedNotifications == 1 &&
          report.str().find("committed=1 unchanged=0 failed=1 rejected-notifications=1") != std::string::npos,
        "group caller exposes successful commits, failed children and lost notifications separately");

  check(!result.children[0] && result.children[0].error() == TreeError::STALE_NODE,
        "group preserves failed child receipt");
  check(result.children[1] && result.children[1]->changed && result.aggregate == CheckState::MIXED,
        "group continues after failure and recomputes partial authoritative selection");
  auto binding = take(bindCheckbox(*f.tree, *f.router, f.node, f.model, [&](CheckStateResult receipt) {
    check(receipt && receipt->snapshot->value == CheckState::UNCHECKED, "callback keeps original receipt");
    take(f.model->set(CheckState::MIXED));
  }));

  f.activate();
  take(binding.sync());
  check(f.value() == CheckState::MIXED, "reentrant model mutation wins at next sync");
}

struct GlyphSink : PaintContext {
  std::string prefix;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *text = std::get_if<TextCommand>(&command); text && prefix.empty()) {
        prefix = text->text.bytes();
      }
    }

    return {};
  }
};

void mixedGlyph() {
  Fixture f(CheckState::MIXED);
  auto binding = take(bindCheckbox(*f.tree, *f.router, f.node, f.model, [](CheckStateResult) {
  }));
  const auto viewport = take(CellRect::from({}, {12, 1}));

  for (const auto enabled : {true, false}) {
    take(f.tree->setEnabled(f.node, enabled));
    take(syncControl(*f.tree, f.node, {}));

    for (const auto size : {CellSize{12, 1}, CellSize{1, 1}, CellSize{0, 0}}) {
      GlyphSink sink;

      take(f.tree->frame([&] {
        take(f.tree->arrange(f.root, viewport, viewport));
        take(f.tree->arrange(f.node, take(CellRect::from({}, size)), viewport));
        take(f.tree->paint(f.node, sink));
      }));
      check(size.width == 0 || size.height == 0 ? sink.prefix.empty() : sink.prefix == "[-] ",
            "mixed glyph is distinct; zero extent emits no paint");
    }
  }

  WidthPolicy policy;

  policy.revision = 4;
  take(setControlText(*f.tree, f.node, take(GraphemeText::from("Unicode \xe7\x95\x8c", policy))));
  GlyphSink changed;

  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, viewport, viewport));
    take(f.tree->arrange(f.node, viewport, viewport));
    take(f.tree->paint(f.node, changed));
  }));
  check(changed.prefix == "[-] ", "text/width-policy replacement preserves mixed decoration");
}

void failedCommitAndCallback() {
  Fixture f;
  const auto source = take(f.tree->emplace<Widget>(f.root));
  auto model = std::shared_ptr<Property<CheckState>>(
    take(Property<CheckState>::create(*f.tree, source, CheckState::MIXED, Invalidation::PAINT)));
  int failures{};
  std::optional<CheckStateBinding> binding;

  binding.emplace(take(bindCheckbox(*f.tree, *f.router, f.node, model, [&](CheckStateResult result) {
    check(!result && result.error() == TreeError::STALE_NODE, "precommit failure is explicit");
    ++failures;
  })));
  take(f.tree->remove(source));
  f.activate();
  check(failures == 1 && f.value() == CheckState::MIXED && take(model->snapshot())->value == CheckState::MIXED,
        "failed commit changes neither model nor presentation");
  binding.reset();
  binding.emplace(take(bindCheckbox(*f.tree, *f.router, f.node, f.model, [](CheckStateResult) {
    throw 7;
  })));
  const auto failed = f.router->invoke(f.node, {Action::ACTIVATE});

  check(!failed && failed.error() == TreeError::CALLBACK_FAILED &&
          take(f.model->snapshot())->value == CheckState::CHECKED && f.value() == CheckState::CHECKED,
        "callback exception retains committed state and typed routing failure");
}

void managedRecovery() {
  ApplicationOptions options;

  options.scheduler.data_capacity = 1;

  auto app = take(Application::create(options));
  auto scene = take(ManagedScene::create(*app));
  const auto model = take(scene->state(CheckState::UNCHECKED));
  ui::Ref<CheckBox> reference;
  int calls{};

  take(
    ui::mount(*scene, ui::checkbox(reference, "Shared", model, [&](CheckStateResult) -> std::expected<void, TreeError> {
      ++calls;

      return {};
    })));
  take(scene->prepare(*app, {20, 2}));
  take(model.set(CheckState::CHECKED));
  const auto mixed = take(model.set(CheckState::MIXED));

  check(mixed.rejected == 1, "managed test establishes rejected notification");
  take(app->tree().read(reference.node(), [](const CheckBox &box) {
    check(box.checkState() == CheckState::UNCHECKED, "undelivered model value has not changed presentation");
  }));
  take(scene->prepare(*app, {20, 2}));
  take(app->tree().read(reference.node(), [](const CheckBox &box) {
    check(box.checkState() == CheckState::MIXED, "managed before-frame sync recovers without manual binding sync");
  }));
  take(app->scheduler().dispatch());
  take(scene->prepare(*app, {20, 2}));
  check(calls == 0, "managed recovery and old deliveries produce no business events");
}

template <class Options>
concept InitialOptionsAccepted =
  requires(Options &&options) { ui::checkbox("Options", CheckState::MIXED, std::forward<Options>(options)); };

template <class Options>
concept ManagedOptionsAccepted = requires(ManagedState<CheckState> state, Options &&options) {
  ui::checkbox("Options", state, std::forward<Options>(options));
};

struct ConvertibleOptions {
  operator CheckBoxOptions() const;
};

static_assert(!InitialOptionsAccepted<ConvertibleOptions>);
static_assert(!ManagedOptionsAccepted<ConvertibleOptions>);
static_assert(InitialOptionsAccepted<CheckBoxOptions>);
static_assert(InitialOptionsAccepted<CheckBoxOptions &>);
static_assert(InitialOptionsAccepted<const CheckBoxOptions &>);
static_assert(ManagedOptionsAccepted<CheckBoxOptions>);
static_assert(ManagedOptionsAccepted<const CheckBoxOptions &>);
static_assert(!InitialOptionsAccepted<ControlStyle>);
static_assert(!InitialOptionsAccepted<SizePolicy>);
static_assert(!InitialOptionsAccepted<bool>);
static_assert(!ManagedOptionsAccepted<ControlStyle>);
static_assert(!ManagedOptionsAccepted<SizePolicy>);
static_assert(!ManagedOptionsAccepted<bool>);
static_assert(std::same_as<decltype(ui::checkbox("Legacy typed", CheckState::MIXED, {})), ui::Description<CheckBox>>);
static_assert(std::same_as<decltype(ui::checkbox("Legacy bool", false, {})), ui::Description<CheckBox>>);
static_assert(requires(ManagedState<CheckState> state) { ui::checkbox("Legacy shared", state, {}); });

void optionsFirstFactories() {
  ui::Ref<CheckBox> initialRef;
  CheckBoxOptions options{.cycle = CheckCycle::THREE_STATE};
  int initialCalls{};
  auto recipe =
    ui::checkbox("Initial", CheckState::MIXED, options, [&](CheckStateResult result) -> std::expected<void, TreeError> {
      check(result && result->snapshot->value == CheckState::UNCHECKED, "options-first initial receipt is exact");
      ++initialCalls;

      return {};
    }).reference(initialRef);

  options.cycle = CheckCycle::TWO_STATE;

  auto lazy = ui::scene(std::move(recipe));

  take(lazy.prepareChecked());
  auto &owner = take(lazy.managed()).get();
  auto &app = ui::detail::SceneAccess::application(owner);

  take(app.router().invoke(initialRef.node(), {Action::ACTIVATE}));
  check(initialCalls == 1, "initial factory copied lvalue options before later mutation");
  take(lazy.close());

  auto sharedApp = take(Application::create());
  auto sharedScene = take(ManagedScene::create(*sharedApp));
  auto state = take(sharedScene->state(CheckState::MIXED));
  ui::Ref<CheckBox> sharedRef;
  int sharedCalls{};
  const CheckBoxOptions fixed{.cycle = CheckCycle::THREE_STATE};

  take(ui::mount(*sharedScene,
                 ui::checkbox("Shared", state, fixed, [&](CheckStateResult result) -> std::expected<void, TreeError> {
                   check(result && result->snapshot->value == CheckState::UNCHECKED,
                         "options-first shared receipt is exact");
                   ++sharedCalls;

                   return {};
                 }).reference(sharedRef)));
  take(sharedApp->router().invoke(sharedRef.node(), {Action::ACTIVATE}));
  check(sharedCalls == 1 && take(state.snapshot())->value == CheckState::UNCHECKED,
        "managed options-first const lvalue preserves three-state policy");

  ui::Ref<CheckBox> temporaryRef;

  take(ui::mount(
    *sharedScene,
    ui::checkbox("Temporary", state, CheckBoxOptions{.cycle = CheckCycle::THREE_STATE}).reference(temporaryRef)));
  take(sharedApp->router().invoke(temporaryRef.node(), {Action::ACTIVATE}));
  check(take(state.snapshot())->value == CheckState::CHECKED, "managed factory owns temporary options");
  take(sharedApp->router().invoke(temporaryRef.node(), {Action::ACTIVATE}));
  check(take(state.snapshot())->value == CheckState::MIXED,
        "temporary three-state options remain alive after factory entry");
}

void declarative() {
  ui::Ref<CheckBox> reference;
  int calls{};
  auto scene = ui::scene(
    ui::checkbox("Selection", CheckState::MIXED, [&](CheckStateResult result) -> std::expected<void, TreeError> {
      check(result && result->snapshot->value == CheckState::CHECKED, "declarative default mixed activates checked");
      ++calls;

      return {};
    }).reference(reference));
  take(scene.prepareChecked());
  auto &owner = take(scene.managed()).get();
  auto &app = ui::detail::SceneAccess::application(owner);

  take(app.router().invoke(reference.node(), {Action::ACTIVATE}));
  check(calls == 1, "declarative owns typed binding");
  take(scene.close());
  const auto closed = reference.focus();

  check(!closed && std::get<TreeError>(closed.error().cause) == TreeError::CLOSED,
        "scene cleanup closes reference access with typed CLOSED");
}

void declarativeChange() {
  ui::Ref<CheckBox> reference;
  std::array<CheckState, 2> values{};
  std::size_t calls{};
  auto scene =
    ui::scene(ui::checkbox(reference, "Selection").threeState()) | ui::onChange(reference, [&](CheckState value) {
      check(calls < values.size(), "one change callback per activation");
      values[calls++] = value;
    });
  take(scene.prepareChecked());
  auto &owner = take(scene.managed()).get();
  auto &app = ui::detail::SceneAccess::application(owner);
  take(app.router().invoke(reference.node(), {Action::ACTIVATE}));
  take(app.router().invoke(reference.node(), {Action::ACTIVATE}));
  check(calls == 2 && values[0] == CheckState::CHECKED && values[1] == CheckState::MIXED,
        "installed change adapter observes the three-state cycle");
  take(scene.close());
}
} // namespace

int main() {
  transitionsAndRecovery();
  inputAdmission();
  invalidAndBoolRebind();
  lifetimeAndErrors();
  declarative();
  declarativeChange();
  optionsFirstFactories();
  managedRecovery();
  failedCommitAndCallback();
  partialFailureAndReentry();
  mixedGlyph();

  return 0;
}
