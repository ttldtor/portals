// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/common.hpp"
#include "../replay/data.hpp"
#include <array>
#include <format>
#include <portals/chart.hpp>
#include <portals/grid.hpp>
#include <portals/line_edit.hpp>
#include <portals/style.hpp>
#include <portals/virtual_table.hpp>
#include <ranges>

namespace dashboard {
using namespace portals;
using example::take;

struct Options {
  // Borrowed for the scene lifetime. Samples only this dashboard's snapshot ingress;
  // no clocks, allocation or callback work occur when the observer is null.
  using IngressObserver = void (*)(void *, SchedulerTime);
  IngressObserver ingressObserver{};
  void *ingressContext{};
  std::uint64_t rows{1000000};
  std::uint32_t rate{1000};
  std::uint64_t seed{42};
  std::size_t maxTicksPerPump{512};
  std::uint64_t delayMs{};
  std::uint64_t durationMs{};
  bool replay{};
  bool ascii{};
};

struct Quote {
  std::uint64_t instrument;
  Ohlc prices;
};

// Each logical instrument follows the same lossless market aggregate with a fixed
// price offset. Pages own only the ranges displayed by the table and chart.
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

  auto accepted = publisher.publish(pendingLatest);

  if (!accepted) {
    return std::unexpected(accepted.error());
  }

  pendingLatest.reset();

  return {};
}

class Scene final : public ApplicationScene {
public:
  Scene(Application &app, Options options)
      : app_(app), options_(options), styles_(app.tree()), scope_(take(app.scheduler().makeScope())),
        started_(app.now()) {
    auto &tree = app.tree();
    grid_ = take(tree.emplace<Grid>(app.root()));
    title_ = take(tree.emplace<Label>(
      grid_, text(options.ascii ? "Portals | Tab focus | arrows select/scroll | F2 theme | F10 quit"
                                : "Portals \xe7\x95\x8c e\xcc\x81 | Tab focus | F2 theme | F10 quit")));
    editor_ = take(tree.emplace<LineEdit>(grid_));
    inspect_ = take(tree.emplace<Button>(grid_, text("Inspect (unchanged)")));
    status_ = take(tree.emplace<Label>(grid_, text("Starting market")));
    name_ = take(Property<std::string>::create(tree, grid_, "Market note", Invalidation::PAINT));
    editBinding_ = take(bindLineEdit(tree, app.router(), editor_, name_, [](LineEditResult) {
    }));
    inspectBinding_ = take(bindButton(app.router(), inspect_, [this] {
      take(name_->set(take(name_->snapshot())->value));
    }));
    auto initial = snapshot();
    model_ = take(Model<Quote>::create(tree, grid_, initial));
    const std::array<TableColumn<Quote>, 5> columns{{{{1, 1},
                                                      text("Instrument"),
                                                      13,
                                                      [this](const Quote &q) {
                                                        return text(std::format("SYM-{:07}", q.instrument));
                                                      }},
                                                     {{2, 1},
                                                      text("Open"),
                                                      12,
                                                      [this](const Quote &q) {
                                                        return text(std::format("{:.2f}", q.prices.open));
                                                      }},
                                                     {{3, 1},
                                                      text("High"),
                                                      12,
                                                      [this](const Quote &q) {
                                                        return text(std::format("{:.2f}", q.prices.high));
                                                      }},
                                                     {{4, 1},
                                                      text("Low"),
                                                      12,
                                                      [this](const Quote &q) {
                                                        return text(std::format("{:.2f}", q.prices.low));
                                                      }},
                                                     {{5, 1}, text("Close"), 12, [this](const Quote &q) {
                                                        return text(std::format("{:.2f}", q.prices.close));
                                                      }}}};
    VirtualTableOptions tableOptions;
    tableOptions.overscan_rows = 2;
    table_ =
      take(tree.emplace<VirtualTable<Quote>>(grid_, take(model_->openRange()), std::span(columns), tableOptions));

    ChartOptions chartOptions;
    chartOptions.mode = ChartMode::CANDLES;
    chart_ = take(tree.emplace<Chart<Quote>>(
      grid_, take(model_->openRange()),
      [](const Quote &quote) -> std::optional<Ohlc> {
        return quote.prices;
      },
      chartOptions));
    tableBinding_ = take(bindVirtualTable(app.router(), tree, table_));
    take(app.router().setFocus(table_));
    take(styles_.bind(title_, {.type = "Label"}));
    take(styles_.bind(editor_, {.type = "LineEdit"}));
    take(styles_.bind(inspect_, {.type = "Button"}));
    take(styles_.bind(status_, {.type = "Label"}));
    take(styles_.bind(table_, {.type = "Table"}));
    take(styles_.bind(chart_, {.type = "Chart"}));
    take(styles_.replace(modernTheme()));
    ingress_ = take(app.scheduler().connect<ModelSnapshot<Quote>>(
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

  std::expected<void, TreeError> prepare(Application &app, CellSize size) override {
    const auto body = std::max(0, size.height - HEADER_ROWS);
    const CellSize tableSize{size.width, (body + 1) / 2};
    const CellSize chartSize{size.width, body / 2};

    if (!lastSize_ || *lastSize_ != size) {
      GridOptions grid;
      grid.columns = {fixed(size.width)};
      grid.rows = {fixed(std::min(size.height, 1)),
                   fixed(std::clamp(size.height - 1, 0, 1)),
                   fixed(std::clamp(size.height - 2, 0, 1)),
                   fixed(std::clamp(size.height - 3, 0, 1)),
                   fixed(tableSize.height),
                   fixed(chartSize.height)};
      grid.cells = {{title_.id(), 0, 0},  {editor_.id(), 1, 0}, {inspect_.id(), 2, 0},
                    {status_.id(), 3, 0}, {table_.id(), 4, 0},  {chart_.id(), 5, 0}};
      take(setGridOptions(app.tree(), grid_, grid));
      lastSize_ = size;
    }

    const auto focused = take(app.router().focused());
    take(editBinding_->sync(focused));
    take(syncControl(app.tree(), inspect_, focused));
    // Exactly one model pin feeds both views. Each preparation retains the pin and
    // its own immutable range through Application's following layout/paint frame.
    const auto pin = take(model_->pin());
    const auto base = take(pin.read({0, 1}));
    const auto bar = base.rows().empty() ? aggregate() : base.rows()[0].value.prices;
    const auto tableWork = take(prepareVirtualTable(app.tree(), table_, pin, tableSize));
    std::uint64_t first{};
    take(app.tree().read(table_, [&](const auto &table) {
      first = table.firstRow();
      maxPreparedRows_ = std::max(maxPreparedRows_, table.preparedRange().count);
      pendingVisible_ = table.rangeState() == RangeState::PENDING;
    }));
    const auto chartWork = take(prepareChart(app.tree(), chart_, pin, chartSize, first));
    accept(0, tableWork, bar);
    accept(1, chartWork, bar);
    std::uint64_t chartRevision{};
    take(app.tree().read(chart_, [&](const auto &chart) {
      chartRevision = chart.dataRevision();
    }));
    coherent_ = coherent_ && chartRevision == pin.dataRevision();
    lastRevision_ = pin.dataRevision();
    take(setControlText(
      app.tree(), status_,
      text(std::format("{} ticks | rev {} | {} rows | {} | {}", ticks_, lastRevision_, options_.rows,
                       pendingVisible_ ? "range loading" : "range ready", turbo_ ? "Turbo" : "Modern"))));

    return {};
  }

  std::expected<bool, TreeError> input(Application &app, const InputEvent &event) override {
    if (example::quitKey(event)) {
      app.requestStop();

      return true;
    }

    if (const auto *key = std::get_if<KeyEvent>(&event); key && key->key == InputKey::F2) {
      turbo_ = !turbo_;
      take(styles_.replace(turbo_ ? turboTheme() : modernTheme()));

      return true;
    }

    return false;
  }

  void stop(Application &) override {
    take(timer_.disconnect());

    for (auto &slot : requests_) {
      if (slot.request) {
        slot.request->cancel();
      }
    }

    pendingLatest_.reset();
    stoppedAt_ = app_.now();
  }

  std::uint64_t pendingTicks() const noexcept {
    return scheduledTicks_ - ticks_;
  }

  std::optional<SchedulerTime> stoppedAt() const noexcept {
    return stoppedAt_;
  }

  std::uint64_t tickCount() const noexcept {
    return ticks_;
  }

  std::uint64_t scheduledTicks() const noexcept {
    return scheduledTicks_;
  }

  Ohlc aggregate() const noexcept {
    return bucket_.value().value_or(Ohlc{100, 100, 100, 100});
  }

  bool coherent() const noexcept {
    return coherent_;
  }

  std::size_t maxPreparedRows() const noexcept {
    return maxPreparedRows_;
  }

  bool pendingVisible() const noexcept {
    return pendingVisible_;
  }

  std::uint64_t displayedRevision() const noexcept {
    return lastRevision_;
  }

  bool turbo() const noexcept {
    return turbo_;
  }

  const std::shared_ptr<Property<std::string>> &note() const noexcept {
    return name_;
  }

  Node<VirtualTable<Quote>> table() const noexcept {
    return table_;
  }

private:
  static constexpr std::int32_t HEADER_ROWS = 4;
  static constexpr std::uint64_t PROVIDER_CADENCE_MS = 10;
  static constexpr std::size_t INITIAL_PAGE_ROWS = 256;

  struct RequestSlot {
    std::optional<ModelRequest<Quote>> request;
    RowRange range{};
    std::uint64_t readyAt{};
    Ohlc aggregate{};
    bool acquired{};
  };

  static AxisSize fixed(std::int32_t extent) {
    return {.minimum = extent, .preferred = extent, .maximum = extent, .mode = SizeMode::FIXED};
  }

  GraphemeText text(std::string_view value) const {
    return example::text(value, app_.widthPolicy());
  }

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

    if (slot.range != request->range()) {
      slot.readyAt = app_.now() + options_.delayMs;
      slot.range = request->range();
      slot.acquired = false;
    }

    if (slot.request) {
      slot.request->cancel();
    }

    slot.request = *request;
    slot.aggregate = aggregate;
  }

  void forward() {
    auto result = forwardLatest(model_->publisher(), pendingLatest_);

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
  StyleEngine styles_;
  UiScope scope_;
  Subscription timer_;
  SchedulerConnection<ModelSnapshot<Quote>> ingress_;
  Node<Grid> grid_;
  Node<Label> title_, status_;
  Node<LineEdit> editor_;
  Node<Button> inspect_;
  Node<VirtualTable<Quote>> table_;
  Node<Chart<Quote>> chart_;
  std::shared_ptr<Property<std::string>> name_;
  std::unique_ptr<Model<Quote>> model_;
  std::optional<LineEditBinding> editBinding_;
  ActionBinding tableBinding_, inspectBinding_;
  std::shared_ptr<const ModelSnapshot<Quote>> pendingLatest_;
  std::array<RequestSlot, 2> requests_;
  OhlcBucket bucket_;
  std::uint64_t started_, ticks_{}, lastRevision_{}, scheduledTicks_{};
  std::optional<CellSize> lastSize_;
  std::optional<SchedulerTime> stoppedAt_;
  std::size_t maxPreparedRows_{};
  bool coherent_{true}, pendingVisible_{}, turbo_{}, retrySnapshot_{};
};
} // namespace dashboard
