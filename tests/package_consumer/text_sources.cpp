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
  checked(owner->close());

  return calls == 1 && !committed.snapshot() && retained.bytes() == "external" ? 0 : 6;
}
