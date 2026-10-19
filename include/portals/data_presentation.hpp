// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <array>
#include <portals/controls.hpp>
#include <portals/model.hpp>
#include <portals/text_value.hpp>

namespace portals {
/** @brief Presentation facts; offline is explicit domain information, never inferred from an error string. */
enum class DataPhase {
  READY,     ///< Successful nonempty content; the reserved status strip is blank.
  LOADING,   ///< Initial or viewport request is pending.
  EMPTY,     ///< Successful content has zero logical rows.
  FAILED,    ///< Domain failure; independent technical errors retain their typed cause.
  OFFLINE,   ///< Explicit provider connectivity state, never inferred by the control.
  REFRESHING ///< Pending work with provider-permitted last-good content still visible.
};

/** @brief Owning scalar state of one immutable presentation producer. */
struct DataPresentation {
  DataPhase phase{DataPhase::READY}; ///< Requested presentation; technical errors are separate.
  std::uint64_t generation{1};       ///< Nonzero producer generation; older values are ignored, never wrapped.
  /** @param other Owning state to compare. @return True when phase and generation match. */
  constexpr bool operator==(const DataPresentation &other) const = default;
};

/** @brief Owning facts from the same successful virtual-view preparation, without another model read. */
struct DataRangeStatus {
  bool prepared{};                       ///< False before preparation or after hidden preparation.
  RangeState state{RangeState::PENDING}; ///< Readiness of this prepared range, not the entire model.
  std::uint64_t rowCount{};              ///< Logical model count; zero denotes a known empty model.
  std::uint64_t dataRevision{};          ///< Provider revision; zero remains valid under the existing model contract.
  bool requestPending{}; ///< This view still owns current request work; queue acceptance is not UI completion.
};

/** @param status Prepared owning facts. @return Mapped phase, or INVALID_ARGUMENT for an unknown readiness enum. */
constexpr std::expected<DataPhase, TreeError> dataPhase(DataRangeStatus status) {
  if (status.state != RangeState::PENDING && status.state != RangeState::READY && status.state != RangeState::FAILED) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (!status.prepared) {
    return DataPhase::LOADING;
  }
  if (status.requestPending) {
    return status.state == RangeState::READY ? DataPhase::REFRESHING : DataPhase::LOADING;
  }
  if (status.state == RangeState::FAILED) {
    return DataPhase::FAILED;
  }
  if (!status.rowCount) {
    return DataPhase::EMPTY;
  }
  return status.state == RangeState::READY ? DataPhase::READY : DataPhase::LOADING;
}

/** @param phase Presentation to inspect. @return True only for LOADING or REFRESHING, including false for unknown
 * enums. */
constexpr bool isDataPending(DataPhase phase) noexcept {
  return phase == DataPhase::LOADING || phase == DataPhase::REFRESHING;
}
/** @brief Explicit content policy; automatic range wrappers always keep their request-owning body live. */
enum class DataContentPolicy {
  KEEP_BODY, ///< Keep the body visible and prepared in every phase.
  REPLACE    ///< Explicit mode only: hide the independently owned body outside READY/REFRESHING.
};

/** @brief Owning message expressions resolved once under the receiving scene width policy. */
struct DataStateLabels {
  TextValue loading{"Loading..."};        ///< Message while initial/range work is pending.
  TextValue empty{"No data"};             ///< Successful empty model message.
  TextValue error{"Unable to load data"}; ///< Domain failure message; technical diagnostics are separate.
  TextValue offline{"Offline"};           ///< Explicit domain connectivity message.
  TextValue refreshing{"Refreshing..."};  ///< Current or explicitly permitted last-good data remains visible.
  TextValue retry{"Retry"};               ///< Caption of the optional explicit retry action.
};

/** @brief Construction-only settings for retained data presentation. */
struct DataStateConfig {
  /// @brief Release owning construction settings.
  ~DataStateConfig() = default; // Keep checked MSVC owning expression transfers fallible.
  bool showStatus{true};        ///< Mount a stable status strip; false creates no timer, labels or retry button.
  std::int32_t statusRows{1};   ///< Positive fixed strip height in terminal rows, including READY.
  DataContentPolicy contentPolicy{
    DataContentPolicy::KEEP_BODY};    ///< REPLACE requires explicit independently owned work.
  SchedulerTime spinnerInterval{100}; ///< Positive monotonic milliseconds between steps; no catch-up burst.
  DataStateLabels labels;             ///< Owning expressions, validated only when the strip is used.
  std::function<std::expected<std::string, TextResourceError>(std::string_view)>
    resolver;                ///< Owning mount-time i18n adapter; no automatic locale subscription.
  ControlStyle messageStyle; ///< Manual message/indicator appearance beneath theme resolution.
  ControlStyle retryStyle;   ///< Manual retry appearance beneath theme resolution.
  SizePolicy policy;         ///< Root layout constraints, validated by the existing solver.
  std::function<std::expected<void, TreeError>()> retry; ///< Optional owning explicit action; never starts implicitly.
};

namespace detail {
struct DataStateRecord;
struct DataStateOwner;
} // namespace detail

/** @brief Retained Column composition of status and content; it owns no provider, cache or second runtime. */
class DataStateView : public Column {
public:
  /** @param policy Root layout constraints. Mount through ui::dataStateView to configure the composite. */
  explicit DataStateView(SizePolicy policy = {}) : Column({}, policy) {
  }

  /** @return Owning last accepted presentation; read through WidgetTree::read on its UI thread. */
  DataPresentation presentation() const noexcept {
    return presentation_;
  }

private:
  friend class ManagedScene;
  friend std::expected<void, TreeError> setDataStateView(WidgetTree &, Node<DataStateView>, DataPresentation);
  std::weak_ptr<detail::DataStateRecord> record_;
  DataPresentation presentation_;
};

/** @param tree Borrowed UI owner. @param node Live configured own-value composite.
 * @param value Owning nonzero-generation state; older values are no-ops after affinity/liveness checks.
 * @return Success, original tree/NO_MEMORY error, or INVALID_ARGUMENT for invalid/bound/automatic modes.
 * Changes stage owning text before visual publication. Mutations inside a frame are WOULD_BLOCK.
 */
std::expected<void, TreeError> setDataStateView(WidgetTree &tree, Node<DataStateView> node, DataPresentation value);
} // namespace portals
