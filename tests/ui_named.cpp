// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "package_consumer/virtual_support.hpp"
#include <portals/render.hpp>
#include <portals/ui.hpp>
#include <portals/ui_scene_resource.hpp>

using namespace portals;

// Braced legacy options must remain unambiguous after named overloads are added.
static_assert(requires(ManagedModel<int> model, std::span<const TableColumn<int>> columns,
                       ManagedListFormatter<int> format, ManagedChartAccessor<int> accessor) {
  ui::virtualTable(model, columns, {});
  ui::virtualList(model, format, {});
  ui::chart(model, accessor, {});
});

// Ref-first overloads retain bare positional options and owning named configurations.
static_assert(requires(ManagedModel<int> model, std::span<const TableColumn<int>> columns,
                       ManagedListFormatter<int> format, ManagedChartAccessor<int> accessor,
                       ui::Ref<VirtualTable<int>> table, ui::Ref<VirtualList<int>> list, ui::Ref<Chart<int>> chart) {
  ui::virtualTable(table, model, columns, {});
  ui::virtualList(list, model, format, {});
  ui::chart(chart, model, accessor, {});
  ui::virtualTable(table, model, columns, ui::VirtualTableConfig<int>{});
  ui::virtualList(list, model, format, ui::VirtualListConfig<int>{});
  ui::chart(chart, model, accessor, ui::ChartConfig<int>{});
});

template <class Config>
concept TableConfigAccepted =
  requires(ManagedModel<int> model, std::span<const TableColumn<int>> columns, Config &&config) {
    ui::virtualTable(model, columns, std::forward<Config>(config));
  };

static_assert(TableConfigAccepted<ui::VirtualTableConfig<int>>);
static_assert(TableConfigAccepted<ui::VirtualTableConfig<int> &>);
static_assert(TableConfigAccepted<const ui::VirtualTableConfig<int> &>);
static_assert(!TableConfigAccepted<ui::VirtualTableConfig<double>>);
static_assert(!TableConfigAccepted<ui::ChartConfig<int>>);

template <class T>
concept RowHeightStage = requires(ui::Description<T> recipe) { std::move(recipe).rowHeight(2); };
template <class T>
concept TableBudgetStage = requires(ui::Description<T> recipe) { std::move(recipe).maxColumns(2); };

struct DerivedListForSchema : VirtualList<int> {
  using VirtualList<int>::VirtualList;
};

static_assert(RowHeightStage<VirtualList<int>> && RowHeightStage<VirtualTable<int>>);
static_assert(!RowHeightStage<Button> && !RowHeightStage<DerivedListForSchema>);
static_assert(TableBudgetStage<VirtualTable<int>> && !TableBudgetStage<VirtualList<int>>);

template <class T>
concept ChartStages = requires(ui::Description<T> recipe) {
  std::move(recipe).mode(ChartMode::CANDLES).chartGlyphSet(ChartGlyphSet::ASCII).candleGap(0).yRange(std::nullopt);
};

struct DerivedChartForSchema : Chart<int> {
  using Chart<int>::Chart;
};

static_assert(ChartStages<Chart<int>> && !ChartStages<Button> && !ChartStages<DerivedChartForSchema>);
static_assert(!RowHeightStage<Chart<int>> && !TableBudgetStage<Chart<int>>);

namespace {
using namespace virtualConsumer;

template <class Factory> void invalidSettings(Factory factory) {
  ReplayTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  const auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
  const auto mounted = take(ui::mount(*scene, factory(model)));
  const auto result = scene->run(terminal);

  check(!result && result.error().phase == ManagedPhase::PREPARE && result.error().node == mounted.id(),
        "Named settings lost preparation error identity");
  check(std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT,
        "Invalid named settings were ignored");
}

LayoutSnapshot prepareDataViews(ManagedScene &scene, Application &app, CellSize size = INITIAL_SIZE) {
  take(scene.prepare(app, size));

  std::optional<LayoutSnapshot> layout;

  take(app.tree().frame([&] {
    const auto bounds = take(CellRect::from({}, size));

    take(layoutTree(app.tree(), app.root(), bounds, bounds));
    layout = take(LayoutSnapshot::capture(app.tree(), scene.root(), LayoutSnapshotProfile::GEOMETRY));
  }));
  take(scene.prepareViews(app, *layout));
  take(scene.completedLayout(app));

  return *layout;
}

void dataScalarParity() {
  std::optional<CellRect> listBounds, tableBounds;

  for (int mode{}; mode < 5; ++mode) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::Ref<VirtualList<Ohlc>> list;
    ui::Ref<VirtualTable<Ohlc>> table;
    std::size_t listCalls{}, tableCalls{}, preparedCalls{};
    int factories{};
    auto format = [&](const ModelRow<Ohlc> &) {
      ++listCalls;

      return virtualConsumer::text("row");
    };
    auto columns = priceColumns();

    for (auto &column : columns) {
      column.format = [&](const Ohlc &) {
        ++tableCalls;

        return virtualConsumer::text("row");
      };
    }

    const ManagedViewHandler<Ohlc> prepared =
      [&](const ModelFrame<Ohlc> &frame,
          const std::optional<ModelRequest<Ohlc>> &request) -> std::expected<void, TreeError> {
      check(frame.dataRevision() == DATA_REVISION && !request, "scalar data recipe lost the actual ready model");
      ++preparedCalls;

      return {};
    };
    VirtualListOptions listOptions;
    VirtualTableOptions tableOptions;

    listOptions.row_height = 3;
    listOptions.overscan_rows = 9;
    listOptions.normal.bold = true;
    tableOptions.row_height = 3;
    tableOptions.overscan_rows = 9;
    tableOptions.max_columns = 1;
    tableOptions.max_prepared_cells = 1;
    tableOptions.max_prepared_text_bytes = 1;
    tableOptions.header.underline = true;

    if (!mode) {
      listOptions.row_height = tableOptions.row_height = 2;
      listOptions.overscan_rows = tableOptions.overscan_rows = 0;
      tableOptions.max_columns = 2;
      tableOptions.max_prepared_cells = 6;
      tableOptions.max_prepared_text_bytes = 64;
      take(ui::mount(*scene, ui::column(ui::virtualList(list, model, format, listOptions, {}, prepared).height(6),
                                        ui::virtualTable(table, model, std::span<const TableColumn<Ohlc>>(columns),
                                                         tableOptions, {}, prepared)
                                          .height(7))));
    } else if (mode == 1) {
      take(ui::mount(
        *scene, ui::column(ui::virtualList(list, model, format,
                                           ui::VirtualListConfig<Ohlc>{.options = listOptions, .prepared = prepared})
                             .rowHeight(2)
                             .overscanRows(0)
                             .height(6),
                           ui::virtualTable(table, model, std::span<const TableColumn<Ohlc>>(columns),
                                            ui::VirtualTableConfig<Ohlc>{.options = tableOptions, .prepared = prepared})
                             .rowHeight(2)
                             .overscanRows(0)
                             .maxColumns(2)
                             .maxPreparedCells(6)
                             .maxPreparedTextBytes(64)
                             .height(7))));
    } else if (mode == 2) {
      auto listRecipe = ui::virtualList(list, model, format, listOptions, {}, prepared).height(6) | ui::rowHeight(2) |
                        ui::overscanRows(0);
      auto tableRecipe =
        ui::virtualTable(table, model, std::span<const TableColumn<Ohlc>>(columns), tableOptions, {}, prepared)
          .height(7) |
        ui::rowHeight(2) | ui::overscanRows(0) | ui::maxColumns(2) | ui::maxPreparedCells(6) |
        ui::maxPreparedTextBytes(64);

      take(ui::mount(*scene, ui::column(std::move(listRecipe), std::move(tableRecipe))));
    } else if (mode == 3) {
      ui::ControlContext controls;

      controls.defaults("VirtualList", {.rowHeight = 2, .overscanRows = 5})
        .define("Rows", "VirtualList")
        .defaults(
          "VirtualTable",
          {.rowHeight = 2, .overscanRows = 5, .maxColumns = 2, .maxPreparedCells = 6, .maxPreparedTextBytes = 64})
        .define("Prices", "VirtualTable");
      take(ui::mount(
        *scene,
        ui::column(
          ui::virtualList(list, model, format, listOptions, {}, prepared).variant("Rows").overscanRows(0).height(6),
          ui::virtualTable(table, model, std::span<const TableColumn<Ohlc>>(columns), tableOptions, {}, prepared)
            .variant("Prices")
            .overscanRows(0)
            .height(7)),
        controls));
    } else {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      take(options.registry.add<VirtualList<Ohlc>>("BoundList", [&](ui::SceneResourceArguments arguments) {
        ++factories;
        check(arguments.text == "list metadata", "old list text metadata was discarded");

        return ui::virtualList(list, model, format, listOptions, {}, prepared).rowHeight(4).overscanRows(7);
      }));
      take(options.registry.add<VirtualTable<Ohlc>>("BoundTable", [&](ui::SceneResourceArguments arguments) {
        ++factories;
        check(arguments.text == "table metadata", "old table text metadata was discarded");

        return ui::virtualTable(table, model, std::span<const TableColumn<Ohlc>>(columns), tableOptions, {}, prepared);
      }));
      take(ui::mount(*scene, ui::resource(R"(
        definitions{Rows=BoundList{rowHeight=2 overscanRows=4} Prices=BoundTable{rowHeight=2 overscanRows=4 maxColumns=2 maxPreparedCells=6 maxPreparedTextBytes=64}}
        scene{VBox{Rows{text="list metadata" overscanRows=0 height=6} Prices{text="table metadata" overscanRows=0 height=7}}}
      )",
                                          options)));
      check(factories == 2, "data providers must be the two explicitly registered code factories");
    }

    take(setVirtualListFirst(app->tree(), list.node(), 10));
    take(setVirtualTableFirst(app->tree(), table.node(), 10));
    const auto layout = prepareDataViews(*scene, *app);

    take(app->tree().read(list.node(), [&](const VirtualList<Ohlc> &view) {
      check(view.visibleRange() == RowRange{10, 3} && view.preparedRange() == RowRange{10, 3},
            "list row height/explicit zero overscan did not reach actual prepared ranges");
      check(view.styleDefaults().normal.bold, "list scalar patch lost actual manual style options");
    }));
    take(app->tree().read(table.node(), [&](const VirtualTable<Ohlc> &view) {
      check(view.visibleRange() == RowRange{10, 3} && view.preparedRange() == RowRange{10, 3},
            "table row height/explicit zero overscan did not reach actual prepared ranges");
      check(view.styleDefaults().header.underline, "table scalar patch lost actual header style options");
    }));
    check(listCalls == 3 && tableCalls == 6 && preparedCalls == 2,
          "scalar recipes lost native preparation or formatted invisible rows");
    const auto actualList = take(layout.geometry(list.node())).geometry.local;
    const auto actualTable = take(layout.geometry(table.node())).geometry.local;

    if (!mode) {
      listBounds = actualList;
      tableBounds = actualTable;
    } else {
      check(actualList == *listBounds && actualTable == *tableBounds,
            "data scalar input paths changed prepared geometry");
    }

    (void)take(app->tree().consumeFrameRequest());
    prepareDataViews(*scene, *app);
    check(listCalls == 3 && tableCalls == 6, "unchanged scalar data view reformatted rows during idle preparation");
  }
}

void scalarRejectionAndColdList() {
  {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::Ref<VirtualList<Ohlc>> unpublished;
    auto result = ui::mount(*scene, ui::virtualList(unpublished, model, [](const ModelRow<Ohlc> &) {
                                      return virtualConsumer::text("row");
                                    }).rowHeight(0));

    check(!result && result.error().sceneResource &&
            result.error().sceneResource->code == SceneResourceErrorCode::INVALID_VALUE &&
            unpublished.node() == Node<VirtualList<Ohlc>>{} &&
            take(app->tree().inspect(scene->root())).children.empty(),
          "invalid scalar patch must reject before node retention/reference publication");
    take(ui::mount(*scene, ui::virtualList(unpublished, model, [](const ModelRow<Ohlc> &) {
                             return virtualConsumer::text("row");
                           }).rowHeight(2)));
    check(unpublished.node() != Node<VirtualList<Ohlc>>{}, "rejected scalar patch left a reserved reference behind");
  }
  {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    ui::Ref<LineEdit> editor;
    ui::Ref<VirtualList<TextSegment>> list;
    ui::VirtualListConfig<TextSegment> config;

    config.options.row_height = 3;
    config.options.overscan_rows = 9;
    take(ui::mount(
      *scene,
      ui::column(
        ui::virtualList(ui::words(editor.draftText()), config).rowHeight(2).overscanRows(0).reference(list).height(6),
        ui::lineEdit(editor, "one two three four five six"))));
    prepareDataViews(*scene, *app);
    take(app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
      check(view.visibleRange() == RowRange{0, 3} && view.preparedRange() == RowRange{0, 3},
            "cold text-list recipe bypassed the same native row settings path");
    }));
  }
}

void explicitDataSchemas() {
  enum class SchemaMode {
    NARROW,
    EXPANDED,
    GLYPH_EXPANDED,
    FRAME_EXPANDED,
    GLYPH_FRAME_EXPANDED
  };

  for (const bool table : {false, true}) {
    for (const auto mode : {SchemaMode::NARROW, SchemaMode::EXPANDED, SchemaMode::GLYPH_EXPANDED,
                            SchemaMode::FRAME_EXPANDED, SchemaMode::GLYPH_FRAME_EXPANDED}) {
      const bool expanded = mode != SchemaMode::NARROW;
      ReplayTerminal terminal;
      auto app = take(Application::create({}, terminal.clock()));
      auto scene = take(ManagedScene::create(*app));
      auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
      ui::Ref<VirtualList<Ohlc>> list;
      ui::Ref<VirtualTable<Ohlc>> grid;
      ui::SceneResourceOptions options;
      std::vector<std::string_view> paths{"text", "width", "height", "flex"};

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.controls.emplace();
      if (expanded) {
        paths.insert(paths.end(), {"rowHeight", "overscanRows"});
        if (table) {
          paths.insert(paths.end(), {"maxColumns", "maxPreparedCells", "maxPreparedTextBytes"});
        }
      }

      if (mode == SchemaMode::GLYPH_EXPANDED || mode == SchemaMode::GLYPH_FRAME_EXPANDED) {
        paths.insert(paths.end(), {"glyphProfile", "glyphFallback"});
      }

      if (mode == SchemaMode::FRAME_EXPANDED || mode == SchemaMode::GLYPH_FRAME_EXPANDED) {
        for (const auto &property : ui::controlPropertySchema(table ? "VirtualTable" : "VirtualList")) {
          if (property.path.starts_with("frame.")) {
            paths.push_back(property.path);
          }
        }
      }

      options.controls->registerType("Bound", paths);
      int callbacks{};

      if (table) {
        take(options.registry.add<VirtualTable<Ohlc>>("Bound", [&](ui::SceneResourceArguments arguments) {
          ++callbacks;
          check(arguments.text == "metadata", "explicit table allowlist lost old text metadata");
          VirtualTableOptions actual;

          actual.row_height = 3;
          actual.overscan_rows = 0;

          return ui::virtualTable(grid, model, priceColumns(), actual);
        }));
      } else {
        take(options.registry.add<VirtualList<Ohlc>>("Bound", [&](ui::SceneResourceArguments arguments) {
          ++callbacks;
          check(arguments.text == "metadata", "explicit list allowlist lost old text metadata");

          return ui::virtualList(
            list, model,
            [](const ModelRow<Ohlc> &) {
              return virtualConsumer::text("row");
            },
            VirtualListOptions{.row_height = 3, .overscan_rows = 0});
        }));
      }
      const std::string patch = !expanded ? ""
                                : table
                                  ? "rowHeight=2 overscanRows=0 maxColumns=2 maxPreparedCells=6 maxPreparedTextBytes=64"
                                  : "rowHeight=2 overscanRows=0";
      const std::string source =
        "definitions{Alias=Bound{" + patch + "}} scene{Alias{text=\"metadata\" height=" + (table ? "7" : "6") + "}}";

      take(ui::mount(*scene, ui::vbox(ui::resource(source, options))));
      prepareDataViews(*scene, *app);
      check(callbacks == 1, "explicit data schema must preserve one real code factory invocation");
      const auto rows = expanded ? std::uint64_t{3} : std::uint64_t{2};
      const auto checkRanges = [&](const auto &view) {
        const auto visible = view.visibleRange();
        const auto prepared = view.preparedRange();
        const bool matches = visible == RowRange{0, rows} && prepared == RowRange{0, rows};

        if (!matches) {
          std::cerr << "table=" << table << " expanded=" << expanded << " visible={" << visible.first << ','
                    << visible.count << "} prepared={" << prepared.first << ',' << prepared.count << "} expected={0,"
                    << rows << "}\n";
        }

        return matches;
      };

      if (table) {
        take(app->tree().read(grid.node(), [&](const VirtualTable<Ohlc> &view) {
          check(checkRanges(view), "full table schema or old narrow native options changed their intended ranges");
        }));
      } else {
        take(app->tree().read(list.node(), [&](const VirtualList<Ohlc> &view) {
          check(checkRanges(view), "full list schema or old narrow native options changed their intended ranges");
        }));
      }
    }
  }
}

void conflictingDataSchemaRejected() {
  ReplayTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
  ui::SceneResourceOptions options;
  constexpr std::string_view PATHS[]{"text",         "width",        "height",        "flex",      "rowHeight",
                                     "overscanRows", "glyphProfile", "glyphFallback", "maxColumns"};
  int callbacks{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.controls.emplace();
  options.controls->registerType("Bound", PATHS);
  take(options.registry.add<VirtualList<Ohlc>>("Bound", [&](ui::SceneResourceArguments) {
    ++callbacks;

    return ui::virtualList(model, [](const ModelRow<Ohlc> &) {
      return virtualConsumer::text("row");
    });
  }));

  const auto result = ui::mount(*scene, ui::resource("scene{Bound{}}", options));

  check(!result && result.error().sceneResource &&
          result.error().sceneResource->code == SceneResourceErrorCode::DUPLICATE_TYPE && callbacks == 0 &&
          take(app->tree().inspect(scene->root())).children.empty(),
        "VirtualList schema with table-only maxColumns must reject before factory invocation or publication");
}

void dataFactoryLeafAccounting() {
  for (const std::size_t budget : {2U, 3U}) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::Ref<VirtualTable<Ohlc>> table;
    ui::SceneResourceOptions options;
    int callbacks{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    take(options.registry.add<VirtualTable<Ohlc>>("Bound", [&](ui::SceneResourceArguments) {
      ++callbacks;
      SizePolicy policy;

      policy.height = {.preferred = 7, .mode = SizeMode::FIXED};
      return ui::virtualTable(table, model, priceColumns(), {}, policy).rowHeight(4).maxColumns(2);
    }));
    auto result = ui::mount(*scene, ui::vbox(ui::resource("scene{Bound{rowHeight=2 overscanRows=0}}", options)));

    check(callbacks == 1, "new factory leaf is resolved only after the prevalidated resource invokes its binding");
    if (budget == 2) {
      check(!result && result.error().sceneResource &&
              result.error().sceneResource->code == SceneResourceErrorCode::MATERIALIZATION_LIMIT &&
              table.node() == Node<VirtualTable<Ohlc>>{} && take(app->tree().inspect(scene->root())).children.empty(),
            "genuine extra factory leaf must exhaust aggregate budget without publishing a reference");
    } else {
      take(std::move(result));
      prepareDataViews(*scene, *app);
      take(app->tree().read(table.node(), [&](const VirtualTable<Ohlc> &view) {
        const auto visible = view.visibleRange();
        const auto prepared = view.preparedRange();
        const bool matches = visible == RowRange{0, 3} && prepared == RowRange{0, 3};

        if (!matches) {
          std::cerr << "budget=" << budget << " visible={" << visible.first << ',' << visible.count << "} prepared={"
                    << prepared.first << ',' << prepared.count << "} expected={0,3}\n";
        }

        check(matches, "resource high-word leaves must win and be charged once beside a new factory leaf");
      }));
    }
  }
}

struct ChartSink : PaintContext {
  std::vector<RenderCommand> commands;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    for (const auto &value : values) {
      commands.emplace_back(value);
    }

    return {};
  }
};

using ChartCells = std::vector<std::pair<std::string, CellStyle>>;

ChartCells chartCells(Application &app, Node<Chart<Ohlc>> chart, CellSize size, WidthPolicy policy = {}) {
  ChartCells cells;

  take(app.tree().frame([&] {
    ChartSink sink;

    take(app.tree().paint(chart, sink));
    const auto frame = take(renderFullFrame(size, sink.commands, policy));

    for (std::int32_t y = 0; y < size.height; ++y) {
      for (std::int32_t x = 0; x < size.width; ++x) {
        const auto cell = take(frame.at({x, y}));

        cells.emplace_back(std::string(cell.glyph), cell.style);
      }
    }
  }));

  return cells;
}

void chartDeclarationRangeValidation() {
  for (const bool inactive : {false, true}) {
    ui::ControlContext context;
    ui::ControlSettings declaration{.yRange = {.minimum = 4, .maximum = 3}};

    if (inactive) {
      declaration.yRange.enabled = false;
    }

    context.defaults("Chart", declaration).define("Cleared", "Chart", {.yRange = {.enabled = false}});
    take(ui::detail::ControlContextAccess::installVirtualViewSchema(context, "Chart"));

    const auto validated = context.validate();

    check(inactive ? validated.has_value()
                   : !validated && validated.error().code == ui::ControlContextErrorCode::INVALID_VALUE,
          "eager validation must reject an invalid ancestor even when a later declaration disables its range");
    if (inactive) {
      check(take(context.resolve("Cleared")).settings.yRange.enabled == false,
            "explicitly inactive finite ancestor bounds must remain valid and clearable");
    }
  }
}

void chartScalarParity() {
  std::optional<ChartCells> asciiCandles;

  for (int scenario = 0; scenario < 6; ++scenario) {
    std::optional<ChartCells> baseline;
    std::optional<CellRect> baselineBounds;
    const auto chartMode = scenario == 0 ? ChartMode::LINE : ChartMode::CANDLES;
    const auto glyphSet = (scenario == 2 || scenario == 5) ? ChartGlyphSet::BOX_DRAWING : ChartGlyphSet::ASCII;
    const std::optional<ChartRange> range = scenario == 0 || scenario == 4 ? std::nullopt
                                            : scenario == 3                ? std::optional{ChartRange{104, 104}}
                                                                           : std::optional{ChartRange{90, 120}};

    for (int path = 0; path < 5; ++path) {
      ReplayTerminal terminal;
      auto app = take(Application::create({}, terminal.clock()));
      auto scene = take(ManagedScene::create(*app));
      auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
      ui::Ref<Chart<Ohlc>> chart;
      std::size_t accesses{}, preparedCalls{}, originCalls{};
      int factories{};
      ChartOptions actual;

      actual.mode = ChartMode::LINE;
      actual.glyph_set = ChartGlyphSet::ASCII;
      actual.candle_gap = 4;
      actual.y_range = ChartRange{50, 150};
      actual.line.bold = actual.rising.bold = actual.falling.bold = true;
      if (scenario == 5) {
        actual.width_policy.ambiguous = AmbiguousWidth::WIDE;
      }

      const ManagedChartAccessor<Ohlc> accessor = [&](const Ohlc &value) {
        ++accesses;

        return std::optional{value};
      };
      const ManagedChartOrigin first = [&]() -> std::expected<std::uint64_t, TreeError> {
        ++originCalls;

        return 3;
      };
      const ManagedViewHandler<Ohlc> prepared =
        [&](const ModelFrame<Ohlc> &frame,
            const std::optional<ModelRequest<Ohlc>> &request) -> std::expected<void, TreeError> {
        check(frame.dataRevision() == DATA_REVISION && !request, "chart scalar adapter lost actual model services");
        ++preparedCalls;

        return {};
      };
      ui::ControlSettings patch{
        .mode = chartMode, .chartGlyphSet = glyphSet, .candleGap = 1, .yRange = {.enabled = range.has_value()}};

      if (range) {
        patch.yRange.minimum = range->minimum;
        patch.yRange.maximum = range->maximum;
      }
      if (path == 0) {
        actual.mode = chartMode;
        actual.glyph_set = glyphSet;
        actual.candle_gap = 1;
        actual.y_range = range;
        take(ui::mount(*scene, ui::vbox(ui::chart(chart, model, accessor, actual, first, prepared))));
      } else if (path == 1) {
        take(ui::mount(
          *scene, ui::vbox(ui::chart(chart, model, accessor,
                                     ui::ChartConfig<Ohlc>{.options = actual, .first = first, .prepared = prepared})
                             .mode(chartMode)
                             .chartGlyphSet(glyphSet)
                             .candleGap(1)
                             .yRange(range))));
      } else if (path == 2) {
        take(
          ui::mount(*scene, ui::vbox((ui::chart(chart, model, accessor, actual, first, prepared) | ui::mode(chartMode) |
                                      ui::chartGlyphSet(glyphSet) | ui::candleGap(1) | ui::yRange(range)))));
      } else if (path == 3) {
        ui::ControlContext controls;

        controls.defaults("Chart", patch).define("Prices", "Chart");
        take(ui::mount(*scene, ui::vbox(ui::chart(chart, model, accessor, actual, first, prepared).variant("Prices")),
                       controls));
      } else {
        ui::SceneResourceOptions options;

        options.format = ui::SceneResourceFormat::CONFIGURATION;
        take(options.registry.add<Chart<Ohlc>>("Bound", [&](ui::SceneResourceArguments arguments) {
          ++factories;
          check(arguments.text == "metadata", "chart family wrapper discarded old text metadata");

          return ui::chart(chart, model, accessor, actual, first, prepared).candleGap(4);
        }));
        const std::string bounds = range ? "enabled=true minimum=" + std::to_string(range->minimum) +
                                             " maximum=" + std::to_string(range->maximum)
                                         : "enabled=false";
        const std::string source =
          "definitions{Prices=Bound{mode=" + std::string(chartMode == ChartMode::LINE ? "LINE" : "CANDLES") +
          " chartGlyphSet=" + (glyphSet == ChartGlyphSet::ASCII ? "ASCII" : "BOX_DRAWING") + " candleGap=1 yRange{" +
          bounds + "}}} scene{VBox{Prices{text=\"metadata\"}}}";

        take(ui::mount(*scene, ui::resource(source, options)));
        check(factories == 1, "chart resource must use its explicit code-bound factory exactly once");
      }

      const auto layout = prepareDataViews(*scene, *app, {7, 5});
      const auto geometry = take(layout.geometry(chart.node())).geometry.local;
      const auto rows = chartMode == ChartMode::LINE ? std::size_t{7} : std::size_t{4};

      take(app->tree().read(chart.node(), [&](const Chart<Ohlc> &view) {
        const auto visible = view.visibleRange();

        if (visible != RowRange{3, rows}) {
          std::cerr << "chart scenario=" << scenario << " path=" << path << " visible={" << visible.first << ','
                    << visible.count << "} expected={3," << rows << "} viewport=" << geometry.size().width << 'x'
                    << geometry.size().height << '\n';
        }

        check(visible == RowRange{3, rows}, "chart mode/gap patch lost actual visible domain interval");
      }));
      check(accesses == rows && preparedCalls == 1 && originCalls == 1,
            "chart scalar adapter changed accessor, preparation or first callback ownership/order");
      const auto cells = chartCells(*app, chart.node(), geometry.size(), actual.width_policy);

      check(std::ranges::any_of(cells,
                                [](const auto &cell) {
                                  return cell.first != " " && cell.second.bold;
                                }),
            "chart scalar patch lost actual manual brush style");
      if (path == 0) {
        baseline = cells;
        baselineBounds = geometry;
        if (scenario == 1) {
          asciiCandles = cells;
        } else if (scenario == 5) {
          check(cells == *asciiCandles, "wide chart glyph policy must preserve native one-cell ASCII fallback");
        }
      } else {
        if (cells != *baseline || geometry != *baselineBounds) {
          std::cerr << "chart scenario=" << scenario << " path=" << path << '\n';
        }

        check(cells == *baseline && geometry == *baselineBounds,
              "native chart options/method/pipe/context/resource paths paint different geometry or cells");
      }
      (void)take(app->tree().consumeFrameRequest());
      prepareDataViews(*scene, *app, {7, 5});
      check(accesses == 2 * rows, "forced chart preparation differs from native projection work");

      // A forced preparation is not idle dispatch. Verify the actual scheduler gate separately.
      // The final chart-only WIDE policy is tested by the explicit manual renderer above.
      if (scenario != 5) {
        std::size_t settledAccesses{}, settledSubmissions{}, checkpoints{};

        terminal.checkpoint = [&](std::size_t next, TerminalRead &) {
          if (next == 0) {
            settledAccesses = accesses;
            settledSubmissions = terminal.submissions;
          } else {
            check(accesses == settledAccesses && terminal.submissions == settledSubmissions,
                  "idle chart event loop prepared or submitted another frame");
          }

          ++checkpoints;
        };

        take(scene->run(terminal));
        check(terminal.closed && checkpoints == 3 && settledSubmissions == 1,
              "chart idle scenario skipped initial presentation, checkpoints or cleanup");
      }
    }
  }
}

void chartRangeFailuresAndPreservation() {
  for (const int scenario : {0, 1, 2, 3}) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::Ref<Chart<Ohlc>> chart;
    int callbacks{};
    ChartOptions options;
    ui::ControlSettings patch;

    if (scenario == 1) {
      options.y_range = ChartRange{0, 2};
      patch.yRange.minimum = 3;
    } else if (scenario == 2) {
      patch.mode = static_cast<ChartMode>(99);
    } else if (scenario == 3) {
      patch.candleGap = -1;
    } else {
      patch.yRange.minimum = 0;
    }
    const auto result = ui::mount(*scene, ui::chart(
                                            chart, model,
                                            [&](const Ohlc &value) {
                                              ++callbacks;

                                              return std::optional{value};
                                            },
                                            options)
                                            .settings(patch));

    check(!result && result.error().phase == ManagedPhase::MOUNT && chart.node() == Node<Chart<Ohlc>>{} &&
            callbacks == 0 && take(app->tree().inspect(scene->root())).children.empty(),
          "invalid chart patch must reject before callbacks and reference publication");
    take(ui::mount(*scene, ui::chart(chart, model, [](const Ohlc &value) {
                             return std::optional{value};
                           }).yRange(ChartRange{1, 1})));
  }
}

void chartSparseRangeMerge() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    std::optional<ChartCells> expected;

    for (const bool sparse : {false, true}) {
      ReplayTerminal terminal;
      auto app = take(Application::create({}, terminal.clock()));
      auto scene = take(ManagedScene::create(*app));
      auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
      ui::Ref<Chart<Ohlc>> chart;
      ChartOptions actual;
      ui::ControlContext controls;
      ui::ControlSettings patch;

      actual.mode = ChartMode::CANDLES;
      actual.candle_gap = 1;
      actual.y_range = ChartRange{90, 120};
      if (!sparse) {
        if (scenario == 1) {
          actual.y_range->minimum = 95;
        } else if (scenario == 2) {
          actual.y_range = ChartRange{};
        } else if (scenario == 3) {
          actual.y_range.reset();
        }
      } else {
        actual.candle_gap = 4;
        patch.candleGap = 1;
        if (scenario == 1) {
          patch.yRange.minimum = 95;
        } else if (scenario == 2) {
          actual.y_range.reset();
          patch.yRange.enabled = true;
        } else if (scenario == 3) {
          controls.defaults("Chart", {.yRange = {.enabled = true, .minimum = 90, .maximum = 120}});
          patch.yRange.enabled = false;
        }
      }
      auto recipe = ui::chart(
        chart, model,
        [](const Ohlc &value) {
          return std::optional{value};
        },
        actual);

      if (sparse) {
        (void)std::move(recipe).settings(patch);
      }

      take(ui::mount(*scene, std::move(recipe), controls));
      prepareDataViews(*scene, *app, {7, 5});
      const auto cells = chartCells(*app, chart.node(), {7, 5});

      if (!sparse) {
        expected = cells;
      } else {
        check(cells == *expected,
              "sparse chart range failed actual options preservation/bound-only/create/inherited clear semantics");
      }
    }
  }
}

void explicitChartSchemas() {
  for (const bool expanded : {false, true}) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::Ref<Chart<Ohlc>> chart;
    ui::SceneResourceOptions options;
    std::vector<std::string_view> paths{"text"};
    int callbacks{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace();
    if (expanded) {
      paths.insert(paths.end(),
                   {"mode", "chartGlyphSet", "candleGap", "yRange.enabled", "yRange.minimum", "yRange.maximum"});
    }
    options.controls->registerType("Bound", paths);
    take(options.registry.add<Chart<Ohlc>>("Bound", [&](ui::SceneResourceArguments arguments) {
      ++callbacks;
      check(arguments.text == "metadata", "old Chart text metadata must remain code-factory owned");
      ChartOptions actual;

      actual.mode = ChartMode::CANDLES;
      actual.candle_gap = 2;
      actual.y_range = ChartRange{90, 120};

      return ui::chart(
        chart, model,
        [](const Ohlc &value) {
          return std::optional{value};
        },
        actual);
    }));
    const std::string patch =
      expanded ? "mode=CANDLES chartGlyphSet=BOX_DRAWING candleGap=1 yRange{enabled=true minimum=90 maximum=120}" : "";
    const std::string source = "definitions{Prices=Bound{" + patch + "}} scene{Prices{text=\"metadata\"}}";

    take(ui::mount(*scene, ui::resource(source, options)));
    prepareDataViews(*scene, *app, {7, 5});
    take(app->tree().read(chart.node(), [&](const Chart<Ohlc> &view) {
      check(view.visibleRange() == RowRange{0, expanded ? std::size_t{4} : std::size_t{3}},
            "Chart old narrow/full expanded schemas changed actual option ranges");
    }));
    check(callbacks == 1, "Chart explicit schema invoked more than one real factory");
  }
}

void chartFactoryLeafAccounting() {
  for (const std::size_t budget : {5U, 6U}) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::Ref<Chart<Ohlc>> chart;
    ui::SceneResourceOptions options;
    int factories{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    take(options.registry.add<Chart<Ohlc>>("Bound", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::chart(chart, model,
                       [](const Ohlc &value) {
                         return std::optional{value};
                       })
        .candleGap(4)
        .chartGlyphSet(ChartGlyphSet::BOX_DRAWING);
    }));
    auto result = ui::mount(
      *scene,
      ui::resource("scene{Bound{mode=CANDLES candleGap=1 yRange{enabled=true minimum=90 maximum=120}}}", options));

    check(factories == 1, "chart budget rejection must follow the actual registered factory changes");
    if (budget == 5) {
      check(!result && result.error().sceneResource &&
              result.error().sceneResource->code == SceneResourceErrorCode::MATERIALIZATION_LIMIT &&
              chart.node() == Node<Chart<Ohlc>>{} && take(app->tree().inspect(scene->root())).children.empty(),
            "new Chart glyph leaf must exhaust the budget without publishing a reference");
    } else {
      take(std::move(result));
      prepareDataViews(*scene, *app, {7, 5});
      take(app->tree().read(chart.node(), [&](const Chart<Ohlc> &view) {
        check(view.visibleRange() == RowRange{0, 4},
              "materialized Chart range leaves were charged twice or factory gap overrode resource");
      }));
    }
  }
}

void chartRangeGroupNames() {
  for (int spelling = 0; spelling < 3; ++spelling) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
    ui::SceneResourceOptions options;
    ui::Ref<Chart<Ohlc>> chart;
    ui::Ref<Label> child;
    int childFactories{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Chart<Ohlc>>(
      "Bound",
      [&](ui::SceneResourceArguments) {
        return ui::chart(chart, model, [](const Ohlc &value) {
          return std::optional{value};
        });
      },
      true));
    if (spelling) {
      take(options.registry.add<Label>("yRange", [&](ui::SceneResourceArguments arguments) {
        ++childFactories;

        return ui::label(arguments.text).reference(child);
      }));
    }
    const std::string range = spelling == 1   ? "yRange{}"
                              : spelling == 2 ? "yRange={enabled=true minimum=90 maximum=120}"
                                              : "yRange{enabled=true minimum=90 maximum=120}";
    const std::string source = "definitions{Prices=Bound{}} scene{Prices{" + range + "}}";

    take(ui::mount(*scene, ui::resource(source, options)));
    check(childFactories == (spelling == 1 ? 1 : 0),
          "scoped Chart range group stole a registered child name or swallowed an empty child");
    check((child.node() != Node<Label>{}) == (spelling == 1), "registered yRange child was not actually retained");
    prepareDataViews(*scene, *app, {7, 5});
  }
  for (const bool declared : {false, true}) {
    ReplayTerminal terminal;
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    ui::SceneResourceOptions options;
    int calls{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    if (declared) {
      take(options.registry.add<Label>("yRange", [&](ui::SceneResourceArguments arguments) {
        ++calls;

        return ui::label(arguments.text);
      }));
    }
    auto result = ui::mount(*scene, ui::resource("scene{VBox{yRange{}}}", options));

    check(declared ? bool(result) && calls == 1 : !result && calls == 0,
          "yRange grouping became a global child name reservation beneath VBox");
  }
}

void originLifetimeAndError() {
  ReplayTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  const auto model = take(scene->model<Ohlc>(std::make_shared<const Prices>()));
  auto owned = std::make_shared<int>(0);
  const std::weak_ptr<int> observed = owned;
  ui::Ref<Chart<Ohlc>> chart;
  auto recipe = ui::chart(
    chart, model,
    [](const Ohlc &value) {
      return std::optional{value};
    },
    ui::ChartConfig<Ohlc>{.first = [owned]() -> std::expected<std::uint64_t, TreeError> {
      ++*owned;

      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }});

  owned.reset();
  check(!observed.expired(), "Named chart lost origin capture before mount");

  const auto node = take(ui::mount(*scene, std::move(recipe)));
  const auto result = scene->run(terminal);

  check(!result && result.error().phase == ManagedPhase::PREPARE && result.error().node == node.id(),
        "Named chart origin lost typed error identity");
  check(std::get<TreeError>(result.error().cause) == TreeError::INVALID_GEOMETRY,
        "Named chart origin callback was not forwarded");
  check(observed.expired(), "Named chart retained captures after run cleanup");
}
} // namespace

int main() {
  // Construction must stay valid without a live scene; model validation belongs to mount.
  const ui::VirtualListConfig<int> listConfig{};
  const ui::ChartConfig<int> chartConfig{};
  auto list = ui::virtualList(ManagedModel<int>{}, {}, listConfig);
  auto chart = ui::chart(ManagedModel<int>{}, {}, chartConfig);
  auto table = ui::virtualTable(ManagedModel<int>{}, {}, ui::VirtualTableConfig<int>{});

  (void)list;
  (void)chart;
  (void)table;

  invalidSettings([](ManagedModel<Ohlc> model) {
    ui::VirtualTableConfig<Ohlc> config{};

    config.options.max_columns = 0;

    return ui::virtualTable(model, virtualConsumer::priceColumns(), config);
  });
  invalidSettings([](ManagedModel<Ohlc> model) {
    ui::VirtualListConfig<Ohlc> config{};

    config.options.row_height = 0;

    return ui::virtualList(
      model,
      [](const ModelRow<Ohlc> &) {
        return virtualConsumer::text("row");
      },
      config);
  });
  invalidSettings([](ManagedModel<Ohlc> model) {
    ui::ChartConfig<Ohlc> config{};

    config.options.candle_gap = -1;

    return ui::chart(
      model,
      [](const Ohlc &value) {
        return std::optional{value};
      },
      config);
  });
  invalidSettings([](ManagedModel<Ohlc> model) {
    VirtualListOptions options;
    options.row_height = 0;
    return ui::virtualList(
             model,
             [](const ModelRow<Ohlc> &) {
               return virtualConsumer::text("row");
             },
             options)
      .overscanRows(0);
  });
  invalidSettings([](ManagedModel<Ohlc> model) {
    return ui::virtualTable(model, priceColumns()).maxColumns(1);
  });
  for (int cap{}; cap < 3; ++cap) {
    invalidSettings([cap](ManagedModel<Ohlc> model) {
      VirtualTableOptions actual;

      if (cap == 0) {
        actual.max_columns = 1;
      } else if (cap == 1) {
        actual.max_prepared_cells = 1;
      } else {
        actual.max_prepared_text_bytes = 1;
      }

      return ui::virtualTable(model, priceColumns(), actual).rowHeight(2).overscanRows(0);
    });
  }
  chartDeclarationRangeValidation();
  chartSparseRangeMerge();
  explicitChartSchemas();
  chartFactoryLeafAccounting();
  chartRangeGroupNames();
  chartScalarParity();
  chartRangeFailuresAndPreservation();
  explicitDataSchemas();
  conflictingDataSchemaRejected();
  dataFactoryLeafAccounting();
  dataScalarParity();
  scalarRejectionAndColdList();
  originLifetimeAndError();
}
