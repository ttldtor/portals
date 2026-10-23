// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "managed_error_helpers.hpp"
#include <portals/detail/setup_failure.hpp>
#include <portals/managed.hpp>
#include <portals/ui_action.hpp>

#include <algorithm>
#include <exception>
#include <limits>

namespace portals {
namespace {
void increment(std::size_t &count) noexcept {
  if (count != std::numeric_limits<std::size_t>::max()) {
    ++count;
  }
}

void copyLabel(std::string &target, const std::string &source, std::size_t &remaining, bool &truncated) {
  const auto count = std::min(source.size(), remaining);

  target.assign(source, 0, count);
  remaining -= count;
  truncated = truncated || count != source.size();
}

void copyContexts(ManagedDiagnostic &diagnostic, std::span<const ManagedDiagnosticContext> contexts,
                  ManagedDiagnosticOptions options) {
  const auto count = std::min(contexts.size(), options.maxContexts);
  auto remaining = options.maxContextBytes;

  diagnostic.contextsTruncated = count != contexts.size();
  diagnostic.contexts.reserve(count);

  for (const auto &source : contexts.first(count)) {
    auto &target = diagnostic.contexts.emplace_back();

    target.origin = source.origin;
    copyLabel(target.operation, source.operation, remaining, diagnostic.contextsTruncated);
    copyLabel(target.name, source.name, remaining, diagnostic.contextsTruncated);
    copyLabel(target.scope, source.scope, remaining, diagnostic.contextsTruncated);
    copyLabel(target.expectedType, source.expectedType, remaining, diagnostic.contextsTruncated);
  }
}
} // namespace

std::optional<std::size_t> ManagedScene::recordError(ManagedError failure,
                                                     std::span<const ManagedDiagnosticContext> contexts,
                                                     std::source_location origin, bool *observerInvoked,
                                                     bool runFailure, const ui::detail::SetupFailureTicket *shortTicket,
                                                     bool contextUnavailable, bool dialogPrimary) noexcept {
  if (observerInvoked) {
    *observerInvoked = false;
  }

  if (contextUnavailable) {
    increment(contextFailures_);
  }

  if (dialogPrimary) {
    // Admission owns the first failure even when bounded history cannot retain its entry.
    dialogPrimaryRecorded_ = true;
    failureDiagnostic_.reset();
  }

  if (errorCount_ == errors_.size()) {
    increment(droppedErrors_);

    if (reporting_) {
      increment(suppressedNotifications_);

      return {};
    }

    // Reuse storage created with the scene; nested overflow must not overwrite this active borrow.
    overflowDiagnostic_.error = std::move(failure);
    overflowDiagnostic_.origin = origin;
    overflowDiagnostic_.storageOverflow = true;
    overflowDiagnostic_.contextsTruncated = contextUnavailable || !contexts.empty();
    overflowDiagnostic_.contextUnavailable = contextUnavailable;
    overflowDiagnostic_.notificationSuppressed = false;
    overflowDiagnostic_.observerFailed = false;
    const bool invoked = notifyError(overflowDiagnostic_, runFailure, shortTicket);

    if (observerInvoked) {
      *observerInvoked = invoked;
    }

    return {};
  }

  const auto index = errorCount_++;

  auto &diagnostic = errors_[index];

  // Entries and their empty context containers were constructed before mounting the scene.
  diagnostic.error = std::move(failure);
  diagnostic.origin = origin;
  diagnostic.contextUnavailable = contextUnavailable;

  if (dialogPrimary) {
    failureDiagnostic_ = index;
  }

  try {
    if (contextUnavailable) {
      diagnostic.contextsTruncated = true;
    } else {
      copyContexts(diagnostic, contexts, options_.diagnostics);
    }
  } catch (const std::bad_alloc &) {
    diagnostic.contexts.clear();
    diagnostic.contextUnavailable = true;
    diagnostic.contextsTruncated = !contexts.empty();
    increment(contextFailures_);
  }

  const bool invoked = notifyError(diagnostic, runFailure, shortTicket);

  if (observerInvoked) {
    *observerInvoked = invoked;
  }

  return index;
}

bool ManagedScene::notifyError(ManagedDiagnostic &diagnostic, bool runFailure,
                               const ui::detail::SetupFailureTicket *shortTicket) noexcept {
  ui::detail::CurrentSceneScope mask;
  if (!errorObserver_) {
    return false;
  }

  if (reporting_) {
    diagnostic.notificationSuppressed = true;
    increment(suppressedNotifications_);

    return false;
  }

  struct Reporting {
    bool &flag;
    bool &runFlag;
    bool previousRunFlag;

    ~Reporting() {
      flag = false;
      runFlag = previousRunFlag;
    }
  } guard{reporting_, notifyingRunFailure_, notifyingRunFailure_};

  auto callback = errorObserver_;

  reporting_ = true;
  notifyingRunFailure_ = runFailure;

  if (shortTicket) {
    shortTicket->notifying();
  }

  try {
    (*callback)(diagnostic);
  } catch (...) {
    diagnostic.observerFailed = true;
    increment(observerFailures_);
  }

  return true;
}

std::expected<void, TreeError> ManagedScene::reportError(ManagedError failure,
                                                         std::span<const ManagedDiagnosticContext> contexts,
                                                         std::source_location origin) noexcept {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (closed_) {
    return std::unexpected(TreeError::CLOSED);
  }

  (void)recordError(std::move(failure), contexts, origin);

  return {};
}

void ManagedScene::reportShortError(ManagedError &failure, const char *operation, std::string_view name,
                                    std::string_view expectedType, std::source_location origin,
                                    bool admittedRuntime) noexcept {
  failure.operation = operation;
  failure.operationOrigin = origin;

  if (owner_ != std::this_thread::get_id() || closed_) {
    const auto ticket = ui::detail::SetupFailureScope::latch(failure);

    return;
  }

  // Admission commits to collector entry. No allocation or user callback precedes this receipt and latch.
  const std::byte attempt{};
  failure.reportReceipt.collector_ = lifetime_;
  const bool primary = admittedRuntime && !failure_;

  if (primary) {
    failure_ = failure;
    app_.requestStop();
  }

  const auto ticket = ui::detail::SetupFailureScope::latch(failure, &attempt);

  if (primary) {
    // latch may stamp an entered collector's execution; retain the same owning envelope.
    failure_ = failure;
    bool invoked{};

    // Optional labels are omitted here: publishing the actual primary slot must allocate nothing.
    (void)recordError(failure, {}, origin, &invoked, true, &ticket, false, true);
    failureNotified_ = failureNotified_ || invoked;

    return;
  }

  try {
    const ManagedDiagnosticContext context{
      std::string(operation), std::string(name), {}, std::string(expectedType), origin};

    (void)recordError(failure, std::span(&context, 1), origin, nullptr, false, &ticket);
  } catch (const std::bad_alloc &) {
    (void)recordError(failure, {}, origin, nullptr, false, &ticket, true);
  } catch (const std::length_error &) {
    (void)recordError(failure, {}, origin, nullptr, false, &ticket, true);
  }
}

std::expected<void, ManagedError> ManagedScene::onError(ManagedErrorHandler callback) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  ManagedErrorHandler incoming;

  incoming.swap(callback);

  auto allowed = boundary(ManagedPhase::BIND, root_);

  if (!allowed) {
    return allowed;
  }

  try {
    auto replacement =
      incoming
        ? std::make_shared<ManagedErrorHandler>(ui::detail::currentSceneCallback(facadeContext(), std::move(incoming)))
        : nullptr;

    errorObserver_ = std::move(replacement);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
  }

  return {};
}

std::expected<ManagedDiagnosticBatch, TreeError> ManagedScene::diagnostics() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  try {
    ManagedDiagnosticBatch batch{std::vector<ManagedDiagnostic>(errors_.begin(), errors_.begin() + errorCount_),
                                 droppedErrors_,
                                 contextFailures_,
                                 observerFailures_,
                                 suppressedNotifications_,
                                 closed_};

    // Copy into expected directly: MSVC Debug's noexcept vector move may allocate a proxy.
    return std::expected<ManagedDiagnosticBatch, TreeError>(std::in_place, batch);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> ManagedScene::clearErrors() noexcept {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (reporting_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  for (auto &diagnostic : std::span(errors_.data(), errorCount_)) {
    diagnostic.contexts.clear();
    diagnostic.contextsTruncated = false;
    diagnostic.contextUnavailable = false;
    diagnostic.storageOverflow = false;
    diagnostic.notificationSuppressed = false;
    diagnostic.observerFailed = false;
  }

  errorCount_ = 0;
  droppedErrors_ = 0;
  contextFailures_ = 0;
  observerFailures_ = 0;
  suppressedNotifications_ = 0;
  failureDiagnostic_.reset();

  return {};
}

std::expected<ManagedDiagnosticBatch, TreeError> ManagedScene::drainErrors() {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (reporting_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  try {
    ManagedDiagnosticBatch batch{std::vector<ManagedDiagnostic>(errors_.begin(), errors_.begin() + errorCount_),
                                 droppedErrors_,
                                 contextFailures_,
                                 observerFailures_,
                                 suppressedNotifications_,
                                 closed_};

    struct ClearOnSuccess {
      ManagedScene &scene;
      int exceptions{std::uncaught_exceptions()};

      ~ClearOnSuccess() {
        if (std::uncaught_exceptions() == exceptions) {
          (void)scene.clearErrors();
        }
      }
    } guard{*this};

    return std::expected<ManagedDiagnosticBatch, TreeError>(std::in_place, batch);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

EditorCommandResult ManagedScene::admitActionResult(std::expected<void, ManagedError> result) {
  if (result) {
    return EditorCommandResult({}, {}, false);
  }

  const auto failure = result.error();
  const bool first = !failure_;
  if (first) {
    // First-primary disposition is committed before any observer or capture can reenter.
    failure_ = failure;
    dialogPrimaryRecorded_ = true;
    failureDiagnostic_.reset();
  }
  app_.requestStop();
  if (!focusReported(failure)) {
    bool invoked{};
    (void)recordError(
      failure, {},
      failure.operationOrigin.value_or(failure.registrationOrigin.value_or(std::source_location::current())), &invoked,
      first, nullptr, false, first);
    if (first) {
      failureNotified_ = failureNotified_ || invoked;
    }
  }
  return EditorCommandResult(std::unexpected(bridge(failure)), lifetime_, true);
}

void ManagedScene::remember(ManagedError failure, std::source_location origin,
                            std::span<const ManagedDiagnosticContext> contexts) {
  const bool first = !failure_;

  if (first) {
    failure_ = failure;
  }

  bool invoked{};
  const auto diagnostic = recordError(std::move(failure), contexts, origin, &invoked, first);

  if (first) {
    failureDiagnostic_ = diagnostic;
    failureNotified_ = invoked;
  }

  app_.requestStop();
}

} // namespace portals
