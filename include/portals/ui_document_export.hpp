// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_export.hpp>
#include <portals/ui_ref.hpp>

namespace portals::ui {
/** @param viewer Weak live viewer; its current source is captured before worker admission.
 * @param output Owned output path; an existing file is never overwritten.
 * @param completed Optional UI completion, including partial-byte counts on failure.
 * @param origin Admission site retained for diagnostics.
 * @return Checked asynchronous admission. Source reads and output writes run on workers.
 * Closing/removing the viewer cancels work without joining. A model replacement cannot
 * redirect the captured source. A partial file is preserved after cancellation/failure.
 */
std::expected<void, ManagedError> startExport(Ref<DocumentView> viewer, std::filesystem::path output,
                                              std::function<void(const DocumentExportResult &)> completed = {},
                                              std::source_location origin = std::source_location::current());
/** @param viewer Weak live viewer. @param output New output path.
 * @param completed Optional owning UI completion. @param origin Original command site.
 * @return Admission result, collecting a rejected command exactly once in the scene.
 * Runtime source/sink failures enter the same scene error path; see startExport for lifetime policy.
 */
bool exportFile(Ref<DocumentView> viewer, std::filesystem::path output,
                std::function<void(const DocumentExportResult &)> completed = {},
                std::source_location origin = std::source_location::current());
} // namespace portals::ui
