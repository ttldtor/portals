// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"
#include "theme_selector_state.hpp"

#include <portals/ui.hpp>

#include <algorithm>

namespace portals {
namespace {
using SelectorCommand = detail::ThemeSelectorCommand;
// Each reserved scrollbar may consume one cell; retain a positive content viewport on both axes.
constexpr CellSize MINIMUM_CHOICE_AREA{2, 2};
constexpr std::int32_t FRAME_AND_TITLE_HEIGHT = 3;
// Frame, title, one status line and three actions precede the choice viewport.
constexpr CellSize MINIMUM_SELECTOR_PANEL{MINIMUM_CHOICE_AREA.width + 2, MINIMUM_CHOICE_AREA.height + 7};
static_assert(std::is_nothrow_copy_constructible_v<ManagedError>);

ManagedError selectorError(NodeId node, TreeError cause, std::source_location origin) {
  ManagedError failure{ManagedPhase::INPUT, node, cause, {}, {}};
  failure.operation = "ThemeSelector";
  failure.operationOrigin = origin;
  return failure;
}

struct SelectorGuard {
  bool &busy;
  bool previous;

  explicit SelectorGuard(bool &busy) : busy(busy), previous(busy) {
    busy = true;
  }

  ~SelectorGuard() {
    busy = previous;
  }
};

template <class F> struct OnExit {
  F action;
  bool enabled{true};

  ~OnExit() {
    if (enabled) {
      action();
    }
  }
};
} // namespace

namespace ui::detail {
std::expected<std::shared_ptr<portals::detail::ThemeSelectorRecord>, ManagedError>
ThemeSelectorAccess::enter(const std::shared_ptr<ThemeSelectorTarget> &target, std::source_location origin) {
  if (!target) {
    return std::unexpected(selectorError({}, TreeError::CLOSED, origin));
  }
  if (target->owner != std::this_thread::get_id()) {
    return std::unexpected(selectorError({}, TreeError::WRONG_THREAD, origin));
  }
  if (target->transition) {
    return std::unexpected(selectorError({}, TreeError::WOULD_BLOCK, origin));
  }
  if (!target->published || target->lifetime.expired()) {
    return std::unexpected(selectorError({}, TreeError::CLOSED, origin));
  }
  auto record = target->record.lock();
  if (!record || record->retired || !record->attached) {
    return std::unexpected(selectorError({}, TreeError::CLOSED, origin));
  }
  if (record->busy) {
    return std::unexpected(selectorError(record->dialog->root.id(), TreeError::WOULD_BLOCK, origin));
  }
  auto &scene = *target->scene;
  scene.retireThemeSelector();
  if (target->lifetime.expired() || record->retired) {
    return std::unexpected(selectorError(record->dialog->root.id(), TreeError::CLOSED, origin));
  }
  auto allowed = scene.boundary(ManagedPhase::INPUT, record->dialog->root);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  return record;
}

std::expected<void, ManagedError> ThemeSelectorAccess::command(const std::shared_ptr<ThemeSelectorTarget> &target,
                                                               SelectorCommand command, std::size_t index,
                                                               std::source_location origin) {
  auto record = enter(target, origin);

  if (!record) {
    return std::unexpected(record.error());
  }

  return target->scene->changeThemeSelector(*record, command, index, origin);
}

void ThemeSelectorAccess::interactive(const std::shared_ptr<ThemeSelectorTarget> &target, SelectorCommand command,
                                      std::size_t index) {
  const auto origin = std::source_location::current();
  const auto previous = target->last;
  const auto result = ThemeSelectorAccess::command(target, command, index, origin);
  const auto completed = target->last;
  // Reporting is part of the same user command; an observer cannot begin another delivery.
  SelectorGuard reporting(target->transition);
  const auto report = [&](const ManagedError &failure) {
    if (target->lifetime.expired()) {
      return;
    }
    auto record = target->record.lock();
    if (!record || record->retired || target->scene->closed_ ||
        !target->scene->boundary(ManagedPhase::INPUT, record->dialog->root)) {
      return;
    }
    if (!ManagedScene::focusReported(failure)) {
      (void)target->scene->reportError(failure, {}, failure.operationOrigin.value_or(origin));
    }
  };
  if (completed && completed != previous) {
    if (completed->callbackError && !target->callbackReported) {
      target->callbackReported = true;
      report(*completed->callbackError);
    }
    if (completed->closeError && !target->closeReported) {
      target->closeReported = true;
      report(*completed->closeError);
    }
  } else if (!result) {
    report(result.error());
  }
}

Description<Dialog> ThemeSelectorAccess::describe(ThemeSelectorRef reference, ThemeSelectionRef model,
                                                  ThemeSelectorOptions options) {
  return Access::themeSelectorControl(
    describeView(std::move(reference), std::move(model), std::move(options), {}, false));
}

Description<Dialog> ThemeSelectorAccess::describeView(ThemeSelectorRef reference, ThemeSelectionRef model,
                                                      ThemeSelectorOptions options, ControlSettings settings,
                                                      bool resolvedSettings) {
  return Access::cold<Dialog>(
    [reference = std::move(reference), model = std::move(model), options, settings = std::move(settings),
     resolvedSettings](ManagedScene &scene, Node<Widget> parent,
                       MountContext &context) mutable -> std::expected<Node<Dialog>, ManagedError> {
      if (!resolvedSettings && context.controlSettings) {
        settings = *context.controlSettings;
        options = controlThemeSelectorOptions(settings, std::move(options));
      }

      if (!options.textResolver && context.themeSelectorResolver) {
        options.textResolver = *context.themeSelectorResolver;
      }

      struct RestoreResolver {
        MountContext &context;
        const TextResolver *previous;

        ~RestoreResolver() {
          context.themeSelectorResolver = previous;
        }
      } restore{context, std::exchange(context.themeSelectorResolver, nullptr)};

      const auto target = reference.target_;
      const auto origin = std::source_location::current();
      if (!target || target->owner != std::this_thread::get_id()) {
        return std::unexpected(selectorError(parent.id(), TreeError::WRONG_THREAD, origin));
      }
      scene.retireThemeSelector();
      if (target->published || scene.themeSelector_ || options.dialog.mode != DialogMode::MODAL ||
          !options.textLimits.max_input_bytes || !options.textLimits.max_cluster_bytes ||
          !options.textLimits.max_graphemes || !options.textLimits.max_cells) {
        return std::unexpected(selectorError(parent.id(), TreeError::INVALID_ARGUMENT, origin));
      }
      options.dialog.closeOnEscape = false;
      const auto policy = scene.widthPolicy();
      const auto lifetime = std::weak_ptr<const void>(scene.lifetime_);
      const auto resolve = [&](const TextValue &value) -> std::expected<GraphemeText, ManagedError> {
        auto text = options.textResolver ? value.resolve(policy, options.textResolver, options.textLimits)
                                         : value.resolve(policy, options.textLimits);
        if (!text) {
          auto failure = selectorError(parent.id(), TreeError::INVALID_ARGUMENT, origin);
          if (const auto *cause = std::get_if<TextError>(&text.error())) {
            failure.text = *cause;
          } else {
            failure.resource = std::get<TextResourceError>(text.error());
          }
          return std::unexpected(failure);
        }
        return *text;
      };
      const std::array<const TextValue *, 7> expressions{
        &options.title,       &options.apply,       &options.cancel,        &options.closeKeepingPreview,
        &options.emptyStatus, &options.errorStatus, &options.conflictStatus};
      std::array<std::optional<GraphemeText>, 7> captions;
      for (std::size_t i = 0; i < expressions.size(); ++i) {
        auto caption = resolve(*expressions[i]);
        if (!caption) {
          return std::unexpected(caption.error());
        }
        captions[i] = *caption;
        if (lifetime.expired()) {
          return std::unexpected(selectorError(parent.id(), TreeError::CLOSED, origin));
        }
      }
      auto record = std::make_shared<portals::detail::ThemeSelectorRecord>(model, target);
      record->shortcut = options.openingShortcut;
      record->receiver = std::make_shared<portals::detail::StyleThemeReceiver>();
      if (options.onCommitted) {
        record->callback = std::make_shared<decltype(options.onCommitted)>(options.onCommitted);
      }
      if (lifetime.expired()) {
        return std::unexpected(selectorError(parent.id(), TreeError::CLOSED, origin));
      }
      const std::array<bool, 6> materializedCaptions{settings.themeSelector.labels.apply.has_value(),
                                                     settings.themeSelector.labels.cancel.has_value(),
                                                     settings.themeSelector.labels.closeKeepingPreview.has_value(),
                                                     settings.themeSelector.labels.emptyStatus.has_value(),
                                                     settings.themeSelector.labels.errorStatus.has_value(),
                                                     settings.themeSelector.labels.conflictStatus.has_value()};
      auto body = Access::cold<Column>(
        [record, captions, materializedCaptions, panelHeight = options.dialog.maximumPanelSize.height](
          ManagedScene &owner, Node<Widget> into, MountContext &mount) -> std::expected<Node<Column>, ManagedError> {
          auto body = owner.emplace<Column>(into);
          if (!body) {
            return std::unexpected(body.error());
          }
          SizePolicy bodyPolicy;
          bodyPolicy.height.minimum = MINIMUM_SELECTOR_PANEL.height - FRAME_AND_TITLE_HEIGHT;
          bodyPolicy.height.preferred = std::max(0, panelHeight - FRAME_AND_TITLE_HEIGHT);
          auto sized = setSizePolicy(owner.app_.tree(), *body, bodyPolicy);
          if (!sized) {
            return std::unexpected(selectorError(body->id(), sized.error(), std::source_location::current()));
          }
          auto rows = Access::cold<Column>([record](ManagedScene &owner, Node<Widget> into,
                                                    MountContext &) -> std::expected<Node<Column>, ManagedError> {
            auto rows = owner.emplace<Column>(into);
            if (rows) {
              record->rows = *rows;
            }
            return rows;
          });
          auto area = ui::scrollArea(std::move(rows), ScrollAreaOptions{.focusable = false});
          auto mounted = Access::mount(std::move(area), owner, *body, mount);
          if (!mounted) {
            return std::unexpected(mounted.error());
          }
          record->scroll = *mounted;
          SizePolicy scrollPolicy;
          scrollPolicy.width.minimum = MINIMUM_CHOICE_AREA.width;
          scrollPolicy.height = {.minimum = MINIMUM_CHOICE_AREA.height, .preferred = 0, .mode = SizeMode::FLEX};
          sized = setSizePolicy(owner.app_.tree(), *mounted, scrollPolicy);
          if (!sized) {
            return std::unexpected(selectorError(mounted->id(), sized.error(), std::source_location::current()));
          }
          for (std::size_t i = 0; i < record->status.size(); ++i) {
            const TextValue caption(captions[i + 4]->bytes());
            auto description = ui::label(caption);

            if (materializedCaptions[i + 3]) {
              Access::preparedThemeSelectorCaption(description, caption);
            }
            auto label = Access::mount(std::move(description), owner, *body, mount);
            if (!label) {
              return std::unexpected(label.error());
            }
            record->status[i] = *label;
            auto shown = owner.app_.tree().setVisible(*label, i == 0);
            if (!shown) {
              return std::unexpected(selectorError(label->id(), shown.error(), std::source_location::current()));
            }
          }
          const std::array commands{SelectorCommand::APPLY, SelectorCommand::CANCEL, SelectorCommand::KEEP};
          for (std::size_t i = 0; i < commands.size(); ++i) {
            const TextValue caption(captions[i + 1]->bytes());
            auto description = ui::button(caption, [target = record->target, command = commands[i]] {
              ThemeSelectorAccess::interactive(target, command);
            });

            if (materializedCaptions[i]) {
              Access::preparedThemeSelectorCaption(description, caption);
            }
            auto button = Access::mount(std::move(description), owner, *body, mount);
            if (!button) {
              return std::unexpected(button.error());
            }
            if (i == 0) {
              record->apply = *button;
              auto enabled = owner.app_.tree().setEnabled(*button, false);
              if (!enabled) {
                return std::unexpected(selectorError(button->id(), enabled.error(), std::source_location::current()));
              }
            }
          }
          return *body;
        });
      const TextValue title(captions[0]->bytes());
      auto recipe =
        Access::internalDialog(ui::dialog(title, std::move(body), options.dialog), settings.title ? &title : nullptr);
      auto mounted = Access::mount(std::move(recipe), scene, parent, context);
      if (!mounted) {
        return mounted;
      }
      record->dialog = scene.findDialog(*mounted);
      if (!record->dialog) {
        return std::unexpected(selectorError(mounted->id(), TreeError::STALE_NODE, origin));
      }

      // This composition owns routed modal input; generic eager Escape is deliberately disabled.
      auto entry = scene.find(*mounted);
      entry->binding = std::monostate{};
      auto routed = [&scene, weak = std::weak_ptr(record), lifetime](Widget &, const RoutedEvent &event) {
        auto record = weak.lock();
        if (!record || record->retired || !record->attached || !record->dialog->open || lifetime.expired()) {
          return EventReply::IGNORED;
        }
        std::optional<SelectorCommand> command;
        std::optional<std::size_t> next;
        if (const auto *action = std::get_if<ActionEvent>(&event)) {
          if (action->action == portals::Action::CANCEL) {
            command = SelectorCommand::CANCEL;
          } else if (!record->buttons.empty()) {
            const auto current = record->selected.value_or(0);
            if (action->action == portals::Action::MOVE_DOWN) {
              next = record->selected ? std::min(current + 1, record->buttons.size() - 1) : 0;
            }
            if (action->action == portals::Action::MOVE_UP) {
              next = record->selected ? (current ? current - 1 : 0) : record->buttons.size() - 1;
            }
            if (action->action == portals::Action::MOVE_HOME) {
              next = 0;
            }
            if (action->action == portals::Action::MOVE_END) {
              next = record->buttons.size() - 1;
            }
          }
        } else if (const auto *key = std::get_if<KeyEvent>(&event);
                   key && !record->buttons.empty() &&
                   (key->key == InputKey::PAGE_DOWN || key->key == InputKey::PAGE_UP)) {
          auto geometry = scene.geometry(record->scroll);
          const auto page = geometry ? static_cast<std::size_t>(std::max(1, geometry->clip.size().height)) : 1;
          const auto current = record->selected.value_or(0);
          next = key->key == InputKey::PAGE_DOWN
                   ? std::min(current + std::min(page, record->buttons.size() - 1), record->buttons.size() - 1)
                   : current - std::min(current, page);
        } else if (const auto *mouse = std::get_if<MouseInputEvent>(&event);
                   mouse && mouse->button == MouseButton::LEFT && mouse->action == MouseAction::PRESS) {
          auto panel = scene.geometry(record->dialog->panel);
          if (panel && !panel->surface.contains({mouse->column, mouse->row})) {
            command = SelectorCommand::CANCEL;
          }
        }
        if (next) {
          ThemeSelectorAccess::interactive(record->target, SelectorCommand::SELECT, *next);
          return EventReply::HANDLED;
        }
        if (command) {
          ThemeSelectorAccess::interactive(record->target, *command);
          return EventReply::HANDLED;
        }
        return EventReply::IGNORED;
      };
      auto binding = scene.app_.router().bind(*mounted, routed, false);
      if (!binding) {
        return std::unexpected(selectorError(mounted->id(), binding.error(), origin));
      }
      entry->binding = std::move(*binding);
      // Row navigation must run before the enclosing area's ordinary scroll-key handling.
      auto rowsBinding = scene.app_.router().bind(record->rows, routed, false);
      if (!rowsBinding) {
        return std::unexpected(selectorError(record->rows.id(), rowsBinding.error(), origin));
      }
      scene.find(record->rows)->binding = std::move(*rowsBinding);
      if (lifetime.expired()) {
        return std::unexpected(selectorError(mounted->id(), TreeError::CLOSED, origin));
      }
      auto admitted = scene.boundary(ManagedPhase::BIND, *mounted);
      if (!admitted) {
        return std::unexpected(admitted.error());
      }
      if (target->published || scene.themeSelector_) {
        return std::unexpected(selectorError(mounted->id(), TreeError::INVALID_ARGUMENT, origin));
      }
      record->receiver->node = *mounted;
      scene.themeSelector_ = record;
      return mounted;
    });
}

Description<Dialog> ThemeSelectorAccess::describeDefault(ThemeSelectorRef reference,
                                                         std::span<const ThemeChoice> choices,
                                                         const ThemeSelectionOptions &selection,
                                                         const std::optional<std::string> &initialId,
                                                         ThemeSelectorOptions options) {
  auto catalog = std::make_shared<ThemeSelectorCatalog>();

  catalog->choices.assign(choices.begin(), choices.end());
  catalog->selection = selection;
  catalog->initialId = initialId;

  auto description =
    Access::cold<Dialog>([reference = std::move(reference), catalog = std::move(catalog),
                          options](ManagedScene &scene, Node<Widget> parent,
                                   MountContext &context) mutable -> std::expected<Node<Dialog>, ManagedError> {
      const auto origin = std::source_location::current();
      const auto target = reference.target_;

      if (!target) {
        return std::unexpected(selectorError(parent.id(), TreeError::CLOSED, origin));
      }
      if (target->owner != std::this_thread::get_id()) {
        return std::unexpected(selectorError(parent.id(), TreeError::WRONG_THREAD, origin));
      }

      scene.retireThemeSelector();

      if (target->published || scene.themeSelector_) {
        return std::unexpected(selectorError(parent.id(), TreeError::INVALID_ARGUMENT, origin));
      }

      const auto settings = context.controlSettings ? *context.controlSettings : ControlSettings{};
      auto selection = catalog->selection;
      auto initialId = catalog->initialId;
      auto configured = controlThemeCatalogOptions(settings, selection, initialId);
      if (!configured) {
        return std::unexpected(selectorError(parent.id(), configured.error(), origin));
      }

      ThemeSelectionRef model;
      auto installed = scene.installThemeSelection(model, catalog->choices, selection);

      if (!installed) {
        return std::unexpected(installed.error());
      }

      const auto lifetime = std::weak_ptr<const void>(scene.lifetime_);
      const auto ownedModel = scene.themeSelection_;
      const auto releaseModel = [&scene, lifetime, ownedModel] {
        // This synchronous transaction owns only the just-installed, still-unpreviewed model.
        if (!lifetime.expired() && scene.themeSelection_ == ownedModel) {
          scene.themeSelection_.reset();
        }
      };
      OnExit admission{releaseModel};

      context.rollback.emplace_back(releaseModel);
      admission.enabled = false;

      auto effective = controlThemeSelectorOptions(settings, options);
      auto recipe = describeView(reference, model, std::move(effective), settings, true);

      auto mounted = Access::mount(std::move(recipe), scene, parent, context);

      if (!mounted) {
        return mounted;
      }

      scene.themeSelector_->initialId = std::move(initialId);

      return mounted;
    });

  return Access::themeCatalogControl(std::move(description));
}

Description<Dialog> defaultThemeSelector(ThemeSelectorRef reference, std::span<const ThemeChoice> choices,
                                         const ThemeSelectionOptions &selection,
                                         const std::optional<std::string> &initialId, ThemeSelectorOptions options) {
  return ThemeSelectorAccess::describeDefault(std::move(reference), choices, selection, initialId, std::move(options));
}
} // namespace ui::detail

void ManagedScene::retireThemeSelector() noexcept {
  auto record = themeSelector_;
  if (!record) {
    return;
  }
  auto eligible = app_.tree().inputEligible(record->dialog->root,
                                            record->dialog->open ? Node<Widget>{} : Node<Widget>(record->dialog->root));
  if (!closed_ && !record->retired && eligible && *eligible) {
    return;
  }
  record->retired = true;
  record->receiver->retired = true;
  if (record->target->record.lock() == record) {
    record->target->record.reset();
  }
  themeSelector_.reset();
  // Canonical cancellation lifetime is detached here before releasing user captures.
  if (record->openingLifetime) {
    record->openingLifetime->connected.store(false);
  }
  (void)record->opening.remove();
  record->callback.reset();
}

std::expected<void, ManagedError> ManagedScene::attachThemeSelector(const std::optional<ManagedError> *setupFailure) {
  retireThemeSelector();
  const auto record = themeSelector_;
  if (!record || record->attached) {
    return {};
  }
  auto allowed = boundary(ManagedPhase::BIND, record->dialog->root);
  if (!allowed) {
    return allowed;
  }
  const auto modelTarget = record->model.target_;
  const auto origin = std::source_location::current();
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  if (!modelTarget || !modelTarget->published || modelTarget->lifetime.expired() || modelTarget->scene != this ||
      record->target->published || !themeSelection_ ||
      (themeSelection_->selector.lock() && !themeSelection_->selector.lock()->retired)) {
    return std::unexpected(selectorError(record->dialog->root.id(), TreeError::INVALID_ARGUMENT, origin));
  }
  if (record->initialId) {
    // Catalog normalization precedes mounting; the first theme factory follows all Scene configuration.
    const auto validateOwner = [&]() -> std::expected<void, ManagedError> {
      if (setupFailure && *setupFailure) {
        return std::unexpected(**setupFailure);
      }
      if (lifetime.expired()) {
        return std::unexpected(selectorError(record->dialog->root.id(), TreeError::CLOSED, origin));
      }

      auto live = boundary(ManagedPhase::BIND, record->dialog->root);

      if (!live || themeSelector_ != record || record->target->published ||
          themeSelection_ != modelTarget->record.lock()) {
        return !live ? live
                     : std::unexpected(selectorError(record->dialog->root.id(), TreeError::INVALID_ARGUMENT, origin));
      }

      return {};
    };
    auto previewed = record->model.preview(*record->initialId, origin);

    if (!previewed) {
      return previewed;
    }
    auto live = validateOwner();

    if (!live) {
      return live;
    }

    auto committed = record->model.commit(origin);

    if (!committed) {
      return committed;
    }

    live = validateOwner();

    if (!live) {
      return live;
    }

    record->initialId.reset();
  }
  try {
    const auto model = themeSelection_;
    const auto snapshot = model->snapshot;
    std::vector<Node<Button>> buttons(0);
    std::vector<Node<Column>> rows(0);
    std::optional<KeyBinding> opening;
    auto openingLifetime = std::make_shared<detail::ActionCancellation>();
    OnExit rollback{[&] {
      openingLifetime->connected.store(false);
      if (!lifetime.expired() && !closed_) {
        for (auto row : rows) {
          (void)remove(row);
        }
        if (opening) {
          (void)opening->remove();
        }
      }
    }};
    buttons.reserve(snapshot->catalog->choices.size());
    rows.reserve(snapshot->catalog->choices.size());
    if (lifetime.expired()) {
      return std::unexpected(selectorError(record->dialog->root.id(), TreeError::CLOSED, origin));
    }
    for (std::size_t i = 0; i < snapshot->catalog->choices.size(); ++i) {
      const auto choice = snapshot->catalog->choices[i];
      auto row = emplace<Column>(record->rows);
      if (!row) {
        return std::unexpected(row.error());
      }
      rows.push_back(*row);
      auto button = emplace<Button>(*row, choice->label);
      if (!button) {
        return std::unexpected(button.error());
      }
      buttons.push_back(*button);
      auto styled = style(*button, StyleTarget{.type = "Button", .classes = {"theme-choice"}});
      if (!styled) {
        return styled;
      }
      auto bound =
        onClick(*button, [target = record->target, weak = std::weak_ptr(record), i](const ClickEvent &click) {
          const auto record = weak.lock();
          const auto *action = std::get_if<ActionEvent>(&click.event);
          const bool apply = record && record->selected == i && action && action->source == ClickSource::KEYBOARD;
          ui::detail::ThemeSelectorAccess::interactive(target, apply ? SelectorCommand::APPLY : SelectorCommand::SELECT,
                                                       i);
        });
      if (!bound) {
        return bound;
      }
      if (choice->description) {
        auto description = emplace<Label>(*row, *choice->description);
        if (!description) {
          return std::unexpected(description.error());
        }
      }
    }
    if (record->shortcut) {
      KeyInfo info(TextValue("Themes"));
      info.owner = root_;
      info.ownerLifetime_ = openingLifetime;
      auto bound = onKey(ui::press(*record->shortcut), std::move(info), [target = record->target] {
        ui::detail::ThemeSelectorAccess::interactive(target, SelectorCommand::OPEN);
      });
      if (!bound) {
        return std::unexpected(bound.error());
      }
      opening = *bound;
    }
    if (lifetime.expired()) {
      return std::unexpected(selectorError(record->dialog->root.id(), TreeError::CLOSED, origin));
    }
    allowed = boundary(ManagedPhase::BIND, record->dialog->root);
    const auto other = model->selector.lock();
    if (setupFailure && *setupFailure) {
      return std::unexpected(**setupFailure);
    }
    if (!allowed || themeSelector_ != record || record->target->published || modelTarget->scene != this ||
        modelTarget->record.lock() != model || themeSelection_ != model || (other && !other->retired)) {
      return !allowed ? allowed
                      : std::unexpected(selectorError(record->dialog->root.id(), TreeError::INVALID_ARGUMENT, origin));
    }
    record->buttons.swap(buttons);
    if (opening) {
      record->opening = *opening;
    }
    record->openingLifetime = std::move(openingLifetime);
    record->modelRecord = themeSelection_;
    record->attached = true;
    themeSelection_->selector = record;
    styles_.themeReceiver_ = record->receiver;
    record->target->scene = this;
    record->target->lifetime = lifetime_;
    record->target->record = record;
    record->target->published = true;
    rollback.enabled = false;
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(selectorError(record->dialog->root.id(), TreeError::NO_MEMORY, origin));
  } catch (...) {
    return std::unexpected(selectorError(record->dialog->root.id(), TreeError::CALLBACK_FAILED, origin));
  }
}

std::expected<void, ManagedError> ManagedScene::prepareThemeSelector() {
  retireThemeSelector();
  const auto record = themeSelector_;
  if (!record) {
    return {};
  }
  if (!record->attached) {
    auto attached = attachThemeSelector();
    if (!attached) {
      return attached;
    }
  }
  record->receiver->visible = record->dialog->open;
  if (record->busy || !record->dialog->open || !record->receiver->dirty) {
    return {};
  }
  const auto model = record->modelRecord.lock();
  if (!model) {
    return std::unexpected(
      selectorError(record->dialog->root.id(), TreeError::CLOSED, std::source_location::current()));
  }
  const bool active =
    model->snapshot->status == ui::ThemeSelectionStatus::ACTIVE && styles_.ownsPreview(model->preview);
  const bool superseded = model->snapshot->status == ui::ThemeSelectionStatus::ACTIVE && !active;
  record->applyEnabled = active && record->selected && *record->selected < model->snapshot->catalog->choices.size() &&
                         model->snapshot->previewedId == model->snapshot->catalog->choices[*record->selected]->id;
  const auto status = superseded ? 2u : record->failed ? 1u : 0u;
  auto enabled = app_.tree().setEnabled(record->apply, record->applyEnabled);
  if (!enabled) {
    return std::unexpected(selectorError(record->apply.id(), enabled.error(), std::source_location::current()));
  }
  for (std::size_t i = 0; i < record->status.size(); ++i) {
    auto shown = app_.tree().setVisible(record->status[i], i == status);
    if (!shown) {
      return std::unexpected(selectorError(record->status[i].id(), shown.error(), std::source_location::current()));
    }
  }
  record->receiver->visible = record->dialog->open;
  record->receiver->dirty = false;
  return {};
}

std::expected<void, ManagedError>
ManagedScene::changeThemeSelector(const std::shared_ptr<detail::ThemeSelectorRecord> &record, SelectorCommand command,
                                  std::size_t index, std::source_location origin) {
  SelectorGuard guard(record->busy);
  SelectorGuard transition(record->target->transition);
  const auto root = record->dialog->root;
  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  const auto target = record->target;
  const auto model = record->modelRecord.lock();
  if (!model) {
    return std::unexpected(selectorError(root.id(), TreeError::CLOSED, origin));
  }
  const auto live = [&] {
    return !lifetime.expired() && !closed_ && !record->retired && app_.tree().inputParent(root).has_value();
  };
  const auto refresh = [&] {
    if (live()) {
      record->receiver->visible = record->dialog->open;
      record->receiver->dirty = true;
      if (record->dialog->open) {
        (void)app_.tree().invalidate(root, Invalidation::PAINT);
      }
    } else {
      record->receiver->retired = true;
      record->retired = true;
    }
  };
  try {
    if (command == SelectorCommand::OPEN) {
      if (!record->dialog->open) {
        auto host = record->dialog->host.lock();
        auto geometry =
          host ? app_.tree().observeGeometry(host->root)
               : std::expected<WidgetTree::GeometryObservation, TreeError>(std::unexpected(TreeError::STALE_NODE));
        if (!geometry || !geometry->geometry.valid) {
          return std::unexpected(
            selectorError(root.id(), geometry ? TreeError::INVALID_GEOMETRY : geometry.error(), origin));
        }
        // Existing Dialog geometry computes the usable margin/caps before modal publication.
        auto available = Dialog(record->dialog->options).childAvailable(geometry->geometry.surface.size());
        if (!available || available->width < MINIMUM_SELECTOR_PANEL.width ||
            available->height < MINIMUM_SELECTOR_PANEL.height) {
          return std::unexpected(
            selectorError(root.id(), available ? TreeError::INVALID_GEOMETRY : available.error(), origin));
        }
        record->selected.reset();
        if (model->snapshot->status == ui::ThemeSelectionStatus::ACTIVE && styles_.ownsPreview(model->preview)) {
          const auto &choices = model->snapshot->catalog->choices;
          for (std::size_t i = 0; i < choices.size(); ++i) {
            if (model->snapshot->previewedId == choices[i]->id) {
              record->selected = i;
              break;
            }
          }
        }
      }
      auto focus = record->buttons.empty() ? Node<Widget>(record->apply)
                                           : Node<Widget>(record->buttons[record->selected.value_or(0)]);
      record->dialog->initialFocus = focus;
      auto opened = record->dialog->open ? this->focus(focus) : openDialog(root);
      record->failed = !opened;
      refresh();
      return opened;
    }
    if (!record->dialog->open) {
      return std::unexpected(selectorError(root.id(), TreeError::CLOSED, origin));
    }
    if (command == SelectorCommand::SELECT) {
      if (index >= model->snapshot->catalog->choices.size()) {
        return std::unexpected(selectorError(root.id(), TreeError::INVALID_ARGUMENT, origin));
      }
      const auto choice = model->snapshot->catalog->choices[index];
      auto preview = record->model.preview(choice->id, origin);
      record->failed = !preview;
      if (preview) {
        record->selected = index;
      }
      refresh();
      if (preview && live()) {
        return focus(record->buttons[index]);
      }
      return preview;
    }
    if (command == SelectorCommand::APPLY) {
      auto admitted = model->checkTransition(root_.id(), origin);
      if (!admitted) {
        return admitted;
      }

      if (!record->selected || *record->selected >= model->snapshot->catalog->choices.size()) {
        return std::unexpected(selectorError(root.id(), TreeError::INVALID_ARGUMENT, origin));
      }
      // Outcome/ID and callback ownership precede the guarded model commit.
      auto outcome = std::make_shared<ui::ThemeApplyOutcome>();
      outcome->committedId.assign(model->snapshot->catalog->choices[*record->selected]->id);
      const auto callback = record->callback;
      auto committed =
        changeThemeSelection(model, detail::ThemeSelectionCommand::COMMIT, {}, origin, outcome->committedId);
      if (!committed) {
        record->failed = true;
        refresh();
        return committed;
      }
      // Commit is complete. Guard remains held; no callback or close failure can repeat this delivery.
      if (callback) {
        try {
          auto delivered = (*callback)(outcome->committedId);
          if (!delivered) {
            outcome->callbackError = delivered.error();
          }
        } catch (const std::bad_alloc &) {
          outcome->callbackError = selectorError(root.id(), TreeError::NO_MEMORY, origin);
        } catch (...) {
          outcome->callbackError = selectorError(root.id(), TreeError::CALLBACK_FAILED, origin);
        }
      }
      if (live()) {
        auto closed = dismissDialog(root);
        if (!closed) {
          outcome->closeError = closed.error();
        }
        outcome->closeDisposition = !live()                ? ui::ThemeSelectorCloseDisposition::RETIRED
                                    : record->dialog->open ? ui::ThemeSelectorCloseDisposition::OPEN
                                                           : ui::ThemeSelectorCloseDisposition::CLOSED;
      } else {
        outcome->closeDisposition = ui::ThemeSelectorCloseDisposition::RETIRED;
      }
      target->callbackReported = false;
      target->closeReported = false;
      target->last = outcome;
      record->failed = outcome->callbackError.has_value() || outcome->closeError.has_value();
      refresh();
      if (outcome->callbackError) {
        return std::unexpected(*outcome->callbackError);
      }
      if (outcome->closeError) {
        return std::unexpected(*outcome->closeError);
      }
      return {};
    }
    if (command == SelectorCommand::CANCEL) {
      auto cancelled = record->model.cancel(origin);
      if (!cancelled) {
        record->failed = true;
        refresh();
        return cancelled;
      }
      record->selected.reset();
      record->failed = false;
    }
    auto closed = live() ? dismissDialog(root) : std::expected<void, ManagedError>{};
    record->failed = !closed;
    refresh();
    return closed;
  } catch (const std::bad_alloc &) {
    record->failed = true;
    refresh();
    return std::unexpected(selectorError(root.id(), TreeError::NO_MEMORY, origin));
  } catch (...) {
    record->failed = true;
    refresh();
    return std::unexpected(selectorError(root.id(), TreeError::CALLBACK_FAILED, origin));
  }
}
} // namespace portals

namespace portals::ui {
ThemeSelectorRef::ThemeSelectorRef() : target_(std::make_shared<detail::ThemeSelectorTarget>()) {
}

std::expected<void, ManagedError> ThemeSelectorRef::open(std::source_location origin) const {
  return detail::ThemeSelectorAccess::command(target_, portals::detail::ThemeSelectorCommand::OPEN, 0, origin);
}

std::expected<void, ManagedError> ThemeSelectorRef::preview(std::string_view id, std::source_location origin) const {
  auto record = detail::ThemeSelectorAccess::enter(target_, origin);

  if (!record) {
    return std::unexpected(record.error());
  }

  return (*record)->model.preview(id, origin);
}

std::expected<void, ManagedError> ThemeSelectorRef::commit(std::source_location origin) const {
  auto record = detail::ThemeSelectorAccess::enter(target_, origin);

  if (!record) {
    return std::unexpected(record.error());
  }

  return (*record)->model.commit(origin);
}

std::expected<std::shared_ptr<const ThemeSelectionSnapshot>, ManagedError> ThemeSelectorRef::snapshot() const {
  auto record = detail::ThemeSelectorAccess::enter(target_, std::source_location::current());

  if (!record) {
    return std::unexpected(record.error());
  }

  return (*record)->model.snapshot();
}

std::expected<void, ManagedError> ThemeSelectorRef::apply(std::source_location origin) const {
  return detail::ThemeSelectorAccess::command(target_, portals::detail::ThemeSelectorCommand::APPLY, 0, origin);
}

std::expected<void, ManagedError> ThemeSelectorRef::cancel(std::source_location origin) const {
  return detail::ThemeSelectorAccess::command(target_, portals::detail::ThemeSelectorCommand::CANCEL, 0, origin);
}

std::expected<void, ManagedError> ThemeSelectorRef::closeKeepingPreview(std::source_location origin) const {
  return detail::ThemeSelectorAccess::command(target_, portals::detail::ThemeSelectorCommand::KEEP, 0, origin);
}

std::expected<std::shared_ptr<const ThemeApplyOutcome>, ManagedError> ThemeSelectorRef::lastApply() const {
  if (target_ && target_->owner != std::this_thread::get_id()) {
    return std::unexpected(selectorError({}, TreeError::WRONG_THREAD, std::source_location::current()));
  }
  return target_ ? target_->last : std::shared_ptr<const ThemeApplyOutcome>{};
}

Description<Dialog> themeSelector(ThemeSelectorRef reference, ThemeSelectionRef model, ThemeSelectorOptions options) {
  return detail::ThemeSelectorAccess::describe(std::move(reference), std::move(model), std::move(options));
}
} // namespace portals::ui
