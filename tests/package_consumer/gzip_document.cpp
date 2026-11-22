// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <portals/gzip_document.hpp>
#include <portals/ui_gzip.hpp>
#include <string_view>

int main() {
  constexpr std::array<unsigned char, 36> ARCHIVE{31,  139, 8,   0,   0,   0,   0,   0,   2,   255, 243, 72,
                                                  205, 201, 201, 215, 81,  72,  44,  74,  206, 200, 44,  75,
                                                  85,  228, 2,   0,   159, 142, 167, 104, 16,  0,   0,   0};
  constexpr std::string_view TEXT = "Hello, archive!\n";
  const auto bytes =
    std::make_shared<const std::string>(reinterpret_cast<const char *>(ARCHIVE.data()), ARCHIVE.size());
  const auto compressed = std::make_shared<portals::MemoryDocumentProvider>(bytes);
  const auto budget = portals::SourceBudget::create();

  if (!budget) {
    return 1;
  }

  auto gzip = portals::GzipDocumentProvider::create(compressed, *budget);

  if (!gzip) {
    return 2;
  }

  const auto metadata = (*gzip)->metadata({});

  if (!metadata || metadata->byteExtent) {
    return 3;
  }

  std::array<std::byte, 64> output{};
  const auto read = (*gzip)->read({metadata->dataRevision, 0}, output, {});

  if (!read || !read->finalEnd || read->bytesRead != TEXT.size() ||
      std::string_view(reinterpret_cast<const char *>(output.data()), read->bytesRead) != TEXT) {
    return 4;
  }

  const auto logical = (*gzip)->metadata({});

  if (!logical || logical->byteExtent != TEXT.size()) {
    return 5;
  }

  const auto descriptor = portals::ui::gzip(compressed);

  if (!descriptor.budget()) {
    return 6;
  }

  auto recipe = portals::ui::viewer(descriptor);
  (void)recipe;

  return 0;
}
