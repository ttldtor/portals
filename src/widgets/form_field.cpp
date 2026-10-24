// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_form_field.hpp>

namespace portals {
namespace detail {
struct FormFieldRecord {
  WidgetTree *tree{};
  std::weak_ptr<const void> lifetime;
  Node<FormField> field;
  Node<Widget> editor;
  Node<Label> message;
  WidthPolicy widthPolicy{};
  std::shared_ptr<const FieldValidation> presentation{std::make_shared<FieldValidation>()};
  std::shared_ptr<const FieldValidation> pending;
  std::shared_ptr<std::function<FieldValidation(std::string_view)>> rule;
  bool required{};
  std::string requiredMessage;
  bool active{true};
  bool validating{};
};
} // namespace detail

FieldValidation FieldValidation::valid(std::string message) {
  FieldValidation value;

  value.status = FieldStatus::VALID;
  value.message.swap(message);

  return value;
}

FieldValidation FieldValidation::invalid(std::string message) {
  FieldValidation value;

  value.status = FieldStatus::INVALID;
  value.message.swap(message);

  return value;
}

FieldValidation FieldValidation::pending(std::string message) {
  FieldValidation value;

  value.status = FieldStatus::PENDING;
  value.message.swap(message);

  return value;
}

FormField::FormField(FormFieldAppearance options)
    : Column(options.layout, options.policy), record_(std::make_shared<detail::FormFieldRecord>()) {
}

FormField::~FormField() {
  record_->active = false;
}

FieldValidation FormField::validation() const {
  return *record_->presentation;
}

Node<Widget> FormField::editor() const {
  return record_->editor;
}

namespace {
std::expected<std::string, TreeError> presentationText(const FieldValidation &value) {
  switch (value.status) {
  case FieldStatus::UNCHECKED:
    return value.message;
  case FieldStatus::VALID:
    return value.message.empty() ? std::string("Valid") : "Valid: " + value.message;
  case FieldStatus::INVALID:
    return value.message.empty() ? std::string("Error: Invalid value") : "Error: " + value.message;
  case FieldStatus::PENDING:
    return value.message.empty() ? std::string("Checking...") : "Checking: " + value.message;
  }

  return std::unexpected(TreeError::INVALID_ARGUMENT);
}
} // namespace

std::expected<void, TreeError> setFormFieldValidation(WidgetTree &tree, Node<FormField> node, FieldValidation value) {
  std::shared_ptr<detail::FormFieldRecord> record;
  auto admitted = tree.edit(node, [&](FormField &field) {
    record = field.record_;
  });

  if (!admitted) {
    return admitted;
  }

  try {
    auto bytes = presentationText(value);

    if (!bytes) {
      return std::unexpected(bytes.error());
    }

    auto text = GraphemeText::from(*bytes, record->widthPolicy);

    if (!text) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto next = std::make_shared<const FieldValidation>(std::move(value));
    auto updated = setControlText(tree, record->message, std::move(*text));

    if (!updated) {
      return updated;
    }

    auto shown = tree.setVisible(record->message, !bytes->empty());

    if (!shown) {
      return shown;
    }

    record->presentation = std::move(next);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
}
} // namespace portals

namespace portals::ui {
namespace {
ManagedError fieldError(Node<Widget> node, TreeError cause) {
  return {ManagedPhase::BIND, node.id(), cause, {}, {}};
}

FieldValidation evaluate(const std::shared_ptr<portals::detail::FormFieldRecord> &record, std::string_view candidate) {
  if (record->required && candidate.empty()) {
    return FieldValidation::invalid(record->requiredMessage);
  }

  return (*record->rule)(candidate);
}

std::expected<void, TreeError> publish(const std::shared_ptr<portals::detail::FormFieldRecord> &record) {
  if (!record->pending || !record->active || record->lifetime.expired()) {
    return {};
  }

  auto value = std::exchange(record->pending, {});

  return setFormFieldValidation(*record->tree, record->field, *value);
}
} // namespace

std::expected<std::shared_ptr<portals::detail::FormFieldRecord>, ManagedError>
detail::Access::fieldRecord(ManagedScene &scene, Node<Widget> node) {
  std::shared_ptr<portals::detail::FormFieldRecord> record;
  auto read = scene.app_.tree().read(node, [&](const Widget &widget) {
    if (const auto *field = dynamic_cast<const FormField *>(&widget)) {
      record = field->record_;
    }
  });

  if (!read || !record) {
    return std::unexpected(fieldError(node, read ? TreeError::INVALID_ARGUMENT : read.error()));
  }

  return record;
}

std::expected<FieldValidation, ManagedError> detail::Access::fieldValidation(ManagedScene &scene, Node<Widget> node) {
  auto record = fieldRecord(scene, node);

  if (!record) {
    return std::unexpected(record.error());
  }

  try {
    return *(*record)->presentation;
  } catch (const std::bad_alloc &) {
    return std::unexpected(fieldError(node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> detail::Access::displayField(ManagedScene &scene, Node<Widget> node,
                                                               FieldValidation value) {
  auto record = fieldRecord(scene, node);

  if (!record) {
    return std::unexpected(record.error());
  }

  auto updated = setFormFieldValidation(scene.app_.tree(), (*record)->field, std::move(value));

  return updated ? std::expected<void, ManagedError>{} : std::unexpected(fieldError(node, updated.error()));
}

std::expected<bool, ManagedError> detail::Access::validateField(ManagedScene &scene, Node<Widget> node) {
  auto found = fieldRecord(scene, node);

  if (!found) {
    return std::unexpected(found.error());
  }

  auto record = *found;

  auto admitted = scene.app_.tree().edit(record->field, [](FormField &) {
  });

  if (!admitted) {
    return std::unexpected(fieldError(node, admitted.error()));
  }
  if (!record->rule) {
    return std::unexpected(fieldError(node, TreeError::INVALID_ARGUMENT));
  }

  if (record->validating) {
    return std::unexpected(fieldError(node, TreeError::WOULD_BLOCK));
  }

  if (*scene.textMutating_) {
    return std::unexpected(fieldError(node, TreeError::WOULD_BLOCK));
  }

  struct TextGuard {
    std::shared_ptr<bool> flag;

    explicit TextGuard(std::shared_ptr<bool> value) : flag(std::move(value)) {
      *flag = true;
    }

    ~TextGuard() {
      *flag = false;
    }
  } textGuard(scene.textMutating_);

  struct ValidationGuard {
    bool &value;

    explicit ValidationGuard(bool &flag) : value(flag) {
      value = true;
    }

    ~ValidationGuard() {
      value = false;
    }
  } guard(record->validating);

  try {
    std::string candidate;
    auto copied = scene.app_.tree().read(record->editor, [&](const Widget &widget) {
      if (const auto *editor = dynamic_cast<const LineEdit *>(&widget)) {
        candidate = editor->text().bytes();
      }
    });

    if (!copied) {
      return std::unexpected(fieldError(node, copied.error()));
    }

    auto value = evaluate(record, candidate);

    if (value.status != FieldStatus::VALID && value.status != FieldStatus::INVALID) {
      return std::unexpected(fieldError(node, TreeError::INVALID_ARGUMENT));
    }

    if (record->lifetime.expired()) {
      return std::unexpected(fieldError(node, TreeError::CLOSED));
    }
    const bool valid = value.status == FieldStatus::VALID;
    auto updated = setFormFieldValidation(scene.app_.tree(), record->field, std::move(value));

    return updated ? std::expected<bool, ManagedError>(valid) : std::unexpected(fieldError(node, updated.error()));
  } catch (const std::bad_alloc &) {
    return std::unexpected(fieldError(node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(fieldError(node, TreeError::CALLBACK_FAILED));
  }
}

Description<FormField> detail::Access::formFieldComposition(
  std::string caption,
  portals::detail::MoveOnlyFunction<std::expected<Description<Widget>, ManagedError>(const ControlSettings &)> editor,
  FormFieldOptions options, std::shared_ptr<RecipeLayout> layout,
  std::shared_ptr<std::weak_ptr<portals::detail::FormFieldRecord>> bridge) {
  ControlSettings arguments;

  arguments.spinnerLabel = TextValue(caption);

  auto recipe = cold<FormField>([caption = std::move(caption), editor = std::move(editor), options = std::move(options),
                                 bridge = std::move(bridge)](
                                  ManagedScene &scene, Node<Widget> parent,
                                  MountContext &context) mutable -> std::expected<Node<FormField>, ManagedError> {
    const ControlSettings settings = context.controlSettings ? *context.controlSettings : ControlSettings{};
    const auto resolve = [&](const std::optional<TextValue> &source,
                             std::string &destination) -> std::expected<void, ManagedError> {
      if (!source) {
        return {};
      }

      auto text = source->resolve(scene.widthPolicy());

      if (!text) {
        auto failure = fieldError(parent, TreeError::INVALID_ARGUMENT);

        if (const auto *error = std::get_if<TextError>(&text.error())) {
          failure.text = *error;
        } else {
          failure.resource = std::get<TextResourceError>(text.error());
        }

        return std::unexpected(failure);
      }

      destination = text->bytes();

      return {};
    };

    for (auto result : {resolve(settings.spinnerLabel, caption), resolve(settings.fieldHelp, options.help),
                        resolve(settings.fieldRequiredMessage, options.requiredMessage)}) {
      if (!result) {
        return std::unexpected(result.error());
      }
    }

    options.required = settings.fieldRequired.value_or(options.required);
    options.appearance.layout = controlLinearOptions(settings, options.appearance.layout);

    auto field = scene.emplace<FormField>(parent, options.appearance);

    if (!field) {
      return field;
    }

    // Nested mounts may fail before this cold factory returns its root to Description.
    if (context.root == Node<Widget>{}) {
      context.root = *field;
    }

    auto record = *fieldRecord(scene, *field);

    record->tree = &scene.app_.tree();
    record->lifetime = lifetime(scene);
    record->field = *field;
    record->widthPolicy = scene.widthPolicy();
    record->required = options.required;
    record->requiredMessage = options.requiredMessage;

    if (bridge) {
      *bridge = record;
    }

    auto scope = scene.focusScope(*field);

    if (!scope) {
      return std::unexpected(scope.error());
    }

    if (options.required) {
      caption += " (required)";
    }

    auto labelNode =
      mount(label(caption, options.appearance.labelStyle).classes("form-field-label"), scene, *field, context);

    if (!labelNode) {
      return std::unexpected(labelNode.error());
    }

    auto child = editor(settings);

    if (!child) {
      return std::unexpected(child.error());
    }

    auto editorNode = mount(std::move(*child).focusDefault(), scene, *field, context);

    if (!editorNode) {
      return std::unexpected(editorNode.error());
    }

    record->editor = *editorNode;

    auto help =
      mount(label(options.help, options.appearance.helpStyle).classes("form-field-help").visible(!options.help.empty()),
            scene, *field, context);

    if (!help) {
      return std::unexpected(help.error());
    }

    auto message = mount(label("", options.appearance.messageStyle).classes("form-field-message").visible(false), scene,
                         *field, context);

    if (!message) {
      return std::unexpected(message.error());
    }

    record->message = *message;

    auto labelEntry = scene.bindingEntry(*labelNode);

    if (!labelEntry) {
      return std::unexpected(labelEntry.error());
    }

    auto linked = scene.app_.router().bind(
      *labelNode,
      [&scene, field = *field, alive = lifetime(scene)](Label &, const RoutedEvent &event) {
        const auto *mouse = std::get_if<MouseInputEvent>(&event);

        if (!mouse || mouse->button != MouseButton::LEFT || mouse->action != MouseAction::PRESS) {
          return EventReply::IGNORED;
        }

        if (alive.expired()) {
          return EventReply::HANDLED;
        }

        auto focused = scene.focus(field);

        if (!focused) {
          scene.remember(focused.error());
        }

        return EventReply::HANDLED;
      },
      false);

    if (!linked) {
      return std::unexpected(fieldError(*labelNode, linked.error()));
    }

    (*labelEntry)->binding.emplace<ActionBinding>(std::move(*linked));

    return field;
  });

  return controlArguments(configuredControl(withLayout(std::move(recipe), std::move(layout)), "FormField"),
                          std::move(arguments));
}

Description<FormField> detail::Access::formField(std::string caption, Description<Widget> editor,
                                                 FormFieldOptions options) {
  auto layout = captureLayouts(editor);
  auto factory = [editor = std::move(editor)](
                   const ControlSettings &settings) mutable -> std::expected<Description<Widget>, ManagedError> {
    const auto requested = controlSettingsPresenceSet(settings);
    const auto editorOnly = controlSettingsPresenceSet(ControlSettings{.text = TextValue(""),
                                                                       .editMode = EditMode::IMMEDIATE,
                                                                       .readOnly = false,
                                                                       .programmaticChange = ProgrammaticChange::RECORD,
                                                                       .displayMode = LineEditDisplayMode::NORMAL,
                                                                       .placeholder = TextValue(""),
                                                                       .history = {false, 0, 0, false, false},
                                                                       .limits = {0, 0, 0, 0}});

    if ((requested & editorOnly).any()) {
      return std::unexpected(fieldError({}, TreeError::INVALID_ARGUMENT));
    }

    return std::move(editor);
  };

  return formFieldComposition(std::move(caption), std::move(factory), std::move(options), std::move(layout));
}

Description<FormField> detail::Access::formField(std::string caption, std::string initial, FormFieldConfig config) {
  auto bridge = std::make_shared<std::weak_ptr<portals::detail::FormFieldRecord>>();
  auto rule = std::make_shared<std::function<FieldValidation(std::string_view)>>(
    [validate = std::move(config.validate),
     legacy = std::move(config.editor.validate)](std::string_view candidate) -> FieldValidation {
      auto value = validate ? validate(candidate) : FieldValidation::valid();

      if (value.status == FieldStatus::VALID && legacy && !legacy(candidate)) {
        return FieldValidation::invalid();
      }

      return value;
    });

  config.editor.validate = [bridge, rule](std::string_view candidate) {
    auto record = bridge->lock();

    if (!record || !record->active) {
      return false;
    }

    if (record->validating) {
      throw std::logic_error("Reentrant field validation");
    }

    struct Guard {
      bool &flag;
      explicit Guard(bool &value) : flag(value) {
        flag = true;
      }
      ~Guard() {
        flag = false;
      }
    } guard(record->validating);

    if (!record->rule) {
      record->rule = rule;
    }

    auto value = evaluate(record, candidate);

    if (value.status != FieldStatus::VALID && value.status != FieldStatus::INVALID) {
      throw std::invalid_argument("A synchronous field validator must return VALID or INVALID");
    }

    const bool accepted = value.status == FieldStatus::VALID;

    record->pending = std::make_shared<const FieldValidation>(std::move(value));

    return accepted;
  };

  config.editor.result = [bridge, result = std::move(config.editor.result)](LineEditResult receipt) {
    auto record = bridge->lock();
    auto updated = record ? publish(record) : std::expected<void, TreeError>{};
    auto delivered = result ? result(std::move(receipt)) : std::expected<void, TreeError>{};

    return !updated ? updated : delivered;
  };

  FormFieldOptions options{.help = std::move(config.help),
                           .required = config.required,
                           .requiredMessage = std::move(config.requiredMessage),
                           .appearance = std::move(config.appearance)};
  auto factory = [initial = std::move(initial), config = std::move(config.editor)](
                   const ControlSettings &settings) mutable -> std::expected<Description<Widget>, ManagedError> {
    ControlSettings patch{.text = settings.text,
                          .editMode = settings.editMode,
                          .readOnly = settings.readOnly,
                          .programmaticChange = settings.programmaticChange,
                          .displayMode = settings.displayMode,
                          .placeholder = settings.placeholder,
                          .history = settings.history,
                          .limits = settings.limits};

    return erase(lineEdit(initial, std::move(config)).settings(std::move(patch)));
  };
  auto recipe = formFieldComposition(std::move(caption), std::move(factory), std::move(options), {}, bridge);

  // Parent setup runs after the composition has mounted its real children, before Ref publication.
  return std::move(recipe).setup([bridge, rule = std::move(rule)](ManagedScene &scene, Node<FormField> field) mutable {
    auto record = fieldRecord(scene, field);

    if (!record) {
      return std::expected<void, ManagedError>(std::unexpected(record.error()));
    }

    (*record)->rule = std::move(rule);
    *bridge = *record;

    return std::expected<void, ManagedError>{};
  });
}

Description<FormField> formField(std::string_view caption, std::string_view initial, FormFieldConfig config) {
  return detail::Access::formField(std::string(caption), std::string(initial), std::move(config));
}

Description<FormField> detail::customFormField(std::string caption, Description<Widget> editor,
                                               FormFieldOptions options) {
  return Access::formField(std::move(caption), std::move(editor), std::move(options));
}

std::expected<FieldValidation, ManagedError> detail::fieldValidation(const std::shared_ptr<RefTarget> &target) {
  auto scene = eventScene(target);

  return scene ? Access::fieldValidation(**scene, target->node) : std::unexpected(scene.error());
}

Ref<LineEdit> detail::fieldEditor(const std::shared_ptr<RefTarget> &target) {
  Ref<LineEdit> reference;
  auto scene = eventScene(target);
  auto record =
    scene
      ? Access::fieldRecord(**scene, target->node)
      : std::expected<std::shared_ptr<portals::detail::FormFieldRecord>, ManagedError>(std::unexpected(scene.error()));
  auto published = record ? NamedAccess::publish(reference, **scene, (*record)->editor)
                          : std::expected<void, ManagedError>(std::unexpected(record.error()));

  if (!published) {
    NamedAccess::reject(reference, published.error());
  }

  return reference;
}

std::expected<bool, ManagedError> detail::validateField(const std::shared_ptr<RefTarget> &target) {
  auto scene = eventScene(target);

  return scene ? Access::validateField(**scene, target->node) : std::unexpected(scene.error());
}

std::expected<void, ManagedError> detail::displayField(const std::shared_ptr<RefTarget> &target,
                                                       FieldValidation value) {
  auto scene = eventScene(target);

  return scene ? Access::displayField(**scene, target->node, std::move(value)) : std::unexpected(scene.error());
}
} // namespace portals::ui
