// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/document_source.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value.has_value());

  return std::move(*value);
}

template <class F> auto worker(F &&function) {
  return std::async(std::launch::async, std::forward<F>(function)).get();
}

std::shared_ptr<ByteDocumentSource> memorySource(const std::shared_ptr<SourceBudget> &budget, std::string text,
                                                 std::size_t maxPageBytes = 64 * 1024) {
  auto data = std::make_shared<const std::string>(std::move(text));
  auto provider = std::make_shared<MemoryDocumentProvider>(std::move(data));

  return take(ByteDocumentSource::create(std::move(provider), budget, {maxPageBytes, 4096}));
}

DocumentMetadata inspect(ByteDocumentSource &source) {
  return take(worker([&] {
    return source.inspect();
  }));
}

std::string text(const DocumentBytePage &page) {
  auto bytes = page.bytes();

  return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

class SyntheticProvider final : public ByteDocumentProvider {
public:
  std::uint64_t extent{(std::uint64_t{1} << 32) + 129};
  std::uint64_t revision{1};
  bool extentKnown{true};
  bool seekable{true};
  bool oversized{};
  bool noProgress{};
  bool throwAllocation{};
  bool throwCallback{};
  std::size_t shortRead{std::numeric_limits<std::size_t>::max()};
  std::size_t metadataCalls{};
  std::size_t readCalls{};
  std::size_t maxDestination{};
  std::uint64_t lastOffset{};
  std::stop_source *cancelDuringRead{};
  std::optional<DocumentError> error;

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {seekable, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    ++metadataCalls;

    return ByteDocumentMetadata{revision, extentKnown ? std::optional<std::uint64_t>(extent) : std::nullopt};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token) override {
    ++readCalls;
    maxDestination = std::max(maxDestination, destination.size());
    lastOffset = command.offset;

    if (throwAllocation) {
      throw std::bad_alloc();
    }

    if (throwCallback) {
      throw 1;
    }

    if (error) {
      return std::unexpected(*error);
    }

    if (command.dataRevision != revision) {
      return std::unexpected(DocumentError{SourceError::REVISION_CHANGED, {}, std::nullopt});
    }

    if (command.offset > extent) {
      return std::unexpected(DocumentError{SourceError::INVALID_ARGUMENT, {}, std::nullopt});
    }

    if (oversized) {
      return ByteReadResult{destination.size() + 1, false};
    }

    if (noProgress) {
      return ByteReadResult{0, false};
    }

    const auto count = static_cast<std::size_t>(
      std::min<std::uint64_t>(std::min(destination.size(), shortRead), extent - command.offset));

    std::fill_n(destination.data(), count, std::byte{'x'});

    if (cancelDuringRead) {
      cancelDuringRead->request_stop();
    }

    return ByteReadResult{count, command.offset + count == extent};
  }
};

struct FileFixture {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-document-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");

  ~FileFixture() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
  }

  void write(std::string_view value, bool append = false) {
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));

    REQUIRE(output.is_open());
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    REQUIRE(output.good());
  }
};
} // namespace

TEST_CASE("document source is cold and enforces worker-only metadata and reads") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();
  auto source = take(ByteDocumentSource::create(provider, budget));

  CHECK(provider->metadataCalls == 0);
  CHECK(provider->readCalls == 0);
  CHECK(source->inspect().error().code == SourceError::WRONG_THREAD);
  CHECK(source->read({{source->stamp(), 1}, 0, 1, 1}).error().code == SourceError::WRONG_THREAD);

  auto metadata = inspect(*source);
  auto page = take(worker([&] {
    return source->read({metadata.revision, 0, 3, 27});
  }));

  CHECK(page.revision() == metadata.revision);
  CHECK(page.generation() == 27);
  CHECK(text(page) == "xxx");
  CHECK(budget->usage().work == 0);
}

TEST_CASE("document byte coordinates remain 64 bit with unknown extent") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();

  provider->extentKnown = false;

  auto source = take(ByteDocumentSource::create(provider, budget, {17, 4096}));
  auto metadata = inspect(*source);
  const auto offset = (std::uint64_t{1} << 32) + 120;
  auto page = take(worker([&] {
    return source->read({metadata.revision, offset, 17, 71});
  }));

  CHECK_FALSE(metadata.byteExtent.has_value());
  CHECK(page.offset() == offset);
  CHECK(provider->lastOffset == offset);
  CHECK(page.bytes().size() == 9);
  CHECK(page.finalEnd());
  CHECK(provider->maxDestination == 17);
}

TEST_CASE("document rejects overflow foreign identity and oversized requests before provider I/O") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();
  auto source = take(ByteDocumentSource::create(provider, budget, {8, 4096}));
  auto revision = inspect(*source).revision;

  worker([&] {
    CHECK(source->read({revision, 0, 0, 0}).error().code == SourceError::INVALID_ARGUMENT);
    CHECK(source->read({revision, 0, 9, 0}).error().code == SourceError::INVALID_ARGUMENT);
    CHECK(source->read({revision, std::numeric_limits<std::uint64_t>::max(), 1, 0}).error().code ==
          SourceError::POSITION_EXHAUSTED);

    auto foreign = revision;

    ++foreign.source.epoch;
    CHECK(source->read({foreign, 0, 1, 0}).error().code == SourceError::STALE_SOURCE);
  });

  CHECK(provider->readCalls == 0);
}

TEST_CASE("retained page pins share one budget across views and outlive source") {
  SourceBudgetOptions options;

  options.maxPages = 1;

  auto budget = take(SourceBudget::create(options));
  auto source = memorySource(budget, "abcdefgh", 4);
  auto other = memorySource(budget, "other", 4);
  auto firstRevision = inspect(*source).revision;
  auto otherRevision = inspect(*other).revision;
  auto page = take(worker([&] {
    return source->read({firstRevision, 0, 4, 2});
  }));
  auto retained = page;
  const auto pinnedBytes = budget->usage().bytes;

  CHECK(worker([&] {
          return other->read({otherRevision, 0, 4, 3});
        })
          .error()
          .code == SourceError::WOULD_BLOCK);
  page = {};
  CHECK(budget->usage().bytes == pinnedBytes);
  source.reset();
  CHECK(text(retained) == "abcd");
  CHECK(budget->usage().pages == 1);
  retained = {};
  CHECK(budget->usage().pages == 0);

  auto admitted = worker([&] {
    return other->read({otherRevision, 0, 4, 3});
  });

  REQUIRE(admitted);
  CHECK(text(*admitted) == "othe");
}

TEST_CASE("document cancellation discards late reads without releasing retained pins") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();
  auto source = take(ByteDocumentSource::create(provider, budget));
  auto revision = inspect(*source).revision;
  auto retained = take(worker([&] {
    return source->read({revision, 0, 4, 1});
  }));
  const auto pinned = budget->usage().bytes;
  std::stop_source stop;

  stop.request_stop();
  CHECK(worker([&] {
          return source->read({revision, 0, 4, 2}, stop.get_token());
        })
          .error()
          .code == SourceError::CANCELLED);
  CHECK(provider->readCalls == 1);

  std::stop_source during;

  provider->cancelDuringRead = &during;
  CHECK(worker([&] {
          return source->read({revision, 0, 4, 3}, during.get_token());
        })
          .error()
          .code == SourceError::CANCELLED);
  CHECK(budget->usage().bytes == pinned);
  CHECK(budget->usage().pages == 1);
  CHECK(budget->usage().work == 0);
  CHECK(text(retained) == "xxxx");
}

TEST_CASE("document preserves native causes and rejects malformed provider replies") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();
  auto source = take(ByteDocumentSource::create(provider, budget));
  auto revision = inspect(*source).revision;

  worker([&] {
    provider->oversized = true;
    CHECK(source->read({revision, 0, 4, 0}).error().code == SourceError::INVALID_REPLY);
    provider->oversized = false;
    provider->noProgress = true;
    CHECK(source->read({revision, 0, 4, 0}).error().code == SourceError::NO_PROGRESS);
    provider->noProgress = false;
    provider->throwAllocation = true;
    CHECK(source->read({revision, 0, 4, 0}).error().code == SourceError::NO_MEMORY);
    provider->throwAllocation = false;
    provider->throwCallback = true;
    CHECK(source->read({revision, 0, 4, 0}).error().code == SourceError::CALLBACK_FAILED);
    provider->throwCallback = false;
    provider->error =
      DocumentError{SourceError::IO_ERROR, std::make_error_code(std::errc::permission_denied), std::nullopt};

    auto failed = source->read({revision, 0, 4, 0});

    CHECK(failed.error().code == SourceError::IO_ERROR);
    CHECK(failed.error().native == std::errc::permission_denied);
  });

  CHECK(budget->usage().pages == 0);
  CHECK(budget->usage().work == 0);
}

TEST_CASE("sparse byte index handles split CRLF lone CR LF and trailing empty lines") {
  auto budget = take(SourceBudget::create());
  auto source = memorySource(budget, "a\r\nb\rc\n\r\nz\r", 1);
  auto index = take(SparseDocumentIndex::create(budget, inspect(*source).revision, {2, 1, 1}));
  const std::array<std::uint64_t, 6> expected{0, 3, 5, 7, 9, 11};

  CHECK_FALSE(index->progress().exactLineCount.has_value());

  worker([&] {
    for (std::size_t step = 0; step < 20 && !index->progress().exactLineCount; ++step) {
      auto before = index->progress().scannedBytes;
      auto progress = take(index->advance(*source));

      CHECK(progress.scannedBytes - before <= 1);
      CHECK(progress.checkpoints <= 2);
    }

    REQUIRE(index->progress().exactLineCount.has_value());
    CHECK(*index->progress().exactLineCount == expected.size());

    for (std::size_t line = 0; line < expected.size(); ++line) {
      DocumentLineLocation location;

      for (std::size_t step = 0; step < 20 && location.state == DocumentLocationState::PENDING; ++step) {
        location = take(index->locateLine(*source, line));
      }

      CHECK(location.state == DocumentLocationState::EXACT);
      CHECK(location.checkpoint.line == line);
      CHECK(location.checkpoint.offset == expected[line]);
    }

    CHECK(take(index->locateLine(*source, expected.size())).state == DocumentLocationState::UNKNOWN);
  });
}

TEST_CASE("line lookup resumes bounded work across a long logical line and horizontal windows") {
  auto budget = take(SourceBudget::create());
  auto source = memorySource(budget, std::string(10001, 'x') + "\nend", 127);
  auto metadata = inspect(*source);
  auto index = take(SparseDocumentIndex::create(budget, metadata.revision, {4, 16, 256}));

  worker([&] {
    auto first = take(index->locateLine(*source, 1));

    CHECK(first.state == DocumentLocationState::PENDING);
    CHECK(index->progress().scannedBytes <= 127);
    CHECK_FALSE(index->progress().exactLineCount.has_value());

    auto window = take(source->read({metadata.revision, 9000, 127, 8}));

    CHECK(window.bytes().size() == 127);
    CHECK(text(window) == std::string(127, 'x'));

    auto location = first;

    for (std::size_t step = 0; step < 100 && location.state == DocumentLocationState::PENDING; ++step) {
      auto before = index->progress().scannedBytes;

      location = take(index->locateLine(*source, 1));
      CHECK(index->progress().scannedBytes - before <= 127);
    }

    CHECK(location.state == DocumentLocationState::EXACT);
    CHECK(location.checkpoint.offset == 10002);
    CHECK(index->progress().checkpoints <= 4);
  });
}

TEST_CASE("sparse checkpoint thinning stays bounded and supports backward exact replay") {
  std::string input;

  for (int i = 0; i < 200; ++i) {
    input += "x\n";
  }

  auto budget = take(SourceBudget::create());
  auto source = memorySource(budget, std::move(input), 17);
  auto index = take(SparseDocumentIndex::create(budget, inspect(*source).revision, {3, 1, 17}));
  const auto resident = budget->usage().bytes;

  worker([&] {
    while (!index->progress().exactLineCount) {
      take(index->advance(*source));
      CHECK(index->progress().checkpoints <= 3);
      CHECK(budget->usage().bytes == resident);
    }

    CHECK(*index->progress().exactLineCount == 201);
    CHECK(index->checkpointBeforeByte(0).offset == 0);
    CHECK(index->checkpointBeforeByte(205).offset <= 205);

    for (auto line : {199u, 3u, 150u, 0u, 200u}) {
      DocumentLineLocation location;

      for (std::size_t step = 0; step < 30 && location.state == DocumentLocationState::PENDING; ++step) {
        location = take(index->locateLine(*source, line));
      }

      CHECK(location.state == DocumentLocationState::EXACT);
      CHECK(location.checkpoint.offset == line * 2);
    }
  });
}

TEST_CASE("unknown extent reaches exact total only from explicit EOF including empty document") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();

  provider->extentKnown = false;
  provider->extent = 5;
  provider->shortRead = 2;

  auto source = take(ByteDocumentSource::create(provider, budget));
  auto index = take(SparseDocumentIndex::create(budget, inspect(*source).revision));

  worker([&] {
    CHECK_FALSE(take(index->advance(*source)).exactLineCount.has_value());
    CHECK_FALSE(take(index->advance(*source)).exactLineCount.has_value());
    CHECK(*take(index->advance(*source)).exactLineCount == 1);
    CHECK(index->progress().scannedBytes == 5);
  });

  auto empty = memorySource(budget, "");
  auto emptyIndex = take(SparseDocumentIndex::create(budget, inspect(*empty).revision));

  worker([&] {
    CHECK(*take(emptyIndex->advance(*empty)).exactLineCount == 1);
    CHECK(take(emptyIndex->locateLine(*empty, 0)).checkpoint.offset == 0);
    CHECK(take(emptyIndex->locateLine(*empty, 1)).state == DocumentLocationState::UNKNOWN);
  });
}

TEST_CASE("cached sparse locations retain worker revision and cancellation fences") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<SyntheticProvider>();

  provider->extent = 0;

  auto source = take(ByteDocumentSource::create(provider, budget));
  auto metadata = inspect(*source);
  auto index = take(SparseDocumentIndex::create(budget, metadata.revision));

  worker([&] {
    take(index->advance(*source));
  });
  CHECK(index->locateLine(*source, 0).error().code == SourceError::WRONG_THREAD);
  ++provider->revision;

  worker([&] {
    CHECK(index->locateLine(*source, 0).error().code == SourceError::REVISION_CHANGED);
    CHECK(index->locateLine(*source, 1).error().code == SourceError::REVISION_CHANGED);
    CHECK(index->advance(*source).error().code == SourceError::REVISION_CHANGED);
    CHECK(source->read({metadata.revision, 0, 1, 0}).error().code == SourceError::REVISION_CHANGED);
  });

  auto other = memorySource(budget, "foreign");

  worker([&] {
    CHECK(index->advance(*other).error().code == SourceError::STALE_SOURCE);
  });

  std::stop_source cancelled;

  cancelled.request_stop();
  worker([&] {
    CHECK(index->locateLine(*source, 0, cancelled.get_token()).error().code == SourceError::CANCELLED);
  });
}

TEST_CASE("index validates quota policy and tiny shared budget without hidden allocation growth") {
  auto budget = take(SourceBudget::create());
  auto source = memorySource(budget, "x\n");
  auto revision = inspect(*source).revision;
  auto usage = budget->usage();

  CHECK(SparseDocumentIndex::create(budget, revision, {1, 1, 1}).error().code == SourceError::INVALID_ARGUMENT);
  CHECK(SparseDocumentIndex::create(budget, revision, {2, 1, 1, DocumentLinePolicy::UTF16_LE}).error().code ==
        SourceError::UNSUPPORTED);
  CHECK(SparseDocumentIndex::create(budget, revision, {std::numeric_limits<std::size_t>::max(), 1, 1}).error().code ==
        SourceError::INVALID_ARGUMENT);
  CHECK(budget->usage().bytes == usage.bytes);

  auto tiny = take(SourceBudget::create({4096, 1, 1, 1}));
  auto provider = std::make_shared<SyntheticProvider>();

  CHECK(ByteDocumentSource::create(provider, tiny).error().code == SourceError::WOULD_BLOCK);
  CHECK(tiny->usage().bytes == 4096);
  CHECK(tiny->usage().consumers == 0);
}

TEST_CASE("file document adapter stays cold preserves native causes and has no record limit") {
  FileFixture fixture;
  auto budget = take(SourceBudget::create());
  auto provider = take(FileDocumentProvider::create(fixture.path));
  auto source = take(ByteDocumentSource::create(provider, budget));
  auto missing = worker([&] {
    return source->inspect();
  });

  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == SourceError::IO_ERROR);
  REQUIRE(missing.error().fileFailure.has_value());
  CHECK(missing.error().fileFailure->error == FileLogError::OPEN_FAILED);
  CHECK(missing.error().fileFailure->operation == FileLogOperation::OPEN);
  CHECK(missing.error().fileFailure->nativeCode != 0);

  fixture.write(std::string(100000, 'x') + "\r\nend");

  auto metadata = inspect(*source);
  auto page = take(worker([&] {
    return source->read({metadata.revision, 90000, 100, 1});
  }));

  CHECK(text(page) == std::string(100, 'x'));
  CHECK(metadata.byteExtent == 100005);

  fixture.write("more", true);

  auto changed = worker([&] {
    return source->read({metadata.revision, 0, 1, 2});
  });

  REQUIRE_FALSE(changed);
  CHECK(changed.error().code == SourceError::REVISION_CHANGED);
  CHECK(inspect(*source).revision.dataRevision != metadata.revision.dataRevision);
  CHECK(text(page) == std::string(100, 'x'));
}

TEST_CASE("file document small I/O block returns bounded short windows") {
  FileFixture fixture;

  fixture.write("abcdef");

  auto budget = take(SourceBudget::create());
  auto provider = take(FileDocumentProvider::create(fixture.path, 1));
  auto source = take(ByteDocumentSource::create(provider, budget));
  auto metadata = inspect(*source);
  auto first = worker([&] {
    return source->read({metadata.revision, 0, 2, 1});
  });

  REQUIRE(first.has_value());
  CHECK(text(*first) == "a");
  CHECK_FALSE(first->finalEnd());

  auto last = take(worker([&] {
    return source->read({metadata.revision, 5, 2, 2});
  }));

  CHECK(text(last) == "f");
  CHECK(last.finalEnd());

  auto normalProvider = take(FileDocumentProvider::create(fixture.path));
  auto wideSource = take(ByteDocumentSource::create(normalProvider, budget, {128 * 1024, 4096}));
  auto wideRevision = inspect(*wideSource).revision;
  auto wide = take(worker([&] {
    return wideSource->read({wideRevision, 0, 128 * 1024, 3});
  }));

  CHECK(text(wide) == "abcdef");
  CHECK(wide.finalEnd());
}

namespace {
class GatedDocumentProvider final : public ByteDocumentProvider {
public:
  explicit GatedDocumentProvider(std::shared_future<void> release) : release_(std::move(release)) {
  }

  std::promise<void> entered;
  std::atomic<std::size_t> active{};
  std::atomic<std::size_t> peakActive{};
  std::atomic<std::size_t> calls{};
  std::atomic<bool> gateTimedOut{};

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {true, true};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    return ByteDocumentMetadata{1, 8};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &, std::span<std::byte> destination,
                                                    std::stop_token) override {
    const auto current = active.fetch_add(1) + 1;
    auto peak = peakActive.load();

    while (peak < current && !peakActive.compare_exchange_weak(peak, current)) {
    }

    const auto call = calls.fetch_add(1);

    if (call == 0) {
      entered.set_value();

      if (release_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        gateTimedOut = true;
        active.fetch_sub(1);

        return std::unexpected(DocumentError{SourceError::CALLBACK_FAILED, {}, std::nullopt});
      }
    }

    std::fill(destination.begin(), destination.end(), std::byte{'v'});
    active.fetch_sub(1);

    return ByteReadResult{destination.size(), false};
  }

private:
  std::shared_future<void> release_;
};
} // namespace

TEST_CASE("cancelled blocked document read retains work and page credit until provider returns") {
  auto budget = take(SourceBudget::create());
  std::promise<void> release;
  auto provider = std::make_shared<GatedDocumentProvider>(release.get_future().share());
  auto entered = provider->entered.get_future();
  auto source = take(ByteDocumentSource::create(provider, budget));
  auto revision = inspect(*source).revision;
  const auto baselineBytes = budget->usage().bytes;
  std::stop_source cancel;
  auto pending = std::async(std::launch::async, [&] {
    return source->read({revision, 0, 4, 1}, cancel.get_token());
  });

  CHECK(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(provider->active == 1);
  cancel.request_stop();
  CHECK(pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  CHECK(budget->usage().pages == 1);
  CHECK(budget->usage().work == 1);
  CHECK(budget->usage().bytes > baselineBytes);

  // The owner must reject before waiting on the worker-held provider mutex.
  CHECK(source->inspect().error().code == SourceError::WRONG_THREAD);
  CHECK(source->read({revision, 0, 1, 2}).error().code == SourceError::WRONG_THREAD);
  CHECK_FALSE(provider->gateTimedOut.load());
  CHECK(provider->active == 1);
  CHECK(budget->usage().pages == 1);
  CHECK(budget->usage().work == 1);

  release.set_value();
  CHECK(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

  auto result = pending.get();

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == SourceError::CANCELLED);
  CHECK(budget->usage().pages == 0);
  CHECK(budget->usage().work == 0);
  CHECK(budget->usage().bytes == baselineBytes);
  CHECK_FALSE(provider->gateTimedOut.load());
}

TEST_CASE("shared document source serializes two worker reads without UI lock waits") {
  auto budget = take(SourceBudget::create());
  std::promise<void> release;
  auto provider = std::make_shared<GatedDocumentProvider>(release.get_future().share());
  auto entered = provider->entered.get_future();
  auto source = take(ByteDocumentSource::create(provider, budget));
  auto revision = inspect(*source).revision;
  auto first = std::async(std::launch::async, [&] {
    return source->read({revision, 0, 2, 11});
  });

  CHECK(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

  std::promise<void> secondStarted;
  auto secondEntered = secondStarted.get_future();
  auto second = std::async(std::launch::async, [&] {
    secondStarted.set_value();

    return source->read({revision, 2, 2, 12});
  });

  CHECK(secondEntered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(second.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  CHECK(source->inspect().error().code == SourceError::WRONG_THREAD);
  CHECK(source->read({revision, 0, 1, 13}).error().code == SourceError::WRONG_THREAD);
  CHECK_FALSE(provider->gateTimedOut.load());

  release.set_value();
  CHECK(first.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(second.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

  auto firstPage = take(first.get());
  auto secondPage = take(second.get());

  CHECK(provider->calls == 2);
  CHECK(provider->peakActive == 1);
  CHECK(provider->active == 0);
  CHECK_FALSE(provider->gateTimedOut.load());
  CHECK(text(firstPage) == "vv");
  CHECK(text(secondPage) == "vv");
  CHECK(firstPage.generation() == 11);
  CHECK(secondPage.generation() == 12);
  CHECK(budget->usage().work == 0);
  CHECK(budget->usage().pages == 2);
}
