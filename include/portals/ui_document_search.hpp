// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_search.hpp>
#include <portals/ui_document_view.hpp>
#include <portals/ui_find_bar.hpp>

namespace portals {
class LogView;
}

namespace portals::ui {
/** @param target Weak viewer observation; its recipe may follow this bar in the scene.
 * @param initialQuery Initial UTF-8 literal, empty by default.
 * @param config Ordinary query editor, buttons and selected callback; search/searchFailed remain model-only.
 * @param search Bounded worker/query/snippet storage; no document-size or total-hit cap.
 * @return Scene-owned asynchronous literal search. Next/Previous wrap the complete source.
 * Navigation queues work and returns an empty immediate handle; selected receives its later result.
 * Only the selected match is retained; storedCount is not a global hit count.
 * Source/encoding replacement clears results while retaining the query and reading position;
 * edit the query or choose Next/Previous to search the replacement. Manual navigation during
 * a pending search prevents automatic reveal. Source access and decoding remain on workers.
 */
Description<FindBar> findBar(Ref<DocumentView> target, std::string_view initialQuery = {}, FindBarConfig config = {},
                             DocumentSearchOptions search = {});
/** @param reference Weak bar observation. @param target Weak viewer observation.
 * @param initialQuery Initial literal. @param config Standard query composition settings.
 * @param search Bounded streaming search settings. @return Ref-first streaming search recipe.
 */
Description<FindBar> findBar(Ref<FindBar> reference, Ref<DocumentView> target, std::string_view initialQuery = {},
                             FindBarConfig config = {}, DocumentSearchOptions search = {});
/** @param target Weak file-backed LogView observation; other model families report UNSUPPORTED.
 * @param initialQuery Initial literal. @param config Ordinary query composition settings.
 * @param search Bounded streaming search settings. @return File-search recipe over the same source budget.
 */
Description<FindBar> findBar(Ref<LogView> target, std::string_view initialQuery = {}, FindBarConfig config = {},
                             DocumentSearchOptions search = {});
/** @param reference Weak bar observation. @param target Weak file-backed log observation.
 * @param initialQuery Initial literal. @param config Standard query composition settings.
 * @param search Bounded streaming search settings. @return Ref-first file-search recipe.
 */
Description<FindBar> findBar(Ref<FindBar> reference, Ref<LogView> target, std::string_view initialQuery = {},
                             FindBarConfig config = {}, DocumentSearchOptions search = {});
} // namespace portals::ui
