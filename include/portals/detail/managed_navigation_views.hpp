// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

// Included after ManagedScene and its navigation records are complete.
namespace portals {
namespace detail {
/// @cond INTERNAL
inline bool hasModelRestoreIntent(const ModelViewCheckpoint &value) noexcept {
  return value.current || value.reading || !value.selection.rows().empty() ||
         unresolvedPosition(value.currentResolution) || unresolvedPosition(value.readingResolution) ||
         unresolvedPosition(value.selection.resolution());
}

/// @endcond
} // namespace detail

template <class T>
std::expected<ManagedNavigation, ManagedError> ManagedScene::navigation(ManagedModel<T> model,
                                                                        NavigationOptions options) {
  auto source = modelSource(model, root_);

  if (!source) {
    return std::unexpected(source.error());
  }
  if ((*source)->navigationResource.identity.instance) {
    const auto resource = (*source)->navigationResource;

    return makeModelNavigation(model, resource, resource.identity, options, false);
  }

  auto resource = ModelResource::create();

  return resource ? makeModelNavigation(model, *resource, resource->identity, options, false)
                  : std::unexpected(error(ManagedPhase::STATE, root_, detail::navigationTreeError(resource.error())));
}

template <class T>
std::expected<ManagedNavigation, ManagedError> ManagedScene::navigation(ManagedModel<T> model, ModelResource resource,
                                                                        SourceStamp session,
                                                                        NavigationOptions options) {
  return makeModelNavigation(model, resource, session, options, true);
}

template <class T>
std::expected<ManagedNavigation, ManagedError>
ManagedScene::makeModelNavigation(ManagedModel<T> model, ModelResource resource, SourceStamp session,
                                  NavigationOptions options, bool explicitDomain) {
  auto source = modelSource(model, root_);

  if (!source) {
    return std::unexpected(source.error());
  }
  if ((*source)->navigationResource.identity.instance && (*source)->navigationResource != resource) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::INVALID_ARGUMENT));
  }

  auto pinned = (*source)->pin();

  if (!pinned) {
    return std::unexpected(error(ManagedPhase::STATE, root_, pinned.error()));
  }

  try {
    detail::ManagedNavigationRecord::Capture capture =
      [](const std::shared_ptr<detail::ManagedNavigationRecord> &record,
         RowId id) -> std::expected<ModelBookmark, TreeError> {
      auto source = std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(record->model.lock());

      if (!source || source->retired || !source->frame) {
        return std::unexpected(TreeError::CLOSED);
      }
      if (!source->frame->dataRevision()) {
        if (record->bookmark && record->bookmark->resource() != record->resource) {
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
        auto unavailable = record->setPosition(record->bookmark, PositionResolution::UNSUPPORTED);
        return std::unexpected(unavailable ? TreeError::WOULD_BLOCK : unavailable.error());
      }

      auto location = source->frame->locateRow(id);

      if (!location) {
        return std::unexpected(location.error());
      }
      if (location->state != RowLookupState::FOUND) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }

      auto bookmark = ModelBookmark::create(
        record->resource,
        {PositionKind::ITEM, id, location->index, source->frame->dataRevision(), PositionAffinity::FORWARD}, {},
        record->options.bookmarks);

      return bookmark ? std::expected<ModelBookmark, TreeError>(std::move(*bookmark))
                      : std::unexpected(detail::navigationTreeError(bookmark.error()));
    };
    std::function<std::expected<void, TreeError>(const std::shared_ptr<detail::ManagedNavigationRecord> &)> sync =
      [](const auto &record) -> std::expected<void, TreeError> {
      auto source = std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(record->model.lock());

      if (!source || source->retired || !source->frame) {
        record->close();

        return {};
      }
      if (source->origin) {
        auto live = source->origin->check();

        if (!live) {
          if (source->origin->retired || source->origin->closed) {
            record->close();

            return {};
          }

          return std::unexpected(live.error());
        }
      }

      const auto frame = *source->frame;
      const auto revision = frame.dataRevision();
      const auto publication = frame.revision();

      if (record->history && record->history->pending) {
        return record->owner->template retryCanonicalRestore<T>(record, source);
      }
      if (record->frameRevision == publication) {
        return {};
      }
      if (!revision) {
        // Revision zero is valid for data-only views, but cannot qualify a bookmark.
        record->frameRevision = publication;
        return {};
      }

      auto resolver = FinitePositionResolver<T>::create(record->resource, record->session, frame.snapshot(),
                                                        record->changes.toRevision() == revision ? record->changes
                                                                                                 : PositionChangeMap{});

      if (!resolver) {
        return std::unexpected(detail::navigationTreeError(resolver.error()));
      }

      record->cancel();
      for (bool reading : {false, true}) {
        auto bookmark = reading ? record->readingBookmark : record->bookmark;

        if (record->closed) {
          return {};
        }
        if (!bookmark) {
          continue;
        }

        auto channel = (reading ? record->reading : record->current).snapshot();

        if (!channel) {
          return std::unexpected(channel.error());
        }

        const auto outcome = (*channel)->value.resolution;
        const bool completed = outcome == PositionResolution::EXACT || outcome == PositionResolution::NEAREST;

        // Preserve an earlier successful channel while retrying unfinished work on a newer publication.
        if (bookmark->position().revision == revision && completed) {
          continue;
        }

        const auto operation = record->operation;
        auto position = bookmark->position();
        std::optional<PositionReply> reply;

        if (bookmark->resource() == record->resource && position.kind == PositionKind::ITEM &&
            position.revision <= revision && bookmark->metadata().empty()) {
          auto location = frame.locateRow(position.row);

          if (!location) {
            return std::unexpected(location.error());
          }
          if (record->closed || operation != record->operation) {
            return {};
          }
          if (location->state == RowLookupState::FOUND) {
            // An indexed surviving identity needs no row payload or completion overlay.
            position.offset = location->index;
            position.revision = revision;
            reply = PositionReply{record->resource, record->session, revision, 1, PositionResolution::EXACT, position};
          }
        }
        if (!reply) {
          PositionRequest request{record->session, revision, 1, *bookmark};
          auto resolved = resolver->resolve(request);

          if (!resolved) {
            return std::unexpected(detail::navigationTreeError(resolved.error()));
          }
          if (record->closed || operation != record->operation) {
            return {};
          }

          reply = std::move(*resolved);
        }

        if (reply->state == PositionResolution::EXACT || reply->state == PositionResolution::NEAREST) {
          auto updated =
            ModelBookmark::create(record->resource, reply->position, bookmark->metadata(), record->options.bookmarks);

          if (!updated) {
            return std::unexpected(detail::navigationTreeError(updated.error()));
          }

          bookmark = std::move(*updated);
        } else if (reply->state == PositionResolution::EMPTY) {
          bookmark.reset();
        }

        auto applied = record->setPosition(std::move(bookmark), reply->state, reading);

        if (!applied) {
          return applied;
        }
      }

      record->frameRevision = publication;

      return {};
    };
    auto captureOwner = std::make_shared<detail::ManagedNavigationRecord::Capture>(std::move(capture));
    auto syncOwner = std::make_shared<detail::ManagedNavigationRecord::Synchronize>(std::move(sync));
    auto result = navigation(resource, session, options);

    if (!result) {
      return result;
    }

    auto record = result->record_.lock();

    if (!(*source)->navigationResource.identity.instance) {
      (*source)->navigationResource = resource;
    }
    (*source)->navigationExplicit = (*source)->navigationExplicit || explicitDomain;

    record->finite = true;
    record->model = *source;
    record->frameRevision = (*source)->frame->revision();
    record->capture = std::move(captureOwner);
    record->synchronize = std::move(syncOwner);

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NO_MEMORY));
  }
}

template <class T>
std::expected<void, ManagedError> ManagedScene::bindNavigation(Node<VirtualList<T>> node, ManagedNavigation navigation,
                                                               NavigationBindingOptions options) {
  return bindNavigationView<T>(node, std::move(navigation), options);
}

template <class T>
std::expected<void, ManagedError> ManagedScene::bindNavigation(Node<VirtualTable<T>> node, ManagedNavigation navigation,
                                                               NavigationBindingOptions options) {
  return bindNavigationView<T>(node, std::move(navigation), options);
}

template <class T, class W>
std::expected<void, ManagedError> ManagedScene::bindNavigationView(Node<W> node, ManagedNavigation navigation,
                                                                   NavigationBindingOptions options) {
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  auto record = navigation.lock();
  auto entry = find(node);
  auto source = entry ? std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(entry->viewSource.lock()) : nullptr;

  if (!record || !source || (*record)->scene != root_.id() || !(*record)->finite ||
      (!options.current && !options.readingAnchor) ||
      (options.projection != NavigationProjection::SAME_MODEL &&
       options.projection != NavigationProjection::PRESERVES_ROW_IDS) ||
      (options.projection == NavigationProjection::SAME_MODEL && (*record)->model.lock() != source)) {
    return std::unexpected(error(ManagedPhase::BIND, node, !record ? record.error() : TreeError::INVALID_ARGUMENT));
  }
  if (std::ranges::any_of(navigation_->bindings, [&](const auto &binding) {
        return binding->node == node;
      })) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  struct BindingState {
    std::optional<std::uint64_t> offset;
    std::uint64_t modelRevision{};
    std::uint64_t currentRevision{};
    std::uint64_t readingRevision{};
    std::optional<std::uint64_t> input;
    std::uint64_t inputRevision{};
    std::optional<std::uint64_t> pendingInput;
    std::uint64_t pendingSourceRevision{};
    std::uint64_t pendingCurrentRevision{};
    bool readingInput{};
    bool ensurePending{};
  };

  try {
    auto state = std::make_shared<BindingState>();
    auto binding = std::make_shared<detail::ManagedNavigationBinding>();

    binding->node = node;
    binding->context = *record;
    binding->options = options;
    binding->stage = [this, node, options](const auto &base, ModelRebindOptions policy, const auto &history,
                                           const auto *position, const void *override) {
      auto source = std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(base);
      if (!source) {
        return std::expected<std::unique_ptr<detail::PreparedModelViewChange>, ManagedError>(
          std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT)));
      }
      auto prepared = override ? *static_cast<const std::optional<ManagedViewHandler<T>> *>(override)
                               : std::optional<ManagedViewHandler<T>>{};
      return prepareViewModelChange<T>(node, source, std::move(prepared), policy, history, position, &options);
    };
    binding->installed = [this, node, navigation, state, options](const detail::ModelViewCheckpoint *value,
                                                                  const detail::ModelRestoreChannels *channels) {
      auto record = navigation.record_.lock();
      auto entry = find(node);
      auto source =
        entry ? std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(entry->viewSource.lock()) : nullptr;
      auto preparation = entry ? entry->viewPrepare.template target<ViewPreparation<T, W>>() : nullptr;
      if (value && channels) {
        if (!record || !preparation || !preparation->restoration || !preparation->restoration->pending) {
          return;
        }
        auto restoration = preparation->restoration;
        if (channels->current && options.current) {
          restoration->pending->current = value->current;
          restoration->pending->currentResolution = value->currentResolution;
          if (auto current = record->current.snapshot()) {
            state->currentRevision = (*current)->revision;
            restoration->currentRevision = (*current)->revision;
          }
        }
        if (channels->reading && options.readingAnchor) {
          restoration->pending->reading = value->reading;
          restoration->pending->readingResolution = value->readingResolution;
          if (auto reading = record->reading.snapshot()) {
            state->readingRevision = (*reading)->revision;
            restoration->readingRevision = (*reading)->revision;
          }
        }
        if (channels->selection) {
          restoration->pending->selection = value->selection;
          restoration->pending->selectedPositions = value->selectedPositions;
          if (auto selection = record->selection.snapshot()) {
            restoration->selectionRevision = (*selection)->revision;
          }
        }
        return;
      }
      *state = {};
      if (record && source && source->frame) {
        state->modelRevision = source->frame->dataRevision();
        if (auto current = record->current.snapshot()) {
          state->currentRevision = (*current)->revision;
        }
        if (auto reading = record->reading.snapshot()) {
          state->readingRevision = (*reading)->revision;
        }
        if (preparation && preparation->restoration) {
          preparation->restoration->context = record;
          preparation->restoration->currentRevision = state->currentRevision;
          preparation->restoration->readingRevision = state->readingRevision;
          if (auto selection = record->selection.snapshot()) {
            preparation->restoration->selectionRevision = (*selection)->revision;
          }
        }
      }
    };
    binding->before = [this, node, navigation, options, state]() -> std::expected<void, TreeError> {
      auto record = navigation.lock();
      auto entry = find(node);
      auto source =
        entry ? std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(entry->viewSource.lock()) : nullptr;

      if (!record || !source || source->retired || !source->frame) {
        return !record && record.error() != TreeError::CLOSED ? std::unexpected(record.error())
                                                              : std::expected<void, TreeError>{};
      }

      if (!source->frame->dataRevision()) {
        return {};
      }
      std::uint64_t offset{};
      auto read = app_.tree().read(node, [&](const W &view) {
        state->input = view.intent_;
        state->inputRevision = view.intent_revision_;
        offset = view.scrollState().rasterOffset();
      });

      if (!read) {
        return read;
      }

      const auto revision = source->frame->dataRevision();

      bool validInput = state->input && state->inputRevision == revision;
      const bool scrolled =
        state->offset && *state->offset != offset && !validInput && state->modelRevision == revision;

      state->readingInput = options.readingAnchor && scrolled;
      if (scrolled) {
        state->ensurePending = false;
      }
      auto current = (*record)->current.snapshot();
      auto reading = (*record)->reading.snapshot();

      if (!current || !reading) {
        return std::unexpected(!current ? current.error() : reading.error());
      }

      bool staleInput{};
      if (validInput) {
        const bool samePending =
          state->pendingInput == state->input && state->pendingSourceRevision == state->inputRevision;

        staleInput = samePending && state->pendingCurrentRevision != (*current)->revision;
        if (staleInput) {
          validInput = false;
          state->input.reset();
          state->pendingInput.reset();
        } else if (!samePending) {
          state->pendingInput = state->input;
          state->pendingSourceRevision = state->inputRevision;
          state->pendingCurrentRevision = (*current)->revision;
        }
      } else {
        state->pendingInput.reset();
      }

      const bool currentChanged = (*current)->revision != state->currentRevision;
      const bool modelChanged = revision != state->modelRevision;

      if (options.current && !validInput && (currentChanged || modelChanged || state->ensurePending)) {
        auto selected = (*current)->value.row;
        if ((*current)->value.resolution != PositionResolution::EXACT &&
            (*current)->value.resolution != PositionResolution::NEAREST) {
          selected.reset();
        }

        if (selected) {
          auto location = source->frame->locateRow(*selected);

          if (!location) {
            return std::unexpected(location.error());
          }
          if (location->state != RowLookupState::FOUND) {
            selected.reset();
          }
        }

        std::optional<RowId> previous;
        read = app_.tree().read(node, [&](const W &view) {
          previous = view.selection();
        });

        if (!read) {
          return read;
        }
        if (previous != selected || staleInput) {
          auto changed = [&] {
            if constexpr (std::same_as<W, VirtualList<T>>) {
              return setVirtualListSelection(app_.tree(), node, selected);
            } else {
              return setVirtualTableSelection(app_.tree(), node, selected);
            }
          }();

          if (!changed) {
            return changed;
          }
        }
        if (selected && (currentChanged || state->ensurePending)) {
          auto visible = [&] {
            if constexpr (std::same_as<W, VirtualList<T>>) {
              return ensureVirtualListVisible(app_.tree(), node, *source->frame, *selected);
            } else {
              return ensureVirtualTableVisible(app_.tree(), node, *source->frame, *selected);
            }
          }();

          if (!visible && visible.error() != TreeError::WOULD_BLOCK) {
            return std::unexpected(visible.error());
          }

          state->ensurePending = !visible;
        } else if (!selected) {
          state->ensurePending = false;
        }
      }

      if (options.readingAnchor && !validInput && !state->readingInput &&
          ((*reading)->revision != state->readingRevision || modelChanged) && (*reading)->value.row &&
          ((*reading)->value.resolution == PositionResolution::EXACT ||
           (*reading)->value.resolution == PositionResolution::NEAREST)) {
        auto location = source->frame->locateRow(*(*reading)->value.row);

        if (!location) {
          return std::unexpected(location.error());
        }
        if (location->state == RowLookupState::FOUND) {
          auto changed = [&] {
            if constexpr (std::same_as<W, VirtualList<T>>) {
              return setVirtualListFirst(app_.tree(), node, location->index);
            } else {
              return setVirtualTableFirst(app_.tree(), node, location->index);
            }
          }();

          if (!changed) {
            return changed;
          }
        }
      }

      state->currentRevision = (*current)->revision;
      state->readingRevision = (*reading)->revision;

      return {};
    };
    binding->after = [this, node, navigation, options, state]() -> std::expected<void, TreeError> {
      auto record = navigation.lock();
      auto entry = find(node);
      auto source =
        entry ? std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(entry->viewSource.lock()) : nullptr;

      if (!record || !source || source->retired || !source->frame) {
        return !record && record.error() != TreeError::CLOSED ? std::unexpected(record.error())
                                                              : std::expected<void, TreeError>{};
      }

      if (!source->frame->dataRevision()) {
        return {};
      }
      const auto currentBefore = (*record)->current.snapshot();
      const auto readingBefore = (*record)->reading.snapshot();
      if (!currentBefore || !readingBefore) {
        return std::unexpected(!currentBefore ? currentBefore.error() : readingBefore.error());
      }
      const auto lifetime = std::weak_ptr(lifetime_);
      const auto owner = *record;
      auto finish = [&](std::expected<void, TreeError> result) {
        if (!lifetime.expired() && !closed_ && navigation_ && !owner->closed) {
          auto current = owner->current.snapshot();
          auto reading = owner->reading.snapshot();
          if ((current && (*current)->revision != (*currentBefore)->revision) ||
              (reading && (*reading)->revision != (*readingBefore)->revision)) {
            // This requests the latest shared state, even after partial success
            // or a newer nested operation. No old node/position is retained.
            navigation_->refreshAfterLayout = true;
          }
        }
        return result;
      };
      auto stillBound = [&]() -> std::expected<bool, TreeError> {
        if (lifetime.expired() || closed_ || source->retired || !source->frame) {
          return false;
        }
        auto bound = find(node);
        if (!bound || bound->viewSource.lock() != source) {
          return false;
        }
        auto live = app_.tree().read(node, [](const W &) {
        });
        if (!live && live.error() != TreeError::STALE_NODE && live.error() != TreeError::CLOSED) {
          return std::unexpected(live.error());
        }
        return static_cast<bool>(live);
      };

      std::optional<RowId> selected;
      std::optional<RowId> anchor;
      bool consumed{};
      bool materialized{};
      std::uint64_t offset{};
      auto read = app_.tree().read(node, [&](const W &view) {
        selected = view.selection();
        anchor = view.anchorRow();
        consumed = !view.intent_;
        if (state->input && consumed && view.prepared_ &&
            view.prepared_->frame.dataRevision() == state->inputRevision) {
          const auto first = view.prepared_->window.visible.first;
          const auto index = *state->input;

          if (index >= first && index - first < view.prepared_->rows.size()) {
            materialized = selected && view.prepared_->rows[static_cast<std::size_t>(index - first)].id == selected;
          }
        }
        offset = view.scrollState().rasterOffset();
      });

      if (!read) {
        return finish(read);
      }
      if (options.current && materialized && selected && state->inputRevision == source->frame->dataRevision()) {
        const auto operation = (*record)->operation;
        auto updated = navigation.navigate(*selected);

        if (!updated) {
          return finish(updated);
        }
        auto live = stillBound();
        if (!live || !*live || owner->closed || owner->operation != operation + 1) {
          return finish(!live ? std::unexpected(live.error()) : std::expected<void, TreeError>{});
        }
      }
      record = navigation.lock();
      if (!record) {
        return finish(record.error() == TreeError::CLOSED ? std::expected<void, TreeError>{}
                                                          : std::unexpected(record.error()));
      }
      if (state->readingInput && anchor) {
        const auto operation = (*record)->operation;
        auto capture = (*record)->capture;
        auto bookmark = (*capture)(*record, *anchor);

        if (!bookmark) {
          return finish(std::unexpected(bookmark.error()));
        }

        record = navigation.lock();
        if (!record) {
          return finish(record.error() == TreeError::CLOSED ? std::expected<void, TreeError>{}
                                                            : std::unexpected(record.error()));
        }
        auto live = stillBound();
        if (!live || !*live || (*record)->operation != operation) {
          return finish(!live ? std::unexpected(live.error()) : std::expected<void, TreeError>{});
        }

        auto updated = (*record)->setPosition(std::move(*bookmark), PositionResolution::EXACT, true);

        if (!updated) {
          return finish(updated);
        }
      }

      if (consumed) {
        state->pendingInput.reset();
      }
      state->offset = offset;
      state->modelRevision = source->frame->dataRevision();
      if (auto current = (*record)->current.snapshot()) {
        state->currentRevision = (*current)->revision;
      }
      if (auto reading = (*record)->reading.snapshot()) {
        state->readingRevision = (*reading)->revision;
      }

      return finish({});
    };
    navigation_->bindings.push_back(std::move(binding));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

template <class T>
std::expected<void, TreeError>
ManagedScene::retryCanonicalRestore(const std::shared_ptr<detail::ManagedNavigationRecord> &record,
                                    const std::shared_ptr<detail::ManagedModelSourceOf<T>> &source) {
  auto history = record->history;
  auto pending = history ? history->pending : nullptr;
  if (!pending || !source->frame ||
      (pending->publication == source->frame->revision() && pending->sourceRevision == source->navigationRevision)) {
    return {};
  }
  const auto frame = *source->frame;
  const auto operation = record->operation;
  const auto sequence = record->sequence;
  const auto sourceRevision = source->navigationRevision;
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  const auto originNode = root_.id();

  struct PreparingRetry {
    ManagedScene *scene;
    std::weak_ptr<const void> lifetime;
    bool active{true};

    ~PreparingRetry() {
      if (active && !lifetime.expired()) {
        scene->preparing_ = false;
      }
    }
  } admission{this, lifetime};

  preparing_ = true;
  std::optional<std::uint64_t> selectedRevision;
  auto defer = [&](TreeError cause) -> std::expected<void, TreeError> {
    if (!lifetime.expired() && !record->closed && record->operation == operation && record->sequence == sequence &&
        record->model.lock() == source && history->pending == pending && !source->retired &&
        source->navigationRevision == sourceRevision && source->frame &&
        source->frame->revision() == frame.revision()) {
      auto selection = record->selection.snapshot();
      if (!selection || !selectedRevision || (*selection)->revision != *selectedRevision) {
        return {};
      }
      // A failed admission keeps the accepted intent. Only a newer publication
      // retries it; paint and resize cannot create allocation/worker polling.
      pending->publication = frame.revision();
      pending->sourceRevision = sourceRevision;
      record->frameRevision = frame.revision();
      preparing_ = false;
      admission.active = false;
      (void)reportError(error(ManagedPhase::PREPARE, {}, cause));
    }
    return {};
  };
  try {
    auto currentOwner = record->current.lock();
    auto readingOwner = record->reading.lock();
    auto selectionOwner = record->selection.lock();
    auto currentBase = record->current.snapshot();
    auto readingBase = record->reading.snapshot();
    auto selectionBase = record->selection.snapshot();
    if (!currentOwner || !readingOwner || !selectionOwner || !currentBase || !readingBase || !selectionBase) {
      return std::unexpected(TreeError::CLOSED);
    }
    selectedRevision = (*selectionBase)->revision;
    if (operation == std::numeric_limits<std::uint64_t>::max()) {
      return defer(TreeError::IDENTITY_EXHAUSTED);
    }
    auto channels = pending->channels;
    channels.current =
      channels.current && pending->currentRevision == (*currentBase)->revision && pending->sequence == sequence;
    channels.reading = channels.reading && pending->readingRevision == (*readingBase)->revision;
    channels.selection = channels.selection && pending->selectionRevision == (*selectionBase)->revision;
    auto value = pending->value;
    value.current = record->bookmark;
    value.currentResolution = (*currentBase)->value.resolution;
    value.reading = record->readingBookmark;
    value.readingResolution = (*readingBase)->value.resolution;
    if (!channels.selection) {
      value.selection = (*selectionBase)->value;
      value.selectedPositions.reset();
    }
    // Ordinary current/reading reconciliation uses the latest manual intent;
    // selected-set reconciliation is confined to the still-owned restore intent.
    const detail::ModelRestoreChannels resolutionChannels{true, true, channels.selection};
    PositionResolution outcome{};
    auto resolved = resolveViewCheckpoint<T>(std::move(value), source, history, history->options().bookmarks, outcome,
                                             {}, &resolutionChannels);
    if (!resolved) {
      return defer(resolved.error());
    }
    if (lifetime.expired()) {
      return {};
    }
    auto staged = history->stage(source, 0, *resolved);
    if (!staged) {
      return defer(staged.error());
    }
    auto logical = [&](const std::optional<ModelBookmark> &bookmark, PositionResolution state) {
      NavigationPosition result;
      result.resource = record->resource;
      result.resolution = state;
      if (bookmark) {
        const auto position = bookmark->position();
        result.kind = position.kind;
        result.affinity = position.affinity;
        if (position.kind == PositionKind::ITEM) {
          result.row = position.row;
        } else {
          result.offset = position.offset;
        }
      }
      return result;
    };
    auto publication =
      record->preparePublication(logical(resolved->current, resolved->currentResolution), resolved->selection,
                                 logical(resolved->reading, resolved->readingResolution));
    if (!publication) {
      return defer(publication.error());
    }
    std::shared_ptr<detail::ModelCanonicalRestore> next;
    if (detail::pendingModelRestoration(*resolved, channels)) {
      auto allocated = history->canonicalRestore();
      if (!allocated) {
        return defer(allocated.error());
      }
      next = std::move(*allocated);
      next->value = *resolved;
      next->channels = channels;
      next->publication = frame.revision();
      next->sourceRevision = sourceRevision;
      next->sequence = sequence;
      next->currentRevision = publication->current.next->revision;
      next->readingRevision = publication->reading.next->revision;
      next->selectionRevision = publication->selection.next->revision;
    }
    if (lifetime.expired() || record->closed || record->operation != operation || record->sequence != sequence ||
        record->model.lock() != source || history->pending != pending || source->retired || !source->frame ||
        source->frame->revision() != frame.revision() || source->navigationRevision != sourceRevision ||
        !history->valid(*staged) || record->current.snapshot() != currentBase ||
        record->reading.snapshot() != readingBase || record->selection.snapshot() != selectionBase) {
      return {};
    }
    auto valid = record->validatePublication(*publication);
    if (!valid) {
      return defer(valid.error());
    }
    auto invalidated = record->invalidatePublication(*publication);
    if (!invalidated) {
      return defer(invalidated.error());
    }
    auto cancellation = std::move(record->cancellation);
    record->cancellation = std::stop_source(std::nostopstate);
    auto request = std::move(record->request);
    record->request.reset();
    record->bookmark = resolved->current;
    record->readingBookmark = resolved->reading;
    history->commit(std::move(*staged));
    history->pending = std::move(next);
    record->frameRevision = frame.revision();
    ++record->operation;
    detail::ManagedNavigationRecord::installPublication(*publication);
    for (const auto &binding : navigation_->bindings) {
      if (binding->context.lock() == record) {
        binding->installed(&*resolved, &channels);
      }
    }
    const auto notification = detail::ManagedNavigationRecord::publishPublication(*publication);
    preparing_ = false;
    admission.active = false;
    cancellation.request_stop();
    if (notification && !lifetime.expired()) {
      ManagedError failure;
      failure.phase = ManagedPhase::PREPARE;
      failure.node = originNode;
      failure.cause = ApplicationError{ApplicationPhase::DISPATCH, *notification};
      (void)reportError(std::move(failure));
    }
    return {};
  } catch (const std::bad_alloc &) {
    return defer(TreeError::NO_MEMORY);
  } catch (...) {
    return defer(TreeError::CALLBACK_FAILED);
  }
}

template <class T>
std::expected<void, ManagedError> ManagedNavigation::setModel(ManagedModel<T> model, ModelRebindOptions options) const {
  auto record = lock();
  if (!record) {
    return std::unexpected(ManagedScene::error(ManagedPhase::INPUT, {}, record.error()));
  }
  if (!(*record)->owner || !(*record)->finite) {
    return std::unexpected(ManagedScene::error(ManagedPhase::INPUT, {}, TreeError::INVALID_ARGUMENT));
  }
  return (*record)->owner->replaceNavigationModel(*record, std::move(model), options);
}

template <class T> bool ManagedNavigation::model(ManagedModel<T> model, ModelRebindOptions options) const {
  auto result = setModel(std::move(model), options);
  if (!result) {
    if (auto record = lock(); record && (*record)->owner) {
      (void)(*record)->owner->reportError(result.error());
    }
  }
  return result.has_value();
}

template <class T>
std::expected<void, ManagedError>
ManagedScene::replaceNavigationModel(const std::shared_ptr<detail::ManagedNavigationRecord> &record,
                                     ManagedModel<T> model, ModelRebindOptions options, Node<Widget> overrideNode,
                                     std::optional<ManagedViewHandler<T>> prepared) {
  auto allowed = boundary(ManagedPhase::INPUT, root_);
  if (!allowed) {
    return allowed;
  }
  if (record->closed || record->lifetime.expired()) {
    return std::unexpected(error(ManagedPhase::INPUT, root_, TreeError::CLOSED));
  }
  auto source = model.lock();
  if (!source || (*source)->retired || !(*source)->model || (*source)->scene != root_.id() || record->closed ||
      record->scene != root_.id() || !record->finite) {
    return std::unexpected(error(ManagedPhase::INPUT, root_, !source ? source.error() : TreeError::INVALID_ARGUMENT));
  }
  const auto previousModel = record->model.lock();
  const auto previousSource = std::dynamic_pointer_cast<detail::ManagedModelSourceOf<T>>(previousModel);
  if (!previousSource) {
    return std::unexpected(
      error(ManagedPhase::INPUT, root_, previousModel ? TreeError::INVALID_ARGUMENT : TreeError::CLOSED));
  }
  const bool sameModel = previousModel == *source;
  if (sameModel && !prepared && options.restore == ModelRestorePolicy::REMEMBER) {
    return {};
  }
  if (record->operation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(error(ManagedPhase::INPUT, root_, TreeError::IDENTITY_EXHAUSTED));
  }
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  const auto operation = record->operation;
  const auto sequence = record->sequence;
  const auto originNode = root_.id();

  struct Admission {
    ManagedScene *scene;
    std::weak_ptr<const void> lifetime;
    bool active{true};

    ~Admission() {
      if (active && !lifetime.expired()) {
        scene->preparing_ = false;
      }
    }
  } admission{this, lifetime};

  preparing_ = true;
  auto fail = [&](TreeError cause) -> std::expected<void, ManagedError> {
    return std::unexpected(error(ManagedPhase::INPUT, {}, cause));
  };
  try {
    // Count construction also keeps MSVC Debug proxy allocation within this boundary.
    std::vector<std::shared_ptr<detail::ManagedNavigationBinding>> members(0);
    std::vector<std::unique_ptr<detail::PreparedModelViewChange>> staged(0);
    for (const auto &binding : navigation_->bindings) {
      if (binding->context.lock() == record) {
        if (binding->options.projection != NavigationProjection::SAME_MODEL || !binding->stage) {
          return fail(TreeError::INVALID_ARGUMENT);
        }
        members.push_back(binding);
      }
    }
    staged.reserve(members.size());
    auto currentOwner = record->current.lock();
    auto selectionOwner = record->selection.lock();
    auto readingOwner = record->reading.lock();
    if (!currentOwner || !selectionOwner || !readingOwner) {
      return fail(!currentOwner     ? currentOwner.error()
                  : !selectionOwner ? selectionOwner.error()
                                    : readingOwner.error());
    }
    auto currentBase = record->current.snapshot();
    auto selectionBase = record->selection.snapshot();
    auto readingBase = record->reading.snapshot();
    if (!currentBase || !selectionBase || !readingBase) {
      return fail(!currentBase ? currentBase.error() : !selectionBase ? selectionBase.error() : readingBase.error());
    }

    auto history = record->history;
    if (!history) {
      auto limits = options.checkpoints.value_or(
        ModelCheckpointOptions{record->options.maxCheckpointBytes, record->options.bookmarks});
      auto created = detail::ModelViewHistory::create(lifetime, limits);
      if (!created) {
        return fail(created.error());
      }
      history = std::move(*created);
    } else if (options.checkpoints &&
               (options.checkpoints->maxBytes != history->options().maxBytes ||
                options.checkpoints->bookmarks.maxBytes != history->options().bookmarks.maxBytes)) {
      return fail(TreeError::INVALID_ARGUMENT);
    }

    detail::ModelViewCheckpoint target;
    const auto navigationRevision = (*source)->navigationRevision;
    auto pinned = (*source)->pin();
    if (!pinned || lifetime.expired()) {
      return fail(!pinned ? pinned.error() : TreeError::CLOSED);
    }
    const auto revision = (*source)->frame->revision();
    std::optional<detail::ModelViewHistoryChange> outgoing;
    std::optional<detail::ModelViewHistoryChange> destination;
    std::optional<detail::ModelViewCheckpoint> continuation;
    PositionResolution resolution = PositionResolution::EMPTY;
    if (!sameModel || options.restore == ModelRestorePolicy::RESET) {
      if (!sameModel) {
        detail::ModelViewCheckpoint value{record->bookmark, (*selectionBase)->value, record->readingBookmark, 0};
        value.currentResolution = (*currentBase)->value.resolution;
        value.readingResolution = (*readingBase)->value.resolution;
        if (auto cached = history->find(previousModel, 0);
            cached && cached->value.selection.resource() == value.selection.resource() &&
            std::ranges::equal(cached->value.selection.rows(), value.selection.rows())) {
          value.selectedPositions = cached->value.selectedPositions;
        }
        auto captured = captureSelectedPositions(value, previousSource, history, history->options().bookmarks);
        if (!captured || lifetime.expired()) {
          return fail(!captured ? captured.error() : TreeError::CLOSED);
        }
        if (record->resource == (*source)->navigationResource) {
          continuation = std::move(value);
        } else {
          auto saved = history->stage(previousModel, 0, std::move(value));
          if (!saved) {
            return fail(saved.error());
          }
          outgoing = std::move(*saved);
        }
      }
      if (options.restore == ModelRestorePolicy::REMEMBER) {
        if (continuation) {
          target = std::move(*continuation);
        } else if (auto cached = history->find(*source, 0)) {
          target = cached->value;
        }
      }
      if (detail::hasModelRestoreIntent(target)) {
        auto resolved =
          resolveViewCheckpoint<T>(std::move(target), *source, history, history->options().bookmarks, resolution);
        if (!resolved || lifetime.expired()) {
          return fail(!resolved ? resolved.error() : TreeError::CLOSED);
        }
        target = std::move(*resolved);
      }
    } else {
      target = {record->bookmark, (*selectionBase)->value, record->readingBookmark, 0};
      resolution = (*currentBase)->value.resolution;
    }
    auto resource = detail::modelNavigationResource(**source);
    if (!resource) {
      return fail(resource.error());
    }
    if (target.selection.rows().empty()) {
      target.selection.resource_ = *resource;
      target.selection.resolution_ = PositionResolution::EMPTY;
    }
    auto savedDestination = history->stage(*source, 0, target);
    if (!savedDestination) {
      return fail(savedDestination.error());
    }
    destination = std::move(*savedDestination);

    // Every view registration uses this capacity; individual stages cannot cause
    // a later push_back allocation after another participant has been installed.
    std::erase_if((*source)->prepared, [](const auto &registration) {
      return registration.expired();
    });
    (*source)->prepared.reserve((*source)->prepared.size() + members.size());
    for (const auto &member : members) {
      auto position = target;
      if (member->options.readingAnchor && target.reading) {
        position.first = target.reading->position().offset;
      } else {
        position.reading.reset();
      }
      auto next = member->stage(*source, options, history, &position,
                                member->node == overrideNode && prepared ? &prepared : nullptr);
      if (lifetime.expired()) {
        return fail(TreeError::CLOSED);
      }
      if (!next) {
        return std::unexpected(next.error());
      }
      staged.push_back(std::move(*next));
    }
    auto logical = [&](const std::optional<ModelBookmark> &bookmark, PositionResolution outcome) {
      NavigationPosition value;
      value.resource = *resource;
      value.resolution = outcome;
      if (bookmark) {
        const auto position = bookmark->position();
        value.kind = position.kind;
        value.affinity = position.affinity;
        if (position.kind == PositionKind::ITEM) {
          value.row = position.row;
        } else {
          value.offset = position.offset;
        }
      }
      return value;
    };
    auto publication = record->preparePublication(logical(target.current, target.currentResolution), target.selection,
                                                  logical(target.reading, target.readingResolution));
    if (!publication) {
      return fail(publication.error());
    }
    std::shared_ptr<detail::ModelCanonicalRestore> restoredIntent;
    if (detail::pendingModelRestoration(target, {})) {
      auto created = history->canonicalRestore();
      if (!created) {
        return fail(created.error());
      }
      restoredIntent = std::move(*created);
      restoredIntent->value = target;
      restoredIntent->publication = revision;
      restoredIntent->sourceRevision = navigationRevision;
      restoredIntent->sequence = sequence;
      restoredIntent->currentRevision = publication->current.next->revision;
      restoredIntent->readingRevision = publication->reading.next->revision;
      restoredIntent->selectionRevision = publication->selection.next->revision;
    }

    // This is deliberately after every provider/resolver/user staging callout.
    if (lifetime.expired() || record->closed || record->operation != operation || record->sequence != sequence ||
        record->model.lock() != previousModel || (*source)->retired || !(*source)->frame ||
        (*source)->frame->revision() != revision || (*source)->navigationRevision != navigationRevision ||
        record->current.snapshot() != currentBase || record->selection.snapshot() != selectionBase ||
        record->reading.snapshot() != readingBase || (outgoing && !history->valid(*outgoing)) ||
        (destination && !history->valid(*destination))) {
      return fail(TreeError::WOULD_BLOCK);
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
      if (!std::ranges::contains(navigation_->bindings, members[index]) || members[index]->context.lock() != record) {
        return fail(TreeError::WOULD_BLOCK);
      }
      auto valid = staged[index]->validate();
      if (!valid) {
        return fail(valid.error());
      }
    }
    auto valid = record->validatePublication(*publication);
    if (!valid) {
      return fail(valid.error());
    }
    for (const auto &next : staged) {
      auto invalidated = next->invalidate();
      if (!invalidated) {
        return fail(invalidated.error());
      }
    }
    auto invalidated = record->invalidatePublication(*publication);
    if (!invalidated) {
      return fail(invalidated.error());
    }

    // No callout occurs between validation and the last install. Old consumers,
    // requests, snapshots, callback owners and cancellation are retained locally.
    for (const auto &next : staged) {
      auto installed = next->install();
      if (!installed) {
        return fail(installed.error());
      }
    }
    auto previousCancellation = std::move(record->cancellation);
    record->cancellation = std::stop_source(std::nostopstate);
    auto previousRequest = std::move(record->request);
    record->request.reset();
    record->model = *source;
    record->resource = *resource;
    record->session = resource->identity;
    record->bookmark = std::move(target.current);
    record->readingBookmark = std::move(target.reading);
    record->changes = (*source)->navigationChanges;
    record->frameRevision = revision;
    record->history = history;
    auto previousRestore = std::move(history->pending);
    history->pending = std::move(restoredIntent);
    ++record->operation;
    if (outgoing) {
      history->commit(std::move(*outgoing));
    }
    if (destination) {
      history->commit(std::move(*destination));
    }
    for (const auto &next : staged) {
      next->commitHistory();
    }
    detail::ManagedNavigationRecord::installPublication(*publication);
    for (const auto &member : members) {
      member->installed(nullptr, nullptr);
    }
    const auto notification = detail::ManagedNavigationRecord::publishPublication(*publication);

    // Retirement callbacks may now start a newer operation. There are no old
    // model/view/property writes after the first retirement callout.
    preparing_ = false;
    admission.active = false;
    previousCancellation.request_stop();
    for (const auto &next : staged) {
      next->retire();
    }
    if (notification && !lifetime.expired()) {
      ManagedError failure;
      failure.phase = ManagedPhase::INPUT;
      failure.node = originNode;
      failure.cause = ApplicationError{ApplicationPhase::DISPATCH, *notification};
      // This receipt belongs to the completed older publication even if a stop
      // callback has already installed a newer coherent group.
      (void)reportError(std::move(failure));
    }
    return {};
  } catch (const std::bad_alloc &) {
    return fail(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    return fail(TreeError::NODE_LIMIT);
  } catch (...) {
    return fail(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals
