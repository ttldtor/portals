// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);

  if (!scene) {
    return 2;
  }

  ui::Ref<LogView> log;
  auto mounted = ui::mount(**scene, ui::logView(log, "Ready\n", {.limits = {.maxLines = 2}}).height(6));

  if (!mounted || !log.append("界\ncafe\u0301\n")) {
    return 3;
  }

  auto status = log.status();

  if (!status || status->retainedLines != 2 || status->droppedLines != 1 || !status->followTail) {
    return 4;
  }

  const auto invalid = log.appendBatch("good\n\xff");

  if (invalid || invalid.error().text != TextError{TextErrorKind::INVALID_UTF8, 5}) {
    return 5;
  }

  return log.clear() && log.tail() && log.status()->retainedLines == 0 ? 0 : 6;
}
