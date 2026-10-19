// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "theme_selection_state.hpp"

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
} // namespace

namespace ui::detail {
struct ThemeSelectionAccess {
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

    if (record->busy) {
      return std::unexpected(
        selectionError(target->root, TreeError::WOULD_BLOCK, "Theme selection transition is active", origin));
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

      record->choices.push_back({data, choice.factory_});
      catalog->choices.push_back(data);
    }

    snapshot->catalog = catalog;
    record->snapshot = snapshot;

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
                                   std::source_location origin) {
  using Command = detail::ThemeSelectionCommand;

  const auto root = root_.id();
  const auto lifetime = std::weak_ptr<const void>(lifetime_);

  // Capture every authority component before factories or owning candidate/model copies.
  const auto capture = styles_.capturePreview();
  const bool active =
    record->snapshot->status == ui::ThemeSelectionStatus::ACTIVE && styles_.ownsPreview(record->preview);

  if (command != Command::PREVIEW && !active) {
    if (command == Command::CANCEL && record->snapshot->status == ui::ThemeSelectionStatus::IDLE) {
      return {};
    }

    return std::unexpected(selectionError(root, TreeError::INVALID_ARGUMENT, "No owned active theme preview", origin));
  }

  SelectionGuard guard(record->busy);

  try {
    std::optional<StyleSheet> sheet;
    const detail::ThemeSelectionChoice *choice{};

    if (command == Command::PREVIEW) {
      const auto found = std::ranges::find_if(record->choices, [&](const auto &entry) {
        return entry.data->id == id;
      });

      if (found == record->choices.end()) {
        return std::unexpected(selectionError(root, TreeError::INVALID_ARGUMENT, "Unknown theme catalog ID", origin));
      }

      choice = &*found;

      auto produced = choice->factory();

      if (!produced) {
        ManagedError failure{ManagedPhase::STYLE, root, produced.error(), {}, {}};

        selectionOrigin(failure, origin);

        return std::unexpected(failure);
      }

      // A factory may synchronously close the scene or perform an external style operation.
      if (lifetime.expired()) {
        return std::unexpected(selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
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
    }

    // The owning receipt and all ID storage are ready before the engine's sole commit.
    auto snapshot = std::make_shared<ui::ThemeSelectionSnapshot>(*record->snapshot);

    if (command == Command::PREVIEW) {
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
    const auto mutation = command == Command::PREVIEW  ? StyleEngine::Mutation::PREVIEW
                          : command == Command::COMMIT ? StyleEngine::Mutation::PREVIEW_COMMIT
                                                       : StyleEngine::Mutation::PREVIEW_CANCEL;

    if (lifetime.expired()) {
      return std::unexpected(selectionError(root, TreeError::CLOSED, "Theme selection owner was destroyed", origin));
    }

    auto installed = mutateStyles(mutation, {}, {}, {}, sheet ? &*sheet : nullptr, {}, &preview, nullptr, nullptr,
                                  record->layers, &capture);

    if (!installed) {
      auto failure = installed.error();
      selectionOrigin(failure, origin);

      return std::unexpected(failure);
    }

    record->snapshot = std::move(snapshot);
    record->preview = command == Command::PREVIEW ? preview : StyleLayerHandle{};

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
