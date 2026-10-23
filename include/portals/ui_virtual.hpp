// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_construction.hpp>

namespace portals::ui {

template <class Content, class Initial>
Description<DataStateView> detail::Access::dataState(Initial initial, Description<Content> content,
                                                     DataStateConfig config, bool automatic) {
  auto result = cold<DataStateView>([initial, config = std::move(config), automatic](
                                      ManagedScene &scene, Node<Widget> parent, MountContext &context) mutable {
    auto effective = controlDataStateConfig(*context.controlSettings, std::move(config));

    if (effective.showStatus && !effective.resolver && context.dataStateResolver) {
      effective.resolver = *context.dataStateResolver;
    }

    if constexpr (std::same_as<Initial, DataPresentation>) {
      return scene.beginDataState(parent, initial, std::move(effective), automatic);
    } else {
      return scene.beginBoundDataState(parent, initial, std::move(effective));
    }
  });
  layoutChild(result, content);
  result.state_->children.push_back(
    [content = std::move(content), automatic](ManagedScene &scene, Node<Widget> parent,
                                              MountContext &context) mutable -> std::expected<void, ManagedError> {
      const auto finalizers = context.finalize.size();

      auto child = mount(std::move(content), scene, parent, context);

      if (!child) {
        return std::unexpected(child.error());
      }

      if (automatic && context.finalize.size() != finalizers) {
        // Cold virtual bodies attach their model during the existing mount finalization.
        // Publish the relationship afterward, before any reference or preparation escapes.
        context.finalize.push_back([&scene, parent, body = *child] {
          return scene.finishDataState(parent, body, true);
        });

        return {};
      }

      return scene.finishDataState(parent, *child, automatic);
    });
  return configuredControl(std::move(result), "DataStateView");
}

/** @tparam Content Retained widget type mounted as the one body subtree.
 * @param initial Owning explicit state, with nonzero producer generation.
 * @param content Consuming body recipe; independent work is required when REPLACE hides it.
 * @param config Construction-only labels, fixed strip, styles and optional retry.
 * @return Single-use composition; validation/allocation errors preserve ordinary mount rollback.
 */
template <class Content>
Description<DataStateView> dataStateView(DataPresentation initial, Description<Content> content,
                                         DataStateConfig config = {}) {
  return detail::Access::dataState(initial, std::move(content), std::move(config), false);
}

/** @tparam Source Exactly ManagedState<DataPresentation> or readonly ManagedSource<DataPresentation>.
 * @tparam Content Retained body widget type.
 * @param source Weak same-scene one-way observation; no writable alias escapes.
 * @param content Consuming body recipe. @param config Owning immutable presentation settings.
 * @return Single-use bound composition; initial snapshot is validated before publication.
 */
template <class Source, class Content>
  requires(std::same_as<Source, ManagedState<DataPresentation>> ||
           std::same_as<Source, ManagedSource<DataPresentation>>)
Description<DataStateView> dataStateView(Source source, Description<Content> content, DataStateConfig config = {}) {
  return detail::Access::dataState(source, std::move(content), std::move(config), false);
}

/** @tparam View VirtualList/VirtualTable type exposing existing prepared owning facts.
 * @param content Consuming managed virtual view; its provider callback retains ordinary work ownership.
 * @param config Stable strip and optional explicit retry; REPLACE is rejected.
 * @return Automatic composition using the body's successful Prepared without repinning or I/O.
 */
template <class View>
  requires requires(const View &view) {
    { view.dataStatus() } -> std::same_as<DataRangeStatus>;
  }
Description<DataStateView> dataStateView(Description<View> content, DataStateConfig config = {}) {
  return detail::Access::dataState(DataPresentation{DataPhase::LOADING, 1}, std::move(content), std::move(config),
                                   true);
}

/// @brief Describe a managed virtual table without opening a range until mount.
/// @tparam T Immutable model row accepted by the column formatters; deduced from model.
/// @param model Weak UI-affine handle belonging to the scene receiving this recipe.
/// @param columns Descriptions copied now, including owning headers and formatter objects.
/// @param options Existing table settings, validated by managed preparation.
/// @param policy Existing layout constraints for the table.
/// @param prepared Owning provider callback after preparation, including retries.
/// Empty is valid for ready snapshots; pending work without it returns INVALID_ARGUMENT.
/// @return Single-use recipe with standard input, styles and automatic preparation.
/// Construction may throw while copying columns or callbacks. Mount preserves typed errors
/// and subtree rollback. The scene owns the model; reference captures remain borrowed.
template <class T>
Description<VirtualTable<T>> virtualTable(ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          VirtualTableOptions options = {}, SizePolicy policy = {},
                                          std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  // A recipe may outlive the caller's array or vector. Copy before retaining a mount callback.
  auto ownedColumns = std::vector<TableColumn<T>>(columns.begin(), columns.end());

  auto description = detail::Access::cold<VirtualTable<T>>(
    [model, columns = std::move(ownedColumns), options, policy,
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) mutable {
      auto effective = detail::controlVirtualTableOptions(*context.controlSettings, options);

      return scene.virtualTable(parent, model, columns, effective, policy, std::move(prepared));
    });

  return detail::Access::configuredControl(std::move(description), "VirtualTable");
}

/// @brief Describe a managed virtual list without opening a range until mount.
/// @tparam T Immutable model row accepted by the formatter; deduced from model.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param delegate Owning formatter invoked for visible rows during preparation.
/// @param options Existing list settings, validated by managed preparation.
/// @param policy Existing layout constraints for the list.
/// @param prepared Owning provider callback after preparation. Empty rejects pending work
/// with INVALID_ARGUMENT. Captured references must remain valid until retirement.
/// @return Single-use recipe retaining callbacks and standard managed input/style behavior.
/// Capture construction may throw; mount preserves typed errors and subtree rollback.
template <class T>
Description<VirtualList<T>> virtualList(ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        VirtualListOptions options = {}, SizePolicy policy = {},
                                        std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  auto description = detail::Access::cold<VirtualList<T>>(
    [model, delegate = std::move(delegate), options, policy,
     prepared = std::move(prepared)](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) mutable {
      auto effective = detail::controlVirtualListOptions(*context.controlSettings, options);

      return scene.virtualList(parent, model, std::move(delegate), effective, policy, std::move(prepared));
    });

  return detail::Access::configuredControl(std::move(description), "VirtualList");
}

/// @brief Describe a managed chart using the existing frame preparation order.
/// @tparam T Immutable model row projected into optional OHLC values; deduced from model.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param accessor Owning projection; nullopt represents a gap, as in the managed chart API.
/// @param options Existing chart mode, width and glyph configuration.
/// @param first Owning origin callback, invoked in composition registration order during
/// preparation. Empty means row zero. NodeRef observations are already published then.
/// @param prepared Owning provider callback after preparation, including retries. Empty
/// rejects pending work with INVALID_ARGUMENT. Explicit reference captures remain borrowed.
/// @return Single-use recipe with automatic style/preparation and no input binding.
/// Capture construction may throw; mount preserves typed errors and subtree rollback.
template <class T>
Description<Chart<T>> chart(ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                            ChartOptions options = {}, ManagedChartOrigin first = {},
                            std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  auto description = detail::Access::cold<Chart<T>>(
    [model, accessor = std::move(accessor), options, first = std::move(first), prepared = std::move(prepared)](
      ManagedScene &scene, Node<Widget> parent,
      detail::MountContext &context) mutable -> std::expected<Node<Chart<T>>, ManagedError> {
      auto effective = detail::controlChartOptions(*context.controlSettings, options);

      if (!effective) {
        return std::unexpected(detail::error(parent, effective.error()));
      }

      return scene.chart(parent, model, std::move(accessor), *effective, std::move(first), std::move(prepared));
    });

  return detail::Access::configuredControl(std::move(description), "Chart");
}

/// @brief Named owning settings for a table recipe; defaults match the positional factory.
/// @tparam T Immutable model row type, matching the receiving ManagedModel.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct VirtualTableConfig {
  VirtualTableOptions options{};    ///< Existing dimensions, width policy, budgets and styles.
  SizePolicy policy{};              ///< Layout constraints in terminal cells.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/// @brief Named owning settings for a list recipe; defaults match the positional factory.
/// @tparam T Immutable model row type, matching the receiving ManagedModel.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct VirtualListConfig {
  VirtualListOptions options{};     ///< Existing row height, overscan, width policy and styles.
  SizePolicy policy{};              ///< Layout constraints in terminal cells.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/// @brief Named owning settings for a chart recipe; defaults match the positional factory.
/// @tparam T Immutable model row type projected by the chart accessor.
/// Copying captures may throw; explicit reference captures remain borrowed until retirement.
template <class T> struct ChartConfig {
  ChartOptions options{};           ///< Existing plot mode, width, glyphs and styles.
  ManagedChartOrigin first{};       ///< Origin evaluated during preparation; empty starts at row zero.
  ManagedViewHandler<T> prepared{}; ///< Provider callback; empty rejects pending work with INVALID_ARGUMENT.
};

/** @brief Owning cold text collection recipe; attaching happens only during a complete mount. */
struct TextCollectionRecipe {
  TextSource source;        ///< Weak direct caption/draft/committed text identity, including same-attempt pending Ref.
  TextSplitOptions options; ///< Positive token and whole-input limits.
  bool splitWords{};        ///< True for whitespace-delimited tokens; false for extended graphemes.
};

/** @param source Read-only text identity; copying does not read or attach.
 * @param options Whole-input limits and positive token cap.
 * @return Cold whitespace-token recipe; punctuation is preserved, not UAX #29 word-break.
 */
inline TextCollectionRecipe words(TextSource source, TextSplitOptions options = {}) {
  return {std::move(source), options, true};
}

/** @param source Read-only text identity, including a direct Ref reserved in this mount.
 * @param options Whole-input limits and positive token cap.
 * @return Cold recipe with one token per existing extended grapheme, including whitespace.
 */
inline TextCollectionRecipe graphemes(TextSource source, TextSplitOptions options = {}) {
  return {std::move(source), options, false};
}

/** @param recipe Cold text transform retaining owning segments with reset-on-change row IDs.
 * @param config Existing list options and sizing; the default formatter displays each complete token.
 * @return Single-use list recipe; pure initial finalization precedes all public Ref publication.
 * The temporary internal shell cannot prepare or paint before its real model and consumer are installed.
 */
inline Description<VirtualList<TextSegment>> virtualList(TextCollectionRecipe recipe,
                                                         VirtualListConfig<TextSegment> config = {}) {
  auto description = detail::Access::cold<VirtualList<TextSegment>>(
    [recipe = std::move(recipe), config = std::move(config)](
      ManagedScene &scene, Node<Widget> parent,
      detail::MountContext &context) mutable -> std::expected<Node<VirtualList<TextSegment>>, ManagedError> {
      config.options = detail::controlVirtualListOptions(*context.controlSettings, config.options);

      auto graph = std::make_shared<detail::Access::ColdGraph>();
      graph->scene = &scene;
      context.rollback.push_back([graph] {
        graph->retire();
      });
      graph->pending = true;
      detail::Access::beginCold(scene);
      config.options.width_policy = scene.widthPolicy();
      auto node = scene.emplace<VirtualList<TextSegment>>(
        parent, ModelConsumer<TextSegment>{}, ManagedListFormatter<TextSegment>([](const ModelRow<TextSegment> &row) {
          return row.value.text();
        }),
        config.options, config.policy);
      if (!node) {
        return node;
      }
      context.finalize.push_back([&scene, &context, graph, node = *node, recipe = std::move(recipe),
                                  prepared = std::move(config.prepared)]() mutable {
        return detail::Access::completeTextList(scene, node, recipe.source, recipe.splitWords, recipe.options, context,
                                                graph, std::move(prepared));
      });
      return node;
    });

  return detail::Access::configuredControl(std::move(description), "VirtualList");
}

/// @brief Describe a table using named settings and the same owning mount transaction.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly VirtualTableConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param columns Definitions copied now, including headers and formatter captures.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe; mount and preparation preserve the positional factory's typed errors.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy table options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualTableConfig<T>>
Description<VirtualTable<T>>
virtualTable(ManagedModel<T> model, std::type_identity_t<std::span<const TableColumn<T>>> columns, Config &&config) {
  return virtualTable(model, columns, config.options, config.policy, std::forward<Config>(config).prepared);
}

/// @brief Describe a list using named settings without changing its managed lifecycle.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly VirtualListConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param delegate Owning visible-row formatter; reference captures remain borrowed.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe preserving positional mount, preparation and error semantics.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy list options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualListConfig<T>>
Description<VirtualList<T>> virtualList(ManagedModel<T> model, std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        Config &&config) {
  return virtualList(model, std::move(delegate), config.options, config.policy, std::forward<Config>(config).prepared);
}

/// @brief Describe a chart using named settings without changing preparation order.
/// @tparam T Immutable row type deduced from model.
/// @tparam Config Exactly ChartConfig<T>, optionally cv/ref qualified.
/// @param model Weak UI-affine handle belonging to the receiving scene.
/// @param accessor Owning projection to optional OHLC; nullopt represents a gap.
/// @param config Settings copied from lvalues or consumed from rvalues; captures may throw.
/// @return Single-use recipe preserving positional mount, preparation and error semantics.
/// Use an explicitly typed aggregate: bare {} continues to mean legacy chart options.
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, ChartConfig<T>>
Description<Chart<T>> chart(ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                            Config &&config) {
  return chart(model, std::move(accessor), config.options, std::forward<Config>(config).first,
               std::forward<Config>(config).prepared);
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty view observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param columns Column descriptions copied by the original factory.
 * @param options Existing table configuration.
 * @param policy Existing layout constraints.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing table recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<VirtualTable<T>> virtualTable(Ref<VirtualTable<T>> reference, ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          VirtualTableOptions options = {}, SizePolicy policy = {},
                                          std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return virtualTable(model, columns, options, policy, std::move(prepared)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty view observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param delegate Owning row formatter retained by the original recipe.
 * @param options Existing list configuration.
 * @param policy Existing layout constraints.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing list recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<VirtualList<T>> virtualList(Ref<VirtualList<T>> reference, ManagedModel<T> model,
                                        std::type_identity_t<ManagedListFormatter<T>> delegate,
                                        VirtualListOptions options = {}, SizePolicy policy = {},
                                        std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return virtualList(model, std::move(delegate), options, policy, std::move(prepared)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @param reference Empty chart observation published after complete mount.
 * @param model Existing scene-owned model.
 * @param accessor Owning OHLC projection retained by the original recipe.
 * @param options Existing chart configuration.
 * @param first Owning origin callback; empty starts at row zero.
 * @param prepared Owning provider callback; pending ranges require it.
 * @return Existing chart recipe preserving preparation, rollback and provider semantics.
 */
template <class T>
Description<Chart<T>> chart(Ref<Chart<T>> reference, ManagedModel<T> model,
                            std::type_identity_t<ManagedChartAccessor<T>> accessor, ChartOptions options = {},
                            ManagedChartOrigin first = {}, std::type_identity_t<ManagedViewHandler<T>> prepared = {}) {
  return chart(model, std::move(accessor), options, std::move(first), std::move(prepared))
    .reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original VirtualTableConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param columns Owning column descriptions copied by the original factory.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named table recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualTableConfig<T>>
Description<VirtualTable<T>> virtualTable(Ref<VirtualTable<T>> reference, ManagedModel<T> model,
                                          std::type_identity_t<std::span<const TableColumn<T>>> columns,
                                          Config &&config) {
  return virtualTable(model, columns, std::forward<Config>(config)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original VirtualListConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param delegate Owning row formatter retained by the original recipe.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named list recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, VirtualListConfig<T>>
Description<VirtualList<T>> virtualList(Ref<VirtualList<T>> reference, ManagedModel<T> model,
                                        std::type_identity_t<ManagedListFormatter<T>> delegate, Config &&config) {
  return virtualList(model, std::move(delegate), std::forward<Config>(config)).reference(std::move(reference));
}

/** @tparam T Immutable managed model row type.
 * @tparam Config Exactly the original ChartConfig<T>, optionally cv/ref qualified.
 * @param reference Empty observing slot published after complete mount.
 * @param model Existing scene-owned model.
 * @param accessor Owning OHLC projection retained by the original recipe.
 * @param config Original named settings forwarded into the existing recipe.
 * @return Existing named chart recipe with additive reference publication.
 */
template <class T, class Config>
  requires std::same_as<std::remove_cvref_t<Config>, ChartConfig<T>>
Description<Chart<T>> chart(Ref<Chart<T>> reference, ManagedModel<T> model,
                            std::type_identity_t<ManagedChartAccessor<T>> accessor, Config &&config) {
  return chart(model, std::move(accessor), std::forward<Config>(config)).reference(std::move(reference));
}

/** @brief Sparse exact list/table row-layout stage. */
struct VirtualRowsModifier {
  std::optional<std::int32_t> height;  ///< Positive row height.
  std::optional<std::size_t> overscan; ///< Extra requested rows per side; zero is present.

  /** @tparam T Exact native list/table type. @param description Consumed recipe.
   * @param modifier Sparse row options. @return Recipe staging the same checked leaves as its methods.
   */
  template <class T>
    requires(detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value)
  friend Description<T> operator|(Description<T> &&description, VirtualRowsModifier modifier) {
    if (modifier.height) {
      (void)std::move(description).rowHeight(*modifier.height);
    }

    if (modifier.overscan) {
      (void)std::move(description).overscanRows(*modifier.overscan);
    }

    return std::move(description);
  }
};

/** @param value Positive logical-row height in cells. @return Exact native list/table stage. */
inline VirtualRowsModifier rowHeight(std::int32_t value) {
  return {value, {}};
}

/** @param value Additional requested rows per side; zero overrides defaults. @return Exact native list/table stage. */
inline VirtualRowsModifier overscanRows(std::size_t value) {
  return {{}, value};
}

/** @brief Sparse exact table preparation-budget stage. */
struct VirtualTableBudgetModifier {
  std::optional<std::size_t> columns; ///< Positive configured-column cap.
  std::optional<std::size_t> cells;   ///< Positive visible body-cell cap.
  std::optional<std::size_t> bytes;   ///< Positive header/body UTF-8 cap.

  /** @tparam T Exact native table type. @param description Consumed recipe.
   * @param modifier Sparse table budgets. @return Recipe staging the same checked leaves as its methods.
   */
  template <class T>
    requires detail::IsVirtualTable<T>::value
  friend Description<T> operator|(Description<T> &&description, VirtualTableBudgetModifier modifier) {
    if (modifier.columns) {
      (void)std::move(description).maxColumns(*modifier.columns);
    }

    if (modifier.cells) {
      (void)std::move(description).maxPreparedCells(*modifier.cells);
    }

    if (modifier.bytes) {
      (void)std::move(description).maxPreparedTextBytes(*modifier.bytes);
    }

    return std::move(description);
  }
};

/** @param value Positive configured-column cap. @return Exact native table stage. */
inline VirtualTableBudgetModifier maxColumns(std::size_t value) {
  return {value, {}, {}};
}

/** @param value Positive visible body-cell cap. @return Exact native table stage. */
inline VirtualTableBudgetModifier maxPreparedCells(std::size_t value) {
  return {{}, value, {}};
}

/** @param value Positive header/body UTF-8 cap. @return Exact native table stage. */
inline VirtualTableBudgetModifier maxPreparedTextBytes(std::size_t value) {
  return {{}, {}, value};
}

/** @brief Sparse exact chart construction stage; callbacks and providers stay in the recipe. */
struct ChartModifier {
  ControlSettings patch{}; ///< Only the six chart leaves are staged by the named helpers.

  /** @tparam T Exact native chart type. @param description Consumed recipe.
   * @param modifier Owning chart literals. @return Same exact recipe with its sparse patch applied.
   */
  template <class T>
    requires detail::IsChart<T>::value
  friend Description<T> operator|(Description<T> &&description, ChartModifier modifier) {
    detail::Access::chartSettings(description, std::move(modifier.patch));

    return std::move(description);
  }
};

/** @param value Existing native line/candle mode. @return Exact native chart stage. */
inline ChartModifier mode(ChartMode value) {
  return {{.mode = value}};
}

/** @param value Chart-only candle glyph domain. @return Exact native chart stage. */
inline ChartModifier chartGlyphSet(ChartGlyphSet value) {
  return {{.chartGlyphSet = value}};
}

/** @param cells Nonnegative empty columns between candles. @return Exact native chart stage. */
inline ChartModifier candleGap(std::int32_t cells) {
  return {{.candleGap = cells}};
}

/** @param range Finite ordered/equal endpoints, or nullopt for automatic scale. @return Exact native chart stage. */
inline ChartModifier yRange(std::optional<ChartRange> range) {
  ControlChartRangeSettings patch{.enabled = range.has_value()};

  if (range) {
    patch.minimum = range->minimum;
    patch.maximum = range->maximum;
  }

  return {{.yRange = patch}};
}

/** @brief Owning sparse DataStateView construction stage; no expression is resolved here. */
struct DataStateModifier {
  ~DataStateModifier() = default;

  ControlSettings patch{}; ///< Only the ten data-state leaves are staged by the named helpers.

  /** @tparam T Exact native DataStateView type. @param description Consumed recipe.
   * @param modifier Owning status patch. @return Same exact recipe with native checked settings staged.
   */
  template <class T>
    requires std::same_as<T, DataStateView>
  friend Description<T> operator|(Description<T> &&description, DataStateModifier modifier) {
    if (modifier.patch.showStatus) {
      (void)std::move(description).showStatus(*modifier.patch.showStatus);
    }

    if (modifier.patch.statusRows) {
      (void)std::move(description).statusRows(*modifier.patch.statusRows);
    }

    if (modifier.patch.contentPolicy) {
      (void)std::move(description).contentPolicy(*modifier.patch.contentPolicy);
    }

    if (modifier.patch.spinnerInterval) {
      (void)std::move(description).spinnerInterval(*modifier.patch.spinnerInterval);
    }

    (void)std::move(description).labels(std::move(modifier.patch.labels));

    return std::move(description);
  }
};

/** @param value Native status visibility; false suppresses all status-label and timer work. @return Exact native
 * DataStateView stage. */
inline DataStateModifier showStatus(bool value = true) {
  return {{.showStatus = value}};
}

/** @param value Positive status strip rows. @return Exact native DataStateView stage. */
inline DataStateModifier statusRows(std::int32_t value) {
  return {{.statusRows = value}};
}

/** @param value Existing native body policy. @return Exact native DataStateView stage. */
inline DataStateModifier contentPolicy(DataContentPolicy value) {
  return {{.contentPolicy = value}};
}

/** @param value Positive full-width monotonic milliseconds. @return Exact native DataStateView stage. */
inline DataStateModifier spinnerInterval(SchedulerTime value) {
  return {{.spinnerInterval = value}};
}

/** @param values Owning sparse status expressions. @return Exact native DataStateView stage. */
inline DataStateModifier labels(ControlDataStateLabelSettings values) {
  return {{.labels = std::move(values)}};
}

} // namespace portals::ui
