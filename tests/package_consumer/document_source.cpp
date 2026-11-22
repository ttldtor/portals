// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_export.hpp>
#include <portals/document_search.hpp>
#include <portals/document_source.hpp>

#include <future>
#include <string>

namespace {
class CountingSink final : public portals::DocumentExportSink {
public:
  std::size_t count{};
  bool finished{};

  portals::DocumentWriteReply write(std::span<const std::byte> bytes, std::stop_token) override {
    count += bytes.size();
    return {bytes.size(), {}};
  }

  std::expected<void, portals::DocumentError> finish(std::stop_token) override {
    finished = true;
    return {};
  }
};
} // namespace

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
    if (!line || line->state != DocumentLocationState::EXACT || line->checkpoint.offset != 7) {
      return false;
    }

    auto search = DocumentSearchSession::create(*source, "second", {.hint = DocumentEncoding::UTF8}, {.workBytes = 2});

    if (!search) {
      return false;
    }

    bool found{};

    for (unsigned turn = 0; turn < 64; ++turn) {
      auto step = (*search)->advance();

      if (!step) {
        return false;
      }

      if (step->state != DocumentSearchState::PENDING) {
        found = step->state == DocumentSearchState::FOUND && step->match && step->match->byteOffset == 7 &&
                step->match->byteLength == 6 && step->match->text == "second";
        break;
      }
    }

    if (!found) {
      return false;
    }

    auto copied = copyDocumentBytes(*source, {7, 13});

    if (!copied) {
      return false;
    }

    const auto bytes = (*copied)->bytes();

    if (std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != "second") {
      return false;
    }

    auto sink = std::make_shared<CountingSink>();
    auto exported = exportDocument(*source, sink);

    return exported.complete && exported.bytesWritten == 13 && sink->count == 13 && sink->finished;
  });
  return result.get() ? 0 : 3;
}
