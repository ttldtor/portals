// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "provider.hpp"
#include <format>
#include <portals/input_match.hpp>
#include <portals/table_column.hpp>
#include <portals/text_format.hpp>
#include <portals/ui.hpp>

namespace dashboard {
using namespace portals;
using example::take;

/** @brief Compact market scene with bounded provider loading and one owning fluent runtime.
 * The stationary object outlives callbacks borrowing it. Provider cancellation precedes model retirement.
 */
class FluentScene {
public:
  /** @param options Provider and display settings retained for this run.
   * @param runtime Existing runner policies and optional borrowed replay clock.
   */
  explicit FluentScene(Options options = {}, ui::SceneOptions runtime = {})
      : FluentScene(
          [options] {
            return options;
          },
          runtime) {
  }

  /** @param options Caller-owned settings read after command-line preflight.
   * @param runtime Existing runner policies for embedding.
   */
  explicit FluentScene(ui::OptionsRef<Options> options, ui::SceneOptions runtime = {})
      : FluentScene(
          [options] {
            return options.get();
          },
          runtime) {
  }

  /** @return The owning scene for application options, handlers and the standard runner. */
  ui::Scene &scene() noexcept {
    return scene_;
  }

private:
  FluentScene(std::function<Options()> readOptions, ui::SceneOptions runtime)
      : scene_(ui::scene(
          [this, readOptions = std::move(readOptions)](ManagedScene &owner, Application &app) {
            return recipe(owner, app, readOptions());
          },
          runtime)) {
    scene_.theme(modernTheme())
      .configure([this](ManagedScene &owner, Application &app) {
        configure(owner, app);
      })
      .onStop([this] {
        if (provider_) {
          auto provider = std::move(provider_);

          provider->stop();
        }
      });
  }

public:
  FluentScene(const FluentScene &) = delete;
  FluentScene &operator=(const FluentScene &) = delete;
  FluentScene(FluentScene &&) = delete;
  FluentScene &operator=(FluentScene &&) = delete;

private:
  ui::Description<Column> recipe(ManagedScene &owner, Application &app, Options options) {
    app_ = &app;
    provider_ = std::make_unique<Provider>(app, options);
    name_ = take(owner.state<std::string>("Market note"));
    model_ = take(owner.model<Quote>(provider_->initialSnapshot()));

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

    return ui::vbox(LinearOptions{.padding = {.left = 1, .right = 1}},
                    ui::label(title_, "Portals market | F2 theme | F10 quit"),
                    ui::hbox(LinearOptions{.gap = 2}, ui::lineEdit(editor_, name_).flexX(),
                             ui::button(inspectButton_, "Inspect", inspect)),
                    ui::label(status_, "Starting market"),
                    ui::virtualTable(table_, model_, columns, tableConfig).flexY(2),
                    ui::vbox().height(1),
                    ui::vbox(ui::chart(chart_, model_, &Quote::prices, chartConfig)).flexY());
  }

  void configure(ManagedScene &owner, Application &app) {
    take(app.router().setFocus(table_.node()));

    take(owner.onInput([this, &owner](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (!keyPressed(event, InputKey::F2, {})) {
        return false;
      }

      const auto next = !turbo_;

      return owner.theme(next ? turboTheme() : modernTheme()).transform([this, next] {
        turbo_ = next;

        return true;
      });
    }));
    take(owner.onKey(ui::press("F10 | Ctrl+C"), [] {
      ui::quit();
    }));
    provider_->start(model_);
  }

  std::expected<void, TreeError> tablePrepared(const ModelFrame<Quote> &pin,
                                               const std::optional<ModelRequest<Quote>> &work) {
    auto read = app_->tree().read(table_.node(), [&](const auto &table) {
      firstRow_ = table.firstRow();
      pendingVisible_ = table.rangeState() == RangeState::PENDING;
    });

    return read.and_then([&] {
      return provider_->tablePrepared(pin, work);
    });
  }

  std::expected<void, TreeError> chartPrepared(const ModelFrame<Quote> &,
                                               const std::optional<ModelRequest<Quote>> &work) {
    provider_->chartPrepared(work);

    // Ref::text mutation is disallowed during managed preparation; this is the existing view callback.
    const auto caption = std::format("{} | {} ticks | {}", turbo_ ? "Turbo" : "Modern", provider_->tickCount(),
                                     pendingVisible_ ? "Loading" : "Live");

    return setControlText(app_->tree(), status_.node(), example::text(caption, app_->widthPolicy()));
  }

  Application *app_{};
  std::unique_ptr<Provider> provider_;
  ui::Ref<Label> title_, status_;
  ui::Ref<LineEdit> editor_;
  ui::Ref<Button> inspectButton_;
  ui::Ref<VirtualTable<Quote>> table_;
  ui::Ref<Chart<Quote>> chart_;
  ManagedState<std::string> name_;
  ManagedModel<Quote> model_;
  std::uint64_t firstRow_{};
  bool pendingVisible_{}, turbo_{};
  ui::Scene scene_;
};
} // namespace dashboard
