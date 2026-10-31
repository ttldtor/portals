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

  auto scene = ui::scene(R"scene(
    VBox {
      Label#title { text = i18n("profile.title") }
      Label#nameLabel { text = i18n("profile.name") }
      LineEdit#name { text = "Ada", flexX = 1, minWidth = 16, maxWidth = 40 }
      Button#save { text = i18n("profile.save") }
      Label#savedLabel { text = i18n("profile.saved") }
      Label#savedValue { "" }
      Label { "F2: EN/RU | F10: quit" }
    }
  )scene",
                         {.resolver = resolver});
  const auto title = scene.get<Label>("title");
  const auto nameLabel = scene.get<Label>("nameLabel");
  const auto savedLabel = scene.get<Label>("savedLabel");
  const auto savedValue = scene.get<Label>("savedValue");
  const auto name = scene.get<LineEdit>("name");
  const auto save = scene.get<Button>("save");

  // clang-format off
  return scene
    | ui::theme(nordTheme())
    | ui::style(R"pss(
        Button { width: fit-content; }
        LineEdit { background: #3b4252; }
      )pss")
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
