// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/find_model_source.hpp>
#include <portals/ui_find_bar.hpp>

namespace portals::ui {
/// @cond INTERNAL
namespace detail {
template <class T, class Projection> struct FindModelSourceOf : portals::detail::FindModelSource {
  FindModelSourceOf(ManagedModel<T> model, Projection projection)
      : model_(std::move(model)), projection_(std::make_shared<const Projection>(std::move(projection))) {
  }

  FindModelSourceOf(std::shared_ptr<const ModelSnapshot<T>> source, Projection projection)
      : initial_(std::move(source)), projection_(std::make_shared<const Projection>(std::move(projection))),
        owning_(true) {
  }

  std::expected<void, ManagedError> attach(ManagedScene &scene) override {
    if (owning_) {
      return attachSnapshot(scene, std::move(initial_));
    }

    auto valid = portals::detail::ManagedModelAccess::validate(model_, scene.root().id());

    return valid ? std::expected<void, ManagedError>{}
                 : std::unexpected(ManagedError{ManagedPhase::STATE, scene.root().id(), valid.error(), {}, {}});
  }

  std::expected<std::shared_ptr<const portals::detail::FindWorkerSource>, TreeError>
  capture(ManagedScene &scene) override {
    auto snapshot = portals::detail::ManagedModelAccess::snapshot(model_, scene.root().id());

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    if (!worker_ || worker_->pin != *snapshot) {
      worker_ = std::make_shared<const portals::detail::FindWorkerSourceOf<T, Projection>>(*snapshot, projection_);
    }

    return worker_;
  }

protected:
  std::expected<void, ManagedError> attachSnapshot(ManagedScene &scene,
                                                   std::shared_ptr<const ModelSnapshot<T>> snapshot) {
    auto model = scene.model<T>(std::move(snapshot));

    if (!model) {
      return std::unexpected(model.error());
    }

    model_ = *model;

    return {};
  }

private:
  ManagedModel<T> model_;
  std::shared_ptr<const ModelSnapshot<T>> initial_;
  std::shared_ptr<const Projection> projection_;
  std::shared_ptr<const portals::detail::FindWorkerSource> worker_;
  bool owning_{};
};

template <class T, class Projection> struct FindCollectionSourceOf final : FindModelSourceOf<T, Projection> {
  FindCollectionSourceOf(std::expected<RowCollection<T>, TreeError> rows, Projection projection)
      : FindModelSourceOf<T, Projection>(ManagedModel<T>{}, std::move(projection)), rows_(std::move(rows)) {
  }

  std::expected<void, ManagedError> attach(ManagedScene &scene) override {
    if (!rows_) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, scene.root().id(), rows_.error(), {}, {}});
    }

    // Reuse the existing immutable collection adapter; model creation stays in the mount collector.
    auto snapshot = std::make_shared<const portals::detail::CollectionSnapshot<T>>(*rows_, 1);

    return this->attachSnapshot(scene, std::move(snapshot));
  }

private:
  std::expected<RowCollection<T>, TreeError> rows_;
};

template <class Projection, class T>
concept FindProjection = std::is_nothrow_move_constructible_v<Projection> &&
                         std::same_as<std::invoke_result_t<const Projection &, const T &>, std::string_view>;

inline Description<FindBar> modelFindBar(std::shared_ptr<portals::detail::FindModelSource> binding,
                                         std::string_view initial, FindBarConfig config) {
  if (initial.size() > config.editor.options.limits.max_input_bytes ||
      config.editor.options.limits.max_input_bytes > FindLimits::MAX_QUERY_BYTES ||
      !portals::detail::validFindSearchOptions(config.search)) {
    return Access::cold<FindBar>(
      [](ManagedScene &, Node<Widget> parent, MountContext &) -> std::expected<Node<FindBar>, ManagedError> {
        return std::unexpected(ManagedError{ManagedPhase::TEXT, parent.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      });
  }

  return Access::findBar({}, std::string(initial), std::move(config), std::move(binding));
}
} // namespace detail

/// @endcond

/** @brief Search a scene model on the application's background executor; no UI corpus scan or synchronous fallback.
 * @tparam T Immutable owning domain row satisfying ModelSnapshot<T>'s worker-safe read/destruction contract.
 * @tparam Projection Nothrow-movable worker-safe immutable callable returning exactly string_view from const T&.
 * @param model Weak model from this scene; search uses the same base pin as this cycle's prepared views.
 * @param projection Owning projection invoked only on a worker. Its view must borrow from the retained snapshot;
 * do not capture a scene, UI Ref, ModelFrame, coroutine storage, or any UI-affine reachable object.
 * @param initialQuery Ordinary initial query draft, admitted by the standard editor's TextLimits.
 * @param config Editor/navigation configuration and independent worker scan/result budgets.
 * @return Single-use retained FindBar. Model/scene errors fail mount; newest provider failures use searchFailed.
 * Literal overlapping matches require whole source graphemes. Navigation wraps the stored prefix, and truncated
 * means an additional match exists. Old range handles own their bytes; current() fences source/query identity.
 * The runner owns its pool; hosted sessions must inject an executor. Projected byte limits do not bound T's
 * original transitive payload, whose final shared owner may be released on UI or a worker.
 */
template <class T, class Projection>
  requires detail::FindProjection<Projection, T>
Description<FindBar> findBar(ManagedModel<T> model, Projection projection, std::string_view initialQuery = {},
                             FindBarConfig config = {}) {
  return detail::modelFindBar(
    std::make_shared<detail::FindModelSourceOf<T, Projection>>(std::move(model), std::move(projection)), initialQuery,
    std::move(config));
}

/** @brief Create an ordinary scene-owned model for one immutable owning source and search it in the background.
 * @tparam T Immutable domain row satisfying the model-backed overload's worker ownership contract.
 * @tparam Projection Worker-safe immutable projection returning string_view borrowed from T.
 * @param source Non-null shared immutable base snapshot; retained by the ordinary managed model created at mount.
 * @param projection Owning worker-only text projection; the same capture restrictions as ManagedModel apply.
 * @param initialQuery Standard editor query draft; empty is an ordinary state.
 * @param config Existing composition settings and independently bounded search work.
 * @return Retained model-backed FindBar; model creation failures use the existing checked mount/runner collector.
 */
template <class T, class Projection>
  requires detail::FindProjection<Projection, T>
Description<FindBar> findBar(std::shared_ptr<const ModelSnapshot<T>> source, Projection projection,
                             std::string_view initialQuery = {}, FindBarConfig config = {}) {
  return detail::modelFindBar(
    std::make_shared<detail::FindModelSourceOf<T, Projection>>(std::move(source), std::move(projection)), initialQuery,
    std::move(config));
}

/** @brief Search an owning checked collection using its existing immutable model adapter.
 * @tparam T Copy-constructible equality-comparable owning row accepted by RowCollection<T>.
 * @tparam Projection Worker-only immutable callable returning string_view borrowed from T.
 * @param rows Owning collection or its original validation failure; failure is collected at checked mount.
 * @param projection Owning worker-safe text projection, subject to the model overload's capture restrictions.
 * @param initialQuery Ordinary initial query draft; empty by default.
 * @param config Existing editor/navigation settings and independent worker budgets.
 * @return Retained FindBar; creates an ordinary ManagedModel with the existing collection snapshot adapter.
 */
template <std::copy_constructible T, class Projection>
  requires std::equality_comparable<T> && detail::FindProjection<Projection, T>
Description<FindBar> findBar(std::expected<RowCollection<T>, TreeError> rows, Projection projection,
                             std::string_view initialQuery = {}, FindBarConfig config = {}) {
  return detail::modelFindBar(
    std::make_shared<detail::FindCollectionSourceOf<T, Projection>>(std::move(rows), std::move(projection)),
    initialQuery, std::move(config));
}

/** @brief Search an already validated owning collection without copying its domain payload.
 * @tparam T Owning row accepted by RowCollection<T>.
 * @tparam Projection Immutable worker-only string_view projection.
 * @param rows Validated immutable rows, shared with the ordinary model created at mount.
 * @param projection Owning worker-safe projection, never called on UI.
 * @param initialQuery Standard editor query draft, empty by default.
 * @param config Existing composition options and independently bounded search work.
 * @return Existing checked-collection recipe with no alternate runtime or reference family.
 */
template <std::copy_constructible T, class Projection>
  requires std::equality_comparable<T> && detail::FindProjection<Projection, T>
Description<FindBar> findBar(RowCollection<T> rows, Projection projection, std::string_view initialQuery = {},
                             FindBarConfig config = {}) {
  return findBar(std::expected<RowCollection<T>, TreeError>(std::move(rows)), std::move(projection), initialQuery,
                 std::move(config));
}

/** @tparam Source ManagedModel, immutable snapshot, RowCollection or its checked result accepted above.
 * @tparam Projection Worker-only immutable text projection accepted by the selected source overload.
 * @param reference Existing weak FindBar slot published only after complete mount.
 * @param source Owning immutable snapshot or weak managed model from this scene.
 * @param projection Worker-safe text projection, never called on UI.
 * @param initialQuery Standard initial editor query draft.
 * @param config Existing checked composition settings and worker budgets.
 * @return Existing model-backed recipe sharing the ordinary Ref family.
 */
template <class Source, class Projection>
  requires requires(Source source, Projection projection, std::string_view query, FindBarConfig config) {
    portals::ui::findBar(std::move(source), std::move(projection), query, std::move(config));
  }
Description<FindBar> findBar(Ref<FindBar> reference, Source source, Projection projection,
                             std::string_view initialQuery = {}, FindBarConfig config = {}) {
  return findBar(std::move(source), std::move(projection), initialQuery, std::move(config))
    .reference(std::move(reference));
}
} // namespace portals::ui
