// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/theme_selector.hpp>

#include "support/managed_text_fixture.hpp"

namespace {
constexpr CellSize SELECTOR_VIEWPORT{80, 24};

StyleSheet theme() {
  StyleRule rule;
  rule.declarations.foreground = RgbColor{220, 10, 10};
  return {.rules = {rule}};
}

struct SelectorFixture {
  ui::Ref<ThemeSelector> selector;
  ui::Ref<Dialog> view;
  ui::Ref<Button> opener;
  ManagedScene *managed{};
  Application *app{};
  std::size_t factories{};
  std::function<void()> otherFactory;
  ui::Scene scene;

  explicit SelectorFixture(ui::ThemeSelectorOptions options = {}, const std::optional<std::string> &initialId = {})
      : scene(ui::scene(ui::button(opener, "Open",
                                   [reference = selector] {
                                     return reference.open();
                                   }),
                        ui::themeSelector(selector,
                                          ui::ThemeSelectorCatalog{.choices = {{"red", "Red 🙂é界",
                                                                                [this] {
                                                                                  ++factories;

                                                                                  return theme();
                                                                                },
                                                                                TextValue("Unicode catalog preview")},
                                                                               {"other", "Other",
                                                                                [this] {
                                                                                  if (otherFactory) {
                                                                                    otherFactory();
                                                                                  }

                                                                                  return theme();
                                                                                },
                                                                                TextValue("Alternative preview")}},
                                                                   .initialId = initialId},
                                          options)
                          .reference(view))) {
    scene.configure([this](ManagedScene &owner, Application &application) {
      managed = &owner;
      app = &application;
    });
    take(scene.prepareChecked());
    prepare();
  }

  void prepare(CellSize size = SELECTOR_VIEWPORT) {
    take(managed->prepare(*app, size));
    for (std::size_t pass = 0; pass < ApplicationOptions{}.maxPreparationPasses; ++pass) {
      std::optional<LayoutSnapshot> layout;
      take(app->tree().frame([&] {
        const auto bounds = take(CellRect::from({0, 0}, size));
        take(layoutTree(app->tree(), take(app->tree().root()), bounds, bounds));
        layout = take(LayoutSnapshot::capture(app->tree(), managed->root(), LayoutSnapshotProfile::GEOMETRY));
      }));
      take(app->tree().consumeFrameRequest());
      take(managed->prepareViews(*app, *layout));
      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        take(managed->completedLayout(*app));
        return;
      }
    }
    check(false, "selector layout converges within existing runner budget");
  }

  Node<Widget> button(Node<Widget> root, std::string_view caption) {
    Node<Widget> result;
    take(app->tree().read(root, [&](const Widget &widget) {
      const auto *value = dynamic_cast<const Button *>(&widget);
      if (value && value->text().bytes() == caption) {
        result = root;
      }
    }));
    if (result != Node<Widget>{}) {
      return result;
    }
    for (auto child : take(app->tree().inspect(root)).children) {
      result = button(child, caption);
      if (result != Node<Widget>{}) {
        return result;
      }
    }
    return {};
  }

  void choose(std::string_view caption) {
    const auto node = button(view.node(), caption);
    check(node != Node<Widget>{}, "choice exists");
    take(app->router().invoke(node, {Action::ACTIVATE}));
  }

  bool open() {
    return take(app->tree().inspect(view.node())).visible;
  }
};

void canonicalReferences() {
  ui::Ref<ThemeSelector> cold;
  auto coldCopy = cold;
  auto coldMoved = std::move(coldCopy);

  check(treeCause(coldCopy.open().error(), TreeError::CLOSED), "moved-from selector reference remains closed");
  check(treeCause(coldMoved.open().error(), TreeError::CLOSED) && !take(coldMoved.lastApply()),
        "canonical cold selector preserves checked commands and empty owning metadata");

  {
    ui::ThemeSelectionRef missingModel;
    auto failed = ui::scene(ui::overlayHost(ui::label("Body"), ui::themeSelector(cold, missingModel)));

    check(!failed.prepareChecked(), "selector without installed model fails its existing attachment contract");
  }

  check(treeCause(cold.open().error(), TreeError::CLOSED) && !take(cold.lastApply()),
        "failed attachment publishes no canonical selector identity");

  SelectorFixture f;
  ui::ThemeSelectorRef legacy = f.selector;
  const auto before = allocationTest::count;
  ui::Ref<ThemeSelector> adapted(legacy);
  auto copied = adapted;
  auto moved = std::move(copied);

  check(allocationTest::count == before, "selector adaptation and reference copies add no identity allocation");

  std::optional<ManagedError> foreign;
  std::thread worker([&] {
    const auto result = moved.lastApply();

    if (!result) {
      foreign = result.error();
    }
  });

  worker.join();
  check(foreign && treeCause(*foreign, TreeError::WRONG_THREAD), "canonical selector rejects foreign metadata access");
  take(moved.open());
  f.choose("Red 🙂é界");
  take(adapted.apply());

  const auto outcome = take(legacy.lastApply());

  check(outcome && take(f.selector.lastApply()) == outcome && take(moved.lastApply()) == outcome,
        "legacy and canonical handles share one completed Apply identity");
  take(f.scene.close());
  check(take(moved.lastApply()) == outcome && !moved.open(),
        "retired canonical selector retains owning metadata without reviving the view");
}

void lifecycleAndAdmission() {
  ui::ThemeSelectorRef cold;
  check(treeCause(cold.open().error(), TreeError::CLOSED) && !take(cold.lastApply()), "cold checked identity");
  SelectorFixture f;
  take(f.selector.open());
  check(f.open() && f.factories == 0, "open invokes no theme factory");
  f.choose("Red 🙂é界");
  check(f.factories == 1 && take(f.selector.snapshot())->previewedId == "red", "row previews once");
  take(f.selector.closeKeepingPreview());
  check(!f.open() && take(f.selector.snapshot())->status == ui::ThemeSelectionStatus::ACTIVE, "keep closes only view");
  f.prepare();
  take(f.selector.open());
  take(f.selector.apply());
  auto first = take(f.selector.lastApply());
  check(first && first->committedId == "red" && first->closeDisposition == ui::ThemeSelectorCloseDisposition::CLOSED,
        "apply publishes completed owning outcome");
  f.prepare();
  take(f.selector.open());
  check(!f.selector.apply() && take(f.selector.lastApply()) == first,
        "reopen does not infer current choice from history");
  f.choose("Red 🙂é界");
  take(f.selector.preview("other"));
  check(!f.selector.apply() && take(f.selector.snapshot())->previewedId == "other" &&
          take(f.selector.lastApply()) == first,
        "selected ID guard preserves independent model preview");
  take(f.selector.cancel());
  allocationTest::remaining = 0;
  auto retained = f.selector.lastApply();
  allocationTest::remaining = -1;
  check(retained && *retained == first, "completed metadata read allocates nothing");
  take(f.scene.close());
  check(take(f.selector.lastApply()) == first && !f.selector.open(), "retired view retains owning history");
}

void routedCancelAndSupersession() {
  SelectorFixture f;
  take(f.selector.open());
  f.prepare();
  take(f.app->router().dispatch(KeyEvent{InputKey::DOWN}));
  check(f.factories == 1, "routed navigation previews exactly once");
  take(f.managed->theme(theme()));
  f.prepare();
  const auto apply = f.button(f.view.node(), "Apply");
  check(!take(f.app->tree().inspect(apply)).enabled, "equal external authority disables Apply without polling");
  auto cancelled = f.selector.cancel();
  const auto *style = !cancelled ? std::get_if<StyleError>(&cancelled.error().cause) : nullptr;
  check(style && style->cause == TreeError::INVALID_ARGUMENT && f.open(), "superseded cancel cannot hide conflict");
  take(f.selector.closeKeepingPreview());
  check(!f.open(), "explicit recovery closes conflict view");
  f.prepare();
  take(f.app->tree().consumeFrameRequest());
  take(f.managed->theme(theme()));
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "closed equal authority only latches dirty");
  take(f.selector.open());
  f.choose("Other");
  take(f.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  check(!f.open() && take(f.selector.snapshot())->status == ui::ThemeSelectionStatus::IDLE,
        "routed Escape restores before ordinary dismissal");
  f.prepare({2, 2});
  check(treeCause(f.selector.open().error(), TreeError::INVALID_GEOMETRY) && !f.open(),
        "tiny unusable geometry rejects before modal publication");
}

void callbackAndCloseOutcome() {
  ui::ThemeSelectorRef reference;
  std::size_t delivered{};
  ManagedError failure{ManagedPhase::STYLE,
                       {},
                       StyleError{.cause = TreeError::INVALID_ARGUMENT, .message = "application callback"},
                       {},
                       TreeError::CLOSED};
  failure.operation = "save-theme";
  ui::ThemeSelectorOptions options;
  options.onCommitted = [&](std::string_view id) -> std::expected<void, ManagedError> {
    ++delivered;
    check(id == "red" && treeCause(reference.apply().error(), TreeError::WOULD_BLOCK), "callback reentry rejected");
    check(!take(reference.lastApply()), "callback sees previous completed result only");
    return std::unexpected(failure);
  };
  SelectorFixture f(options);
  reference = f.selector;
  take(f.managed->onDismiss(f.view.node(), [](DialogDismissReason) -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }));
  take(f.selector.open());
  f.choose("Red 🙂é界");
  const auto applied = f.selector.apply();
  auto outcome = take(f.selector.lastApply());
  check(!applied && applied.error().operation == failure.operation && delivered == 1 && !f.open(),
        "callback failure does not skip published close");
  check(outcome && outcome->callbackError && outcome->callbackError->cleanup == TreeError::CLOSED &&
          outcome->callbackError->operation == failure.operation && outcome->closeError &&
          treeCause(*outcome->closeError, TreeError::CALLBACK_FAILED) &&
          outcome->closeDisposition == ui::ThemeSelectorCloseDisposition::CLOSED,
        "two complete independent errors and actual close retained");
  check(!f.selector.apply() && delivered == 1 && take(f.selector.lastApply()) == outcome, "delivery cannot repeat");
}

void outcomeAllocationAndRetirement() {
  std::size_t calls{};
  SelectorFixture *owner{};
  ui::ThemeSelectorOptions options;
  options.onCommitted = [&](std::string_view) -> std::expected<void, ManagedError> {
    ++calls;
    take(owner->managed->remove(owner->view.node()));
    return {};
  };
  SelectorFixture f(options);
  owner = &f;
  take(f.selector.open());
  f.choose("Red 🙂é界");
  allocationTest::remaining = 0;
  const auto failed = f.selector.apply();
  allocationTest::remaining = -1;
  check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY) && !take(f.selector.lastApply()) && calls == 0 &&
          take(f.selector.snapshot())->status == ui::ThemeSelectionStatus::ACTIVE,
        "new outcome allocation fails before model commit or callback");
  take(f.selector.apply());
  auto outcome = take(f.selector.lastApply());
  check(calls == 1 && outcome && outcome->closeDisposition == ui::ThemeSelectorCloseDisposition::RETIRED,
        "callback self-removal pins completed metadata without stale close");
}

void stagedOpenAndCloseFailure() {
  SelectorFixture f;
  allocationTest::remaining = 0;
  const auto failedOpen = f.selector.open();
  allocationTest::remaining = -1;
  check(!failedOpen && treeCause(failedOpen.error(), TreeError::NO_MEMORY) && !f.open(),
        "prepublication open failure leaves closed identity retryable");
  take(f.selector.open());
  take(f.selector.closeKeepingPreview());

  std::size_t calls{};
  ui::ThemeSelectorOptions options;
  options.onCommitted = [&](std::string_view) -> std::expected<void, ManagedError> {
    ++calls;
    allocationTest::remaining = 0;
    return {};
  };
  SelectorFixture second(options);
  take(second.selector.open());
  second.choose("Red 🙂é界");
  const auto result = second.selector.apply();
  allocationTest::remaining = -1;
  const auto outcome = take(second.selector.lastApply());
  check(!result && treeCause(result.error(), TreeError::NO_MEMORY) && calls == 1 && second.open() && outcome &&
          outcome->closeError && !outcome->callbackError &&
          outcome->closeDisposition == ui::ThemeSelectorCloseDisposition::OPEN &&
          take(second.selector.snapshot())->status == ui::ThemeSelectionStatus::IDLE,
        "committed delivery survives independent prepublication close allocation failure");
  take(second.selector.closeKeepingPreview());
  check(!second.open() && calls == 1 && take(second.selector.lastApply()) == outcome,
        "explicit close retry cannot replay commit or delivery");
}

void pointerAndNarrowReveal() {
  SelectorFixture f;
  take(f.managed->focus(f.opener.node()));
  f.prepare({12, 11});
  take(f.selector.open());
  f.prepare({12, 11});
  take(f.app->router().dispatch(KeyEvent{InputKey::END}));
  f.prepare({12, 11});
  const auto other = f.button(f.view.node(), "Other");
  const auto geometry = take(f.app->tree().inspect(other)).geometry;
  if (geometry.surface.intersection(geometry.clip).empty()) {
    std::cerr << "narrow other surface=" << geometry.surface.origin().x << ',' << geometry.surface.origin().y << ' '
              << geometry.surface.size().width << 'x' << geometry.surface.size().height
              << " clip=" << geometry.clip.origin().x << ',' << geometry.clip.origin().y << ' '
              << geometry.clip.size().width << 'x' << geometry.clip.size().height << " valid=" << geometry.valid
              << " preview=" << take(f.selector.snapshot())->previewedId.value_or("none") << '\n';
  }
  check(geometry.valid && !geometry.surface.intersection(geometry.clip).empty() &&
          take(f.selector.snapshot())->previewedId == "other",
        "narrow viewport reveals navigated row through existing ScrollArea");
  f.prepare();
  const auto red = f.button(f.view.node(), "Red 🙂é界");
  const auto rectangle = take(f.app->tree().inspect(red)).geometry;
  const auto point = rectangle.surface.intersection(rectangle.clip).origin();
  take(f.app->router().dispatch(MouseInputEvent{point.x, point.y, MouseButton::LEFT, MouseAction::PRESS}));
  check(f.factories == 1 && take(f.selector.snapshot())->previewedId == "red",
        "pointer and keyboard share one preview path");
  take(f.app->router().dispatch(MouseInputEvent{0, 0, MouseButton::LEFT, MouseAction::PRESS}));
  check(!f.open() && take(f.selector.snapshot())->status == ui::ThemeSelectionStatus::IDLE &&
          take(f.app->router().focused()) == f.opener.node(),
        "outside exact panel cancels before close and returns focus");
}

void interactiveReportsAndObserverRetirement() {
  ManagedError original{ManagedPhase::STYLE, {}, StyleError{.message = "callback"}, {}, TreeError::STALE_NODE};
  ui::ThemeSelectorOptions options;
  options.onCommitted = [original](std::string_view) -> std::expected<void, ManagedError> {
    return std::unexpected(original);
  };
  SelectorFixture f(options);
  take(f.managed->onDismiss(f.view.node(), [](DialogDismissReason) -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }));
  std::size_t reports{};
  take(f.managed->onError([&](const ManagedDiagnostic &diagnostic) {
    ++reports;
    const auto outcome = take(f.selector.lastApply());
    check(outcome && outcome->callbackError && outcome->closeError, "both errors published before observer");
    check(diagnostic.error.phase == ManagedPhase::STYLE && diagnostic.error.cleanup == TreeError::STALE_NODE,
          "callback error reported first with its independent cleanup intact");
    check(treeCause(f.selector.cancel().error(), TreeError::WOULD_BLOCK),
          "observer cannot reenter selector transition");
    take(f.app->tree().remove(f.view.node()));
  }));
  take(f.selector.open());
  f.choose("Red 🙂é界");
  f.prepare();
  f.choose("Apply");
  const auto outcome = take(f.selector.lastApply());
  check(reports == 1 && outcome && outcome->callbackError && outcome->closeError && bool(f.scene.checked()),
        "report-only observer retirement skips stale second report without losing completed errors");
}

void shortcutRetirementDuringProjectionOom() {
  SelectorFixture f;
  auto old = take(f.managed->keyBindings());
  check(old.bindings.size() == 1 && old.bindings[0].eligibility == KeyEligibility::ELIGIBLE,
        "opening annotation is ordinary canonical metadata");
  const KeyEvent prefix{InputKey::CHARACTER, U'k', {.control = true}};
  const KeyEvent suffix{InputKey::CHARACTER, U't', {.control = true}};
  check(take(f.managed->input(*f.app, prefix)), "opening sequence prefix admitted");
  take(f.app->tree().setEnabled(f.view.node(), false));
  allocationTest::remaining = 0;
  const auto query = f.managed->keyBindings();
  allocationTest::remaining = -1;
  check(!query && treeCause(query.error(), TreeError::NO_MEMORY), "retired projection OOM is typed failure");
  check(!take(f.managed->input(*f.app, suffix)) && !take(f.managed->input(*f.app, prefix)) && f.factories == 0,
        "scalar retirement rejects pending suffix and new prefix despite failed key removal");
  check(old.bindings.size() == 1 && old.bindings[0].label.bytes() == "Themes" &&
          old.bindings[0].eligibility == KeyEligibility::ELIGIBLE,
        "completed immutable metadata remains readable and historical");
  auto current = take(f.managed->keyBindings());
  check(current.bindings.empty(), "next successful projection follows existing retired-owner filtering");
}

ui::Scene *independentScene{};
bool independentPrepared{};

bool modelCause(const ManagedError &error, TreeError expected) {
  const auto *style = std::get_if<StyleError>(&error.cause);

  return treeCause(error, expected) || (style && style->cause == expected);
}

void defaultInitialAndDelegatedCommands() {
  std::size_t callbacks{};
  ui::ThemeSelectorOptions options;

  options.onCommitted = [&](std::string_view) -> std::expected<void, ManagedError> {
    ++callbacks;

    return {};
  };

  SelectorFixture f(options, "red");
  const auto initial = take(f.selector.snapshot());

  check(f.factories == 1 && callbacks == 0 && initial->status == ui::ThemeSelectionStatus::IDLE &&
          initial->lastCommittedId == "red" && !take(f.selector.lastApply()) && !f.open(),
        "startup choice commits after setup without a UI Apply callback, outcome or open");
  take(f.selector.preview("other"));
  take(f.selector.commit());
  check(take(f.selector.snapshot())->lastCommittedId == "other" && callbacks == 0 && !take(f.selector.lastApply()),
        "programmatic preview and commit use the same default model without UI Apply delivery");
  take(f.managed->onKey(ui::press("F2"), [reference = f.selector] {
    return reference.preview("red").and_then([reference] {
      return reference.commit();
    });
  }));
  check(take(f.managed->input(*f.app, KeyEvent{InputKey::F2})) &&
          take(f.selector.snapshot())->lastCommittedId == "red" && callbacks == 0,
        "application F2 handler uses the same default catalog and checked model commands");
  take(f.selector.open());

  const auto parent = take(f.app->tree().inspect(f.view.node())).parent;
  const auto oldChildren = take(f.app->tree().inspect(parent)).children;
  const auto before = f.factories;

  take(f.selector.open());
  check(f.factories == before && take(f.app->tree().inspect(parent)).children == oldChildren,
        "reopening refocuses the retained selector without creating another host or view");
  f.choose("Other");
  take(f.selector.apply());

  const auto completed = take(f.selector.lastApply());

  check(callbacks == 1 && completed && completed->committedId == "other", "UI Apply still owns its once-only delivery");
  take(f.scene.close());
  check(initial->catalog->choices.size() == 2 && initial->lastCommittedId == "red" &&
          take(f.selector.lastApply()) == completed && !f.selector.snapshot(),
        "owning catalog and Apply metadata survive shutdown without a live model/view reference");
}

void defaultInitialOrderingAndFactoryFailure() {
  ui::Ref<ThemeSelector> selector;
  ui::Ref<Button> subject;
  ManagedScene *managed{};
  Application *application{};
  bool configured{}, afterConfiguration{};
  std::size_t resolutions{}, factories{};
  ui::ThemeSelectorCatalog catalog;

  catalog.choices.emplace_back("red", i18n("red-label"), [&] {
    ++factories;
    afterConfiguration = configured;

    return theme();
  });
  catalog.selection.textResolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "Red";
  };
  catalog.initialId = "red";

  StyleRule applicationRule, userRule;

  applicationRule.declarations.foreground = RgbColor{10, 10, 220};
  userRule.declarations.minWidth = 23;

  auto owner = ui::scene(ui::button(subject, "Subject"), ui::themeSelector(selector, catalog));

  owner.styleLayers({.maxLayers = 8});
  owner.styleLayer(StyleTier::APPLICATION, "application", StyleSheet{.rules = {applicationRule}});
  owner.styleLayer(StyleTier::USER, "user", StyleSheet{.rules = {userRule}});
  owner.configure([&](ManagedScene &scene, Application &app) {
    managed = &scene;
    application = &app;
    configured = true;
  });
  take(owner.prepareChecked());
  take(managed->prepare(*application, SELECTOR_VIEWPORT));

  ComputedStyle computed;

  take(application->tree().read(subject.node(), [&](const Button &button) {
    computed = *button.computedStyle();
  }));
  check(afterConfiguration && factories == 1 && resolutions == 1 && take(selector.snapshot())->lastCommittedId == "red",
        "one catalog normalization precedes view publication and initial factory follows all scene configurations");
  check(computed.normal.foreground == RgbColor{10, 10, 220} && computed.minWidth == 23,
        "default startup preserves configured APPLICATION and USER overrides");
  take(owner.close());

  for (const bool memory : {false, true}) {
    ui::Ref<ThemeSelector> failed;
    auto failure = ui::scene(ui::label("Body"),
                             ui::themeSelector(failed, ui::ThemeSelectorCatalog{.choices = {{"bad", "Bad",
                                                                                             [memory]() -> StyleSheet {
                                                                                               if (memory) {
                                                                                                 throw std::bad_alloc();
                                                                                               }

                                                                                               throw std::runtime_error(
                                                                                                 "initial factory");
                                                                                             }}},
                                                                                .initialId = "bad"}));
    const auto result = failure.prepareChecked();

    check(!result && modelCause(result.error(), memory ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED) &&
            !failed.snapshot() && !take(failed.lastApply()),
          "initial factory failure is typed and never publishes the default selector or Apply outcome");
  }
}

void defaultCatalogRejectionAndMountRollback() {
  ui::Ref<ThemeSelector> invalid;
  ui::Ref<Dialog> unpublished;
  auto rejected = ui::scene(ui::label("Body"),
                            ui::themeSelector(invalid, ui::ThemeSelectorCatalog{.choices = {{"same", "First", theme},
                                                                                            {"same", "Second", theme}}})
                              .reference(unpublished));
  const auto result = rejected.prepareChecked();

  check(!result && modelCause(result.error(), TreeError::INVALID_ARGUMENT) && unpublished.node() == Node<Dialog>{} &&
          !invalid.snapshot(),
        "invalid catalog is rejected before creating or publishing its view");

  ui::Ref<ThemeSelector> first, second;
  auto duplicate = ui::scene(ui::label("Body"), ui::themeSelector(first, {{"first", "First", theme}}),
                             ui::themeSelector(second, {{"second", "Second", theme}}));
  const auto duplicated = duplicate.prepareChecked();

  check(!duplicated && modelCause(duplicated.error(), TreeError::INVALID_ARGUMENT) && !first.snapshot() &&
          !second.snapshot(),
        "two default installers fail without replacing or publishing either view");

  Fixture f;
  ui::Ref<ThemeSelector> failed;
  ui::Ref<Dialog> failedView;
  auto attempt =
    ui::overlayHost(ui::label("Body"), ui::themeSelector(failed, {{"red", "Red", theme}}).reference(failedView),
                    ui::dialog("Late failure", ui::label("Body"))
                      .setup([](ManagedScene &, Node<Dialog>) -> std::expected<void, ManagedError> {
                        return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED});
                      }));
  const auto mounted = ui::mount(*f.scene, std::move(attempt));

  check(!mounted && treeCause(mounted.error(), TreeError::CALLBACK_FAILED) && failedView.node() == Node<Dialog>{},
        "late low-level mount failure leaves the default view unpublished");

  ui::Ref<ThemeSelector> retry;

  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Retry"), ui::themeSelector(retry, {{"red", "Red", theme}}))));
  take(f.scene->prepare(*f.app, SELECTOR_VIEWPORT));
  check(take(retry.snapshot())->catalog->choices.size() == 1,
        "attempt-owned model rollback permits a fresh default install in the same existing owner");

  ui::ThemeSelectionRef explicitModel;
  ui::Ref<ThemeSelector> conflict;
  int initialFactories{};
  auto explicitConflict =
    ui::scene(ui::label("Body"), ui::themeSelector(conflict, ui::ThemeSelectorCatalog{.choices = {{"default", "Default",
                                                                                                   [&] {
                                                                                                     ++initialFactories;
                                                                                                     return theme();
                                                                                                   }}},
                                                                                      .initialId = "default"}));

  explicitConflict | ui::themeSelection(explicitModel, {{"explicit", "Explicit", theme}});

  const auto conflicted = explicitConflict.prepareChecked();

  check(!conflicted && modelCause(conflicted.error(), TreeError::INVALID_ARGUMENT) && initialFactories == 0 &&
          !conflict.snapshot(),
        "pending explicit model conflicts before the initial theme factory is invoked");

  ui::ThemeSelectionRef original;
  auto originalOwner = ui::scene(ui::label("Original"));

  originalOwner | ui::themeSelection(original, {{"original", "Original", theme}});
  take(originalOwner.prepareChecked());

  const auto before = take(original.snapshot());
  ui::Ref<ThemeSelector> cannotReplace;
  const auto replacement = ui::mount(take(originalOwner.managed()).get(),
                                     ui::themeSelector(cannotReplace, {{"replacement", "Replacement", theme}}));

  check(!replacement && modelCause(replacement.error(), TreeError::INVALID_ARGUMENT) &&
          take(original.snapshot()) == before,
        "rejected default install never removes a previous explicit model");
  take(originalOwner.close());

  ui::Ref<ThemeSelector> legacyEmpty;
  auto legacy = ui::scene(ui::label("Body"), ui::themeSelector(legacyEmpty, {}));
  const auto empty = legacy.prepareChecked();

  check(!empty && empty.error().phase == ManagedPhase::INPUT && treeCause(empty.error(), TreeError::INVALID_ARGUMENT),
        "bare braces retain the legacy unpublished-model attachment failure");
}

void initialFactoryRemovalNeverPublishes() {
  ui::Ref<ThemeSelector> selector;
  ui::Ref<Dialog> view;
  ManagedScene *managed{};
  bool configured{}, rejectedReentry{}, removed{};
  auto token = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = token;
  auto owner =
    ui::scene(ui::label("Body"),
              ui::themeSelector(
                selector, ui::ThemeSelectorCatalog{.choices = {{"remove", "Remove",
                                                                [&, token] {
                                                                  const auto opened = selector.open();
                                                                  const auto previewed = selector.preview("remove");
                                                                  const auto committed = selector.commit();

                                                                  rejectedReentry =
                                                                    configured && !opened && !previewed && !committed &&
                                                                    treeCause(opened.error(), TreeError::CLOSED) &&
                                                                    treeCause(previewed.error(), TreeError::CLOSED) &&
                                                                    treeCause(committed.error(), TreeError::CLOSED) &&
                                                                    !selector.snapshot();
                                                                  take(managed->remove(view.node()));
                                                                  removed = true;

                                                                  return theme();
                                                                }}},
                                                   .initialId = "remove"})
                .reference(view));

  owner.configure([&](ManagedScene &scene) {
    managed = &scene;
    configured = true;
  });
  token.reset();

  const auto result = owner.prepareChecked();

  check(!result && modelCause(result.error(), TreeError::STALE_NODE) && rejectedReentry && removed &&
          !selector.snapshot() && !take(selector.lastApply()) && lifetime.expired(),
        "initial factory removal rejects stale attachment and releases default model/view captures");
}

void initialFactoryTargetsItsOwnNestedScene() {
  example::ReplayTerminal innerTerminal({}, 100, SELECTOR_VIEWPORT);
  example::ReplayTerminal outerTerminal({}, 100, SELECTOR_VIEWPORT);
  ui::SceneOptions innerOptions, outerOptions;
  ui::Ref<ThemeSelector> selector;

  innerOptions.clock = innerTerminal.clock();
  outerOptions.clock = outerTerminal.clock();

  auto inner = ui::scene(innerOptions, ui::label("Inner"),
                         ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                                           [] {
                                                                                             ui::quit();

                                                                                             return theme();
                                                                                           }}},
                                                                              .initialId = "red"}));
  auto outer = ui::scene(ui::label("Outer"), outerOptions);

  outer.configure([&](ManagedScene &) {
    take(inner.prepareChecked());
  });
  take(outer.prepareChecked());
  take(inner.run(innerTerminal));
  take(outer.run(outerTerminal));
  std::cout << "nested initial factory inner-bytes=" << innerTerminal.output.size()
            << " outer-bytes=" << outerTerminal.output.size() << '\n';
  check(innerTerminal.output.empty() && !outerTerminal.output.empty(),
        "nested initial factory quit targets its own scene and restores the outer current scene");
}

void initialFactoryShortFailureStaysWithItsOwner() {
  ui::Ref<ThemeSelector> selector;
  ui::Ref<LineEdit> field;
  bool targetPublishedAtStop{};
  std::size_t innerNotifications{}, outerNotifications{}, reports{}, later{}, stopped{};
  std::optional<ManagedError> innerFailure;
  example::ReplayTerminal terminal({}, 100, SELECTOR_VIEWPORT);
  auto inner = ui::scene(
    ui::lineEdit(field, "Original"),
    ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                      [field] {
                                                                        field.text().set(std::string_view("\xff", 1));

                                                                        return theme();
                                                                      }}},
                                                         .initialId = "red"}));

  inner.onError([&](const ManagedDiagnostic &) {
    ++innerNotifications;
  });
  inner.onStop([&]() -> std::expected<void, ManagedError> {
    ++stopped;
    targetPublishedAtStop = selector.snapshot().has_value();

    return std::unexpected(ManagedError{ManagedPhase::CLEANUP, {}, TreeError::NODE_LIMIT});
  });
  inner.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    check(report.failure && report.failure->setupStopped && report.failure->cleanup == TreeError::NODE_LIMIT,
          "inner final report retains the short primary and independent stop error");
  });

  auto outer = ui::scene(ui::label("Outer"));

  outer.onError([&](const ManagedDiagnostic &) {
    ++outerNotifications;
  });
  outer.configure([&](ManagedScene &) {
    const auto result = inner.prepareChecked();

    if (!result) {
      innerFailure = result.error();
    }
  });
  outer.configure([&](ManagedScene &) {
    ++later;
  });
  take(outer.prepareChecked());
  check(innerFailure && innerFailure->setupStopped && innerFailure->operation &&
          std::string_view(innerFailure->operation) == "text.set" && innerFailure->execution &&
          std::string_view(innerFailure->execution) == "scene.themeSelector" &&
          innerFailure->cleanup == TreeError::NODE_LIMIT && !targetPublishedAtStop,
        "initial short failure stops its own attachment before commit/publication without replacing the primary");
  check(innerNotifications == 1 && outerNotifications == 0 && later == 1 && stopped == 1 &&
          inner.runApp(terminal) == 1 && terminal.output.empty() && !terminal.closed && reports == 1,
        "nested setup attribution restores its parent and reports inner failure once without terminal entry");
  take(outer.close());
}

void defaultAdmissionAllocationFailures() {
  constexpr std::ptrdiff_t MAX_ADMISSION_BUDGET = 2048;

  for (const bool initial : {false, true}) {
    std::size_t failures{}, successes{};

    for (std::ptrdiff_t budget = 0; budget <= MAX_ADMISSION_BUDGET; ++budget) {
      ui::Ref<ThemeSelector> selector;
      ui::ThemeSelectorCatalog catalog;

      catalog.choices.emplace_back("red", "Red", theme);
      if (initial) {
        catalog.initialId = "red";
      }

      auto owner = ui::scene(ui::label("Body").setup([&](ManagedScene &, Node<Label>) {
        if (!initial) {
          allocationTest::remaining = budget;
        }

        return std::expected<void, ManagedError>{};
      }),
                             ui::themeSelector(selector, catalog));

      owner.configure([&](ManagedScene &) {
        if (initial) {
          allocationTest::remaining = budget;
        }
      });

      const auto result = owner.prepareChecked();

      allocationTest::remaining = -1;

      if (!result) {
        ++failures;
        check(modelCause(result.error(), TreeError::NO_MEMORY) && !selector.snapshot() && !take(selector.lastApply()),
              "default model or initial choice allocation failure is typed and never publishes its facade");
      } else {
        ++successes;
        check(take(selector.snapshot())->lastCommittedId ==
                (initial ? std::optional<std::string>("red") : std::nullopt),
              "successful admission preserves initial choice semantics");
        take(owner.close());
        std::cout << "default selector initial=" << initial << " OOM failures=" << failures
                  << " first-success-budget=" << budget << '\n';

        break;
      }
    }

    check(failures && successes,
          "default admission sweep reaches both allocation rejection and successful publication");
  }
}

void prepareIndependent() {
  independentPrepared = bool(independentScene->prepareChecked());
}

void finalSharedTargetAdmission() {
  ui::ThemeSelectorRef shared;
  ui::ThemeSelectionRef innerModel, outerModel;
  ManagedScene *innerManaged{};
  Application *innerApp{};
  auto inner = ui::scene(ui::overlayHost(ui::label("Inner"), ui::themeSelector(shared, innerModel)));
  inner | ui::themeSelection(innerModel, {{"inner", "Inner", theme}});
  inner.configure([&](ManagedScene &scene, Application &app) {
    innerManaged = &scene;
    innerApp = &app;
  });
  auto outer = ui::scene(ui::overlayHost(ui::label("Outer"), ui::themeSelector(shared, outerModel)));
  outer | ui::themeSelection(outerModel, {{"outer", "Outer", theme}});
  independentScene = &inner;
  outer.configure([](ManagedScene &) {
    // The next allocation is inside post-configuration attach, after the cold admission check.
    allocationTest::hookAfter = 0;
    allocationTest::hook = prepareIndependent;
  });
  const auto result = outer.prepareChecked();
  allocationTest::hook = nullptr;
  independentScene = nullptr;
  check(independentPrepared && !result && treeCause(result.error(), TreeError::INVALID_ARGUMENT),
        "final shared-target validation rejects independently published view");
  take(innerManaged->prepare(*innerApp, SELECTOR_VIEWPORT));
  take(innerApp->tree().frame([&] {
    const auto bounds = take(CellRect::from({0, 0}, SELECTOR_VIEWPORT));
    take(layoutTree(innerApp->tree(), take(innerApp->tree().root()), bounds, bounds));
  }));
  take(shared.open());
  take(innerModel.preview("inner"));
  take(shared.closeKeepingPreview());
  check(take(innerModel.snapshot())->previewedId == "inner" && bool(inner.checked()),
        "failed outer cleanup preserves inner target, model and commands");
}

void externalFactoryCannotApply() {
  std::size_t callbacks{};
  ui::ThemeSelectorOptions options;
  options.onCommitted = [&](std::string_view) -> std::expected<void, ManagedError> {
    ++callbacks;
    return {};
  };
  SelectorFixture f(options);
  take(f.selector.open());
  f.choose("Red 🙂é界");
  bool nestedSucceeded{};
  std::optional<ManagedError> nestedError;
  std::optional<std::source_location> nestedOrigin;
  std::size_t nestedAllocations{};
  f.otherFactory = [&] {
    nestedOrigin = std::source_location::current();
    const auto before = allocationTest::attempts;
    const auto nested = f.selector.apply(*nestedOrigin);
    nestedAllocations = allocationTest::attempts - before;
    nestedSucceeded = bool(nested);
    if (!nested) {
      nestedError = nested.error();
    }
  };
  const auto outer = f.selector.preview("other");
  const auto *style = nestedError ? std::get_if<StyleError>(&nestedError->cause) : nullptr;
  check(!nestedSucceeded && nestedError && nestedError->phase == ManagedPhase::STYLE && style &&
          style->cause == TreeError::WOULD_BLOCK && nestedError->operationOrigin && nestedOrigin &&
          nestedError->operationOrigin->line() == nestedOrigin->line(),
        "external preview factory Apply rejects exact model busy admission");
  check(outer && callbacks == 0 && !take(f.selector.lastApply()) && f.open() &&
          take(f.selector.snapshot())->status == ui::ThemeSelectionStatus::ACTIVE &&
          take(f.selector.snapshot())->previewedId == "other",
        "rejected nested Apply preserves outer preview, callback, outcome and open view");
  check(nestedAllocations == 0, "busy model admission precedes owning outcome allocation");
}
} // namespace

int main() try {
  defaultInitialAndDelegatedCommands();
  defaultInitialOrderingAndFactoryFailure();
  defaultCatalogRejectionAndMountRollback();
  initialFactoryRemovalNeverPublishes();
  initialFactoryTargetsItsOwnNestedScene();
  initialFactoryShortFailureStaysWithItsOwner();
  defaultAdmissionAllocationFailures();
  canonicalReferences();
  lifecycleAndAdmission();
  routedCancelAndSupersession();
  callbackAndCloseOutcome();
  outcomeAllocationAndRetirement();
  stagedOpenAndCloseFailure();
  pointerAndNarrowReveal();
  interactiveReportsAndObserverRetirement();
  shortcutRetirementDuringProjectionOom();
  finalSharedTargetAdmission();
  externalFactoryCannotApply();
  return 0;
} catch (const std::exception &failure) {
  allocationTest::remaining = -1;
  std::cerr << failure.what() << '\n';
  return 1;
}
