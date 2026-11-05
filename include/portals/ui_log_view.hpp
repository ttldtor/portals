// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/log_view.hpp>
#include <portals/ui_description.hpp>

namespace portals {
struct LogLine;
class FileLogModel;
class SourceBudget;
struct FileLogOptions;
enum class SourceError;
template <class T> class WindowProvider;
} // namespace portals

namespace portals::ui {
template <class T> class Description;
template <class T> class Ref;

namespace detail {
struct LogSourceData;
struct ExternalLogAccess;
} // namespace detail

/** @brief Positive external-source display caps; independent of rolling-log append limits. */
struct LogSourceLimits {
  std::size_t maxRecords{512};           ///< Maximum logical records retained in one local prepared window.
  std::size_t maxBytes{1024 * 1024};     ///< Maximum retained record payload bytes before display expansion.
  std::size_t maxRecordBytes{64 * 1024}; ///< Maximum single source record, including rich finite strings.
};

/** @brief External log presentation and native navigation settings. */
struct LogSourceViewConfig {
  LogSourceLimits limits{}; ///< Bounded display payload caps, additionally constrained by the shared source ledger.
  bool followTail{true};    ///< Start at and follow the available tail when the provider supports it.
  bool readOnly{};          ///< Disable user navigation; checked programmatic navigation remains available.
};

/** @brief Owning deferred external log binding; construction performs no file I/O.
 * Copies share the provider and budget, while each mounted view owns its navigation.
 */
class LogSource {
public:
  LogSource() = default; ///< Empty descriptor; mounting reports INVALID_ARGUMENT.
  /** @return Retained rich file model, or null for a generic provider or failed creation.
   * Include log_file.hpp to use its worker-only byte metadata and search operations.
   */
  std::shared_ptr<FileLogModel> fileModel() const noexcept;
  /** @return Exact finite logical-record count, or empty for cursor/file sources; never visual wrapped rows. */
  std::optional<std::uint64_t> rowCount() const noexcept;
  /** @return Shared source/prepared-text credit root, available after the first successful mount. */
  std::shared_ptr<SourceBudget> budget() const noexcept;

private:
  friend struct detail::ExternalLogAccess;

  explicit LogSource(std::shared_ptr<detail::LogSourceData> data, SourceError error = {})
      : data_(std::move(data)), error_(error) {
  }

  std::shared_ptr<detail::LogSourceData> data_;
  SourceError error_{};
};

/** @param path Owning copy of a UTF-8 native file path; opened only on an application worker.
 * @return Deferred source; allocation/path errors are reported by the mounted scene.
 */
LogSource logFile(std::string_view path);
/** @param path Owning UTF-8 path. @param options Explicit bounded file/model operations.
 * @param budget Shared source/preparation root. @return Deferred rich file source; no I/O occurs here.
 */
LogSource logFile(std::string_view path, const FileLogOptions &options, std::shared_ptr<SourceBudget> budget = {});
/** @param provider Worker-safe immutable-page provider; no load occurs here.
 * @param budget Shared source/prepared-text ledger; null creates one during mount.
 * @return Owning deferred source, including exact deferred setup failures.
 */
LogSource logSource(std::shared_ptr<WindowProvider<LogLine>> provider, std::shared_ptr<SourceBudget> budget = {});

namespace detail {
/// @cond INTERNAL
LogSource finiteLogSource(std::shared_ptr<WindowProvider<LogLine>> provider, std::uint64_t rows, std::uint64_t revision,
                          std::shared_ptr<SourceBudget> budget);
LogSource failedLogSource(SourceError error);
/// @endcond
} // namespace detail

/** @param reference Weak observation published only after complete mount success.
 * @param initial Complete UTF-8 batch copied into the recipe now.
 * @param config Positive resource/navigation settings; omitted scalar properties inherit existing defaults.
 * @param style Manual appearances beneath theme rules.
 * @return Owning native bounded-log recipe; invalid text/settings retain exact typed mount errors.
 */
Description<LogView> logView(Ref<LogView> reference, std::string_view initial = {}, LogViewConfig config = {},
                             ControlStyle style = {});
/** @param initial Complete UTF-8 batch copied now. @param config Bounded owning settings.
 * @param style Manual appearances. @return Native log recipe without an observing Ref.
 */
Description<LogView> logView(std::string_view initial = {}, LogViewConfig config = {}, ControlStyle style = {});
/** @param source Owning external source; I/O, text parsing and wrapping run on the application executor.
 * @param config Bounded presentation and navigation settings; source records are never appended to the rolling log.
 * @param style Manual appearances beneath theme rules.
 * @return Native external-log recipe with node-scoped cancellation and local-window navigation.
 */
Description<LogView> logView(LogSource source, LogSourceViewConfig config = {}, ControlStyle style = {});
/** @param reference Weak observation published after complete mount success.
 * @param source Deferred external source. @param config Presentation limits. @param style Manual appearances.
 * @return Native external-log recipe publishing the observing reference.
 */
Description<LogView> logView(Ref<LogView> reference, LogSource source, LogSourceViewConfig config = {},
                             ControlStyle style = {});
} // namespace portals::ui

namespace portals {
/** @param tree UI owner outside frames. @param node Mounted finite-source log.
 * @param ordinal Exact logical record position, in [0,rowCount()).
 * @return Deferred bounded seek admission or UNSUPPORTED for non-finite sources; no identity lookup is required.
 */
std::expected<void, SourceError> seekLogRow(WidgetTree &tree, Node<LogView> node, std::uint64_t ordinal);
/** @param tree UI owner outside frames. @param node Mounted file-source log.
 * @param byteOffset Original byte position of a record's first byte in the current file generation.
 * @return Deferred bounded seek admission or UNSUPPORTED for other source families.
 */
std::expected<void, SourceError> seekLogByte(WidgetTree &tree, Node<LogView> node, std::uint64_t byteOffset);
} // namespace portals
