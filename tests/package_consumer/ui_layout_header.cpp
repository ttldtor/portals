// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/ui_layout.hpp>

namespace {
[[maybe_unused]] auto spaces() {
  return portals::ui::hbox(portals::ui::spacer(), portals::ui::spacing(0), portals::ui::spacer(2));
}
} // namespace
