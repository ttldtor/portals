// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace portals {

/** @brief Observable resource lookup failure, distinct from UTF-8 validation. */
enum class TextResourceError {
  /** @brief The caller's resolver could not find the requested resource key. */
  MISSING_RESOURCE,
  /** @brief Resource resolution was requested without a caller-owned resolver. */
  RESOLVER_REQUIRED,
  /** @brief The resolver reported failure or threw a non-allocation exception. */
  RESOLVER_FAILED
};

} // namespace portals
