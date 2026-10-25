// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/dialog.hpp>

namespace portals::ui::detail {
struct SceneActivity;
}

namespace portals::detail {
struct DialogHostRecord;
struct MessageRecord;

struct DialogRecord {
  Node<Dialog> root;
  Node<Widget> panel;
  Node<Widget> body;
  Node<Button> close;
  Node<Widget> anchor;
  Node<Widget> initialFocus;
  DialogOptions options;
  std::uint64_t activationRevision{};
  std::weak_ptr<DialogHostRecord> host;
  std::weak_ptr<MessageRecord> message;
  std::shared_ptr<std::function<std::expected<void, TreeError>(DialogDismissReason)>> dismissed;
  bool ready{};
  bool open{};
  bool keepDisabledPicture{};
  bool retired{};
};

struct DialogHostRecord {
  Node<OverlayHost> root;
  Node<Widget> body;
  Node<Button> close;
  std::vector<std::shared_ptr<DialogRecord>> layers = std::vector<std::shared_ptr<DialogRecord>>(0);
  std::size_t maximum{};
  bool operating{};
  bool staging{};
  bool ready{};
  bool retired{};
  std::weak_ptr<ui::detail::SceneActivity> choiceOwner;
};
} // namespace portals::detail
