// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../examples/dashboard/provider.hpp"
#include <doctest.h>

namespace {
using portals::RowRange;
constexpr std::size_t ROW_BYTES = sizeof(portals::ModelRow<dashboard::Quote>);

dashboard::LoadingOptions smallCache() {
  dashboard::LoadingOptions options;

  options.pageRows = 16;
  options.cachePages = 4;
  options.cacheBytes = options.pageRows * options.cachePages * ROW_BYTES;
  options.backendJobs = 1;
  options.prefetchPages = 1;

  return options;
}
} // namespace

TEST_CASE("useful page deadline survives shifted demand and jobs are bounded") {
  dashboard::PageLoading loading(smallCache(), ROW_BYTES, 1000, 250);

  loading.setDemand(0, {160, 5}, 0);
  loading.setDemand(0, {165, 5}, 100);
  CHECK_FALSE(loading.advance(249));
  CHECK(loading.advance(250));
  CHECK(loading.loaded({165, 5}));
  CHECK(loading.metrics().usefulLoads == 1);
  CHECK(loading.metrics().peakJobs == 1);
  CHECK(loading.metrics().peakPages <= 4);
  CHECK(loading.metrics().peakBytes <= smallCache().cacheBytes);
  CHECK_FALSE(loading.advance(251));
}

TEST_CASE("adjacent loaded pages expose a single range across boundaries") {
  auto options = smallCache();

  options.backendJobs = 2;
  options.prefetchPages = 0;

  dashboard::PageLoading loading(options, ROW_BYTES, 1000, 20);

  loading.setDemand(0, {175, 4}, 0);
  CHECK(loading.advance(20));
  CHECK(loading.loaded({175, 4}));

  const auto ranges = loading.readyRanges();

  CHECK(ranges.size() == 2);
  CHECK(ranges[1] == RowRange{160, 32});
}

TEST_CASE("direction changes retain cache hits and cancel obsolete bounded jobs") {
  dashboard::PageLoading loading(smallCache(), ROW_BYTES, 1000, 50);

  loading.setDemand(0, {160, 5}, 0);
  CHECK(loading.advance(50));
  loading.setDemand(0, {176, 5}, 50);
  CHECK(loading.advance(100));
  loading.setDemand(0, {160, 5}, 100);
  CHECK(loading.loaded({160, 5}));
  CHECK(loading.metrics().cacheHits > 0);
  CHECK(loading.metrics().cacheMisses > 0);
  loading.setDemand(0, {500, 5}, 101);
  CHECK(loading.metrics().cancelledLoads > 0);
  loading.stop();
  CHECK(loading.metrics().activeJobs == 0);
  CHECK(loading.metrics().cachedPages == 0);
  CHECK(loading.metrics().chargedBytes == 0);
  loading.setDemand(0, {160, 5}, 1000);
  CHECK_FALSE(loading.advance(1000));
  CHECK(loading.metrics().cachedPages == 0);
}

TEST_CASE("budget overflow is admitted as failure instead of unbounded queued work") {
  auto options = smallCache();

  options.cachePages = 2;
  options.prefetchPages = 0;

  dashboard::PageLoading loading(options, ROW_BYTES, 1000, 10);

  loading.setDemand(0, {160, 40}, 0);
  CHECK_FALSE(loading.admitted(0));
  CHECK(loading.metrics().cachedPages == 1);
  CHECK(loading.metrics().activeJobs == 0);
  loading.setDemand(0, {999, 1}, 0);
  CHECK(loading.admitted(0));
  CHECK(loading.advance(10));
  CHECK(loading.loaded({999, 1}));
  CHECK(loading.readyRanges().back() == RowRange{992, 8});
}

TEST_CASE("byte accounting bounds effective pages and rejects invalid arithmetic") {
  auto options = smallCache();

  options.cacheBytes = options.pageRows * ROW_BYTES * 2;
  options.prefetchPages = 1000;

  dashboard::PageLoading loading(options, ROW_BYTES, 1000, 10);

  loading.setDemand(0, {160, 5}, 0);
  CHECK(loading.advance(10));
  CHECK(loading.metrics().peakPages <= 2);
  CHECK(loading.metrics().peakBytes <= options.cacheBytes);
  loading.setDemand(0, {std::numeric_limits<std::uint64_t>::max(), 10}, 10);
  CHECK_FALSE(loading.admitted(0));
  options.pageRows = std::numeric_limits<std::size_t>::max();
  CHECK_THROWS_AS(dashboard::PageLoading(options, ROW_BYTES, 1000, 10), std::invalid_argument);
}

TEST_CASE("repeated distant navigation keeps payload, queued pages and work bounded") {
  const auto options = smallCache();
  dashboard::PageLoading loading(options, ROW_BYTES, 10000, 250);

  for (std::uint64_t step = 0; step < 1000; ++step) {
    const auto now = step * 17;

    loading.setDemand(0, {(step * 37) % 9900, 5}, now);
    loading.setDemand(1, {(step * 41) % 9900, 7}, now);
    loading.advance(now);

    const auto &metrics = loading.metrics();
    const auto ranges = loading.readyRanges();
    std::size_t readyRows{};

    for (const auto range : ranges) {
      readyRows += range.count;
    }

    CHECK(metrics.cachedPages <= options.cachePages);
    CHECK(metrics.activeJobs <= options.backendJobs);
    CHECK(metrics.chargedBytes <= options.cacheBytes);
    CHECK(readyRows * ROW_BYTES <= options.cacheBytes);
  }

  CHECK(loading.metrics().cancelledLoads > 0);
}

TEST_CASE("stable chart demand finishes while table continually replaces its page with one backend job") {
  auto options = smallCache();

  options.prefetchPages = 0;

  dashboard::PageLoading loading(options, ROW_BYTES, 10000, 250);

  loading.setDemand(0, {160, 1}, 0);
  loading.setDemand(1, {800, 1}, 0);

  for (std::uint64_t step = 1; step <= 10; ++step) {
    const auto now = step * 100;

    loading.setDemand(0, {160 + 16 * step, 1}, now);
    loading.advance(now);
  }

  CHECK(loading.loaded({800, 1}));
  CHECK(loading.metrics().peakJobs == 1);
}

TEST_CASE("logical row and clock endpoints do not wrap page work arithmetic") {
  const auto endpoint = std::numeric_limits<std::uint64_t>::max();
  dashboard::PageLoading loading(smallCache(), ROW_BYTES, endpoint, 250);

  loading.setDemand(0, {endpoint - 4, 4}, endpoint - 1);
  CHECK(loading.admitted(0));
  CHECK_FALSE(loading.advance(endpoint - 1));
  CHECK(loading.advance(endpoint));
  CHECK(loading.loaded({endpoint - 4, 4}));
  CHECK(loading.readyRanges().back().count == 15);
}

TEST_CASE("slow source remains finite and idle polls stop reporting new readiness") {
  dashboard::PageLoading loading(smallCache(), ROW_BYTES, 1000, 2000);

  loading.setDemand(0, {175, 4}, 0);
  CHECK_FALSE(loading.advance(1999));
  CHECK(loading.advance(2000));
  CHECK_FALSE(loading.loaded({175, 4}));
  CHECK(loading.advance(4000));
  CHECK(loading.loaded({175, 4}));
  CHECK(loading.advance(6000));
  CHECK_FALSE(loading.advance(8000));
  CHECK_FALSE(loading.advance(10000));
  CHECK(loading.metrics().peakJobs == 1);
}

TEST_CASE("page availability regenerates exact immutable revision payloads and rejects stale completion") {
  using namespace portals;
  using example::take;

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  const auto root = take(tree->root());
  const std::array<RowRange, 1> initial{{{0, 16}}};
  auto model = take(Model<dashboard::Quote>::create(
    *tree, root, std::make_shared<const dashboard::Snapshot>(1, 1000, Ohlc{100, 100, 100, 100}, initial)));
  auto consumer = take(model->openRange());
  auto old = take(consumer.request({160, 4}));
  dashboard::PageLoading loading(smallCache(), ROW_BYTES, 1000, 50);

  loading.setDemand(0, old.range(), 0);
  take(model->publisher().publish(
    std::make_shared<const dashboard::Snapshot>(2, 1000, Ohlc{101, 104, 100, 103}, initial)));
  take(scheduler->dispatch());
  CHECK(old.stopRequested());

  auto current = take(consumer.request({165, 4}));

  loading.setDemand(0, current.range(), 20);
  CHECK(loading.advance(50));

  const auto ranges = loading.readyRanges();
  auto page = std::make_shared<const dashboard::Snapshot>(2, 1000, Ohlc{101, 104, 100, 103}, ranges);
  const auto stale = old.complete(page);

  CHECK_FALSE(stale);
  CHECK(stale.error() == SchedulerError::CLOSED);
  const auto wrongRevision =
    current.complete(std::make_shared<const dashboard::Snapshot>(1, 1000, Ohlc{100, 100, 100, 100}, ranges));

  CHECK_FALSE(wrongRevision);
  CHECK(wrongRevision.error() == SchedulerError::INVALID_ARGUMENT);

  const auto wrongCount =
    current.complete(std::make_shared<const dashboard::Snapshot>(2, 999, Ohlc{101, 104, 100, 103}, ranges));

  CHECK_FALSE(wrongCount);
  CHECK(wrongCount.error() == SchedulerError::INVALID_ARGUMENT);
  take(current.complete(page));
  take(scheduler->dispatch());

  const auto result = take(take(model->pin()).range(consumer.id()));
  const auto offset = static_cast<double>((165 * 37) % 101) / 10.;

  CHECK(result.state() == RangeState::READY);
  CHECK(result.rows()[0].value.prices.close == 103 + offset);
  CHECK(result.rows()[0].id == RowId{166, 1});
}

TEST_CASE("completion backpressure retries and context retirement suppresses delivery") {
  using namespace portals;
  using example::take;

  SchedulerOptions options;

  options.data_capacity = 1;

  auto scheduler = take(Scheduler::create(options));
  auto tree = take(WidgetTree::create(*scheduler));
  const auto root = take(tree->root());
  const std::array<RowRange, 1> ranges{{{160, 16}}};
  auto page = std::make_shared<const dashboard::Snapshot>(1, 1000, Ohlc{100, 100, 100, 100}, ranges);
  auto model = take(Model<dashboard::Quote>::create(*tree, root, page));
  auto table = take(model->openRange());
  auto chart = take(model->openRange());
  auto first = take(table.request({160, 4}));
  auto second = take(chart.request({160, 4}));

  take(first.complete(page));

  const auto rejected = second.complete(page);

  CHECK_FALSE(rejected);
  CHECK(rejected.error() == SchedulerError::WOULD_BLOCK);
  take(scheduler->dispatch());
  take(second.complete(page));
  take(scheduler->dispatch());
  CHECK(take(take(model->pin()).range(chart.id())).state() == RangeState::READY);

  auto hidden = take(table.request({160, 4}));

  hidden.cancel();
  CHECK(hidden.complete(page).error() == SchedulerError::CLOSED);

  auto removed = take(chart.request({160, 4}));

  take(tree->remove(root));
  CHECK(removed.complete(page).error() == SchedulerError::CLOSED);
}

TEST_CASE("undersized demand yields existing failed range with owning error storage") {
  using namespace portals;
  using example::take;

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  const auto root = take(tree->root());
  auto model = take(Model<dashboard::Quote>::create(
    *tree, root, std::make_shared<const dashboard::Snapshot>(1, 1000, Ohlc{}, std::span<const RowRange>{})));
  auto consumer = take(model->openRange());
  auto request = take(consumer.request({160, 40}));
  auto page = std::make_shared<const dashboard::Snapshot>(1, 1000, Ohlc{}, std::span<const RowRange>{}, true);

  take(request.complete(page));
  take(scheduler->dispatch());

  const auto result = take(take(model->pin()).range(consumer.id()));

  CHECK(result.state() == RangeState::FAILED);
  CHECK(result.error() == "requested range exceeds page cache budget");
  page.reset();
  model.reset();
  CHECK(result.error() == "requested range exceeds page cache budget");
}
