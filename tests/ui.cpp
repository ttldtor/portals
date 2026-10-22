// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace {
using namespace portals;

template <class T>
concept CanNest = requires(T &&value) { ui::column(std::forward<T>(value)); };

static_assert(CanNest<ui::Description<Label>>);
static_assert(!CanNest<ui::Description<Label> &>);
static_assert(!std::copy_constructible<ui::Description<Label>>);

void check(bool condition, std::string_view message, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(message) + " at line " + std::to_string(where.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  check(result.has_value(), "unexpected API failure", where);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool treeCause(const ManagedError &error, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == cause;
}

struct Fixture {
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(std::size_t capacity = 32, WidthPolicy width = {}) {
    ApplicationOptions options;

    options.widthPolicy = width;
    app = take(Application::create(options));
    scene = take(ManagedScene::create(*app, {.maxNodes = capacity}));
  }

  void prepare() {
    take(scene->prepare(*app, {40, 10}));
  }
};

struct Custom final : Widget {
  std::string caption;
  int events{};
  int prepares{};

  explicit Custom(std::string text) : caption(std::move(text)) {
  }
};

struct MountFailure final : Widget {
  void mount() override {
    throw std::runtime_error("expected mount failure");
  }
};

struct Reentrant final : Widget {
  ManagedScene &scene;
  bool &blocked;

  Reentrant(ManagedScene *owner, bool *result) : scene(*owner), blocked(*result) {
  }

  void unmount() override {
    const auto attempt = scene.emplace<Widget>(scene.root());

    blocked = !attempt && treeCause(attempt.error(), TreeError::WOULD_BLOCK);
  }
};

template <class T> struct ArgumentWidget final : Widget {
  T value;

  explicit ArgumentWidget(T argument) : value(std::move(argument)) {
  }
};

struct OverloadedArgument final : Widget {
  bool tupleConstructor{};

  explicit OverloadedArgument(std::tuple<int, int>) : tupleConstructor(true) {
  }

  OverloadedArgument(int, int) {
  }
};

struct RemovalOutcome {
  bool called{};
  bool removed{};
  bool blocked{};
};

struct RemovesSibling final : Widget {
  ManagedScene &scene;
  Node<Widget> sibling;
  RemovalOutcome &outcome;

  RemovesSibling(ManagedScene *owner, Node<Widget> target, RemovalOutcome *result)
      : scene(*owner), sibling(target), outcome(*result) {
  }

  void unmount() override {
    const auto result = scene.remove(sibling);

    outcome = {true, result.has_value(), !result && treeCause(result.error(), TreeError::WOULD_BLOCK)};
  }
};

struct CaptureLifetime {
  int &destructions;
  int visits{};

  explicit CaptureLifetime(int &counter) : destructions(counter) {
  }

  ~CaptureLifetime() {
    ++destructions;
  }
};

void customArgumentShape() {
  Fixture fixture;
  using Tuple = std::tuple<int, std::string>;
  using Pair = std::pair<int, std::string>;

  const auto tuple = take(ui::mount(*fixture.scene, ui::custom<ArgumentWidget<Tuple>>(Tuple{7, "tuple"})));
  const auto pair = take(ui::mount(*fixture.scene, ui::custom<ArgumentWidget<Pair>>(Pair{9, "pair"})));
  const auto overloaded = take(ui::mount(*fixture.scene, ui::custom<OverloadedArgument>(std::tuple{1, 2})));

  take(fixture.app->tree().read(tuple, [](const ArgumentWidget<Tuple> &widget) {
    check(widget.value == Tuple{7, "tuple"}, "one tuple remains one constructor argument");
  }));
  take(fixture.app->tree().read(pair, [](const ArgumentWidget<Pair> &widget) {
    check(widget.value == Pair{9, "pair"}, "one pair remains one constructor argument");
  }));
  take(fixture.app->tree().read(overloaded, [](const OverloadedArgument &widget) {
    check(widget.tupleConstructor, "tuple capture does not silently select an unpacked overload");
  }));
}

void setupOrderIsPreorder() {
  Fixture fixture;
  std::vector<std::string_view> calls;
  const auto mark = [&](std::string_view name) {
    return [&, name](ManagedScene &, auto) -> std::expected<void, ManagedError> {
      calls.push_back(name);

      return {};
    };
  };

  auto recipe = ui::column(ui::row(ui::label("first").setup(mark("first"))).setup(mark("row")),
                           ui::label("second").setup(mark("second")))
                  .setup(mark("root first"))
                  .setup(mark("root second"));

  take(ui::mount(*fixture.scene, std::move(recipe)));
  check(calls == std::vector<std::string_view>{"root first", "root second", "row", "first", "second"},
        "parent modifiers run in registration order before children in argument order");
}

void textLimitKeepsTypedError() {
  Fixture fixture;
  const auto existing = take(ui::mount(*fixture.scene, ui::label("existing")));
  const std::string oversized(TextLimits{}.max_input_bytes + 1, 'x');
  const auto originalError = GraphemeText::from(oversized).error();
  ui::NodeRef<Label> reference;
  const auto failed = ui::mount(*fixture.scene, ui::column(ui::label(oversized).reference(reference)));

  check(!failed && treeCause(failed.error(), TreeError::NODE_LIMIT) && failed.error().text == originalError,
        "text input limit retains the original category and offset");
  check(reference.node() == Node<Label>{} &&
          take(fixture.app->tree().inspect(fixture.scene->root())).children == std::vector<Node<Widget>>{existing},
        "text limit publishes no reference and preserves the previous subtree");
}

void storedStringsAndSingleUse() {
  Fixture fixture;
  ui::NodeRef<Label> labelRef;
  ui::NodeRef<Custom> customRef;
  ui::Description<Column> stored;

  {
    std::string caption = "owned text";
    std::string custom = "owned custom";

    LinearOptions rowOptions;

    rowOptions.gap = 2;
    stored = ui::column(ui::label(std::string_view(caption)).reference(labelRef),
                        ui::row(rowOptions, ui::custom<Custom>(std::string_view(custom)).reference(customRef)));
    caption.assign(100, 'x');
    custom.assign(100, 'y');
  }

  check(labelRef.node() == Node<Label>{}, "reference is not published by description construction");

  auto moved = std::move(stored);
  const auto empty = ui::mount(*fixture.scene, std::move(stored));

  check(!empty && treeCause(empty.error(), TreeError::INVALID_ARGUMENT), "moved-from description rejected");

  const auto root = take(ui::mount(*fixture.scene, std::move(moved)));
  const auto repeated = ui::mount(*fixture.scene, std::move(moved));

  check(!repeated && treeCause(repeated.error(), TreeError::INVALID_ARGUMENT), "consumed description rejected");
  take(fixture.app->tree().read(labelRef.node(), [](const Label &label) {
    check(label.text().bytes() == "owned text", "source string storage is not borrowed");
  }));
  take(fixture.app->tree().read(customRef.node(), [](const Custom &custom) {
    check(custom.caption == "owned custom", "custom string_view argument is materialized");
  }));

  const auto children = take(fixture.app->tree().inspect(root)).children;

  check(children.size() == 2 && children.front() == labelRef.node(), "preorder parent-child ownership");
  check(take(fixture.app->tree().inspect(children.back())).children.front() == customRef.node(),
        "mixed nested layouts");
  take(fixture.scene->remove(root));
  check(!fixture.app->tree().inspect(labelRef.node()), "ref remains observing stale identity after remove");
}

void referencesAreTransactional() {
  Fixture fixture;
  ui::NodeRef<Label> ref;
  const auto duplicate =
    ui::mount(*fixture.scene, ui::column(ui::label("first").reference(ref), ui::label("second").reference(ref)));

  check(!duplicate && treeCause(duplicate.error(), TreeError::INVALID_ARGUMENT), "duplicate slot rejected");
  check(ref.node() == Node<Label>{}, "failed mount publishes no refs");
  check(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(), "failed root removed");

  const auto original = take(ui::mount(*fixture.scene, ui::label("live").reference(ref)));
  const auto reused = ui::mount(*fixture.scene, ui::label("replacement").reference(ref));

  check(!reused && treeCause(reused.error(), TreeError::INVALID_ARGUMENT), "published slot cannot be overwritten");
  check(ref.node() == original && take(fixture.app->tree().inspect(fixture.scene->root())).children.size() == 1,
        "failed reuse preserves previous node and reference");

  ui::NodeRef<Label> reserved;
  bool nestedRejected{};
  const auto outer = take(ui::mount(
    *fixture.scene, ui::label("outer").reference(reserved).setup([&](ManagedScene &scene,
                                                                     Node<Label>) -> std::expected<void, ManagedError> {
      const auto nested = ui::mount(scene, ui::label("nested").reference(reserved));

      nestedRejected = !nested && treeCause(nested.error(), TreeError::INVALID_ARGUMENT);
      check(reserved.node() == Node<Label>{}, "reentrant mount cannot publish pending outer reference");

      return {};
    })));

  check(nestedRejected && reserved.node() == outer, "reentrant mount cannot steal another transaction's ref");

  ui::NodeRef<Label> moved;
  auto destination = std::move(moved);
  const auto invalidRef = ui::mount(*fixture.scene, ui::label("moved ref").reference(moved));

  check(!invalidRef && treeCause(invalidRef.error(), TreeError::INVALID_ARGUMENT) &&
          destination.node() == Node<Label>{},
        "moved-from reference is rejected without dereference");
}

void rollbackReclaimsBindingsAndBudget() {
  Fixture fixture(4);
  const auto existing = take(ui::mount(*fixture.scene, ui::label("existing")));
  const auto model = take(fixture.scene->state(std::string("initial")));
  ui::NodeRef<LineEdit> failedRef;
  std::weak_ptr<int> lifetime;
  bool blocked{};

  for (int attempt = 0; attempt < 3; ++attempt) {
    auto owner = std::make_shared<int>(attempt);

    lifetime = owner;

    auto description = ui::column(
      ui::lineEdit(model, {.result = [owner](LineEditResult) -> std::expected<void, TreeError> {
                     return {};
                   }})
        .reference(failedRef),
      ui::custom<Reentrant>(fixture.scene.get(), &blocked)
        .setup([](ManagedScene &, Node<Reentrant> node) -> std::expected<void, ManagedError> {
          return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_GEOMETRY, {}, {}});
        }));

    owner.reset();

    const auto failed = ui::mount(*fixture.scene, std::move(description));

    check(!failed && treeCause(failed.error(), TreeError::INVALID_GEOMETRY) &&
            failed.error().phase == ManagedPhase::BIND,
          "original typed setup error survives rollback");
    check(!failed.error().cleanup && blocked, "rollback guards unmount reentry");
    check(failedRef.node() == Node<LineEdit>{}, "partially bound editor ref not published");
    check(take(fixture.app->tree().inspect(fixture.scene->root())).children == std::vector<Node<Widget>>{existing},
          "rollback preserves only earlier subtree");
    take(fixture.app->router().focused());
    check(lifetime.expired(), "disconnected callback eventually reclaimed by existing router");
  }

  auto valid = take(ui::mount(*fixture.scene, ui::column(ui::lineEdit(model), ui::label("new"))));

  check(take(model.snapshot())->value == "initial", "state survives partial mount failures");
  fixture.prepare();
  take(fixture.scene->remove(valid));
  take(ui::mount(*fixture.scene, ui::column(ui::label("a"), ui::label("b"))));
  fixture.prepare();
}

void mountErrorsAndRemovedNodes() {
  Fixture fixture(3);
  ui::NodeRef<Label> ref;
  const auto failed =
    ui::mount(*fixture.scene, ui::column(ui::label("ready").reference(ref), ui::custom<MountFailure>()));

  check(!failed && treeCause(failed.error(), TreeError::CALLBACK_FAILED), "checked mount failure propagated");
  check(ref.node() == Node<Label>{}, "mount failure does not publish preceding ref");
  check(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(), "partial tree removed");

  const auto retired =
    ui::mount(*fixture.scene,
              ui::label("remove").setup([&](ManagedScene &, Node<Label> node) -> std::expected<void, ManagedError> {
                take(fixture.app->tree().remove(node));

                return {};
              }));

  check(!retired && treeCause(retired.error(), TreeError::STALE_NODE) && !retired.error().cleanup,
        "successful setup cannot return a dead mounted node");

  const std::string invalidBytes = "valid prefix\xFF";
  const auto expectedTextError = GraphemeText::from(invalidBytes).error();
  const auto invalidText = ui::mount(*fixture.scene, ui::label(invalidBytes));

  check(!invalidText && treeCause(invalidText.error(), TreeError::INVALID_ARGUMENT) &&
          invalidText.error().text == expectedTextError,
        "invalid UTF-8 retains exact category and byte offset at mount");
  take(ui::mount(*fixture.scene, ui::column(ui::label("a"), ui::label("b"))));
}

void customAndCallbackLifetime() {
  int destructions{};
  Fixture fixture;
  ui::NodeRef<Custom> custom;
  ui::NodeRef<Button> button;
  int clicks{};
  std::weak_ptr<CaptureLifetime> lifetime;

  {
    auto owner = std::make_shared<CaptureLifetime>(destructions);

    lifetime = owner;

    auto description =
      ui::column(ui::custom<Custom>("extension")
                   .reference(custom)
                   .style({.type = "Control"})
                   .onEvent([](Custom &widget, const RoutedEvent &) -> std::expected<EventReply, TreeError> {
                     ++widget.events;

                     return EventReply::HANDLED;
                   })
                   .onPrepare([](Custom &widget, Node<Widget>) -> std::expected<void, TreeError> {
                     ++widget.prepares;

                     return {};
                   }),
                 ui::button("remove", [&, owner]() -> std::expected<void, TreeError> {
                   ++clicks;
                   ++owner->visits;
                   take(fixture.scene->remove(button.node()));
                   check(owner->visits == 1 && !lifetime.expired() && destructions == 0,
                         "admitted callback retains its owned capture after logical removal");

                   return {};
                 }).reference(button));

    take(ui::mount(*fixture.scene, std::move(description)));
  }

  fixture.prepare();
  take(fixture.scene->theme(turboTheme()));
  take(fixture.app->router().invoke(custom.node(), {Action::ACTIVATE}));
  take(fixture.app->tree().read(custom.node(), [](const Custom &widget) {
    check(widget.events == 1 && widget.prepares == 1 && widget.styleRevision() != 0,
          "custom final retains callbacks and style after description destruction");
  }));
  take(fixture.app->router().invoke(button.node(), {Action::ACTIVATE}));
  check(clicks == 1 && !fixture.app->tree().inspect(button.node()), "managed remove safe inside admitted callback");
  take(fixture.app->router().focused());
  check(lifetime.expired() && destructions == 1, "owned capture is reclaimed exactly once after callback return");

  const auto repeated = fixture.app->router().invoke(button.node(), {Action::ACTIVATE});

  check(!repeated && repeated.error() == TreeError::STALE_NODE && clicks == 1 && destructions == 1,
        "retired callback cannot be admitted again");
}

void deferredUnmountKeepsCoreSemantics() {
  RemovalOutcome immediate;
  RemovalOutcome deferred;
  Fixture fixture;
  const Node<Widget> existing = take(ui::mount(*fixture.scene, ui::label("existing")));
  const auto direct =
    take(ui::mount(*fixture.scene, ui::custom<RemovesSibling>(fixture.scene.get(), existing, &immediate)));

  take(fixture.scene->remove(direct));
  check(immediate.called && immediate.blocked && !immediate.removed,
        "managed mutation reentering an active remove is blocked");

  take(fixture.app->tree().withScope(fixture.scene->root(), [&](const UiScope &) {
    auto recipe =
      ui::custom<RemovesSibling>(fixture.scene.get(), existing, &deferred)
        .setup([](ManagedScene &, Node<RemovesSibling> node) -> std::expected<void, ManagedError> {
          return std::unexpected(ManagedError{ManagedPhase::MOUNT, node.id(), TreeError::INVALID_GEOMETRY, {}, {}});
        });
    const auto failed = ui::mount(*fixture.scene, std::move(recipe));

    check(!failed && treeCause(failed.error(), TreeError::INVALID_GEOMETRY) && !failed.error().cleanup,
          "rollback under an outer borrow preserves its original error");
    check(!deferred.called && fixture.app->tree().inspect(existing).has_value(),
          "outer borrow postpones unmount and its external effects");
  }));

  check(deferred.called && deferred.removed && !deferred.blocked && !fixture.app->tree().inspect(existing),
        "deferred unmount retains the core's sibling-removal semantics");
}

void sharedWidthPolicyAndStandardSemantics() {
  constexpr WidthPolicy WIDTH{AmbiguousWidth::WIDE, EmojiWidth::ONE, 42};
  Fixture fixture(8, WIDTH);
  const auto checked = take(fixture.scene->state(false));
  const auto name = take(fixture.scene->state(std::string("draft")));
  ui::NodeRef<Label> label;
  ui::NodeRef<CheckBox> checkbox;
  ui::NodeRef<LineEdit> editor;
  int changes{};

  take(ui::mount(*fixture.scene, ui::column(ui::label("width").reference(label),
                                            ui::checkbox("enabled", checked,
                                                         [&](CheckBoxResult receipt) -> std::expected<void, TreeError> {
                                                           check(receipt && receipt->snapshot->value,
                                                                 "standard commit precedes callback");
                                                           ++changes;

                                                           return {};
                                                         })
                                              .reference(checkbox),
                                            ui::lineEdit(name).reference(editor))));
  fixture.prepare();
  take(fixture.app->tree().read(label.node(), [&](const Label &widget) {
    check(widget.text().policy() == WIDTH, "label uses scene width policy");
  }));
  take(fixture.app->tree().read(editor.node(), [&](const LineEdit &widget) {
    check(widget.text().policy() == WIDTH && widget.text().bytes() == "draft", "editor shares policy and bound model");
  }));
  take(fixture.app->router().invoke(checkbox.node(), {Action::ACTIVATE}));
  check(take(checked.snapshot())->value && changes == 1, "checkbox keeps standard semantics");

  const auto duplicate =
    ui::mount(*fixture.scene, ui::checkbox("duplicate", checked)
                                .onEvent([](CheckBox &, const RoutedEvent &) -> std::expected<EventReply, TreeError> {
                                  return EventReply::HANDLED;
                                }));

  check(!duplicate && treeCause(duplicate.error(), TreeError::INVALID_ARGUMENT),
        "raw adapter cannot replace standard binding");
}

void instanceStateModifiers() {
  Fixture fixture;
  ui::Ref<Custom> method, pipe, child;
  ui::Ref<Column> parent;
  ui::Ref<Button> disabled;
  int activations{};
  bool setupObserved{};

  const auto mounted = take(ui::mount(
    *fixture.scene,
    ui::column(
      ui::custom<Custom>("method").reference(method).visible().visible(false).enabled(false).enabled().setup(
        [&](ManagedScene &, Node<Custom> node) -> std::expected<void, ManagedError> {
          const auto snapshot = take(fixture.app->tree().inspect(node));

          setupObserved = !snapshot.visible && snapshot.enabled && method.node() == Node<Custom>{};

          return {};
        }),
      ui::custom<Custom>("pipe").reference(pipe) | ui::visible() | ui::visible(false) | ui::enabled(false) |
        ui::enabled(),
      ui::column(ui::custom<Custom>("child").reference(child).visible().enabled()).reference(parent).visible(false),
      ui::button(disabled, "Disabled", [&] {
        ++activations;
      }).enabled(false))));
  auto &tree = fixture.app->tree();

  check(setupObserved, "final instance state precedes setup and reference publication");
  check(!take(tree.inspect(method.node())).visible && take(tree.inspect(method.node())).enabled &&
          !take(tree.inspect(pipe.node())).visible && take(tree.inspect(pipe.node())).enabled,
        "method and pipe modifiers agree on final non-sized custom widgets");
  check(take(tree.inspect(child.node())).visible && take(tree.inspect(child.node())).enabled &&
          !take(tree.inputEligible(child.node())),
        "ancestor visibility dominates independent local child flags");
  check(take(tree.measure(parent.node(), {40, 10})) == CellSize{}, "hidden subtree does not measure");
  check(take(tree.measure(disabled.node(), {40, 10})).width > 0 && !take(tree.inputEligible(disabled.node())),
        "disabled control retains layout while input is excluded");
  take(tree.setVisible(parent.node(), true));
  check(take(tree.inputEligible(child.node())), "revealing ancestor preserves the child's local flags");
  take(tree.setEnabled(parent.node(), false));
  check(take(tree.inspect(child.node())).enabled && !take(tree.inputEligible(child.node())),
        "disabled ancestor dominates a locally enabled child");
  fixture.prepare();
  take(tree.read(disabled.node(), [](const Button &widget) {
    check(!widget.enabled(), "existing control synchronization selects disabled presentation");
  }));
  take(fixture.app->router().invoke(disabled.node(), {Action::ACTIVATE}));
  check(activations == 0, "disabled standard binding does not invoke its callback");
  take(tree.consumeFrameRequest());
  fixture.prepare();
  check(take(tree.consumeFrameRequest()) == Invalidation::NONE, "instance flags do not create an idle redraw loop");
  take(fixture.scene->remove(mounted));
  check(!tree.inspect(method.node()) && !tree.inspect(disabled.node()), "state modifiers retain normal cleanup");

  auto noOp = ui::detail::Access::describe<Custom>([&](ManagedScene &scene, Node<Widget> owner) {
    auto node = scene.emplace<Custom>(owner, "no-op");

    take(tree.consumeFrameRequest());

    return node;
  });

  take(ui::mount(*fixture.scene, std::move(noOp).visible(false).visible().enabled(false).enabled().setup(
                                   [&](ManagedScene &, Node<Custom>) -> std::expected<void, ManagedError> {
                                     check(take(tree.consumeFrameRequest()) == Invalidation::NONE,
                                           "only final default flags apply, preserving equal-value no-op");

                                     return {};
                                   })));

  const auto before = take(tree.inspect(fixture.scene->root())).children.size();
  ui::Ref<Custom> abandoned;
  auto failed = ui::mount(*fixture.scene,
                          ui::column(ui::custom<Custom>("abandoned").reference(abandoned).visible(false).enabled(false),
                                     ui::custom<MountFailure>()));

  check(!failed && failed.error().phase == ManagedPhase::MOUNT && abandoned.node() == Node<Custom>{} &&
          take(tree.inspect(fixture.scene->root())).children.size() == before,
        "partial mount failure removes flagged subtree and publishes no reference");

  ui::Ref<Column> scope;
  ui::Ref<Button> hiddenDefault, fallback;

  take(ui::mount(*fixture.scene, ui::column(ui::button(hiddenDefault, "Hidden default",
                                                       [] {
                                                       })
                                              .visible(false)
                                              .focusDefault(),
                                            ui::button(fallback, "Fallback",
                                                       [] {
                                                       }))
                                   .reference(scope)
                                   .focusScope()));
  take(scope.focus());
  check(take(fixture.app->router().focused()) == fallback.node(),
        "existing focus-default policy skips an ineligible hidden default");
}

void instanceClasses() {
  Fixture fixture;
  ui::Ref<Custom> method, pipe, replaced, cleared;
  std::string list = " \taccent\r\naccent\f\vsecondary ";
  auto description = ui::custom<Custom>("method")
                       .reference(method)
                       .style(StyleTarget{.type = "Special", .id = "identity", .control = true, .baseTypes = {"Base"}})
                       .classes("unused")
                       .classes(list);
  auto modifier = ui::classes(list);

  list.assign("changed after capture");

  const auto split = ui::detail::splitClasses(" \taccent\r\naccent\f\vsecondary ");

  check(split.values == std::vector<std::string>{"accent", "accent", "secondary"},
        "class splitting uses all six ASCII separators and preserves order and duplicates");
  check(ui::detail::splitClasses("x\xc2\xa0y").values == std::vector<std::string>{"x\xc2\xa0y"},
        "non-ASCII whitespace stays within the class name");
  take(ui::mount(*fixture.scene,
                 ui::column(std::move(description), ui::custom<Custom>("pipe").reference(pipe) | std::move(modifier),
                            ui::custom<Custom>("replaced")
                              .reference(replaced)
                              .classes("accent secondary")
                              .style(StyleTarget{.type = "Special", .classes = {"replacement"}}),
                            ui::custom<Custom>("cleared").reference(cleared).classes("accent secondary").classes(""))));
  take(
    fixture.scene->theme(take(parsePss("Special.accent { foreground:#010203; } Base.secondary { background:#040506; } "
                                       "#identity.accent { bold:true; } Control.accent { italic:true; } "
                                       ".accent.secondary { underline:true; } .replacement { bold:true; }"))));
  take(fixture.app->tree().read(method.node(), [](const Custom &widget) {
    check(widget.computedStyle().has_value(), "class metadata participates in theme matching");

    const auto &style = widget.computedStyle()->normal;

    check(style.foreground == RgbColor{1, 2, 3} && style.background == RgbColor{4, 5, 6} && style.bold &&
            style.italic && style.underline,
          "class replacement preserves type, id, base type and control identity");
  }));
  take(fixture.app->tree().read(pipe.node(), [](const Custom &widget) {
    check(widget.computedStyle() && widget.computedStyle()->normal.underline,
          "owning pipe class list matches the method after input destruction");
  }));
  take(fixture.app->tree().read(replaced.node(), [](const Custom &widget) {
    check(widget.computedStyle() && widget.computedStyle()->normal.bold && !widget.computedStyle()->normal.underline,
          "later style replaces classes in source order");
  }));
  take(fixture.app->tree().read(cleared.node(), [](const Custom &widget) {
    check(widget.computedStyle() && !widget.computedStyle()->normal.underline,
          "explicit empty classes clear previous values");
  }));

  bool foreignRejected{};

  std::thread worker([&] {
    const auto result = fixture.scene->classes(method.node(), {});

    foreignRejected =
      !result && result.error().phase == ManagedPhase::STYLE && treeCause(result.error(), TreeError::WRONG_THREAD);
  });

  worker.join();
  check(foreignRejected, "managed classes preserves thread validation and STYLE phase");
  take(fixture.app->tree().frame([&] {
    const auto result = fixture.scene->classes(method.node(), {});

    check(!result && result.error().phase == ManagedPhase::STYLE && treeCause(result.error(), TreeError::WOULD_BLOCK),
          "managed classes rejects frame mutation without replacing metadata");
  }));

  ui::Ref<Custom> abandoned;
  const auto before = take(fixture.app->tree().inspect(fixture.scene->root())).children.size();
  auto failed =
    ui::mount(*fixture.scene, ui::column(ui::custom<Custom>("abandoned").reference(abandoned).classes("accent"),
                                         ui::custom<MountFailure>()));

  check(!failed && abandoned.node() == Node<Custom>{} &&
          take(fixture.app->tree().inspect(fixture.scene->root())).children.size() == before,
        "failed class-bearing subtree rolls back without publishing references");
}

void exceptionsAndForeignMount() {
  Fixture fixture;
  const auto thrown = ui::mount(
    *fixture.scene, ui::label("failure").setup([](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
      throw std::bad_alloc();
    }));

  check(!thrown && treeCause(thrown.error(), TreeError::NO_MEMORY) && !thrown.error().cleanup,
        "allocation exception within setup typed and subtree reclaimed");

  bool rejected{};
  std::thread worker([&] {
    const auto result = ui::mount(*fixture.scene, ui::label("foreign"));

    rejected = !result && treeCause(result.error(), TreeError::WRONG_THREAD);
  });

  worker.join();
  check(rejected && take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
        "foreign mount rejected without mutation");
}

void managedFocusSetupIsolation() {
  enum class Boundary {
    FOCUS,
    SCOPE,
    DEFAULT,
    ENTER,
    LEAVE
  };

  struct Observation {
    Boundary boundary;
    int calls{};
    int later{};
    bool attributed{};
    bool closed{};
    bool prepared{};
  };

  struct Retirement {
    std::function<void()> callback;

    explicit Retirement(std::function<void()> action) : callback(std::move(action)) {
    }

    ~Retirement() {
      callback();
    }
  };

  constexpr std::array BOUNDARIES{Boundary::FOCUS, Boundary::SCOPE, Boundary::DEFAULT, Boundary::ENTER,
                                  Boundary::LEAVE};
  std::array<Observation, BOUNDARIES.size()> observations{};
  std::size_t index{};

  // Raw callbacks and capture retirement bypass the existing managed callback masks.
  for (auto boundary : BOUNDARIES) {
    Fixture fixture;
    ui::Ref<Column> target, abandoned;
    ui::Ref<Label> missing;

    take(ui::mount(*fixture.scene, ui::column(ui::column(target), ui::column(abandoned))));

    Observation observed{boundary};
    const auto unavailable = [&] {
      ++observed.calls;

      auto failure = missing.text() = "unrelated callback or retirement";
      auto result = failure.result();

      observed.closed = !result && treeCause(result.error(), TreeError::CLOSED);
      observed.attributed = !result && result.error().setupStopped;
    };
    ActionBinding binding, scope, priorBinding;
    bool observing{};

    if (boundary == Boundary::FOCUS || boundary == Boundary::ENTER || boundary == Boundary::LEAVE) {
      binding = take(fixture.app->router().bind(
        target.node(),
        [](Column &, const RoutedEvent &) {
          return EventReply::IGNORED;
        },
        [&](Column &, bool focused) -> std::expected<void, TreeError> {
          if (observing && focused == (boundary != Boundary::LEAVE)) {
            unavailable();
          }

          return {};
        }));
      if (boundary == Boundary::ENTER || boundary == Boundary::LEAVE) {
        take(fixture.scene->focusScope(target.node()));
      }
      if (boundary == Boundary::LEAVE) {
        priorBinding = take(fixture.app->router().bind(abandoned.node(), [](Column &, const RoutedEvent &) {
          return EventReply::IGNORED;
        }));
        take(fixture.app->router().setFocus(abandoned.node()));
        take(target.enterFocusScope());
      }
    } else {
      if (boundary == Boundary::DEFAULT) {
        scope = take(fixture.app->router().focusScope(target.node()));
      }

      auto capture = std::make_shared<Retirement>(unavailable);

      binding = take(fixture.app->router().bind(abandoned.node(), [capture](Column &, const RoutedEvent &) {
        return EventReply::IGNORED;
      }));
      capture.reset();
      binding.disconnect();
    }

    observing = true;
    auto outer = ui::scene(ui::label("outer"));

    outer.onError([](const ManagedDiagnostic &) {
    });
    outer
      .configure([&](ManagedScene &) {
        switch (boundary) {
        case Boundary::FOCUS:
          take(target.focus());

          break;
        case Boundary::SCOPE:
          take(fixture.scene->focusScope(target.node()));

          break;
        case Boundary::DEFAULT:
          take(fixture.scene->focusDefault(target.node()));

          break;
        case Boundary::ENTER:
          take(target.enterFocusScope());

          break;
        case Boundary::LEAVE:
          take(target.leaveFocusScope());

          break;
        }
      })
      .configure([&](ManagedScene &) {
        ++observed.later;
      });
    observed.prepared = outer.prepareChecked().has_value();
    observations[index++] = observed;
  }

  // Gather every path before asserting so the red run records all three boundaries.
  for (const auto &observed : observations) {
    std::cout << "focus setup boundary=" << static_cast<int>(observed.boundary) << " calls=" << observed.calls
              << " attributed=" << observed.attributed << " prepared=" << observed.prepared
              << " later=" << observed.later << '\n';
  }

  for (const auto &observed : observations) {
    check(observed.calls == 1 && observed.closed && !observed.attributed && observed.prepared && observed.later == 1,
          "managed focus callback/retirement cannot stop unrelated outer setup");
  }
}

void focusCallbacksAndRemount() {
  Fixture fixture;
  ui::Ref<Button> target, trigger, unownedTrigger, missing;
  bool unownedClosed{};

  take(ui::mount(*fixture.scene, ui::column(ui::button(target, "Target",
                                                       [] {
                                                       }),
                                            ui::button(trigger, "Focus",
                                                       [target] {
                                                         (void)target.focus();
                                                       }),
                                            ui::button(unownedTrigger, "Unowned", [&] {
                                              auto result = missing.focus();

                                              unownedClosed = !result && treeCause(result.error(), TreeError::CLOSED);
                                            }))));
  take(fixture.app->router().invoke(trigger.node(), {Action::ACTIVATE}));
  check(take(fixture.app->router().focused()) == target.node(), "void action can request checked Ref focus");
  check(take(fixture.scene->diagnostics()).errors.empty(), "successful void focus action reports no error");
  take(fixture.app->tree().setVisible(target.node(), false));
  take(fixture.app->router().invoke(trigger.node(), {Action::ACTIVATE}));

  auto errors = take(fixture.scene->diagnostics());

  check(errors.errors.size() == 1 && treeCause(errors.errors.front().error, TreeError::INVALID_ARGUMENT),
        "owned failure in void focus action is collected exactly once");
  take(fixture.app->router().invoke(unownedTrigger.node(), {Action::ACTIVATE}));
  check(unownedClosed && take(fixture.scene->diagnostics()).errors.size() == 1,
        "unpublished Ref returns CLOSED without borrowing the unrelated action collector");

  ui::Ref<Column> retiredGroup;
  ui::Ref<Button> retiredChild;
  auto retired = take(ui::mount(*fixture.scene, ui::column(retiredGroup, ui::button(retiredChild, "Retired",
                                                                                    [] {
                                                                                    })
                                                                           .focusDefault())
                                                  .focusScope()));

  take(retiredGroup.focus());
  take(fixture.scene->remove(retired));

  ui::Ref<Column> replacementGroup;
  ui::Ref<Button> replacementChild;
  auto replacement =
    take(ui::mount(*fixture.scene, ui::column(replacementGroup, ui::button(replacementChild, "Replacement",
                                                                           [] {
                                                                           })
                                                                  .focusDefault())
                                     .focusScope()));

  check(replacement.id().slot == retired.id().slot && replacement.id() != retired.id(),
        "scope remount reuses slot only with a fresh exact generation");

  auto stale = retiredGroup.focus();

  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE), "retired scope Ref cannot enter remounted scope");
  take(replacementGroup.focus());
  check(take(fixture.app->router().focused()) == replacementChild.node(), "new scope Ref selects only remounted child");

  auto reused = ui::mount(*fixture.scene, ui::column(retiredGroup).focusScope());

  check(!reused && treeCause(reused.error(), TreeError::INVALID_ARGUMENT), "remount requires a fresh observing Ref");
  check(take(fixture.app->router().focused()) == replacementChild.node(),
        "failed stale Ref remount preserves new focus");
}

void declarativeFocusReturn() {
  Fixture fixture;
  ui::Ref<Column> group;
  ui::Ref<Button> inside, outside, enter, leave, remove;
  take(ui::mount(*fixture.scene, ui::column(ui::button(outside, "Outside",
                                                       [] {
                                                       }),
                                            ui::column(group, ui::button(inside, "Inside",
                                                                         [] {
                                                                         })
                                                                .focusDefault())
                                              .focusScope(),
                                            ui::button(enter, "Enter",
                                                       [group] {
                                                         (void)group.enterFocusScope();
                                                       }),
                                            ui::button(leave, "Leave",
                                                       [group] {
                                                         (void)group.leaveFocusScope();
                                                       }),
                                            ui::button(remove, "Remove", [&] {
                                              take(fixture.scene->remove(group.node()));
                                            }))));
  take(outside.focus());
  take(fixture.app->router().invoke(enter.node(), {Action::ACTIVATE}));
  check(take(fixture.app->router().focused()) == inside.node(), "void callback enters focus-return scope");
  take(fixture.app->router().invoke(enter.node(), {Action::ACTIVATE}));
  check(take(fixture.scene->diagnostics()).errors.size() == 1, "duplicate void entry reports once");
  take(fixture.app->router().invoke(leave.node(), {Action::ACTIVATE}));
  check(take(fixture.app->router().focused()) == outside.node(), "void callback leaves to saved focus");
  check(take(fixture.scene->diagnostics()).errors.size() == 1, "successful leave adds no diagnostic");
  take(group.enterFocusScope());
  take(fixture.app->router().invoke(remove.node(), {Action::ACTIVATE}));
  check(take(fixture.app->router().focused()) == outside.node(), "callback removal recovers through same router");
  auto stale = group.leaveFocusScope();
  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE), "removed reference cannot leave a remounted root");
  fixture.scene.reset();
  auto closed = group.enterFocusScope();
  check(!closed && treeCause(closed.error(), TreeError::CLOSED), "return reference safely outlives owner");
}

void declarativeFocusScopes() {
  Fixture fixture;
  ui::Ref<Column> group;
  ui::Ref<Button> first;
  ui::Ref<Button> preferred;
  ui::Ref<Button> outside;

  check(!group.focus() && treeCause(group.focus().error(), TreeError::CLOSED), "unpublished scope Ref is closed");

  auto mounted = take(ui::mount(*fixture.scene, ui::column(ui::column(group,
                                                                      ui::button(first, "First",
                                                                                 [] {
                                                                                 }),
                                                                      ui::button(preferred, "Preferred",
                                                                                 [] {
                                                                                 })
                                                                        .focusDefault())
                                                             .focusScope(),
                                                           ui::button(outside, "Outside", [] {
                                                           }))));

  take(group.focus());
  check(take(fixture.app->router().focused()) == preferred.node(), "declarative scope enters default without tokens");
  take(first.focus());
  take(outside.focus());
  take(group.focus());
  check(take(fixture.app->router().focused()) == first.node(), "Ref scope restores exact last target");

  std::thread worker([&] {
    auto rejected = group.focus();

    check(!rejected && treeCause(rejected.error(), TreeError::WRONG_THREAD), "Ref focus rejects foreign thread");
  });

  worker.join();
  take(fixture.scene->remove(mounted));

  auto stale = group.focus();

  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE), "removed scope Ref never resolves by slot");
  fixture.scene.reset();

  auto closed = group.focus();

  check(!closed && treeCause(closed.error(), TreeError::CLOSED), "scope Ref safely outlives scene");

  Fixture rollback;
  ui::Ref<Column> failed;
  auto result =
    ui::mount(*rollback.scene,
              ui::column(failed, ui::button("No action").focusDefault())
                .focusScope()
                .setup([](ManagedScene &, Node<Column>) -> std::expected<void, ManagedError> {
                  return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::INVALID_ARGUMENT, {}, {}});
                }));

  check(!result && failed.node() == Node<Column>{}, "failed scope mount publishes no Ref");

  ui::Ref<Button> noScope;
  auto missing = ui::mount(*rollback.scene, ui::button(noScope, "Missing scope", [] {
                                            }).focusDefault());

  check(!missing && treeCause(missing.error(), TreeError::INVALID_ARGUMENT) && noScope.node() == Node<Button>{},
        "default without scope rolls back instead of silently mutating global focus");
}
} // namespace

int main() {
  try {
    managedFocusSetupIsolation();
    focusCallbacksAndRemount();
    declarativeFocusReturn();
    declarativeFocusScopes();
    customArgumentShape();
    setupOrderIsPreorder();
    textLimitKeepsTypedError();
    storedStringsAndSingleUse();
    referencesAreTransactional();
    rollbackReclaimsBindingsAndBudget();
    mountErrorsAndRemovedNodes();
    customAndCallbackLifetime();
    deferredUnmountKeepsCoreSemantics();
    sharedWidthPolicyAndStandardSemantics();
    instanceStateModifiers();
    instanceClasses();
    exceptionsAndForeignMount();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }

  std::cout << "owning description Gate A checks passed\n";

  return 0;
}
