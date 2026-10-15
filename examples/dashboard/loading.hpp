// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <array>
#include <portals/model.hpp>
#include <vector>

namespace dashboard {
/** @brief Synthetic page availability budgets; payload charging excludes allocator overhead and retained pins. */
struct LoadingOptions {
  std::size_t pageRows{256};          ///< Logical rows charged per page, including a short final page.
  std::size_t cachePages{8};          ///< Maximum availability records, including the pinned initial page.
  std::size_t cacheBytes{256 * 1024}; ///< Maximum charged row payload in one generated snapshot.
  std::size_t backendJobs{4};         ///< Maximum simultaneous delayed page jobs.
  std::size_t prefetchPages{1};       ///< Maximum adjacent pages per consumer in its navigation direction.
};

/** @brief Observable page probes, completed work and bounded availability high water. */
struct LoadingMetrics {
  std::uint64_t cacheHits{};      ///< Demand page probes already ready.
  std::uint64_t cacheMisses{};    ///< Demand page probes not yet ready.
  std::uint64_t usefulLoads{};    ///< Page jobs completed while covering current demand.
  std::uint64_t cancelledLoads{}; ///< Active jobs cancelled by demand replacement or stop.
  std::size_t cachedPages{};      ///< Current bounded availability records, including queued jobs.
  std::size_t activeJobs{};       ///< Current delayed jobs with a fixed deadline.
  std::size_t chargedBytes{};     ///< Conservative payload charge for current availability records.
  std::size_t peakPages{};        ///< Maximum availability records during this run.
  std::size_t peakJobs{};         ///< Maximum simultaneous delayed jobs during this run.
  std::size_t peakBytes{};        ///< Maximum conservative payload charge during this run.
};

/** @brief UI-only synthetic availability policy independent of replaceable ModelRequest generations.
 * Loaded logical rows survive market revisions; actual values must be regenerated from each immutable pin.
 * Owns no rows, widgets, callbacks or scheduler tokens. Two consumers match the dashboard table and chart.
 */
class PageLoading {
public:
  /** @param options Owning finite page, payload and work budgets.
   * @param rowBytes Payload accounting unit for one materialized row, in bytes; must be positive.
   * @param rows Logical model size, which does not allocate rows.
   * @param delayMs Backend latency in application clock milliseconds.
   * @throws std::invalid_argument For zero budgets, payload multiplication overflow or less than one charged page.
   */
  PageLoading(LoadingOptions options, std::size_t rowBytes, std::uint64_t rows, std::uint64_t delayMs)
      : options_(options), rows_(rows), delayMs_(delayMs) {
    if (!options.pageRows || !options.cachePages || !options.backendJobs || !rowBytes || options.pageRows > std::numeric_limits<std::size_t>::max() / rowBytes) {
      throw std::invalid_argument("invalid page loading budget");
    }

    pageBytes_ = options.pageRows * rowBytes;
    capacity_ = std::min(options.cachePages, options.cacheBytes / pageBytes_);

    if (!capacity_) {
      throw std::invalid_argument("page cache cannot hold one charged page");
    }

    if (rows_) {
      pages_.push_back({0, true, {}});
    }

    observe();
  }

  /** @param consumer Table (0) or chart (1) demand slot.
   * @param range Valid logical interval; invalid or over-budget intervals are not admitted.
   * @param now Current application clock in milliseconds; admitted jobs retain this start deadline.
   * @throws std::out_of_range If consumer is neither table nor chart.
   */
  void setDemand(std::size_t consumer, portals::RowRange range, std::uint64_t now) {
    if (stopped_) {
      return;
    }

    auto &demand = demands_.at(consumer);

    if (demand.range.count && range.first != demand.range.first) {
      demand.backwards = range.first < demand.range.first;
    }

    demand.range = range;
    reconcile(now);
  }

  /** @param now Application clock in milliseconds.
   * @return True once when at least one job becomes ready; idle polling does not request publication.
   */
  bool advance(std::uint64_t now) {
    if (stopped_) {
      return false;
    }

    bool changed{};

    for (auto &page : pages_) {
      if (!page.readyAt || now < *page.readyAt) {
        continue;
      }

      page.ready = true;
      page.readyAt.reset();
      metrics_.usefulLoads += demanded(page.index);
      changed = true;
    }

    startJobs(now);
    observe();

    return changed;
  }

  /** @param consumer Table (0) or chart (1) demand slot.
   * @return True if its complete page coverage fits the selected bounded working set.
   */
  bool admitted(std::size_t consumer) const {
    return demands_.at(consumer).admitted;
  }

  /** @param range Checked logical interval to probe without starting work.
   * @return True when every covering page is ready; invalid intervals return false.
   */
  bool loaded(portals::RowRange range) const {
    if (!valid(range)) {
      return false;
    }

    if (!range.count) {
      return true;
    }

    const auto last = lastPage(range);

    for (auto index = range.first / options_.pageRows;; ++index) {
      const auto found = find(index);

      if (found == pages_.end() || !found->ready) {
        return false;
      }

      if (index == last) {
        return true;
      }
    }
  }

  /** @return Owning sorted contiguous ready intervals, bounded by charged cache capacity. */
  std::vector<portals::RowRange> readyRanges() const {
    std::vector<portals::RowRange> result;

    for (const auto &page : pages_) {
      if (!page.ready) {
        continue;
      }

      const auto first = page.index * options_.pageRows;
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(options_.pageRows, rows_ - first));

      if (!result.empty() && result.back().first + result.back().count == first) {
        result.back().count += count;
      } else {
        result.push_back({first, count});
      }
    }

    return result;
  }

  /** @return Borrowed UI-only counters valid until this policy is destroyed. */
  const LoadingMetrics &metrics() const noexcept {
    return metrics_;
  }

  /** @brief Cancel and release all availability and queued work; subsequent advance stays idle. */
  void stop() {
    for (const auto &page : pages_) {
      metrics_.cancelledLoads += page.readyAt.has_value();
    }

    pages_.clear();
    desired_.clear();
    queuedPages_.clear();
    demands_ = {};
    stopped_ = true;
    observe();
  }

private:
  struct Demand {
    portals::RowRange range{};
    bool backwards{};
    bool admitted{};
  };

  struct Page {
    std::uint64_t index{};
    bool ready{};
    std::optional<std::uint64_t> readyAt;
  };

  bool valid(portals::RowRange range) const {
    return range.first <= rows_ && range.count <= rows_ - range.first;
  }

  std::uint64_t lastPage(portals::RowRange range) const {
    return (range.first + range.count - 1) / options_.pageRows;
  }

  std::vector<Page>::const_iterator find(std::uint64_t index) const {
    return std::ranges::find(pages_, index, &Page::index);
  }

  bool desired(std::uint64_t index) const {
    return std::ranges::find(desired_, index) != desired_.end();
  }

  bool demanded(std::uint64_t index) const {
    return std::ranges::any_of(demands_, [&](const auto &demand) {
      return demand.admitted && demand.range.count && index >= demand.range.first / options_.pageRows && index <= lastPage(demand.range);
    });
  }

  void append(std::uint64_t index) {
    if (!desired(index)) {
      desired_.push_back(index);
    }
  }

  void chooseDemand() {
    desired_.clear();

    if (rows_) {
      append(0); // The initial row carries the immutable aggregate shared by table and chart preparation.
    }

    for (auto &demand : demands_) {
      demand.admitted = valid(demand.range);

      if (!demand.admitted || !demand.range.count) {
        continue;
      }

      const auto first = demand.range.first / options_.pageRows;
      const auto last = lastPage(demand.range);
      const auto pageCount = last - first + 1;

      if (pageCount > capacity_) {
        demand.admitted = false;

        continue;
      }

      auto missing = pageCount;

      for (auto index : desired_) {
        missing -= index >= first && index <= last;
      }

      if (missing > capacity_ - desired_.size()) {
        demand.admitted = false;

        continue;
      }

      for (auto index = first;; ++index) {
        const auto found = find(index);

        metrics_.cacheHits += found != pages_.end() && found->ready;
        metrics_.cacheMisses += found == pages_.end() || !found->ready;
        append(index);

        if (index == last) {
          break;
        }
      }
    }
  }

  void choosePrefetch() {
    for (const auto &demand : demands_) {
      if (!demand.admitted || !demand.range.count) {
        continue;
      }

      auto index = demand.backwards ? demand.range.first / options_.pageRows : lastPage(demand.range);
      const auto last = (rows_ - 1) / options_.pageRows;

      for (std::size_t count = 0; count < options_.prefetchPages && desired_.size() < capacity_; ++count) {
        if ((demand.backwards && !index) || (!demand.backwards && index == last)) {
          break;
        }

        demand.backwards ? --index : ++index;
        append(index);
      }
    }
  }

  void reconcile(std::uint64_t now) {
    chooseDemand();
    choosePrefetch();

    std::erase_if(pages_, [&](const auto &page) {
      if (page.ready || desired(page.index)) {
        return false;
      }

      metrics_.cancelledLoads += page.readyAt.has_value();

      return true;
    });

    std::erase_if(queuedPages_, [&](auto index) {
      return find(index) == pages_.end();
    });

    for (auto index : desired_) {
      if (find(index) != pages_.end()) {
        continue;
      }

      if (pages_.size() == capacity_) {
        const auto victim = std::ranges::find_if(pages_, [&](const auto &page) {
          return !desired(page.index);
        });

        pages_.erase(victim);
      }

      pages_.push_back({index, false, {}});
      queuedPages_.push_back(index);
    }

    std::ranges::sort(pages_, {}, &Page::index);
    startJobs(now);
    observe();
  }

  void startJobs(std::uint64_t now) {
    auto active = static_cast<std::size_t>(std::ranges::count_if(pages_, [](const auto &page) {
      return page.readyAt.has_value();
    }));

    // Preserve FIFO among queued demand pages so a moving consumer cannot starve its stable neighbour.
    // The queue contains only bounded availability identities, without an ever-increasing age counter.
    while (active < options_.backendJobs && !queuedPages_.empty()) {
      auto queued = std::ranges::find_if(queuedPages_, [&](auto index) {
        return demanded(index);
      });

      if (queued == queuedPages_.end()) {
        queued = queuedPages_.begin();
      }

      auto page = std::ranges::find(pages_, *queued, &Page::index);

      page->readyAt = now + std::min(delayMs_, std::numeric_limits<std::uint64_t>::max() - now);
      queuedPages_.erase(queued);
      ++active;
    }
  }

  void observe() {
    metrics_.cachedPages = pages_.size();
    metrics_.activeJobs = static_cast<std::size_t>(std::ranges::count_if(pages_, [](const auto &page) {
      return page.readyAt.has_value();
    }));
    metrics_.chargedBytes = pages_.size() * pageBytes_;
    metrics_.peakPages = std::max(metrics_.peakPages, metrics_.cachedPages);
    metrics_.peakJobs = std::max(metrics_.peakJobs, metrics_.activeJobs);
    metrics_.peakBytes = std::max(metrics_.peakBytes, metrics_.chargedBytes);
  }

  LoadingOptions options_;
  std::uint64_t rows_, delayMs_;
  std::size_t pageBytes_{}, capacity_{};
  std::array<Demand, 2> demands_;
  std::vector<Page> pages_;
  std::vector<std::uint64_t> desired_;
  std::vector<std::uint64_t> queuedPages_;
  LoadingMetrics metrics_;
  bool stopped_{};
};
} // namespace dashboard
