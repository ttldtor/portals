// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "theme_preparation.hpp"
#include "theme_selection_state.hpp"
#include "theme_selector_state.hpp"

#include <portals/managed.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>

namespace portals {
namespace {
ManagedError selectionError(NodeId node, TreeError cause, const char *message,
                            std::source_location origin = std::source_location::current()) {
  ManagedError failure{ManagedPhase::STYLE, node, StyleError{cause, 1, 1, message}, {}, {}};

  failure.operation = "theme selection";
  failure.operationOrigin = origin;

  return failure;
}

struct SelectionGuard {
  bool &flag;

  explicit SelectionGuard(bool &flag) : flag(flag) {
    flag = true;
  }

  ~SelectionGuard() {
    flag = false;
  }
};

struct SelectionSetupGuard {
  bool &flag;
  std::weak_ptr<const void> lifetime;

  SelectionSetupGuard(bool &flag, const std::shared_ptr<const void> &lifetime) : flag(flag), lifetime(lifetime) {
    flag = true;
  }

  ~SelectionSetupGuard() {
    if (!lifetime.expired()) {
      flag = false;
    }
  }
};

void selectionOrigin(ManagedError &failure, std::source_location origin) {
  failure.operation = "theme selection";
  failure.operationOrigin = origin;
}

void dirtySelector(const std::shared_ptr<detail::ThemeSelectionRecord> &record, bool failed = false) noexcept {
  if (auto selector = record->selector.lock()) {
    selector->failed = failed;
    selector->receiver->dirty = true;
  }
}

void retirePreparation(const std::shared_ptr<detail::ThemeSelectionRecord> &record) noexcept {
  if (record->preparation) {
    record->preparation->cancelWork();
  }

  record->intent.reset();
  record->prepared.reset();
  record->commitWhenPrepared = false;

  if (record->priorSnapshot) {
    record->snapshot = std::move(record->priorSnapshot);
  }

  record->completionSnapshot.reset();
}
} // namespace

std::expected<void, ManagedError> detail::ThemeSelectionRecord::checkTransition(NodeId owner,
                                                                                std::source_location origin) const {
  if (busy) {
    return std::unexpected(
      selectionError(owner, TreeError::WOULD_BLOCK, "Theme selection transition is active", origin));
  }

  return {};
}

namespace ui::detail {
struct ThemeSelectionAccess {
  static void failed(ManagedScene &scene, const std::shared_ptr<portals::detail::ThemeSelectionRecord> &record,
                     ManagedError failure, bool report = true) noexcept {
    if (!record->intent || !record->completionSnapshot) {
      return;
    }

    const auto origin = record->intent->origin;

    selectionOrigin(failure, origin);
    auto settled = std::move(record->completionSnapshot);

    settled->preparing = false;
    settled->preparationError = failure;
    record->snapshot = std::move(settled);
    record->prepared.reset();
    dirtySelector(record, true);
    if (report) {
      (void)scene.reportError(failure, {}, origin);
    }
  }

  static void complete(ManagedScene &scene, const std::shared_ptr<portals::detail::ThemeSelectionRecord> &record,
                       const std::shared_ptr<const portals::detail::PreparedThemeResult> &result) noexcept {
    if (!record->intent || record->intent != result->intent || result->intent->choice >= record->choices.size() ||
        record->choices[result->intent->choice].preparedFactory != result->intent->source) {
      return;
    }

    record->preparation->hasWork = false;

    try {
      if (result->styleError) {
        failed(scene, record, ManagedError{ManagedPhase::STYLE, scene.root_.id(), *result->styleError, {}, {}});
      } else if (result->asyncError) {
        auto failure = selectionError(scene.root_.id(), TreeError::CALLBACK_FAILED, "Theme preparation failed");

        failure.async = result->asyncError;
        failed(scene, record, failure);
      } else {
        record->prepared = result;

        const auto &id = record->choices[result->intent->choice].data->id;
        auto installed = scene.changeThemeSelection(record, portals::detail::ThemeSelectionCommand::PREPARED_PREVIEW,
                                                    id, result->intent->origin);

        record->prepared.reset();

        if (!installed) {
          failed(scene, record, installed.error());
        } else {
          const bool initialCommit = std::exchange(record->commitWhenPrepared, false);
          auto settled = std::move(record->completionSnapshot);

          record->intent.reset();
          record->priorSnapshot.reset();
          dirtySelector(record);

          if (initialCommit) {
            auto committed = scene.changeThemeSelection(record, portals::detail::ThemeSelectionCommand::COMMIT, {},
                                                        result->intent->origin);

            if (!committed) {
              // The preview was installed successfully. Preserve that real state,
              // using the preallocated receipt if the optional commit cannot stage.
              settled->status = ThemeSelectionStatus::ACTIVE;
              settled->previewedId.swap(settled->requestedId);
              settled->requestedId.reset();
              settled->preparationError = committed.error();
              record->snapshot = std::move(settled);
              dirtySelector(record, true);
              (void)scene.reportError(committed.error(), {}, result->intent->origin);
            }
          }
        }
      }
    } catch (const std::bad_alloc &) {
      auto failure = selectionError(scene.root_.id(), TreeError::NO_MEMORY, "Theme completion allocation failed");

      failure.async = AsyncError::NO_MEMORY;
      failed(scene, record, failure);
    } catch (...) {
      auto failure = selectionError(scene.root_.id(), TreeError::CALLBACK_FAILED, "Theme completion callback failed");

      failure.async = AsyncError::CALLBACK_FAILED;
      failed(scene, record, failure);
    }
  }

  static std::expected<void, ManagedError>
  prepare(ManagedScene &scene, const std::shared_ptr<portals::detail::ThemeSelectionRecord> &record) {
    auto required = SceneAccess::requireBackground(scene);

    if (!required) {
      return required;
    }

    auto activity = std::make_shared<portals::detail::ThemePreparation>();
    auto &application = SceneAccess::application(scene);
    auto scope = application.scheduler().makeScope();

    if (!scope) {
      auto failure = selectionError(scene.root_.id(), TreeError::NO_MEMORY, "Theme completion scope rejected");

      failure.async = portals::detail::asyncError(scope.error());

      return std::unexpected(failure);
    }

    activity->application = &application;
    activity->lifetime = scene.lifetime_;
    activity->scope = std::move(*scope);

    const auto weak = std::weak_ptr(record);
    const auto lifetime = std::weak_ptr<const void>(scene.lifetime_);
    auto connected = application.scheduler().connect<portals::detail::PreparedThemeResult>(
      activity->scope, [weak, lifetime, owner = &scene](auto result) noexcept {
        if (lifetime.expired()) {
          return;
        }

        if (auto record = weak.lock()) {
          complete(*owner, record, result);
        }
      });

    if (!connected) {
      auto failure = selectionError(scene.root_.id(), TreeError::NO_MEMORY, "Theme completion registration rejected");

      failure.async = portals::detail::asyncError(connected.error());

      return std::unexpected(failure);
    }

    activity->subscription = std::move(connected->subscription);
    activity->publisher = connected->publisher;
    activity->failed = [weak, lifetime, owner = &scene](std::optional<BackgroundError> background,
                                                        std::optional<AsyncError> async) noexcept {
      if (lifetime.expired()) {
        return;
      }

      if (auto record = weak.lock()) {
        auto failure =
          selectionError(owner->root_.id(), TreeError::CALLBACK_FAILED, "Theme background admission failed");

        failure.background = background;
        failure.async = async;
        failed(*owner, record, failure);
      }
    };

    record->preparation = std::move(activity);

    return {};
  }

  static std::expected<std::shared_ptr<portals::detail::ThemeSelectionRecord>, ManagedError>
  enter(const ThemeSelectionRef &reference, std::source_location origin) {
    const auto &target = reference.target_;

    if (!target) {
      return std::unexpected(selectionError({}, TreeError::CLOSED, "Theme selection is empty", origin));
    }

    // Immutable affinity is checked before any mutable state or raw scene pointer.
    if (target->owner != std::this_thread::get_id()) {
      return std::unexpected(
        selectionError({}, TreeError::WRONG_THREAD, "Theme selection belongs to another thread", origin));
    }

    if (!target->published || target->lifetime.expired()) {
      return std::unexpected(selectionError(target->root, TreeError::CLOSED, "Theme selection is not live", origin));
    }

    auto record = target->record.lock();

    if (!record) {
      return std::unexpected(selectionError(target->root, TreeError::CLOSED, "Theme selection is closed", origin));
    }

    auto allowed = target->scene->boundary(ManagedPhase::STYLE, target->scene->root_);

    if (!allowed) {
      auto failure = allowed.error();
      selectionOrigin(failure, origin);

      return std::unexpected(failure);
    }

    auto transition = record->checkTransition(target->root, origin);
    if (!transition) {
      return std::unexpected(transition.error());
    }

    return record;
  }

  static std::expected<void, ManagedError> install(ManagedScene &scene, const ThemeSelectionRef &reference,
                                                   const std::vector<ThemeChoice> &choices,
                                                   const ThemeSelectionOptions &options, std::source_location origin) {
    auto result = scene.installThemeSelection(reference, choices, options);

    if (!result) {
      selectionOrigin(result.error(), origin);
    }

    return result;
  }

  static std::expected<void, ManagedError> change(const ThemeSelectionRef &reference,
                                                  portals::detail::ThemeSelectionCommand command, std::string_view id,
                                                  std::source_location origin) {
    auto record = enter(reference, origin);

    if (!record) {
      return std::unexpected(record.error());
    }

    return reference.target_->scene->changeThemeSelection(*record, command, id, origin);
  }

  static auto snapshot(const ThemeSelectionRef &reference) {
    auto record = enter(reference, std::source_location::current());
    using Result = std::expected<std::shared_ptr<const ThemeSelectionSnapshot>, ManagedError>;

    if (!record) {
      return Result(std::unexpected(record.error()));
    }

    return reference.target_->scene->readThemeSelection(*record);
  }
};
} // namespace ui::detail

std::expected<void, ManagedError> ManagedScene::installThemeSelection(const ui::ThemeSelectionRef &reference,
                                                                      const std::vector<ui::ThemeChoice> &choices,
                                                                      const ui::ThemeSelectionOptions &options) {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return allowed;
  }

  const auto &target = reference.target_;

  if (!target) {
    return std::unexpected(selectionError(root_.id(), TreeError::CLOSED, "Theme selection is empty"));
  }

  if (target->owner != owner_) {
    return std::unexpected(
      selectionError(root_.id(), TreeError::WRONG_THREAD, "Theme selection belongs to another thread"));
  }

  if (themeSelection_ || installingThemeSelection_ || target->published) {
    return std::unexpected(
      selectionError(root_.id(), TreeError::INVALID_ARGUMENT, "One theme selection model is already installed"));
  }

  if (choices.empty() || !options.maxChoices || choices.size() > options.maxChoices || !options.maxIdBytes ||
      !options.maxLabelBytes || !options.maxDescriptionBytes || !options.maxTextBytes ||
      !options.textLimits.max_input_bytes || !options.textLimits.max_cluster_bytes ||
      !options.textLimits.max_graphemes || !options.textLimits.max_cells) {
    return std::unexpected(selectionError(root_.id(), TreeError::INVALID_ARGUMENT, "Invalid theme catalog limits"));
  }

  SelectionSetupGuard guard(installingThemeSelection_, lifetime_);

  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  const auto root = root_.id();
  const auto policy = app_.widthPolicy();

  try {
    auto record = std::make_shared<detail::ThemeSelectionRecord>();
    auto catalog = std::make_shared<ui::ThemeCatalog>();
    auto snapshot = std::make_shared<ui::ThemeSelectionSnapshot>();

    record->layers = options.layers;
    record->choices.reserve(choices.size());
    catalog->choices.reserve(choices.size());

    std::size_t bytes{};
    const auto charge = [&](std::size_t size) {
      if (size > options.maxTextBytes - bytes) {
        return false;
      }

      bytes += size;

      return true;
    };

    const auto resolve = [&](const TextValue &value) {
      return options.textResolver ? value.resolve(policy, options.textResolver, options.textLimits)
                                  : value.resolve(policy, options.textLimits);
    };

    const auto textFailure = [&](const TextValueError &cause) {
      auto failure = selectionError(root, TreeError::INVALID_ARGUMENT, "Invalid theme catalog text");

      if (const auto *text = std::get_if<TextError>(&cause)) {
        failure.text = *text;
      } else {
        failure.resource = std::get<TextResourceError>(cause);
      }

      return failure;
    };

    for (const auto &choice : choices) {
      if (choice.id_.empty() || choice.id_.size() > options.maxIdBytes || !charge(choice.id_.size()) ||
          std::ranges::any_of(record->choices, [&](const auto &entry) {
            return entry.data->id == choice.id_;
          })) {
        return std::unexpected(
          selectionError(root, TreeError::INVALID_ARGUMENT, "Invalid or duplicate theme catalog ID"));
      }

      auto id = GraphemeText::from(choice.id_, policy, options.textLimits);

      if (!id) {
        return std::unexpected(textFailure(TextValueError{id.error()}));
      }

      auto label = resolve(choice.label_);

      if (!label) {
        return std::unexpected(textFailure(label.error()));
      }

      if (label->bytes().size() > options.maxLabelBytes || !charge(label->bytes().size())) {
        return std::unexpected(
          selectionError(root, TreeError::INVALID_ARGUMENT, "Theme catalog label budget exhausted"));
      }

      auto data = std::make_shared<ui::ThemeChoiceData>(choice.id_, *label);

      if (choice.description_) {
        auto description = resolve(*choice.description_);

        if (!description) {
          return std::unexpected(textFailure(description.error()));
        }

        if (description->bytes().size() > options.maxDescriptionBytes || !charge(description->bytes().size())) {
          return std::unexpected(
            selectionError(root, TreeError::INVALID_ARGUMENT, "Theme catalog description budget exhausted"));
        }

        data->description = *description;
      }

      record->choices.push_back({data, choice.factory_, choice.preparedFactory_});
      catalog->choices.push_back(data);
    }

    snapshot->catalog = catalog;
    record->snapshot = snapshot;

    if (std::ranges::any_of(record->choices, [](const auto &choice) {
          return bool(choice.preparedFactory);
        })) {
      auto prepared = ui::detail::ThemeSelectionAccess::prepare(*this, record);

      if (!prepared) {
        return prepared;
      }
    }

    if (lifetime.expired()) {
      return std::unexpected(selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed"));
    }

    allowed = boundary(ManagedPhase::STYLE, root_);

    if (!allowed) {
      return allowed;
    }

    // Another scene can publish this shared target from a resolver or owning-copy hook.
    if (target->published) {
      return std::unexpected(
        selectionError(root, TreeError::INVALID_ARGUMENT, "Theme selection target is already published"));
    }

    // No callbacks, copies or allocations follow publication.
    themeSelection_ = record;
    target->scene = this;
    target->record = record;
    target->lifetime = lifetime;
    target->root = root;
    target->published = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(selectionError(root, TreeError::NO_MEMORY, "Theme catalog allocation failed"));
  } catch (const std::length_error &) {
    return std::unexpected(selectionError(root, TreeError::INVALID_ARGUMENT, "Theme catalog storage limit exceeded"));
  } catch (...) {
    return std::unexpected(selectionError(root, TreeError::CALLBACK_FAILED, "Theme catalog callback failed"));
  }
}

std::expected<std::shared_ptr<const ui::ThemeSelectionSnapshot>, ManagedError>
ManagedScene::readThemeSelection(const std::shared_ptr<detail::ThemeSelectionRecord> &record) const {
  if (record->snapshot->status != ui::ThemeSelectionStatus::ACTIVE || styles_.ownsPreview(record->preview)) {
    return record->snapshot;
  }

  const auto root = root_.id();
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  const auto previous = record->snapshot;
  SelectionGuard guard(record->busy);

  try {
    auto snapshot = std::make_shared<ui::ThemeSelectionSnapshot>(*previous);

    snapshot->status = ui::ThemeSelectionStatus::SUPERSEDED;

    if (lifetime.expired()) {
      return std::unexpected(selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed"));
    }

    auto allowed = boundary(ManagedPhase::STYLE, root_);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    return std::shared_ptr<const ui::ThemeSelectionSnapshot>(std::move(snapshot));
  } catch (const std::bad_alloc &) {
    return std::unexpected(selectionError(root, TreeError::NO_MEMORY, "Theme snapshot allocation failed"));
  }
}

std::expected<void, ManagedError>
ManagedScene::changeThemeSelection(const std::shared_ptr<detail::ThemeSelectionRecord> &record,
                                   detail::ThemeSelectionCommand command, std::string_view id,
                                   std::source_location origin, std::optional<std::string_view> expectedId) {
  auto admitted = record->checkTransition(root_.id(), origin);
  if (!admitted) {
    return admitted;
  }

  using Command = detail::ThemeSelectionCommand;
  const bool previewing = command == Command::PREVIEW || command == Command::PREPARED_PREVIEW;

  const auto root = root_.id();
  const auto lifetime = std::weak_ptr<const void>(lifetime_);

  // Capture every authority component before factories or owning candidate/model copies.
  const auto capture = styles_.capturePreview();
  const bool active =
    record->snapshot->status == ui::ThemeSelectionStatus::ACTIVE && styles_.ownsPreview(record->preview);

  if (command == Command::PREPARED_PREVIEW) {
    const auto &intent = record->prepared->intent;
    const auto &before = intent->authority;
    const auto &after = capture.lease;

    if (capture.lifetime.lock() != intent->engine.lock() || before.layered != after.layered ||
        before.epoch != after.epoch || before.id != after.id || before.owner != after.owner) {
      return std::unexpected(
        selectionError(root, TreeError::INVALID_ARGUMENT, "Theme authority changed during preparation", origin));
    }
  }

  // Cancellation may synchronously invoke a registered stop callback. Admit the
  // transition guard before retiring work, just as for synchronous factories.
  SelectionGuard guard(record->busy);

  if (command == Command::COMMIT && record->snapshot->requestedId &&
      (record->snapshot->preparing || record->snapshot->preparationError ||
       record->snapshot->requestedId != record->snapshot->previewedId)) {
    return std::unexpected(selectionError(root, TreeError::WOULD_BLOCK, "Requested theme is not installed", origin));
  }

  if (command == Command::CANCEL) {
    retirePreparation(record);
  }

  if (!previewing && !active) {
    if (command == Command::CANCEL && record->snapshot->status == ui::ThemeSelectionStatus::IDLE) {
      return {};
    }

    return std::unexpected(selectionError(root, TreeError::INVALID_ARGUMENT, "No owned active theme preview", origin));
  }

  try {
    std::optional<StyleSheet> sheet;
    const StyleSheet *candidate{};
    const detail::ThemeSelectionChoice *choice{};

    if (previewing) {
      const auto found = std::ranges::find_if(record->choices, [&](const auto &entry) {
        return entry.data->id == id;
      });

      if (found == record->choices.end()) {
        return std::unexpected(selectionError(root, TreeError::INVALID_ARGUMENT, "Unknown theme catalog ID", origin));
      }

      choice = &*found;

      if (command == Command::PREVIEW) {
        retirePreparation(record);
      }

      if (command == Command::PREVIEW && choice->preparedFactory) {
        if (lifetime.expired()) {
          return std::unexpected(
            selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
        }

        // Establish the same lazy layered representation before retaining authority.
        // An ordinary first USER/APPLICATION patch during preparation must not
        // look like a foreign THEME handoff merely because it enables layers.
        if (!styles_.capturePreview().lease.layered) {
          auto layered = enableStyleLayers(record->layers.value_or(StyleLayerOptions{}));

          if (!layered) {
            return layered;
          }
        }

        if (lifetime.expired()) {
          return std::unexpected(
            selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
        }

        const auto requestCapture = styles_.capturePreview();
        const auto limits = record->layers.value_or(styles_.layerState_.options);
        auto intent = std::make_shared<detail::PreparedThemeIntent>();
        auto result = std::make_shared<detail::PreparedThemeResult>();
        auto pending = std::make_shared<ui::ThemeSelectionSnapshot>(*record->snapshot);
        auto settled = std::make_shared<ui::ThemeSelectionSnapshot>(*record->snapshot);

        intent->choice = static_cast<std::size_t>(found - record->choices.begin());
        intent->source = choice->preparedFactory;
        intent->authority = requestCapture.lease;
        intent->engine = requestCapture.lifetime;
        intent->origin = origin;
        result->intent = intent;
        pending->requestedId = choice->data->id;
        pending->preparing = true;
        pending->preparationError.reset();
        settled->requestedId = choice->data->id;
        settled->preparing = false;
        settled->preparationError.reset();

        auto job = std::make_unique<detail::PreparedThemeJob>(choice->preparedFactory, result,
                                                              record->preparation->publisher, limits);

        if (lifetime.expired()) {
          return std::unexpected(
            selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
        }

        auto allowed = boundary(ManagedPhase::STYLE, root_);

        if (!allowed) {
          return allowed;
        }

        // Retirement above made this activity inactive, so admission prunes any
        // previous weak entry before retaining exactly one current invocation.
        auto roster = ui::detail::SceneAccess::admitActivity(*this, record->preparation);

        if (!roster) {
          return roster;
        }

        if (lifetime.expired()) {
          return std::unexpected(
            selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
        }

        record->completionSnapshot = std::move(settled);
        record->priorSnapshot = record->snapshot;
        record->intent = std::move(intent);
        record->snapshot = std::move(pending);
        dirtySelector(record);

        std::optional<BackgroundError> admission;
        std::optional<AsyncError> exception;

        try {
          auto accepted = record->preparation->replace(std::move(job));

          if (!accepted) {
            admission = accepted.error();
          }
        } catch (const std::bad_alloc &) {
          exception = AsyncError::NO_MEMORY;
        } catch (...) {
          exception = AsyncError::CALLBACK_FAILED;
        }

        if (admission || exception) {
          record->preparation->cancelWork();

          auto failure = selectionError(root, TreeError::CALLBACK_FAILED, "Theme background admission failed", origin);

          failure.background = admission;
          failure.async = exception;
          ui::detail::ThemeSelectionAccess::failed(*this, record, failure, false);

          return std::unexpected(failure);
        }

        return {};
      }

      if (command == Command::PREPARED_PREVIEW) {
        // Immutable worker storage stays pinned through the engine's normal
        // budget admission and staging; do not copy it beforehand on UI.
        candidate = &record->prepared->sheet;
      } else {
        auto produced = choice->factory();

        if (!produced) {
          ManagedError failure{ManagedPhase::STYLE, root, produced.error(), {}, {}};

          selectionOrigin(failure, origin);

          return std::unexpected(failure);
        }

        // A factory may synchronously close the scene or perform an external style operation.
        if (lifetime.expired()) {
          return std::unexpected(
            selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
        }

        auto allowed = boundary(ManagedPhase::STYLE, root_);

        if (!allowed) {
          auto failure = allowed.error();
          selectionOrigin(failure, origin);

          return std::unexpected(failure);
        }

        auto unchanged = styles_.validatePreviewCapture(capture);

        if (!unchanged) {
          ManagedError failure{ManagedPhase::STYLE, root, unchanged.error(), {}, {}};

          selectionOrigin(failure, origin);

          return std::unexpected(failure);
        }

        // Transfer the factory's owning result without copying payload before engine budget admission.
        sheet.emplace();
        sheet->rules.swap(produced->rules);
        sheet->scope = produced->scope;
        candidate = &*sheet;
      }
    }

    // The owning receipt and all ID storage are ready before the engine's sole commit.
    auto snapshot = std::make_shared<ui::ThemeSelectionSnapshot>(*record->snapshot);

    snapshot->requestedId.reset();
    snapshot->preparing = false;
    snapshot->preparationError.reset();

    if (previewing) {
      snapshot->status = ui::ThemeSelectionStatus::ACTIVE;
      snapshot->previewedId = choice->data->id;
    } else {
      snapshot->status = ui::ThemeSelectionStatus::IDLE;

      if (command == Command::COMMIT) {
        snapshot->lastCommittedId = snapshot->previewedId;
      }

      snapshot->previewedId.reset();
    }

    StyleLayerHandle preview = active ? record->preview : StyleLayerHandle{};
    const auto mutation = previewing                   ? StyleEngine::Mutation::PREVIEW
                          : command == Command::COMMIT ? StyleEngine::Mutation::PREVIEW_COMMIT
                                                       : StyleEngine::Mutation::PREVIEW_CANCEL;

    if (lifetime.expired()) {
      return std::unexpected(selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
    }

    if (expectedId && (command != Command::COMMIT || !record->snapshot->previewedId ||
                       *record->snapshot->previewedId != *expectedId)) {
      return std::unexpected(
        selectionError(root, TreeError::INVALID_ARGUMENT, "Selected preview changed before Apply", origin));
    }

    auto installed =
      mutateStyles(mutation, {}, {}, {}, candidate, {}, &preview, nullptr, nullptr, record->layers, &capture);

    if (!installed) {
      auto failure = installed.error();
      selectionOrigin(failure, origin);

      return std::unexpected(failure);
    }

    record->snapshot = std::move(snapshot);
    record->preview = previewing ? preview : StyleLayerHandle{};

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(selectionError(root, TreeError::NO_MEMORY, "Theme selection allocation failed", origin));
  } catch (const std::length_error &) {
    return std::unexpected(
      selectionError(root, TreeError::INVALID_ARGUMENT, "Theme selection storage limit exceeded", origin));
  } catch (...) {
    return std::unexpected(selectionError(root, TreeError::CALLBACK_FAILED, "Theme factory failed", origin));
  }
}
} // namespace portals

namespace portals::ui {
ThemeSelectionRef::ThemeSelectionRef() : target_(std::make_shared<detail::ThemeSelectionTarget>()) {
}

std::expected<void, ManagedError> ThemeSelectionRef::preview(std::string_view id, std::source_location origin) const {
  return detail::ThemeSelectionAccess::change(*this, portals::detail::ThemeSelectionCommand::PREVIEW, id, origin);
}

std::expected<void, ManagedError> ThemeSelectionRef::commit(std::source_location origin) const {
  return detail::ThemeSelectionAccess::change(*this, portals::detail::ThemeSelectionCommand::COMMIT, {}, origin);
}

std::expected<void, ManagedError> ThemeSelectionRef::cancel(std::source_location origin) const {
  return detail::ThemeSelectionAccess::change(*this, portals::detail::ThemeSelectionCommand::CANCEL, {}, origin);
}

std::expected<std::shared_ptr<const ThemeSelectionSnapshot>, ManagedError> ThemeSelectionRef::snapshot() const {
  return detail::ThemeSelectionAccess::snapshot(*this);
}

ThemeSelectionStage::ThemeSelectionStage(ThemeSelectionRef reference, std::initializer_list<ThemeChoice> catalog,
                                         ThemeSelectionOptions options, std::source_location origin)
    : description_(std::make_shared<detail::ThemeSelectionDescription>(std::move(reference), catalog, options)),
      origin_(origin) {
}

Scene &operator|(Scene &owner, const ThemeSelectionStage &stage) {
  auto configure = [description = stage.description_, origin = stage.origin_](ManagedScene &scene) {
    return detail::ThemeSelectionAccess::install(scene, description->reference, description->choices,
                                                 description->options, origin);
  };
  detail::SceneAccess::configure(owner, configure, stage.origin_);

  return owner;
}

Scene operator|(Scene &&input, const ThemeSelectionStage &stage) {
  Scene owner(std::move(input));
  owner | stage;

  return owner;
}

ThemeSelectionStage themeSelection(ThemeSelectionRef reference, std::initializer_list<ThemeChoice> catalog,
                                   ThemeSelectionOptions options, std::source_location origin) {
  return ThemeSelectionStage(std::move(reference), catalog, options, origin);
}
} // namespace portals::ui
