// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>
#include <string>

namespace {
template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) {
    std::exit(EXIT_FAILURE);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}
} // namespace

/** @brief Verify shared captions and owning sources using only the installed SDK. */
int main() {
  using namespace portals;

  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app));
  auto model = checked(owner->state(std::string("first")));
  ui::Ref<LineEdit> field;
  ui::Ref<Label> caption;
  ui::Ref<Button> button;
  auto committed = field.committedText();
  int calls{};

  if (committed.snapshot()) {
    return 1;
  }

  checked(ui::mount(*owner, ui::vbox(ui::lineEdit(field, model), ui::label(caption, model),
                                     ui::button(button, model,
                                                [&] {
                                                  ++calls;
                                                }),
                                     ui::label(text("Count: {}", 42)))));
  checked(owner->prepare(*app, {40, 8}));

  if (checked(caption.captionText().snapshot()).bytes() != "first" ||
      checked(button.captionText().snapshot()).bytes() != "first" || checked(committed.snapshot()).bytes() != "first") {
    return 2;
  }

  checked((field.text() = "edited").result());
  checked(owner->prepare(*app, {40, 8}));

  if (checked(caption.captionText().snapshot()).bytes() != "edited" ||
      checked(committed.snapshot()).bytes() != "edited") {
    return 3;
  }

  checked((caption.text() = "manual").result());
  checked(model.set(std::string("edited")));
  checked(owner->prepare(*app, {40, 8}));

  if (checked(caption.captionText().snapshot()).bytes() != "manual") {
    return 4;
  }

  checked(model.set(std::string("external")));
  checked(owner->prepare(*app, {40, 8}));
  auto retained = checked(committed.snapshot());

  if (checked(caption.captionText().snapshot()).bytes() != "external" ||
      checked(button.captionText().snapshot()).bytes() != "external" ||
      checked(field.draftText().snapshot()).bytes() != "external" || owner->bindText(caption.node(), model)) {
    return 5;
  }

  checked(app->router().invoke(button.node(), {Action::ACTIVATE}));

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> preview, accepted;
  ui::LineEditConfig config;
  config.options.editMode = EditMode::EXPLICIT;
  config.validate = [](std::string_view value) {
    return value != "invalid";
  };
  checked(ui::mount(*owner, ui::vbox(ui::label(preview, draft.text()), ui::lineEdit(draft, "base", config),
                                     ui::label(accepted, draft.committedText()))));
  int edited{}, finished{};
  bool rejected{};
  checked(owner->onTextEdited(draft.node(), [&] {
    ++edited;
  }));
  checked(owner->onEditingFinished(draft.node(), [&](const EditingFinishedEvent &event) {
    ++finished;
    rejected = event.outcome == EditingFinishedOutcome::VALIDATION_REJECTED;
  }));
  checked((draft.text() = "invalid").result());
  checked(app->router().setFocus(draft.node()));
  checked(app->router().dispatch(KeyEvent{InputKey::ENTER}));
  checked(owner->prepare(*app, {40, 12}));
  if (edited != 0 || finished != 1 || !rejected || checked(preview.text().get()) != "invalid" ||
      checked(accepted.text().get()) != "base") {
    return 7;
  }
  checked(app->router().dispatch(TextInputEvent{U'!'}));
  checked(app->router().dispatch(KeyEvent{InputKey::ENTER}));
  checked(owner->prepare(*app, {40, 12}));
  if (edited != 1 || finished != 2 || rejected || checked(accepted.text().get()) != "invalid!") {
    return 8;
  }
  checked(owner->close());

  return calls == 1 && !committed.snapshot() && retained.bytes() == "external" ? 0 : 6;
}
