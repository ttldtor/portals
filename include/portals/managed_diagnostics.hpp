// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/application.hpp>
#include <portals/detail/pss_data.hpp>
#include <portals/pss_file_error.hpp>
#include <portals/shortcut.hpp>
#include <portals/style.hpp>
#include <portals/text_resource.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

namespace portals {

class ManagedScene;

namespace detail {
/** @brief Default-empty internal evidence that a short error entered one live collector.
 * Copies preserve provenance without retaining a scene. Only the managed reporting boundary can stamp it.
 */
class ManagedErrorReceipt {
public:
  ManagedErrorReceipt() = default;

private:
  friend class portals::ManagedScene;
  std::weak_ptr<const void> collector_;
};
} // namespace detail

/** @brief Boundary at which a managed operation failed. */
enum class ManagedPhase {
  MOUNT,   ///< Create the scene root or attach and register a managed widget.
  STATE,   ///< Create and retain a scene-owned property.
  BIND,    ///< Register a managed callback, binding or presentation adapter.
  STYLE,   ///< Register or apply styles, including a theme change.
  PREPARE, ///< Synchronize managed presentation, focus and pending updates before a frame.
  INPUT,   ///< Dispatch input or deliver a managed control/property callback.
  RUN,     ///< Enter the application runner or propagate a failure from it.
  CLEANUP, ///< Remove a managed subtree or retire the scene and its registrations.
  TEXT     ///< Read or mutate a fluent caption/editor draft through a checked observing reference.
};

/** @brief Original error plus independent runner/restoration and cleanup failures. */
struct ManagedError {
  ManagedPhase phase{ManagedPhase::RUN}; ///< First failing managed operation.
  NodeId node{};                         ///< Target identity, empty when no node was involved.
  /// @brief Unformatted original error; StyleError messages follow their static-storage contract.
  std::variant<TreeError, StyleError, ApplicationError> cause;
  std::optional<ApplicationError> application; ///< Runner error, including terminal restoration.
  std::optional<TreeError> cleanup;            ///< Independent failure while retiring the subtree.
  std::optional<TextError> text{};             ///< Original text-validation category and byte offset, when present.
  std::optional<TextResourceError> resource{}; ///< Original resource lookup category, when present.
  std::optional<PssError>
    pss{}; ///< Exact allocation-free PSS failure, including its owned subject and source position.
  std::optional<PssFileError> file{}; ///< Original file operation and error code, independent of parser failures.
  /// @brief Immutable owning source identity, or empty when unavailable; never borrowed through an error message.
  /// Facades may preallocate this identity before application. Low-level non-OOM failures may attach it best-effort.
  /// Invalid/oversized identities use an empty label or a bounded complete UTF-8 prefix; original PSS errors survive.
  /// NO_MEMORY handling performs no label allocation. Copies share the identity without retaining a scene.
  std::shared_ptr<const std::string> styleSource{};
  std::optional<ShortcutError> shortcut{}; ///< Exact shortcut syntax/value cause and owned offset, when present.
  std::optional<std::source_location>
    registrationOrigin{}; ///< Static registration identity for a shortcut action failure; never a borrowed source view.
  const char *operation{}; ///< Library static-storage operation label, independent of allocated diagnostic contexts.
  std::optional<std::source_location>
    operationOrigin{};     ///< Original entered operation site, before facade propagation.
  const char *execution{}; ///< Library static-storage facade boundary label; does not replace the original phase.
  bool setupStopped{};     ///< A scoped short failure stopped subsequent setup/run after the current callback returned.
  detail::ManagedErrorReceipt reportReceipt{}; ///< Opaque copied reporting provenance; default construction is empty.
};

/** @brief Finite error-history and causal-context budgets. All limits must be positive. */
struct ManagedDiagnosticOptions {
  std::size_t maxErrors{64};         ///< Maximum retained errors; the oldest entries survive overflow.
  std::size_t maxContexts{8};        ///< Maximum causal breadcrumbs in each error, in supplied order.
  std::size_t maxContextBytes{1024}; ///< Total bytes retained across breadcrumb labels in each error.
};

/** @brief Owning causal breadcrumb; source_location refers to compiler-owned static source names. */
struct ManagedDiagnosticContext {
  std::string operation = std::string(0, '\0');    ///< Operation label, copied without formatting or interpretation.
  std::string name = std::string(0, '\0');         ///< Optional application reference or property name.
  std::string scope = std::string(0, '\0');        ///< Optional application scope name.
  std::string expectedType = std::string(0, '\0'); ///< Optional expected widget or value type name.
  std::source_location origin{}; ///< Original registration/lookup call site, captured before deferral.
};

/** @brief One owned error and its bounded causal trace, independent of widget lifetime. */
struct ManagedDiagnostic {
  ManagedError error; ///< Original typed error and independent final failures.
  /// @brief Owning breadcrumbs, oldest supplied context first; empty construction can propagate allocation failure.
  std::vector<ManagedDiagnosticContext> contexts = std::vector<ManagedDiagnosticContext>(0);
  std::source_location origin{}; ///< Report call site; not a physical runtime stack.
  bool contextsTruncated{};      ///< Context count or label byte budget truncated the trace.
  bool contextUnavailable{};     ///< Context allocation failed; error and origin survive.
  bool storageOverflow{};        ///< Observer-only fallback: history was already full.
  bool notificationSuppressed{}; ///< A nested report was stored without recursive notification.
  bool observerFailed{};         ///< The observer threw while receiving this error.
};

/** @brief Owning snapshot or drained history; counters saturate at size_t's maximum. */
struct ManagedDiagnosticBatch {
  /// @brief Retained errors in report order; no borrowed widget state.
  std::vector<ManagedDiagnostic> errors = std::vector<ManagedDiagnostic>(0);
  std::size_t droppedErrors{};           ///< Reports omitted because the bounded history was full.
  std::size_t contextFailures{};         ///< Reports whose breadcrumb allocation failed.
  std::size_t observerFailures{};        ///< Observer exceptions, separate from original causes.
  std::size_t suppressedNotifications{}; ///< Nested reports which could not invoke onError recursively.
  bool closed{};                         ///< Scene was closed when this snapshot was taken.
};

/** @brief Scene-owned UI observer; the diagnostic is borrowed only until the call returns. */
using ManagedErrorHandler = std::function<void(const ManagedDiagnostic &)>;

} // namespace portals
