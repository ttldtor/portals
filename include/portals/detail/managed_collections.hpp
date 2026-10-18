// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

// Included after ManagedScene; no publication endpoint or writable source escapes an adapter.
namespace portals {
namespace detail {
/// @cond INTERNAL
template <class T> class CollectionSnapshot final : public ModelSnapshot<T> {
public:
  CollectionSnapshot(RowCollection<T> rows, std::uint64_t revision)
      : ModelSnapshot<T>(revision, rows.rows().size()), rows_(std::move(rows)) {
  }

  ModelRows<T> read(RowRange range) const override {
    return {RangeState::READY, rows_.rows().subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    return rows_.locate(id);
  }

private:
  RowCollection<T> rows_;
};

inline std::expected<RowCollection<TextSegment>, TreeError> segmentRows(const TextSegments &segments,
                                                                        std::uint64_t generation, std::size_t maxRows) {
  if (segments.size() > maxRows) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
  try {
    std::vector<ModelRow<TextSegment>> rows(std::initializer_list<ModelRow<TextSegment>>{});
    rows.reserve(segments.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
      rows.push_back({RowId{static_cast<std::uint64_t>(i) + 1, generation}, *segments.at(i)});
    }
    return RowCollection<TextSegment>::from(rows, CollectionLimits{maxRows});
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

/// @endcond
} // namespace detail

template <class T, class Collection, class MakeRows>
std::expected<ManagedModel<T>, ManagedError>
ManagedScene::collectionModel(ManagedState<Collection> input, CollectionModelOptions options, MakeRows makeRows) {
  auto allowed = boundary(ManagedPhase::STATE, root_);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  const auto fail = [&](TreeError cause) {
    return std::unexpected(error(ManagedPhase::STATE, root_, cause));
  };
  auto admitted = observerAdmission(root_);
  if (!admitted) {
    return std::unexpected(admitted.error());
  }
  auto property = input.lock();
  if (!property || input.scene_ != root_.id() || !options.maxRows) {
    return fail(!property ? property.error() : TreeError::INVALID_ARGUMENT);
  }
  if (models_.size() >= options_.maxModels) {
    return fail(TreeError::NODE_LIMIT);
  }
  try {
    auto initial = (*property)->snapshot();
    if (!initial) {
      return fail(initial.error());
    }
    auto rows = makeRows((*initial)->value, 1, options.maxRows);
    if (!rows) {
      return fail(rows.error());
    }
    auto snapshot = std::make_shared<detail::CollectionSnapshot<T>>(std::move(*rows), 1);
    auto created = Model<T>::create(app_.tree(), root_, std::move(snapshot), options.model);
    if (!created) {
      return fail(created.error());
    }
    auto source = std::make_shared<detail::ManagedModelSourceOf<T>>();
    source->model = std::move(*created);
    source->scene = root_.id();
    source->sinkOnly = true;
    source->origin = input.origin_;
    source->sync = [weak = std::weak_ptr(source), property = std::weak_ptr(*property), origin = input.origin_,
                    makeRows = std::move(makeRows), options, applied = (*initial)->revision,
                    generation = std::uint64_t{1}]() mutable -> std::expected<bool, TreeError> {
      // Check origin before source lookup. Last good pins/outputs are independent owning values.
      if (origin) {
        auto live = origin->check();
        if (!live) {
          return origin->retired || origin->closed ? std::expected<bool, TreeError>(false)
                                                   : std::unexpected(live.error());
        }
      }
      auto target = weak.lock();
      auto input = property.lock();
      if (!target || !input) {
        return std::unexpected(TreeError::CLOSED);
      }
      auto current = input->snapshot();
      if (!current) {
        return std::unexpected(current.error());
      }
      if ((*current)->revision == applied) {
        return false;
      }
      if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
      }
      try {
        auto rows = makeRows((*current)->value, generation + 1, options.maxRows);
        if (!rows) {
          return std::unexpected(rows.error());
        }
        auto next = std::make_shared<detail::CollectionSnapshot<T>>(std::move(*rows), generation + 1);
        if (origin) {
          auto live = origin->check();
          if (!live) {
            return origin->retired || origin->closed ? std::expected<bool, TreeError>(false)
                                                     : std::unexpected(live.error());
          }
        }
        auto committed = Model<T>::applySnapshot(*target->model->state_, std::move(next));
        if (!committed) {
          return std::unexpected(committed.error());
        }
        // Reset IDs advance only with a successful model commit, never with failed preparation.
        ++generation;
        applied = (*current)->revision;
        return true;
      } catch (const std::bad_alloc &) {
        return std::unexpected(TreeError::NO_MEMORY);
      } catch (...) {
        return std::unexpected(TreeError::CALLBACK_FAILED);
      }
    };
    auto pinned = source->pin();
    if (!pinned) {
      return fail(pinned.error());
    }
    ManagedModel<T> result;
    result.source_ = source;
    result.owner_ = owner_;
    models_.push_back(std::move(source));
    return result;
  } catch (const std::bad_alloc &) {
    return fail(TreeError::NO_MEMORY);
  } catch (...) {
    return fail(TreeError::CALLBACK_FAILED);
  }
}

template <class Source>
  requires detail::ManagedCollectionSource<Source>
std::expected<ManagedModel<typename detail::CollectionSource<std::remove_cvref_t<Source>>::Row>, ManagedError>
ManagedScene::model(Source &&source, CollectionModelOptions options) {
  using T = typename detail::CollectionSource<std::remove_cvref_t<Source>>::Row;
  return collectionModel<T>(
    source.state_, options,
    [](const RowCollection<T> &rows, std::uint64_t, std::size_t maxRows) -> std::expected<RowCollection<T>, TreeError> {
      return rows.rows().size() <= maxRows ? std::expected<RowCollection<T>, TreeError>(rows)
                                           : std::unexpected(TreeError::NODE_LIMIT);
    });
}
} // namespace portals
