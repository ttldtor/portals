// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>

namespace {
using namespace portals;

template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) {
    std::exit(EXIT_FAILURE);
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct ClipboardProbe : Clipboard {
  int writes{};

  std::expected<std::string, TreeError> read(std::size_t) override {
    return std::string("paste");
  }

  std::expected<void, TreeError> write(std::string_view) override {
    ++writes;
    return {};
  }
};

struct PaintProbe : PaintContext {
  std::string text;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *value = std::get_if<TextCommand>(&command)) {
        text.append(value->text.bytes());
      }
    }
    return {};
  }
};
} // namespace

/** @brief Check installed display options, owning placeholder, typed clear and preserved plaintext source behavior. */
int main() {
  using namespace portals;
  constexpr std::string_view UNICODE = "e\xcc\x81\xe7\x95\x8c";

  for (auto mode : {LineEditDisplayMode::NORMAL, LineEditDisplayMode::PASSWORD, LineEditDisplayMode::NO_ECHO}) {
    auto app = checked(Application::create());
    auto owner = checked(ManagedScene::create(*app));
    auto clipboard = std::make_shared<ClipboardProbe>();
    ui::Ref<LineEdit> editor;
    ui::Ref<Button> clear;
    checked(ui::mount(*owner, ui::vbox(ui::lineEdit(editor, "",
                                                    {.options = {.displayMode = mode, .placeholder = "Prompt"},
                                                     .clipboard = clipboard}),
                                       ui::button(clear, "Clear", [editor] {
                                         return editor.clear();
                                       }))));
    checked(app->router().setFocus(editor.node()));
    checked(editor.text().set(UNICODE).result());
    checked(owner->prepare(*app, {20, 4}));
    if (checked(editor.committedText().snapshot()).bytes() != UNICODE) {
      return 2;
    }
    PaintProbe paint;
    checked(app->tree().frame([&] {
      const auto viewport = checked(CellRect::from({}, {20, 4}));
      checked(layoutTree(app->tree(), checked(app->tree().root()), viewport, viewport));
      checked(app->tree().paint(editor.node(), paint));
    }));
    if ((mode == LineEditDisplayMode::PASSWORD &&
         (paint.text.empty() || paint.text.find_first_not_of('*') != std::string::npos)) ||
        (mode == LineEditDisplayMode::NO_ECHO && !paint.text.empty())) {
      return 3;
    }
    checked(app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'a', {.control = true}}));
    checked(app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'c', {.control = true}}));
    if (clipboard->writes != (mode == LineEditDisplayMode::NORMAL ? 1 : 0)) {
      return 4;
    }
    checked(app->router().invoke(clear.node(), {Action::ACTIVATE}));
    if (!checked(editor.text().get()).empty()) {
      return 5;
    }
    checked(editor.undo().result());
    if (checked(editor.text().get()) != UNICODE) {
      return 6;
    }
    checked(owner->close());
  }
  return 0;
}
