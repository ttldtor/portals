// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_source.hpp>

#include <future>
#include <string>

int main() {
  using namespace portals;
  auto budget = SourceBudget::create();
  if (!budget) {
    return 1;
  }
  auto provider = std::make_shared<MemoryDocumentProvider>(std::make_shared<const std::string>("first\r\nsecond"));
  auto source = ByteDocumentSource::create(provider, *budget);
  if (!source) {
    return 2;
  }
  const auto onOwner = (*source)->inspect();
  if (onOwner || onOwner.error().code != SourceError::WRONG_THREAD) {
    return 2;
  }
  auto result = std::async(std::launch::async, [&] {
    auto metadata = (*source)->inspect();
    if (!metadata || metadata->byteExtent != 13) {
      return false;
    }
    auto page = (*source)->read({metadata->revision, 7, 6, 1});
    if (!page || !page->finalEnd() || page->bytes().size() != 6 || page->generation() != 1) {
      return false;
    }
    auto index = SparseDocumentIndex::create(*budget, metadata->revision);
    if (!index) {
      return false;
    }
    auto line = (*index)->locateLine(**source, 1);
    return line && line->state == DocumentLocationState::EXACT && line->checkpoint.offset == 7;
  });
  return result.get() ? 0 : 3;
}
