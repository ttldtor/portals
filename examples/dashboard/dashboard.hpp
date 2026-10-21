// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "provider.hpp"
#include <format>
#include <memory>
#include <portals/input_match.hpp>
#include <portals/table_column.hpp>
#include <portals/text_format.hpp>
#include <portals/ui.hpp>

namespace dashboard {
using namespace portals;
using example::take;

namespace detail {
/** @brief Presentation state retained by the owning Scene through UI cleanup. */
struct Presentation {
  ui::Description<Column> recipe(ManagedScene &owner, Application &app, Options options) {
    app_ = &app;
    provider_ = std::make_unique<Provider>(app, options);
    name_ = take(owner.state<std::string>("Market note"));
    model_ = take(owner.model<Quote>(provider_->initialSnapshot()));

    const auto inspect = [this]() -> std::expected<void, TreeError> {
      const auto value = name_.snapshot();

      if (!value) {
        return std::unexpected(value.error());
      }

      const auto updated = name_.set((*value)->value);

      if (!updated) {
        return std::unexpected(updated.error());
      }

      return {};
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

    // clang-format off
    auto body = ui::vbox(LinearOptions{.padding = {.left = 1, .right = 1}},
      ui::label(title_, "Portals market | F2 theme | F10 quit"),
      ui::hbox(LinearOptions{.gap = 2}, ui::lineEdit(editor_, name_).flexX(),
               ui::button(inspectButton_, "Inspect", inspect),
               ui::button("Themes", [selector = selector_] { return selector.open(); })),
      ui::label(status_, "Starting market"),
      ui::virtualTable(table_, model_, columns, tableConfig).flexY(2),
      ui::vbox().height(1),
      ui::vbox(ui::chart(chart_, model_, &Quote::prices, chartConfig)).flexY());
    // clang-format on

    return body;
  }

  std::expected<void, ManagedError> configure(ManagedScene &owner, Application &app) {
    take(app.router().setFocus(table_.node()));

    take(owner.onInput([this](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (!keyPressed(event, InputKey::F2, {})) {
        return false;
      }

      const auto next = !turbo_;

      auto previewed = selector_.preview(next ? "turbo" : "modern");

      if (!previewed) {
        return std::unexpected(previewed.error());
      }

      auto committed = selector_.commit();

      if (!committed) {
        return std::unexpected(committed.error());
      }

      turbo_ = next;

      return true;
    }));
    take(owner.onKey(ui::press("F10 | Ctrl+C"), [] {
      ui::quit();
    }));
    provider_->start(model_);

    return {};
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
    const auto caption = std::format("Committed {} | {} ticks | {}", turbo_ ? "Turbo" : "Modern",
                                     provider_->tickCount(), pendingVisible_ ? "Loading" : "Live");

    return setControlText(app_->tree(), status_.node(), example::text(caption, app_->widthPolicy()));
  }

  Application *app_{};
  std::unique_ptr<Provider> provider_;
  ui::Ref<ThemeSelector> selector_;
  ui::Ref<Label> title_, status_;
  ui::Ref<LineEdit> editor_;
  ui::Ref<Button> inspectButton_;
  ui::Ref<VirtualTable<Quote>> table_;
  ui::Ref<Chart<Quote>> chart_;
  ManagedState<std::string> name_;
  ManagedModel<Quote> model_;
  std::uint64_t firstRow_{};
  bool pendingVisible_{}, turbo_{};
};
} // namespace detail

/** @brief One owning market Scene; its retained presentation state outlives borrowed callbacks.
 * Provider cancellation precedes managed model retirement, including partial setup failure.
 */
class FluentScene {
public:
  /** @param options Provider settings retained for this run.
   * @param runtime Existing runner policies and optional borrowed replay clock.
   */
  explicit FluentScene(Options options = {}, ui::SceneOptions runtime = {})
      : FluentScene([options] {
          return options;
        },
                    runtime) {
  }

  /** @param options Caller-owned settings read during scene preparation.
   * @param runtime Existing runner policies for embedding.
   */
  explicit FluentScene(ui::OptionsRef<Options> options, ui::SceneOptions runtime = {})
      : FluentScene([options] {
          return options.get();
        },
                    runtime) {
  }

  /** @return The owning Scene, which may be moved out without retaining this wrapper. */
  ui::Scene &scene() noexcept {
    return scene_;
  }

  FluentScene(const FluentScene &) = delete;
  FluentScene &operator=(const FluentScene &) = delete;
  FluentScene(FluentScene &&) = delete;
  FluentScene &operator=(FluentScene &&) = delete;

private:
  FluentScene(std::function<Options()> readOptions, ui::SceneOptions runtime)
      : scene_(makeScene(std::move(readOptions), runtime)) {
  }

  static ui::Scene makeScene(std::function<Options()> readOptions, ui::SceneOptions runtime) {
    auto state = std::make_shared<detail::Presentation>();
    ui::ThemeSelectorCatalog catalog{
      .choices = {{"modern", "Modern", modernTheme}, {"turbo", "Turbo", turboTheme}},
      .initialId = "modern"};
    ui::ThemeSelectorOptions selectorOptions;
    selectorOptions.onCommitted = [state](std::string_view id) -> std::expected<void, ManagedError> {
      state->turbo_ = id == "turbo";

      return {};
    };

    auto scene = ui::scene(runtime, [state, readOptions = std::move(readOptions)](ManagedScene &owner, Application &app) {
      return state->recipe(owner, app, readOptions());
    },
                           ui::themeSelector(state->selector_, catalog, selectorOptions));
    scene.configure([state](ManagedScene &owner, Application &app) {
      return state->configure(owner, app);
    });
    scene.onStop([state] {
      if (state->provider_) {
        auto provider = std::move(state->provider_);

        provider->stop();
      }
    });

    return scene;
  }

  ui::Scene scene_;
};
} // namespace dashboard
