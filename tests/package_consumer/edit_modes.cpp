// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);

  if (!scene) {
    return 2;
  }

  auto immediateModel = (*scene)->state(std::string("initial"));
  auto completedModel = (*scene)->state(std::string("initial"));
  auto explicitModel = (*scene)->state(std::string("initial"));

  if (!immediateModel || !completedModel || !explicitModel) {
    return 3;
  }

  ui::Ref<LineEdit> immediate;
  ui::Ref<LineEdit> completed;
  ui::Ref<LineEdit> explicitField;
  LineEditTrigger immediateTrigger = LineEditTrigger::INPUT;
  LineEditTrigger completedTrigger = LineEditTrigger::INPUT;
  LineEditTrigger explicitTrigger = LineEditTrigger::BLUR;
  auto mounted = ui::mount(
    **scene,
    ui::column(ui::lineEdit(*immediateModel, {.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
                 immediateTrigger = receipt.trigger;

                 return {};
               }})
                 .reference(immediate),
               ui::lineEdit(*completedModel, {.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
                                                completedTrigger = receipt.trigger;

                                                return {};
                                              },
                                              .options = {.editMode = EditMode::ON_COMPLETION}})
                 .reference(completed),
               ui::lineEdit(*explicitModel, {.result = [&](LineEditResult receipt) -> std::expected<void, TreeError> {
                                               explicitTrigger = receipt.trigger;

                                               return {};
                                             },
                                             .options = {.editMode = EditMode::EXPLICIT}})
                 .reference(explicitField)));

  if (!mounted || !(immediate.text() = "immediate").result()) {
    return 4;
  }

  auto value = immediateModel->snapshot();

  if (!value || (*value)->value != "immediate" || immediateTrigger != LineEditTrigger::FLUENT ||
      !(completed.text() = "completion").result() || !(*app)->router().setFocus(completed.node()) ||
      !(*app)->router().setFocus(explicitField.node())) {
    return 5;
  }

  value = completedModel->snapshot();

  if (!value || (*value)->value != "completion" || completedTrigger != LineEditTrigger::BLUR ||
      !(explicitField.text() = "draft").result() || !(*app)->router().setFocus(immediate.node())) {
    return 6;
  }

  value = explicitModel->snapshot();

  if (!value || (*value)->value != "initial" || !(*app)->router().invoke(explicitField.node(), {Action::ACTIVATE})) {
    return 7;
  }

  value = explicitModel->snapshot();

  return value && (*value)->value == "draft" && explicitTrigger == LineEditTrigger::INPUT ? 0 : 8;
}
