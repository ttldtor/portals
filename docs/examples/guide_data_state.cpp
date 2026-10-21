// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Explore explicit presentation states while keeping ordinary content live.
 * @return Zero on orderly exit, one after terminal restoration and a typed diagnostic.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<DataStateView> presentation;
    return ui::scene(ui::vbox(
             ui::dataStateView(DataPresentation{DataPhase::READY},
                               ui::label("The content stays available."))
               .reference(presentation),
             ui::button("Loading", [presentation] {
               return presentation.display({DataPhase::LOADING});
             }),
             ui::button("Ready", [presentation] {
               return presentation.display({DataPhase::READY});
             }),
             ui::button("Empty", [presentation] {
               return presentation.display({DataPhase::EMPTY});
             }),
             ui::button("Failed", [presentation] {
               return presentation.display({DataPhase::FAILED});
             }),
             ui::button("Offline", [presentation] {
               return presentation.display({DataPhase::OFFLINE});
             }),
             ui::button("Refreshing", [presentation] {
               return presentation.display({DataPhase::REFRESHING});
             }),
             ui::button("Exit", [] { ui::quit(); })))
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
