// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <system_error>

namespace portals {

/** @brief File boundary that failed before a PSS document could be installed. */
enum class PssFileOperation {
  OPEN,       ///< Open the explicitly requested file for reading.
  READ,       ///< Read bytes from an already opened file.
  INPUT_LIMIT ///< File bytes exceeded the admitted parser input budget.
};

/** @brief Allocation-free original file failure; the owning source identity is stored separately.
 * Capture cause immediately after the failing operation. The standard error category must outlive
 * this value, as required by std::error_code; the PSS file boundary uses standard static categories.
 * No file handle, input view, path view, or dynamically formatted message is retained.
 */
struct PssFileError {
  PssFileOperation operation{PssFileOperation::OPEN}; ///< Exact failing file boundary.
  std::error_code cause{};                            ///< Original platform error, or file_too_large for INPUT_LIMIT.
};

} // namespace portals
