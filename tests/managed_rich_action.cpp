// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"
#include <portals/key_binding.hpp>

static_assert(portals::detail::ManagedAction<decltype([]() -> std::expected<void, portals::ManagedError> {
                                               return {};
                                             }),
                                             portals::KeyEvent>);
static_assert(portals::detail::ManagedAction<decltype([]() -> std::expected<int, portals::TreeError> {
                                               return 7;
                                             }),
                                             portals::KeyEvent>);
static_assert(!portals::detail::ManagedAction<decltype([]() -> std::expected<int, portals::ManagedError> {
                                                return 7;
                                              }),
                                              portals::KeyEvent>);
static_assert(!portals::detail::ManagedAction<decltype([]() -> std::expected<void, portals::TextResourceError> {
                                                return {};
                                              }),
                                              portals::KeyEvent>);
static_assert(std::is_nothrow_copy_constructible_v<portals::ManagedError>);
static_assert(std::is_nothrow_copy_constructible_v<std::expected<void, portals::ManagedError>>);

namespace {
ManagedError richFailure(NodeId node = {}) {
  ManagedError error{ManagedPhase::TEXT, node, TreeError::STALE_NODE, {}, {}};
  error.resource = TextResourceError::MISSING_RESOURCE;
  error.shortcut = ShortcutError{ShortcutErrorKind::UNKNOWN_KEY, 17};
  error.operation = "original.checked";
  error.operationOrigin = std::source_location::current();
  error.registrationOrigin = std::source_location::current();
  error.styleSource = std::make_shared<const std::string>("owning-source-label-beyond-short-string-storage");
  return error;
}

void fresh(bool clear, bool overflow, bool observer, int cause) {
  ManagedOptions options;
  options.diagnostics.maxErrors = 1;
  Fixture f(options);
  auto original = richFailure(f.scene->root().id());
  if (cause == 1) {
    original.cause = StyleError{};
  }
  if (cause == 2) {
    original.cause = ApplicationError{};
  }
  int notifications{};
  if (overflow) {
    take(f.scene->reportError(richFailure()));
  }
  if (observer) {
    take(f.scene->onError([&](const ManagedDiagnostic &diagnostic) {
      ++notifications;
      check(diagnostic.error.phase == original.phase && diagnostic.error.cause.index() == original.cause.index() &&
              diagnostic.error.styleSource == original.styleSource &&
              diagnostic.error.operationOrigin->line() == original.operationOrigin->line(),
            "rich observer receives exact owning error before reentry");
      if (clear) {
        const auto cleared = f.scene->clearErrors();
        check(!cleared && cleared.error() == TreeError::WOULD_BLOCK,
              "existing observer mutation guard rejects reentrant clear without altering admission");
      }
    }));
  }
  std::size_t start{};
  take(f.scene->onKey(ui::press("A"), KeyInfo(TextValue("Fail")), [&]() -> std::expected<void, ManagedError> {
    start = allocationTest::attempts;
    allocationTest::remaining = 0;
    return std::unexpected(original);
  }));
  auto invoked = f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, U'a', {}, KeyAction::PRESS});
  const auto attempts = allocationTest::attempts - start;
  allocationTest::remaining = -1;
  if (!invoked || !*invoked || attempts) {
    std::cerr << "rich admission: clear=" << clear << " overflow=" << overflow << " observer=" << observer
              << " cause=" << cause << " start=" << start << " attempts=" << attempts << " invoked=" << bool(invoked)
              << " consumed=" << (invoked && *invoked) << '\n';
  }
  check(invoked && *invoked && attempts == 0,
        "rich failure admission/primary/receipt/bridge allocates nothing and consumes entered input");
  if (clear) {
    take(f.scene->clearErrors());
  }
  auto run = f.scene->run(f.terminal);
  check(!run && run.error().phase == original.phase && run.error().node == original.node &&
          run.error().cause.index() == original.cause.index() && run.error().resource == original.resource &&
          run.error().shortcut == original.shortcut && run.error().operation == original.operation &&
          run.error().operationOrigin->line() == original.operationOrigin->line() &&
          run.error().styleSource == original.styleSource,
        "final primary preserves full envelope without generic TreeError replacement");
  check(notifications == (observer ? 1 : 0), "clear/full history/absent observer never replays admitted rich primary");
}

void reported(bool foreign, bool expired) {
  Fixture receiving;
  std::optional<ManagedError> error;
  std::unique_ptr<Fixture> source;
  auto *owner = &receiving;
  if (foreign) {
    source = std::make_unique<Fixture>();
    owner = source.get();
  }
  ui::Ref<Label> label;
  take(ui::mount(*owner->scene, ui::label(label, "Before")));
  auto invalid = label.text().set("\xff").result();
  check(!invalid, "genuine checked failure carries original collector receipt");
  error = invalid.error();
  if (expired) {
    source.reset();
  }
  int notifications{};
  take(receiving.scene->onError([&](const ManagedDiagnostic &) {
    ++notifications;
  }));
  take(receiving.scene->onKey(ui::press("A"), KeyInfo(TextValue("Reported failure")),
                              [error]() -> std::expected<void, ManagedError> {
                                return std::unexpected(*error);
                              }));
  take(receiving.scene->input(*receiving.app, KeyEvent{InputKey::CHARACTER, U'a', {}, KeyAction::PRESS}));
  auto run = receiving.scene->run(receiving.terminal);
  check(!run && run.error().text == error->text && run.error().operation == error->operation &&
          run.error().operationOrigin->line() == error->operationOrigin->line(),
        "own/foreign/expired original receipt retains typed primary");
  check(notifications == 0, "reported attempts never replay a historical observer notification");
}

void nativeFacade() {
  example::ReplayTerminal terminal({{10, "a", {}}}, 100, {20, 4});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  const auto original = richFailure();
  auto scene = ui::scene(ui::label("Native rich action"), options);
  scene.onKey("A", ui::keyInfo("Fail"), [original]() -> std::expected<void, ManagedError> {
    return std::unexpected(original);
  });
  int notifications{}, reports{};
  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notifications;
    check(diagnostic.error.operation == original.operation, "native observer retains original operation");
  });
  scene.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    check(terminal.closed && report.failure && report.failure->shortcut == original.shortcut,
          "native final report follows cleanup with exact primary");
  });
  check(scene.runApp(terminal) == 1 && notifications == 1 && reports == 1,
        "ordinary facade rich failure is notified once and stops via existing runner");
}

void earlierPrimaryAndSelfRemoval() {
  Fixture f;
  const auto original = richFailure();
  take(f.scene->onKey(ui::press("A"), []() -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }));
  std::optional<KeyBinding> binding;
  binding = take(
    f.scene->onKey(ui::press("B"), KeyInfo(TextValue("Remove and fail")), [&]() -> std::expected<void, ManagedError> {
      auto removed = binding->remove();
      if (!removed) {
        return removed;
      }
      return std::unexpected(original);
    }));
  auto first = f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, U'a', {}, KeyAction::PRESS});
  check(!first && first.error() == TreeError::INVALID_GEOMETRY, "old callback establishes earlier technical primary");
  take(f.scene->input(*f.app, KeyEvent{InputKey::CHARACTER, U'b', {}, KeyAction::PRESS}));
  auto run = f.scene->run(f.terminal);
  check(!run && treeCause(run.error(), TreeError::INVALID_GEOMETRY),
        "later self-removing rich callback cannot replace an unrelated earlier primary");
}
} // namespace
int main() try {
  for (int cause{}; cause < 3; ++cause) {
    fresh(false, false, true, cause);
  }
  fresh(true, false, true, 0);
  fresh(false, true, true, 0);
  fresh(false, false, false, 0);
  reported(false, false);
  reported(true, false);
  reported(true, true);
  nativeFacade();
  earlierPrimaryAndSelfRemoval();
  std::cout << "rich-action: exact-concepts/noalloc/full-primary/receipts/native PASS\n";
} catch (const std::exception &error) {
  allocationTest::remaining = -1;
  std::cerr << error.what() << '\n';
  return 1;
}
