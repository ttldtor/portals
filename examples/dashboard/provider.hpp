// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/common.hpp"
#include "../replay/data.hpp"
#include <array>
#include <portals/chart.hpp>
#include <portals/managed.hpp>
#include <ranges>

namespace dashboard {
using namespace portals;
using example::take;

/** @brief Provider workload, replay and display settings for one dashboard run. */
struct Options {
  /** @brief Borrowed callback sampling this scene's snapshot ingress latency in scheduler time units. */
  using IngressObserver = void (*)(void *, SchedulerTime);
  /** @brief Optional observer; when null, ingress performs no sampling work. */
  IngressObserver ingressObserver{};
  /** @brief Borrowed observer context, required to remain valid through scene shutdown. */
  void *ingressContext{};
  /** @brief Number of logical instruments exposed by the virtual model. */
  std::uint64_t rows{1000000};
  /** @brief Synthetic or replay ticks generated per second of application clock time. */
  std::uint32_t rate{1000};
  /** @brief Seed for deterministic synthetic prices. */
  std::uint64_t seed{42};
  /** @brief Maximum tick work in one provider pump after a clock jump. */
  std::size_t maxTicksPerPump{512};
  /** @brief Additional delay in milliseconds before a requested range becomes ready. */
  std::uint64_t delayMs{};
  /** @brief Optional run duration in milliseconds; zero leaves stopping to input or the caller. */
  std::uint64_t durationMs{};
  /** @brief Use the fixed replay prices instead of the synthetic generator. */
  bool replay{};
  /** @brief Select ASCII captions and chart glyphs for limited terminals. */
  bool ascii{};
  /** @brief Preserve the detailed legacy layout for direct Scene and benchmark consumers.
   * The shared CLI selects the compact showcase by default and exposes this layout through --stress.
   */
  bool stress{true};
};

struct Quote {
  std::uint64_t instrument;
  Ohlc prices;
};

// Each logical instrument follows the same lossless market aggregate with a fixed
// price offset. Pages own bounded prefetched ranges around the table and chart.
class Snapshot final : public ModelSnapshot<Quote> {
public:
  struct Page {
    RowRange range;
    std::vector<ModelRow<Quote>> rows;
  };

  Snapshot(std::uint64_t revision, std::uint64_t count, Ohlc aggregate, std::span<const RowRange> ranges)
      : ModelSnapshot(revision, count), aggregate(aggregate) {
    for (auto range : ranges) {
      range.first = std::min(range.first, count);
      range.count = static_cast<std::size_t>(std::min<std::uint64_t>(range.count, count - range.first));
      Page page{range, {}};
      page.rows.reserve(range.count);

      for (std::size_t i = 0; i < range.count; ++i) {
        const auto index = range.first + i;
        const double offset = static_cast<double>((index * 37) % 101) / 10.;
        page.rows.push_back(
          {{index + 1, 1},
           {index,
            {aggregate.open + offset, aggregate.high + offset, aggregate.low + offset, aggregate.close + offset}}});
      }

      pages_.push_back(std::move(page));
    }
  }

  ModelRows<Quote> read(RowRange range) const override {
    for (const auto &page : pages_) {
      if (range.first >= page.range.first && range.first - page.range.first <= page.range.count &&
          range.count <= page.range.count - static_cast<std::size_t>(range.first - page.range.first)) {
        return {RangeState::READY,
                std::span(page.rows).subspan(static_cast<std::size_t>(range.first - page.range.first), range.count),
                {}};
      }
    }

    return {};
  }

  RowLookup locateRow(RowId id) const override {
    return id.generation == 1 && id.value && id.value <= rowCount() ? RowLookup{RowLookupState::FOUND, id.value - 1}
                                                                    : RowLookup{RowLookupState::NOT_FOUND};
  }

  const Ohlc aggregate;

private:
  std::vector<Page> pages_;
};

inline double syntheticPrice(std::uint64_t index, std::uint64_t seed) noexcept {
  // Unsigned arithmetic defines this deterministic integer generator on every toolchain.
  auto value = index + seed * 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;

  return 100. + static_cast<double>((value ^ (value >> 31)) % 2001) / 100.;
}

inline std::expected<void, SchedulerError> forwardLatest(const Publisher<ModelSnapshot<Quote>> &publisher,
                                                         std::shared_ptr<const ModelSnapshot<Quote>> &pendingLatest) {
  if (!pendingLatest) {
    return {};
  }

  return publisher.publish(pendingLatest).transform([&](PublishResult) {
    pendingLatest.reset();
  });
}

/** @brief Synchronous bounded market provider on the existing UI scheduler.
 *
 * Owns requests, prefetch pages and ingress; it has no tree, router or widget dependency.
 *
 * Stationary on UI because scheduler callbacks borrow this object.
 */
class Provider {
public:
  /** @param app Borrowed runtime and clock; it outlives this UI-only provider.
   * @param options Owning workload and borrowed ingress observer, valid through stop().
   *
   * Scheduler setup failures use the example's fail-fast policy.
   */
  Provider(Application &app, Options options)
      : app_(app), options_(options), scope_(take(app.scheduler().makeScope())), started_(app.now()) {
  }

  Provider(const Provider &) = delete;
  Provider &operator=(const Provider &) = delete;
  Provider(Provider &&) = delete;
  Provider &operator=(Provider &&) = delete;

  /** @return Immutable initial page for one scene-owned model; no provider work is started. */
  std::shared_ptr<const Snapshot> initialSnapshot() const {
    return snapshot();
  }

  /** @param model Weak UI-affine handle to the receiving scene's model.
   *
   * Call once after UI setup. Ingress, timer and model completion use the same scheduler.
   */
  void start(ManagedModel<Quote> model) {
    model_ = model;

    ingress_ = take(app_.scheduler().connect<ModelSnapshot<Quote>>(
      scope_,
      [this](auto value) {
        if (options_.ingressObserver) {
          options_.ingressObserver(options_.ingressContext, app_.scheduler().metrics().data.last_latency);
        }

        pendingLatest_ = std::move(value);
        forward();
      },
      SchedulerLane::DATA, SchedulerOverflow::REPLACE_LATEST));

    schedule();
  }

  /** @param pin Shared immutable table/chart pin captured by managed preparation.
   * @param work New table interval, retained until completion or cancellation.
   * @return Success or the original error reading the common aggregate.
   */
  std::expected<void, TreeError> tablePrepared(const ModelFrame<Quote> &pin,
                                               const std::optional<ModelRequest<Quote>> &work) {
    return pin.read({0, 1}).transform([&](const auto &base) {
      // Chart preparation follows table preparation and shares this immutable aggregate.
      preparedAggregate_ = base.rows().empty() ? aggregate() : base.rows()[0].value.prices;
      accept(0, work, preparedAggregate_);
    });
  }

  /** @param work Optional chart interval from the same managed preparation pass as the table.
   *
   * Retains independent completion and the table pin's immutable aggregate.
   */
  void chartPrepared(const std::optional<ModelRequest<Quote>> &work) {
    accept(1, work, preparedAggregate_);
  }

  /** @brief Cancel provider work once, before the receiving managed scene retires its model.
   *
   * UI-only; subsequent calls preserve the first stop time and perform no additional work.
   * The benchmark uses this boundary independently of tree teardown.
   */
  void stop() {
    if (stoppedAt_) {
      return;
    }

    take(timer_.disconnect());

    for (auto &slot : requests_) {
      if (slot.request) {
        slot.request->cancel();
      }
    }

    pendingLatest_.reset();
    stoppedAt_ = app_.now();
  }

  /** @return Total logical instrument count, independently of bounded cached pages. */
  std::uint64_t rows() const noexcept {
    return options_.rows;
  }

  /** @return Domain ticks still waiting after a bounded pump or cancellation. */
  std::uint64_t pendingTicks() const noexcept {
    return scheduledTicks_ - ticks_;
  }

  /** @return First provider cancellation time in scheduler units, or empty before stop(). */
  std::optional<SchedulerTime> stoppedAt() const noexcept {
    return stoppedAt_;
  }

  /** @return Number of domain ticks accumulated before display snapshot coalescing. */
  std::uint64_t tickCount() const noexcept {
    return ticks_;
  }

  /** @return Number of domain ticks due according to the last observed application time. */
  std::uint64_t scheduledTicks() const noexcept {
    return scheduledTicks_;
  }

  /** @return Lossless aggregate, or the initial flat price before the first tick. */
  Ohlc aggregate() const noexcept {
    return bucket_.value().value_or(Ohlc{100, 100, 100, 100});
  }

private:
  static constexpr std::uint64_t PROVIDER_CADENCE_MS = 10;
  static constexpr std::size_t INITIAL_PAGE_ROWS = 256;
  static constexpr std::size_t PREFETCH_ROWS = INITIAL_PAGE_ROWS / 2;

  struct RequestSlot {
    std::optional<ModelRequest<Quote>> request;
    RowRange range{};
    std::uint64_t readyAt{};
    Ohlc aggregate{};
    bool acquired{};
  };

  std::shared_ptr<const Snapshot> snapshot() const {
    std::array<RowRange, 3> pages{
      {{0, static_cast<std::size_t>(std::min<std::uint64_t>(INITIAL_PAGE_ROWS, options_.rows))}}};
    std::size_t count = 1;

    for (const auto &slot : requests_) {
      if (slot.acquired) {
        pages[count++] = slot.range;
      }
    }

    return std::make_shared<const Snapshot>(ticks_ + 1, options_.rows, aggregate(), std::span(pages).first(count));
  }

  void accept(std::size_t index, const std::optional<ModelRequest<Quote>> &request, Ohlc aggregate) {
    if (!request) {
      return;
    }

    auto &slot = requests_[index];
    const auto range = request->range();
    const auto before = static_cast<std::size_t>(std::min<std::uint64_t>(range.first, PREFETCH_ROWS));
    const auto after =
      static_cast<std::size_t>(std::min<std::uint64_t>(options_.rows - range.first - range.count, PREFETCH_ROWS));
    const RowRange page{range.first - before, range.count + before + after};

    if (slot.range != page) {
      slot.readyAt = app_.now() + options_.delayMs;
      slot.range = page;
      slot.acquired = false;
    }

    if (slot.request) {
      slot.request->cancel();
    }

    slot.request = *request;
    slot.aggregate = aggregate;
  }

  void forward() {
    auto result = forwardLatest(take(model_.publisher()), pendingLatest_);

    if (!result && result.error() != SchedulerError::WOULD_BLOCK) {
      take(std::move(result));
    }
  }

  void serviceRanges() {
    // Reverse order deliberately exercises independent chart/table completions.

    for (auto &slot : requests_ | std::views::reverse) {
      if (!slot.request || app_.now() < slot.readyAt) {
        continue;
      }

      slot.acquired = true;
      const auto page = std::make_shared<const Snapshot>(slot.request->dataRevision(), options_.rows, slot.aggregate,
                                                         std::span(&slot.range, 1));
      auto completed = slot.request->complete(page);

      if (completed || completed.error() == SchedulerError::CLOSED) {
        slot.request.reset();
      } else if (completed.error() != SchedulerError::WOULD_BLOCK) {
        take(std::move(completed));
      }
    }
  }

  void pump() {
    const auto elapsed = app_.now() - started_;
    const auto target = (elapsed / 1000) * options_.rate + ((elapsed % 1000) * options_.rate) / 1000;
    scheduledTicks_ = target;
    const auto previous = ticks_;
    const auto batchEnd = ticks_ + std::min<std::uint64_t>(target - ticks_, options_.maxTicksPerPump);

    while (ticks_ < batchEnd) {
      const auto price = options_.replay ? example::REPLAY_PRICES[ticks_ % example::REPLAY_PRICES.size()]
                                         : syntheticPrice(ticks_, options_.seed);

      if (!bucket_.add(price)) {
        throw std::runtime_error("nonfinite generated tick");
      }

      ++ticks_;
    }

    serviceRanges();
    forward();

    if (ticks_ != previous || retrySnapshot_) {
      auto published = ingress_.publisher.publish(snapshot());
      retrySnapshot_ = !published && published.error() == SchedulerError::WOULD_BLOCK;

      if (!published && !retrySnapshot_) {
        take(std::move(published));
      }
    }

    if (options_.durationMs && elapsed >= options_.durationMs) {
      app_.requestStop();

      return;
    }

    schedule();
  }

  void schedule() {
    timer_ = take(app_.scheduler().at(scope_, app_.now() + PROVIDER_CADENCE_MS, [this] {
      pump();
    }));
  }

  Application &app_;
  Options options_;
  UiScope scope_;
  Subscription timer_;
  SchedulerConnection<ModelSnapshot<Quote>> ingress_;
  ManagedModel<Quote> model_;
  std::shared_ptr<const ModelSnapshot<Quote>> pendingLatest_;
  std::array<RequestSlot, 2> requests_;
  OhlcBucket bucket_;
  Ohlc preparedAggregate_{};
  std::uint64_t started_, ticks_{}, scheduledTicks_{};
  std::optional<SchedulerTime> stoppedAt_;
  bool retrySnapshot_{};
};
} // namespace dashboard
