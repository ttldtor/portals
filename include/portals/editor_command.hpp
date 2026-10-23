// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

#include <memory>

namespace portals {
class ManagedScene;

namespace ui::detail {
struct RefAccess;
struct ActionAccess;
}

/** @brief Owning checked editor command, preserving exactly its collector reporting receipt.
 * Construction is private: adapters cannot mistake an unrelated failure for an already reported command.
 * Copying preserves the receipt without keeping a scene or widget alive. No implicit expected conversion exists.
 */
class EditorCommandResult {
public:
  /** @return Checked result; extracting this expected deliberately loses reporting provenance. */
  std::expected<void, TreeError> result() const noexcept {
    return result_;
  }

  /** @return True when the command succeeded, including an empty history no-op. */
  explicit operator bool() const noexcept {
    return result_.has_value();
  }

private:
  friend class ManagedScene;
  friend struct ui::detail::RefAccess;
  friend struct ui::detail::ActionAccess;

  EditorCommandResult(std::expected<void, TreeError> result, std::weak_ptr<const void> collector, bool reported)
      : result_(result), collector_(std::move(collector)), reported_(reported) {
  }

  std::expected<void, TreeError> result_;
  std::weak_ptr<const void> collector_;

  bool reported_{};
};
} // namespace portals
