// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
void initialAndSameFrame() {
  Fixture f;
  ui::Ref<LineEdit> input;
  ui::Ref<Label> caption;
  auto closed = f.scene->liveText(input.draftText());
  check(!closed && treeCause(closed.error(), TreeError::CLOSED), "unpublished Ref has no fake initial value");
  auto state = take(f.scene->state(std::string("initial")));
  take(ui::mount(*f.scene, ui::column(ui::lineEdit(input, state), ui::label(caption, state))));
  auto draft = take(f.scene->liveText(input.draftText()));
  auto shared = take(f.scene->liveText(input.draftText()));
  auto committed = take(f.scene->liveText(input.committedText()));
  auto label = take(f.scene->liveText(caption.captionText()));
  check(take(draft.snapshot()) == take(shared.snapshot()), "duplicate capture shares owning Property");
  auto mismatch = f.scene->liveText(input.draftText(), PropertyOptions{.max_observers = 1});
  check(!mismatch && treeCause(mismatch.error(), TreeError::INVALID_ARGUMENT), "shared capture checks options");
  int callbacks{}, custom{};
  take(f.scene->onPrepare(caption.node(), [&](Label &, Node<Widget>) {
    ++custom;
    return std::expected<void, TreeError>{};
  }));
  take(f.scene->observe(draft, [&](const GraphemeText &value) {
    ++callbacks;
    check(value.bytes() == "initial" || value.bytes() == "external", "initial and post-sync owning values");
  }));
  take(state.set("external"));
  f.prepare();
  check(take(draft.snapshot())->value.bytes() == "external" &&
          take(committed.snapshot())->value.bytes() == "external" &&
          take(label.snapshot())->value.bytes() == "external",
        "clean model reaches all text sources in same prepare");
  check(callbacks == 2 && custom == 1, "convergence does not repeat custom sync or equal callbacks");
  f.prepare();
  check(callbacks == 2 && take(draft.snapshot())->revision == 1, "equal capture does not publish or repeat");
  std::expected<std::shared_ptr<const PropertyChange<GraphemeText>>, TreeError> foreign;
  std::thread other([&] {
    foreign = draft.snapshot();
  });
  other.join();
  check(!foreign && foreign.error() == TreeError::WRONG_THREAD, "affinity checked before mutable UI metadata");
  take(f.scene->close());
  check(!draft.snapshot() && draft.snapshot().error() == TreeError::CLOSED, "live sources close explicitly");
}

void removalAndAdmittedOwners() {
  Fixture f;
  ui::Ref<Label> origin;
  take(ui::mount(*f.scene, ui::label(origin, "before")));
  auto live = take(f.scene->liveText(origin.captionText()));
  bool remove{};
  auto derived = take(f.scene->derive(live, [&](const GraphemeText &text) {
    if (remove) {
      take(f.scene->remove(origin.node()));
    }
    return std::string(text.bytes());
  }));
  int calls{};
  take(f.scene->observe(live, [&](const GraphemeText &) {
    ++calls;
  }));
  f.prepare();
  check(calls == 1, "initial observe delivers once");
  remove = true;
  take((origin.text() = "after").result());
  f.prepare();
  check(calls == 1, "later direct observer quietly retires after earlier callback removes origin");
  check(take(derived.snapshot())->value == "before", "admitted computation cannot publish after origin removal");
  check(!live.snapshot() && live.snapshot().error() == TreeError::STALE_NODE, "explicit stale source remains stale");
  auto retained = take(derived.snapshot());
  f.dispatch();
  f.prepare();
  check(retained->value == "before", "owning output survives origin retirement and cancelled queue");
}

void coldSiblingAndRollback() {
  Fixture f;
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> list;
  auto state = take(f.scene->state(std::string("one two")));
  take(ui::mount(
    *f.scene, ui::column(ui::virtualList(ui::words(input.draftText())).reference(list), ui::lineEdit(input, state))));
  check(input.node() != Node<LineEdit>{} && list.node() != Node<VirtualList<TextSegment>>{},
        "cold list resolves later sibling Ref before public publication");
  f.prepare();
  ui::Ref<LineEdit> rejected;
  auto bad =
    ui::mount(*f.scene, ui::column(ui::lineEdit(rejected, state),
                                   ui::virtualList(ui::words(rejected.draftText(), TextSplitOptions{.maxTokens = 1}))));
  check(!bad && rejected.node() == Node<LineEdit>{}, "initial transform failure rolls back nodes and Ref");
  auto normal = take(f.scene->state(7));
  check(take(normal.snapshot())->value == 7, "failed cold setup does not poison ordinary runtime");
}

void explicitDraftAndRejectedNotices() {
  Fixture f({}, 1);
  ui::Ref<LineEdit> input;
  auto state = take(f.scene->state(std::string("accepted")));
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  take(ui::mount(*f.scene, ui::lineEdit(input, state, config)));
  auto draft = take(f.scene->liveText(input.draftText()));
  auto committed = take(f.scene->liveText(input.committedText()));
  int calls{};
  take(f.scene->observe(draft, [&](const GraphemeText &) {
    ++calls;
  }));
  f.prepare();
  auto filler = take(f.scene->state(0));
  take(f.scene->observe(filler, [](const int &) {
  }));
  f.prepare();
  take(filler.set(1));
  take((input.text() = "draft").result());
  f.prepare();
  check(take(draft.snapshot())->value.bytes() == "draft" && take(committed.snapshot())->value.bytes() == "accepted",
        "explicit dirty draft and accepted text remain distinct");
  check(calls == 2, "latest source recovers after saturated notification lane");
  f.dispatch();
  f.prepare();
  check(calls == 2, "queued duplicate cannot repeat recovered successful revision");
}

void typedCaptureErrors() {
  Fixture f;
  ui::Ref<LineEdit> input;
  auto state = take(f.scene->state(std::string("safe")));
  take(ui::mount(*f.scene, ui::lineEdit(input, state)));
  auto live = take(f.scene->liveText(input.draftText()));
  take(state.set(std::string("\xff", 1)));
  auto failed = f.scene->prepare(*f.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::INVALID_ARGUMENT, "standard sync propagates original typed failure");
  const auto history = take(f.scene->diagnostics());
  check(history.errors.size() == 1 && history.errors.back().error.text == TextError{TextErrorKind::INVALID_UTF8, 0},
        "standard sync enriches one diagnostic from the exact failed model snapshot");
  check(take(live.snapshot())->value.bytes() == "safe", "capture failure keeps last good source");
  auto invalid = f.scene->liveText(input.committedText());
  check(!invalid && invalid.error().text == TextError{TextErrorKind::INVALID_UTF8, 0},
        "initial capture retains text category");
  take(state.set("repaired"));
  f.prepare();
  check(take(live.snapshot())->value.bytes() == "repaired",
        "explicit later prepare uses repaired model without recovery timer");
}

void originTailAndAdmittedFailure() {
  Fixture f;
  ui::Ref<Label> origin;
  take(ui::mount(*f.scene, ui::label(origin, "one")));
  auto live = take(f.scene->liveText(origin.captionText()));
  auto trigger = take(f.scene->state(0));
  int first{}, second{};
  take(f.scene->observe(live, [&](const GraphemeText &) {
    ++first;
  }));
  bool retire{};
  take(f.scene->observe(trigger, [&](const int &) {
    if (retire) {
      take(f.app->tree().remove(origin.node()));
    }
    ++second;
  }));
  f.prepare();
  retire = true;
  take(trigger.set(1));
  f.prepare();
  check(first == 1 && second == 2, "tail scan quietly retires earlier origin before weak property revision read");
  check(take(f.scene->diagnostics()).errors.empty(), "expected origin retirement records no error");

  Fixture admitted;
  ui::Ref<Label> source;
  take(ui::mount(*admitted.scene, ui::label(source, "safe")));
  auto value = take(admitted.scene->liveText(source.captionText()));
  take(admitted.scene->observe(value, [&](const GraphemeText &) -> std::expected<void, TreeError> {
    take(admitted.scene->remove(source.node()));
    return std::unexpected(TreeError::NODE_LIMIT);
  }));
  auto failed = admitted.scene->prepare(*admitted.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::NODE_LIMIT, "real admitted callback error survives self removal");
  check(take(admitted.scene->diagnostics()).errors.size() == 1, "admitted failure is reported once");
}

void liveCaptureAllocationRollback() {
  std::size_t allocations{};
  {
    Fixture f;
    ui::Ref<Label> input;
    take(ui::mount(*f.scene, ui::label(input, "a sufficiently long caption for copied text ownership")));
    const auto before = allocationTest::count;
    auto live = take(f.scene->liveText(input.captionText()));
    allocations = allocationTest::count - before;
  }
  for (std::size_t i = 0; i < allocations; ++i) {
    ManagedOptions budget;
    budget.maxStates = 1;
    Fixture f(budget);
    ui::Ref<Label> input;
    take(ui::mount(*f.scene, ui::label(input, "a sufficiently long caption for copied text ownership")));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(i);
    auto failed = f.scene->liveText(input.captionText());
    allocationTest::remaining = -1;
    if (failed || !treeCause(failed.error(), TreeError::NO_MEMORY)) {
      std::cerr << "live sweep index=" << i << " total=" << allocations << " succeeded=" << failed.has_value();
      if (!failed) {
        std::cerr << " cause=" << static_cast<int>(std::get<TreeError>(failed.error().cause));
      }
      std::cerr << '\n';
    }
    check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY),
          "live initial allocation fails without publication");
    auto valid = take(f.scene->liveText(input.captionText()));
    check(take(valid.snapshot())->revision == 0, "failed initial capture releases all state budgets");
    take(f.app->tree().remove(input.node()));
    auto reused = take(f.scene->state(9));
    check(take(reused.snapshot())->value == 9, "low-level origin retirement reclaims capture state budget");
  }
  std::cout << "live initial allocation points: " << allocations << '\n';
}

void frozenListAfterRetirement() {
  ManagedOptions budget;
  budget.maxModels = 1;
  budget.maxStates = 3;
  Fixture f(budget);
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> list;
  auto state = take(f.scene->state(std::string("one two three four five six")));
  int callbacks{};
  ui::VirtualListConfig<TextSegment> config;
  config.prepared = [&](const ModelFrame<TextSegment> &, const std::optional<ModelRequest<TextSegment>> &) {
    ++callbacks;
    return std::expected<void, TreeError>{};
  };
  take(
    ui::mount(*f.scene, ui::column(ui::lineEdit(input, state),
                                   ui::virtualList(ui::words(input.draftText()), config).reference(list).height(2))));
  f.prepare();
  prepareViews(f, {30, 4});
  const int before = callbacks;
  take(setVirtualListSelection(f.app->tree(), list.node(), RowId{2, 1}));
  take(f.app->tree().remove(input.node()));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), list.node(), 4));
  prepareViews(f, {40, 8});
  std::optional<RowId> selected;
  RowRange visible;
  take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
    selected = view.selection();
    visible = view.visibleRange();
  }));
  check(selected == RowId{2, 1} && visible.first == 4 && visible.count == 2,
        "frozen owning collection supports resize and scroll beyond old prepared viewport");
  check(callbacks == before, "retirement never invokes old collection callbacks");
  auto reused = take(f.scene->state(8));
  check(take(reused.snapshot())->value == 8, "private cold input/output state budgets are reclaimed");
  auto rows = take(RowCollection<int>::from(std::span<const ModelRow<int>>{}));
  auto ordinary = take(f.scene->model<int>(std::make_shared<portals::detail::CollectionSnapshot<int>>(rows, 1)));
  check(ordinary.publisher().has_value(), "retired private model releases scene model budget");
  auto frozenLayout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root()));
  take(setVirtualListFirst(f.app->tree(), list.node(), 0));
  allocationTest::remaining = 0;
  auto failedFrozen = f.scene->prepareViews(*f.app, frozenLayout);
  allocationTest::remaining = -1;
  check(!failedFrozen && failedFrozen.error() == TreeError::NO_MEMORY,
        "frozen view preparation retains typed allocation error");
  take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
    check(view.visibleRange().first == 4 && view.selection() == RowId{2, 1},
          "failed frozen preparation retains prior prepared rows and selection");
  }));
  take(f.scene->prepareViews(*f.app, frozenLayout));
}

void receiverRemovalAndColdRollbackAllocations() {
  ManagedOptions budget;
  budget.maxStates = 3;
  budget.maxModels = 1;
  Fixture f(budget);
  ui::Ref<Label> input;
  ui::Ref<VirtualList<TextSegment>> list;
  take(ui::mount(*f.scene, ui::label(input, "one two")));
  take(ui::mount(*f.scene, ui::virtualList(ui::words(input.captionText())).reference(list)));
  f.prepare();
  take(f.scene->remove(list.node()));
  auto state = take(f.scene->state(4));
  check(take(state.snapshot())->value == 4 && take(input.captionText().snapshot()).bytes() == "one two",
        "removing only receiver reclaims private graph without removing original widget");

  std::size_t allocations{};
  {
    Fixture baseline;
    ui::Ref<LineEdit> editor;
    auto model = take(baseline.scene->state(std::string("one two")));
    auto recipe = ui::column(ui::lineEdit(editor, model), ui::virtualList(ui::words(editor.draftText())));
    const auto before = allocationTest::count;
    auto root = ui::mount(*baseline.scene, std::move(recipe));
    allocations = allocationTest::count - before;
    check(root.has_value(), "calibrate complete cold mount");
  }
  std::size_t failures{};
  for (std::size_t i = 0; i < allocations; ++i) {
    ManagedOptions limits;
    limits.maxStates = 3;
    limits.maxModels = 1;
    Fixture attempt(limits);
    ui::Ref<LineEdit> editor;
    auto model = take(attempt.scene->state(std::string("one two")));
    auto recipe = ui::column(ui::lineEdit(editor, model), ui::virtualList(ui::words(editor.draftText())));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(i);
    auto result = ui::mount(*attempt.scene, std::move(recipe));
    allocationTest::remaining = -1;
    if (result) {
      // Best-effort style/diagnostic allocation may be nonessential; any success must be a complete graph.
      check(editor.node() != Node<LineEdit>{}, "successful mount publishes complete Ref");
      take(attempt.scene->remove(*result));
    } else {
      ++failures;
      if (!treeCause(result.error(), TreeError::NO_MEMORY)) {
        std::cerr << "cold sweep index=" << i
                  << " cause=" << static_cast<int>(std::get<TreeError>(result.error().cause)) << '\n';
      }
      check(treeCause(result.error(), TreeError::NO_MEMORY), "cold allocation failure retains typed NO_MEMORY");
      check(editor.node() == Node<LineEdit>{}, "failed complete mount never publishes editor Ref");
    }
    ui::Ref<LineEdit> valid;
    take(
      ui::mount(*attempt.scene, ui::column(ui::lineEdit(valid, model), ui::virtualList(ui::words(valid.draftText())))));
    attempt.prepare();
    check(take(model.snapshot())->value == "one two",
          "cold rollback preserves external state and reclaims all private budgets");
  }
  check(failures > 0, "cold allocation sweep exercised real failure paths");
  std::cout << "cold mount allocation points: " << allocations << " rejected: " << failures << '\n';
}

void runtimeCaptureAllocationBoundaries() {
  const std::string replacement(300, 'x');
  std::size_t allocations{};
  {
    Fixture f;
    ui::Ref<Label> input;
    take(ui::mount(*f.scene, ui::label(input, "before")));
    auto source = take(f.scene->liveText(input.captionText()));
    take(f.scene->observe(source, [](const GraphemeText &) {
    }));
    f.prepare();
    take((input.text() = replacement).result());
    const auto before = allocationTest::count;
    f.prepare();
    allocations = allocationTest::count - before;
  }
  std::size_t beforeCommit{}, afterCommit{};
  for (std::size_t i = 0; i < allocations; ++i) {
    Fixture f;
    ui::Ref<Label> input;
    take(ui::mount(*f.scene, ui::label(input, "before")));
    auto source = take(f.scene->liveText(input.captionText()));
    take(f.scene->observe(source, [](const GraphemeText &) {
    }));
    f.prepare();
    take((input.text() = replacement).result());
    allocationTest::remaining = static_cast<std::ptrdiff_t>(i);
    auto result = f.scene->prepare(*f.app, VIEWPORT);
    allocationTest::remaining = -1;
    if (!result) {
      check(result.error() == TreeError::NO_MEMORY, "runtime allocation preserves typed NO_MEMORY");
      if (take(source.snapshot())->revision == 0) {
        ++beforeCommit;
      } else {
        ++afterCommit;
      }
    }
    f.prepare();
    check(take(source.snapshot())->value.bytes() == replacement && take(source.snapshot())->revision == 1,
          "explicit retry resumes committed/latest text without duplicate source revision");
  }
  check(beforeCommit > 0 && afterCommit > 0, "runtime sweep covers both sides of the Property publication boundary");
  std::cout << "runtime capture points: " << allocations << " precommit failures: " << beforeCommit
            << " postcommit failures: " << afterCommit << '\n';
}

void resetIdsUndoAndIdle() {
  Fixture f;
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> list;
  auto state = take(f.scene->state(std::string("one two")));
  std::optional<ModelFrame<TextSegment>> frame;
  ui::VirtualListConfig<TextSegment> config;
  config.prepared = [&](const ModelFrame<TextSegment> &value, const std::optional<ModelRequest<TextSegment>> &) {
    frame = value;
    return std::expected<void, TreeError>{};
  };
  take(
    ui::mount(*f.scene, ui::column(ui::lineEdit(input, state),
                                   ui::virtualList(ui::words(input.draftText()), config).reference(list).height(2))));
  f.prepare();
  prepareViews(f);
  check(take(frame->locateRow({1, 1})).state == RowLookupState::FOUND, "initial token generation starts at one");
  take(setVirtualListSelection(f.app->tree(), list.node(), RowId{1, 1}));
  take((input.text() = " one  two ").result());
  f.prepare();
  prepareViews(f);
  std::optional<RowId> selection;
  take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
    selection = view.selection();
  }));
  check(selection == RowId{1, 1} && take(frame->locateRow({1, 1})).state == RowLookupState::FOUND,
        "equal ordered tokens retain generation despite delimiter offset changes");
  auto retained = *frame;
  take((input.text() = "one two three").result());
  f.prepare();
  prepareViews(f);
  take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
    selection = view.selection();
  }));
  check(!selection && take(frame->locateRow({1, 2})).state == RowLookupState::FOUND,
        "changed token sequence receives new IDs and clears missing selection");
  take(input.undo().result());
  f.prepare();
  prepareViews(f);
  check(take(frame->locateRow({1, 3})).state == RowLookupState::FOUND &&
          take(retained.locateRow({1, 1})).state == RowLookupState::FOUND,
        "Undo generates fresh IDs while retained earlier pin keeps its original identities");

  Fixture idle;
  ui::Ref<LineEdit> editor;
  auto model = take(idle.scene->state(std::string("one two")));
  take(ui::mount(*idle.scene, ui::column(ui::lineEdit(editor, model),
                                         ui::virtualList(ui::graphemes(editor.draftText())).height(2))));
  auto stats = take(idle.scene->run(idle.terminal));
  check(stats.frames == 1, "live text graph introduces no idle redraw or recovery timer");
}

void modelApplicationAllocationRollback() {
  std::size_t allocations{};
  std::size_t failures{};
  const auto exercise = [&](std::optional<std::size_t> failAt) {
    Fixture f;
    ui::Ref<LineEdit> input;
    ui::Ref<VirtualList<TextSegment>> list;
    auto state = take(f.scene->state(std::string("one two")));
    std::optional<ModelFrame<TextSegment>> frame;
    ui::VirtualListConfig<TextSegment> config;
    config.prepared = [&](const ModelFrame<TextSegment> &value, const std::optional<ModelRequest<TextSegment>> &) {
      frame = value;
      return std::expected<void, TreeError>{};
    };
    take(
      ui::mount(*f.scene, ui::column(ui::lineEdit(input, state),
                                     ui::virtualList(ui::words(input.draftText()), config).reference(list).height(2))));
    f.prepare();
    prepareViews(f);
    auto retained = *frame;
    take(setVirtualListSelection(f.app->tree(), list.node(), RowId{1, 1}));
    take((input.text() = "one two three").result());
    const auto count = allocationTest::count;
    if (failAt) {
      allocationTest::remaining = static_cast<std::ptrdiff_t>(*failAt);
    }
    auto result = f.scene->prepare(*f.app, VIEWPORT);
    allocationTest::remaining = -1;
    if (!failAt) {
      allocations = allocationTest::count - count;
    }
    if (!result) {
      ++failures;
      check(result.error() == TreeError::NO_MEMORY, "model staging reports allocation error before a successful frame");
      take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
        check(view.selection() == RowId{1, 1}, "failed model prepare cannot change prior visible selection");
      }));
    }
    f.prepare();
    prepareViews(f);
    check(
      frame->dataRevision() == 2 && take(frame->locateRow({1, 2})).state == RowLookupState::FOUND,
      "model failure preserves cursor/generation until successful application; retry never skips or double advances");
    check(take(retained.locateRow({1, 1})).state == RowLookupState::FOUND,
          "older owning model pin survives every staging failure and successful replacement");
  };
  exercise({});
  for (std::size_t i = 0; i < allocations; ++i) {
    exercise(i);
  }
  check(failures > 0, "model allocation sweep exercised real application failures");
  std::cout << "collection runtime staging points: " << allocations << " failures: " << failures << '\n';
}

void frozenRetryAndCaptureRetirement() {
  for (bool retryBeforeRemoval : {false, true}) {
    Fixture f;
    ui::Ref<Label> origin;
    ui::Ref<VirtualList<TextSegment>> list;
    auto captured = std::make_shared<int>(9);
    std::weak_ptr<int> weak = captured;
    int callbacks{};
    ui::VirtualListConfig<TextSegment> config;
    config.prepared = [&, captured](const ModelFrame<TextSegment> &, const std::optional<ModelRequest<TextSegment>> &) {
      ++callbacks;
      check(!weak.expired() && *captured == 9, "admitted prepared callback retains its own captures");
      return std::expected<void, TreeError>{};
    };
    captured.reset();
    take(ui::mount(
      *f.scene,
      ui::column(ui::label(origin, "one two three four five six"),
                 ui::virtualList(ui::words(origin.captionText()), std::move(config)).reference(list).height(2))));
    config.prepared = {};
    f.prepare();
    prepareViews(f);
    check(callbacks == 1 && !weak.expired(), "live prepared registration owns callback until retirement");
    take(setVirtualListSelection(f.app->tree(), list.node(), RowId{2, 1}));
    if (retryBeforeRemoval) {
      take(retryVirtualList(f.app->tree(), list.node()));
    }
    take(f.scene->remove(origin.node()));
    check(weak.expired(), "origin retirement releases obsolete prepared capture while receiver remains live");
    f.prepare();
    if (!retryBeforeRemoval) {
      take(retryVirtualList(f.app->tree(), list.node()));
    }
    prepareViews(f, {40, 8});
    take(setVirtualListFirst(f.app->tree(), list.node(), 4));
    take(retryVirtualList(f.app->tree(), list.node()));
    prepareViews(f, {50, 10});
    take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
      check(view.visibleRange().first == 4 && view.visibleRange().count == 2 && view.selection() == RowId{2, 1},
            "local retry before/after detach preserves frozen resize/scroll and offscreen stable selection");
    }));
    check(callbacks == 1 && weak.expired(), "frozen retry never invokes or retains old prepared callback");
  }
}

void preparedCallbackOriginRemoval() {
  for (bool typedFailure : {false, true}) {
    Fixture f;
    ui::Ref<Label> origin;
    ui::Ref<VirtualList<TextSegment>> list;
    auto captured = std::make_shared<int>(7);
    std::weak_ptr<int> weak = captured;
    int calls{};
    ui::VirtualListConfig<TextSegment> config;
    config.prepared = [&,
                       captured](const ModelFrame<TextSegment> &,
                                 const std::optional<ModelRequest<TextSegment>> &) -> std::expected<void, TreeError> {
      ++calls;
      take(f.app->tree().remove(origin.node()));
      check(!weak.expired() && *captured == 7, "low-level removal cannot destroy an executing prepared callable");
      return typedFailure ? std::expected<void, TreeError>(std::unexpected(TreeError::NODE_LIMIT))
                          : std::expected<void, TreeError>{};
    };
    captured.reset();
    take(ui::mount(
      *f.scene,
      ui::column(ui::label(origin, "one two three four five six"),
                 ui::virtualList(ui::words(origin.captionText()), std::move(config)).reference(list).height(2))));
    config.prepared = {};
    f.prepare();
    std::optional<LayoutSnapshot> layout;
    take(f.app->tree().frame([&] {
      const auto bounds = take(CellRect::from({0, 0}, VIEWPORT));
      take(layoutTree(f.app->tree(), take(f.app->tree().root()), bounds, bounds));
      layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root()));
    }));
    auto result = f.scene->prepareViews(*f.app, *layout);
    check(typedFailure ? (!result && result.error() == TreeError::NODE_LIMIT) : result.has_value(),
          "admitted prepared result survives origin removal unchanged");
    check(weak.expired() && calls == 1, "obsolete capture is released after admitted return with live receiver");
    f.prepare();
    take(retryVirtualList(f.app->tree(), list.node()));
    take(setVirtualListFirst(f.app->tree(), list.node(), 4));
    prepareViews(f, {40, 8});
    take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
      check(view.visibleRange().first == 4, "frozen complete data still formats after admitted origin removal");
    }));
    check(calls == 1, "no old callback after admitted retirement");
  }
}

void correctedLiveInputRecovery() {
  for (int sourceKind = 0; sourceKind < 4; ++sourceKind) {
    const bool modelEdit = sourceKind != 0;
    Fixture f;
    ui::Ref<LineEdit> input;
    ui::Ref<Label> caption;
    ui::Ref<VirtualList<TextSegment>> list;
    auto state = take(f.scene->state(std::string("one two")));
    take(ui::mount(*f.scene, ui::column(ui::virtualList(ui::words(sourceKind == 2   ? caption.captionText()
                                                                  : sourceKind == 3 ? input.committedText()
                                                                                    : input.draftText(),
                                                                  TextSplitOptions{.maxTokens = 2}))
                                          .reference(list),
                                        ui::lineEdit(input, state), ui::label(caption, state))));
    int custom{};
    take(f.scene->onPrepare(input.node(), [&](LineEdit &, Node<Widget>) {
      ++custom;
      return std::expected<void, TreeError>{};
    }));
    f.prepare();
    prepareViews(f);
    const auto edit = [&](const char *text) {
      if (modelEdit) {
        take(state.set(text));
      } else {
        take((input.text() = text).result());
      }
    };
    edit("one two three");
    auto failed = f.scene->prepare(*f.app, VIEWPORT);
    check(!failed && failed.error() == TreeError::NODE_LIMIT, "invalid live words retain exact typed error");
    check(custom == 2, "failed live computation follows exactly one custom sync");
    edit("three four");
    f.prepare();
    prepareViews(f);
    check(custom == 3, "corrected source reaches capture without repeating arbitrary custom sync");
    take(f.app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
      check(view.visibleRange().count == 2, "same-scene recovery publishes corrected collection");
    }));
    f.prepare();
    check(custom == 4, "recovered live graph converges and subsequent prepare remains usable");
  }
}

void failedLiveChainRecoveryAndRetirement() {
  Fixture f;
  ui::Ref<Label> origin;
  take(ui::mount(*f.scene, ui::label(origin, "good")));
  auto live = take(f.scene->liveText(origin.captionText()));
  auto first = take(f.scene->derive(live, [](const GraphemeText &value) {
    return std::string(value.bytes());
  }));
  int attempts{};
  bool reject = true;
  auto second = take(f.scene->derive(first, [&](const std::string &value) -> std::expected<std::string, TreeError> {
    ++attempts;
    if (reject && value == "bad") {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    return value;
  }));
  f.prepare();
  take((origin.text() = "bad").result());
  auto failed = f.scene->prepare(*f.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::NODE_LIMIT && attempts == 2, "chain retains first typed failure");
  failed = f.scene->prepare(*f.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::NODE_LIMIT && attempts == 3,
        "unchanged invalid chain retries once per explicit prepare");
  take((origin.text() = "corrected").result());
  f.prepare();
  check(take(second.snapshot())->value == "corrected" && attempts == 4,
        "weak scheduling provenance spans ordinary owning derived outputs");
  take((origin.text() = "bad").result());
  failed = f.scene->prepare(*f.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::NODE_LIMIT, "second chain failure retained");
  take(f.scene->remove(origin.node()));
  reject = false;
  f.prepare();
  check(take(second.snapshot())->value == "bad" && attempts == 6,
        "origin retirement leaves ordinary derived retry independent and owning");
}

void liveRecoveryPreservesOrdinaryErrorsAndAdmittedRemoval() {
  Fixture f;
  ui::Ref<Label> origin;
  take(ui::mount(*f.scene, ui::label(origin, "good")));
  auto live = take(f.scene->liveText(origin.captionText()));
  bool reject{};
  bool remove{};
  int calls{};
  auto derived = take(f.scene->derive(live, [&](const GraphemeText &value) -> std::expected<std::string, TreeError> {
    ++calls;
    if (reject) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    if (remove) {
      take(f.scene->remove(origin.node()));
    }
    return std::string(value.bytes());
  }));
  auto state = take(f.scene->state(0));
  bool ordinaryReject{};
  take(f.scene->observe(state, [&](const int &) -> std::expected<void, TreeError> {
    return ordinaryReject ? std::expected<void, TreeError>(std::unexpected(TreeError::NO_MEMORY))
                          : std::expected<void, TreeError>{};
  }));
  f.prepare();
  reject = true;
  take((origin.text() = "bad").result());
  auto failed = f.scene->prepare(*f.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::NODE_LIMIT && calls == 2, "live failure before correction");
  reject = false;
  ordinaryReject = true;
  take(state.set(1));
  take((origin.text() = "corrected").result());
  failed = f.scene->prepare(*f.app, VIEWPORT);
  check(!failed && failed.error() == TreeError::NO_MEMORY && calls == 2,
        "unrelated ordinary recovery error still precedes built-in sync/capture");
  ordinaryReject = false;
  remove = true;
  f.prepare();
  check(calls == 3 && take(derived.snapshot())->value == "good",
        "recovered admitted computation survives removal but cannot publish through retired origin");
  f.prepare();
  check(calls == 3, "removed direct live computation quietly detaches after recovery");
}
} // namespace

int main() {
  try {
    initialAndSameFrame();
    removalAndAdmittedOwners();
    coldSiblingAndRollback();
    explicitDraftAndRejectedNotices();
    typedCaptureErrors();
    originTailAndAdmittedFailure();
    liveCaptureAllocationRollback();
    frozenListAfterRetirement();
    receiverRemovalAndColdRollbackAllocations();
    runtimeCaptureAllocationBoundaries();
    resetIdsUndoAndIdle();
    modelApplicationAllocationRollback();
    frozenRetryAndCaptureRetirement();
    preparedCallbackOriginRemoval();
    correctedLiveInputRecovery();
    failedLiveChainRecoveryAndRetirement();
    liveRecoveryPreservesOrdinaryErrorsAndAdmittedRemoval();
  } catch (const std::exception &error) {
    allocationTest::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
