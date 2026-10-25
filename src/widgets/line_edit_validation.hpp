// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/line_edit.hpp>

namespace portals::detail {
struct EditorValidationIdentity {
  std::uint64_t generation{};
  std::uint64_t revision{};

  bool operator==(const EditorValidationIdentity &) const = default;
};

enum class EditorValidationDecision {
  ACCEPT,
  REJECT,
  PENDING
};

// The binding stages work inside its transaction. A routed flush still has an outer tree borrow;
// service and the weak widget retirement hook provide the final cancellation boundary.
struct LineEditValidationGate {
  virtual ~LineEditValidationGate() = default;
  virtual std::expected<EditorValidationDecision, TreeError> check(std::string_view, EditorValidationIdentity) = 0;
  virtual void retire() noexcept = 0;
  virtual void hold() noexcept = 0;
  virtual void flush(bool afterBorrow) noexcept = 0;
  virtual void close() noexcept = 0;
  // Only the explicit completed worker-error branch carries worker diagnostic provenance.
  virtual void finished(LineEditResult &, bool workerFailure = false) = 0;
};

using EditorValidationComplete = std::function<std::expected<std::optional<LineEditResult>, TreeError>(
  EditorValidationIdentity, bool, std::optional<TreeError>)>;

struct LineEditValidationAccess {
  static std::expected<EditorValidationComplete, TreeError> attach(LineEditBinding &,
                                                                   std::shared_ptr<LineEditValidationGate>);
};
} // namespace portals::detail
