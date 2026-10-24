// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/scroll_controls.hpp>
#include <variant>

namespace portals {
namespace detail {
struct LogViewState;
struct LogViewAccess;
} // namespace detail

/** @brief Positive plain-log admission and retention bounds; oversized settings are rejected. */
struct LogLimits {
  std::size_t maxLines{512};            ///< Retained records, including empty records; ceiling 8192.
  std::size_t maxBytes{256 * 1024};     ///< Retained UTF-8 payload without separators; ceiling 4 MiB.
  std::size_t maxBatchBytes{16 * 1024}; ///< Complete input batch including separators; ceiling 256 KiB.
  std::size_t maxRecordBytes{4 * 1024}; ///< One record excluding its separator; ceiling 64 KiB.
};

/** @brief Owning bounded-log settings; frame bars use ON_FRAME without a horizontal gutter. */
struct LogViewConfig {
  LogLimits limits{};    ///< Positive resource limits, checked before mounting or accepting input.
  bool followTail{true}; ///< Initially follow the latest visual row.
  bool readOnly{};       ///< Disable user navigation; programmatic append, clear and tail remain allowed.
};

/** @brief Exact native admission failure, retaining original UTF-8 diagnostics. */
using LogError = std::variant<TreeError, TextError>;

/** @brief Native log default: share available vertical space; explicit SizePolicy values remain literal. */
inline constexpr SizePolicy DEFAULT_LOG_VIEW_POLICY{.width = {}, .height = {.mode = SizeMode::FLEX}};

/** @brief Owning receipt for one atomic complete batch; drop-oldest is a successful outcome. */
struct LogAppendReceipt {
  std::size_t acceptedLines{}; ///< All parsed records, including records displaced by this same batch.
  std::size_t droppedLines{};  ///< Old and new records displaced by this batch.
  std::size_t retainedLines{}; ///< Records retained after commit.
  std::size_t retainedBytes{}; ///< Retained payload bytes after commit.
};

/** @brief Owning scalar observation; never retains record payload or widgets. */
struct LogStatus {
  std::size_t retainedLines{};    ///< Current record count.
  std::size_t retainedBytes{};    ///< Current UTF-8 payload bytes, excluding separators.
  std::uint64_t droppedLines{};   ///< Cumulative displaced records since clear.
  std::uint64_t newWhilePaused{}; ///< Accepted records since following was paused.
  bool followTail{true};          ///< Follow the latest visual row during preparation.
  bool droppedSaturated{};        ///< The displaced count reached its representable ceiling.
  bool newWhilePausedSaturated{}; ///< The paused count reached its representable ceiling.
};

/** @brief Native bounded plain-text log with whole-grapheme wrap and a stable reading anchor.
 * The tree exclusively owns records and prepared visible-row text. No background work, callback,
 * clipboard or record snapshots are retained. Use checked tree commands between frames on UI.
 * Construct through ManagedScene::logView or ui::logView: managed preparation and its scoped
 * scroll-intent observation are required. Raw WidgetTree emplacement alone is not a usable log;
 * commands and measurement return INVALID_ARGUMENT until managed initialization is present.
 * The native frame requires right and bottom sides for its scrollbar and status/tail action.
 * CRLF, CR and LF separate complete records; a final separator adds no phantom empty record.
 */
class LogView : public ScrollArea {
public:
  /** @param width Complete immutable text policy.
   * @param config Bounded retention and navigation settings.
   * @param style Manual appearances beneath the existing theme.
   * @param policy Existing layout constraints.
   * @throws std::bad_alloc On construction allocation; tree emplacement translates it.
   */
  explicit LogView(WidthPolicy width = {}, LogViewConfig config = {}, ControlStyle style = {},
                   SizePolicy policy = DEFAULT_LOG_VIEW_POLICY);
  /** @param width Immutable text policy. @param config Resource/navigation bounds.
   * @param style Manual appearances. @param policy Layout constraints.
   * @param frame Owning native frame options; right and bottom sides must remain enabled.
   * @throws std::bad_alloc On allocation before mounting.
   */
  LogView(WidthPolicy width, LogViewConfig config, ControlStyle style, SizePolicy policy, FrameOptions frame);
  /** @brief Destroy bounded owning content on UI after the last tree borrow. */
  ~LogView() override;
  /** @param available Offered outer extent. @return Cached prepared extent with existing frame minimum. */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param children Must be empty: the log paints its own bounded rows.
   * @return Cached content extent or INVALID_ARGUMENT for externally inserted children.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @param context Synchronous sink; all text was prepared before paint.
   * @return Existing frame/renderer error; no allocation, mutation or callback is performed here.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;

private:
  friend struct detail::LogViewAccess;
  std::unique_ptr<detail::LogViewState> log_;
};

/** @param limits Positive bounds, including maxRecordBytes <= maxBytes and maxBatchBytes.
 * @return Success or INVALID_ARGUMENT, without allocation or side effects.
 */
std::expected<void, TreeError> validateLogLimits(LogLimits limits) noexcept;
/** @param tree Borrowed UI owner, outside frames; managed preparation must remain attached.
 * @param node Managed initialized live log; raw WidgetTree emplacement returns INVALID_ARGUMENT.
 * @param bytes Complete borrowed UTF-8 batch; copied before successful return.
 * @return Atomic receipt or original tree/text error. Failed admission leaves all log data unchanged.
 * Empty input is a checked no-op. Later frame preparation errors do not undo a successful append.
 */
std::expected<LogAppendReceipt, LogError> appendLogBatch(WidgetTree &tree, Node<LogView> node, std::string_view bytes);
/** @param tree UI owner with managed preparation. @param node Managed initialized live log.
 * @return Success or tree error; clears records/counters, preserves ID history, resumes following.
 */
std::expected<void, TreeError> clearLog(WidgetTree &tree, Node<LogView> node);
/** @param tree UI owner with managed preparation. @param node Managed initialized live log.
 * @return Success or tree error; resumes following on the next ordinary prepared frame.
 */
std::expected<void, TreeError> followLogTail(WidgetTree &tree, Node<LogView> node);
/** @param tree UI owner with managed preparation. @param node Managed initialized live log.
 * @return Owning counters or exact tree/frame/affinity failure, without reporting or invalidation.
 */
std::expected<LogStatus, TreeError> logStatus(WidgetTree &tree, Node<LogView> node);
} // namespace portals
