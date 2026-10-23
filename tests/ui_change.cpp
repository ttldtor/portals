// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
struct UnsupportedChange {
  int operator()(CheckState) const;
};

struct BoolReceiptFallback {
  int *valueCalls;
  int *receiptCalls;

  int operator()(bool) const {
    ++*valueCalls;

    return 1;
  }

  void operator()(const PropertyUpdate<bool> &receipt) const {
    check(receipt.changed && receipt.snapshot->value, "bool fallback receives the original committed receipt");
    ++*receiptCalls;
  }
};

struct StateReceiptFallback {
  int *valueCalls;
  int *receiptCalls;

  int operator()(CheckState) const {
    ++*valueCalls;

    return 1;
  }

  void operator()(const PropertyUpdate<CheckState> &receipt) const {
    check(receipt.changed && receipt.snapshot->value == CheckState::CHECKED,
          "state fallback receives the original committed receipt");
    ++*receiptCalls;
  }
};

struct GenericValueOnly {
  int *calls;

  template <class T> auto operator()(const T &) const {
    static_assert(std::same_as<T, bool> || std::same_as<T, CheckState>);
    ++*calls;
  }
};

static_assert(detail::ChangeAction<BoolReceiptFallback, bool>);
static_assert(detail::ChangeAction<StateReceiptFallback, CheckState>);
static_assert(detail::ChangeAction<GenericValueOnly, bool>);
static_assert(detail::ChangeAction<GenericValueOnly, CheckState>);
static_assert(!detail::ChangeAction<UnsupportedChange, CheckState>);
static_assert(detail::ChangeAction<decltype([](bool) {
                                   }),
                                   bool>);
static_assert(!detail::ChangeAction<decltype([](bool) {
                                    }),
                                    CheckState>);
static_assert(detail::ChangeAction<decltype([](auto) {
                                   }),
                                   bool>);
static_assert(detail::ChangeAction<decltype([](auto) {
                                   }),
                                   CheckState>);
static_assert(std::is_nothrow_copy_constructible_v<PropertyUpdate<bool>>);
static_assert(std::is_nothrow_copy_constructible_v<PropertyUpdate<CheckState>>);

void cyclesAndModelRejection() {
  Fixture f;
  ui::Ref<CheckBox> method, pipe;
  take(ui::mount(
    *f.scene, ui::column(ui::checkbox(method, "method").threeState(), ui::checkbox(pipe, "pipe") | ui::threeState())));

  for (const auto expected : {CheckState::CHECKED, CheckState::MIXED, CheckState::UNCHECKED}) {
    for (const auto node : {method.node(), pipe.node()}) {
      take(f.app->router().invoke(node, {Action::ACTIVATE}));
      take(f.app->tree().read(node, [&](const CheckBox &widget) {
        check(widget.checkState() == expected, "method and pipe select the real three-state cycle");
      }));
    }
  }

  const auto model = take(f.scene->state(false));

  for (const bool usePipe : {false, true}) {
    ui::Ref<CheckBox> rejected;
    auto description = ui::checkbox(rejected, "bool", model);
    auto mounted =
      ui::mount(*f.scene, usePipe ? std::move(description) | ui::threeState() : std::move(description).threeState());
    check(!mounted && treeCause(mounted.error(), TreeError::INVALID_ARGUMENT), "bool model rejects three-state");
    check(rejected.node() == Node<CheckBox>{}, "failed mount never publishes its reference");
  }

  auto initial = ui::mount(*f.scene, ui::checkbox("bool initial", false).threeState());
  auto mixed = ui::mount(*f.scene, ui::checkbox("bound", model).settings({.state = CheckState::MIXED}));
  check(!initial && treeCause(initial.error(), TreeError::INVALID_ARGUMENT), "bool initial rejects three-state");
  check(!mixed && treeCause(mixed.error(), TreeError::INVALID_ARGUMENT), "bool model rejects mixed settings");

  take(ui::mount(*f.scene, ui::checkbox("authority", model).settings({.state = CheckState::CHECKED})));
  check(!take(model.snapshot())->value, "settings never replace the explicit bound model value");
}

void valuesReceiptsAndRegistration() {
  Fixture f({}, 1);
  ui::Ref<CheckBox> boolean, state;
  ui::Ref<ThreeState> derived;
  const auto model = take(f.scene->state(false));
  PropertyUpdate<bool> legacy, advanced;
  PropertyUpdate<CheckState> stateReceipt;
  int calls{}, generic{}, typed{};
  take(ui::mount(*f.scene, ui::column(ui::checkbox(boolean, "bool", model,
                                                   [&](CheckBoxResult result) -> std::expected<void, TreeError> {
                                                     legacy = take(result);
                                                     // Move the model again: late delivery must retain the original
                                                     // activation snapshot.
                                                     take(model.set(false));

                                                     return {};
                                                   }),
                                      ui::checkbox(state, "state"), ui::threeState(derived, "derived"))));
  take(f.scene->onChange(boolean.node(), [&](bool value) {
    check(value, "ordinary callback receives the committed activation value");
    ++calls;
  }));
  take(f.scene->onChange(boolean.node(), [&](const PropertyUpdate<bool> &receipt) {
    advanced = receipt;
  }));
  take(f.scene->onChange(state.node(), [&](auto value) {
    static_assert(std::same_as<decltype(value), bool> || std::same_as<decltype(value), CheckState>);
    ++generic;
  }));
  take(f.scene->onChange(state.node(), [&](const PropertyUpdate<CheckState> &receipt) {
    stateReceipt = receipt;
  }));
  take(f.scene->onChange(Node<CheckBox>{derived.node()}, [&](CheckState) {
    ++typed;
  }));

  auto mismatch = f.scene->onChange(state.node(), [](bool) {
  });
  check(!mismatch && treeCause(mismatch.error(), TreeError::INVALID_ARGUMENT), "wrong backing fails admission");
  auto duplicate = f.scene->bind(boolean.node(), model);
  check(!duplicate && treeCause(duplicate.error(), TreeError::INVALID_ARGUMENT), "late handlers do not rebind input");

  take(model.set(true));
  take(model.set(false));
  check(calls == 0, "external writes never deliver a user change");
  take(f.app->router().invoke(boolean.node(), {Action::ACTIVATE}));
  check(calls == 1 && !take(model.snapshot())->value, "one input delivers one original change despite legacy mutation");
  check(advanced.snapshot == legacy.snapshot && advanced.changed && advanced.rejected != 0 &&
          advanced.notification_error == legacy.notification_error && advanced.accepted == legacy.accepted &&
          advanced.cancelled == legacy.cancelled && advanced.rejected == legacy.rejected,
        "advanced callback retains original owning receipt and rejected publication accounting");
  take(f.app->router().invoke(state.node(), {Action::ACTIVATE}));
  take(f.app->router().invoke(derived.node(), {Action::ACTIVATE}));
  check(generic == 1 && typed == 1, "generic values and derived views reuse the same binding");
  check(stateReceipt.changed && stateReceipt.snapshot->value == CheckState::CHECKED &&
          stateReceipt.snapshot->revision != 0,
        "CheckState advanced delivery retains its own typed revision");

  std::optional<ManagedError> wrongThread;
  std::thread worker([&] {
    auto result = f.scene->onChange(boolean.node(), [](bool) {
    });
    if (!result) {
      wrongThread = result.error();
    }
  });
  worker.join();
  check(wrongThread && treeCause(*wrongThread, TreeError::WRONG_THREAD), "registration retains UI affinity");
  take(f.scene->remove(state.node()));

  auto stale = f.scene->onChange(state.node(), [](CheckState) {
  });
  check(!stale && treeCause(stale.error(), TreeError::STALE_NODE), "retired node returns typed liveness error");
}

void fifoAndErrors() {
  Fixture f;
  ui::Ref<CheckBox> reference;
  const auto model = take(f.scene->state(false));
  std::vector<int> order;
  bool appendedLegacy{}, appendedLate{};
  take(
    ui::mount(*f.scene, ui::checkbox(reference, "order", model, [&](CheckBoxResult) -> std::expected<void, TreeError> {
      order.push_back(0);
      if (!std::exchange(appendedLegacy, true)) {
        take(f.scene->onChange(reference.node(), [&](bool) {
          order.push_back(2);
        }));
      }

      return {};
    })));
  take(f.scene->onChange(reference.node(), [&](bool) {
    order.push_back(1);
    if (!std::exchange(appendedLate, true)) {
      take(f.scene->onChange(reference.node(), [&](bool) {
        order.push_back(3);
      }));
    }
  }));
  take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
  check(order == std::vector{0, 1}, "additions during legacy and late delivery wait for the next activation");
  order.clear();
  take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
  check(order == std::vector{0, 1, 2, 3}, "retained handlers run FIFO after legacy");

  take(f.scene->onChange(reference.node(), [](bool) -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }));
  int afterFailure{};
  take(f.scene->onChange(reference.node(), [&](bool) {
    ++afterFailure;
  }));
  take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));

  const auto errors = take(f.scene->diagnostics());
  check(afterFailure == 0 && errors.errors.size() == 1 &&
          treeCause(errors.errors.back().error, TreeError::INVALID_GEOMETRY),
        "checked callback errors stop FIFO and report once without rolling back");
  check(take(model.snapshot())->value, "handler failure cannot veto model commit");
}

void receiptFallbackAndValuePrecedence() {
  Fixture f;
  ui::Ref<CheckBox> boolean, state;
  const auto model = take(f.scene->state(false));
  int valueCalls{}, receiptCalls{}, genericCalls{};

  take(ui::mount(*f.scene,
                 ui::column(ui::checkbox(boolean, "bool fallback", model), ui::checkbox(state, "state fallback"))));
  take(f.scene->onChange(boolean.node(), BoolReceiptFallback{&valueCalls, &receiptCalls}));
  take(f.scene->onChange(state.node(), StateReceiptFallback{&valueCalls, &receiptCalls}));
  take(f.scene->onChange(boolean.node(), GenericValueOnly{&genericCalls}));
  take(f.scene->onChange(state.node(), GenericValueOnly{&genericCalls}));
  take(f.app->router().invoke(boolean.node(), {Action::ACTIVATE}));
  take(f.app->router().invoke(state.node(), {Action::ACTIVATE}));
  check(valueCalls == 0 && receiptCalls == 2, "unsupported value results select the admitted receipt overload");
  check(genericCalls == 2, "admitted generic values do not instantiate receipt candidates");
}

void removalAndOwnerDestruction() {
  for (const bool destroyOwner : {false, true}) {
    Fixture f;
    ui::Ref<CheckBox> reference;
    int later{};
    take(ui::mount(*f.scene, ui::checkbox(reference, "retire")));
    auto capture = std::make_shared<int>(7);
    std::weak_ptr<int> weak = capture;
    take(f.scene->onChange(reference.node(), [&, capture](CheckState) {
      if (destroyOwner) {
        f.scene.reset();
      } else {
        take(f.scene->remove(reference.node()));
      }
      check(!weak.expired() && *capture == 7, "executing handler capture survives retirement");
    }));
    take(f.scene->onChange(reference.node(), [&](CheckState) {
      ++later;
    }));
    capture.reset();
    take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
    check(later == 0 && weak.expired(), "retirement stops remaining callbacks and releases captures");
  }

  for (const bool throws : {false, true}) {
    Fixture f;
    ui::Ref<CheckBox> reference;
    const auto model = take(f.scene->state(false));
    int later{};
    take(ui::mount(*f.scene, ui::checkbox(reference, "legacy destruction", model,
                                          [&](CheckBoxResult) -> std::expected<void, TreeError> {
                                            f.scene.reset();
                                            if (throws) {
                                              throw std::runtime_error("legacy failure after destruction");
                                            }

                                            return std::unexpected(TreeError::INVALID_GEOMETRY);
                                          })));
    take(f.scene->onChange(reference.node(), [&](bool) {
      ++later;
    }));
    take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
    check(!f.scene && later == 0, "legacy destruction guards failure and exception diagnostics");
  }
}

void budgetsAndAllocation() {
  ManagedOptions options;
  options.maxChangeHandlers = 1;
  options.maxChangeHandlersPerNode = 1;
  Fixture f(options);
  ui::Ref<CheckBox> first, second;
  take(ui::mount(*f.scene, ui::column(ui::checkbox(first, "first"), ui::checkbox(second, "second"))));
  int delivered{};

  take(f.scene->onChange(first.node(), [&](CheckState) {
    ++delivered;
  }));

  auto full = f.scene->onChange(second.node(), [](CheckState) {
  });
  check(!full && treeCause(full.error(), TreeError::NODE_LIMIT), "global callback budget is bounded");
  take(f.app->router().invoke(first.node(), {Action::ACTIVATE}));
  check(delivered == 1, "failed registration neither consumes budget nor publishes a callback");
  take(f.scene->remove(first.node()));
  take(f.scene->onChange(second.node(), [](CheckState) {
  }));
  take(f.scene->close());

  options.maxChangeHandlers = 0;
  auto invalid = ManagedScene::create(*f.app, options);
  check(!invalid && treeCause(invalid.error(), TreeError::INVALID_ARGUMENT), "zero change budget is invalid");
}

std::function<void()> *admissionReentry{};

struct AdmissionCapture {
  std::shared_ptr<int> lifetime;

  void operator()(CheckState) const {
  }
};

void reenterAdmission() {
  (*admissionReentry)();
}

void allocationReentry() {
  std::size_t allocations{};
  {
    Fixture f;
    ui::Ref<CheckBox> reference;
    take(ui::mount(*f.scene, ui::checkbox(reference, "measure reentry")));
    auto capture = std::make_shared<int>();

    const auto before = allocationTest::attempts;

    take(f.scene->onChange(reference.node(), AdmissionCapture{std::move(capture)}));
    allocations = allocationTest::attempts - before;
  }

  for (const int mode : {0, 1, 2}) {
    for (std::size_t point{}; point < allocations; ++point) {
      ManagedOptions options;
      options.maxChangeHandlers = 1;
      options.maxChangeHandlersPerNode = 1;
      Fixture f(options);
      ui::Ref<CheckBox> first, second;
      take(ui::mount(*f.scene, ui::column(ui::checkbox(first, "outer"), ui::checkbox(second, "inner"))));
      bool entered{}, innerAdmitted{};
      std::optional<ManagedError> innerError;
      auto capture = std::make_shared<int>();
      const auto captured = std::weak_ptr(capture);
      std::function<void()> reentry = [&] {
        entered = true;

        if (mode == 2) {
          f.scene.reset();

          return;
        }

        auto inner = mode == 1 ? f.scene->remove(first.node()) : f.scene->onChange(second.node(), [](CheckState) {
        });

        innerAdmitted = bool(inner);

        if (!inner) {
          innerError = inner.error();
        }
      };

      admissionReentry = &reentry;
      allocationTest::hookAfter = static_cast<std::ptrdiff_t>(point);
      allocationTest::hook = reenterAdmission;

      auto outer = f.scene->onChange(first.node(), AdmissionCapture{std::move(capture)});

      allocationTest::hook = nullptr;
      admissionReentry = nullptr;
      check(entered, "each registration allocation exercises one-shot reentry");

      if (mode == 2) {
        check(!outer && treeCause(outer.error(), TreeError::CLOSED),
              "owner destruction during admission returns CLOSED without accessing the retired owner");
        check(captured.expired(), "destroyed owner admission releases its callback captures");

        continue;
      }

      check(!(outer && innerAdmitted), "allocation reentry cannot admit both outer and inner mutations");

      if (innerAdmitted) {
        check(!outer && treeCause(outer.error(), mode == 1 ? TreeError::STALE_NODE : TreeError::NODE_LIMIT),
              "successful inner mutation is reflected by final outer admission");
      } else {
        check(outer && innerError && treeCause(*innerError, TreeError::WOULD_BLOCK),
              "guarded publication rejects inner mutation with WOULD_BLOCK");
      }

      check(outer || captured.expired(), "rejected admission releases its callback captures");

      if (mode == 1) {
        take(f.scene->remove(first.node()));
        check(captured.expired(), "retirement releases admitted callback captures");
        take(f.scene->onChange(second.node(), [](CheckState) {
        }));
      }
    }
  }

  ManagedOptions options;
  options.maxChangeHandlers = 4;
  options.maxChangeHandlersPerNode = 4;
  Fixture f(options);
  ui::Ref<CheckBox> reference;
  int delivered{};
  auto capture = std::make_shared<int>();
  const auto captured = std::weak_ptr(capture);

  take(ui::mount(*f.scene, ui::checkbox(reference, "capacity retry")));

  std::function<void()> reentry = [&] {
    for (int index{}; index < 2; ++index) {
      take(f.scene->onChange(reference.node(), [&](CheckState) {
        ++delivered;
      }));
    }
  };

  admissionReentry = &reentry;
  allocationTest::hookAfter = static_cast<std::ptrdiff_t>(allocations - 1);
  allocationTest::hook = reenterAdmission;

  auto outer = f.scene->onChange(reference.node(), AdmissionCapture{std::move(capture)});

  allocationTest::hook = nullptr;
  admissionReentry = nullptr;
  check(!outer && treeCause(outer.error(), TreeError::WOULD_BLOCK),
        "reentry exceeding staged capacity rejects outer admission without retrying internally");
  check(captured.expired(), "capacity rejection releases its unpublished callback captures");
  take(f.scene->onChange(reference.node(), [&](CheckState) {
    ++delivered;
  }));
  take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
  check(delivered == 3, "capacity rejection preserves inner handlers and permits a clean outer retry");
}

void allocationFailureSteps() {
  std::size_t required{};
  {
    Fixture f;
    ui::Ref<CheckBox> reference;
    take(ui::mount(*f.scene, ui::checkbox(reference, "measure admission")));

    const auto before = allocationTest::attempts;

    take(f.scene->onChange(reference.node(), [](CheckState) {
    }));
    required = allocationTest::attempts - before;
  }

  for (std::size_t step = 0; step < required; ++step) {
    ManagedOptions options;
    options.maxChangeHandlers = 1;
    options.maxChangeHandlersPerNode = 1;
    Fixture f(options);
    ui::Ref<CheckBox> reference;
    int delivered{};
    take(ui::mount(*f.scene, ui::checkbox(reference, "allocation rollback")));

    allocationTest::remaining = static_cast<std::ptrdiff_t>(step);
    auto failed = f.scene->onChange(reference.node(), [&](CheckState) {
      ++delivered;
    });
    allocationTest::remaining = -1;
    check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY), "each new admission allocation fails atomically");
    take(f.scene->onChange(reference.node(), [&](CheckState) {
      ++delivered;
    }));
    take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
    check(delivered == 1, "failed admission publishes no partial handler or budget");
  }

  Fixture f;
  const auto node = take(f.scene->emplace<CheckBox>(f.scene->root(), take(GraphemeText::from("chain allocation"))));
  const auto model = take(f.scene->state(false));

  allocationTest::remaining = 0;
  auto failed = f.scene->bind(node, model);
  allocationTest::remaining = -1;
  check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY), "new chain allocation leaves binding unpublished");
  take(f.scene->bind(node, model));
  take(f.scene->onChange(node, [](bool) {
  }));
}

void commandFailureReportsOnce() {
  Fixture f;
  ui::Ref<CheckBox> reference;
  ui::Ref<LineEdit> editor;
  take(ui::mount(*f.scene, ui::column(ui::checkbox(reference, "command"), ui::lineEdit(editor, ""))));
  take(f.scene->remove(editor.node()));
  take(f.scene->onChange(reference.node(), [&](CheckState) {
    return editor.undo();
  }));
  int after{};
  take(f.scene->onChange(reference.node(), [&](CheckState) {
    ++after;
  }));
  take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));

  const auto errors = take(f.scene->diagnostics());
  check(after == 0 && errors.errors.size() == 1 && treeCause(errors.errors.back().error, TreeError::STALE_NODE),
        "reported command receipt stops delivery without duplicate diagnostics");
}

void failedActivationAndLegacyFailure() {
  Fixture f;
  ui::Ref<CheckBox> reference;
  const auto model = take(f.scene->state(CheckState::UNCHECKED));
  int legacy{}, late{};
  take(ui::mount(*f.scene, ui::checkbox(reference, "failed activation", model,
                                        [&](CheckStateResult result) -> std::expected<void, TreeError> {
                                          ++legacy;
                                          check(!result && result.error() == TreeError::INVALID_ARGUMENT,
                                                "legacy still receives raw failed results");

                                          return {};
                                        })));
  take(f.scene->onChange(reference.node(), [&](CheckState) {
    ++late;
  }));
  take(model.set(static_cast<CheckState>(99)));
  take(f.app->router().invoke(reference.node(), {Action::ACTIVATE}));
  check(legacy == 1 && late == 0, "failed activation never emits a successful change");

  for (const bool throws : {false, true}) {
    Fixture owner;
    ui::Ref<CheckBox> node;
    take(ui::mount(*owner.scene, ui::checkbox(node, "legacy error", take(owner.scene->state(false)),
                                              [&](CheckBoxResult result) -> std::expected<void, TreeError> {
                                                check(result && result->changed,
                                                      "legacy failure happens after the real commit");
                                                if (throws) {
                                                  throw std::runtime_error("legacy failed");
                                                }

                                                return std::unexpected(TreeError::INVALID_GEOMETRY);
                                              })));
    take(owner.scene->onChange(node.node(), [&](bool) {
      ++late;
    }));
    take(owner.app->router().invoke(node.node(), {Action::ACTIVATE}));

    const auto errors = take(owner.scene->diagnostics());
    check(late == 0 && errors.errors.size() == 1 &&
            treeCause(errors.errors.back().error, throws ? TreeError::CALLBACK_FAILED : TreeError::INVALID_GEOMETRY),
          "legacy checked failure and exception keep exact diagnostics and stop late delivery");
  }
}

void scenePipe() {
  example::ReplayTerminal terminal({{10, " ", {}}}, 20, {30, 4});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<CheckBox> reference;
  int changed{};
  auto scene =
    ui::scene(ui::checkbox(reference, "pipe").threeState(), options) | ui::onChange(reference, [&](CheckState value) {
      check(value == CheckState::CHECKED, "scene pipe receives actual state");
      ++changed;
    });
  take(scene.run(terminal));
  check(changed == 1 && terminal.closed, "scene stage composes with existing input and cleanup");
}

void toastSeverity() {
  Fixture f;
  ui::NotificationCenter center;
  NotificationOptions options;
  options.capacity = 16;
  options.maxVisible = 8;
  take(ui::mount(*f.scene, ui::notifications(center, ui::label("body"), options)));
  take(center.post(ui::toast("info").success().info()));
  take(center.post(ui::toast("success").success()));
  take(center.post(ui::toast("warning").warning()));
  take(center.post(ui::toast("failure").error()));
  take(center.post(ui::toast("pipe info") | ui::info()));
  take(center.post(ui::toast("pipe success") | ui::error() | ui::success()));
  take(center.post(ui::toast("pipe warning") | ui::warning()));
  take(center.post(ui::toast("pipe failure", {.duration = std::nullopt}).action("action", [] {
  }) | ui::error()));

  const auto snapshot = take(center.snapshot());
  const std::array expected{ToastSeverity::INFO,    ToastSeverity::SUCCESS, ToastSeverity::WARNING,
                            ToastSeverity::FAILURE, ToastSeverity::INFO,    ToastSeverity::SUCCESS,
                            ToastSeverity::WARNING, ToastSeverity::FAILURE};
  check(snapshot->items.size() == expected.size(), "error is presentation and does not fail admission");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    check(snapshot->items[index].data->severity == expected[index], "method and pipe map existing severity roles");
  }
  check(snapshot->items.back().data->actionLabel.has_value() && !snapshot->items.back().deadline,
        "severity retains actions and persistent options");
}
} // namespace

int main() {
  try {
    cyclesAndModelRejection();
    valuesReceiptsAndRegistration();
    receiptFallbackAndValuePrecedence();
    fifoAndErrors();
    removalAndOwnerDestruction();
    budgetsAndAllocation();
    allocationReentry();
    allocationFailureSteps();
    commandFailureReportsOnce();
    failedActivationAndLegacyFailure();
    scenePipe();
    toastSeverity();
  } catch (const std::exception &failure) {
    std::cerr << failure.what() << '\n';

    return 1;
  }

  return 0;
}
