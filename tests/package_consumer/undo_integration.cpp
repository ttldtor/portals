// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <iostream>
#include <stdexcept>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("checked public operation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("installed history contract failed");
  }
}
} // namespace

int main() {
  try {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    const auto model = take(scene->state(std::string("base")));

    ui::Ref<LineEdit> field;
    ui::Ref<Button> undo;
    ui::Ref<Button> redo;

    take(ui::mount(*scene, ui::column(ui::lineEdit(model).reference(field),
                                      ui::button("Undo",
                                                 [field] {
                                                   return field.undo();
                                                 })
                                        .reference(undo),
                                      ui::control<Button>("Redo", {}, {}).reference(redo))));
    take(scene->onClick(redo.node(), [field] {
      return field.redo();
    }));
    take((field.text() = text("{}", "next")).result());
    require(field.canUndo());
    take(app->router().invoke(undo.node(), {Action::ACTIVATE}));
    require(take(model.snapshot())->value == "base");
    require(field.canRedo());
    take(app->router().invoke(redo.node(), {Action::ACTIVATE}));
    require(take(field.text().get()) == "next");
    take((field.text(ProgrammaticChange::RESET_BASELINE) = text("{}", "loaded")).result());
    require(!field.canUndo() && !field.canRedo());
    take((field.text() += "!").result());
    take(field.stopGrouping().result());
    take(field.undo().result());
    require(take(field.text().get()) == "loaded");
    require(take(field.historyAvailability()).canRedo);
    require(take(scene->diagnostics()).errors.empty());

    std::cout << "installed Undo/Redo, RECORD/RESET and compact adapters passed\n";

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
