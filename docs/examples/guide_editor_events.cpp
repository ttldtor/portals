#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> message;
    ui::Ref<Label> status;
    ui::Ref<Button> append;

    ui::LineEditConfig config;
    config.options.editMode = EditMode::EXPLICIT;
    config.validate = [](std::string_view value) { return value != "invalid"; };

    auto scene = ui::scene(ui::vbox(
      ui::label("Enter accepts; Escape cancels. The word 'invalid' is rejected."),
      ui::lineEdit(message, "Hello", config),
      ui::label("Visible draft:"),
      ui::label(message.text()),
      ui::label("Accepted value:"),
      ui::label(message.committedText()),
      ui::label(status, "Ready"),
      ui::button(append, "Append !")));

    return std::move(scene)
      | ui::onTextEdited(message, [status] {
          status.text() = "User edit";
        })
      | ui::onEditingFinished(message, [status](const EditingFinishedEvent &event) {
          if (event.outcome == EditingFinishedOutcome::VALIDATION_REJECTED) {
            status.text() = "Rejected: choose a different word; the draft is preserved.";
          } else if (event.outcome == EditingFinishedOutcome::CONFLICT) {
            status.text() = "Conflict: the model changed; Escape accepts its latest value.";
          } else if (event.outcome == EditingFinishedOutcome::FAILED) {
            status.text() = "The editing operation failed.";
          } else if (event.outcome == EditingFinishedOutcome::DEFERRED) {
            status.text() = "Draft preserved; press Enter to accept it.";
          } else {
            status.text() = "Accepted value is up to date.";
          }
        })
      | ui::onClick(append, [message] {
          message.text() += "!";
        })
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
