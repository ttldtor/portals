// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/model_position.hpp>

bool modelPositionHeaderProbe() {
  portals::ModelBookmark bookmark;

  return bookmark.metadata().empty() && bookmark.position().revision == 0;
}
