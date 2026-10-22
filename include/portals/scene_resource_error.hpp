// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <system_error>

namespace portals {
/** @brief Checked resource failure; no arbitrary expression is evaluated. */
enum class SceneResourceErrorCode {
  SYNTAX,                ///< Missing delimiter, invalid token or unsupported expression.
  INVALID_UTF8,          ///< Source contains an invalid UTF-8 sequence.
  INVALID_LIMIT,         ///< A resource limit is zero or depth exceeds the supported stack bound.
  SOURCE_LIMIT,          ///< Source bytes exceed the configured cap.
  NODE_LIMIT,            ///< Node count exceeds the configured cap.
  DEPTH_LIMIT,           ///< Nesting exceeds the configured cap.
  STRING_LIMIT,          ///< Decoded string bytes exceed the configured cap.
  PROPERTY_LIMIT,        ///< A node has too many properties.
  UNKNOWN_TYPE,          ///< Type is absent from the explicit allowlist.
  DUPLICATE_TYPE,        ///< Registry already contains this explicit type name.
  UNKNOWN_PROPERTY,      ///< Property is unsupported for this registered type.
  DUPLICATE_ID,          ///< Two nodes declare the same nonempty id.
  DUPLICATE_PROPERTY,    ///< A property or positional text was repeated.
  INVALID_VALUE,         ///< A property value violates its declared primitive bounds.
  CHILDREN_FORBIDDEN,    ///< A leaf type declares child nodes.
  FILE_IO,               ///< File open/read failed; system carries the OS cause when available.
  NO_MEMORY,             ///< Parser, resolver or recipe storage allocation failed.
  TEXT,                  ///< Original text/resource validation failed; ManagedError retains its category.
  FACTORY,               ///< An explicit typed recipe factory failed; its original cause is preserved.
  CYCLIC_TYPE,           ///< A named control inherits from itself, directly or indirectly.
  TYPE_LIMIT,            ///< Named control definitions exceed their finite context budget.
  MATERIALIZATION_LIMIT, ///< Merged property leaves exceed the complete mount budget.
  SECRET_LITERAL         ///< A protected editor declares a nonempty literal or text-resource expression.
};

/** @brief Owning resource position, independent of parser, scene and widget lifetimes. */
struct SceneResourceError {
  SceneResourceErrorCode code{SceneResourceErrorCode::SYNTAX}; ///< Original resource failure category.
  std::shared_ptr<const std::string>
    source{}; ///< Safe source label or file path, never string-document bytes; may be empty on allocation failure.
  std::size_t offset{};     ///< Zero-based byte offset in the resource, including UTF-8 bytes.
  std::size_t line{1};      ///< One-based source line; zero for a registration failure without a source position.
  std::size_t column{1};    ///< One-based byte column; zero for registration failures; tabs count as one byte.
  std::error_code system{}; ///< Independent file-system failure; empty for parser failures.
  std::shared_ptr<const std::string>
    propertyPath{}; ///< Owning type/property path, including an inherited setting; empty when unavailable.
};
} // namespace portals
