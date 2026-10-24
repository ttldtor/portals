// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/find_bar.hpp>
#include <portals/ui_controls.hpp>
#include <portals/ui_layout.hpp>

namespace portals::ui {
/** @brief Owning bounded literal search configuration; the normal editor owns query admission/history. */
struct FindBarConfig {
  /** @brief Keep aggregate transfers fallible on checked standard-library implementations. */
  ~FindBarConfig() = default;

  FindLimits limits{}; ///< Source/range caps, checked against FindLimits hard ceilings.
  LineEditConfig editor{
    .options = {.limits = {1024, 1024, 1024, 2048}}}; ///< Query options; automatic width shares toolbar space.
  TextValue previousLabel{"Previous"};                ///< Owning previous-match button caption.
  TextValue nextLabel{"Next"};                        ///< Owning next-match button caption.
  TextValue clearLabel{"Clear"};                      ///< Owning clear-query button caption.
  FindBarAppearance appearance{}; ///< Layout/manual style defaults; callbacks never run during styling.
  std::function<void(FindMatch)>
    selected{}; ///< Owning callback after publication; no initial delivery or rollback on failure.
};

/** @param document Source copied only after its configured byte cap is checked; invalid UTF-8 fails mount.
 * @param initialQuery Initial editor draft, bounded by editor TextLimits; empty is an ordinary state.
 * @param config Owning options/services/callback. Exact case-sensitive bytes match only at whole source graphemes;
 * overlapping matches are retained. A truncated count covers only the first stored prefix, which navigation wraps.
 * Deferred drafts keep previous results until their normal publication boundary. Clear explicitly confirms empty text.
 * @return Single-use retained composition; partial mount rolls back its complete subtree and publishes no refs.
 */
Description<FindBar> findBar(std::string_view document, std::string_view initialQuery = {}, FindBarConfig config = {});

/** @param reference Existing weak slot, published only after the whole composition mounts.
 * @param document Bounded owning source input. @param initialQuery Initial query, empty by default.
 * @param config Owning synchronous options. @return Ordinary FindBar recipe sharing the existing Ref family.
 */
inline Description<FindBar> findBar(Ref<FindBar> reference, std::string_view document,
                                    std::string_view initialQuery = {}, FindBarConfig config = {}) {
  return findBar(document, initialQuery, std::move(config)).reference(std::move(reference));
}
} // namespace portals::ui
