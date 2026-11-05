// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_log_view.hpp>
#include <portals/ui_ref.hpp>

static_assert(requires(portals::ui::Ref<portals::LogView> view, portals::ui::LogSource source) {
  { view.model(source) } -> std::same_as<bool>;
  { view.setModel(source) } -> std::same_as<std::expected<void, portals::ManagedError>>;
});
