// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/async.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>
#include <cstdio>
#include <type_traits>

namespace portals::ui {

namespace {
constexpr const char *phaseName(ManagedPhase phase) noexcept {
  switch (phase) {
  case ManagedPhase::MOUNT:
    return "MOUNT";
  case ManagedPhase::STATE:
    return "STATE";
  case ManagedPhase::BIND:
    return "BIND";
  case ManagedPhase::STYLE:
    return "STYLE";
  case ManagedPhase::PREPARE:
    return "PREPARE";
  case ManagedPhase::INPUT:
    return "INPUT";
  case ManagedPhase::RUN:
    return "RUN";
  case ManagedPhase::CLEANUP:
    return "CLEANUP";
  case ManagedPhase::TEXT:
    return "TEXT";
  case ManagedPhase::LAYOUT:
    return "LAYOUT";
  }

  return "UNKNOWN";
}

constexpr const char *treeErrorName(TreeError cause) noexcept {
  switch (cause) {
  case TreeError::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case TreeError::WRONG_THREAD:
    return "WRONG_THREAD";
  case TreeError::WRONG_SESSION:
    return "WRONG_SESSION";
  case TreeError::STALE_NODE:
    return "STALE_NODE";
  case TreeError::CLOSED:
    return "CLOSED";
  case TreeError::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case TreeError::NO_MEMORY:
    return "NO_MEMORY";
  case TreeError::IDENTITY_EXHAUSTED:
    return "IDENTITY_EXHAUSTED";
  case TreeError::NODE_LIMIT:
    return "NODE_LIMIT";
  case TreeError::DEPTH_LIMIT:
    return "DEPTH_LIMIT";
  case TreeError::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  case TreeError::INVALID_GEOMETRY:
    return "INVALID_GEOMETRY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(ApplicationPhase value) noexcept {
  switch (value) {
  case ApplicationPhase::CREATE:
    return "CREATE";
  case ApplicationPhase::OPEN:
    return "OPEN";
  case ApplicationPhase::INPUT:
    return "INPUT";
  case ApplicationPhase::DISPATCH:
    return "DISPATCH";
  case ApplicationPhase::PREPARE:
    return "PREPARE";
  case ApplicationPhase::RENDER:
    return "RENDER";
  case ApplicationPhase::SUBMIT:
    return "SUBMIT";
  case ApplicationPhase::FLUSH:
    return "FLUSH";
  case ApplicationPhase::STOP:
    return "STOP";
  case ApplicationPhase::CLOSE:
    return "CLOSE";
  case ApplicationPhase::CONTROL_LATENCY:
    return "CONTROL_LATENCY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(ApplicationFailure value) noexcept {
  switch (value) {
  case ApplicationFailure::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case ApplicationFailure::ALREADY_RUN:
    return "ALREADY_RUN";
  case ApplicationFailure::NOT_STARTED:
    return "NOT_STARTED";
  case ApplicationFailure::REENTRANT_OPERATION:
    return "REENTRANT_OPERATION";
  case ApplicationFailure::NO_MEMORY:
    return "NO_MEMORY";
  case ApplicationFailure::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  case ApplicationFailure::OBSERVER_FAILED:
    return "OBSERVER_FAILED";
  case ApplicationFailure::BACKGROUND_FAILED:
    return "BACKGROUND_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(AsyncError value) noexcept {
  switch (value) {
  case AsyncError::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case AsyncError::WRONG_THREAD:
    return "WRONG_THREAD";
  case AsyncError::CLOSED:
    return "CLOSED";
  case AsyncError::CANCELLED:
    return "CANCELLED";
  case AsyncError::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case AsyncError::NO_MEMORY:
    return "NO_MEMORY";
  case AsyncError::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  case AsyncError::IDENTITY_EXHAUSTED:
    return "IDENTITY_EXHAUSTED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(BackgroundError value) noexcept {
  switch (value) {
  case BackgroundError::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case BackgroundError::UNAVAILABLE:
    return "UNAVAILABLE";
  case BackgroundError::CLOSED:
    return "CLOSED";
  case BackgroundError::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case BackgroundError::NO_MEMORY:
    return "NO_MEMORY";
  case BackgroundError::START_FAILED:
    return "START_FAILED";
  case BackgroundError::WRONG_THREAD:
    return "WRONG_THREAD";
  case BackgroundError::LIMIT_EXCEEDED:
    return "LIMIT_EXCEEDED";
  case BackgroundError::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(TerminalErrorKind value) noexcept {
  switch (value) {
  case TerminalErrorKind::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case TerminalErrorKind::UNSUPPORTED:
    return "UNSUPPORTED";
  case TerminalErrorKind::SYSTEM:
    return "SYSTEM";
  case TerminalErrorKind::CLOSED:
    return "CLOSED";
  case TerminalErrorKind::OUTPUT_PENDING:
    return "OUTPUT_PENDING";
  case TerminalErrorKind::OUTPUT_LIMIT:
    return "OUTPUT_LIMIT";
  case TerminalErrorKind::INPUT_DECODE:
    return "INPUT_DECODE";
  case TerminalErrorKind::OUTPUT_UNCERTAIN:
    return "OUTPUT_UNCERTAIN";
  case TerminalErrorKind::INPUT_LIMIT:
    return "INPUT_LIMIT";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(SchedulerError value) noexcept {
  switch (value) {
  case SchedulerError::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case SchedulerError::WRONG_THREAD:
    return "WRONG_THREAD";
  case SchedulerError::WRONG_SCHEDULER:
    return "WRONG_SCHEDULER";
  case SchedulerError::CLOSED:
    return "CLOSED";
  case SchedulerError::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case SchedulerError::NO_MEMORY:
    return "NO_MEMORY";
  case SchedulerError::REENTRANT_DISPATCH:
    return "REENTRANT_DISPATCH";
  case SchedulerError::IDENTITY_EXHAUSTED:
    return "IDENTITY_EXHAUSTED";
  case SchedulerError::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(InputError value) noexcept {
  switch (value) {
  case InputError::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case InputError::BACKWARD_TIME:
    return "BACKWARD_TIME";
  case InputError::CLOSED:
    return "CLOSED";
  case InputError::FAILED:
    return "FAILED";
  case InputError::OFFSET_OVERFLOW:
    return "OFFSET_OVERFLOW";
  case InputError::INVALID_WINDOW:
    return "INVALID_WINDOW";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(RenderErrorKind value) noexcept {
  switch (value) {
  case RenderErrorKind::COMMAND_LIMIT:
    return "COMMAND_LIMIT";
  case RenderErrorKind::TEXT_BYTE_LIMIT:
    return "TEXT_BYTE_LIMIT";
  case RenderErrorKind::WORK_LIMIT:
    return "WORK_LIMIT";
  case RenderErrorKind::POLICY_MISMATCH:
    return "POLICY_MISMATCH";
  case RenderErrorKind::BUFFER_ERROR:
    return "BUFFER_ERROR";
  case RenderErrorKind::ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(FrameError value) noexcept {
  switch (value) {
  case FrameError::PENDING:
    return "PENDING";
  case FrameError::POISONED:
    return "POISONED";
  case FrameError::FRAME_LIMIT:
    return "FRAME_LIMIT";
  case FrameError::OUTPUT_LIMIT:
    return "OUTPUT_LIMIT";
  case FrameError::ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(CellBufferError value) noexcept {
  switch (value) {
  case CellBufferError::INVALID_LIMITS:
    return "INVALID_LIMITS";
  case CellBufferError::INVALID_POLICY:
    return "INVALID_POLICY";
  case CellBufferError::COLUMN_LIMIT:
    return "COLUMN_LIMIT";
  case CellBufferError::ROW_LIMIT:
    return "ROW_LIMIT";
  case CellBufferError::CELL_LIMIT:
    return "CELL_LIMIT";
  case CellBufferError::OUT_OF_BOUNDS:
    return "OUT_OF_BOUNDS";
  case CellBufferError::INVALID_GRAPHEME:
    return "INVALID_GRAPHEME";
  case CellBufferError::POLICY_MISMATCH:
    return "POLICY_MISMATCH";
  case CellBufferError::WIDE_AT_EDGE:
    return "WIDE_AT_EDGE";
  case CellBufferError::GLYPH_LIMIT:
    return "GLYPH_LIMIT";
  case CellBufferError::GLYPH_BYTE_LIMIT:
    return "GLYPH_BYTE_LIMIT";
  case CellBufferError::STYLE_LIMIT:
    return "STYLE_LIMIT";
  case CellBufferError::ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(TextErrorKind value) noexcept {
  switch (value) {
  case TextErrorKind::INVALID_UTF8:
    return "INVALID_UTF8";
  case TextErrorKind::INPUT_LIMIT:
    return "INPUT_LIMIT";
  case TextErrorKind::CLUSTER_LIMIT:
    return "CLUSTER_LIMIT";
  case TextErrorKind::GRAPHEME_LIMIT:
    return "GRAPHEME_LIMIT";
  case TextErrorKind::CELL_LIMIT:
    return "CELL_LIMIT";
  case TextErrorKind::INVALID_POLICY:
    return "INVALID_POLICY";
  case TextErrorKind::NOT_BOUNDARY:
    return "NOT_BOUNDARY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(ShortcutErrorKind value) noexcept {
  switch (value) {
  case ShortcutErrorKind::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case ShortcutErrorKind::SOURCE_LIMIT:
    return "SOURCE_LIMIT";
  case ShortcutErrorKind::ALTERNATIVE_LIMIT:
    return "ALTERNATIVE_LIMIT";
  case ShortcutErrorKind::STROKE_LIMIT:
    return "STROKE_LIMIT";
  case ShortcutErrorKind::EMPTY_ALTERNATIVE:
    return "EMPTY_ALTERNATIVE";
  case ShortcutErrorKind::EMPTY_TOKEN:
    return "EMPTY_TOKEN";
  case ShortcutErrorKind::UNKNOWN_KEY:
    return "UNKNOWN_KEY";
  case ShortcutErrorKind::DUPLICATE_MODIFIER:
    return "DUPLICATE_MODIFIER";
  case ShortcutErrorKind::EXPECTED_KEY:
    return "EXPECTED_KEY";
  case ShortcutErrorKind::UNSUPPORTED_SEQUENCE:
    return "UNSUPPORTED_SEQUENCE";
  case ShortcutErrorKind::INVALID_CHORD:
    return "INVALID_CHORD";
  case ShortcutErrorKind::NO_MEMORY:
    return "NO_MEMORY";
  case ShortcutErrorKind::INVALID_ACTION_SET:
    return "INVALID_ACTION_SET";
  case ShortcutErrorKind::AMBIGUOUS_PREFIX:
    return "AMBIGUOUS_PREFIX";
  case ShortcutErrorKind::RESERVED_CANCEL:
    return "RESERVED_CANCEL";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(TextResourceError value) noexcept {
  switch (value) {
  case TextResourceError::MISSING_RESOURCE:
    return "MISSING_RESOURCE";
  case TextResourceError::RESOLVER_REQUIRED:
    return "RESOLVER_REQUIRED";
  case TextResourceError::RESOLVER_FAILED:
    return "RESOLVER_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(PssErrorCode value) noexcept {
  switch (value) {
  case PssErrorCode::NONE:
    return "NONE";
  case PssErrorCode::INPUT_LIMIT:
    return "INPUT_LIMIT";
  case PssErrorCode::RULE_LIMIT:
    return "RULE_LIMIT";
  case PssErrorCode::CLASS_LIMIT:
    return "CLASS_LIMIT";
  case PssErrorCode::DECLARATION_LIMIT:
    return "DECLARATION_LIMIT";
  case PssErrorCode::TEXT_LIMIT:
    return "TEXT_LIMIT";
  case PssErrorCode::DOCUMENT_LIMIT:
    return "DOCUMENT_LIMIT";
  case PssErrorCode::SOURCE_NAME_LIMIT:
    return "SOURCE_NAME_LIMIT";
  case PssErrorCode::INVALID_UTF8:
    return "INVALID_UTF8";
  case PssErrorCode::NUL_BYTE:
    return "NUL_BYTE";
  case PssErrorCode::EXPECTED_SELECTOR:
    return "EXPECTED_SELECTOR";
  case PssErrorCode::EXPECTED_IDENTIFIER:
    return "EXPECTED_IDENTIFIER";
  case PssErrorCode::EXPECTED_PUNCTUATION:
    return "EXPECTED_PUNCTUATION";
  case PssErrorCode::UNTERMINATED_COMMENT:
    return "UNTERMINATED_COMMENT";
  case PssErrorCode::DUPLICATE_ID:
    return "DUPLICATE_ID";
  case PssErrorCode::DUPLICATE_STATE:
    return "DUPLICATE_STATE";
  case PssErrorCode::UNKNOWN_STATE:
    return "UNKNOWN_STATE";
  case PssErrorCode::UNKNOWN_PROPERTY:
    return "UNKNOWN_PROPERTY";
  case PssErrorCode::INVALID_VALUE:
    return "INVALID_VALUE";
  case PssErrorCode::INVALID_WIDTH:
    return "INVALID_WIDTH";
  case PssErrorCode::ALLOCATION:
    return "ALLOCATION";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(PssFileOperation value) noexcept {
  switch (value) {
  case PssFileOperation::OPEN:
    return "OPEN";
  case PssFileOperation::READ:
    return "READ";
  case PssFileOperation::INPUT_LIMIT:
    return "INPUT_LIMIT";
  }

  return "UNKNOWN";
}

void reportTerminal(const TerminalError &cause) noexcept {
  std::fprintf(stderr, "TerminalError(kind=%s, native=%d, restoration=%d)", categoryName(cause.kind), cause.native_code,
               cause.restoration_code);
}

void reportApplication(const ApplicationError &cause) noexcept {
  std::fprintf(stderr, "ApplicationError(phase=%s, cause=", categoryName(cause.phase));
  std::visit(
    [](const auto &value) {
      if constexpr (std::same_as<std::decay_t<decltype(value)>, TerminalError>) {
        reportTerminal(value);
      } else if constexpr (std::same_as<std::decay_t<decltype(value)>, RenderError>) {
        std::fprintf(stderr, "RenderError(kind=%s, buffer=%s)", categoryName(value.kind),
                     categoryName(value.buffer_error));
      } else if constexpr (std::same_as<std::decay_t<decltype(value)>, TreeError>) {
        std::fprintf(stderr, "TreeError(%s)", treeErrorName(value));
      } else {
        std::fprintf(stderr, "%s", categoryName(value));
      }
    },
    cause.cause);
  if (cause.background) {
    std::fprintf(stderr, ", background=BackgroundError(%s)", categoryName(*cause.background));
  }

  std::fputc(')', stderr);

  if (cause.restoration) {
    std::fputs(" restoration=", stderr);
    reportTerminal(*cause.restoration);
  }
}

// Protected diagnostics contain only library categories and numeric metadata.
void reportSecureError(const SecureStringError &error) noexcept {
  std::fprintf(stderr, "operation=%u, code=%u, native=%u", static_cast<unsigned>(error.operation),
               static_cast<unsigned>(error.code), static_cast<unsigned>(error.nativeCode));
}

void reportSecureCleanup(const SecureStringCleanupErrors &cleanup) noexcept {
  std::fprintf(stderr, ", cleanup-count=%zu, cleanup-omitted=%zu", cleanup.count, cleanup.omittedCleanupCount);
  for (std::size_t index = 0; index < std::min(cleanup.count, cleanup.errors.size()); ++index) {
    std::fputs(", cleanup=(", stderr);
    reportSecureError(cleanup.errors[index]);
    std::fputc(')', stderr);
  }
}

void reportSecureFailure(const SecureStringFailure &failure) noexcept {
  std::fputs("secure=(", stderr);
  reportSecureError(failure.primary);
  reportSecureCleanup(failure.cleanup);
  std::fputc(')', stderr);
}

void reportPasswordFailure(const PasswordFailure &failure) noexcept {
  std::fprintf(stderr, " password=(kind=%zu, ", failure.index());
  std::visit(
    [](const auto &value) {
      using T = std::remove_cvref_t<decltype(value)>;
      if constexpr (std::same_as<T, TreeError>) {
        std::fprintf(stderr, "tree=%s", treeErrorName(value));
      } else if constexpr (std::same_as<T, TextError>) {
        std::fprintf(stderr, "text=%s, offset=%zu", categoryName(value.kind), value.byte_offset);
      } else if constexpr (std::same_as<T, PasswordInputError>) {
        std::fprintf(stderr, "input=%u", static_cast<unsigned>(value));
      } else if constexpr (std::same_as<T, SecureStringFailure>) {
        reportSecureFailure(value);
      } else {
        if (value.ownership) {
          std::fprintf(stderr, "owner=%s, ", treeErrorName(*value.ownership));
        }
        if (value.access) {
          if (value.access->primary) {
            std::fputs("primary=(", stderr);
            reportSecureError(*value.access->primary);
            std::fputs("), ", stderr);
          }
          if constexpr (std::same_as<T, PasswordValueAccessFailure<SecureStringFailure>>) {
            if (value.access->callbackError) {
              std::fputs("callback-", stderr);
              reportSecureFailure(*value.access->callbackError);
            }
          } else if constexpr (std::same_as<T, PasswordValueAccessFailure<TextError>>) {
            if (value.access->callbackError) {
              std::fprintf(stderr, "callback-text=%s, offset=%zu", categoryName(value.access->callbackError->kind),
                           value.access->callbackError->byte_offset);
            }
          } else if (value.access->callbackError) {
            std::fputs("callback-error=unit", stderr);
          }
          reportSecureCleanup(value.access->cleanup);
        }
      }
    },
    failure);
  std::fputc(')', stderr);
}

void reportFailure(const ManagedError &failure, std::source_location origin, std::size_t droppedErrors) noexcept {
  std::fprintf(stderr, "Portals: scene failed (phase=%s) at %s:%u: ", phaseName(failure.phase), origin.file_name(),
               origin.line());
  if (failure.operation) {
    std::fprintf(stderr, "operation=%s ", failure.operation);
  }

  if (failure.execution) {
    std::fprintf(stderr, "execution=%s ", failure.execution);
  }

  if (failure.setupStopped) {
    std::fputs("response=setup-stopped ", stderr);
  }

  std::visit(
    [](const auto &cause) {
      using Cause = std::decay_t<decltype(cause)>;

      if constexpr (std::same_as<Cause, TreeError>) {
        std::fprintf(stderr, "TreeError(%s)", treeErrorName(cause));
      } else if constexpr (std::same_as<Cause, StyleError>) {
        std::fprintf(stderr, "StyleError(cause=%s, line=%zu, column=%zu)", treeErrorName(cause.cause), cause.line,
                     cause.column);
      } else {
        reportApplication(cause);
      }
    },
    failure.cause);

  if (failure.styleSource) {
    std::fputs(" source=", stderr);
    std::fwrite(failure.styleSource->data(), 1, failure.styleSource->size(), stderr);
  }

  if (failure.sceneResource) {
    const auto &resource = *failure.sceneResource;

    // The owning source is a safe label/path; never echo document or secret bytes.
    if (resource.line == 0) {
      std::fprintf(stderr, " scene-resource=(code=%u, registration, system=%d)", static_cast<unsigned>(resource.code),
                   resource.system.value());
    } else {
      std::fprintf(stderr, " scene-resource=(code=%u, offset=%zu, line=%zu, column=%zu, system=%d)",
                   static_cast<unsigned>(resource.code), resource.offset, resource.line, resource.column,
                   resource.system.value());
    }
  }

  if (failure.pss) {
    const auto &pss = *failure.pss;

    std::fprintf(stderr, " pss=(code=%s, offset=%zu, line=%zu, column=%zu, subject=", categoryName(pss.code),
                 pss.location.offset, pss.location.line, pss.location.column);
    std::fwrite(pss.subject.data(), 1, std::min(pss.subjectSize, pss.subject.size()), stderr);
    std::fputs(", message=", stderr);
    std::fwrite(pss.message.data(), 1, pss.message.size(), stderr);
    std::fputc(')', stderr);
  }

  if (failure.file) {
    const auto &file = *failure.file;

    std::fprintf(stderr, " file=(operation=%s, category=%s, value=%d)", categoryName(file.operation),
                 file.cause.category().name(), file.cause.value());
  }

  if (failure.application) {
    std::fputs(" runner=", stderr);
    reportApplication(*failure.application);
  }

  if (failure.cleanup) {
    std::fprintf(stderr, " cleanup=TreeError(%s)", treeErrorName(*failure.cleanup));
  }

  if (failure.text) {
    std::fprintf(stderr, " text=(kind=%s, offset=%zu)", categoryName(failure.text->kind), failure.text->byte_offset);
  }

  if (failure.resource) {
    std::fprintf(stderr, " resource=%s", categoryName(*failure.resource));
  }

  if (failure.async) {
    std::fprintf(stderr, " async=AsyncError(%s)", categoryName(*failure.async));
  }

  if (failure.background) {
    std::fprintf(stderr, " background=BackgroundError(%s)", categoryName(*failure.background));
  }

  if (failure.password) {
    reportPasswordFailure(*failure.password);
  }

  if (failure.shortcut) {
    std::fprintf(stderr, " shortcut=(cause=%s, offset=%zu)", categoryName(failure.shortcut->cause),
                 failure.shortcut->offset);
  }

  if (droppedErrors) {
    std::fprintf(stderr, " dropped=%zu", droppedErrors);
  }

  std::fputc('\n', stderr);
}
} // namespace

void reportToStderr(const SceneReport &report) noexcept {
  if (report.commandLineFailure) {
    const char *category = "INVALID_ARGUMENT";

    switch (report.commandLineFailure->code) {
    case CommandLineErrorCode::INVALID_ARGUMENT:
      break;
    case CommandLineErrorCode::CALLBACK_FAILED:
      category = "CALLBACK_FAILED";
      break;
    case CommandLineErrorCode::NO_MEMORY:
      category = "NO_MEMORY";
      break;
    case CommandLineErrorCode::PROCESS_CONTEXT:
      category = "PROCESS_CONTEXT";
      break;
    }

    std::fprintf(stderr, "Portals: command line error (%s): %s\n", category, report.commandLineFailure->message.data());
  }

  if (report.failure) {
    reportFailure(*report.failure, report.origin, report.droppedErrors);
  } else if (!report.commandLineFailure) {
    std::fprintf(stderr, "Portals: recoverable diagnostics: retained=%zu, dropped=%zu; inspect diagnostics()\n",
                 report.retainedErrors, report.droppedErrors);
  }
}

namespace detail {
void deliverFactoryReport(const SceneReportHandler &sink, const SceneReport &report) noexcept {
  FactoryCleanupScope factoryMask;
  SetupFailureScope reportingScope({}, nullptr, nullptr);
  CurrentSceneScope currentMask;

  try {
    if (sink) {
      sink(report);
    } else {
      reportToStderr(report);
    }
  } catch (...) {
    reportToStderr(report);
  }
}

SceneReport factoryFailureReport(ManagedError failure, std::source_location origin,
                                 const FactoryCleanup &cleanup) noexcept {
  SceneReport report;

  if (!failure.operation) {
    failure.operation = "scene.factory";
    failure.operationOrigin = origin;
  }

  failure.execution = "scene.factory";

  report.failure = std::move(failure);
  report.origin = report.failure->operationOrigin.value_or(origin);
  report.droppedErrors = cleanup.attach(*report.failure);

  return report;
}

void reportFactoryFailure(const SceneReportHandler &sink, ManagedError failure, std::source_location origin,
                          const FactoryCleanup &cleanup) noexcept {
  const auto report = factoryFailureReport(std::move(failure), origin, cleanup);

  deliverFactoryReport(sink, report);
}
} // namespace detail

} // namespace portals::ui
