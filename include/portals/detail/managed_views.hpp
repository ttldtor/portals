// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

// Included by managed.hpp after the scene definition; no additional runtime lives here.
namespace portals {
template <class T>
std::expected<VisibleModelSnapshot<T>, VisibleModelError>
ManagedScene::visibleModelSnapshot(Node<VirtualTable<T>> node) const {
  auto allowed = boundary(ManagedPhase::LAYOUT, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto revision = app_.tree().geometryRevision();

  if (!revision) {
    return std::unexpected(error(ManagedPhase::PREPARE, node, revision.error()));
  }

  if (*revision != layoutRevision_) {
    return std::unexpected(VisibleModelReadError{VisibleModelReadState::NOT_PREPARED, {}});
  }

  auto shown = detail::listShown(app_.tree(), node);

  if (!shown) {
    return std::unexpected(error(ManagedPhase::PREPARE, node, shown.error()));
  }

  if (!*shown) {
    return VisibleModelSnapshot<T>{};
  }

  auto geometry = app_.tree().observeGeometry(node);

  if (!geometry) {
    return std::unexpected(error(ManagedPhase::PREPARE, node, geometry.error()));
  }

  std::expected<VisibleModelSnapshot<T>, VisibleModelError> result;
  auto read = app_.tree().read(node, [&](const VirtualTable<T> &table) {
    try {
      auto snapshot = table.visibleSnapshot(geometry->geometry);

      if (snapshot) {
        result = std::move(*snapshot);
      } else {
        result = std::unexpected(std::move(snapshot.error()));
      }
    } catch (const std::bad_alloc &) {
      result = std::unexpected(error(ManagedPhase::PREPARE, node, TreeError::NO_MEMORY));
    }
  });

  if (!read) {
    return std::unexpected(error(ManagedPhase::PREPARE, node, read.error()));
  }

  return result;
}

template <class T>
std::expected<ManagedModel<T>, ManagedError> ManagedScene::model(std::shared_ptr<const ModelSnapshot<T>> initial,
                                                                 ModelOptions options, ManagedViewHandler<T> prepared) {
  auto allowed = boundary(ManagedPhase::STATE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (models_.size() >= options_.maxModels) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NODE_LIMIT));
  }

  try {
    auto created = Model<T>::create(app_.tree(), root_, std::move(initial), options);

    if (!created) {
      return std::unexpected(error(ManagedPhase::STATE, root_, created.error()));
    }

    auto source = std::make_shared<detail::ManagedModelSourceOf<T>>();
    ManagedModel<T> handle;

    source->model = std::move(*created);
    source->scene = root_.id();
    if (prepared) {
      source->preparation = std::make_shared<ManagedViewHandler<T>>(std::move(prepared));
    }
    handle.source_ = source;
    handle.owner_ = owner_;
    models_.push_back(std::move(source));

    return handle;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NO_MEMORY));
  }
}

template <class T>
std::expected<std::shared_ptr<detail::ManagedModelSourceOf<T>>, ManagedError>
ManagedScene::modelSource(ManagedModel<T> model, Node<Widget> parent) {
  auto allowed = boundary(ManagedPhase::MOUNT, parent);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto source = model.lock();

  if (!source) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, source.error()));
  }

  if ((*source)->retired || !(*source)->model) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::CLOSED));
  }

  if ((*source)->scene != root_.id()) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::INVALID_ARGUMENT));
  }

  return *source;
}

/// @cond INTERNAL
// Keep the binding in the existing preparation slot. Its concrete type lets model replacement
// inspect the source and callback without adding optional-feature storage to every scene entry.
template <class T, class W> struct ManagedScene::ViewPreparation {
  ManagedScene *scene;
  Node<W> node;
  std::shared_ptr<detail::ManagedModelSourceOf<T>> source;
  std::shared_ptr<detail::ManagedPreparedRegistration<T>> registration;
  ManagedChartOrigin first;
  [[no_unique_address]] std::conditional_t<std::same_as<W, Chart<T>>, std::monostate,
                                           std::shared_ptr<detail::ModelViewRestoreState>> restoration;

  std::expected<void, TreeError> operator()(const LayoutSnapshot &layout) const {
    const auto retireOrigin = [&]() -> std::expected<void, TreeError> {
      if (source->origin) {
        auto live = source->origin->check();

        if (!live) {
          if (!source->origin->retired && !source->origin->closed) {
            return std::unexpected(live.error());
          }

          scene->pruneLiveText();
        }
      }

      return {};
    };
    auto live = retireOrigin();

    if (!live) {
      return live;
    }

    if (!source->frame) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if constexpr (std::same_as<W, VirtualList<T>> || std::same_as<W, VirtualTable<T>>) {
      if (restoration && restoration->pending) {
        if (auto context = restoration->context.lock()) {
          auto current = context->current.snapshot();
          auto reading = context->reading.snapshot();
          auto selection = context->selection.snapshot();
          auto navigated = [](const auto &snapshot, std::uint64_t revision) {
            return snapshot->revision != revision && snapshot->value.row &&
                   (snapshot->value.resolution == PositionResolution::EXACT ||
                    snapshot->value.resolution == PositionResolution::NEAREST);
          };
          if (current && reading &&
              ((restoration->shareCurrent && navigated(*current, restoration->currentRevision)) ||
               (restoration->shareReading && navigated(*reading, restoration->readingRevision)))) {
            // A newer explicit navigation command supersedes the retained restore intent.
            restoration->pending.reset();
            restoration->attempted = false;
            auto cleared = scene->app_.tree().edit(node, [](W &view) {
              view.preparedSuppressed_ = false;
            });
            if (!cleared) {
              return cleared;
            }
          } else if (selection && (*selection)->revision != restoration->selectionRevision) {
            restoration->pending->selection = (*selection)->value;
            restoration->pending->selectedPositions.reset();
            restoration->channels.selection = false;
            restoration->selectionRevision = (*selection)->revision;
            if (!detail::pendingModelRestoration(*restoration->pending, restoration->channels) &&
                restoration->attemptedRevision == source->frame->revision() &&
                restoration->attemptedProofRevision == source->navigationRevision) {
              restoration->resolution = restoration->pending->currentResolution == PositionResolution::NEAREST ||
                                            restoration->pending->readingResolution == PositionResolution::NEAREST
                                          ? PositionResolution::NEAREST
                                          : PositionResolution::EXACT;
              restoration->pending.reset();
              auto cleared = scene->app_.tree().edit(node, [](W &view) {
                view.preparedSuppressed_ = false;
              });
              if (!cleared) {
                return cleared;
              }
            }
          }
        }
      }
      if (restoration && restoration->pending) {
        auto retained = restoration;
        auto model = source;
        const auto lifetime = std::weak_ptr<const void>(scene->lifetime_);
        auto shown = detail::listShown(scene->app_.tree(), node);
        if (!shown) {
          return std::unexpected(shown.error());
        }
        if (!*shown) {
          return {};
        }
        const auto revision = source->frame->revision();
        const auto proofRevision = source->navigationRevision;

        if (restoration->attemptedRevision != revision ||
            restoration->attemptedProofRevision != source->navigationRevision) {
          restoration->attempted = true;
          auto outcome = restoration->resolution;
          auto resolved = scene->resolveViewCheckpoint<T>(*restoration->pending, source, restoration->history,
                                                          restoration->history->options().bookmarks, outcome, {},
                                                          &restoration->channels);

          if (lifetime.expired()) {
            return std::unexpected(TreeError::CLOSED);
          }
          auto entry = scene->find(node);
          auto current = entry ? entry->viewPrepare.template target<ViewPreparation<T, W>>() : nullptr;
          if (!current || current->source != model || current->restoration != retained) {
            return std::unexpected(TreeError::CLOSED);
          }
          if (source->retired || !source->frame || source->frame->revision() != revision ||
              source->navigationRevision != proofRevision) {
            return {};
          }

          if (!resolved) {
            return std::unexpected(resolved.error());
          }

          restoration->attemptedRevision = revision;
          restoration->attemptedProofRevision = proofRevision;
          restoration->resolution = outcome;
          if (restoration->resolution == PositionResolution::EXACT ||
              restoration->resolution == PositionResolution::NEAREST ||
              restoration->resolution == PositionResolution::EMPTY) {
            auto installed = scene->app_.tree().edit(node, [&](W &view) {
              view.selected_ = resolved->current && (resolved->currentResolution == PositionResolution::EXACT ||
                                                     resolved->currentResolution == PositionResolution::NEAREST)
                                 ? std::optional(resolved->current->position().row)
                                 : std::nullopt;
              // A uint64 row with the default zero fraction is always valid.
              (void)view.scroll_.setOffset(resolved->first);
              view.anchor_ = resolved->reading && resolved->reading->position().kind == PositionKind::ITEM
                               ? std::optional(resolved->reading->position().row)
                               : std::nullopt;
              view.anchorRevision_ = source->frame->dataRevision();
              view.dirty_ = true;
              view.preparedSuppressed_ = false;
            });

            if (!installed) {
              return installed;
            }
            restoration->pending.reset();
          }
        }

        if (restoration->pending) {
          // A saved unresolved anchor has no safe row-zero substitute. Readiness
          // publications retry it; redraw alone does not start another attempt.
          return {};
        }
      }
    }

    if constexpr (std::same_as<W, VirtualList<T>>) {
      if (source->retired) {
        auto detached = scene->app_.tree().edit(node, [](W &view) {
          view.detachedCollection_ = true;
          view.request_.cancel();
          view.request_ = {};
          view.consumer_ = {};
        });

        if (!detached) {
          return detached;
        }
      }
    }

    auto navigation = scene->navigationBefore(node);

    if (!navigation) {
      return navigation;
    }

    auto work = [&]() -> std::expected<std::optional<ModelRequest<T>>, TreeError> {
      if constexpr (std::same_as<W, VirtualList<T>>) {
        return prepareVirtualList(scene->app_.tree(), node, *source->frame, layout);
      } else if constexpr (std::same_as<W, VirtualTable<T>>) {
        return prepareVirtualTable(scene->app_.tree(), node, *source->frame, layout);
      } else {
        auto origin = first ? first() : std::expected<std::uint64_t, TreeError>(0);

        if (!origin) {
          return std::unexpected(origin.error());
        }

        return prepareChart(scene->app_.tree(), node, *source->frame, layout, *origin);
      }
    }();

    if (!work) {
      return std::unexpected(work.error());
    }

    navigation = scene->navigationAfter(node);

    if (!navigation) {
      return navigation;
    }

    live = retireOrigin();

    if (!live) {
      return live;
    }

    if (source->retired) {
      return {};
    }

    // Disconnecting registration never destroys the std::function currently being invoked.
    auto action = registration && registration->action ? registration->action : source->preparation;

    if (action) {
      try {
        auto result = (*action)(*source->frame, *work);
        auto retired = retireOrigin();

        return result ? retired : result;
      } catch (...) {
        (void)retireOrigin();
        throw;
      }
    }

    // Without a provider callback there is nobody to retain and dispatch a receipt.
    if (*work) {
      (*work)->cancel();

      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    return {};
  }
};

/// @endcond

template <class T, class W>
std::expected<detail::ModelViewCheckpoint, TreeError>
ManagedScene::captureViewCheckpoint(Node<W> node, const std::shared_ptr<detail::ManagedModelSourceOf<T>> &source,
                                    BookmarkOptions options) {
  auto resource = detail::modelNavigationResource(*source);
  if (!resource) {
    return std::unexpected(resource.error());
  }

  detail::ModelViewCheckpoint value;
  std::optional<RowId> selected;
  std::optional<RowId> anchor;
  std::optional<std::uint64_t> selectedOffset;
  std::uint64_t revision{};
  bool versionKnown{};
  auto read = app_.tree().read(node, [&](const W &view) {
    selected = view.selected_;
    anchor = view.anchor_;
    value.first = view.scroll_.rasterOffset();
    if (view.prepared_) {
      versionKnown = true;
      revision = view.prepared_->frame.dataRevision();
      for (std::size_t index = 0; index < view.prepared_->rows.size(); ++index) {
        if (view.prepared_->rows[index].id == selected) {
          selectedOffset = view.prepared_->window.visible.first + index;
          break;
        }
      }
    } else if (anchor) {
      versionKnown = true;
      revision = view.anchorRevision_;
    }
  });
  if (!read) {
    return std::unexpected(read.error());
  }
  if (!versionKnown) {
    auto pinned = source->pin();
    if (!pinned) {
      return std::unexpected(pinned.error());
    }
    revision = source->frame->dataRevision();
  }
  const auto readingRevision = revision;

  if (!revision) {
    // Data-only models may be unversioned. Retain unsupported metadata without
    // inventing a positive bookmark revision or rejecting a healthy new binding.
    value.selection.resource_ = *resource;
    if (selected) {
      value.currentResolution = PositionResolution::UNSUPPORTED;
      value.selection.rows_ = std::make_shared<const std::vector<RowId>>(std::initializer_list<RowId>{*selected});
      value.selection.resolution_ = PositionResolution::UNSUPPORTED;
    }
    value.readingResolution = PositionResolution::UNSUPPORTED;
    return value;
  }

  if (selected && !selectedOffset) {
    auto located = source->frame->locateRow(*selected);
    if (!located) {
      return std::unexpected(located.error());
    }
    if (located->state != RowLookupState::FOUND) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    selectedOffset = located->index;
    revision = source->frame->dataRevision();
  }
  auto make = [&](PositionKind kind, RowId row, std::uint64_t offset, std::uint64_t dataRevision) {
    return ModelBookmark::create(*resource, {kind, row, offset, dataRevision, PositionAffinity::FORWARD}, {}, options);
  };
  if (selected) {
    auto mark = make(PositionKind::ITEM, *selected, *selectedOffset, revision);
    if (!mark) {
      return std::unexpected(detail::navigationTreeError(mark.error()));
    }
    value.current = std::move(*mark);
    value.currentResolution = PositionResolution::EXACT;
    value.selection.rows_ = std::make_shared<const std::vector<RowId>>(std::initializer_list<RowId>{*selected});
    value.selection.resolution_ = PositionResolution::EXACT;
  }
  value.selection.resource_ = *resource;
  auto reading =
    make(anchor ? PositionKind::ITEM : PositionKind::GAP, anchor.value_or(RowId{}), value.first, readingRevision);
  if (!reading) {
    return std::unexpected(detail::navigationTreeError(reading.error()));
  }
  value.reading = std::move(*reading);
  value.readingResolution = PositionResolution::EXACT;

  return value;
}

template <class T>
std::expected<void, TreeError> ManagedScene::captureSelectedPositions(
  detail::ModelViewCheckpoint &value, const std::shared_ptr<detail::ManagedModelSourceOf<T>> &source,
  const std::shared_ptr<detail::ModelViewHistory> &history, BookmarkOptions options) {
  if (value.selection.rows().empty()) {
    return {};
  }
  if (source->frame && !source->frame->dataRevision()) {
    value.selection.resolution_ = PositionResolution::UNSUPPORTED;
    return {};
  }
  if (value.selectedPositions) {
    return {};
  }
  if (!source->frame || value.selection.resource() != source->navigationResource) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  const auto frame = *source->frame;
  auto positions = history->selectedPositions(value.selection.rows().size());
  if (!positions) {
    return std::unexpected(positions.error());
  }
  for (auto id : value.selection.rows()) {
    auto located = frame.locateRow(id);
    if (!located) {
      return std::unexpected(located.error());
    }
    if (located->state != RowLookupState::FOUND) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto position = ModelBookmark::create(
      value.selection.resource(),
      {PositionKind::ITEM, id, located->index, frame.dataRevision(), PositionAffinity::FORWARD}, {}, options);
    if (!position) {
      return std::unexpected(detail::navigationTreeError(position.error()));
    }
    (*positions)->positions.push_back(std::move(*position));
  }
  value.selectedPositions = std::move(*positions);
  return {};
}

template <class T>
std::expected<detail::ModelViewCheckpoint, TreeError> ManagedScene::resolveViewCheckpoint(
  detail::ModelViewCheckpoint value, const std::shared_ptr<detail::ManagedModelSourceOf<T>> &source,
  const std::shared_ptr<detail::ModelViewHistory> &history, BookmarkOptions options, PositionResolution &resolution,
  PositionChangeMap changes, const detail::ModelRestoreChannels *requestedChannels) {
  if (!source->frame) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  const auto frame = *source->frame;
  const auto originalCurrent = value.current;
  const auto channels = requestedChannels ? *requestedChannels : detail::ModelRestoreChannels{};
  if (!changes && source->navigationChanges.toRevision() == frame.dataRevision()) {
    changes = source->navigationChanges;
  }
  auto resource = detail::modelNavigationResource(*source);
  if (!resource) {
    return std::unexpected(resource.error());
  }
  if (!frame.dataRevision()) {
    auto unavailable = [&](const std::optional<ModelBookmark> &bookmark, PositionResolution previous) {
      if (!bookmark && !detail::unresolvedPosition(previous)) {
        return PositionResolution::EMPTY;
      }
      if (previous == PositionResolution::INCOMPATIBLE || previous == PositionResolution::UNAVAILABLE) {
        return previous;
      }
      if ((bookmark && bookmark->resource() != *resource) ||
          (!bookmark && value.selection.resource().identity.instance && value.selection.resource() != *resource)) {
        return PositionResolution::INCOMPATIBLE;
      }
      return PositionResolution::UNSUPPORTED;
    };
    if (channels.current) {
      value.currentResolution = unavailable(value.current, value.currentResolution);
    }
    if (channels.reading) {
      value.readingResolution = unavailable(value.reading, value.readingResolution);
    }
    if (channels.selection && !value.selection.rows().empty()) {
      value.selection.resolution_ =
        value.selection.resource() == *resource ? PositionResolution::UNSUPPORTED : PositionResolution::INCOMPATIBLE;
    }
    const std::array outcomes{value.currentResolution, value.readingResolution, value.selection.resolution()};
    const std::array enabled{channels.current, channels.reading, channels.selection};
    resolution = frame.rowCount() ? PositionResolution::EXACT : PositionResolution::EMPTY;
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
      if (enabled[index] && detail::unresolvedPosition(outcomes[index])) {
        resolution = outcomes[index];
        break;
      }
    }
    return value;
  }
  auto resolver = FinitePositionResolver<T>::create(*resource, resource->identity, frame.snapshot(), changes);
  if (!resolver) {
    return std::unexpected(detail::navigationTreeError(resolver.error()));
  }
  for (bool reading : {false, true}) {
    if (reading ? !channels.reading : !channels.current) {
      continue;
    }
    auto *bookmark = reading ? &value.reading : &value.current;
    auto &outcome = reading ? value.readingResolution : value.currentResolution;
    if (!*bookmark) {
      if (!detail::unresolvedPosition(outcome)) {
        outcome = PositionResolution::EMPTY;
      } else if (value.selection.resource().identity.instance && value.selection.resource() != *resource) {
        outcome = PositionResolution::INCOMPATIBLE;
      }
      continue;
    }
    auto position = (*bookmark)->position();
    std::optional<PositionReply> reply;
    if ((*bookmark)->resource() == *resource && position.kind == PositionKind::ITEM &&
        (*bookmark)->metadata().empty()) {
      auto located = frame.locateRow(position.row);
      if (!located) {
        return std::unexpected(located.error());
      }
      if (located->state == RowLookupState::FOUND) {
        position.offset = located->index;
        position.revision = frame.dataRevision();
        reply =
          PositionReply{*resource, resource->identity, frame.dataRevision(), 1, PositionResolution::EXACT, position};
      } else if (located->state == RowLookupState::UNSUPPORTED && position.revision == frame.dataRevision()) {
        // A revision-qualified hint is verified with exactly one bounded row read.
        auto row = ModelRange<T>::read(frame.snapshot(), {position.offset, 1}, 1);
        if (!row) {
          return std::unexpected(row.error());
        }
        if (row->state() == RangeState::READY && !row->rows().empty() && row->rows().front().id == position.row) {
          reply =
            PositionReply{*resource, resource->identity, frame.dataRevision(), 1, PositionResolution::EXACT, position};
        } else if (row->state() == RangeState::PENDING) {
          reply =
            PositionReply{*resource, resource->identity, frame.dataRevision(), 1, PositionResolution::PENDING, {}};
        }
      }
    }
    if (!reply) {
      auto resolved = resolver->resolve({resource->identity, frame.dataRevision(), 1, **bookmark});
      if (!resolved) {
        return std::unexpected(detail::navigationTreeError(resolved.error()));
      }
      reply = std::move(*resolved);
    }
    outcome = reply->state;
    if (reply->state == PositionResolution::EXACT || reply->state == PositionResolution::NEAREST) {
      auto updated = ModelBookmark::create(*resource, reply->position, (*bookmark)->metadata(), options);
      if (!updated) {
        return std::unexpected(detail::navigationTreeError(updated.error()));
      }
      *bookmark = std::move(*updated);
    } else if (reply->state == PositionResolution::EMPTY) {
      bookmark->reset();
    }
  }
  if (value.reading) {
    value.first = value.reading->position().offset;
  } else if (!detail::unresolvedPosition(value.readingResolution)) {
    value.first = 0;
  }
  auto aggregate = [&] {
    bool nearest{};
    const std::array outcomes{value.currentResolution, value.readingResolution, value.selection.resolution()};
    const std::array enabled{channels.current, channels.reading, channels.selection};
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
      if (!enabled[index]) {
        continue;
      }
      const auto outcome = outcomes[index];
      if (outcome != PositionResolution::EXACT && outcome != PositionResolution::NEAREST &&
          outcome != PositionResolution::EMPTY) {
        return outcome;
      }
      nearest = nearest || outcome == PositionResolution::NEAREST;
    }
    return !frame.rowCount() ? PositionResolution::EMPTY
           : nearest         ? PositionResolution::NEAREST
                             : PositionResolution::EXACT;
  };
  if (!channels.selection) {
    resolution = aggregate();
    return value;
  }
  const bool selectionCompatible = value.selection.resource() == *resource;
  if (value.selection.rows().empty()) {
    value.selection.resource_ = *resource;
  }
  value.selection.resolution_ = value.selection.rows().empty() ? PositionResolution::EMPTY : PositionResolution::EXACT;
  if (!value.selection.rows().empty()) {
    if (!selectionCompatible) {
      value.selection.resolution_ = PositionResolution::INCOMPATIBLE;
      resolution = aggregate();
      return value;
    }
    const auto ids = value.selection.rows();
    const auto hints = value.selectedPositions;
    if (hints && hints->positions.size() != ids.size()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    std::shared_ptr<std::vector<RowId>> selected;
    std::shared_ptr<detail::ModelSelectionPositions> positions;
    auto preparePositions = [&](std::size_t prefix) -> std::expected<void, TreeError> {
      if (!hints || positions) {
        return {};
      }
      auto allocated = history->selectedPositions(ids.size());
      if (!allocated) {
        return std::unexpected(allocated.error());
      }
      positions = std::move(*allocated);
      positions->positions.insert(positions->positions.end(), hints->positions.begin(),
                                  hints->positions.begin() + prefix);
      return {};
    };
    for (std::size_t index = 0; index < ids.size(); ++index) {
      const auto id = ids[index];
      if (hints && (hints->positions[index].resource() != value.selection.resource() ||
                    hints->positions[index].position().row != id)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      auto located = frame.locateRow(id);
      if (!located) {
        return std::unexpected(located.error());
      }
      const bool currentConfirmed = value.current && value.current->position().row == id &&
                                    (value.currentResolution == PositionResolution::EXACT ||
                                     value.currentResolution == PositionResolution::NEAREST);
      if (located->state != RowLookupState::FOUND && !currentConfirmed) {
        const ModelBookmark *hint{};
        if (hints) {
          hint = &hints->positions[index];
        } else if (originalCurrent && originalCurrent->position().row == id) {
          hint = &*originalCurrent;
        }
        if (located->state == RowLookupState::NOT_FOUND && hint && hint->resource() == *resource && changes &&
            changes.fromRevision() == hint->position().revision) {
          auto mapped = changes.map(hint->position());
          if (!mapped) {
            return std::unexpected(detail::navigationTreeError(mapped.error()));
          }
          if (mapped->removed) {
            if (!selected) {
              selected = std::make_shared<std::vector<RowId>>(ids.begin(), ids.begin() + index);
              selected->reserve(ids.size());
            }
            auto prepared = preparePositions(index);
            if (!prepared) {
              return std::unexpected(prepared.error());
            }
            continue;
          }
        }
        value.selection.resolution_ = located->state == RowLookupState::UNSUPPORTED ? PositionResolution::UNSUPPORTED
                                                                                    : PositionResolution::INCOMPATIBLE;
        resolution = aggregate();
        return value;
      }
      if (selected) {
        selected->push_back(id);
      }
      if (hints) {
        const auto offset = located->state == RowLookupState::FOUND ? located->index : value.current->position().offset;
        const auto previous = hints->positions[index].position();
        if (!positions && (previous.offset != offset || previous.revision != frame.dataRevision())) {
          auto prepared = preparePositions(index);
          if (!prepared) {
            return std::unexpected(prepared.error());
          }
        }
        if (positions) {
          auto updated = ModelBookmark::create(
            *resource, {PositionKind::ITEM, id, offset, frame.dataRevision(), PositionAffinity::FORWARD}, {}, options);
          if (!updated) {
            return std::unexpected(detail::navigationTreeError(updated.error()));
          }
          positions->positions.push_back(std::move(*updated));
        }
      }
    }
    if (selected) {
      value.selection.rows_ = std::move(selected);
    }
    if (positions) {
      value.selectedPositions = std::move(positions);
    }
    if (value.selection.rows().empty()) {
      value.selection.resolution_ = PositionResolution::EMPTY;
    }
  }
  resolution = aggregate();
  return value;
}

template <class T, class W>
std::expected<Node<W>, ManagedError> ManagedScene::attachView(Node<W> node,
                                                              std::shared_ptr<detail::ManagedModelSourceOf<T>> source,
                                                              ManagedViewHandler<T> handler, ManagedChartOrigin first) {
  auto fail = [&](TreeError cause) -> std::expected<Node<W>, ManagedError> {
    auto failure = error(ManagedPhase::BIND, node, cause);
    auto removed = remove(node);

    if (!removed) {
      failure.cleanup = std::get<TreeError>(removed.error().cause);
    }

    return std::unexpected(failure);
  };

  try {
    if constexpr (std::same_as<W, VirtualList<T>>) {
      auto configured = app_.tree().edit(node, [&](W &list) {
        list.clearMissingSelection_ = source->sinkOnly;
      });
      if (!configured) {
        return fail(configured.error());
      }
    }
    auto entry = find(node);

    std::shared_ptr<detail::ManagedPreparedRegistration<T>> registration;
    if (handler) {
      registration = std::make_shared<detail::ManagedPreparedRegistration<T>>();
      registration->action = std::make_shared<ManagedViewHandler<T>>(std::move(handler));
      std::erase_if(source->prepared, [](const auto &weak) {
        return weak.expired();
      });
      source->prepared.push_back(registration);
    }

    // Factory-created nodes cannot carry another binding. Construct the whole callback
    // before publishing it, so allocation failure retires the new child transactionally.
    entry->viewSource = source;
    entry->viewPrepare = ViewPreparation<T, W>{this, node, source, registration, std::move(first)};

    if constexpr (std::same_as<W, VirtualTable<T>> || std::same_as<W, VirtualList<T>>) {
      auto binding = [&] {
        if constexpr (std::same_as<W, VirtualTable<T>>) {
          return bindVirtualTable(app_.router(), app_.tree(), node);
        } else {
          return bindVirtualList(app_.router(), app_.tree(), node);
        }
      }();

      if (!binding) {
        return fail(binding.error());
      }

      entry->binding.template emplace<ActionBinding>(std::move(*binding));
    }

    const char *type = std::same_as<W, Chart<T>> ? "Chart" : std::same_as<W, VirtualTable<T>> ? "Table" : "List";

    if (!entry->target || entry->target->type.empty()) {
      auto target = entry->target ? *entry->target : StyleTarget{};

      target.type = type;
      entry->target = std::make_shared<StyleTarget>(target);
    }

    entry->stylePending = true;
    stylesDirty_ = true;

    return node;
  } catch (const std::bad_alloc &) {
    return fail(TreeError::NO_MEMORY);
  } catch (...) {
    return fail(TreeError::CALLBACK_FAILED);
  }
}

/// @cond INTERNAL
/** @brief Staged model replacement that admits resources before installing a view binding. */
template <class T, class W> struct ManagedScene::PreparedViewModelChange final : detail::PreparedModelViewChange {
  ManagedScene *scene{};              ///< Borrowed scene owner, qualified by the weak lifetime guard.
  std::weak_ptr<const void> lifetime; ///< Weak scene lifetime checked before accessing the owner.
  Node<W> node;                       ///< Typed destination node whose generation is checked by the tree.
  std::shared_ptr<Entry> entry;       ///< Pinned entry whose original binding is revalidated before installation.
  std::shared_ptr<detail::ManagedModelSourceOf<T>>
    previousSource; ///< Old source retained through coherent group publication.
  std::shared_ptr<detail::ManagedModelSourceOf<T>>
    source; ///< Destination source admitted before any binding is replaced.
  std::shared_ptr<detail::ManagedPreparedRegistration<T>>
    registration; ///< Optional prepared-callback registration for the destination.
  std::shared_ptr<detail::ModelViewRestoreState>
    restoration; ///< Owned bounded restoration state and history participation.
  std::optional<detail::ModelViewHistoryChange> outgoing;    ///< Pre-admitted checkpoint for the model being left.
  std::optional<detail::ModelViewHistoryChange> destination; ///< Pre-admitted checkpoint for the destination model.
  std::function<std::expected<void, TreeError>(const LayoutSnapshot &)>
    preparation;                     ///< Prepared replacement callable swapped into the entry at install.
  ModelConsumer<T> consumer;         ///< Destination consumer admitted before installation.
  ModelConsumer<T> previousConsumer; ///< Old consumer retained until the replacement is coherent.
  ModelRequest<T> previousRequest;   ///< Old request cancelled only after coherent group publication.
  std::unique_ptr<typename W::Prepared> previousPrepared; ///< Old prepared payload retained past installation.
  detail::ModelViewCheckpoint target;     ///< Resolved current and reading checkpoint for the destination.
  std::optional<RowId> selected;          ///< Captured current row used to reject intervening view changes.
  std::optional<RowId> anchor;            ///< Captured reading anchor used for pre-install validation.
  std::optional<std::uint64_t> intent;    ///< Captured native navigation intent used for conflict detection.
  std::uint64_t intentRevision{};         ///< Captured native navigation intent revision.
  std::uint64_t anchorRevision{};         ///< Captured reading-anchor revision.
  ScrollState scroll;                     ///< Captured raster and fractional scroll state for validation.
  const typename W::Prepared *prepared{}; ///< Borrowed old payload identity, compared without dereferencing.
  const void *binding{};                  ///< Borrowed original preparation identity, compared without dereferencing.
  std::uint64_t sourceRevision{};         ///< Destination frame revision observed during admission.
  std::uint64_t navigationRevision{};     ///< Destination navigation revision observed during admission.
  bool reset{};                           ///< Whether installation replaces native view navigation state.
  bool pending{};                         ///< Whether unresolved restoration suppresses an initial stale frame.

  /** @brief Revalidate source, binding and view intent before installing the staged change.
   * @return Success while the staged inputs remain current, or a typed conflict/tree error.
   */
  std::expected<void, TreeError> validate() const override {
    if (lifetime.expired() || source->retired || previousSource->retired || scene->find(node) != entry ||
        entry->viewSource.lock() != previousSource ||
        entry->viewPrepare.template target<ViewPreparation<T, W>>() != binding ||
        (source->frame && source->frame->revision() != sourceRevision) ||
        source->navigationRevision != navigationRevision || (outgoing && !restoration->history->valid(*outgoing)) ||
        (destination && !restoration->history->valid(*destination))) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    bool unchanged{};
    auto checked = scene->app_.tree().edit(node, [&](W &view) {
      unchanged = view.selected_ == selected && view.anchor_ == anchor && view.intent_ == intent &&
                  view.intent_revision_ == intentRevision && view.anchorRevision_ == anchorRevision &&
                  view.scroll_.rasterOffset() == scroll.rasterOffset() &&
                  view.scroll_.fraction() == scroll.fraction() && view.prepared_.get() == prepared;
    });
    return !checked ? checked : unchanged ? std::expected<void, TreeError>{} : std::unexpected(TreeError::WOULD_BLOCK);
  }

  /** @brief Request paint for a replacement without publishing any new binding.
   * @return Success or the tree invalidation error; no binding is installed.
   */
  std::expected<void, TreeError> invalidate() const override {
    return reset ? scene->app_.tree().invalidate(node, Invalidation::PAINT) : std::expected<void, TreeError>{};
  }

  /** @brief Install pre-admitted state while retaining old owners until coherent publication.
   * @return Success or a typed tree error; retained owners outlive the install operation.
   */
  std::expected<void, TreeError> install() override {
    return scene->app_.tree().edit(node, [&](W &view) {
      if (reset) {
        if (source != previousSource) {
          previousConsumer = std::move(view.consumer_);
          view.consumer_ = std::move(consumer);
        }
        previousRequest = std::move(view.request_);
        previousPrepared = std::move(view.prepared_);
        view.selected_ = !pending && target.current &&
                             (target.currentResolution == PositionResolution::EXACT ||
                              target.currentResolution == PositionResolution::NEAREST)
                           ? std::optional(target.current->position().row)
                           : std::nullopt;
        view.intent_.reset();
        view.intent_revision_ = 0;
        view.scroll_ = {};
        (void)view.scroll_.setOffset(target.first);
        view.anchor_ = target.reading && target.reading->position().kind == PositionKind::ITEM
                         ? std::optional(target.reading->position().row)
                         : std::nullopt;
        view.anchorRevision_ = source->frame ? source->frame->dataRevision() : 0;
        view.dirty_ = true;
        view.retry_ = false;
        view.retryPending_ = false;
        view.retryFailedBase_ = false;
        view.retryFrameRevision_ = 0;
        view.preparedSuppressed_ = pending;
        if constexpr (std::same_as<W, VirtualList<T>>) {
          view.clearMissingSelection_ = source->sinkOnly;
          view.detachedCollection_ = false;
        }
      }
      if (registration) {
        source->prepared.push_back(registration);
      }
      entry->viewSource = source;
      entry->viewPrepare.swap(preparation);
    });
  }

  /** @brief Commit pre-admitted outgoing and destination checkpoints without allocation. */
  void commitHistory() noexcept override {
    if (outgoing) {
      restoration->history->commit(std::move(*outgoing));
    }
    if (destination) {
      restoration->history->commit(std::move(*destination));
    }
  }

  /** @brief Cancel the old request after the replacement group is coherent. */
  void retire() noexcept override {
    previousRequest.cancel();
  }
};

/// @endcond

template <class T, class W>
std::expected<std::unique_ptr<detail::PreparedModelViewChange>, ManagedError>
ManagedScene::prepareViewModelChange(Node<W> node, const std::shared_ptr<detail::ManagedModelSourceOf<T>> &source,
                                     std::optional<ManagedViewHandler<T>> prepared, ModelRebindOptions options,
                                     std::shared_ptr<detail::ModelViewHistory> history,
                                     const detail::ModelViewCheckpoint *sharedPosition,
                                     const NavigationBindingOptions *sharing) {
  auto entry = find(node);
  auto binding = entry ? entry->viewPrepare.template target<ViewPreparation<T, W>>() : nullptr;
  if (!binding) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }
  auto fail = [&](TreeError failure) -> std::expected<std::unique_ptr<detail::PreparedModelViewChange>, ManagedError> {
    return std::unexpected(error(ManagedPhase::INPUT, node, failure));
  };
  auto plan = std::make_unique<PreparedViewModelChange<T, W>>();
  plan->scene = this;
  plan->lifetime = lifetime_;
  plan->node = node;
  plan->entry = entry;
  plan->previousSource = binding->source;
  plan->source = source;
  plan->navigationRevision = source->navigationRevision;
  plan->binding = binding;
  plan->reset = plan->previousSource != source || options.restore == ModelRestorePolicy::RESET;
  auto saved = binding->restoration;
  if (!history) {
    history = saved ? saved->history : nullptr;
  }
  if (!history) {
    auto created = detail::ModelViewHistory::create(lifetime_, options.checkpoints.value_or(ModelCheckpointOptions{}));
    if (!created) {
      return fail(created.error());
    }
    history = std::move(*created);
  } else if (options.checkpoints &&
             (options.checkpoints->maxBytes != history->options().maxBytes ||
              options.checkpoints->bookmarks.maxBytes != history->options().bookmarks.maxBytes)) {
    return fail(TreeError::INVALID_ARGUMENT);
  }
  auto stateCredit = history->reserveState();
  if (!stateCredit) {
    return fail(stateCredit.error());
  }
  plan->restoration = std::make_shared<detail::ModelViewRestoreState>();
  plan->restoration->credit = std::move(*stateCredit);
  plan->restoration->history = history;
  plan->restoration->shareCurrent = sharing && sharing->current;
  plan->restoration->shareReading = sharing && sharing->readingAnchor;
  if (saved && saved->history == history) {
    plan->restoration->role = saved->role;
    plan->restoration->participant = saved->participant;
    plan->restoration->pending = saved->pending;
    plan->restoration->attemptedRevision = saved->attemptedRevision;
    plan->restoration->attemptedProofRevision = saved->attemptedProofRevision;
    plan->restoration->resolution = saved->resolution;
    plan->restoration->attempted = saved->attempted;
  } else {
    auto role = history->role();
    if (!role) {
      return fail(role.error());
    }
    plan->restoration->role = *role;
    auto credit = history->reserveRole();
    if (!credit) {
      return fail(credit.error());
    }
    plan->restoration->participant = std::make_shared<SourceCredit>(std::move(*credit));
  }
  auto action = prepared ? (*prepared ? std::make_shared<ManagedViewHandler<T>>(std::move(*prepared)) : nullptr)
                         : ((plan->previousSource == source || options.retainPreparedOverride) && binding->registration
                              ? binding->registration->action
                              : nullptr);
  if (action) {
    plan->registration = std::make_shared<detail::ManagedPreparedRegistration<T>>();
    plan->registration->action = std::move(action);
    std::erase_if(source->prepared, [](const auto &registration) {
      return registration.expired();
    });
    source->prepared.reserve(source->prepared.size() + 1);
  }
  auto observed = app_.tree().read(node, [&](const W &view) {
    plan->selected = view.selected_;
    plan->anchor = view.anchor_;
    plan->intent = view.intent_;
    plan->intentRevision = view.intent_revision_;
    plan->anchorRevision = view.anchorRevision_;
    plan->scroll = view.scroll_;
    plan->prepared = view.prepared_.get();
  });
  if (!observed) {
    return fail(observed.error());
  }
  if (!sharedPosition) {
    auto pinned = source->pin();
    if (!pinned || plan->lifetime.expired()) {
      return fail(!pinned ? pinned.error() : TreeError::CLOSED);
    }
  }
  plan->sourceRevision = source->frame ? source->frame->revision() : 0;
  if (plan->reset) {
    std::optional<detail::ModelViewCheckpoint> continuation;
    if (plan->previousSource != source) {
      auto captured = saved && saved->pending
                        ? std::expected<detail::ModelViewCheckpoint, TreeError>(*saved->pending)
                        : captureViewCheckpoint<T>(node, plan->previousSource, history->options().bookmarks);
      if (!captured || plan->lifetime.expired()) {
        return fail(!captured ? captured.error() : TreeError::CLOSED);
      }
      if (plan->previousSource->navigationResource == source->navigationResource) {
        continuation = std::move(*captured);
      } else {
        auto outgoing = history->stage(plan->previousSource, plan->restoration->role, std::move(*captured),
                                       plan->restoration->participant);
        if (!outgoing) {
          return fail(outgoing.error());
        }
        plan->outgoing = std::move(*outgoing);
      }
    }
    auto cached = history->find(source, plan->restoration->role);
    auto target =
      options.restore == ModelRestorePolicy::REMEMBER && cached ? cached->value : detail::ModelViewCheckpoint{};
    if (continuation && options.restore == ModelRestorePolicy::REMEMBER) {
      target = std::move(*continuation);
    }
    if (sharedPosition) {
      if (!sharing || sharing->current) {
        target.current = sharedPosition->current;
      }
      target.selection = sharedPosition->selection;
      target.selectedPositions = sharedPosition->selectedPositions;
      if (sharedPosition->reading) {
        target.reading = sharedPosition->reading;
      }
    }
    if (detail::hasModelRestoreIntent(target)) {
      plan->restoration->attempted = true;
      const auto unresolved = target;
      auto shown = detail::listShown(app_.tree(), node);
      if (!shown) {
        return fail(shown.error());
      }
      if (*shown) {
        auto resolved = resolveViewCheckpoint<T>(std::move(target), source, history, history->options().bookmarks,
                                                 plan->restoration->resolution);
        if (!resolved || plan->lifetime.expired()) {
          return fail(!resolved ? resolved.error() : TreeError::CLOSED);
        }
        target = std::move(*resolved);
      } else {
        plan->restoration->resolution = PositionResolution::PENDING;
      }
      plan->pending = !*shown || (plan->restoration->resolution != PositionResolution::EXACT &&
                                  plan->restoration->resolution != PositionResolution::NEAREST &&
                                  plan->restoration->resolution != PositionResolution::EMPTY);
      plan->restoration->pending = plan->pending ? std::optional(unresolved) : std::nullopt;
      plan->restoration->attemptedRevision = *shown ? source->frame->revision() : 0;
      plan->restoration->attemptedProofRevision = source->navigationRevision;
    } else {
      plan->restoration->pending.reset();
      plan->restoration->attempted = false;
    }
    plan->target = target;
    if (options.restore == ModelRestorePolicy::RESET || detail::hasModelRestoreIntent(target)) {
      auto destination = history->stage(source, plan->restoration->role, target, plan->restoration->participant);
      if (!destination) {
        return fail(destination.error());
      }
      plan->destination = std::move(*destination);
    }
    if (plan->previousSource != source) {
      auto opened = source->model->openRange();
      if (!opened) {
        return fail(opened.error());
      }
      plan->consumer = std::move(*opened);
    }
  }
  if (!plan->sourceRevision) {
    plan->sourceRevision = source->frame ? source->frame->revision() : 0;
  }
  plan->preparation = ViewPreparation<T, W>{this, node, source, plan->registration, {}, plan->restoration};
  return std::unique_ptr<detail::PreparedModelViewChange>(std::move(plan));
}

template <class T, class W>
std::expected<void, ManagedError> ManagedScene::replaceViewModel(Node<W> node, ManagedModel<T> model,
                                                                 std::optional<ManagedViewHandler<T>> prepared,
                                                                 ModelRebindOptions options) {
  auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);
  auto binding = entry ? entry->viewPrepare.template target<ViewPreparation<T, W>>() : nullptr;

  if (!binding) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  auto source = model.lock();

  if (!source) {
    return std::unexpected(error(ManagedPhase::INPUT, node, source.error()));
  }

  if ((*source)->retired || !(*source)->model) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::CLOSED));
  }

  if ((*source)->scene != root_.id()) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  const bool sameModel = binding->source == *source;

  // Callback-only changes do not navigate: in particular, they must not rebuild
  // channel outcomes or revoke a caller's outstanding beginResolve receipt.
  if (sameModel && prepared && options.restore == ModelRestorePolicy::REMEMBER) {
    struct ReplacingCallback {
      ManagedScene &scene;
      std::weak_ptr<const void> lifetime;

      ~ReplacingCallback() {
        if (!lifetime.expired()) {
          scene.preparing_ = false;
        }
      }
    } guard{*this, lifetime_};

    preparing_ = true;
    try {
      const auto original = binding;
      auto restoration = binding->restoration;
      auto first = binding->first;
      auto action = *prepared ? std::make_shared<ManagedViewHandler<T>>(std::move(*prepared)) : nullptr;
      std::shared_ptr<detail::ManagedPreparedRegistration<T>> registration;
      if (action) {
        registration = std::make_shared<detail::ManagedPreparedRegistration<T>>();
        registration->action = std::move(action);
        std::erase_if((*source)->prepared, [](const auto &value) {
          return value.expired();
        });
        (*source)->prepared.reserve((*source)->prepared.size() + 1);
      }
      std::function<std::expected<void, TreeError>(const LayoutSnapshot &)> next =
        ViewPreparation<T, W>{this, node, *source, registration, std::move(first), std::move(restoration)};
      if (guard.lifetime.expired() || (*source)->retired || find(node) != entry ||
          entry->viewPrepare.template target<ViewPreparation<T, W>>() != original) {
        return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::WOULD_BLOCK));
      }
      auto live = app_.tree().withScope(node, [](const UiScope &) {
      });
      if (!live) {
        return std::unexpected(error(ManagedPhase::INPUT, node, live.error()));
      }
      if (registration) {
        (*source)->prepared.push_back(registration);
      }
      entry->viewPrepare.swap(next);
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY));
    } catch (...) {
      return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED));
    }
  }

  if (navigation_) {
    for (const auto &member : navigation_->bindings) {
      if (member->node == node) {
        if (member->options.projection != NavigationProjection::SAME_MODEL) {
          return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
        }
        if (auto record = member->context.lock()) {
          return replaceNavigationModel(record, std::move(model), options, node, std::move(prepared));
        }
      }
    }
  }

  if (sameModel && !prepared && options.restore == ModelRestorePolicy::REMEMBER) {
    return {};
  }

  // Model admission and retiring old storage can release provider-owned data. Block scene
  // reentry until the whole transaction (including callback destruction) has completed.
  struct Rebinding {
    ManagedScene &scene;
    std::weak_ptr<const void> lifetime;

    ~Rebinding() {
      if (!lifetime.expired()) {
        scene.preparing_ = false;
      }
    }
  } guard{*this, lifetime_};

  preparing_ = true;

  try {
    auto staged = prepareViewModelChange<T>(node, *source, std::move(prepared), options, {});
    if (!staged) {
      return std::unexpected(staged.error());
    }
    auto valid = (*staged)->validate();
    if (!valid) {
      return std::unexpected(error(ManagedPhase::INPUT, node, valid.error()));
    }
    auto invalidated = (*staged)->invalidate();
    if (!invalidated) {
      return std::unexpected(error(ManagedPhase::INPUT, node, invalidated.error()));
    }
    auto installed = (*staged)->install();
    if (!installed) {
      return std::unexpected(error(ManagedPhase::INPUT, node, installed.error()));
    }
    (*staged)->commitHistory();
    (*staged)->retire();
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED));
  }
}

template <class T>
std::expected<void, ManagedError>
ManagedScene::setModel(Node<VirtualList<T>> node, ManagedModel<T> model,
                       std::type_identity_t<std::optional<ManagedViewHandler<T>>> prepared,
                       ModelRebindOptions options) {
  return replaceViewModel(node, std::move(model), std::move(prepared), options);
}

template <class T>
std::expected<void, ManagedError>
ManagedScene::setModel(Node<VirtualTable<T>> node, ManagedModel<T> model,
                       std::type_identity_t<std::optional<ManagedViewHandler<T>>> prepared,
                       ModelRebindOptions options) {
  return replaceViewModel(node, std::move(model), std::move(prepared), options);
}

template <class T>
std::expected<std::optional<PositionResolution>, ManagedError>
ManagedScene::restoreStatus(Node<VirtualList<T>> node) const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::STATE, node, TreeError::WRONG_THREAD));
  }
  if (closed_) {
    return std::unexpected(error(ManagedPhase::STATE, node, TreeError::CLOSED));
  }
  auto allowed = app_.tree().withScope(node, [](const UiScope &) {
  });
  if (!allowed) {
    return std::unexpected(error(ManagedPhase::STATE, node, allowed.error()));
  }
  auto entry = find(node);
  auto binding = entry ? entry->viewPrepare.template target<ViewPreparation<T, VirtualList<T>>>() : nullptr;
  if (!binding) {
    return std::unexpected(error(ManagedPhase::STATE, node, TreeError::INVALID_ARGUMENT));
  }
  return binding->restoration && binding->restoration->attempted ? std::optional(binding->restoration->resolution)
                                                                 : std::nullopt;
}

template <class T>
std::expected<std::optional<PositionResolution>, ManagedError>
ManagedScene::restoreStatus(Node<VirtualTable<T>> node) const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::STATE, node, TreeError::WRONG_THREAD));
  }
  if (closed_) {
    return std::unexpected(error(ManagedPhase::STATE, node, TreeError::CLOSED));
  }
  auto allowed = app_.tree().withScope(node, [](const UiScope &) {
  });
  if (!allowed) {
    return std::unexpected(error(ManagedPhase::STATE, node, allowed.error()));
  }
  auto entry = find(node);
  auto binding = entry ? entry->viewPrepare.template target<ViewPreparation<T, VirtualTable<T>>>() : nullptr;
  if (!binding) {
    return std::unexpected(error(ManagedPhase::STATE, node, TreeError::INVALID_ARGUMENT));
  }
  return binding->restoration && binding->restoration->attempted ? std::optional(binding->restoration->resolution)
                                                                 : std::nullopt;
}

template <class T>
std::expected<Node<VirtualTable<T>>, ManagedError>
ManagedScene::virtualTable(Node<Widget> parent, ManagedModel<T> model,
                           std::type_identity_t<std::span<const TableColumn<T>>> columns, VirtualTableOptions options,
                           SizePolicy policy, std::type_identity_t<ManagedViewHandler<T>> prepared) {
  auto source = modelSource(model, parent);

  if (!source) {
    return std::unexpected(source.error());
  }

  auto range = (*source)->model->openRange();

  if (!range) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, range.error()));
  }

  auto node = emplace<VirtualTable<T>>(parent, std::move(*range), columns, options, policy);

  if (!node) {
    return node;
  }

  return attachView(*node, *source, std::move(prepared));
}

template <class T>
std::expected<Node<VirtualList<T>>, ManagedError>
ManagedScene::virtualList(Node<Widget> parent, ManagedModel<T> model,
                          std::type_identity_t<ManagedListFormatter<T>> delegate, VirtualListOptions options,
                          SizePolicy policy, std::type_identity_t<ManagedViewHandler<T>> prepared) {
  auto source = modelSource(model, parent);

  if (!source) {
    return std::unexpected(source.error());
  }

  auto range = (*source)->model->openRange();

  if (!range) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, range.error()));
  }

  auto node = emplace<VirtualList<T>>(parent, std::move(*range), std::move(delegate), options, policy);

  if (!node) {
    return node;
  }

  return attachView(*node, *source, std::move(prepared));
}

template <class T>
std::expected<Node<Chart<T>>, ManagedError>
ManagedScene::chart(Node<Widget> parent, ManagedModel<T> model, std::type_identity_t<ManagedChartAccessor<T>> accessor,
                    ChartOptions options, ManagedChartOrigin first,
                    std::type_identity_t<ManagedViewHandler<T>> prepared, SizePolicy policy) {
  auto source = modelSource(model, parent);

  if (!source) {
    return std::unexpected(source.error());
  }

  auto range = (*source)->model->openRange();

  if (!range) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, range.error()));
  }

  auto node = emplace<Chart<T>>(parent, std::move(*range), std::move(accessor), options, policy);

  if (!node) {
    return node;
  }

  return attachView(*node, *source, std::move(prepared), std::move(first));
}
} // namespace portals
