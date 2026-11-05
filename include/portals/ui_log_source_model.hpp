// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/log_file.hpp>
#include <portals/model.hpp>
#include <portals/ui_log_view.hpp>

#include <concepts>

namespace portals::ui::detail {
/// @cond INTERNAL
template <class T, class Codec> class FiniteLogProvider final : public WindowProvider<LogLine> {
public:
  FiniteLogProvider(std::shared_ptr<const ModelSnapshot<T>> snapshot, Codec codec)
      : snapshot_(std::move(snapshot)), codec_(std::move(codec)) {
  }

  SourceCapabilities capabilities() const noexcept override {
    return {true, false, true};
  }

  std::expected<SourceReadReply<LogLine>, SourceError> load(const SourceReadCommand &command, PageBuilder<LogLine> &out,
                                                            std::stop_token stop) override {
    if (!snapshot_ || command.dataRevision != snapshot_->revision()) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    const auto total = snapshot_->rowCount();
    std::uint64_t position{};

    switch (command.anchor.kind) {
    case SourceAnchorKind::ENTRY:
      position = command.direction == SourceDirection::BACKWARD ? total : 0;
      break;
    case SourceAnchorKind::TAIL:
      position = total;
      break;
    case SourceAnchorKind::CURSOR:
      if (command.anchor.cursor.token[1] || command.anchor.cursor.token[0] > total) {
        return std::unexpected(SourceError::INVALID_CURSOR);
      }

      position = command.anchor.cursor.token[0];
      break;
    case SourceAnchorKind::ROW:
      return std::unexpected(SourceError::UNSUPPORTED);
    }

    const bool backwards =
      command.direction == SourceDirection::BACKWARD || command.anchor.kind == SourceAnchorKind::TAIL;
    const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(command.maxRows, backwards ? position : total - position));
    const auto first = backwards ? position - count : position;
    auto range = ModelRange<T>::read(snapshot_, {first, count}, command.maxRows);

    if (!range) {
      return std::unexpected(portals::detail::sourceTreeError(range.error()));
    }

    if (range->state() == RangeState::PENDING) {
      SourceReadReply<LogLine> pending;

      pending.observedRevision = command.dataRevision;

      return pending;
    }

    if (range->state() != RangeState::READY) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }

    try {
      // The throwing overload lets debug proxy allocation report workspace exhaustion.
      std::pmr::string text("", 0, out.workspace());

      for (const auto &row : range->rows()) {
        if (stop.stop_requested()) {
          return std::unexpected(SourceError::CANCELLED);
        }

        text.clear();

        std::invoke(codec_, row.value, text);

        auto added = out.emplace(row.id, snapshot_->revision(), 0, std::string_view(text), out.resource());

        if (!added) {
          return std::unexpected(added.error());
        }
      }
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::PAGE_TOO_LARGE);
    }

    const auto edge = (command.direction == SourceDirection::BACKWARD ? first == 0 : first + count == total)
                        ? SourceEdge::FINAL_END
                        : SourceEdge::MORE;

    return out.ready({command.source, {first, 0}}, {command.source, {first + count, 0}}, edge);
  }

private:
  std::shared_ptr<const ModelSnapshot<T>> snapshot_;
  Codec codec_;
};

/// @endcond
} // namespace portals::ui::detail

namespace portals::ui {
/** @brief Bind rich finite model records without copying domain storage or requiring locateRow().
 * @tparam T Immutable worker-readable domain record, including owning strings.
 * @tparam Codec Worker-safe callable accepting const T& and a bounded pmr::string& output.
 * @param snapshot Existing immutable finite model snapshot; retained through outstanding work.
 * @param codec Appends the display text to the supplied source-workspace string; must not retain it.
 * @param budget Shared ledger for delivered pages and prepared display text.
 * @return Deferred log source preserving exact rowCount and bounded positional read/seek.
 * Allocation failures are retained for scene setup. Formatting runs only on the application worker.
 */
template <class T, class Codec>
  requires std::invocable<Codec &, const T &, std::pmr::string &>
LogSource logSource(std::shared_ptr<const ModelSnapshot<T>> snapshot, Codec codec,
                    std::shared_ptr<SourceBudget> budget = {}) {
  if (!snapshot) {
    return {};
  }

  try {
    return detail::finiteLogSource(std::make_shared<detail::FiniteLogProvider<T, Codec>>(snapshot, std::move(codec)),
                                   snapshot->rowCount(), snapshot->revision(), std::move(budget));
  } catch (const std::bad_alloc &) {
    return detail::failedLogSource(SourceError::NO_MEMORY);
  } catch (...) {
    return detail::failedLogSource(SourceError::CALLBACK_FAILED);
  }
}

/** @tparam T Immutable string-like record convertible to string_view.
 * @param snapshot Worker-readable finite snapshot. @param budget Shared source/preparation ledger.
 * @return Deferred exact-position source using the record text directly.
 */
template <class T>
  requires std::convertible_to<const T &, std::string_view>
LogSource logSource(std::shared_ptr<const ModelSnapshot<T>> snapshot, std::shared_ptr<SourceBudget> budget = {}) {
  return logSource(
    std::move(snapshot),
    [](const T &value, std::pmr::string &out) {
      out.append(std::string_view(value));
    },
    std::move(budget));
}
} // namespace portals::ui
