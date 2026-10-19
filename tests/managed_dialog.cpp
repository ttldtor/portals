// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{40, 12};

template <class Layer>
concept DialogLayerRecipe = requires(Layer &&layer) { ui::overlayHost(ui::label("Body"), std::forward<Layer>(layer)); };

static_assert(DialogLayerRecipe<ui::Description<Dialog>>);
static_assert(!DialogLayerRecipe<ui::Description<Label>>);

void check(bool condition, const char *message, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(message) + " at " + std::to_string(where.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  if (!result) {
    if constexpr (std::same_as<E, ManagedError>) {
      if (auto cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << "phase=" << int(result.error().phase) << " cause=" << int(*cause) << '\n';
      }
    } else if constexpr (std::same_as<E, TreeError>) {
      std::cerr << "tree=" << int(result.error()) << '\n';
    }
  }
  check(bool(result), "unexpected checked failure", where);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  auto value = std::get_if<TreeError>(&error.cause);
  return value && *value == expected;
}

void take(ui::DialogCommandResult result, std::source_location where = std::source_location::current()) {
  take(result.result(), where);
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));

  void layout() {
    take(scene->prepare(*app, VIEWPORT));
    for (int pass = 0; pass != 3; ++pass) {
      std::optional<LayoutSnapshot> snapshot;
      take(app->tree().frame([&] {
        auto bounds = take(CellRect::from({}, VIEWPORT));
        take(layoutTree(app->tree(), app->root(), bounds, bounds));
        snapshot.emplace(take(LayoutSnapshot::capture(
          app->tree(), scene->root(),
          scene->usesGeometrySnapshot() ? LayoutSnapshotProfile::GEOMETRY : LayoutSnapshotProfile::DIMENSIONS)));
      }));
      (void)take(app->tree().consumeFrameRequest());
      take(scene->prepareViews(*app, *snapshot));
      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        return;
      }
    }
    check(false, "placement failed to converge within existing three passes");
  }
};

void modalAndReturns() {
  Fixture f;
  ui::Ref<OverlayHost> host;
  ui::Ref<Dialog> first, second, empty;
  ui::Ref<Button> opener, inside, nested;
  int dismissals{}, background{};
  take(
    ui::mount(*f.scene, ui::overlayHost(host, ui::button("Open").reference(opener).onClick([&] {
      ++background;
    }),
                                        ui::dialog(first, "Confirm", ui::button("OK").reference(inside).onClick([] {
                                        }))
                                          .onDismiss([&](DialogDismissReason) {
                                            ++dismissals;
                                          }),
                                        ui::dialog(second, "Nested", ui::button("Return").reference(nested).onClick([] {
                                        })),
                                        ui::dialog(empty, "Empty", ui::label("No focus target")))));
  f.layout();
  take(opener.focus());
  check(!take(first.opened()), "cold layer closed");
  take(first.open());
  check(take(first.opened()) && take(f.app->router().focused()) == inside.node(), "modal entry publishes focus");
  auto outside = f.app->router().setFocus(opener.node());
  check(!outside && outside.error() == TreeError::INVALID_ARGUMENT, "modal blocks explicit outside focus");
  auto invoked = f.app->router().invoke(opener.node(), {Action::ACTIVATE});
  check(!invoked && invoked.error() == TreeError::INVALID_ARGUMENT && background == 0, "modal blocks outside invoke");
  take(f.app->router().traverse());
  check(take(f.app->router().focused()) == inside.node(), "Tab confined to modal subtree");
  (void)take(f.app->tree().consumeFrameRequest());
  take(first.open());
  check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "equal open does not invalidate");
  take(second.open());
  auto lower = first.dismiss();
  check(!lower && cause(lower.error(), TreeError::WOULD_BLOCK), "lower modal cannot dismiss");
  take(f.app->router().dispatch(KeyEvent{InputKey::ESCAPE}));
  take(f.app->router().dispatch(KeyEvent{.key = InputKey::ESCAPE, .action = KeyAction::REPEAT}));
  take(f.app->router().dispatch(KeyEvent{.key = InputKey::ESCAPE, .action = KeyAction::RELEASE}));
  check(take(first.opened()) && !take(second.opened()), "Escape repeat/release cannot dismiss the next modal");
  check(take(f.app->router().focused()) == inside.node(), "nested modal returns to lower focus");
  take(first.dismiss());
  check(take(f.app->router().focused()) == opener.node() && dismissals == 1, "dismiss returns exact opener once");
  take(first.dismiss());
  check(dismissals == 1, "closed callback not replayed");
  take(empty.open());
  take(f.app->router().invoke(empty.node(), {Action::CANCEL}));
  check(!take(empty.opened()), "empty modal accepts root cancellation");
  f.layout();
  auto order = take(f.app->tree().inspect(host.node()));
  check(order.children.front() == opener.node(), "ordinary body remains first and retains identity");
}

void nonmodalAndRetirement() {
  Fixture f;
  ui::Ref<Button> background, inside;
  ui::Ref<Dialog> dialog;
  int calls{};
  DialogOptions options;
  options.mode = DialogMode::NON_MODAL;
  take(
    ui::mount(*f.scene, ui::overlayHost(ui::button("Background").reference(background).onClick([] {
    }),
                                        ui::dialog(dialog, "Tools", ui::button("Tool").reference(inside).onClick([&] {
                                          ++calls;
                                        }),
                                                   options))));
  f.layout();
  take(background.focus());
  take(dialog.open());
  check(take(f.app->router().focused()) == background.node(), "nonmodal open never steals focus");
  f.layout();
  auto geometry = take(f.app->tree().inspect(inside.node())).geometry;
  MouseInputEvent press;
  press.column = geometry.surface.origin().x;
  press.row = geometry.surface.origin().y;
  press.button = MouseButton::LEFT;
  press.action = MouseAction::PRESS;
  take(f.app->router().dispatch(press));
  check(take(f.app->router().focused()) == background.node(), "nonmodal pointer does not steal focus");
  take(inside.focus());
  take(dialog.dismiss());
  check(take(f.app->router().focused()) == background.node(), "actual nonmodal focus entry returns");
  take(dialog.open());
  take(inside.focus());
  take(f.app->tree().setVisible(dialog.node(), false));
  take(f.app->tree().setVisible(dialog.node(), true));
  check(!take(dialog.opened()), "raw hide/show cannot revive activation");
  take(dialog.open());
  take(f.app->tree().remove(dialog.node()));
  auto stale = dialog.open();
  check(!stale && cause(stale.error(), TreeError::STALE_NODE), "removed layer retains exact stale identity");
  take(f.app->router().focused());
  check(calls == 1, "nonmodal pointer action delivered exactly once");
}

void explicitFocusAfterRevocation() {
  Fixture f;
  ui::Ref<Button> opener, chosen;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::column(ui::button("Opener").reference(opener).onClick([] {
                                           }),
                                                      ui::button("Chosen").reference(chosen).onClick([] {
                                                      })),
                                           ui::dialog(dialog, "Modal", ui::button("Inside").onClick([] {
                                           })))));
  take(opener.focus());
  take(dialog.open());
  take(f.app->tree().setVisible(dialog.node(), false));
  take(chosen.focus());
  check(take(f.app->router().focused()) == chosen.node(), "ended modal cannot overwrite a later explicit focus choice");
}

void lowLevelRetirementBudget() {
  Fixture f;
  OverlayHostOptions options;
  options.maxLayers = 1;
  const auto makeHost = [&] {
    auto host = take(f.scene->emplace<OverlayHost>(f.scene->root(), options));
    auto body = take(f.scene->emplace<Widget>(host));
    take(f.scene->overlayHost(host, body));
    return host;
  };
  const auto makeDialog = [&](Node<OverlayHost> host) {
    auto root = take(f.scene->emplace<Dialog>(host));
    auto panel = take(f.scene->emplace<Widget>(root));
    auto body = take(f.scene->emplace<Widget>(panel));
    take(f.scene->dialog(root, panel, body));
    return root;
  };
  auto host = makeHost();
  auto first = makeDialog(host);
  take(f.app->tree().remove(first));
  auto second = makeDialog(host);
  take(f.scene->openDialog(second));
  take(f.scene->dismissDialog(second));
  take(f.app->tree().remove(host));
  auto replacement = makeHost();
  take(f.scene->openDialog(makeDialog(replacement)));
}

void captureAcrossDialogPublication(bool nonmodal, bool lateCapture) {
  Fixture f;
  ui::Ref<OverlayHost> host;
  ui::Ref<Label> owner;
  ui::Ref<Button> opener;
  ui::Ref<Dialog> dialog;
  DialogOptions options;
  options.mode = nonmodal ? DialogMode::NON_MODAL : DialogMode::MODAL;
  take(ui::mount(*f.scene, ui::overlayHost(host,
                                           ui::column(ui::label("Drag").reference(owner),
                                                      ui::button("Opener").reference(opener).onClick([] {
                                                      })),
                                           ui::dialog(dialog, "Capture", ui::button("Inside").onClick([] {
                                           }),
                                                      options))));
  take(f.app->router().setPointerCaptureCapabilities({true, true, true}));
  std::optional<PointerCapture> capture;
  bool rejectedLate{};
  auto binding = take(f.app->router().bind(
    owner.node(),
    [&](Label &, const RoutedEvent &event) {
      const auto *mouse = std::get_if<MouseInputEvent>(&event);
      if (mouse && mouse->action == MouseAction::PRESS) {
        capture.emplace(take(f.app->router().capturePointer(owner.node(), host.node())));
        if (lateCapture) {
          take(dialog.open());
          auto late = f.app->router().capturePointer(owner.node(), host.node());
          rejectedLate = !late && late.error() == TreeError::INVALID_ARGUMENT;
        }
      }
      return EventReply::HANDLED;
    },
    false));
  f.layout();
  take(opener.focus());
  const auto bounds = take(f.app->tree().inspect(owner.node())).geometry.surface;
  MouseInputEvent press;
  press.column = bounds.origin().x;
  press.row = bounds.origin().y;
  press.button = MouseButton::LEFT;
  press.action = MouseAction::PRESS;
  take(f.app->router().dispatch(press));
  check(capture.has_value(), "admitted outside press obtains the real existing capture owner");
  if (!lateCapture) {
    check(capture->connected(), "capture starts live with a broad host focusRoot");
    take(dialog.open());
  }
  if (nonmodal) {
    check(capture->connected() && take(f.app->router().focused()) == opener.node(),
          "nonmodal publication neither unconditionally cancels capture nor steals focus");
    check(take(f.app->router().pointerCapture()) == Node<Widget>{} && !capture->connected(),
          "existing global eligibility generation conservatively cancels the sequence on validity check");
  } else {
    check(!capture->connected() && take(f.app->router().pointerCapture()) == Node<Widget>{},
          "modal admission cancels the actual outside capture owner even with broad focusRoot");
    check(!lateCapture || rejectedLate, "same admitted outside press cannot reacquire capture after modal publication");
  }
}

void anchoredScrollAndFocusPreparation() {
  Fixture f;
  ui::Ref<ScrollArea> area;
  ui::Ref<Label> anchor;
  ui::Ref<Button> opener, inside;
  ui::Ref<Dialog> dialog;
  take(ui::mount(
    *f.scene,
    ui::overlayHost(
      ui::column(
        ui::scrollArea(area, ui::column(ui::label("zero"), ui::label("one"), ui::label("Anchor").reference(anchor),
                                        ui::label("three"), ui::label("four"), ui::label("five"), ui::label("six")))
          .height(4),
        ui::button("Opener").reference(opener).onClick([] {
        })),
      ui::dialog(dialog, "Scrolled modal",
                 ui::scrollArea(ui::column(ui::label("zero"), ui::label("one"), ui::label("two"), ui::label("three"),
                                           ui::label("four"), ui::label("five"), ui::label("six"),
                                           ui::button("Inside")
                                             .reference(inside)
                                             .onClick([] {
                                             })
                                             .focusDefault()))
                   .height(3)
                   .width(12))
        .anchor(anchor))));
  f.layout();
  take(opener.focus());
  take(dialog.open());
  f.layout();
  const auto panel = take(f.app->tree().inspect(dialog.node())).children.front();
  const auto before = take(f.app->tree().inspect(panel)).geometry.surface;
  auto focused = take(f.app->tree().inspect(inside.node())).geometry;
  check(take(f.app->router().focused()) == inside.node() && !focused.surface.intersection(focused.clip).empty(),
        "published focus observation reveals modal content through existing scroll preparation");
  take(area.setOffset(ScrollAxis::VERTICAL, {1}));
  f.layout();
  const auto after = take(f.app->tree().inspect(panel)).geometry.surface;
  const auto anchorBounds = take(f.app->tree().inspect(anchor.node())).geometry.surface;
  focused = take(f.app->tree().inspect(inside.node())).geometry;
  check(after.origin().y == before.origin().y - 1 && after.origin().y == anchorBounds.bottom() + 1 &&
          !focused.surface.intersection(focused.clip).empty(),
        "anchor motion and scroll focus pins converge inside existing three preparation passes");
}

void bodyAnchorsAndFallback() {
  Fixture f;
  ui::Ref<Label> anchor;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene,
                 ui::overlayHost(ui::column(ui::label("Anchor").reference(anchor), ui::button("Background").onClick([] {
                                 })),
                                 ui::dialog(dialog, "Anchored", ui::label("Body")).anchor(anchor))));
  DialogPlacement configured{};
  take(f.app->tree().read(dialog.node(), [&](const Dialog &widget) {
    configured = widget.options().placement;
  }));
  check(configured == DialogPlacement::BELOW_START, "cold finalization publishes configured placement");
  auto early = f.scene->openDialog(dialog.node());
  check(!early && cause(early.error(), TreeError::WOULD_BLOCK) && !take(dialog.opened()),
        "initial anchor without published geometry has no pending activation");
  f.layout();
  take(dialog.open());
  f.layout();
  auto root = take(f.app->tree().inspect(dialog.node()));
  auto panel = root.children.front();
  auto placed = take(f.app->tree().inspect(panel)).geometry;
  auto observed = take(f.app->tree().inspect(anchor.node())).geometry;
  check(root.geometry.surface.size() == VIEWPORT, "structural dialog root covers full host");
  check(placed.surface.origin().y == observed.surface.bottom() + 1,
        "body anchor is translated to host-local placement");
  take(dialog.dismiss());
  f.layout();
  take(f.app->tree().setEnabled(anchor.node(), false));
  take(dialog.open());
  f.layout();
  check(take(f.app->tree().inspect(panel)).geometry.surface.origin().y == observed.surface.bottom() + 1,
        "disabled visible anchor remains geometric input");
  take(f.scene->remove(anchor.node()));
  f.layout();
  placed = take(f.app->tree().inspect(panel)).geometry;
  check(placed.surface.origin().x == (VIEWPORT.width - placed.surface.size().width) / 2 &&
          placed.surface.origin().y == (VIEWPORT.height - placed.surface.size().height) / 2,
        "stale live anchor falls back to center within the same pass bound");
  take(f.app->tree().read(dialog.node(), [&](const Dialog &widget) {
    configured = widget.options().placement;
  }));
  check(configured == DialogPlacement::BELOW_START, "fallback clears geometry and preserves requested placement");
}

void coldAnchorRejection() {
  {
    Fixture f;
    ui::Ref<Dialog> first, second;
    auto result = ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::dialog(first, "First", ui::label("one")),
                                                      ui::dialog(second, "Second", ui::label("two")).anchor(first)));
    check(!result && cause(result.error(), TreeError::INVALID_ARGUMENT),
          "dialog-to-dialog anchor rejected before publication");
    check(first.node() == Node<Dialog>{} && second.node() == Node<Dialog>{},
          "failed anchor finalization publishes no Ref");
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "failed cold anchor rolls back entire host");
  }
  {
    Fixture f;
    ui::Ref<Label> unpublished;
    ui::Ref<Dialog> dialog;
    auto result = ui::mount(
      *f.scene, ui::overlayHost(ui::label("Body"), ui::dialog(dialog, "Cold", ui::label("body")).anchor(unpublished)));
    check(!result && cause(result.error(), TreeError::CLOSED), "unpublished anchor retains CLOSED");
  }
  {
    Fixture foreign, f;
    ui::Ref<Label> anchor;
    take(ui::mount(*foreign.scene, ui::label("Foreign").reference(anchor)));
    auto result =
      ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::dialog("Foreign", ui::label("body")).anchor(anchor)));
    check(!result && cause(result.error(), TreeError::WRONG_SESSION), "foreign anchor retains WRONG_SESSION");
  }
}

void modalPublicationDuringDelivery(bool nonmodal, bool removeOwner) {
  Fixture f;
  ui::Ref<OverlayHost> host;
  ui::Ref<Column> body;
  ui::Ref<Label> outside;
  ui::Ref<Dialog> dialog;
  DialogOptions options;
  options.mode = nonmodal ? DialogMode::NON_MODAL : DialogMode::MODAL;
  take(ui::mount(*f.scene,
                 ui::overlayHost(host, ui::column(ui::label("Outside").reference(outside)).reference(body).focusScope(),
                                 ui::dialog(dialog, "Published", ui::label("Inside"), options))));
  int outsideEvents{}, completed{}, bodyEvents{}, hostEvents{}, rootEvents{};
  bool reentryBlocked{};
  auto outsideBinding = take(f.app->router().bind(
    outside.node(),
    [&](Label &, const RoutedEvent &) {
      ++outsideEvents;
      take(dialog.open());
      auto nested = f.app->router().dispatch(KeyEvent{InputKey::ENTER});
      reentryBlocked = !nested && nested.error() == TreeError::WOULD_BLOCK;
      if (removeOwner) {
        take(f.app->tree().remove(outside.node()));
      }
      ++completed;
      return EventReply::IGNORED;
    },
    false));
  auto bodyBinding = take(f.app->router().bind(
    body.node(),
    [&](Column &, const RoutedEvent &) {
      ++bodyEvents;
      return EventReply::IGNORED;
    },
    false));
  auto hostBinding = take(f.app->router().bind(
    host.node(),
    [&](OverlayHost &, const RoutedEvent &) {
      ++hostEvents;
      return EventReply::IGNORED;
    },
    false));
  auto rootBinding = take(f.app->router().bind(
    f.scene->root(),
    [&](Widget &, const RoutedEvent &) {
      ++rootEvents;
      return EventReply::IGNORED;
    },
    false));
  f.layout();
  const auto bounds = take(f.app->tree().inspect(outside.node())).geometry.surface;
  MouseInputEvent press;
  press.column = bounds.origin().x;
  press.row = bounds.origin().y;
  press.button = MouseButton::LEFT;
  press.action = MouseAction::PRESS;
  const auto result = take(f.app->router().dispatch(press));
  std::cout << "publication nonmodal=" << nonmodal << " removeOwner=" << removeOwner << " outside=" << outsideEvents
            << " completed=" << completed << " body=" << bodyEvents << " host=" << hostEvents << " root=" << rootEvents
            << " status=" << int(result.status) << '\n';
  check(outsideEvents == 1 && completed == 1 && reentryBlocked,
        "already admitted callback completes once and nested routing remains blocked");
  if (nonmodal) {
    check(bodyEvents == 1 && hostEvents == 1 && rootEvents == 1 && result.status == DispatchStatus::UNHANDLED,
          "nonmodal publication preserves ordinary scope and ancestor delivery");
  } else {
    check(bodyEvents == 0 && hostEvents == 0 && rootEvents == 0,
          "new modal prevents every later outside ancestor admission");
    check(result.status == DispatchStatus::HANDLED && result.handler == dialog.node().id(),
          "new modal supplies physical shield disposition without retargeting");
  }
}

void modalReplacementDuringDelivery(bool reopen) {
  Fixture f;
  ui::Ref<OverlayHost> host;
  ui::Ref<Dialog> first, second;
  ui::Ref<Label> inside, replacement;
  int admitted{}, completed{}, replacementEvents{}, hostEvents{};
  take(ui::mount(*f.scene, ui::overlayHost(
                             host, ui::label("Body"), ui::dialog(first, "First", ui::label("Inside").reference(inside)),
                             ui::dialog(second, "Second", ui::label("Replacement").reference(replacement)))));
  auto insideBinding = take(f.app->router().bind(
    inside.node(),
    [&](Label &, const RoutedEvent &) {
      ++admitted;
      take(first.dismiss());
      if (reopen) {
        take(second.open());
      }
      ++completed;
      return EventReply::IGNORED;
    },
    false));
  auto replacementBinding = take(f.app->router().bind(
    replacement.node(),
    [&](Label &, const RoutedEvent &) {
      ++replacementEvents;
      return EventReply::HANDLED;
    },
    false));
  auto hostBinding = take(f.app->router().bind(
    host.node(),
    [&](OverlayHost &, const RoutedEvent &) {
      ++hostEvents;
      return EventReply::HANDLED;
    },
    false));
  take(first.open());
  f.layout();
  const auto bounds = take(f.app->tree().inspect(inside.node())).geometry.surface;
  MouseInputEvent press;
  press.column = bounds.origin().x;
  press.row = bounds.origin().y;
  press.button = MouseButton::LEFT;
  press.action = MouseAction::PRESS;
  const auto result = take(f.app->router().dispatch(press));
  check(admitted == 1 && completed == 1 && replacementEvents == 0 && hostEvents == 0,
        "dismiss/replacement never retargets or replays the original admitted event");
  check(result.status == DispatchStatus::HANDLED && result.handler == (reopen ? second.node().id() : first.node().id()),
        "current replacement or original closed modal preserves physical shield disposition");
}

void physicalShieldAndScopeAdmission() {
  Fixture f;
  ui::Ref<OverlayHost> host;
  ui::Ref<Dialog> dialog;
  ui::Ref<Column> body;
  ui::Ref<Button> background, inside, outside;
  int actions{}, hostEvents{};
  take(ui::mount(
    *f.scene, ui::column(ui::overlayHost(host,
                                         ui::column(ui::button("Background").reference(background).onClick([&] {
                                           ++actions;
                                         }))
                                           .reference(body)
                                           .focusScope(),
                                         ui::dialog(dialog, "Modal", ui::button("Inside").reference(inside).onClick([] {
                                         })))
                           .height(6),
                         ui::button("Outside host").reference(outside).onClick([&] {
                           ++actions;
                         }))));
  auto invalid = f.app->router().bind(host.node(), [](OverlayHost &, const RoutedEvent &) {
    return EventReply::IGNORED;
  });
  check(!invalid && invalid.error() == TreeError::INVALID_ARGUMENT, "structural host cannot become focusable");
  auto hostBinding = take(f.app->router().bind(
    host.node(),
    [&](OverlayHost &, const RoutedEvent &) {
      ++hostEvents;
      return EventReply::HANDLED;
    },
    false));
  f.layout();
  take(background.focus());
  take(dialog.open());
  f.layout();
  auto scope = body.focus();
  check(!scope && cause(scope.error(), TreeError::INVALID_ARGUMENT), "outside scope entry cannot clear confined focus");
  auto bounds = take(f.app->tree().inspect(host.node())).geometry.surface;
  MouseInputEvent press;
  press.column = bounds.origin().x;
  press.row = bounds.origin().y;
  press.button = MouseButton::LEFT;
  press.action = MouseAction::PRESS;
  check(take(f.app->router().dispatch(press)).status == DispatchStatus::HANDLED,
        "modal outside-panel shield consumes press");
  check(take(f.app->router().focused()) == inside.node() && hostEvents == 0 && actions == 0,
        "shield does not focus or bubble into host/background");
  const auto outsideBounds = take(f.app->tree().inspect(outside.node())).geometry.surface;
  press.column = outsideBounds.origin().x;
  press.row = outsideBounds.origin().y;
  check(take(f.app->router().dispatch(press)).status == DispatchStatus::HANDLED && actions == 0,
        "physical input outside the host is consumed without a typed outside-invoke error");
}
} // namespace

int main() {
  try {
    modalAndReturns();
    nonmodalAndRetirement();
    explicitFocusAfterRevocation();
    lowLevelRetirementBudget();
    captureAcrossDialogPublication(false, false);
    captureAcrossDialogPublication(true, false);
    captureAcrossDialogPublication(false, true);
    anchoredScrollAndFocusPreparation();
    bodyAnchorsAndFallback();
    coldAnchorRejection();
    modalPublicationDuringDelivery(false, false);
    modalPublicationDuringDelivery(false, true);
    modalPublicationDuringDelivery(true, false);
    modalReplacementDuringDelivery(false);
    modalReplacementDuringDelivery(true);
    physicalShieldAndScopeAdmission();
    std::cout << "managed dialog routing/lifetime passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
