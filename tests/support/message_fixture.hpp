// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "managed_text_fixture.hpp"

namespace {
void command(ui::DialogCommandResult result, std::source_location origin = std::source_location::current()) {
  take(result.result(), origin);
}

Node<Widget> findButton(Fixture &f, Node<Widget> root, std::string_view caption) {
  Node<Widget> found;
  take(f.app->tree().read(root, [&](const Widget &widget) {
    if (auto *button = dynamic_cast<const Button *>(&widget); button && button->text().bytes() == caption) {
      found = root;
    }
  }));
  if (found != Node<Widget>{}) {
    return found;
  }
  for (auto child : take(f.app->tree().inspect(root)).children) {
    found = findButton(f, child, caption);
    if (found != Node<Widget>{}) {
      return found;
    }
  }
  return {};
}

void choose(Fixture &f, Node<Widget> root, std::string_view caption) {
  auto button = findButton(f, root, caption);
  check(button != Node<Widget>{}, "choice exists");
  take(f.app->router().invoke(button, {Action::ACTIVATE}));
}

void flushMessages(Fixture &f) {
  take(f.scene->afterDispatch(*f.app));
  f.dispatch();
  take(f.scene->afterDispatch(*f.app));
}

std::shared_ptr<const MessageResult> answer(const ui::MessageBox &box, MessageChoice expected) {
  auto value = take(box.lastResult());
  check(value && value->has_value() && **value == expected, "exact selected choice");
  return value;
}
} // namespace
