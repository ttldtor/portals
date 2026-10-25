// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "line_edit_validation.hpp"
#include <portals/ui_form_field.hpp>

namespace portals::ui::detail {
// Called once by standard field setup, after the real editor binding has mounted.
// The stage callback owns no scene and only stages presentation for the ordinary result bridge.
std::expected<void, ManagedError> attachAsyncField(ManagedScene &, Node<FormField>, Node<Widget>, LineEditBinding &,
                                                   AsyncFieldValidator, FieldValidationLimits,
                                                   std::function<void(std::shared_ptr<const FieldValidation>)> stage);
} // namespace portals::ui::detail
