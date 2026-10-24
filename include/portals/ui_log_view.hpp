// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/log_view.hpp>
#include <portals/ui_description.hpp>

namespace portals::ui {
template <class T> class Description;
template <class T> class Ref;

/** @param reference Weak observation published only after complete mount success.
 * @param initial Complete UTF-8 batch copied into the recipe now.
 * @param config Positive resource/navigation settings; omitted scalar properties inherit existing defaults.
 * @param style Manual appearances beneath theme rules.
 * @return Owning native bounded-log recipe; invalid text/settings retain exact typed mount errors.
 */
Description<LogView> logView(Ref<LogView> reference, std::string_view initial = {}, LogViewConfig config = {},
                             ControlStyle style = {});
/** @param initial Complete UTF-8 batch copied now. @param config Bounded owning settings.
 * @param style Manual appearances. @return Native log recipe without an observing Ref.
 */
Description<LogView> logView(std::string_view initial = {}, LogViewConfig config = {}, ControlStyle style = {});
} // namespace portals::ui
