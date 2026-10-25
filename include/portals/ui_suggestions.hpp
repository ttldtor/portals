// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/search.hpp>
#include <portals/ui_model_find.hpp>
#include <portals/ui_virtual.hpp>

namespace portals {
namespace ui::detail {
class SuggestionsActivity;
}

/** @brief Owning bounded selection; display bytes survive source and scene retirement. */
class SearchSelection {
public:
  /** @return Stable source identity and generation, never a result ordinal. */
  RowId rowId() const noexcept {
    return result_->rows[position_].id;
  }

  /** @return Original bounded UTF-8 label, borrowed while this selection/result lives. */
  std::string_view text() const noexcept {
    return result_->rows[position_].value.text().bytes();
  }

  /** @return Zero-based original source ordinal, independent of result ranking. */
  std::uint64_t index() const noexcept {
    return result_->rows[position_].value.rank.sourceOrdinal;
  }

  /** @return Exact weak source identity that produced this selection. */
  const SearchSourceIdentity &sourceIdentity() const noexcept {
    return result_->source;
  }

  /** @return UI-affine exact current status, CLOSED/WRONG_THREAD access failures preserved. */
  std::expected<bool, ManagedError> current() const;

private:
  friend class ui::detail::SuggestionsActivity;

  SearchSelection(std::shared_ptr<const SearchResult> result, std::size_t position,
                  std::weak_ptr<ui::detail::SuggestionsActivity> owner, std::thread::id thread)
      : result_(std::move(result)), position_(position), owner_(std::move(owner)), thread_(thread) {
  }

  std::shared_ptr<const SearchResult> result_;
  std::size_t position_{};
  std::weak_ptr<ui::detail::SuggestionsActivity> owner_;
  std::thread::id thread_;
};
} // namespace portals

namespace portals::ui {
/** @brief Ordinary list/editor composition settings; search never commits or replaces editor text. */
struct SuggestionsConfig {
  SearchOptions search{};       ///< Finite worker index/query/output budgets and strategy.
  VirtualListOptions options{}; ///< Existing list sizing/appearance; scene width policy wins.
  SizePolicy policy{};          ///< Existing layout constraints, adjustable by ordinary recipe modifiers.
  Ref<Label> status;            ///< Optional weak label; empty means no status label.
  std::function<void(SearchSelection)> activated; ///< UI-only callback after exact-current list-focused activation.
  std::function<void(SearchError)> failed;        ///< UI-only newest provider failure; draft is preserved.
  std::function<bool()> enabled; ///< Optional internal UI admission predicate; false cancels pending work.
  std::optional<ManagedModel<SearchItem>>
    resultModel; ///< Internal scoped popup output; absent creates an ordinary model.
};

/** @brief Cold index/source recipe consumed by virtualList; contains no mounted widget or worker. */
struct SuggestionsRecipe {
  TextSource query; ///< Explicit draftText()/committedText() identity, resolved inside checked mount.
  std::shared_ptr<portals::detail::FindModelSource>
    source; ///< UI source adapter; only immutable capture crosses workers.
};

/** @brief Describe indexed suggestions from the same scene's immutable model.
 * @tparam T Worker-readable immutable domain row.
 * @tparam Projection Owning immutable nothrow-movable worker-safe callable returning borrowed string_view.
 * @param query Explicit draft or committed text source; search changes no editor value/history.
 * @param source Weak model from this scene; captures the shared prepared pin without a second pin.
 * @param projection Worker-only projection. No UI Ref, scene, frame or reachable UI-affine captures.
 * @return Cold source recipe. Index keys include pin/owner/session/revision/normalization/width.
 */
template <class T, class Projection>
  requires detail::FindProjection<Projection, T>
SuggestionsRecipe suggestions(TextSource query, ManagedModel<T> source, Projection projection) {
  return {std::move(query),
          std::make_shared<detail::FindModelSourceOf<T, Projection>>(std::move(source), std::move(projection))};
}

/** @tparam T Immutable owning domain row. @tparam Projection Worker-only string_view projection.
 * @param query Explicit draft/committed text identity. @param source Non-null immutable owning snapshot.
 * @param projection Owning worker-safe callable; borrowed views cannot escape actual snapshot reads.
 * @return Cold recipe creating an ordinary managed model at checked mount.
 */
template <class T, class Projection>
  requires detail::FindProjection<Projection, T>
SuggestionsRecipe suggestions(TextSource query, std::shared_ptr<const ModelSnapshot<T>> source, Projection projection) {
  return {std::move(query),
          std::make_shared<detail::FindModelSourceOf<T, Projection>>(std::move(source), std::move(projection))};
}

/** @tparam T Owning collection row. @tparam Projection Immutable worker-only string_view projection.
 * @param query Explicit input text identity. @param source Validated collection or original validation error.
 * @param projection Worker-safe projection; no UI-affine captures. @return Cold checked collection recipe.
 */
template <std::copy_constructible T, class Projection>
  requires std::equality_comparable<T> && detail::FindProjection<Projection, T>
SuggestionsRecipe suggestions(TextSource query, std::expected<RowCollection<T>, TreeError> source,
                              Projection projection) {
  return {std::move(query),
          std::make_shared<detail::FindCollectionSourceOf<T, Projection>>(std::move(source), std::move(projection))};
}

/** @tparam T Owning collection row. @tparam Projection Immutable worker-only string_view projection.
 * @param query Explicit input text identity. @param source Immutable owning collection.
 * @param projection Worker-safe projection. @return Cold recipe sharing the existing collection adapter.
 */
template <std::copy_constructible T, class Projection>
  requires std::equality_comparable<T> && detail::FindProjection<Projection, T>
SuggestionsRecipe suggestions(TextSource query, RowCollection<T> source, Projection projection) {
  return suggestions(std::move(query), std::expected<RowCollection<T>, TreeError>(std::move(source)),
                     std::move(projection));
}

/** @param recipe Cold input/index source; no source rows are read on UI.
 * @param config Ordinary list settings and UI-only callbacks. Enter activates only a focused list.
 * @return Existing VirtualList with a separate bounded SearchResult model and preformatted highlights.
 * Requires executor workerStorage capability; unsupported hosts report UNAVAILABLE, without UI cache fallback.
 * Empty query clears display, errors retain draft, and all completions/activation fence exact source and query.
 */
inline Description<VirtualList<SearchItem>> virtualList(SuggestionsRecipe recipe, SuggestionsConfig config = {}) {
  return detail::Access::suggestions(std::move(recipe.query), std::move(recipe.source), std::move(config));
}

/** @param reference Existing weak list observation slot. @param recipe Cold query/model source.
 * @param config List/search options and UI callbacks. @return Ordinary checked single-use list recipe.
 */
inline Description<VirtualList<SearchItem>> virtualList(Ref<VirtualList<SearchItem>> reference,
                                                        SuggestionsRecipe recipe, SuggestionsConfig config = {}) {
  return virtualList(std::move(recipe), std::move(config)).reference(std::move(reference));
}
} // namespace portals::ui
