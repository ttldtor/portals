// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/message_box_state.hpp"
#include "support/message_fixture.hpp"

namespace {
void mountSweep() {
  for (std::size_t point = 0; point < 4096; ++point) {
    Fixture f;
    ui::MessageBox box;
    ui::Ref<OverlayHost> host;
    auto recipe = ui::overlayHost(host, ui::label("Body"), ui::unsavedChangesDialog(box, "Unsaved", "Body"));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto mounted = ui::mount(*f.scene, std::move(recipe));
    allocationTest::remaining = -1;
    if (mounted) {
      std::cout << "message mount OOM points=" << point << '\n';
      return;
    }
    check(treeCause(mounted.error(), TreeError::NO_MEMORY), "mount exact NO_MEMORY");
    check(host.node() == Node<OverlayHost>{} && treeCause(box.lastResult().error(), TreeError::CLOSED),
          "all observations stay cold");
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "mount rollback no partial subtree");
    take(ui::mount(*f.scene, ui::overlayHost(ui::label("Retry"), ui::messageBox(box, "Retry", "Body"))));
  }
  check(false, "mount OOM sweep finite bound");
}

void openSweep() {
  for (std::size_t point = 0; point < 2048; ++point) {
    Fixture f;
    ui::MessageBox box;
    ui::Ref<Dialog> dialog;
    take(
      ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::messageBox(box, "Info", "Body").reference(dialog))));
    command(box.open([] {
    }));
    command(box.cancel());
    flushMessages(f);
    auto previous = answer(box, MessageChoice::CANCEL);
    allocationTest::remaining = 0;
    auto retained = box.lastResult();
    allocationTest::remaining = -1;
    check(retained && *retained == previous, "latched snapshot query itself allocates no Debug borrow");
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto opened = box.open([] {
    });
    allocationTest::remaining = -1;
    if (opened) {
      std::cout << "message open OOM points=" << point << '\n';
      return;
    }
    const auto failure = opened.error();
    const auto *application = std::get_if<ApplicationError>(&failure.cause);
    const auto *scheduler = application ? std::get_if<SchedulerError>(&application->cause) : nullptr;
    const bool noMemory =
      treeCause(failure, TreeError::NO_MEMORY) || (scheduler && *scheduler == SchedulerError::NO_MEMORY);
    if (!noMemory) {
      auto cause = std::get_if<TreeError>(&failure.cause);
      std::cerr << "open point=" << point << " phase=" << int(failure.phase) << " cause=" << (cause ? int(*cause) : -1)
                << '\n';
    }
    check(noMemory, "open exact typed allocation cause");
    if (!take(dialog.opened())) {
      check(take(box.lastResult()) == previous, "prepublication failure preserves last result");
    }
  }
  check(false, "open OOM sweep finite bound");
}

void fullErrorHolder() {
  Fixture f;
  ui::Ref<Label> label;
  take(ui::mount(*f.scene, ui::label("Original").reference(label)));
  auto invalid = label.text().set("\xff").result();
  check(!invalid, "real checked facade error has a report receipt");
  auto original = invalid.error();
  original.application =
    ApplicationError{ApplicationPhase::FLUSH, TerminalError{TerminalErrorKind::OUTPUT_PENDING, 17, 19},
                     TerminalError{TerminalErrorKind::OUTPUT_LIMIT, 23, 29}};
  original.cleanup = TreeError::NODE_LIMIT;
  original.resource = TextResourceError::MISSING_RESOURCE;
  original.pss = PssError{.cause = TreeError::NO_MEMORY,
                          .code = PssErrorCode::UNKNOWN_PROPERTY,
                          .location = {31, 7, 11},
                          .subject = {'f', 'o', 'r', 'e', 'g', 'r', 'o', 'u', 'n', 'd'},
                          .subjectSize = 10,
                          .subjectTruncated = true,
                          .message = "Static original parser cause"};
  original.file = PssFileError{PssFileOperation::READ, std::make_error_code(std::errc::io_error)};
  original.styleSource =
    std::make_shared<const std::string>("a-complete-owned-source-label-longer-than-inline-string-storage");
  original.shortcut = ShortcutError{ShortcutErrorKind::UNKNOWN_KEY, 37};
  original.registrationOrigin = std::source_location::current();
  original.operation = "Original.operation";
  original.operationOrigin = std::source_location::current();
  original.execution = "Original.execution";
  original.setupStopped = true;
  original.sceneResource = std::make_shared<const SceneResourceError>(
    SceneResourceError{SceneResourceErrorCode::UNKNOWN_TYPE, original.styleSource, 41, 13, 17,
                       std::make_error_code(std::errc::invalid_argument)});
  const auto diagnosticCount = take(f.scene->diagnostics()).errors.size();
  std::array<std::shared_ptr<const MessageResult>, 3> snapshots;
  for (std::size_t alternative = 0; alternative < snapshots.size(); ++alternative) {
    if (alternative == 0) {
      original.cause = TreeError::NO_MEMORY;
    }
    if (alternative == 1) {
      original.cause = StyleError{TreeError::NO_MEMORY, 43, 47, "Static original style cause"};
    }
    if (alternative == 2) {
      original.cause = *original.application;
    }
    portals::detail::MessageRequest request;
    request.holder = std::make_shared<portals::detail::MessageResultHolder>();
    const auto allocations = allocationTest::count;
    allocationTest::remaining = 0;
    ManagedError copied(original);
    ManagedError moved(std::move(copied));
    copied = original;
    moved = std::move(copied);
    request.latch(std::unexpected(original));
    snapshots[alternative] = request.result;
    const bool noAllocation = allocationTest::count == allocations;
    allocationTest::remaining = -1;
    check(noAllocation && !request.holder && request.result,
          "one holder latch/const alias consumes every writable holder without allocation");
    const auto &error = request.result->error();
    check(error.cause.index() == alternative && error.phase == original.phase && error.node == original.node &&
            error.text == original.text && error.resource == original.resource && error.cleanup == original.cleanup &&
            error.file->operation == original.file->operation && error.file->cause == original.file->cause &&
            error.pss->code == original.pss->code && error.pss->location == original.pss->location &&
            error.pss->subjectView() == original.pss->subjectView() &&
            error.pss->subjectTruncated == original.pss->subjectTruncated &&
            error.styleSource == original.styleSource && error.sceneResource == original.sceneResource &&
            error.shortcut == original.shortcut &&
            error.registrationOrigin->line() == original.registrationOrigin->line() &&
            error.operationOrigin->line() == original.operationOrigin->line() &&
            error.operation == original.operation && error.execution == original.execution && error.setupStopped,
          "all full error categories survive exact immutable publication");
    check(error.application->phase == original.application->phase &&
            std::get<TerminalError>(error.application->cause).native_code == 17 &&
            error.application->restoration->native_code == 23,
          "independent application/terminal restoration remains full");
    if (alternative == 0) {
      check(std::get<TreeError>(error.cause) == TreeError::NO_MEMORY, "exact tree cause");
    }
    if (alternative == 1) {
      check(std::get<StyleError>(error.cause).line == 43 && std::get<StyleError>(error.cause).column == 47,
            "exact style cause");
    }
    if (alternative == 2) {
      check(std::get<ApplicationError>(error.cause).restoration->restoration_code == 29, "exact application cause");
    }
    ui::detail::reportFocusError(ui::detail::MessageAccess::target(label), error, std::source_location::current());
    check(take(f.scene->diagnostics()).errors.size() == diagnosticCount,
          "actual copied report receipt prevents duplicate report");
  }
  take(f.scene->remove(label.node()));
  for (const auto &snapshot : snapshots) {
    check(snapshot && !*snapshot && snapshot->error().styleSource == original.styleSource,
          "full snapshot survives removal independently of original UI");
  }
  std::cout << "full-error holder: all three cause alternatives, genuine report receipt, zero-budget latch PASS\n";
}

void receiveValidationAllocation() {
  Fixture f;
  ui::MessageBox box;
  ui::Ref<Dialog> dialog;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::messageBox(box, "Info", "Body").reference(dialog))));
  auto async = take(UiAsync::create(f.app->scheduler(), f.app->tree(), f.scene->root()));
  allocationTest::remaining = 0;
  auto waiting = box.receive(async->context());
  allocationTest::remaining = -1;
  check(!waiting && treeCause(waiting.error(), TreeError::NO_MEMORY) && !take(dialog.opened()),
        "initial context-validation allocation failure remains NO_MEMORY and adopts no request");
  command(box.open([] {
  }));
}
} // namespace
int main() try {
  mountSweep();
  openSweep();
  fullErrorHolder();
  receiveValidationAllocation();
  std::cout << "message-box-oom: complete mount/adoption sweep PASS\n";
} catch (const std::exception &error) {
  allocationTest::remaining = -1;
  std::cerr << error.what() << '\n';
  return 1;
}
