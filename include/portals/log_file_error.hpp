// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>

namespace portals {
/** @brief File-specific failures; diagnostics retain native codes without allocating messages. */
enum class FileLogError {
  INVALID_ARGUMENT, ///< Invalid option, offset, pattern or buffer size.
  OPEN_FAILED,      ///< Opening the owned path failed.
  READ_FAILED,      ///< Native positioned read failed.
  METADATA_FAILED,  ///< File identity or byte extent could not be read.
  RECORD_TOO_LONG,  ///< A complete record cannot be found within maxRecordBytes.
  INVALID_UTF8,     ///< A complete record contains invalid UTF-8.
  FILE_CHANGED,     ///< Truncation or rotation invalidated the requested generation.
  CANCELLED,        ///< Cooperative cancellation was requested.
  NO_MEMORY         ///< A bounded allocation failed.
};

/** @brief Operation that produced a fixed native diagnostic. */
enum class FileLogOperation {
  CREATE,   ///< Validate options or allocate the path descriptor.
  OPEN,     ///< Open the source path for reading.
  METADATA, ///< Obtain file identity and byte extent.
  READ,     ///< Read a bounded range or record.
  SEARCH    ///< Search a bounded byte chunk.
};

/** @brief Copyable diagnostic; nativeCode is errno/GetLastError for native failures, otherwise zero. */
struct FileLogFailure {
  FileLogError error{FileLogError::INVALID_ARGUMENT}; ///< Semantic failure.
  std::uint32_t nativeCode{};                         ///< Unmodified native error code.
  FileLogOperation operation{FileLogOperation::READ}; ///< Exact failing operation.
};
} // namespace portals
