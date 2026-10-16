// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "form.hpp"

/** @brief Run the resource form with the native terminal.
 * @return Existing runner status after cleanup and typed diagnostic reporting.
 */
int main() {
  return scene_resource_example::form()
    | portals::ui::runApp();
}
