// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "message_box_state.hpp"
#include <portals/ui.hpp>

#include <algorithm>
#include <limits>

namespace portals::detail {
MessageParagraph::MessageParagraph(GraphemeText text, std::size_t maximum) : text_(std::move(text)), maximum_(maximum) {
}

std::expected<CellSize, TreeError> MessageParagraph::measure(CellSize available) {
  const auto width = std::max(available.width, 0);
  if (!width) {
    return CellSize{};
  }
  if (width_ == width) {
    return CellSize{measuredWidth_, static_cast<std::int32_t>(lines_.size())};
  }

  try {
    std::vector<std::optional<GraphemeText>> staged(0);
    std::size_t begin{};
    std::int32_t cells{}, maximumWidth{};
    bool replaceLine{};
    const auto append = [&](std::size_t end, bool replacement = false) -> std::expected<void, TreeError> {
      if (staged.size() >= maximum_ || staged.size() >= std::size_t(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected(TreeError::NODE_LIMIT);
      }
      auto line = GraphemeText::from(
        replacement || replaceLine ? std::string_view("?") : text_.bytes().substr(begin, end - begin), text_.policy());
      if (!line) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      maximumWidth = std::max(maximumWidth, line->measure());
      staged.push_back(std::move(*line));
      return {};
    };
    for (const auto &cluster : text_.graphemes()) {
      const auto bytes = text_.bytes().substr(cluster.byte_begin, cluster.byte_end - cluster.byte_begin);
      if (bytes == "\n" || bytes == "\r" || bytes == "\r\n") {
        auto added = append(cluster.byte_begin);
        if (!added) {
          return std::unexpected(added.error());
        }
        begin = cluster.byte_end;
        cells = 0;
        replaceLine = false;
        continue;
      }
      if (cells && cells + cluster.cells > width) {
        auto added = append(cluster.byte_begin);
        if (!added) {
          return std::unexpected(added.error());
        }
        begin = cluster.byte_begin;
        cells = 0;
        replaceLine = false;
      }
      if (cluster.cells > width) {
        replaceLine = true;
        cells = 1;
      } else {
        cells += cluster.cells;
      }
    }
    if (begin != text_.bytes().size() || staged.empty() || text_.bytes().ends_with('\n') ||
        text_.bytes().ends_with('\r')) {
      auto added = append(text_.bytes().size());
      if (!added) {
        return std::unexpected(added.error());
      }
    }
    lines_.swap(staged);
    width_ = width;
    measuredWidth_ = maximumWidth;
    return CellSize{measuredWidth_, static_cast<std::int32_t>(lines_.size())};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> MessageParagraph::arrange(CellRect local) {
  auto bounds = CellRect::from({}, local.size());
  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  bounds_ = *bounds;
  return {};
}

std::expected<void, TreeError> MessageParagraph::paint(PaintContext &context) {
  if (bounds_.empty()) {
    return {};
  }
  const auto style = computedStyle() ? computedStyle()->normal : CellStyle{};
  for (std::size_t line = 0; line < lines_.size(); ++line) {
    const RenderCommand command = TextCommand{{0, static_cast<std::int32_t>(line)}, *lines_[line], bounds_, style};
    auto submitted = context.submit(std::span(&command, 1));
    if (!submitted) {
      return submitted;
    }
  }
  return {};
}
} // namespace portals::detail

namespace portals::ui {
MessageBox::MessageBox() : target_(std::make_shared<detail::MessageTarget>()) {
}

DialogCommandResult MessageBox::cancel(std::source_location origin) const {
  auto scene = detail::eventScene(target_);
  return detail::MessageAccess::command(
    target_, scene ? detail::MessageAccess::cancel(**scene, target_->node) : std::unexpected(scene.error()), origin);
}

std::expected<std::shared_ptr<const MessageResult>, ManagedError> MessageBox::lastResult() const {
  auto scene = detail::eventScene(target_);
  return scene ? detail::MessageAccess::last(**scene, target_->node) : std::unexpected(scene.error());
}

std::expected<AsyncReceive<MessageResult>, ManagedError> MessageBox::receive(const UiAsyncContext &context) const {
  return receiveTarget(context, {});
}

std::expected<AsyncReceive<MessageResult>, ManagedError>
MessageBox::receiveTarget(const UiAsyncContext &context, std::shared_ptr<detail::RefTarget> owner) const {
  auto scene = detail::eventScene(target_);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  auto node = (*scene)->root();
  if (owner) {
    auto ownerScene = detail::eventScene(owner);
    if (!ownerScene) {
      return std::unexpected(ownerScene.error());
    }
    if (*ownerScene != *scene) {
      return std::unexpected(ManagedError{ManagedPhase::INPUT, owner->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }
    node = owner->node;
  }
  return detail::MessageAccess::receive(**scene, target_->node, node, context);
}

namespace detail {
namespace {
std::expected<GraphemeText, ManagedError> resolve(const TextValue &value, const MessageOptions &options,
                                                  ManagedScene &scene, Node<Widget> parent) {
  auto result = options.resolver ? value.resolve(scene.widthPolicy(), options.resolver, options.textLimits)
                                 : value.resolve(scene.widthPolicy(), options.textLimits);
  if (result) {
    return std::move(*result);
  }
  auto failure = error(parent, TreeError::INVALID_ARGUMENT);
  if (auto text = std::get_if<TextError>(&result.error())) {
    failure = textError(parent, *text);
  } else {
    failure.resource = std::get<TextResourceError>(result.error());
  }
  return std::unexpected(failure);
}
} // namespace

Description<Dialog> MessageAccess::describe(MessageBox box, TextValue title, TextValue body, MessageOptions options,
                                            int kind) {
  auto description = Access::cold<Dialog>(
    [box = std::move(box), title = std::move(title), body = std::move(body), options = std::move(options),
     kind](ManagedScene &scene, Node<Widget> parent,
           MountContext &context) mutable -> std::expected<Node<Dialog>, ManagedError> {
      const auto *settings = context.controlSettings;

      if (settings) {
        options = controlMessageOptions(*settings, std::move(options), kind);
        if (settings->title) {
          title = *settings->title;
        }
        if (settings->body) {
          body = *settings->body;
        }
      }
      if (!options.resolver && context.messageResolver) {
        options.resolver = *context.messageResolver;
      }
      struct RestoreResolver {
        MountContext &context;
        const TextResolver *previous;

        ~RestoreResolver() {
          context.messageResolver = previous;
        }
      } restore{context, std::exchange(context.messageResolver, nullptr)};

      if (!box.target_ || box.target_->owner != std::this_thread::get_id()) {
        return std::unexpected(error(parent, TreeError::WRONG_THREAD));
      }
      if (scene.collectingMessages_ || box.target_->pending || box.target_->node != Node<Widget>{} ||
          !options.maxBodyLines || !options.textLimits.max_input_bytes || !options.textLimits.max_cluster_bytes ||
          !options.textLimits.max_graphemes || !options.textLimits.max_cells ||
          options.dialog.mode != DialogMode::MODAL || !options.dialog.closeOnEscape) {
        return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
      }
      context.release.push_back([target = box.target_] {
        target->pending = false;
      });
      if (box.target_->pending || box.target_->node != Node<Widget>{}) {
        return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
      }
      box.target_->pending = true;
      const std::array<MessageChoice, 3> choices =
        kind == 0   ? std::array{MessageChoice::ACKNOWLEDGE, MessageChoice::CANCEL, MessageChoice::CANCEL}
        : kind == 1 ? std::array{MessageChoice::CONFIRM, MessageChoice::CANCEL, MessageChoice::CANCEL}
                    : std::array{MessageChoice::SAVE, MessageChoice::DISCARD, MessageChoice::CANCEL};
      const std::size_t count = static_cast<std::size_t>(kind + 1);
      const auto selected =
        options.defaultChoice.value_or(kind == 0 ? MessageChoice::ACKNOWLEDGE : MessageChoice::CANCEL);
      if (!std::ranges::contains(std::span(choices).first(count), selected) ||
          ((selected == MessageChoice::CONFIRM || selected == MessageChoice::DISCARD) &&
           !options.allowDestructiveDefault)) {
        return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
      }
      auto heading = resolve(title, options, scene, parent);
      if (!heading) {
        return std::unexpected(heading.error());
      }
      auto paragraph = resolve(body, options, scene, parent);
      if (!paragraph) {
        return std::unexpected(paragraph.error());
      }
      std::array<std::optional<GraphemeText>, 3> labels;
      for (std::size_t i = 0; i < count; ++i) {
        const auto choice = choices[i];
        const auto &expression = choice == MessageChoice::ACKNOWLEDGE ? options.labels.acknowledge
                                 : choice == MessageChoice::CONFIRM   ? options.labels.confirm
                                 : choice == MessageChoice::SAVE      ? options.labels.save
                                 : choice == MessageChoice::DISCARD   ? options.labels.discard
                                                                      : options.labels.cancel;
        auto text = resolve(expression, options, scene, parent);
        if (!text) {
          return std::unexpected(text.error());
        }
        if (text->bytes().empty() || text->bytes().find_first_of("\r\n") != std::string_view::npos) {
          return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
        }
        labels[i] = std::move(*text);
      }
      auto record = std::make_shared<portals::detail::MessageRecord>();
      record->count = count;
      auto content = Access::cold<Column>([paragraph = std::move(*paragraph), labels = labels, record, choices,
                                           selected, maximum = options.maxBodyLines](
                                            ManagedScene &owner, Node<Widget> into,
                                            MountContext &mount) mutable -> std::expected<Node<Column>, ManagedError> {
        auto column = owner.emplace<Column>(into);
        if (!column) {
          return std::unexpected(column.error());
        }
        auto explanation = owner.emplace<portals::detail::MessageParagraph>(*column, std::move(paragraph), maximum);
        if (!explanation) {
          return std::unexpected(explanation.error());
        }
        record->paragraph = *explanation;
        mount.nodes.push_back(*explanation);
        auto styled = owner.style(*explanation, StyleTarget{.type = "MessageParagraph", .classes = {"message-body"}});
        if (!styled) {
          return std::unexpected(styled.error());
        }
        auto actions = owner.emplace<Column>(*column);
        if (!actions) {
          return std::unexpected(actions.error());
        }
        record->actions = *actions;
        mount.nodes.push_back(*actions);
        styled = owner.style(*actions, StyleTarget{.type = "Column", .classes = {"message-actions"}});
        if (!styled) {
          return std::unexpected(styled.error());
        }
        for (std::size_t i = 0; i < record->count; ++i) {
          auto button = owner.emplace<Button>(*actions, std::move(*labels[i]));
          if (!button) {
            return std::unexpected(button.error());
          }
          mount.nodes.push_back(*button);
          record->buttons[i] = *button;
          const auto choice = choices[i];
          const char *name = choice == MessageChoice::ACKNOWLEDGE ? "message-acknowledge"
                             : choice == MessageChoice::CONFIRM   ? "message-confirm"
                             : choice == MessageChoice::SAVE      ? "message-save"
                             : choice == MessageChoice::DISCARD   ? "message-discard"
                                                                  : "message-cancel";
          StyleTarget style = choice == MessageChoice::CONFIRM || choice == MessageChoice::DISCARD
                                ? StyleTarget{.type = "Button", .classes = {name, "destructive"}}
                                : StyleTarget{.type = "Button", .classes = {name}};
          styled = owner.style(*button, std::move(style));
          if (!styled) {
            return std::unexpected(styled.error());
          }
          auto bound =
            owner.onClick(*button, [&owner, weak = std::weak_ptr(record), choice]() -> std::expected<void, TreeError> {
              auto live = weak.lock();
              if (!live || !live->current || live->current->done || live->current->result || live->current->starting) {
                return std::unexpected(TreeError::CLOSED);
              }
              auto request = live->current;
              if (request->async && !portals::detail::asyncInvocationCurrent(request->invocation)) {
                return std::unexpected(TreeError::CLOSED);
              }
              auto eligible = owner.messageParent(request->owner);
              if (!eligible) {
                return std::unexpected(TreeError::CLOSED);
              }
              request->choosing = choice;
              auto dismissed = owner.dialogCommand(live->dialog->root, false, DialogDismissReason::PROGRAMMATIC);
              request->choosing.reset();
              if (!dismissed && !ManagedScene::focusReported(dismissed.error())) {
                owner.reportShortError(dismissed.error(), "MessageBox.choose", {}, "Dialog",
                                       std::source_location::current(), owner.actionAdmitted_);
              }
              return {};
            });
          if (!bound) {
            return std::unexpected(bound.error());
          }
          if (choice == selected) {
            auto focused = owner.focusDefault(*button);
            if (!focused) {
              return std::unexpected(focused.error());
            }
          }
        }
        return *column;
      });
      ScrollAreaOptions scroll;
      // Preserve the entire tiny viewport for wrapped content and actions. The existing
      // area still owns wheel/keyboard scrolling and focus reveal, without bar gutters.
      scroll.horizontal = ScrollBarPolicy::NEVER;
      scroll.vertical = ScrollBarPolicy::NEVER;
      const TextValue resolvedTitle(heading->bytes());
      auto recipe =
        Access::internalDialog(ui::dialog(resolvedTitle, ui::scrollArea(std::move(content), scroll), options.dialog),
                               context.controlPrepared && settings && settings->title ? &resolvedTitle : nullptr);
      auto mounted = Access::mount(std::move(recipe), scene, parent, context);
      if (!mounted) {
        return mounted;
      }
      record->dialog = scene.findDialog(*mounted);
      if (!record->dialog) {
        return std::unexpected(error(*mounted, TreeError::STALE_NODE));
      }
      record->dialog->message = record;
      for (std::size_t i = 0; i < count; ++i) {
        if (choices[i] == selected) {
          record->dialog->initialFocus = record->buttons[i];
        }
      }
      scene.messages_.reserve(scene.messages_.size() + 1);
      auto styled = scene.style(*mounted, StyleTarget{.type = "Dialog",
                                                      .classes = {"dialog", "modal", "message-box",
                                                                  kind == 0   ? "information"
                                                                  : kind == 1 ? "confirm"
                                                                              : "unsaved"}});
      if (!styled) {
        return std::unexpected(styled.error());
      }
      context.publish.push_back([target = box.target_, record, &scene] {
        target->scene = &scene;
        target->lifetime = Access::lifetime(scene);
        target->node = record->dialog->root;
      });
      scene.messages_.push_back(record);
      return mounted;
    });

  return Access::messageControl(std::move(description));
}
} // namespace detail

Description<Dialog> messageBox(MessageBox box, TextValue title, TextValue body, MessageOptions options) {
  return detail::MessageAccess::describe(std::move(box), std::move(title), std::move(body), std::move(options), 0);
}

Description<Dialog> confirmDialog(MessageBox box, TextValue title, TextValue body, MessageOptions options) {
  return detail::MessageAccess::describe(std::move(box), std::move(title), std::move(body), std::move(options), 1);
}

Description<Dialog> unsavedChangesDialog(MessageBox box, TextValue title, TextValue body, MessageOptions options) {
  return detail::MessageAccess::describe(std::move(box), std::move(title), std::move(body), std::move(options), 2);
}

Description<Dialog> messageBox(MessageBox box, std::string_view title, std::string_view body, MessageOptions options) {
  return messageBox(std::move(box), TextValue(title), TextValue(body), std::move(options));
}

Description<Dialog> confirmDialog(MessageBox box, std::string_view title, std::string_view body,
                                  MessageOptions options) {
  return confirmDialog(std::move(box), TextValue(title), TextValue(body), std::move(options));
}

Description<Dialog> unsavedChangesDialog(MessageBox box, std::string_view title, std::string_view body,
                                         MessageOptions options) {
  return unsavedChangesDialog(std::move(box), TextValue(title), TextValue(body), std::move(options));
}
} // namespace portals::ui
