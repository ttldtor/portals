// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

// Included after ManagedScene and its navigation records are complete.
namespace portals {
template <class T>
std::expected<ManagedNavigation, ManagedError> ManagedScene::navigation(ManagedModel<T> model,
                                                                        NavigationOptions options) {
  auto source = modelSource(model, root_);

  if (!source) {
    return std::unexpected(source.error());
  }
  if (navigation_) {
    for (const auto &record : navigation_->contexts) {
      if (record->finite && record->model.lock() == *source) {
        return navigation(model, record->resource, record->session, options);
      }
    }
  }

  auto resource = ModelResource::create();

  return resource ? navigation(model, *resource, resource->identity, options)
                  : std::unexpected(error(ManagedPhase::STATE, root_, detail::navigationTreeError(resource.error())));
}

template <class T>
std::expected<ManagedNavigation, ManagedError> ManagedScene::navigation(ManagedModel<T> model, ModelResource resource,
                                                                        SourceStamp session,
                                                                        NavigationOptions options) {
  auto source = modelSource(model, root_);

  if (!source) {
    return std::unexpected(source.error());
  }

  auto pinned = (*source)->pin();

  if (!pinned) {
    return std::unexpected(error(ManagedPhase::STATE, root_, pinned.error()));
  }

  try {
    std::function<std::expected<ModelBookmark, TreeError>(RowId)> capture =
      [weak = std::weak_ptr(*source), resource, options](RowId id) -> std::expected<ModelBookmark, TreeError> {
      auto source = weak.lock();

      if (!source || source->retired || !source->frame) {
        return std::unexpected(TreeError::CLOSED);
      }

      auto location = source->frame->locateRow(id);

      if (!location) {
        return std::unexpected(location.error());
      }
      if (location->state != RowLookupState::FOUND) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }

      auto bookmark = ModelBookmark::create(
        resource, {PositionKind::ITEM, id, location->index, source->frame->dataRevision(), PositionAffinity::FORWARD},
        {}, options.bookmarks);

      return bookmark ? std::expected<ModelBookmark, TreeError>(std::move(*bookmark))
                      : std::unexpected(detail::navigationTreeError(bookmark.error()));
    };
    std::function<std::expected<void, TreeError>(const std::shared_ptr<detail::ManagedNavigationRecord> &)> sync =
      [weak = std::weak_ptr(*source)](const auto &record) -> std::expected<void, TreeError> {
      auto source = weak.lock();

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

      if (record->frameRevision == publication) {
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
    binding->before = [this, node, navigation, options, weak = std::weak_ptr(source),
                       state]() -> std::expected<void, TreeError> {
      auto record = navigation.lock();
      auto source = weak.lock();

      if (!record || !source || source->retired || !source->frame) {
        return !record && record.error() != TreeError::CLOSED ? std::unexpected(record.error())
                                                              : std::expected<void, TreeError>{};
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
          ((*reading)->revision != state->readingRevision || modelChanged) && (*reading)->value.row) {
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
    binding->after = [this, node, navigation, options, weak = std::weak_ptr(source),
                      state]() -> std::expected<void, TreeError> {
      auto record = navigation.lock();
      auto source = weak.lock();

      if (!record || !source || source->retired || !source->frame) {
        return !record && record.error() != TreeError::CLOSED ? std::unexpected(record.error())
                                                              : std::expected<void, TreeError>{};
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
        auto entry = find(node);
        if (!entry || entry->viewSource.lock() != source) {
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
        auto bookmark = (*capture)(*anchor);

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
} // namespace portals
