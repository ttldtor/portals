// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/dialog.hpp>

namespace portals::detail {
struct DialogHostRecord;

struct DialogRecord {
  Node<Dialog> root;
  Node<Widget> panel;
  Node<Widget> body;
  Node<Widget> anchor;
  DialogOptions options;
  std::uint64_t activationRevision{};
  std::weak_ptr<DialogHostRecord> host;
  std::shared_ptr<std::function<std::expected<void, TreeError>(DialogDismissReason)>> dismissed;
  bool ready{};
  bool open{};
  bool keepDisabledPicture{};
  bool retired{};
};

struct DialogHostRecord {
  Node<OverlayHost> root;
  Node<Widget> body;
  std::vector<std::shared_ptr<DialogRecord>> layers = std::vector<std::shared_ptr<DialogRecord>>(0);
  std::size_t maximum{};
  bool operating{};
  bool staging{};
  bool ready{};
  bool retired{};
};
} // namespace portals::detail
