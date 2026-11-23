// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/log_file.hpp>

namespace portals::detail {
// Worker-local native handle; the caller owns its conservative metadata credit.
class FileReadSession {
public:
  virtual ~FileReadSession() = default;
  virtual FileLogMetadata metadata() const noexcept = 0;
  virtual std::expected<std::size_t, FileLogFailure> read(std::uint64_t offset, std::span<std::byte> destination,
                                                          std::stop_token stop) = 0;
  virtual std::expected<void, FileLogFailure> seal(std::stop_token stop) = 0;
};

std::expected<std::unique_ptr<FileReadSession>, FileLogFailure> openFileReadSession(std::shared_ptr<FileLogModel> model,
                                                                                    std::stop_token stop);
} // namespace portals::detail
