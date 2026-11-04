// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <portals/log_file.hpp>

int main() {
  const auto path =
    std::filesystem::temp_directory_path() /
    ("portals-log-consumer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");

  struct Cleanup {
    std::filesystem::path path;

    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{path};

  {
    std::ofstream output(path, std::ios::binary);
    output << "alpha\nbeta\n";
    if (!output)
      return 1;
  }
  auto model = portals::FileLogModel::create(path);
  if (!model)
    return 2;
  return std::async(std::launch::async,
                    [model = *model] {
                      auto info = model->metadata();
                      std::array<std::byte, 16> bytes{};
                      auto read = model->readBytes(0, bytes);
                      auto found = model->findLiteral("beta", 0, 1024 * 1024);
                      return info && info->byteExtent == 11 && read && *read == 11 && found &&
                                 found->matchByteOffset == 6
                               ? 0
                               : 3;
                    })
    .get();
}
