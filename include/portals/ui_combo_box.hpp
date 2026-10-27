// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/combo_box.hpp>
#include <portals/ui_scene_pipeline.hpp>
#include <portals/ui_suggestions.hpp>

namespace portals::ui {
/** @brief Independent finite browse, popup and indexed-completion settings. */
struct ComboBoxConfig {
  ~ComboBoxConfig() = default;
  LineEditConfig editor{
    .options = {.limits = {4096, 4096, 4096,
                           8192}}}; ///< Normal editor modes, validation and history; protected display is rejected.
  SearchOptions search{.strategy = SearchStrategy::PREFIX}; ///< Finite search budgets; PREFIX by default.
  std::size_t pageRows{32};                                 ///< Positive browse page cap, at most256.
  std::int32_t popupRows{8};                                ///< Positive visible list height in terminal rows.
  std::int32_t popupWidth{60};                              ///< Positive maximum panel width in terminal cells.
  bool searchable{true};                                    ///< DropDown popup offers a separate ordinary query editor.
  std::optional<std::uint64_t> initial;    ///< Optional source ordinal resolved asynchronously after mount.
  std::function<void(SearchError)> failed; ///< Newest provider failure; draft and accepted value survive.
  std::size_t browseScratchBytes{8 * 1024 *
                                 1024}; ///< Transient projection pins/range metadata; output has row/text caps.
};

/** @brief Owning choice matching stage, preserving every worker budget. */
struct ChoiceSearchModifier {
  bool enabled; ///< True selects FUZZY_PREFIX; false selects PREFIX.

  /** @tparam T ComboBox-derived control type, including DropDown.
   * @param description Consumed choice recipe. @param modifier Matching policy.
   * @return Recipe with only its matching strategy overridden.
   */
  template <std::derived_from<ComboBox> T>
  friend Description<T> operator|(Description<T> &&description, ChoiceSearchModifier modifier) {
    return std::move(description).fuzzy(modifier.enabled);
  }
};

/** @param enabled Allow one-edit prefix matching; false restores exact normalized prefixes.
 * @return Owning choice strategy stage, retaining all search budgets.
 */
inline ChoiceSearchModifier fuzzy(bool enabled = true) {
  return {enabled};
}

/** @return Owning choice stage selecting exact normalized label/token prefixes. */
inline ChoiceSearchModifier prefix() {
  return {false};
}

namespace detail {
/// @cond INTERNAL
Description<ComboBox> comboBoxRecipe(std::shared_ptr<portals::detail::FindModelSource>, ComboBoxConfig);
Description<DropDown> dropDownRecipe(std::shared_ptr<portals::detail::FindModelSource>, ComboBoxConfig);
std::shared_ptr<portals::detail::FindModelSource> choiceStrings(std::initializer_list<std::string_view>);
std::expected<void, ManagedError> choiceSelectHandler(const std::shared_ptr<RefTarget> &,
                                                      std::function<void(ChoiceSelection)>, std::source_location);

template <class Projection> struct ChoiceProjection {
  Projection projection;
  std::size_t maxBytes{4096};

  template <class T>
    requires(std::same_as<std::remove_cvref_t<std::invoke_result_t<const Projection &, const T &>>, std::string> ||
             std::same_as<std::invoke_result_t<const Projection &, const T &>, std::string_view> ||
             (std::is_lvalue_reference_v<std::invoke_result_t<const Projection &, const T &>> &&
              std::convertible_to<std::invoke_result_t<const Projection &, const T &>, std::string_view>))
  auto operator()(const T &row) const {
    using Result = std::invoke_result_t<const Projection &, const T &>;
    if constexpr (std::same_as<std::remove_cvref_t<Result>, std::string> && !std::is_lvalue_reference_v<Result>) {
      auto text = std::invoke(projection, row);
      if (text.size() > maxBytes) {
        throw portals::detail::ProjectionLimit{};
      }
      return text;
    } else {
      return std::string_view(std::invoke(projection, row));
    }
  }
};

template <class Projection, class T>
concept ChoiceProjectionFor = std::is_nothrow_move_constructible_v<Projection> &&
                              (std::same_as<std::invoke_result_t<const Projection &, const T &>, std::string_view> ||
                               std::same_as<std::invoke_result_t<const Projection &, const T &>, std::string>);

template <class T, class Projection>
  requires ChoiceProjectionFor<Projection, T>
auto choiceSource(ManagedModel<T> source, Projection projection) {
  return std::make_shared<FindModelSourceOf<T, Projection>>(std::move(source), std::move(projection));
}

template <class T, class Projection>
  requires ChoiceProjectionFor<Projection, T>
auto choiceSource(std::shared_ptr<const ModelSnapshot<T>> source, Projection projection) {
  return std::make_shared<FindModelSourceOf<T, Projection>>(std::move(source), std::move(projection));
}

template <class T, class Projection>
  requires ChoiceProjectionFor<Projection, T>
auto choiceSource(std::expected<RowCollection<T>, TreeError> source, Projection projection) {
  return std::make_shared<FindCollectionSourceOf<T, Projection>>(std::move(source), std::move(projection));
}

template <class T, class Projection>
  requires ChoiceProjectionFor<Projection, T>
auto choiceSource(RowCollection<T> source, Projection projection) {
  return choiceSource(std::expected<RowCollection<T>, TreeError>(std::move(source)), std::move(projection));
}

template <class Source, class Projection>
concept ChoiceSource = requires(Source source, Projection projection) {
  choiceSource(std::move(source), ChoiceProjection<Projection>{std::move(projection)});
};
/// @endcond
} // namespace detail

/** @brief Describe an editable choice over an existing immutable model, snapshot or owning collection.
 * @param source Immutable worker-readable source; no source rows are scanned on UI.
 * @param projection Worker-safe callable returning std::string_view or an owning std::string, or string-like member
 * pointer.
 * @param config Existing editor modes and finite worker/popup limits.
 * @return Ordinary single-use recipe; references publish when the main control, source and activity are ready.
 * The lazy popup identity is empty until opening mounts its private graph.
 */
template <class Source, class Projection>
  requires detail::ChoiceSource<Source, Projection>
Description<ComboBox> comboBox(Source source, Projection projection, ComboBoxConfig config = {}) {
  auto binding = detail::choiceSource(
    std::move(source), detail::ChoiceProjection<Projection>{std::move(projection), config.search.maxRowBytes});
  return detail::comboBoxRecipe(std::move(binding), std::move(config));
}

/** @copydoc comboBox */
template <class Source, class Projection>
  requires detail::ChoiceSource<Source, Projection>
Description<DropDown> dropDown(Source source, Projection projection, ComboBoxConfig config = {}) {
  auto binding = detail::choiceSource(
    std::move(source), detail::ChoiceProjection<Projection>{std::move(projection), config.search.maxRowBytes});
  return detail::dropDownRecipe(std::move(binding), std::move(config));
}

/** @param values Small owning convenience source, copied now; maximum4096 rows and4096 bytes per label.
 * @param config Finite editor, browse, search and popup settings.
 * @return Editable choice; generated stable IDs are private to this immutable collection.
 */
inline Description<ComboBox> comboBox(std::initializer_list<std::string_view> values, ComboBoxConfig config = {}) {
  return detail::comboBoxRecipe(detail::choiceStrings(values), std::move(config));
}

/** @copydoc comboBox(std::initializer_list<std::string_view>, ComboBoxConfig) */
inline Description<DropDown> dropDown(std::initializer_list<std::string_view> values, ComboBoxConfig config = {}) {
  return detail::dropDownRecipe(detail::choiceStrings(values), std::move(config));
}

/** @param reference Ordinary weak observation published only after complete mount.
 * @param source Immutable worker-readable source. @param projection Worker-only label projection.
 * @param config Finite settings. @return Model-backed reference-first choice recipe.
 */
template <class Source, class Projection>
  requires detail::ChoiceSource<Source, Projection>
Description<ComboBox> comboBox(Ref<ComboBox> reference, Source source, Projection projection,
                               ComboBoxConfig config = {}) {
  return comboBox(std::move(source), std::move(projection), std::move(config)).reference(std::move(reference));
}

/** @copydoc comboBox(Ref<ComboBox>, Source, Projection, ComboBoxConfig) */
template <class Source, class Projection>
  requires detail::ChoiceSource<Source, Projection>
Description<DropDown> dropDown(Ref<DropDown> reference, Source source, Projection projection,
                               ComboBoxConfig config = {}) {
  return dropDown(std::move(source), std::move(projection), std::move(config)).reference(std::move(reference));
}

/** @param reference Weak observation. @param values Small copied string source. @param config Finite settings.
 * @return Reference-first owning string convenience recipe.
 */
inline Description<ComboBox> comboBox(Ref<ComboBox> reference, std::initializer_list<std::string_view> values,
                                      ComboBoxConfig config = {}) {
  return comboBox(values, std::move(config)).reference(std::move(reference));
}

/** @copydoc comboBox(Ref<ComboBox>, std::initializer_list<std::string_view>, ComboBoxConfig) */
inline Description<DropDown> dropDown(Ref<DropDown> reference, std::initializer_list<std::string_view> values,
                                      ComboBoxConfig config = {}) {
  return dropDown(values, std::move(config)).reference(std::move(reference));
}

/** @brief Append an ordinary scene-level post-acceptance callback; no manual subscription token.
 * @param reference Published choice observation. @param callback Owning callback receiving accepted selection.
 * @param origin Registration site retained by the existing collector.
 * @return Existing scene pipeline stage; callback failure preserves the published selection.
 */
template <std::derived_from<ComboBox> T, class Callback = detail::QuitCommand>
  requires portals::detail::ManagedAction<Callback, ChoiceSelection>
auto onSelect(Ref<T> reference, Callback callback, std::source_location origin = std::source_location::current()) {
  detail::commandOrigin(callback, origin);
  return detail::SceneStage{[registration = [reference = std::move(reference), callback = std::move(callback),
                                             origin](ManagedScene &scene) mutable -> std::expected<void, ManagedError> {
    auto adapter = detail::Access::choiceHandler(scene, reference.node(), std::move(callback), origin);

    return detail::choiceSelectHandler(detail::NamedAccess::target(reference), std::move(adapter), origin);
  },
                             origin](Scene &owner) mutable {
    detail::SceneAccess::onEditing(owner, registration, origin);
  }};
}
} // namespace portals::ui
