// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/key_help_state.hpp"
#include "support/managed_text_fixture.hpp"
#include <portals/key_hints.hpp>

namespace portals::detail {
struct ManagedScrollTestAccess {
  static std::uint64_t activation(ManagedScene &scene, Node<Widget> node) {
    return scene.helpActivation(node);
  }

  static std::shared_ptr<KeyHelpRecord> record(ManagedScene &scene, Node<Widget> node) {
    return scene.find(node)->keyHelp;
  }

  static std::expected<void, TreeError> prepare(ManagedScene &scene, const std::shared_ptr<KeyHelpRecord> &record,
                                                const LayoutSnapshot &layout) {
    return scene.prepareKeyHelp(record, layout);
  }
};
} // namespace portals::detail

namespace {
void prepareHelp(Fixture &f, CellSize size) {
  take(f.scene->prepare(*f.app, size));
  for (int pass{}; pass < 3; ++pass) {
    std::optional<LayoutSnapshot> layout;
    take(f.app->tree().frame([&] {
      const auto bounds = take(CellRect::from({}, size));
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
      layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
    }));
    (void)take(f.app->tree().consumeFrameRequest());
    take(f.scene->prepareViews(*f.app, *layout));
    if (take(f.app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      break;
    }
  }
  take(f.scene->completedLayout(*f.app));
}

void lifecycle() {
  Fixture f;
  ui::Ref<Button> invoker;
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> hints;
  take(ui::mount(*f.scene, ui::column(ui::button(invoker, "Document",
                                                 [] {
                                                 }),
                                      ui::keyHints(help).reference(hints), ui::helpView(help))));
  int saves{};
  take(f.scene->onKey(ui::press("Ctrl+S"), KeyInfo(TextValue("Save e\xcc\x81 wide \xe7\x95\x8c")), [&] {
    ++saves;
  }));
  take(f.scene->onKey(ui::press("F1"), KeyInfo(TextValue("Help")), [help] {
    return help.open();
  }));
  KeyInfo close(TextValue("Close help"));
  close.scope = help.node();
  take(f.scene->onKey(ui::press("Escape"), std::move(close), [help] {
    return help.close();
  }));
  take(f.scene->focus(invoker.node()));
  check(!take(f.app->tree().inspect(help.node())).visible, "help initially hidden with no implicit opening key");
  for (const auto size : {CellSize{}, CellSize{1, 1}, CellSize{20, 8}}) {
    prepareHelp(f, size);
    take(f.scene->input(*f.app, KeyEvent{InputKey::F1, 0, {}, KeyAction::PRESS}));
    check(take(f.app->tree().inspect(help.node())).visible, "application help key works independently of strip width");
    take(help.open());
    const auto focus = take(f.app->router().focused());
    take(help.open());
    check(take(f.app->router().focused()) == focus, "duplicate open never steals focus or pushes a return record");
    prepareHelp(f, size);
    take(help.close());
    check(!take(f.app->tree().inspect(help.node())).visible && take(f.app->router().focused()) == invoker.node(),
          "ordinary close returns exact surviving invoker");
  }
  take(help.open());
  const auto first = portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node());
  take(f.app->tree().setVisible(help.node(), false));
  (void)take(f.app->router().focused());
  take(f.app->tree().setVisible(help.node(), true));
  take(f.app->router().enterFocusScope(help.node()));
  const auto other = portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node());
  check(first && other > first, "lazy unwind and re-enter publish a distinct activation receipt");
  take(help.close());
  check(take(f.app->tree().inspect(help.node())).visible &&
          portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()) == other,
        "old help close never consumes or hides a new same-root activation (ABA)");
  take(f.app->router().leaveFocusScope(help.node()));
  take(help.open());
  take(help.close());
  check(saves == 0, "help rows are projections and never execute commands");
}

void exhaustion() {
  Fixture f;
  ui::Ref<Button> invoker;
  ui::Ref<Column> ordinary;
  ui::Ref<HelpView> help;
  take(ui::mount(*f.scene, ui::column(ui::column(ui::button(invoker, "Ordinary",
                                                            [] {
                                                            }))
                                        .reference(ordinary)
                                        .focusScope(),
                                      ui::helpView(help))));
  take(f.scene->focus(invoker.node()));
  portals::detail::EventRouterAccess::exhaustFocusReturnSerial(f.app->router());
  auto failed = help.open();
  check(!failed && treeCause(failed.error(), TreeError::IDENTITY_EXHAUSTED) &&
          !take(f.app->tree().inspect(help.node())).visible,
        "HelpView rejects unavailable receipt before visibility/focus publication");
  take(f.app->router().enterFocusScope(ordinary.node()));
  take(f.app->router().leaveFocusScope(ordinary.node()));
  check(take(f.app->router().focused()) == invoker.node(),
        "receipt exhaustion leaves legacy low-level focus behavior intact");
}

void overflowAndRetry() {
  Fixture f;
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> hints;
  ui::Ref<Button> stable;
  take(ui::mount(*f.scene, ui::column(ui::button(stable, "Stable",
                                                 [] {
                                                 }),
                                      ui::keyHints(help).reference(hints), ui::helpView(help))));
  for (const auto key : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
    KeyInfo info(TextValue("Combining e\xcc\x81 and wide \xe7\x95\x8c"));
    info.description = TextValue("First line\nSecond line\nThird line");
    take(f.scene->onKey(ui::press(key), std::move(info), [] {
    }));
  }
  prepareHelp(f, {1, 6});
  auto compact = portals::detail::ManagedScrollTestAccess::record(*f.scene, hints.node());
  auto full = portals::detail::ManagedScrollTestAccess::record(*f.scene, help.node());
  check(compact->presentation->current && compact->presentation->text->caption->bytes() == "+" &&
          compact->presentation->text->overflow == 8 && take(f.app->tree().inspect(hints.node())).enabled,
        "one-cell overflow reserves a whole visible marker with exact omitted row count");
  check(!full->presentation->projection && !full->presentation->text,
        "hidden full help performs no projection or formatting");
  take(f.scene->focus(hints.node()));
  take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  check(take(f.app->tree().inspect(help.node())).visible, "focused overflow Enter opens the checked help target");
  prepareHelp(f, {20, 6});
  check(take(f.app->router().dispatch(KeyEvent{InputKey::PAGE_DOWN})).status == DispatchStatus::HANDLED,
        "focusable full body bubbles page navigation into its existing ScrollArea");
  take(help.close());
  check(take(f.app->router().focused()) == hints.node(), "overflow remains the exact focus-return invoker");
  auto previous = compact->presentation->projection;
  auto previousText = compact->presentation->text;
  take(f.scene->focus(stable.node()));
  take(f.scene->setKeyCapabilities({KeySupport::SUPPORTED, KeySupport::UNKNOWN, KeySupport::UNKNOWN}));
  take(f.app->tree().setEnabled(hints.node(), false));
  int notifications{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  // Fail the new owning projection in view preparation, outside the allocating layout fixture.
  std::optional<LayoutSnapshot> layout;
  take(f.app->tree().frame([&] {
    const auto bounds = take(CellRect::from({}, {20, 6}));
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
  }));
  allocationTest::remaining = 0;
  auto failed = portals::detail::ManagedScrollTestAccess::prepare(*f.scene, compact, *layout);
  allocationTest::remaining = -1;
  take(failed);
  check(!compact->presentation->current && compact->presentation->projection == previous &&
          compact->presentation->text == previousText && notifications == 1,
        "failed fresh projection keeps the coherent last checked pair behind one unavailable marker");
  prepareHelp(f, {20, 6});
  const auto notified = notifications;
  prepareHelp(f, {20, 6});
  check(!compact->presentation->current && notifications == notified,
        "same failed input does not repeat a diagnostic or idle rebuild");
  (void)take(f.scene->keyBindings());
  prepareHelp(f, {20, 6});
  check(compact->presentation->current, "explicit checked query permits retry of the failed view revision");
}

Fixture *receiptFixture{};
Node<Widget> receiptHelp;
bool exhaustedDuringStage{};
bool receiptAlreadyIssued{};

void exhaustDuringStage() {
  if (portals::detail::EventRouterAccess::requiredFocusReceipt(receiptFixture->app->router()) != Node<Widget>{}) {
    exhaustedDuringStage = true;
    receiptAlreadyIssued =
      portals::detail::ManagedScrollTestAccess::activation(*receiptFixture->scene, receiptHelp) != 0;
    portals::detail::EventRouterAccess::exhaustFocusReturnSerial(receiptFixture->app->router());
  }
}

void retainedProjectionBudget() {
  const auto oneRow = sizeof(KeyBindingView) + std::string_view("Save").size() + sizeof(key::Chord) + sizeof(KeyAction);
  ManagedOptions options;
  options.maxKeyProjectionBytes = 2 * oneRow + 128;
  Fixture f(options);
  ui::Ref<HelpView> help;
  ui::Ref<KeyHints> first, second;
  take(ui::mount(*f.scene, ui::column(ui::keyHints(help).reference(first), ui::keyHints(help).reference(second),
                                      ui::helpView(help))));
  auto binding = take(f.scene->onKey(ui::press("Ctrl+S"), KeyInfo(TextValue("Save")), [] {
  }));
  prepareHelp(f, {80, 6});
  auto a = portals::detail::ManagedScrollTestAccess::record(*f.scene, first.node());
  auto b = portals::detail::ManagedScrollTestAccess::record(*f.scene, second.node());
  if (!a->presentation->current || !b->presentation->current ||
      a->presentation->projection != b->presentation->projection) {
    std::cerr << "resident initial: current=" << a->presentation->current << ',' << b->presentation->current
              << " bytes=" << a->projectionBytes << ',' << b->projectionBytes << " text=" << a->textBytes << ','
              << b->textBytes << " same=" << (a->presentation->projection == b->presentation->projection)
              << " budget=" << options.maxKeyProjectionBytes << '\n';
  }
  check(a->presentation->current && b->presentation->current &&
          a->presentation->projection == b->presentation->projection,
        "two projections sharing one owning DTO charge that DTO only once");
  take(binding.setText(TextValue("Save v2"), TextValue("")));
  (void)take(f.scene->keyBindings());
  std::optional<LayoutSnapshot> layout;
  take(f.app->tree().frame([&] {
    const auto bounds = take(CellRect::from({}, {80, 6}));
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root(), LayoutSnapshotProfile::GEOMETRY));
  }));
  take(portals::detail::ManagedScrollTestAccess::prepare(*f.scene, a, *layout));
  check(a->presentation->projection != b->presentation->projection,
        "one updated view retains the other last checked DTO");
  const auto previousA = a->presentation->projection;
  const auto previousB = b->presentation->projection;
  take(binding.setText(TextValue("Save v3"), TextValue("")));
  auto third = f.scene->keyBindings();
  check(!third && treeCause(third.error(), TreeError::NODE_LIMIT) && a->presentation->projection == previousA &&
          b->presentation->projection == previousB,
        "aggregate resident budget includes every distinct historical DTO retained by existing views");
}

void receiptGuardAndFocusErrors() {
  bool allocationSuccess{};
  bool allocationFailure{};
  for (std::ptrdiff_t point{}; point < 256; ++point) {
    Fixture f;
    ui::Ref<HelpView> help;
    take(ui::mount(*f.scene, ui::helpView(help)));
    allocationTest::remaining = point;
    auto opened = help.open();
    allocationTest::remaining = -1;
    if (opened) {
      allocationSuccess = true;
      break;
    }
    allocationFailure = true;
    check(treeCause(opened.error(), TreeError::NO_MEMORY) && !take(f.app->tree().inspect(help.node())).visible &&
            !portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()) &&
            portals::detail::EventRouterAccess::requiredFocusReceipt(f.app->router()) == Node<Widget>{},
          "prepublication allocation failure hides help and restores the receipt guard without a return record");
  }
  check(allocationSuccess && allocationFailure,
        "finite HelpView publication allocation sweep reaches failure and success");
  bool reachedGuard{};
  bool reachedPublished{};
  bool completed{};
  for (std::ptrdiff_t point{}; point < 256; ++point) {
    Fixture f;
    ui::Ref<HelpView> help;
    take(ui::mount(*f.scene, ui::helpView(help)));
    receiptFixture = &f;
    receiptHelp = help.node();
    exhaustedDuringStage = false;
    receiptAlreadyIssued = false;
    allocationTest::hookAfter = point;
    allocationTest::hook = exhaustDuringStage;
    auto opened = help.open();
    const bool hookRemaining = allocationTest::hook != nullptr;
    allocationTest::hook = nullptr;
    check(portals::detail::EventRouterAccess::requiredFocusReceipt(f.app->router()) == Node<Widget>{},
          "checked open restores its dynamic receipt requirement on every exit");
    if (exhaustedDuringStage) {
      if (receiptAlreadyIssued) {
        reachedPublished = true;
        check(opened.has_value() && take(f.app->tree().inspect(help.node())).visible &&
                portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
              "later exhaustion never revokes an already issued exact activation receipt");
      } else {
        reachedGuard = true;
        check(!opened && treeCause(opened.error(), TreeError::IDENTITY_EXHAUSTED) &&
                !take(f.app->tree().inspect(help.node())).visible &&
                !portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
              "exhaustion after staging still rejects before exact return publication");
      }
    }
    if (hookRemaining) {
      completed = true;
      break;
    }
  }
  check(completed && reachedGuard && reachedPublished,
        "finite exhaustion sweep reaches both sides of exact receipt publication and completion");

  Fixture f;
  ui::Ref<HelpView> help;
  take(ui::mount(*f.scene, ui::helpView(help)));
  auto invoker = f.node();
  bool failLoss{}, failGain{}, blocked{};
  auto binding = take(f.app->router().bind(
    invoker,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool gain) -> std::expected<void, TreeError> {
      if (!gain && failLoss) {
        auto nested = help.close();
        blocked = !nested && treeCause(nested.error(), TreeError::WOULD_BLOCK);
        return std::unexpected(TreeError::NO_MEMORY);
      }
      return gain && failGain ? std::expected<void, TreeError>(std::unexpected(TreeError::CALLBACK_FAILED))
                              : std::expected<void, TreeError>{};
    }));
  take(f.scene->focus(invoker));
  failLoss = true;
  auto opened = help.open();
  check(!opened && treeCause(opened.error(), TreeError::NO_MEMORY) && blocked &&
          take(f.app->tree().inspect(help.node())).visible &&
          portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
        "published focus callback NO_MEMORY retains open help and rejects lifecycle reentry");
  failLoss = false;
  failGain = true;
  auto closed = help.close();
  check(!closed && treeCause(closed.error(), TreeError::CALLBACK_FAILED) &&
          !take(f.app->tree().inspect(help.node())).visible && take(f.app->router().focused()) == invoker &&
          !portals::detail::ManagedScrollTestAccess::activation(*f.scene, help.node()),
        "published return callback failure still completes close with its original error");
}
} // namespace
int main() try {
  lifecycle();
  exhaustion();
  overflowAndRetry();
  retainedProjectionBudget();
  receiptGuardAndFocusErrors();
  std::cout << "key-help: tiny/Unicode/open/close/return/activation-ABA/exhaustion PASS\n";
} catch (const std::exception &error) {
  allocationTest::remaining = -1;
  allocationTest::hook = nullptr;
  std::cerr << error.what() << '\n';
  return 1;
}
