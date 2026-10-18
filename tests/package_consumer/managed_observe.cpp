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

/** @brief Exercise declarative managed observation through the installed package. */
int main() {
  using namespace portals;

  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app));
  auto model = checked(owner->state(std::string("Portals")));
  ui::Ref<LineEdit> field;
  ui::Ref<Label> summary;
  int calls{};

  checked(ui::mount(*owner, ui::vbox(ui::lineEdit(field, model),
                                     ui::label(summary, "").observe(model, [summary, &calls](const std::string &value) {
                                       ++calls;
                                       summary.text() = text("Accepted: {}", value);
                                     }))));

  if (calls != 0) {
    return 1;
  }

  checked(owner->prepare(*app, {40, 8}));

  if (calls != 1 || checked(summary.captionText().snapshot()).bytes() != "Accepted: Portals") {
    return 2;
  }

  checked((field.text() = "edited").result());
  checked(owner->prepare(*app, {40, 8}));

  if (calls != 2 || checked(summary.captionText().snapshot()).bytes() != "Accepted: edited") {
    return 3;
  }

  checked(model.set(std::string("edited")));
  checked(owner->prepare(*app, {40, 8}));

  if (calls != 2) {
    return 4;
  }

  checked(owner->remove(summary.node()));
  checked(model.set(std::string("after removal")));
  checked(owner->prepare(*app, {40, 8}));
  checked(owner->close());

  return calls == 2 ? 0 : 5;
}
