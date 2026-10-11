// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "provider.hpp"
#include <format>
#include <portals/chart.hpp>
#include <portals/grid.hpp>
#include <portals/managed.hpp>
#include <portals/style.hpp>
#include <portals/table_column.hpp>
#include <portals/text_format.hpp>
#include <portals/ui.hpp>
#include <portals/virtual_table.hpp>

namespace dashboard {
using namespace portals;
using example::take;

/** @brief Declarative dashboard UI and borrowed adapter over one existing managed runtime.
 * Market provider work is separate from presentation. The object is stationary on UI because
 * the managed callbacks borrow it; the application outlives this scene.
 */
class Scene final : public ApplicationScene {
public:
  /** @param app Borrowed application that outlives the scene and supplies its UI thread and clock.
   * @param options Provider and display settings retained for this run.
   * Mount and scheduler failures are propagated by example::take.
   */
  Scene(Application &app, Options options)
      : app_(app), managed_(take(ManagedScene::create(app))), provider_(app, options), stress_(options.stress) {
    name_ = take(managed_->state<std::string>("Market note"));

    const auto model = take(managed_->model<Quote>(provider_.initialSnapshot()));

    const auto inspect = [this] {
      return name_.snapshot().and_then([this](const auto &value) {
        return name_.set(value->value).transform([](const auto &) {
        });
      });
    };

    const auto priceText = cellFormat<double>("{:.2f}", app.widthPolicy());

    const auto columns = take(tableColumns(
      app.widthPolicy(),
      tableColumn("Instrument", &Quote::instrument, cellFormat<std::uint64_t>("SYM-{:07}", app.widthPolicy()), 13),
      tableColumn<Quote>(
        "Open",
        [](const Quote &quote) {
          return quote.prices.open;
        },
        priceText, 12),
      tableColumn<Quote>(
        "High",
        [](const Quote &quote) {
          return quote.prices.high;
        },
        priceText, 12),
      tableColumn<Quote>(
        "Low",
        [](const Quote &quote) {
          return quote.prices.low;
        },
        priceText, 12),
      tableColumn<Quote>(
        "Close",
        [](const Quote &quote) {
          return quote.prices.close;
        },
        priceText, 12)));

    ui::Ref<Label> title;
    ui::Ref<LineEdit> editor;
    ui::Ref<Button> inspectButton;
    ui::Ref<Row> toolbar;
    const ui::VirtualTableConfig<Quote> tableConfig{.prepared = [this](const auto &pin, const auto &work) {
      return tablePrepared(pin, work);
    }};
    const ui::ChartConfig<Quote> chartConfig{
      .options = {.mode = ChartMode::CANDLES,
                  .width_policy = app.widthPolicy(),
                  .glyph_set = options.ascii ? ChartGlyphSet::ASCII : ChartGlyphSet::BOX_DRAWING,
                  .candle_gap = 1},
      .first = [this]() -> std::expected<std::uint64_t, TreeError> {
        return firstRow_;
      },
      .prepared =
        [this](const auto &pin, const auto &work) {
          return chartPrepared(pin, work);
        }};

    auto heading = ui::label(
      title, options.stress ? (options.ascii ? "Portals | Tab focus | arrows select/scroll | F2 theme | F10 quit"
                                             : "Portals \xe7\x95\x8c e\xcc\x81 | Tab focus | F2 theme | F10 quit")
                            : "Portals market | F2 theme | F10 quit");
    auto note = ui::lineEdit(editor, name_,
                             {.policy = options.stress ? SizePolicy{} : SizePolicy{.width = {.mode = SizeMode::FLEX}}});
    auto inspectAction = ui::button(inspectButton, options.stress ? "Inspect (unchanged)" : "Inspect", inspect);
    auto status = ui::label(status_, "Starting market");
    auto table = ui::virtualTable(table_, model, columns, tableConfig);
    auto chart = ui::chart(chart_, model, &Quote::prices, chartConfig);

    // Both compositions consume the same views and preserve editor/button/table focus order.
    auto description =
      options.stress ? ui::grid(std::move(heading), std::move(note), std::move(inspectAction), std::move(status),
                                std::move(table), std::move(chart))
                     : ui::grid(std::move(heading),
                                ui::row(toolbar, LinearOptions{.gap = 2}, std::move(note), std::move(inspectAction)),
                                std::move(status), std::move(table), std::move(chart));

    const auto gridNode = take(ui::mount(*managed_, std::move(description)));

    take(app.router().setFocus(table_.node()));
    take(managed_->theme(modernTheme()));

    GridOptions grid;
    const AxisSize flexible{.preferred = 0, .mode = SizeMode::FLEX};
    const AxisSize header{.preferred = 1, .maximum = 1, .mode = SizeMode::FIXED};

    grid.columns = {flexible};
    grid.rows = {header, header, header, header, flexible, flexible};
    grid.cells = {{title.node().id(), 0, 0},   {editor.node().id(), 1, 0}, {inspectButton.node().id(), 2, 0},
                  {status_.node().id(), 3, 0}, {table_.node().id(), 4, 0}, {chart_.node().id(), 5, 0}};

    if (!options.stress) {
      // Four fixed tracks leave the remaining height to table/chart in a 2:1 ratio.
      grid.padding = {.left = 1, .right = 1};
      grid.rows = {header, header,  header, AxisSize{.preferred = 0, .mode = SizeMode::FLEX, .weight = 2},
                   header, flexible};
      grid.cells = {{title.node().id(), 0, 0},
                    {toolbar.node().id(), 1, 0},
                    {status_.node().id(), 2, 0},
                    {table_.node().id(), 3, 0},
                    {chart_.node().id(), 5, 0}};
    }

    take(setGridOptions(app.tree(), gridNode, grid));

    provider_.start(model);
  }

  Scene(const Scene &) = delete;
  Scene &operator=(const Scene &) = delete;
  Scene(Scene &&) = delete;
  Scene &operator=(Scene &&) = delete;

  /** @brief Retire provider and UI callbacks before the presentation state they borrow is destroyed. */
  ~Scene() override {
    stopProvider();
    take(managed_->close());
  }

  /** @copydoc portals::ApplicationScene::usesPreparedLayout */
  bool usesPreparedLayout() const noexcept override {
    return managed_->usesPreparedLayout();
  }

  /** @copydoc portals::ApplicationScene::prepare */
  std::expected<void, TreeError> prepare(Application &app, CellSize size) override {
    return managed_->prepare(app, size);
  }

  /** @copydoc portals::ApplicationScene::prepareViews */
  std::expected<void, TreeError> prepareViews(Application &app, const LayoutSnapshot &layout) override {
    return managed_->prepareViews(app, layout);
  }

  /** @copydoc portals::ApplicationScene::input */
  std::expected<bool, TreeError> input(Application &app, const InputEvent &event) override {
    if (example::quitKey(event)) {
      app.requestStop();

      return true;
    }

    if (const auto *key = std::get_if<KeyEvent>(&event); key && key->key == InputKey::F2) {
      turbo_ = !turbo_;
      take(managed_->theme(turbo_ ? turboTheme() : modernTheme()));

      return true;
    }

    return false;
  }

  /** @brief Cancel the synchronous provider once, independently of managed UI cleanup.
   * This boundary lets the benchmark measure provider cancellation without tree teardown.
   * Call on the application's UI thread; subsequent calls preserve the first stop time.
   */
  void stopProvider() {
    provider_.stop();
  }

  /** @copydoc portals::ApplicationScene::stop */
  void stop(Application &app) override {
    stopProvider();
    managed_->stop(app);
  }

  /** @return Domain ticks still waiting after a bounded pump or provider cancellation. */
  std::uint64_t pendingTicks() const noexcept {
    return provider_.pendingTicks();
  }

  /** @return First provider cancellation time in scheduler units, or empty before stopping. */
  std::optional<SchedulerTime> stoppedAt() const noexcept {
    return provider_.stoppedAt();
  }

  /** @return Number of domain ticks accumulated before display coalescing. */
  std::uint64_t tickCount() const noexcept {
    return provider_.tickCount();
  }

  /** @return Number of domain ticks due at the last provider pump. */
  std::uint64_t scheduledTicks() const noexcept {
    return provider_.scheduledTicks();
  }

  /** @return Lossless market aggregate, or the initial flat price before the first tick. */
  Ohlc aggregate() const noexcept {
    return provider_.aggregate();
  }

  /** @return Whether every prepared chart revision matched the shared managed table pin. */
  bool coherent() const noexcept {
    return coherent_;
  }

  /** @return Largest bounded table interval prepared during this run, including overscan. */
  std::size_t maxPreparedRows() const noexcept {
    return maxPreparedRows_;
  }

  /** @return Whether the latest table preparation displayed pending provider data. */
  bool pendingVisible() const noexcept {
    return pendingVisible_;
  }

  /** @return Model revision used by the latest chart preparation. */
  std::uint64_t displayedRevision() const noexcept {
    return lastRevision_;
  }

  /** @return Whether the current UI theme is Turbo rather than Modern. */
  bool turbo() const noexcept {
    return turbo_;
  }

  /** @return Borrowed weak managed note handle; the property survives stop until scene destruction. */
  const ManagedState<std::string> &note() const noexcept {
    return name_;
  }

  /** @return Observing table identity; tree access detects retirement after scene stop. */
  Node<VirtualTable<Quote>> table() const noexcept {
    return table_.node();
  }

private:
  std::expected<void, TreeError> tablePrepared(const ModelFrame<Quote> &pin,
                                               const std::optional<ModelRequest<Quote>> &work) {
    auto read = app_.tree().read(table_.node(), [&](const auto &table) {
      firstRow_ = table.firstRow();
      maxPreparedRows_ = std::max(maxPreparedRows_, table.preparedRange().count);
      pendingVisible_ = table.rangeState() == RangeState::PENDING;
    });

    return read.and_then([&] {
      return provider_.tablePrepared(pin, work);
    });
  }

  std::expected<void, TreeError> chartPrepared(const ModelFrame<Quote> &pin,
                                               const std::optional<ModelRequest<Quote>> &work) {
    provider_.chartPrepared(work);

    auto read = app_.tree().read(chart_.node(), [&](const auto &chart) {
      coherent_ = coherent_ && chart.dataRevision() == pin.dataRevision();
    });

    return read.and_then([&] {
      lastRevision_ = pin.dataRevision();

      // Ref::text mutation is disallowed during managed preparation; this is the existing view callback.
      const auto caption =
        stress_
          ? std::format("{} ticks | rev {} | {} rows | {} | {}", provider_.tickCount(), lastRevision_, provider_.rows(),
                        pendingVisible_ ? "range loading" : "range ready", turbo_ ? "Turbo" : "Modern")
          : std::format("{} | {} ticks | {}", turbo_ ? "Turbo" : "Modern", provider_.tickCount(),
                        pendingVisible_ ? "Loading" : "Live");

      return setControlText(app_.tree(), status_.node(), example::text(caption, app_.widthPolicy()));
    });
  }

  Application &app_;
  std::unique_ptr<ManagedScene> managed_;
  Provider provider_;
  ui::Ref<Label> status_;
  ui::Ref<VirtualTable<Quote>> table_;
  ui::Ref<Chart<Quote>> chart_;
  ManagedState<std::string> name_;
  std::uint64_t firstRow_{}, lastRevision_{};
  std::size_t maxPreparedRows_{};
  bool coherent_{true}, pendingVisible_{}, turbo_{}, stress_;
};
} // namespace dashboard
