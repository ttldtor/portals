// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/numeric_snap_reference.hpp"
#include <portals/slider.hpp>
#include <portals/ui.hpp>

#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
using namespace portals;
int failures{};

void numericCallback(double) {
}

void numericFinishedCallback(const SliderOperationEvent &) {
}

static_assert(requires(ManagedScene &owner, Node<Slider> node) {
  owner.onChange(node, numericCallback);
  owner.onEditingFinished(node, numericFinishedCallback);
  owner.onChange<decltype(&numericCallback)>(node, numericCallback);
  owner.onEditingFinished<decltype(&numericFinishedCallback)>(node, numericFinishedCallback);
});

template <class T>
concept CaptionReference = requires(const ui::Ref<T> &reference) { reference.text(); };

static_assert(!CaptionReference<Slider>);
static_assert(CaptionReference<Label>);

static_assert(requires(ui::Ref<Slider> reference) {
  ui::slider({});
  ui::slider(reference, {});
  ui::slider(reference, 0, {});
});

void check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::cerr << message << '\n';
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location location = std::source_location::current()) {
  if (!result) {
    std::cerr << "fixture operation failed at line " << location.line();

    if constexpr (std::same_as<E, ManagedError>) {
      const auto *tree = std::get_if<TreeError>(&result.error().cause);

      std::cerr << " phase=" << static_cast<int>(result.error().phase)
                << " tree=" << (tree ? static_cast<int>(*tree) : -1)
                << " numeric=" << (result.error().numeric ? static_cast<int>(*result.error().numeric) : -1);

      if (result.error().sceneResource) {
        const auto &resource = *result.error().sceneResource;

        std::cerr << " resource=" << static_cast<int>(resource.code) << " offset=" << resource.offset
                  << " path=" << (resource.propertyPath ? *resource.propertyPath : "");
      }
    }

    std::cerr << '\n';
    std::exit(2);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool numericError(const std::expected<NumericValue, NumericError> &result, NumericError expected) {
  return !result && result.error() == expected;
}

void numericValidation() {
  const NumericValue initial;

  check(initial.value() == 0 && initial.range() == NumericRange{}, "numeric defaults are zero, 0..100, step one");

  for (const auto bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()}) {
    check(numericError(NumericValue::create(bad), NumericError::NOT_FINITE), "nonfinite value stays typed");
    check(numericError(NumericValue::create(0, {.minimum = bad}), NumericError::NOT_FINITE),
          "nonfinite minimum stays typed");
    check(numericError(NumericValue::create(0, {.maximum = bad}), NumericError::NOT_FINITE),
          "nonfinite maximum stays typed");
    check(numericError(NumericValue::create(0, {.step = bad}), NumericError::NOT_FINITE), "nonfinite step stays typed");
  }

  check(numericError(NumericValue::create(0, {.minimum = 1, .maximum = 1}), NumericError::INVALID_RANGE),
        "equal bounds rejected");
  check(numericError(NumericValue::create(0, {.minimum = 1, .maximum = -1}), NumericError::INVALID_RANGE),
        "reversed bounds rejected");
  const auto largest = std::numeric_limits<double>::max();

  check(numericError(NumericValue::create(0, {.minimum = -largest, .maximum = largest}), NumericError::INVALID_RANGE),
        "overflowing positive span rejected");
  check(numericError(NumericValue::create(0, {.step = 0}), NumericError::INVALID_STEP), "zero step rejected");
  check(numericError(NumericValue::create(0, {.step = -1}), NumericError::INVALID_STEP), "negative step rejected");
  check(numericError(NumericValue::create(1e20, {.minimum = 1e20, .maximum = 1e20 + 1e6, .step = 1}),
                     NumericError::UNREPRESENTABLE_STEP),
        "increment unable to advance endpoint rejected");
  check(numericError(NumericValue::create(101), NumericError::OUT_OF_RANGE), "explicit out-of-range value rejected");
  check(take(initial.withValue(0.25)).value() == 0.25 && initial.value() == 0,
        "off-grid programmatic value accepted without changing original");
  check(take(NumericValue::create(0, {.step = largest})).range().step == largest,
        "step above span accepted without overflowing validation");
}

void numericInput() {
  const NumericRange range{0, 10, 3};

  check(detail::snapNumeric(range, -1) == 0 && detail::snapNumeric(range, 11) == 10,
        "pointer saturates exact endpoints");
  check(detail::snapNumeric(range, 4.5) == 6 && detail::snapNumeric(range, 9.5) == 10,
        "snapping ties increase and shortened endpoint remains reachable");
  check(detail::snapNumeric({-10, 10, 3}, -8.5) == -7, "negative grid anchored at minimum");
  const auto wide = std::ldexp(1.0, 58);

  check(detail::snapNumeric({-1, wide, wide}, wide / 4) == -1,
        "endpoint-only snapping retains the original minimum at a large exponent");
  check(detail::snapNumeric({-1, wide, wide / 2}, wide / 8) == -1,
        "sub-span snapping reconstructs the lower tick from its minimum anchor");
  check(detail::snapNumeric({-wide, 1, 96076792050570576.0}, -std::ldexp(1.0, 54)) == -16,
        "snapping retains the FMA-representable third tick after anchor cancellation");
  const auto oddStep = std::nextafter(16.0, std::numeric_limits<double>::infinity());
  const auto oddTick = -16.0 - 5 * std::ldexp(1.0, -48);

  check(take(NumericValue::create(-wide, {-wide, 1, oddStep})).range().step == oddStep &&
          detail::snapNumeric({-wide, 1, oddStep}, -16) == oddTick,
        "snapping retains an odd grid index beyond the exact-integer range of binary64");
  const auto denseStep = std::nextafter(std::ldexp(1.0, -54), std::numeric_limits<double>::infinity());

  check(take(NumericValue::create(0, {0, 1, denseStep})).range().step == denseStep &&
          detail::snapNumeric({0, 1, denseStep}, 0.75) == 0.75,
        "validated dense binary64 grids remain supported without a quotient precision cap");
  const auto offGrid = take(NumericValue::create(0.5, range));

  check(detail::nudgeNumeric(offGrid, 1) == 3.5 && detail::nudgeNumeric(offGrid, -1) == 0,
        "keyboard increments current off-grid value and saturates");
  check(detail::nudgeNumeric(offGrid, 1, 10) == 10, "page count saturates before multiplication");
  check(detail::nudgeNumeric(take(NumericValue::create(0, {.step = std::numeric_limits<double>::max()})), 1,
                             std::numeric_limits<unsigned>::max()) == 100,
        "huge step count does not overflow into infinity");
  const auto fine = take(NumericValue::create(0, {0, 1, std::numeric_limits<double>::epsilon()}));
  const auto snapped = detail::snapNumeric(fine.range(), 0.375);

  check(std::isfinite(snapped) && snapped == 0.375, "large tick count uses no integer quotient conversion");
}

void numericOracle() {
  std::size_t index{};

  for (const auto &reference : test::NUMERIC_SNAP_REFERENCE_CASES) {
    const NumericRange range{std::bit_cast<double>(reference.minimumBits), std::bit_cast<double>(reference.maximumBits),
                             std::bit_cast<double>(reference.stepBits)};
    const auto candidate = std::bit_cast<double>(reference.candidateBits);
    const auto valid = NumericValue::create(candidate, range);
    const auto actual = std::bit_cast<std::uint64_t>(detail::snapNumeric(range, candidate));

    if (!valid || actual != reference.expectedBits) {
      std::cerr << "numeric oracle case " << index << " actual bits " << actual << " expected bits "
                << reference.expectedBits << '\n';
    }

    check(valid && actual == reference.expectedBits, "snapping matches the independent exact rational oracle bits");
    ++index;
  }
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  Node<Slider> node = take(tree->emplace<Slider>(root));
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  std::shared_ptr<Property<NumericValue>> model =
    take(Property<NumericValue>::create(*tree, node, take(NumericValue::create(25)), Invalidation::PAINT));
  std::vector<PropertyUpdate<NumericValue>> changes;
  std::vector<SliderOperationEvent> completions;
  std::function<void()> changeHook;
  std::function<void()> finishedHook;
  std::optional<SliderBinding> binding;

  Fixture(bool capture = true, SchedulerOptions options = {}, bool buttonIdentity = true)
      : scheduler(take(Scheduler::create(options))) {
    take(router->setPointerCaptureCapabilities({capture, capture, capture && buttonIdentity}));
    binding.emplace(take(bindSlider(
      *tree, *router, node, model,
      [&](SliderResult result) {
        changes.push_back(take(std::move(result)));

        if (changeHook) {
          changeHook();
        }
      },
      [&](const SliderOperationEvent &event) {
        completions.push_back(event);

        if (finishedHook) {
          finishedHook();
        }
      })));
    take(router->setFocus(node));
    const auto viewport = take(CellRect::from({}, {11, 1}));

    take(tree->frame([&] {
      take(layoutTree(*tree, root, viewport, viewport));
    }));
  }

  void mouse(int column, MouseAction action) {
    take(router->dispatch(MouseInputEvent{column, 0, MouseButton::LEFT, action}));
  }

  double value() {
    return take(model->snapshot())->value.value();
  }
};

void nativeKeyboard() {
  Fixture f;

  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_RIGHT}));
  check(f.value() == 26 && f.changes.size() == 1 && f.completions.size() == 1,
        "one key action commits and completes once");
  take(f.model->set(take(NumericValue::create(26.5))));
  take(f.binding->sync());
  check(f.changes.size() == 1, "external writes never invoke user onChange");
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_RIGHT}));
  check(f.value() == 27.5, "native key nudge preserves external off-grid value");
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_END}));
  const auto changed = f.changes.size();
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_RIGHT}));
  check(f.changes.size() == changed && !f.completions.back().lastUserUpdate,
        "endpoint no-op completes without a changed receipt");
  take(setSliderReadOnly(*f.tree, f.node, true));
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_LEFT}));
  check(f.value() == 100 && f.changes.size() == changed, "readOnly keyboard does not mutate source");
}

void reentrantSyncCompletion() {
  Fixture step;

  step.changeHook = [&] {
    take(step.binding->sync());
  };
  take(step.router->invoke(step.node, ActionEvent{Action::MOVE_RIGHT}));
  take(step.scheduler->dispatch());
  check(step.completions.size() == 1 && step.completions.back().status == SliderOperationStatus::COMPLETED &&
          step.completions.back().lastUserUpdate->snapshot == step.changes.back().snapshot,
        "reentrant sync during a keyboard step retains one completed operation");
  Fixture click(false);

  click.changeHook = [&] {
    take(click.binding->sync());
  };
  click.mouse(8, MouseAction::PRESS);
  take(click.scheduler->dispatch());
  check(click.completions.size() == 1 && click.completions.back().status == SliderOperationStatus::COMPLETED,
        "reentrant sync during an atomic click never requires capture");
  Fixture release;

  release.mouse(0, MouseAction::PRESS);
  release.changeHook = [&] {
    take(release.binding->sync());
  };
  release.mouse(10, MouseAction::RELEASE);
  take(release.scheduler->dispatch());
  check(release.value() == 100 && release.completions.size() == 1 &&
          release.completions.back().status == SliderOperationStatus::COMPLETED &&
          release.completions.back().lastUserUpdate->snapshot == release.changes.back().snapshot,
        "reentrant sync during an admitted final release retains one completed operation");
}

void widePointerAnchor() {
  const auto wide = std::ldexp(1.0, 58);

  for (const auto divisor : {1.0, 2.0}) {
    Fixture f;

    take(f.model->set(take(NumericValue::create(-1, {-1, wide, wide / divisor}))));
    take(f.binding->sync());
    const auto viewport = take(CellRect::from({}, {divisor == 1 ? 5 : 9, 1}));

    take(f.tree->frame([&] {
      take(layoutTree(*f.tree, f.root, viewport, viewport));
    }));
    f.mouse(1, MouseAction::PRESS);
    f.mouse(1, MouseAction::RELEASE);
    check(f.value() == -1 && f.changes.empty() && f.completions.size() == 1 && !f.completions.back().lastUserUpdate,
          "large-exponent pointer click selects the actual minimum without a false changed receipt");
  }

  Fixture negative;

  take(negative.model->set(take(NumericValue::create(-wide, {-wide, 1, 96076792050570576.0}))));
  take(negative.binding->sync());
  const auto viewport = take(CellRect::from({}, {17, 1}));

  take(negative.tree->frame([&] {
    take(layoutTree(*negative.tree, negative.root, viewport, viewport));
  }));
  negative.mouse(15, MouseAction::PRESS);
  negative.mouse(15, MouseAction::RELEASE);
  check(negative.value() == -16 && negative.changes.size() == 1,
        "large negative anchor pointer route selects the actual third tick -16");
}

void stationaryThumb() {
  Fixture f;

  f.mouse(3, MouseAction::PRESS);
  f.mouse(3, MouseAction::MOVE);
  f.mouse(3, MouseAction::RELEASE);
  check(f.value() == 25 && f.changes.empty() && f.completions.size() == 1 && !f.completions.back().lastUserUpdate &&
          f.completions.back().status == SliderOperationStatus::COMPLETED,
        "stationary rounded thumb completes without snapping the source");
  take(f.model->set(take(NumericValue::create(25.25))));
  take(f.binding->sync());
  f.mouse(3, MouseAction::PRESS);
  f.mouse(3, MouseAction::RELEASE);
  check(f.value() == 25.25 && f.changes.empty() && f.completions.size() == 2 && !f.completions.back().lastUserUpdate,
        "stationary thumb preserves an external off-grid value");
  f.mouse(3, MouseAction::PRESS);
  f.mouse(4, MouseAction::MOVE);
  f.mouse(3, MouseAction::RELEASE);
  check(f.value() == 30 && f.changes.size() == 2 && f.completions.size() == 3,
        "first horizontal movement resumes absolute minimum-anchored snapping");
}

void nativeDrag() {
  Fixture f;

  f.mouse(0, MouseAction::PRESS);
  f.mouse(5, MouseAction::MOVE);
  f.mouse(10, MouseAction::RELEASE);
  std::cout << "drag value=" << f.value() << " changes=" << f.changes.size() << " completions=" << f.completions.size()
            << '\n';
  check(f.value() == 100 && f.changes.size() == 3 && f.completions.size() == 1,
        "live drag has multiple exact commits and one completion");
  check(f.completions.back().initial->value.value() == 25 &&
          f.completions.back().lastUserUpdate->snapshot == f.changes.back().snapshot,
        "completion owns starting value and exact last user receipt");

  f.mouse(2, MouseAction::PRESS);
  take(f.model->set(take(NumericValue::create(77))));
  f.mouse(10, MouseAction::RELEASE);
  std::cout << "conflict value=" << f.value() << " status=" << static_cast<int>(f.completions.back().status) << '\n';
  check(f.value() == 77 && f.completions.back().status == SliderOperationStatus::CONFLICT,
        "newer external value wins over stale release");

  f.mouse(1, MouseAction::PRESS);
  const auto applied = f.value();
  take(f.router->invoke(f.node, ActionEvent{Action::CANCEL}));
  check(f.value() == applied && f.completions.back().status == SliderOperationStatus::CANCELLED,
        "Escape retains already applied live value");

  const auto finished = f.completions.size();
  f.mouse(4, MouseAction::PRESS);
  std::cout << "hidden press value=" << f.value() << " capture=" << take(f.router->pointerCapture()).id().slot << '\n';
  take(f.tree->setVisible(f.node, false));
  take(f.binding->sync());
  check(f.completions.size() == finished, "capture-loss completion is deferred outside prepare");
  auto dispatched = take(f.scheduler->dispatch());
  std::cout << "hidden completions=" << f.completions.size() << " prior=" << finished
            << " status=" << static_cast<int>(f.completions.back().status) << '\n';
  check(f.completions.size() == finished + 1 && f.completions.back().status == SliderOperationStatus::CANCELLED,
        "hidden drag terminates on existing data lane");
}

void nativeClickOnlyAndRemoval() {
  Fixture click(false);

  click.mouse(8, MouseAction::PRESS);
  check(click.value() == 80 && click.completions.size() == 1 &&
          click.completions.back().kind == SliderOperationKind::STEP,
        "host without capture supports one atomic click");

  Fixture removed;

  removed.mouse(2, MouseAction::PRESS);
  take(removed.tree->remove(removed.node));
  take(removed.scheduler->dispatch());
  check(removed.completions.empty(), "removal suppresses late terminal callback");
}

void cancelledReleaseAndCaptureExchange() {
  Fixture stale;

  stale.mouse(1, MouseAction::PRESS);
  const auto applied = stale.value();
  take(stale.router->cancelPointerCapture());
  stale.mouse(10, MouseAction::RELEASE);
  check(stale.value() == applied && stale.completions.size() == 1 &&
          stale.completions.back().status == SliderOperationStatus::CANCELLED,
        "uncaptured stale release never uses the admitted-final-release marker");

  Fixture exchange;
  std::optional<PointerCapture> replacement;
  bool exchanged{};

  exchange.changeHook = [&] {
    if (!exchanged) {
      exchanged = true;
      take(exchange.router->cancelPointerCapture());
      replacement.emplace(take(exchange.router->capturePointer(exchange.node)));
    }
  };
  exchange.mouse(0, MouseAction::PRESS);
  take(exchange.binding->sync());
  check(replacement && replacement->connected() && take(exchange.router->pointerCapture()) == exchange.node,
        "retiring an old slider gesture never cancels reentrantly exchanged capture");
  take(exchange.scheduler->dispatch());
  check(exchange.completions.size() == 1 && exchange.completions.back().status == SliderOperationStatus::CANCELLED,
        "capture exchange completes the old operation once on the existing data lane");
}

void eligibilityAfterCompletion() {
  Fixture f;

  f.mouse(0, MouseAction::PRESS);
  f.finishedHook = [&] {
    take(setSliderReadOnly(*f.tree, f.node, true));
  };
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_RIGHT}));
  check(f.value() == 0 && f.changes.size() == 1,
        "completion callback readOnly change gates the following keyboard commit");

  Fixture empty;
  const auto viewport = take(CellRect::from({}, {0, 1}));

  take(empty.tree->frame([&] {
    take(layoutTree(*empty.tree, empty.root, viewport, viewport));
  }));
  take(empty.router->invoke(empty.node, ActionEvent{Action::MOVE_RIGHT}));
  check(empty.value() == 25 && empty.changes.empty(), "arranged empty geometry cannot mutate through keyboard input");
}

void unknownButtonContinuation() {
  Fixture f(true, {}, false);

  f.mouse(0, MouseAction::PRESS);
  take(f.router->dispatch(MouseInputEvent{8, 0, MouseButton::NO_BUTTON, MouseAction::MOVE}));
  take(f.router->dispatch(MouseInputEvent{8, 0, MouseButton::LEFT, MouseAction::RELEASE, {.shift = true}}));
  check(f.value() == 80 && f.changes.size() == 2 && f.completions.size() == 1 &&
          f.completions.back().status == SliderOperationStatus::COMPLETED,
        "captured unknown-button motion and modified final release remain one admitted drag");
}

void exactPartialDeliveryAndReentry() {
  Fixture full(true, {.data_capacity = 1});
  auto blocker = take(full.tree->connect<int>(full.node, [](Slider &, auto) {
  }));

  take(blocker.publisher.publish(std::make_shared<const int>(1)));
  full.mouse(0, MouseAction::PRESS);
  check(full.value() == 0 && full.changes.size() == 1 && full.changes.back().changed &&
          full.changes.back().rejected == 1 && full.changes.back().accepted == 0 &&
          full.changes.back().notification_error == SchedulerError::WOULD_BLOCK,
        "full data lane preserves exact committed user receipt with rejected observer delivery");
  take(full.router->cancelPointerCapture());
  const auto terminal = full.binding->sync();

  check(!terminal && terminal.error() == TreeError::WOULD_BLOCK && full.value() == 0,
        "rejected deferred completion is typed and never rolls back a live commit");
  take(full.scheduler->dispatch());
  take(full.binding->sync());
  take(full.scheduler->dispatch());
  check(full.completions.empty(), "rejected completion is not silently retried by later synchronization");

  Fixture overwritten;

  overwritten.changeHook = [&] {
    take(overwritten.model->set(take(NumericValue::create(77))));
  };
  overwritten.mouse(0, MouseAction::PRESS);
  check(overwritten.value() == 77 && overwritten.changes.back().snapshot->value.value() == 0 &&
          overwritten.completions.size() == 1 &&
          overwritten.completions.back().status == SliderOperationStatus::CONFLICT &&
          overwritten.completions.back().lastUserUpdate->snapshot == overwritten.changes.back().snapshot,
        "reentrant external write retains exact original user receipt and wins the numeric source");

  Fixture removed;

  removed.changeHook = [&] {
    take(removed.tree->remove(removed.node));
  };
  removed.mouse(0, MouseAction::PRESS);
  take(removed.scheduler->dispatch());
  check(removed.value() == 0 && removed.completions.empty(),
        "removal from live onChange suppresses later widget and completion delivery");
}

void managedModelAndProperty() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto source = take(owner->numericState(1.25, {-5, 5, 0.5}));
  ui::Ref<Slider> first;
  ui::Ref<Slider> second;

  take(ui::mount(*owner, ui::hbox(ui::slider(first, source), ui::slider(second, 4.5))));
  check(!first.value().result(), "fresh numeric property has no fabricated assignment receipt");
  auto assigned = first.value() = 2.25;
  const auto receipt = take(assigned.result());

  check(receipt.changed && receipt.snapshot == take(source.snapshot()) && take(first.value().get()) == 2.25,
        "fluent assignment owns the exact source receipt and keeps off-grid values");
  auto equal = first.value() = 2.25;

  check(!take(equal.result()).changed && take(equal.result()).snapshot->revision == receipt.snapshot->revision,
        "equal fluent assignment does not create a revision");
  auto rejected = first.value() = std::numeric_limits<double>::quiet_NaN();

  check(!rejected.result() && rejected.result().error().numeric == NumericError::NOT_FINITE &&
          take(source.snapshot())->value.value() == 2.25,
        "fluent rejection preserves the typed numeric cause and original source");
  auto copied = first.value() = second.value();

  check(take(copied.result()).snapshot->value.value() == 4.5 && take(source.snapshot())->value.range().step == 0.5,
        "proxy assignment copies one value without rebinding or replacing receiver bounds");
  take(second.value().set(7).result());
  check(take(first.value().get()) == 4.5, "proxy copy never creates a live binding");

  int changed{};
  int finished{};

  take(owner->onChange(first.node(), [&](double value) {
    changed += value == 5 ? 1 : 100;
  }));
  take(owner->onEditingFinished(first.node(), [&](const SliderOperationEvent &) {
    ++finished;
  }));
  take(source.set(4.75));
  check(changed == 0, "managed external writes do not invoke user changes");
  take(app->router().invoke(first.node(), ActionEvent{Action::MOVE_RIGHT}));
  check(take(source.snapshot())->value.value() == 5 && changed == 1 && finished == 1,
        "managed user callback receives the committed numeric value and one completion");
  const auto before = take(source.snapshot());
  auto invalid = source.configure(0, {5, 5, 1});

  check(!invalid && invalid.error().numeric == NumericError::INVALID_RANGE && take(source.snapshot()) == before,
        "atomic numeric configuration rejection leaves range, value and revision unchanged");
}

void initializationAndErrorPreservation() {
  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<Slider> implicit;

    take(ui::mount(*owner, ui::slider(implicit).range(-10, -1).step(0.5)));
    check(take(implicit.value().get()) == -1, "only omitted zero constructor fallback clamps into final range");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<Slider> explicitValue;
    auto mounted = ui::mount(*owner, ui::slider(explicitValue, 0).range(-10, -1));

    check(!mounted && mounted.error().numeric == NumericError::OUT_OF_RANGE && explicitValue.node() == Node<Slider>{},
          "explicit initial outside final bounds never clamps or publishes a Ref");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto source = take(owner->numericState(25));
    ui::Ref<Slider> conflict;
    auto mounted = ui::mount(*owner, ui::slider(conflict, source).range(0, 100));

    check(!mounted && mounted.error().numeric == NumericError::INITIAL_CONFLICT &&
            take(source.snapshot())->value.value() == 25,
          "even equal explicit range competes with external numeric model");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto invalid = owner->numericState(std::numeric_limits<double>::quiet_NaN());
    check(!invalid, "checked numeric source rejects NaN before an error is inspected");

    if (invalid) {
      return;
    }

    const auto original = invalid.error();
    ui::Ref<Slider> unpublished;
    auto mounted = ui::mount(*owner, ui::hbox(ui::label("sibling"), ui::slider(unpublished, invalid)));

    check(!mounted && mounted.error().numeric == original.numeric && mounted.error().phase == original.phase &&
            mounted.error().node == original.node && unpublished.node() == Node<Slider>{},
          "checked model failure survives nested erasure before native node work");
  }
}

void resourceParity() {
  for (const bool invalidRange : {false, true}) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Slider>("Wide", [](ui::SceneResourceArguments) {
      return ui::slider(150, {100, 200, 1});
    }));
    const std::string text = invalidRange ? "scene{Wide{minimum=250}}" : "scene{Wide{minimum=160}}";
    const auto mounted = ui::mount(*owner, ui::resource(text, options));

    check(!mounted &&
            mounted.error().numeric == (invalidRange ? NumericError::INVALID_RANGE : NumericError::OUT_OF_RANGE) &&
            mounted.error().sceneResource && mounted.error().sceneResource->propertyPath &&
            mounted.error().sceneResource->propertyPath->ends_with(".minimum") &&
            mounted.error().sceneResource->offset == text.find("minimum"),
          "sparse causal minimum retains its own source leaf and offset on numeric rejection");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<Slider> reference;
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Slider>("Wide", [=](ui::SceneResourceArguments) {
      return ui::slider(reference, 150, {100, 200, 1});
    }));
    auto mounted = ui::mount(*owner, ui::resource("scene{Wide{value=160}}", options));

    check(mounted && take(reference.value().get()) == 160,
          "sparse resource value validates against the actual factory range");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto source = take(owner->numericState(150, {100, 200, 1}));
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Slider>("WideBound", [=](ui::SceneResourceArguments) {
      return ui::slider(source);
    }));
    auto mounted = ui::mount(*owner, ui::resource("scene{WideBound{value=160}}", options));

    check(!mounted && mounted.error().numeric == NumericError::INITIAL_CONFLICT && mounted.error().sceneResource &&
            take(source.snapshot())->value.value() == 150,
          "sparse resource override reports source conflict before fallback range validation");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Slider>("WideInvalid", [](ui::SceneResourceArguments) {
      return ui::slider(150, {100, 200, 1});
    }));
    auto mounted = ui::mount(*owner, ui::resource("scene{WideInvalid{value=260}}", options));

    check(!mounted && mounted.error().numeric == NumericError::OUT_OF_RANGE && mounted.error().sceneResource &&
            mounted.error().sceneResource->propertyPath &&
            mounted.error().sceneResource->propertyPath->ends_with(".value"),
          "final merged numeric rejection retains the supplied resource leaf context");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto node = take(ui::mount(
      *owner,
      ui::resource(
        "scene{Slider#gain{value=2.5 minimum=-5 maximum=5 step=0.5 pageSteps=4 readOnly=true preferredWidth=17}}",
        options)));
    bool valid{};

    take(app->tree().read(node, [&](const Widget &widget) {
      const auto *slider = dynamic_cast<const Slider *>(&widget);

      valid = slider && slider->value() == 2.5 && slider->range() == NumericRange{-5, 5, 0.5} && slider->readOnly();
    }));
    check(valid && take(app->tree().measure(node, {})).width == 17,
          "configuration resource mounts actual numeric values, readOnly and intrinsic sizing");
    take(app->router().invoke(node, ActionEvent{Action::MOVE_RIGHT}));
    take(app->tree().read(node, [&](const Widget &widget) {
      check(dynamic_cast<const Slider &>(widget).value() == 2.5, "resource readOnly gates real native input");
    }));
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto source = take(owner->numericState(25));
    ui::Ref<Slider> external;
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Slider>("Bound", [=](ui::SceneResourceArguments) {
      return ui::slider(external, source).readOnly(true);
    }));
    take(ui::mount(*owner, ui::resource("scene{Bound{readOnly=false}}", options)));
    take(app->router().invoke(external.node(), ActionEvent{Action::MOVE_RIGHT}));
    check(take(source.snapshot())->value.value() == 26, "present false resource readOnly overrides factory true");
  }

  {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto source = take(owner->numericState(25));
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Slider>("Bound", [=](ui::SceneResourceArguments) {
      return ui::slider(source);
    }));
    auto mounted = ui::mount(*owner, ui::resource("scene{Bound{value=25}}", options));

    check(!mounted && mounted.error().numeric == NumericError::INITIAL_CONFLICT &&
            take(source.snapshot())->value.value() == 25,
          "custom resource factory cannot silently overwrite external numeric source, even with equal value");
  }
}

void semanticNumericErrors() {
  const auto verify = [](ui::Description<Slider> recipe, NumericError expected) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    auto mounted = ui::mount(*owner, std::move(recipe));

    check(!mounted && mounted.error().numeric == expected,
          "declarative numeric rejection preserves exact domain cause before construction");
  };
  const auto nan = std::numeric_limits<double>::quiet_NaN();

  verify(ui::slider(nan), NumericError::NOT_FINITE);
  verify(ui::slider(0).range(nan, 100), NumericError::NOT_FINITE);
  verify(ui::slider(0).step(nan), NumericError::NOT_FINITE);
  verify(ui::slider(0).step(0), NumericError::INVALID_STEP);
  verify(ui::slider(0).step(-1), NumericError::INVALID_STEP);

  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  const auto resource = ui::mount(*owner, ui::resource("scene{Slider{step=0}}", options));

  check(!resource && resource.error().numeric == NumericError::INVALID_STEP && resource.error().sceneResource,
        "finite invalid resource increment keeps numeric cause and original resource context");
}

void numericOnlyCaptionCapability() {
  Fixture native;
  const auto caption = setControlText(*native.tree, Node<Control>{native.node}, take(GraphemeText::from("unused")));

  check(!caption && caption.error() == TreeError::INVALID_ARGUMENT && native.value() == 25,
        "native base-node caption mutation rejects numeric-only Slider");

  auto app = take(Application::create());
  auto managed = take(ManagedScene::create(*app));
  ui::Ref<Slider> reference;

  take(ui::mount(*managed, ui::slider(reference, 25)));
  const auto bound = managed->bindText(Node<Control>{reference.node()}, take(managed->state(std::string("unused"))));

  check(!bound && std::get_if<TreeError>(&bound.error().cause) &&
          std::get<TreeError>(bound.error().cause) == TreeError::INVALID_ARGUMENT &&
          take(reference.value().get()) == 25,
        "base-node bindText cannot retain an invisible numeric caption binding");

  auto owner = ui::scene(ui::slider(25).id("numeric"));
  const auto base = owner.get<Control>("numeric");
  const auto erased = owner.get("numeric");
  const auto numeric = owner.get<Slider>("numeric");
  const auto source = base.text().source();

  take(owner.prepareChecked());
  const auto baseText = base.text().get();
  const auto erasedText = erased.text().get();
  const auto sourceText = source.snapshot();
  const auto invalid = [](const auto &result) {
    return !result && std::get_if<TreeError>(&result.error().cause) &&
           std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT;
  };

  check(invalid(baseText) && invalid(erasedText) && invalid(sourceText),
        "base and erased visible-caption getters reject Slider instead of returning empty text");
  auto assigned = base.text() = "unused";

  check(invalid(assigned.result()) && take(numeric.value().get()) == 25,
        "high-level base caption assignment keeps numeric source and typed failure");
  take(owner.close());
}

void deletingNumericProxyReporter() {
  for (const bool missingSource : {false, true}) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<Slider> reference;
    ui::Ref<Slider> absent;

    take(ui::mount(*owner, ui::slider(reference, 25)));
    auto proxy = std::make_unique<ui::NumericProperty>(reference.value());
    int reports{};

    take(owner->onError([&](const ManagedDiagnostic &) {
      ++reports;
      proxy.reset();
    }));
    const auto observation =
      missingSource ? (*proxy = absent.value()) : proxy->set(std::numeric_limits<double>::quiet_NaN());
    const auto receipt = observation.result();

    check(!proxy && reports == 1 && !receipt && take(reference.value().get()) == 25 &&
            (missingSource || receipt.error().numeric == NumericError::NOT_FINITE),
          "numeric proxy error observation survives reporter deletion of the original proxy");
  }
}

void reentrantNumericProxyReporter() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<Slider> reference;
  ui::Ref<Slider> absent;

  take(ui::mount(*owner, ui::slider(reference, 25)));
  auto proxy = reference.value();
  const auto original = absent.value().get().error();

  take(owner->onError([&](const ManagedDiagnostic &) {
    (void)proxy.set(30);
  }));
  const auto observation = proxy = absent.value();
  const auto receipt = observation.result();

  check(!receipt && std::get_if<TreeError>(&receipt.error().cause) &&
          std::get<TreeError>(receipt.error().cause) == std::get<TreeError>(original.cause),
        "source failure observation retains its own error across reporter reentry into the same proxy");
}

void numericOperationObservations() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<Slider> reference;
  ui::Ref<Slider> absent;

  take(ui::mount(*owner, ui::slider(reference, 25)));
  auto fresh = reference.value();
  const auto origin = std::source_location::current();
  const auto failed = fresh.set(std::numeric_limits<double>::quiet_NaN(), origin);
  const auto freshReceipt = fresh.result();
  const auto failedReceipt = failed.result();

  check(!freshReceipt && std::get_if<TreeError>(&freshReceipt.error().cause) &&
          std::get<TreeError>(freshReceipt.error().cause) == TreeError::INVALID_ARGUMENT &&
          !freshReceipt.error().operation && failedReceipt.error().numeric == NumericError::NOT_FINITE &&
          failedReceipt.error().operation && std::string_view(failedReceipt.error().operation) == "slider.value" &&
          failedReceipt.error().operationOrigin && failedReceipt.error().operationOrigin->line() == origin.line(),
        "numeric mutation returns a reported operation while the original fresh observation stays fresh");
  auto saved = reference.value().set(26);
  const auto original = take(saved.result()).snapshot;
  const auto changed = saved.set(27);
  const auto sourceFailure = saved = absent.value();
  const auto savedReceipt = saved.result();

  check(savedReceipt && savedReceipt->snapshot == original && take(changed.result()).snapshot->value.value() == 27 &&
          !sourceFailure.result() && sourceFailure.result().error().operation,
        "saved numeric observation keeps its previous receipt across later successful and failed mutations");
}

void emptyNumericCallbacks() {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<Slider> reference;

  take(ui::mount(*owner, ui::slider(reference, 25)));
  const auto pointer = owner->onChange(reference.node(), static_cast<void (*)(double)>(nullptr));
  const auto function = owner->onChange(reference.node(), std::function<void(double)>{});
  const auto invalid = [](const auto &result) {
    return !result && std::get_if<TreeError>(&result.error().cause) &&
           std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT;
  };

  check(invalid(pointer) && invalid(function), "empty numeric callbacks are rejected before value adaptation");

  if (!pointer && !function) {
    int calls{};

    take(owner->onChange(reference.node(), [&](double) {
      ++calls;
    }));
    take(app->router().invoke(reference.node(), ActionEvent{Action::MOVE_RIGHT}));
    check(calls == 1, "rejected empty callbacks leave no installed registration");
  }
}

void deletingSceneCallbackAdmission() {
  for (const int trigger : {0, 1, 2}) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    ui::Ref<Slider> reference;

    take(ui::mount(*owner, ui::slider(reference, 25)));

    struct Callback {
      std::unique_ptr<ManagedScene> *owner;
      int trigger;

      Callback(std::unique_ptr<ManagedScene> *source, int when) : owner(source), trigger(when) {
      }

      Callback(const Callback &other) : owner(other.owner), trigger(other.trigger) {
        if (trigger == 1) {
          owner->reset();
        }
      }

      Callback(Callback &&other) : owner(other.owner), trigger(other.trigger) {
        if (trigger == 2) {
          owner->reset();
        }
      }

      explicit operator bool() {
        if (trigger == 0) {
          owner->reset();
        }
        return true;
      }

      void operator()(const SliderOperationEvent &) {
      }
    };

    const auto registered = owner->onEditingFinished(reference.node(), Callback{&owner, trigger});

    check(!owner && !registered && std::get_if<TreeError>(&registered.error().cause) &&
            std::get<TreeError>(registered.error().cause) == TreeError::CLOSED,
          "callback bool/copy/move can destroy an idle scene and admission returns CLOSED without registration");
  }
}
} // namespace

int main() {
  numericValidation();
  numericInput();
  numericOracle();
  nativeKeyboard();
  reentrantSyncCompletion();
  widePointerAnchor();
  stationaryThumb();
  nativeDrag();
  nativeClickOnlyAndRemoval();
  cancelledReleaseAndCaptureExchange();
  eligibilityAfterCompletion();
  unknownButtonContinuation();
  exactPartialDeliveryAndReentry();
  managedModelAndProperty();
  initializationAndErrorPreservation();
  resourceParity();
  semanticNumericErrors();
  numericOnlyCaptionCapability();
  emptyNumericCallbacks();
  reentrantNumericProxyReporter();
  numericOperationObservations();
  deletingNumericProxyReporter();
  deletingSceneCallbackAdmission();
  std::cout << "Slider native failures: " << failures << '\n';

  return failures == 0 ? 0 : 1;
}
