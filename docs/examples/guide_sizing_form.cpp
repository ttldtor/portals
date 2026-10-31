// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

#include <expected>
#include <string>
#include <string_view>

/** @brief Edit and save a profile while resizing and switching captions.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  struct Entry {
    std::string_view key, english, russian;
  };

  constexpr Entry MESSAGES[] = {
    {"profile.title", "Profile", "Профиль"},
    {"profile.name", "Name", "Имя"},
    {"profile.saved", "Saved name", "Сохранённое имя"},
    {"profile.save", "Save", "Сохранить профиль"},
  };
  bool russian = false;
  ui::TextResolver resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    for (const auto &entry : MESSAGES) {
      if (entry.key == key) {
        return std::string(russian ? entry.russian : entry.english);
      }
    }

    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  };

  ui::Ref<Label> title, nameLabel, savedLabel, savedValue;
  ui::Ref<LineEdit> name;
  ui::Ref<Button> save;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::label(i18n("profile.title"), {}, {}, resolver).reference(title),
    ui::label(i18n("profile.name"), {}, {}, resolver).reference(nameLabel),
    ui::lineEdit(name, "Ada").flexX().minWidth(16).maxWidth(40),
    ui::button(i18n("profile.save"), {}, {}, resolver).reference(save).fitWidth(),
    ui::label(i18n("profile.saved"), {}, {}, resolver).reference(savedLabel),
    ui::label(savedValue, ""),
    ui::label("F2: EN/RU | F10: quit")));

  return scene
    | ui::theme(nordTheme())
    | ui::style("LineEdit { background: #3b4252; }")
    | ui::onClick(save, [name, savedValue] {
        savedValue.text() = name.text();
      })
    | ui::onKey("F2", [title, nameLabel, savedLabel, save, &russian, &resolver] {
        russian = !russian;
        title.text(resolver) = i18n("profile.title");
        nameLabel.text(resolver) = i18n("profile.name");
        savedLabel.text(resolver) = i18n("profile.saved");
        save.text(resolver) = i18n("profile.save");
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
