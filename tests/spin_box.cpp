// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <portals/spin_box.hpp>
#include <portals/ui.hpp>
#include <vector>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
thread_local bool counting{};
thread_local std::size_t calls{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  if (allocation_test::counting) {
    ++allocation_test::calls;
  }
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  if (void *value = std::malloc(size ? size : 1)) {
    return value;
  }

  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
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

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return ::operator new(size, std::nothrow);
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;
int failures{};

void check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::cerr << message << '\n';
  }
}

template <class T, class E>
T take(std::expected<T, E> value, std::source_location location = std::source_location::current()) {
  if (!value) {
    std::cerr << "fixture failed at line " << location.line();
    if constexpr (std::same_as<E, ManagedError>) {
      auto tree = std::get_if<TreeError>(&value.error().cause);
      std::cerr << " tree=" << (tree ? static_cast<int>(*tree) : -1)
                << " numeric=" << (value.error().numeric ? static_cast<int>(*value.error().numeric) : -1);
    } else if constexpr (std::same_as<E, TreeError>) {
      std::cerr << " tree=" << static_cast<int>(value.error());
    }
    std::cerr << '\n';
    std::exit(2);
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  Node<Row> row = take(tree->emplace<Row>(root));
  Node<LineEdit> editor;
  Node<Button> down = take(tree->emplace<Button>(row, take(GraphemeText::from("-")), ControlStyle{},
                                                 DEFAULT_CONTENT_POLICY, ButtonChrome::NONE));
  Node<Button> up = take(tree->emplace<Button>(row, take(GraphemeText::from("+")), ControlStyle{},
                                               DEFAULT_CONTENT_POLICY, ButtonChrome::NONE));
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  std::shared_ptr<Property<NumericValue>> model =
    take(Property<NumericValue>::create(*tree, row, take(NumericValue::create(1)), Invalidation::PAINT));
  std::vector<SpinBoxResult> outcomes;
  std::function<void(const SpinBoxResult &)> hook;
  std::optional<SpinBoxBinding> binding;

  explicit Fixture(EditMode mode = EditMode::IMMEDIATE, bool readOnly = false) {
    editor = take(tree->emplace<LineEdit>(row, LineEditOptions{.editMode = mode, .readOnly = readOnly}));
    binding.emplace(take(bindSpinBox(*tree, *router, editor, down, up, model, 10, [&](SpinBoxResult receipt) {
      outcomes.push_back(receipt);
      if (hook) {
        hook(receipt);
      }
    })));
    take(router->setFocus(editor));
  }

  std::string text() {
    std::string result;
    take(tree->read(editor, [&](const LineEdit &edit) {
      result = edit.text().bytes();
    }));
    return result;
  }

  double value() {
    return take(model->snapshot())->value.value();
  }

  void draft(std::string_view text) {
    take(router->dispatch(KeyEvent{InputKey::CHARACTER, U'a', {.control = true}}));
    take(router->dispatch(PasteInputEvent{std::string(text)}));
  }

  void enter() {
    take(router->invoke(editor, ActionEvent{Action::ACTIVATE}));
  }

  void escape() {
    take(router->invoke(editor, ActionEvent{Action::CANCEL}));
  }

  void key(InputKey key) {
    take(router->dispatch(KeyEvent{key}));
  }
};

void editingModes() {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture f(mode);
    f.draft("01");
    check(f.text() == "01", "accepted numeric spelling stays visible");
    f.enter();
    check(f.text() == "01" && f.value() == 1 && f.outcomes.back().commit && !f.outcomes.back().commit->changed,
          "equal publication has its own exact no-op receipt without canonicalizing");
    f.draft("1e0");
    f.enter();
    check(f.text() == "1e0" && f.value() == 1, "exponent spelling survives confirmation");
    for (const auto invalid : {"", "-", "+", "1e", "1e-", "NaN", "inf", "101", "+-1", "1junk"}) {
      f.draft(invalid);
      f.enter();
      check(f.text() == invalid && f.value() == 1, "invalid or incomplete numeric draft stays visible and unpublished");
      f.key(InputKey::UP);
      check(f.text() == invalid && f.value() == 1, "invalid draft is not silently discarded by step");
    }
    f.escape();
    check(f.text() == "1", "Escape adopts latest canonical projection");
    f.draft("2.5");
    check(f.value() == (mode == EditMode::IMMEDIATE ? 2.5 : 1), "draft publication follows existing EditMode");
    f.enter();
    check(f.value() == 2.5, "Enter confirms numeric draft in every mode");
  }
}

void stepsAndHistory() {
  Fixture f(EditMode::EXPLICIT);
  f.key(InputKey::UP);
  check(f.text() == "2" && f.value() == 1, "user step follows EXPLICIT mode");
  f.key(InputKey::PAGE_UP);
  check(f.text() == "12" && f.value() == 1, "page step starts from valid dirty draft");
  take(f.router->dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}));
  check(f.text() == "2", "each step is a separate ordinary history operation");
  f.enter();
  take(f.router->dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}));
  check(f.text() == "1" && f.value() == 2, "own commit preserves history and Undo remains a draft");
  f.enter();
  take(f.model->set(take(NumericValue::create(7.25))));
  take(f.binding->sync(f.editor));
  f.key(InputKey::UP);
  f.enter();
  check(f.value() == 8.25, "clean step preserves authoritative off-grid offset");
  Fixture locked(EditMode::IMMEDIATE, true);
  locked.key(InputKey::UP);
  locked.draft("9");
  take(locked.router->invoke(locked.up, ActionEvent{Action::ACTIVATE}));
  check(locked.text() == "1" && locked.value() == 1, "readOnly gates routed typing and both step entry paths");
  auto programmed = take(locked.binding->setValue(3));
  check(programmed.commit && locked.value() == 3, "readOnly allows programmatic numeric assignment");
}

void externalConflictAndBlur() {
  Fixture f(EditMode::EXPLICIT);
  f.draft("2");
  take(f.model->set(take(NumericValue::create(3))));
  take(f.binding->sync(f.editor));
  f.enter();
  check(f.text() == "2" && f.value() == 3 && f.outcomes.back().editing.status == LineEditStatus::CONFLICT,
        "dirty draft keeps its original revision and reports external conflict");
  f.escape();
  check(f.text() == "3", "Escape after conflict adopts current numeric base");
  f.draft("4");
  take(f.model->set(take(NumericValue::create(3, {0, 100, 0.5}))));
  f.enter();
  check(f.outcomes.back().editing.status == LineEditStatus::CONFLICT,
        "range change is an external revision even at equal numeric value");
  for (auto mode : {EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture blur(mode);
    blur.draft("5");
    auto other = take(blur.tree->emplace<Button>(blur.root, take(GraphemeText::from("Other"))));
    auto otherBinding = take(bindButton(*blur.router, other, [] {
    }));
    take(blur.router->setFocus(other));
    check(blur.value() == (mode == EditMode::ON_COMPLETION ? 5 : 1), "blur retains existing completion policy");
  }
}

void reentryAndRemoval() {
  Fixture f;
  std::optional<SpinBoxResult> original;
  f.hook = [&](const SpinBoxResult &receipt) {
    if (!receipt.commit || receipt.commit->snapshot->value.value() != 2) {
      return;
    }
    original = receipt;
    take(f.model->set(take(NumericValue::create(9))));
    auto synced = f.binding->sync(f.editor);
    check(!synced && synced.error() == TreeError::WOULD_BLOCK, "reentrant sync retains the existing busy gate");
  };
  auto result = take(f.binding->setValue(2));
  check(result.commit && result.commit->snapshot->value.value() == 2 && f.value() == 9 &&
          original->commit->snapshot == result.commit->snapshot,
        "reentrant later model revision never replaces the returned exact receipt");
  take(f.binding->sync(f.editor));
  check(f.text() == "9", "outer commit never rolls projection cache back after reentrant sync");
  Fixture removed;
  removed.hook = [&](const SpinBoxResult &) {
    take(removed.tree->remove(removed.row));
    removed.binding.reset();
  };
  auto receipt = take(removed.binding->setValue(8));
  check(receipt.commit && receipt.commit->snapshot->value.value() == 8,
        "exact receipt survives node and binding removal during delivery");
  Fixture throwing;
  throwing.hook = [](const SpinBoxResult &) {
    throw std::bad_alloc();
  };
  auto thrown = take(throwing.binding->setValue(6));
  check(thrown.commit && thrown.commit->snapshot->value.value() == 6 && thrown.editing.error == TreeError::NO_MEMORY,
        "postcommit callback allocation failure preserves exact committed receipt");
  Fixture draftThrowing(EditMode::EXPLICIT);
  draftThrowing.hook = [](const SpinBoxResult &) {
    throw std::bad_alloc();
  };
  auto draftThrown = take(draftThrowing.binding->setValue(6));
  check(!draftThrown.commit && draftThrown.editing.status == LineEditStatus::EDITED &&
          draftThrown.editing.error == TreeError::NO_MEMORY && draftThrowing.text() == "6" &&
          draftThrowing.value() == 1,
        "callback failure after accepting a deferred draft preserves its editing observation");
}

void legacyBindAllocationObservation() {
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto editor = take(tree->emplace<LineEdit>(root));
  auto router = take(EventRouter::create(*tree));
  std::shared_ptr<Property<std::string>> model =
    take(Property<std::string>::create(*tree, editor, std::string("1"), Invalidation::PAINT));
  allocation_test::calls = 0;
  allocation_test::counting = true;
  auto binding = bindLineEdit(*tree, *router, editor, model, [](LineEditResult) {
  });
  allocation_test::counting = false;
  take(std::move(binding));
  std::cout << "legacy_bind_allocation_calls=" << allocation_test::calls
            << " public_binding_bytes=" << sizeof(LineEditBinding) << '\n';
}

void precommitAllocationBoundaries() {
  bool reachedCommit{};
  int rejected{};
  for (std::ptrdiff_t budget = 0; budget < 128 && !reachedCommit; ++budget) {
    Fixture f;
    f.outcomes.reserve(8);
    allocation_test::remaining = budget;
    auto receipt = f.binding->setValue(12.345678901234567);
    allocation_test::remaining = -1;
    const auto committed = f.value() != 1;
    check(!committed || (receipt && receipt->commit && receipt->commit->snapshot->value.value() == f.value()),
          "allocation failure never replaces an already committed numeric receipt");
    if (!committed) {
      ++rejected;
    }
    reachedCommit = committed;
  }
  check(reachedCommit && rejected > 0,
        "bounded allocation sweep reaches precommit failures and a successful projection");
  Fixture cached;
  take(cached.binding->sync(cached.editor));
  allocation_test::remaining = 0;
  auto same = cached.binding->sync(cached.editor);
  allocation_test::remaining = -1;
  check(same.has_value(), "unchanged synchronized numeric projection needs no formatting allocation");
}

void resourcesAndDraftGet() {
  for (bool enabled : {false, true}) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<SpinBox> reference;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<SpinBox>("Amount", [=](ui::SceneResourceArguments) {
      return ui::spinBox(reference, 5).range(0, 20).step(0.5).readOnly(true).editMode(EditMode::EXPLICIT);
    }));
    const auto text = enabled ? "scene{Amount{readOnly=false}}" : "scene{Amount{}}";
    take(ui::mount(*owner, ui::resource(text, options)));
    auto editor = reference.lineEdit();
    take(app->router().setFocus(editor.node()));
    take(app->router().dispatch(KeyEvent{InputKey::UP}));
    check(take(reference.value().get()) == (enabled ? 5.5 : 5),
          "sparse false resource override preserves factory range/step/mode");
    auto edit = reference.text() = "1e";
    take(edit.result());
    auto incomplete = reference.value().get();
    check(!incomplete && incomplete.error().numeric == NumericError::INVALID_TEXT,
          "numeric get reports incomplete visible draft without falling back to committed model");
    auto invalid = reference.text() = "21";
    take(invalid.result());
    auto outOfRange = reference.value().get();
    check(!outOfRange && outOfRange.error().numeric == NumericError::OUT_OF_RANGE,
          "numeric get retains exact out-of-range draft cause");
  }
  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto model = take(owner->numericState(5));
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<SpinBox>("Bound", [=](ui::SceneResourceArguments) {
      return ui::spinBox(model);
    }));
    auto mounted = ui::mount(*owner, ui::resource("scene{Bound{value=5}}", options));
    check(!mounted && mounted.error().numeric == NumericError::INITIAL_CONFLICT && mounted.error().sceneResource &&
            mounted.error().sceneResource->propertyPath &&
            mounted.error().sceneResource->propertyPath->ends_with(".value"),
          "equal resource override of external numeric source preserves typed conflict and property origin");
  }
  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto mounted = ui::mount(*owner, ui::resource("scene{SpinBox{step=0}}", options));
    check(!mounted && mounted.error().numeric == NumericError::INVALID_STEP && mounted.error().sceneResource,
          "built-in SpinBox resource keeps typed invalid increment diagnostic");
  }
}

void focusLayoutAndTypedEvents() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<SpinBox> reference;
  take(ui::mount(*owner, ui::spinBox(reference, 1)));
  auto editor = reference.lineEdit();
  auto viewport = take(CellRect::from({}, {40, 5}));
  take(app->tree().frame([&] {
    take(layoutTree(app->tree(), app->root(), viewport, viewport));
  }));
  auto geometry = take(app->tree().inspect(reference.node())).geometry;
  check(geometry.local.size() == CellSize{10, 1}, "SpinBox intrinsic row is editor eight plus two one-cell buttons");
  take(app->router().setFocus(editor.node()));
  take(app->router().dispatch(KeyEvent{InputKey::TAB}));
  check(take(app->router().focused()) == Node<Widget>(editor.node()), "arrows add no Tab stops");
  int edited{};
  int finished{};
  int changed{};
  take(owner->onTextEdited(editor.node(), [&](const TextEditedEvent &) {
    ++edited;
  }));
  take(owner->onEditingFinished(editor.node(), [&](const EditingFinishedEvent &) {
    ++finished;
  }));
  take(owner->onChange(reference.node(), [&](double) {
    ++changed;
  }));
  take(app->router().dispatch(KeyEvent{InputKey::UP}));
  take(app->router().invoke(editor.node(), ActionEvent{Action::ACTIVATE}));
  check(edited == 1 && finished == 1 && changed == 1,
        "numeric source retains ordinary typed text/completion distinctions");
  const auto source = editor.text().source();
  auto snapshot = take(source.snapshot());
  check(snapshot.bytes() == "2", "visible text source captures numeric editor without text Property");
}

void proxyReporterRemoval() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<SpinBox> reference;
  take(ui::mount(*owner, ui::spinBox(reference, 1)));
  auto proxy = std::make_unique<ui::SpinBoxNumericProperty>(reference.value());
  int reports{};
  take(owner->onError([&](const ManagedDiagnostic &) {
    ++reports;
    proxy.reset();
  }));
  auto observation = proxy->set(std::numeric_limits<double>::quiet_NaN());
  auto receipt = observation.result();
  check(!proxy && reports == 1 && !receipt && receipt.error().numeric == NumericError::NOT_FINITE,
        "numeric error observation survives reporter deletion of the original proxy");
}

void reviewCleanProgrammaticRevision() {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    for (const double candidate : {3.0, 1.0}) {
      auto app = take(Application::create());
      auto owner = take(ManagedScene::create(*app));
      auto model = take(owner->numericState(1));
      ui::Ref<SpinBox> reference;
      take(ui::mount(*owner, ui::spinBox(reference, model).editMode(mode)));
      take(model.set(2)); // Deliberately no data dispatch or prepare between external write and setter.
      auto assigned = reference.value().set(candidate);
      auto receipt = take(assigned.result());
      if (mode == EditMode::IMMEDIATE) {
        check(receipt.commit && receipt.editing.status == LineEditStatus::COMMITTED &&
                receipt.commit->snapshot->value.value() == candidate &&
                take(model.snapshot())->value.value() == candidate,
              "SPN1 clean immediate numeric setter adopts latest revision, including old-visible candidate");
      } else {
        check(!receipt.commit && receipt.editing.status == LineEditStatus::EDITED &&
                take(model.snapshot())->value.value() == 2 && take(reference.value().get()) == candidate,
              "SPN1 deferred setter adopts latest clean base before accepting its draft");
        take(app->router().invoke(reference.lineEdit().node(), ActionEvent{Action::ACTIVATE}));
        check(take(model.snapshot())->value.value() == candidate,
              "SPN1 deferred numeric setter confirms against the latest clean base without conflict");
      }
    }
  }
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto model = take(owner->numericState(1));
  ui::Ref<SpinBox> reference;
  take(ui::mount(*owner, ui::spinBox(reference, model)));
  take((reference.text() = "1e").result());
  take(model.set(2));
  auto receipt = take(reference.value().set(3).result());
  check(!receipt.commit && receipt.editing.status == LineEditStatus::CONFLICT &&
          take(model.snapshot())->value.value() == 2 && take(reference.text().get()) == "3",
        "SPN1 programmatic numeric edit preserves a genuinely dirty revision conflict");
}

void reviewActionRelease() {
  Fixture immediate;
  const auto initial = take(immediate.model->snapshot());
  const auto outcomes = immediate.outcomes.size();
  const ActionEvent releasedUp{Action::MOVE_UP, {}, ClickSource::KEYBOARD, KeyAction::RELEASE};
  const ActionEvent releasedActivate{Action::ACTIVATE, {}, ClickSource::KEYBOARD, KeyAction::RELEASE};
  take(immediate.router->invoke(immediate.editor, releasedUp));
  take(immediate.router->invoke(immediate.up, releasedActivate));
  take(immediate.router->invoke(immediate.down, releasedActivate));
  check(take(immediate.model->snapshot()) == initial && immediate.text() == "1" &&
          immediate.outcomes.size() == outcomes,
        "SPN2 released semantic step/button actions leave model, draft and callbacks unchanged");

  Fixture explicitEdit(EditMode::EXPLICIT);
  explicitEdit.key(InputKey::UP);
  const auto pending = take(explicitEdit.model->snapshot());
  const auto observed = explicitEdit.outcomes.size();
  take(explicitEdit.router->invoke(explicitEdit.editor, releasedUp));
  take(explicitEdit.router->invoke(explicitEdit.editor, releasedActivate));
  check(explicitEdit.text() == "2" && take(explicitEdit.model->snapshot()) == pending &&
          explicitEdit.outcomes.size() == observed,
        "SPN2 release neither appends a history step nor confirms an explicit draft");
  take(explicitEdit.router->dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}));
  check(explicitEdit.text() == "1", "SPN2 release leaves Undo at the single admitted press");
  take(explicitEdit.router->invoke(explicitEdit.editor,
                                   ActionEvent{Action::MOVE_UP, {}, ClickSource::KEYBOARD, KeyAction::REPEAT}));
  check(explicitEdit.text() == "2", "SPN2 semantic repeats remain admitted");
}

void reviewNumericParsePrecedence() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto model = take(owner->numericState(1));
  ui::Ref<SpinBox> reference;
  take(ui::mount(*owner, ui::spinBox(reference, model).editMode(EditMode::EXPLICIT)));
  const auto committed = take(model.snapshot());
  for (const auto text : {"nan!", "1e999x", "nan", "1e999"}) {
    take((reference.text() = text).result());
    auto parsed = reference.value().get();
    const auto expected = std::string_view(text).ends_with('!') || std::string_view(text).ends_with('x')
                            ? NumericError::INVALID_TEXT
                            : NumericError::NOT_FINITE;
    check(!parsed && parsed.error().numeric == expected && take(reference.text().get()) == text &&
            take(model.snapshot()) == committed,
          "SPN3 full-consumption syntax failure precedes nonfinite/range-conversion classification");
  }
}

void reviewStructuralRemoval() {
  for (std::size_t part = 0; part < 3; ++part) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<SpinBox> reference;
    take(ui::mount(*owner, ui::spinBox(reference, 1)));
    const auto parts = take(app->tree().inspect(reference.node())).children;
    const auto before = take(reference.value().get());
    const auto removed = owner->remove(parts[part]);
    check(!removed && std::get_if<TreeError>(&removed.error().cause) &&
            std::get<TreeError>(removed.error().cause) == TreeError::INVALID_ARGUMENT,
          "SPN4 direct managed removal rejects each structural SpinBox child before mutation");
    if (!removed) {
      check(app->tree().inspect(parts[part]).has_value() && take(reference.value().get()) == before &&
              owner->prepare(*app, {40, 5}).has_value(),
            "SPN4 rejected child removal keeps the numeric editor and preparation live");
    }
    take(owner->remove(reference.node()));
    check(owner->prepare(*app, {40, 5}).has_value(), "SPN4 whole SpinBox removal remains supported");
  }
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto model = take(owner->numericState(1));
  ui::Ref<SpinBox> first;
  ui::Ref<SpinBox> sibling;
  take(ui::mount(*owner, ui::vbox(ui::spinBox(first, model), ui::spinBox(sibling, 10))));
  bool rejected{};
  std::optional<PropertyUpdate<NumericValue>> receipt;
  take(owner->onChange(first.node(), [&](const PropertyUpdate<NumericValue> &original) {
    receipt = original;
    const auto removed = owner->remove(first.lineEdit().node());
    rejected = !removed && std::get_if<TreeError>(&removed.error().cause) &&
               std::get<TreeError>(removed.error().cause) == TreeError::INVALID_ARGUMENT;
  }));
  take(app->router().invoke(first.lineEdit().node(), ActionEvent{Action::MOVE_UP}));
  const auto prepared = owner->prepare(*app, {40, 8});
  check(rejected && receipt && receipt->snapshot == take(model.snapshot()) && receipt->snapshot->value.value() == 2 &&
          prepared,
        "SPN4 child-removal attempt inside onChange preserves exact admitted receipt and scene liveness");
  if (prepared) {
    take(app->router().invoke(sibling.lineEdit().node(), ActionEvent{Action::MOVE_UP}));
    check(take(sibling.value().get()) == 11, "SPN4 sibling numeric field remains interactive after rejected removal");
  }
}

void managedFacade() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto model = take(owner->numericState(1));
  ui::Ref<SpinBox> reference;
  take(ui::mount(*owner, ui::spinBox(reference, model).editMode(EditMode::EXPLICIT)));
  auto editor = reference.lineEdit();
  check(editor.node() != Node<LineEdit>{}, "SpinBox exposes its single real editor");
  const auto assigned = reference.value() = 3;
  auto receipt = take(assigned.result());
  check(!receipt.commit && receipt.editing.status == LineEditStatus::EDITED && take(reference.value().get()) == 3 &&
          take(model.snapshot())->value.value() == 1 && take(editor.text().get()) == "3",
        "numeric proxy distinguishes draft acceptance from numeric commit");
  take(app->router().invoke(editor.node(), ActionEvent{Action::ACTIVATE}));
  check(take(reference.value().get()) == 3, "existing editor Enter publishes numeric proxy draft");
  int changes{};
  take(owner->onChange(reference.node(), [&](double) {
    ++changes;
  }));
  auto text = editor.text() = "04";
  take(text.result());
  take(app->router().invoke(editor.node(), ActionEvent{Action::ACTIVATE}));
  check(changes == 1 && take(reference.value().get()) == 4 && take(editor.text().get()) == "04",
        "text proxy and numeric onChange share one editor/source and preserve spelling");
  auto proxy = reference.value();
  take(owner->remove(reference.node()));
  check(!proxy.get(), "removed control proxy stays stale");
}
} // namespace

int main() {
  reviewCleanProgrammaticRevision();
  reviewActionRelease();
  reviewNumericParsePrecedence();
  reviewStructuralRemoval();
  editingModes();
  stepsAndHistory();
  externalConflictAndBlur();
  reentryAndRemoval();
  legacyBindAllocationObservation();
  precommitAllocationBoundaries();
  resourcesAndDraftGet();
  focusLayoutAndTypedEvents();
  proxyReporterRemoval();
  managedFacade();
  return failures ? 1 : 0;
}
