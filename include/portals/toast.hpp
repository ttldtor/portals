// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/dialog_command_result.hpp>
#include <portals/scheduler.hpp>
#include <portals/text_value.hpp>

namespace portals {
namespace detail {
struct NotificationRecord;
/// @cond INTERNAL
enum class ToastCommand {
  DISMISS,
  ACTIVATE,
  CLEAR,
  VISIBLE,
  REDUCED_MOTION
};
/// @endcond
} // namespace detail
/** @brief Notification meaning, independent of persistence or application error policy. */
enum class ToastSeverity {
  INFO,    ///< Informational message, represented by i in monochrome.
  SUCCESS, ///< Successful application operation, represented by +.
  WARNING, ///< Application warning, represented by !.
  FAILURE  ///< Application failure, represented by x; does not intercept runtime errors.
};
/** @brief Logical admission result; visibility does not acknowledge terminal output. */
enum class ToastPostStatus {
  ACCEPTED_VISIBLE, ///< Selected within the logical visible cap; may still be clipped.
  ACCEPTED_QUEUED,  ///< Retained behind the selected FIFO prefix.
  DEDUPLICATED,     ///< Existing identity, payload and action retained.
  DROPPED           ///< Successful intentional overflow outcome, without an adopted identity.
};
/** @brief Corner of the owning content viewport. */
enum class ToastPlacement {
  BOTTOM_END,   ///< Lower right corner.
  TOP_END,      ///< Upper right corner.
  BOTTOM_START, ///< Lower left corner.
  TOP_START     ///< Upper left corner.
};
/** @brief Policy when the total retained-record budget is full. */
enum class ToastOverflowPolicy {
  DROP_OLDEST_PENDING, ///< Evict the oldest eligible queued transient, otherwise drop the new request.
  REJECT_NEW           ///< Fail with NODE_LIMIT without changing the queue.
};

/** @brief Original center identity and never-reused record sequence. */
struct ToastId {
  NodeId center{};          ///< Complete session, slot and generation of the owning layer.
  std::uint64_t sequence{}; ///< Nonzero monotonically increasing center-local identity.
  /** @param other Complete identity to compare. @return True for the same original notification. */
  bool operator==(const ToastId &other) const noexcept = default;
};

/** @brief Optional static or alternating one-cell diagnostic decoration. */
struct ToastIndicatorOptions {
  /** @brief Keep checked string copies fallible on the MSVC debug standard library. */
  ~ToastIndicatorOptions() = default;
  bool enabled{};                         ///< Keep the ordinary presentation free of an indicator by default.
  TextValue glyph{std::string_view("!")}; ///< Exactly one grapheme occupying one cell under the scene policy.
  bool alternateBackground{};             ///< Opt in to the bounded indicator effect.
  RgbColor foreground{255, 255, 255};     ///< Manual foreground beneath theme overrides.
  RgbColor alertBackground{192, 0, 0};    ///< Alternate background beneath theme overrides.
  SchedulerTime period{500};              ///< Positive milliseconds between phase changes when enabled.
};

/** @brief Owning notification request settings; copying may allocate. */
struct ToastOptions {
  /** @brief Keep checked string copies fallible on the MSVC debug standard library. */
  ~ToastOptions() = default;
  ToastSeverity severity{ToastSeverity::INFO}; ///< Readable semantic severity.
  std::optional<SchedulerTime> duration{5000}; ///< Positive milliseconds after first presentation; null is persistent.
  std::string dedupKey = std::string(0, '\0'); ///< Exact case-sensitive bytes; empty disables deduplication.
  bool dismissOnAction{true};                  ///< Remove before entering the admitted action.
  ToastIndicatorOptions indicator{};           ///< Optional presentation only; no diagnostic interception.
};

/** @brief Fixed host budgets and explicit localization; no borrowed option spans. */
struct NotificationOptions {
  /** @brief Retain ordinary fallible copies of owning resolver storage. */
  ~NotificationOptions() = default;
  std::size_t capacity{32};                             ///< Positive total budget including selected records.
  std::size_t maxVisible{3};                            ///< Logical selection cap in [1, capacity].
  ToastPlacement placement{ToastPlacement::BOTTOM_END}; ///< Corner for retained row placement.
  std::int32_t maximumWidth{48};                        ///< Positive width cap in terminal cells.
  std::int32_t margin{1};                               ///< Nonnegative viewport inset, clamped at tiny extents.
  std::int32_t gap{1};                                  ///< Nonnegative row separation, clamped to the viewport.
  ToastOverflowPolicy overflow{
    ToastOverflowPolicy::DROP_OLDEST_PENDING}; ///< Never evict visible, persistent or running actions.
  bool refreshDeduplicated{};                  ///< Refresh only an existing, previously presented transient deadline.
  bool reducedMotion{};                        ///< Retain static indicators while disabling alternation.
  TextLimits textLimits{16384, 4096, 16384, 32768}; ///< Full resolved message validation caps.
  std::size_t actionLabelBytes{256};                ///< Positive action-label byte cap.
  std::size_t dedupKeyBytes{128};                   ///< Positive deduplication-key byte cap.
  std::size_t maxPaintCells{16384};                 ///< Positive per-card allocated paint-area cap.
  std::function<std::expected<std::string, TextResourceError>(std::string_view)>
    textResolver; ///< UI-only explicit owning resolver.
};

/** @brief Allocation-free queue admission receipt. */
struct ToastPostReceipt {
  ToastPostStatus status{};         ///< Selected, queued, duplicate or intentionally dropped.
  std::optional<ToastId> id;        ///< Original retained identity; absent for DROPPED.
  std::optional<ToastId> evictedId; ///< Exact pending transient evicted by committed overflow policy.
};

/** @brief Immutable owning payload, independent of callback and scene lifetimes. */
struct ToastData {
  /** @brief Keep validated owning-text copies fallible rather than introducing noexcept transfers. */
  ~ToastData() = default;
  GraphemeText text;                       ///< Complete validated message, including undisplayed logical lines.
  std::optional<GraphemeText> actionLabel; ///< Complete action caption, if any; no callable is exposed.
  ToastSeverity severity{};                ///< Owning semantic severity.
};

/** @brief Original action identity and owning immutable data, safe after subtree removal. */
struct ToastActionEvent {
  ToastId id;                            ///< Exact notification admitted for this invocation.
  std::shared_ptr<const ToastData> data; ///< Message lifetime independent of the removed row.
};

/** @brief Owning record observation; no callback or scene references. */
struct ToastSnapshot {
  ToastId id;                            ///< Original center and record identity.
  std::shared_ptr<const ToastData> data; ///< Full immutable payload.
  ToastPostStatus status{};              ///< Current logical selection, independent of clipping.
  std::optional<CellRect> rectangle;     ///< Last final arranged surface rectangle when assigned.
  bool firstPresented{};                 ///< At least one successful final frame had positive clipped cells.
  std::optional<SchedulerTime> deadline; ///< Absolute expiry, absent until first presentation or for persistence.
};

/** @brief Independent bounded queue snapshot with saturating policy counters. */
struct NotificationSnapshot {
  /** @brief Keep checked vector transfers fallible on the MSVC debug standard library. */
  ~NotificationSnapshot() = default;
  std::vector<ToastSnapshot> items = std::vector<ToastSnapshot>(0); ///< FIFO records without UI callbacks.
  std::uint64_t dropped{};                                          ///< Intentionally rejected new messages.
  std::uint64_t evicted{};                                          ///< Pending transient overflow victims.
  bool visible{};                                                   ///< Explicit center visibility policy.
  bool reducedMotion{};                                             ///< Explicit center effect policy.
};

namespace ui {
namespace detail {
struct NotificationTarget;
struct NotificationAccess;
/// @cond INTERNAL
template <class T>
concept ToastActionResult = std::same_as<T, void> || std::same_as<T, std::expected<void, TreeError>>;
template <class Callback>
concept ToastAction =
  std::copy_constructible<Callback> && (requires(Callback &callback, const ToastActionEvent &event) {
    { callback(event) } -> ToastActionResult;
  } || (!std::invocable<Callback &, const ToastActionEvent &> && requires(Callback &callback) {
                                          { callback() } -> ToastActionResult;
                                        }));
/// @endcond
} // namespace detail

/** @brief Owning UI-only request builder; borrowed callback captures remain the caller's responsibility. */
class ToastMessage {
public:
  /** @param text Owning message expression. @param options Owning request policy, checked at adoption. */
  explicit ToastMessage(TextValue text, ToastOptions options = {}) : text_(std::move(text)), options_(options) {
  }

  /** @return Consuming request with INFO presentation; other options and actions are retained. */
  ToastMessage &&info() && {
    options_.severity = ToastSeverity::INFO;

    return std::move(*this);
  }

  /** @return Consuming request with SUCCESS presentation; the last severity modifier wins. */
  ToastMessage &&success() && {
    options_.severity = ToastSeverity::SUCCESS;

    return std::move(*this);
  }

  /** @return Consuming request with WARNING presentation, without changing queue policy. */
  ToastMessage &&warning() && {
    options_.severity = ToastSeverity::WARNING;

    return std::move(*this);
  }

  /** @return Consuming request with FAILURE presentation; this does not cause a runtime failure. */
  ToastMessage &&error() && {
    options_.severity = ToastSeverity::FAILURE;

    return std::move(*this);
  }

  /** @tparam Callback Copyable callable accepting const ToastActionEvent& or no argument, returning void or
   * expected<void,TreeError>.
   * @param label Owning nonempty action caption. @param callback UI-owned callable; never transferred to workers.
   * @return Consuming request retaining the action. Construction may allocate before an owner exists.
   */
  template <class Callback>
    requires detail::ToastAction<Callback>
  ToastMessage &&action(TextValue label, Callback callback) && {
    label_.emplace(std::move(label));

    action_ = std::make_shared<Action>(
      [callback = std::move(callback)](
        [[maybe_unused]] const ToastActionEvent &event) mutable -> std::expected<void, TreeError> {
        const auto invoke = [&]() -> decltype(auto) {
          if constexpr (std::invocable<Callback &, const ToastActionEvent &>) {
            return callback(event);
          } else {
            return callback();
          }
        };

        static_assert(std::same_as<decltype(invoke()), void> ||
                      std::same_as<decltype(invoke()), std::expected<void, TreeError>>);

        if constexpr (std::same_as<decltype(invoke()), void>) {
          invoke();

          return {};
        } else {
          return invoke();
        }
      });

    return std::move(*this);
  }

  /** @tparam Callback Same action contract as the owning-text overload.
   * @param label Borrowed bytes copied immediately. @param callback Owning UI action.
   * @return Consuming request; no string_view is retained.
   */
  template <class Callback>
    requires detail::ToastAction<Callback>
  ToastMessage &&action(std::string_view label, Callback callback) && {
    return std::move(*this).action(TextValue(label), std::move(callback));
  }

private:
  friend struct detail::NotificationAccess;
  friend class portals::ManagedScene;
  using Action = std::function<std::expected<void, TreeError>(const ToastActionEvent &)>;
  TextValue text_;
  ToastOptions options_;
  std::optional<TextValue> label_;
  std::shared_ptr<Action> action_;
};

/** @param text Owning text expression. @param options Owning policy. @return Owning UI request builder. */
inline ToastMessage toast(TextValue text, ToastOptions options = {}) {
  return ToastMessage(std::move(text), options);
}

/** @param text Bytes copied immediately. @param options Owning policy. @return Owning UI request builder. */
inline ToastMessage toast(std::string_view text, ToastOptions options = {}) {
  return toast(TextValue(text), options);
}

/** @brief Presentation-only severity stage; retains all other owning request policy. */
struct ToastSeverityModifier {
  ToastSeverity severity; ///< Existing presentation role.

  /** @param message Consumed request. @param modifier Last severity selection. @return Owning request. */
  friend ToastMessage operator|(ToastMessage &&message, ToastSeverityModifier modifier) {
    switch (modifier.severity) {
    case ToastSeverity::INFO:
      return std::move(message).info();
    case ToastSeverity::SUCCESS:
      return std::move(message).success();
    case ToastSeverity::WARNING:
      return std::move(message).warning();
    case ToastSeverity::FAILURE:
      return std::move(message).error();
    }

    return std::move(message);
  }
};

/** @return INFO presentation stage, equivalent to ToastMessage::info(). */
inline ToastSeverityModifier info() {
  return {ToastSeverity::INFO};
}

/** @return SUCCESS presentation stage, equivalent to ToastMessage::success(). */
inline ToastSeverityModifier success() {
  return {ToastSeverity::SUCCESS};
}

/** @return WARNING presentation stage, equivalent to ToastMessage::warning(). */
inline ToastSeverityModifier warning() {
  return {ToastSeverity::WARNING};
}

/** @return FAILURE presentation stage, equivalent to ToastMessage::error(). */
inline ToastSeverityModifier error() {
  return {ToastSeverity::FAILURE};
}

/** @brief Copyable weak UI-affine semantic handle; the scene owns all queue, action and timer storage.
 * Construction may allocate. A cold/closed handle returns CLOSED; a removed original center returns
 * STALE_NODE and never retargets a replacement. Commands report once through the existing collector.
 * Checked adoption preserves prior state while its owner and services remain live. Independent removal,
 * host shutdown or the existing diagnostic STOP policy can cancel registrations; commands never revive
 * stopped services. A later presentation failure retains admitted data and follows the existing STOP policy.
 */
class NotificationCenter {
public:
  /** @brief Construct an unpublished observing handle on the UI thread; may allocate. */
  NotificationCenter();
  /** @param message Owning UI request borrowed only during this call; retained data and the callable are copied before
   * adoption.
   * @param origin Public call site.
   * @return Exact checked receipt or full error; technical errors are reported once to a surviving collector.
   * Pre-adoption allocation, text, style and timer failures preserve the old queue, identities and timer.
   * Explicit policy drops are successful DROPPED receipts. Direct worker calls return WRONG_THREAD.
   */
  std::expected<ToastPostReceipt, ManagedError>
  post(const ToastMessage &message, std::source_location origin = std::source_location::current()) const;
  /** @param message Owning request borrowed for synchronous adoption. @param origin Public call site.
   * @return Checked short result, discarding the policy receipt; ignored DROPPED is intentionally silent.
   */
  DialogCommandResult show(const ToastMessage &message,
                           std::source_location origin = std::source_location::current()) const;
  /** @param text Owning expression. @param origin Public call site. @return Checked short result. */
  DialogCommandResult show(const TextValue &text, std::source_location origin = std::source_location::current()) const;
  /** @param text Bytes copied immediately. @param origin Public call site. @return Checked short result. */
  DialogCommandResult show(std::string_view text, std::source_location origin = std::source_location::current()) const;
  /** @param id Original identity; foreign centers return WRONG_SESSION. @param origin Public call site. @return Checked
   * removal. */
  DialogCommandResult dismiss(ToastId id, std::source_location origin = std::source_location::current()) const;
  /** @param origin Public call site. @return Success, including no eligible notification. */
  DialogCommandResult dismissLatest(std::source_location origin = std::source_location::current()) const;
  /** @param id Original action identity. @param origin Public call site. @return Checked once-only admission; no
   * automatic retry. */
  DialogCommandResult activate(ToastId id, std::source_location origin = std::source_location::current()) const;
  /** @param origin Public call site. @return Success, including no eligible action. */
  DialogCommandResult activateLatest(std::source_location origin = std::source_location::current()) const;
  /** @param origin Public call site. @return Checked clear without semantic callbacks. */
  DialogCommandResult clear(std::source_location origin = std::source_location::current()) const;
  /** @param visible Explicit center visibility; hide cancels timers immediately. @param origin Public call site.
   * @return Checked change or equal no-op. */
  DialogCommandResult setVisible(bool visible, std::source_location origin = std::source_location::current()) const;
  /** @param reduced Disable effects while retaining static content. @param origin Public call site. @return Checked
   * change or equal no-op. */
  DialogCommandResult setReducedMotion(bool reduced,
                                       std::source_location origin = std::source_location::current()) const;
  /** @return Independent owning observation or exact error; never reports, lays out or mutates UI. */
  std::expected<std::shared_ptr<const NotificationSnapshot>, ManagedError> snapshot() const;

private:
  friend struct detail::NotificationAccess;
  std::shared_ptr<detail::NotificationTarget> target_;
};

/// @cond INTERNAL
namespace detail {
struct NotificationAccess {
  static std::expected<void, ManagedError> reserve(const NotificationCenter &center);
  static void release(const NotificationCenter &center) noexcept;
  static void publish(const NotificationCenter &center,
                      const std::shared_ptr<portals::detail::NotificationRecord> &record) noexcept;
  static std::expected<std::shared_ptr<portals::detail::NotificationRecord>, ManagedError>
  lock(const NotificationCenter &center);
  static DialogCommandResult result(std::expected<void, ManagedError> value);
  static void report(const NotificationCenter &center, ManagedError &failure, const char *operation,
                     std::source_location origin) noexcept;
  static DialogCommandResult command(const NotificationCenter &center, portals::detail::ToastCommand command,
                                     std::optional<ToastId> id, bool flag, std::source_location origin);
};
} // namespace detail

/// @endcond
} // namespace ui
} // namespace portals
