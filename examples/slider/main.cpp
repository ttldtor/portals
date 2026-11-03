// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <format>

/** @brief Adjust two numeric settings through the ordinary retained scene.
 * @return Native application status after terminal restoration and scene cleanup.
 */
int main() {
  using namespace portals;
  ui::Ref<Slider> volume;
  ui::Ref<Slider> balance;
  ui::Ref<Label> status;

  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::vbox(
             ui::label("Audio settings"), ui::label("Drag a handle to preview each value."),
             ui::hbox(ui::label("Volume").width(10), ui::slider(volume, owner.numericState(35, {.step = 5})).flex(1)),
             ui::hbox(ui::label("Balance").width(10), ui::slider(balance, 0).range(-1, 1).step(0.1).flex(1)),
             ui::label(status, "Ready"),
             ui::button("Reset",
                        [=] {
                          volume.value() = 35;
                          balance.value() = 0;
                          status.text() = "Defaults restored";
                        }),
             ui::label("Tab: focus | Arrows/Page/Home/End: adjust | Escape: end drag | F10: quit"))
      .settings({.gap = 1})
      .framed("rounded");
  });

  return scene
    | ui::theme(nordTheme())
    | ui::style("Slider { foreground: #88c0d0; } Slider:focused { foreground: #ebcb8b; }")
    | ui::onChange(volume, [status](double value) {
        status.text() = std::format("Volume preview: {:.0f}", value);
      })
    | ui::onChange(balance, [status](double value) {
        status.text() = std::format("Balance preview: {:+.1f}", value);
      })
    | ui::onEditingFinished(volume, [status](const SliderOperationEvent &event) {
        if (event.status == SliderOperationStatus::COMPLETED && event.lastUserUpdate) {
          status.text() = std::format("Volume applied: {:.0f}", event.lastUserUpdate->snapshot->value.value());
        }
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
