// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
using LegacyBind = std::expected<LineEditBinding, TreeError> (*)(WidgetTree &, EventRouter &, Node<LineEdit>,
                                                                 std::shared_ptr<Property<std::string>>,
                                                                 std::function<void(LineEditResult)>,
                                                                 std::shared_ptr<Clipboard>,
                                                                 std::function<bool(std::string_view)>);
static_assert(std::same_as<decltype(&bindLineEdit), LegacyBind>);
constexpr SchedulerTime LEGACY_TIMEOUT{731};
constexpr ManagedOptions LEGACY_PREFIX{4096, 256, 64, {}, 64, 256, 64, LEGACY_TIMEOUT};
static_assert(LEGACY_PREFIX.keySequenceTimeout == LEGACY_TIMEOUT);
static_assert(LEGACY_PREFIX.maxEditingHandlers == 256 && LEGACY_PREFIX.maxEditingHandlersPerNode == 64);

struct EditorFixture {
  Fixture owner;
  ManagedState<std::string> model = take(owner.scene->state(std::string("base")));
  ui::Ref<LineEdit> field;
  ui::Ref<Button> other;
  std::vector<LineEditResult> legacy;
  std::vector<TextEditedEvent> edited;
  std::vector<EditingFinishedEvent> finished;
  int validations{};
  std::function<void(LineEditResult)> legacyHook;

  explicit EditorFixture(EditMode mode = EditMode::EXPLICIT, std::size_t capacity = 8) : owner({}, capacity) {
    ui::LineEditConfig config;

    config.options.editMode = mode;
    config.options.limits.max_input_bytes = 32;
    config.validate = [&](std::string_view text) {
      ++validations;

      return text != "base!";
    };
    config.result = [&](LineEditResult result) -> std::expected<void, TreeError> {
      legacy.push_back(result);

      if (legacyHook) {
        legacyHook(result);
      }

      return {};
    };
    take(ui::mount(*owner.scene, ui::column(ui::lineEdit(field, model, config), ui::button(other, "other", [] {
                                            }))));
    take(owner.scene->onTextEdited(field.node(), [&](const TextEditedEvent &event) {
      check(!legacy.empty() && legacy.back().status == event.result.status, "legacy result precedes typed edit");
      edited.push_back(event);
    }));
    take(owner.scene->onEditingFinished(field.node(), [&](const EditingFinishedEvent &event) {
      finished.push_back(event);
    }));
    focus();
  }

  void focus() {
    take(owner.app->router().setFocus(field.node()));
  }

  void blur() {
    take(owner.app->router().setFocus(other.node()));
  }

  void enter() {
    take(owner.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  }

  void type(char32_t scalar) {
    take(owner.app->router().dispatch(TextInputEvent{scalar}));
  }
};

void modeMatrix() {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    EditorFixture f(mode);
    f.blur();
    check(f.finished.size() == 1 && f.finished.back().reason == EditingFinishedReason::FOCUS_LOST &&
            f.finished.back().outcome == EditingFinishedOutcome::UNCHANGED && !f.finished.back().result &&
            f.validations == 0,
          "clean blur has one completion with no attempt in every mode");
    f.focus();
    f.type(U'x');
    check(f.edited.size() == 1, "actual accepted user delta produces one event");
    const auto beforeBlur = f.validations;
    f.blur();
    const auto expected = mode == EditMode::EXPLICIT    ? EditingFinishedOutcome::DEFERRED
                          : mode == EditMode::IMMEDIATE ? EditingFinishedOutcome::UNCHANGED
                                                        : EditingFinishedOutcome::APPLIED;
    check(f.finished.back().outcome == expected, "dirty/clean blur follows mode's existing publication policy");
    check(f.validations == beforeBlur + (mode == EditMode::ON_COMPLETION ? 1 : 0), "blur never adds a second attempt");
    check(bool(f.finished.back().result) == (mode == EditMode::ON_COMPLETION), "only attempted blur owns a receipt");
    f.focus();
    const auto beforeEnter = f.validations;
    f.enter();
    check(f.finished.back().reason == EditingFinishedReason::ENTER && f.finished.back().result &&
            f.finished.back().outcome ==
              (mode == EditMode::EXPLICIT ? EditingFinishedOutcome::APPLIED : EditingFinishedOutcome::UNCHANGED) &&
            f.validations == beforeEnter + 1,
          "Enter reuses one exact commit attempt in every mode");
    check(f.finished.back().result->commit->snapshot == f.legacy.back().commit->snapshot,
          "typed finish retains the identical owning receipt snapshot");
    check(take(f.owner.app->router().focused()) == f.field.node(), "Enter does not move focus");
  }
}

void deltasAndRejections() {
  EditorFixture f;
  take((f.field.text() = "fluent").result());
  check(f.edited.empty(), "fluent writes are not user edits");
  take(f.owner.app->router().invoke(f.field.node(), {Action::MOVE_LEFT}));
  take(f.owner.app->router().invoke(f.field.node(), {Action::MOVE_LEFT, {.shift = true}}));
  check(f.edited.empty(), "cursor/selection changes are not text deltas");
  take(f.field.undo().result());
  take(f.field.redo().result());
  check(f.edited.size() == 2, "direct Undo/Redo participate in typed user edits");
  take((f.field.text() = "").result());
  const auto before = f.edited.size();
  take(f.owner.app->router().invoke(f.field.node(), {Action::ERASE_BACKWARD}));
  check(f.edited.size() == before, "empty deletion does not produce a delta");
  take(f.owner.app->router().dispatch(PasteInputEvent{std::string(40, 'x')}));
  check(f.edited.size() == before, "rejected over-limit paste has no text edit event");
  f.type(U'x');
  take(f.field.clear().result());
  check(f.edited.size() == before + 2, "typing and clear each publish an actual delta");
  take(f.field.clear().result());
  check(f.edited.size() == before + 2, "empty clear does not publish another delta");

  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    EditorFixture rejected(mode);
    rejected.type(U'!');
    check(rejected.edited.size() == 1, "accepted rejected draft is still a user delta");
    if (mode == EditMode::IMMEDIATE) {
      check(rejected.edited.front().result.status == LineEditStatus::VALIDATION_FAILED,
            "immediate validator rejection retains the accepted delta's exact result");
    }
    rejected.enter();
    check(rejected.finished.back().outcome == EditingFinishedOutcome::VALIDATION_REJECTED &&
            rejected.finished.back().result->status == LineEditStatus::VALIDATION_FAILED,
          "rejected Enter delivers completion without rewriting its failure");
    rejected.blur();
    rejected.focus();
    check(take(rejected.field.text().get()) == "base!" && take(rejected.model.snapshot())->value == "base",
          "blur/refocus preserve rejected draft and last accepted model");
  }
}

void cancellationDeltas() {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    EditorFixture f(mode);
    take((f.field.text() = "draft").result());
    const auto expected = mode == EditMode::IMMEDIATE ? 0U : 1U;
    take(f.owner.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    check(f.edited.size() == expected && f.finished.empty(), "Escape reports only its real user delta, not completion");
    if (expected) {
      check(f.edited.back().result.status == LineEditStatus::CANCELLED, "Escape preserves its cancellation receipt");
    }
    take(f.owner.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
    check(f.edited.size() == expected, "equal-value Escape emits no second text change");
  }
}

void conflictAndTechnicalFailure() {
  EditorFixture f;
  f.type(U'x');
  take(f.model.set("external"));
  f.enter();
  check(f.finished.back().outcome == EditingFinishedOutcome::CONFLICT &&
          f.finished.back().result->status == LineEditStatus::CONFLICT && f.validations == 0,
        "conflict completion reuses pre-validator rejection");

  Fixture owner;
  const auto model = take(owner.scene->state(std::string("value")));
  ui::Ref<LineEdit> field;
  ui::LineEditConfig config;
  std::optional<EditingFinishedEvent> event;
  config.validate = [](std::string_view) -> bool {
    throw std::runtime_error("validator");
  };
  take(ui::mount(*owner.scene, ui::lineEdit(field, model, config)));
  take(owner.scene->onEditingFinished(field.node(), [&](const EditingFinishedEvent &value) {
    event = value;
  }));
  take(owner.app->router().setFocus(field.node()));
  take(owner.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  check(event && event->outcome == EditingFinishedOutcome::FAILED && event->result->error == TreeError::CALLBACK_FAILED,
        "technical failure still emits completion with original typed error");
}

void callbackLifetimeAndFocus() {
  EditorFixture textBlur(EditMode::ON_COMPLETION);
  take(textBlur.owner.scene->onTextEdited(textBlur.field.node(), [&] {
    textBlur.blur();
  }));
  textBlur.type(U'x');
  check(textBlur.finished.size() == 1 && textBlur.validations == 1 &&
          textBlur.finished.front().outcome == EditingFinishedOutcome::APPLIED &&
          textBlur.finished.front().reason == EditingFinishedReason::FOCUS_LOST,
        "text edit callback blur performs its first completion attempt exactly once");

  EditorFixture f(EditMode::ON_COMPLETION);
  f.type(U'x');
  take(f.owner.scene->onEditingFinished(f.field.node(), [&](const EditingFinishedEvent &) {
    f.blur();
  }));
  f.enter();
  check(f.finished.size() == 1 && f.validations == 1 && take(f.owner.app->router().focused()) == f.other.node(),
        "Enter callback focus change publishes focus without duplicate attempt/completion");
  f.focus();
  f.blur();
  check(f.finished.size() == 2 && f.finished.back().reason == EditingFinishedReason::FOCUS_LOST,
        "later independent focus loss remains observable");

  EditorFixture removed;
  int afterRemoval{};
  take(removed.owner.scene->onTextEdited(removed.field.node(), [&] {
    take(removed.owner.scene->remove(removed.field.node()));
  }));
  take(removed.owner.scene->onTextEdited(removed.field.node(), [&] {
    ++afterRemoval;
  }));
  removed.type(U'x');
  check(afterRemoval == 0, "self-removal safely stops remaining typed chain");

  EditorFixture legacyRemoved;
  legacyRemoved.legacyHook = [&](LineEditResult) {
    take(legacyRemoved.owner.scene->remove(legacyRemoved.field.node()));
  };
  legacyRemoved.type(U'x');
  check(legacyRemoved.edited.empty(), "legacy callback removal suppresses typed event delivery");

  EditorFixture appended;
  int later{};
  take(appended.owner.scene->onTextEdited(appended.field.node(), [&] {
    take(appended.owner.scene->onTextEdited(appended.field.node(), [&] {
      ++later;
    }));
  }));
  appended.type(U'x');
  check(later == 0, "append does not enter the current delivery snapshot");
  appended.type(U'y');
  check(later == 1, "append participates in the next event");
}

void pipeAndErrors() {
  example::ReplayTerminal terminal({{10, "x", {}}, {20, "\r", {}}, {30, "\t", {}}}, 40, {30, 4});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<LineEdit> field;
  int edited{}, finished{}, context{};
  auto scene = ui::scene(ui::column(ui::lineEdit(field, ""), ui::button("other",
                                                                        [] {
                                                                        })),
                         options) |
               ui::onTextEdited(field,
                                [&] {
                                  ++edited;
                                  context += !ui::detail::CurrentSceneScope::snapshot().expired();
                                }) |
               ui::onEditingFinished(field, [&](const EditingFinishedEvent &) {
                 ++finished;
               });
  take(scene.run(terminal));
  check(edited == 1 && finished == 2 && context == 1 && terminal.closed,
        "pipe callbacks compose through existing scene context and automatic cleanup");

  EditorFixture failed;
  int afterFailure{};
  take(failed.owner.scene->onTextEdited(failed.field.node(), []() -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }));
  take(failed.owner.scene->onTextEdited(failed.field.node(), [&] {
    ++afterFailure;
  }));
  failed.type(U'x');
  const auto errors = take(failed.owner.scene->diagnostics());
  check(afterFailure == 0 && !errors.errors.empty() &&
          treeCause(errors.errors.back().error, TreeError::INVALID_GEOMETRY),
        "checked handler error keeps its typed cause and stops the chain");
}

void limits() {
  ManagedOptions options;
  options.maxEditingHandlers = 2;
  options.maxEditingHandlersPerNode = 2;
  Fixture owner(options);
  auto model = take(owner.scene->state(std::string{}));
  const auto field = take(owner.scene->emplace<LineEdit>(owner.scene->root()));
  take(owner.scene->bind(field, model));
  take(owner.scene->onTextEdited(field, [] {
  }));
  take(owner.scene->onEditingFinished(field, [] {
  }));
  const auto full = owner.scene->onEditingFinished(field, [] {
  });
  check(!full && treeCause(full.error(), TreeError::NODE_LIMIT), "both event kinds share finite callback budgets");
  take(owner.scene->remove(field));
  owner.prepare();
  const auto replacement = take(owner.scene->emplace<LineEdit>(owner.scene->root()));
  take(owner.scene->bind(replacement, model));
  take(owner.scene->onTextEdited(replacement, [] {
  }));
  options.maxEditingHandlers = 0;
  const auto invalid = ManagedScene::create(*owner.app, options);
  check(!invalid && treeCause(invalid.error(), TreeError::INVALID_ARGUMENT),
        "zero editor handler budget rejects mount");
}

void notificationLossAndRegistrationAllocation() {
  EditorFixture loss(EditMode::EXPLICIT, 1);
  take(loss.owner.scene->observe(loss.other.node(), loss.model, [](const std::string &) {
  }));
  loss.type(U'x');
  loss.enter();
  const auto &original = *loss.legacy.back().commit;
  const auto &retained = *loss.finished.back().result->commit;
  check(original.changed && original.rejected != 0 && original.notification_error &&
          loss.finished.back().outcome == EditingFinishedOutcome::APPLIED && retained.snapshot == original.snapshot &&
          retained.accepted == original.accepted && retained.rejected == original.rejected &&
          retained.cancelled == original.cancelled && retained.notification_error == original.notification_error,
        "completion retains exact partial-notification receipt without republishing the commit");

  for (const bool completion : {false, true}) {
    ManagedOptions options;
    options.maxEditingHandlers = 1;
    options.maxEditingHandlersPerNode = 1;
    Fixture owner(options);
    const auto model = take(owner.scene->state(std::string{}));
    const auto node = take(owner.scene->emplace<LineEdit>(owner.scene->root()));
    take(owner.scene->bind(node, model));
    int delivered{};
    allocationTest::remaining = 0;
    const auto rejected = completion ? owner.scene->onEditingFinished(node,
                                                                      [&] {
                                                                        ++delivered;
                                                                      })
                                     : owner.scene->onTextEdited(node, [&] {
                                         ++delivered;
                                       });
    allocationTest::remaining = -1;
    check(!rejected && treeCause(rejected.error(), TreeError::NO_MEMORY),
          "registration allocation rejection preserves its checked NO_MEMORY result");
    take(owner.scene->onTextEdited(node, [&] {
      ++delivered;
    }));
    take(owner.app->router().setFocus(node));
    take(owner.app->router().dispatch(TextInputEvent{U'x'}));
    check(delivered == 1, "failed registration retains no callback or budget and permits a fresh registration");
  }
}
} // namespace

int main() {
  try {
    modeMatrix();
    deltasAndRejections();
    cancellationDeltas();
    conflictAndTechnicalFailure();
    callbackLifetimeAndFocus();
    pipeAndErrors();
    limits();
    notificationLossAndRegistrationAllocation();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }

  return 0;
}
