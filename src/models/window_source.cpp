// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/window_source.hpp>

namespace portals::detail {
std::expected<SourceStamp, SourceError> windowSourceStamp() {
  static std::atomic<std::uint64_t> sequence{};
  auto current = sequence.load();

  for (;;) {
    if (current == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
    }

    if (sequence.compare_exchange_weak(current, current + 1)) {
      return SourceStamp{current + 1, 1};
    }
  }
}
} // namespace portals::detail
