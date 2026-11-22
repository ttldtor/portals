// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/document_export.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  return std::move(*result);
}

template <class F> auto worker(F &&function) {
  return std::async(std::launch::async, std::forward<F>(function)).get();
}

DocumentError failure(SourceError code) {
  return {code, {}, {}};
}

class Input final : public ByteDocumentProvider {
public:
  std::string text;
  std::uint64_t revision{1};
  std::size_t reads{};
  std::size_t metadataCalls{};
  std::size_t maxReply{std::numeric_limits<std::size_t>::max()};
  bool knownExtent{true};
  bool seekable{true};
  bool mutateDuringRead{};
  bool throwDuringRead{};
  bool earlyEnd{};
  std::optional<DocumentError> metadataError;
  std::optional<DocumentError> readError;
  std::stop_source *cancelDuringRead{};

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {seekable, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    ++metadataCalls;

    if (metadataError) {
      return std::unexpected(*metadataError);
    }

    return ByteDocumentMetadata{revision, knownExtent ? std::optional<std::uint64_t>(text.size()) : std::nullopt};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> bytes,
                                                    std::stop_token) override {
    ++reads;

    if (throwDuringRead) {
      throw std::runtime_error("input fault");
    }

    if (readError) {
      return std::unexpected(*readError);
    }

    if (command.dataRevision != revision) {
      return std::unexpected(failure(SourceError::REVISION_CHANGED));
    }

    const auto offset = std::min<std::uint64_t>(command.offset, text.size());
    const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(std::min(bytes.size(), maxReply), text.size() - offset));

    std::memcpy(bytes.data(), text.data() + offset, count);

    if (mutateDuringRead) {
      ++revision;
    }

    if (cancelDuringRead) {
      cancelDuringRead->request_stop();
    }

    return ByteReadResult{count, earlyEnd || offset + count == text.size()};
  }
};

struct Fixture {
  std::shared_ptr<SourceBudget> budget;
  std::shared_ptr<Input> input = std::make_shared<Input>();
  std::shared_ptr<ByteDocumentSource> source;

  explicit Fixture(std::string bytes, std::size_t page = 16, SourceBudgetOptions limits = {})
      : budget(take(SourceBudget::create(limits))) {
    input->text = std::move(bytes);
    source = take(ByteDocumentSource::create(input, budget, {.maxPageBytes = page}));
  }
};

class Sink final : public DocumentExportSink {
public:
  std::string bytes;
  std::size_t writes{};
  std::size_t finishes{};
  std::size_t maxReply{std::numeric_limits<std::size_t>::max()};
  std::optional<DocumentError> writeError;
  std::optional<DocumentError> finishError;
  std::function<void()> afterWrite;
  bool throwWrite{};
  bool allocationThrow{};
  bool throwFinish{};
  bool invalidZero{};
  bool invalidCount{};

  DocumentWriteReply write(std::span<const std::byte> input, std::stop_token) override {
    ++writes;
    const auto count = std::min(input.size(), maxReply);

    if (count) {
      bytes.append(reinterpret_cast<const char *>(input.data()), count);
    }

    if (afterWrite) {
      afterWrite();
    }

    if (throwWrite) {
      if (allocationThrow) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("sink fault after acceptance");
    }

    if (invalidCount) {
      return {input.size() + 1, {}};
    }

    return {invalidZero ? 0 : count, writeError};
  }

  std::expected<void, DocumentError> finish(std::stop_token) override {
    ++finishes;

    if (throwFinish) {
      throw std::runtime_error("finish fault");
    }

    if (finishError) {
      return std::unexpected(*finishError);
    }

    return {};
  }
};

std::string copied(const DocumentCopy &copy) {
  if (copy.bytes().empty()) {
    return {};
  }

  return {reinterpret_cast<const char *>(copy.bytes().data()), copy.bytes().size()};
}

struct OutputPath {
  std::filesystem::path path;

  OutputPath() {
    static std::atomic<unsigned> serial{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

    path = std::filesystem::temp_directory_path() /
           ("portals-export-test-" + std::to_string(stamp) + "-" + std::to_string(serial++) + ".bin");
    REQUIRE_FALSE(std::filesystem::exists(path));
  }

  ~OutputPath() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  std::string read() const {
    std::ifstream stream(path, std::ios::binary);

    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  }
};
} // namespace

TEST_CASE("document export preserves raw byte ranges through short reads and partial sink writes") {
  Fixture f(std::string("\xff\0A\0B\0\r\n", 8), 3);
  f.input->maxReply = 2;
  auto sink = std::make_shared<Sink>();
  sink->maxReply = 1;
  const auto baseline = f.budget->usage();
  const auto result = worker([&] {
    return exportDocument(f.source, sink, {1, 7}, {.blockBytes = 2});
  });

  REQUIRE(result.complete);
  CHECK_FALSE(result.error);
  CHECK(result.revision.dataRevision == 1);
  CHECK(result.range.begin == 1);
  CHECK(result.range.end == 7);
  CHECK(result.bytesRead == 6);
  CHECK(result.bytesWritten == 6);
  CHECK(result.writtenCountExact);
  CHECK(sink->bytes == f.input->text.substr(1, 6));
  CHECK(sink->finishes == 1);
  CHECK(f.budget->usage().bytes == baseline.bytes);
  CHECK(f.budget->usage().work == 0);
  CHECK(f.budget->usage().pages == 0);
}

TEST_CASE("export finalizes exactly once on early validation metadata admission and cancellation failures") {
  for (int mode = 0; mode != 6; ++mode) {
    CAPTURE(mode);
    Fixture f("abc");
    auto sink = std::make_shared<Sink>();
    std::stop_source stop;
    SourceCredit pressure;
    auto source = f.source;
    DocumentByteRange range;
    DocumentExportOptions options;

    if (mode == 0) {
      source.reset();
    } else if (mode == 1) {
      options.blockBytes = 0;
    } else if (mode == 2) {
      range = {2, 1};
    } else if (mode == 3) {
      f.input->metadataError = failure(SourceError::IO_ERROR);
    } else if (mode == 4) {
      stop.request_stop();
    } else {
      pressure = take(f.budget->reserve({0, 0, f.budget->options().maxWork, 0}));
    }

    sink->finishError = failure(SourceError::CLOSED);
    const auto result = worker([&] {
      return exportDocument(source, sink, range, options, stop.get_token());
    });

    REQUIRE(result.error);
    CHECK_FALSE(result.complete);
    CHECK(result.error->code != SourceError::CLOSED);
    CHECK(result.bytesWritten == 0);
    CHECK(result.writtenCountExact);
    CHECK(sink->writes == 0);
    CHECK(sink->finishes == 1);
  }
}

TEST_CASE("sink errors and exceptions preserve the actual known prefix and distinguish unknown admission") {
  for (int mode = 0; mode != 7; ++mode) {
    CAPTURE(mode);
    Fixture f("abcdef");
    auto sink = std::make_shared<Sink>();
    sink->maxReply = 2;

    if (mode == 0) {
      sink->writeError = failure(SourceError::IO_ERROR);
    } else if (mode == 1 || mode == 2) {
      sink->throwWrite = true;
      sink->allocationThrow = mode == 2;
    } else if (mode == 3) {
      f.input->throwDuringRead = true;
    } else if (mode == 4) {
      sink->invalidZero = true;
    } else if (mode == 5) {
      sink->invalidCount = true;
    } else {
      sink->throwFinish = true;
    }

    const auto result = worker([&] {
      return exportDocument(f.source, sink);
    });

    REQUIRE(result.error);
    CHECK_FALSE(result.complete);
    CHECK(sink->finishes == 1);

    if (mode == 0) {
      CHECK(result.bytesWritten == 2);
      CHECK(result.writtenCountExact);
    } else if (mode == 1 || mode == 2 || mode == 5) {
      CHECK_FALSE(result.writtenCountExact);
      CHECK(result.bytesWritten == 0);
      CHECK_FALSE(sink->bytes.empty());
      CHECK(result.error->code == (mode == 2   ? SourceError::NO_MEMORY
                                   : mode == 5 ? SourceError::INVALID_REPLY
                                               : SourceError::CALLBACK_FAILED));
    } else if (mode == 3) {
      CHECK(result.bytesWritten == 0);
      CHECK(result.writtenCountExact);
      CHECK(sink->bytes.empty());
      CHECK(result.error->code == SourceError::CALLBACK_FAILED);
    } else if (mode == 4) {
      CHECK(result.error->code == SourceError::INVALID_REPLY);
    } else {
      CHECK(result.bytesWritten == 6);
      CHECK(result.writtenCountExact);
      CHECK(result.error->code == SourceError::CALLBACK_FAILED);
    }
  }
}

TEST_CASE("unknown extent export proves range starts and never treats an offset beyond EOF as an empty success") {
  Fixture f("abcdef", 2);
  f.input->knownExtent = false;
  auto sink = std::make_shared<Sink>();
  auto result = worker([&] {
    return exportDocument(f.source, sink, {2, {}});
  });

  REQUIRE(result.complete);
  CHECK(result.range.end == 6);
  CHECK(result.bytesRead == 5);
  CHECK(sink->bytes == "cdef");

  sink = std::make_shared<Sink>();
  result = worker([&] {
    return exportDocument(f.source, sink, {7, {}});
  });
  REQUIRE(result.error);
  CHECK(result.error->code == SourceError::INVALID_ARGUMENT);
  CHECK(sink->writes == 0);
  CHECK(sink->finishes == 1);

  sink = std::make_shared<Sink>();
  result = worker([&] {
    return exportDocument(f.source, sink, {6, 6});
  });
  REQUIRE(result.complete);
  CHECK(result.bytesRead == 1);
  CHECK(result.bytesWritten == 0);
  CHECK(sink->writes == 1);

  f.input->seekable = false;
  auto nonseekable = take(ByteDocumentSource::create(f.input, f.budget));
  sink = std::make_shared<Sink>();
  result = worker([&] {
    return exportDocument(nonseekable, sink, {1, 2});
  });
  REQUIRE(result.error);
  CHECK(result.error->code == SourceError::UNSUPPORTED);
}

TEST_CASE("export handles empty and unknown extent EOF and rejects premature known EOF") {
  for (const auto known : {false, true}) {
    Fixture f("");
    f.input->knownExtent = known;
    auto sink = std::make_shared<Sink>();
    const auto result = worker([&] {
      return exportDocument(f.source, sink);
    });

    REQUIRE(result.complete);
    CHECK(result.range.end == 0);
    CHECK(result.bytesWritten == 0);
    CHECK(sink->writes == 1);
    CHECK(sink->finishes == 1);
  }

  Fixture f("abcdef", 2);
  f.input->earlyEnd = true;
  auto sink = std::make_shared<Sink>();
  const auto result = worker([&] {
    return exportDocument(f.source, sink);
  });

  REQUIRE(result.error);
  CHECK(result.error->code == SourceError::INVALID_REPLY);
  CHECK(result.bytesRead == 2);
  CHECK(result.bytesWritten == 0);
}

TEST_CASE("export preserves a typed source cause and the first failure when cleanup also fails") {
  Fixture f("abc");
  const auto native = std::make_error_code(std::errc::permission_denied);
  f.input->readError = DocumentError{SourceError::IO_ERROR, native, {}};
  auto sink = std::make_shared<Sink>();
  sink->finishError = failure(SourceError::CLOSED);
  const auto result = worker([&] {
    return exportDocument(f.source, sink);
  });

  REQUIRE(result.error);
  CHECK(result.error->code == SourceError::IO_ERROR);
  CHECK(result.error->native == native);
  CHECK(result.bytesRead == 0);
  CHECK(result.bytesWritten == 0);
  CHECK(sink->finishes == 1);
  const auto copy = worker([&] {
    return copyDocumentBytes(f.source, {0, 3});
  });

  REQUIRE_FALSE(copy);
  CHECK(copy.error().native == native);
}

TEST_CASE("export checks stop and revision around reads and after accepted sink writes") {
  for (int mode = 0; mode != 4; ++mode) {
    CAPTURE(mode);
    Fixture f("abcdef", 3);
    std::stop_source stop;
    auto sink = std::make_shared<Sink>();

    if (mode == 0) {
      f.input->cancelDuringRead = &stop;
    } else if (mode == 1) {
      f.input->mutateDuringRead = true;
    } else if (mode == 2) {
      sink->afterWrite = [&] {
        stop.request_stop();
      };
    } else {
      sink->afterWrite = [&] {
        ++f.input->revision;
      };
    }

    const auto result = worker([&] {
      return exportDocument(f.source, sink, {}, {}, stop.get_token());
    });

    REQUIRE(result.error);
    CHECK(result.error->code == (mode % 2 ? SourceError::REVISION_CHANGED : SourceError::CANCELLED));
    CHECK(result.bytesWritten == (mode < 2 ? 0 : 3));
    CHECK(result.writtenCountExact);
    CHECK(sink->finishes == 1);
  }
}

TEST_CASE("copy owns budgeted original bytes and rejects oversized or unknown open ranges before reads") {
  Fixture f(std::string("A\0B\xff"
                        "C",
                        5),
            2);
  const auto baseline = f.budget->usage().bytes;
  auto copy = worker([&] {
    return take(copyDocumentBytes(f.source, {1, 4}, 3));
  });

  CHECK(copied(*copy) == f.input->text.substr(1, 3));
  CHECK(copy->revision().dataRevision == 1);
  CHECK(f.budget->usage().pages == 1);
  CHECK(f.budget->usage().bytes > baseline);
  const auto reads = f.input->reads;
  const auto excessive = worker([&] {
    return copyDocumentBytes(f.source, {0, 5}, 4);
  });

  REQUIRE_FALSE(excessive);
  CHECK(excessive.error().code == SourceError::PAGE_TOO_LARGE);
  CHECK(f.input->reads == reads);
  f.input->knownExtent = false;
  const auto unknown = worker([&] {
    return copyDocumentBytes(f.source, {});
  });
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().code == SourceError::UNSUPPORTED);
  CHECK(f.input->reads == reads);
  f.source.reset();
  CHECK(copied(*copy) == std::string("\0B\xff", 3));
  copy.reset();
  CHECK(f.budget->usage().bytes == SourceBudget::ROOT_METADATA_BYTES);
  CHECK(f.budget->usage().pages == 0);
}

TEST_CASE("copy validates unknown range EOF revision and cancellation without publishing a partial snapshot") {
  for (int mode = 0; mode != 4; ++mode) {
    Fixture f("abcd", 2);
    f.input->knownExtent = false;
    std::stop_source stop;
    const auto baseline = f.budget->usage().bytes;
    auto range = DocumentByteRange{1, 5};

    if (mode == 1) {
      range = {5, 5};
    } else if (mode == 2) {
      range = {0, 4};
      f.input->mutateDuringRead = true;
    } else if (mode == 3) {
      range = {0, 4};
      f.input->cancelDuringRead = &stop;
    }

    const auto result = worker([&] {
      return copyDocumentBytes(f.source, range, 8, stop.get_token());
    });

    REQUIRE_FALSE(result);
    CHECK(result.error().code == (mode < 2    ? SourceError::INVALID_ARGUMENT
                                  : mode == 2 ? SourceError::REVISION_CHANGED
                                              : SourceError::CANCELLED));
    CHECK(f.budget->usage().bytes == baseline);
    CHECK(f.budget->usage().pages == 0);
  }
}

TEST_CASE("a validated empty copy owns its result without reading payload") {
  Fixture f("abc");
  const auto result = worker([&] {
    return copyDocumentBytes(f.source, {3, 3});
  });

  REQUIRE(result);
  CHECK((*result)->bytes().empty());
  CHECK(f.input->reads == 0);
  CHECK((*result)->revision().dataRevision == 1);
}

TEST_CASE("copy and export classify irreducible shared-root peaks separately from external pressure") {
  SUBCASE("copy and its source page need two page owners") {
    Fixture f("abc", 3, {.maxPages = 1});
    const auto result = worker([&] {
      return copyDocumentBytes(f.source, {0, 3});
    });

    REQUIRE_FALSE(result);
    CHECK(result.error().code == SourceError::PAGE_TOO_LARGE);
    REQUIRE(result.error().budgetDemand);
    REQUIRE(result.error().budgetPeak);
    CHECK(result.error().budgetDemand->pages == 1);
    CHECK(result.error().budgetPeak->pages == 2);
    CHECK(result.error().budgetPeak->work == 1);
    CHECK(f.budget->usage().pages == 0);
  }

  SUBCASE("export and source need two work owners") {
    Fixture f("abc", 3, {.maxWork = 1});
    auto sink = std::make_shared<Sink>();
    const auto result = worker([&] {
      return exportDocument(f.source, sink);
    });

    REQUIRE(result.error);
    CHECK(result.error->code == SourceError::PAGE_TOO_LARGE);
    REQUIRE(result.error->budgetPeak);
    CHECK(result.error->budgetPeak->work == 2);
    CHECK(sink->finishes == 1);
    CHECK(f.budget->usage().work == 0);
  }

  SUBCASE("unrelated page pressure can be released") {
    Fixture f("abc", 3, {.maxPages = 2});
    auto external = take(f.budget->reserve({0, 1, 0, 0}));
    const auto result = worker([&] {
      return copyDocumentBytes(f.source, {0, 3});
    });

    REQUIRE_FALSE(result);
    CHECK(result.error().code == SourceError::WOULD_BLOCK);
    REQUIRE(result.error().budgetPeak);
    CHECK(result.error().budgetPeak->pages == 2);
    external = {};
    CHECK(worker([&] {
      return copyDocumentBytes(f.source, {0, 3});
    }));
  }
}

TEST_CASE("file export is cold exclusive and cleanup never creates an output after an early failure") {
  OutputPath output;
  auto file = take(DocumentFileSink::create(output.path));
  CHECK_FALSE(std::filesystem::exists(output.path));
  Fixture f("original");
  const auto result = worker([&] {
    return exportDocument(f.source, file);
  });

  REQUIRE(result.complete);
  CHECK(output.read() == "original");
  auto collision = take(DocumentFileSink::create(output.path));
  const auto refused = worker([&] {
    return exportDocument(f.source, collision);
  });

  REQUIRE(refused.error);
  CHECK(refused.error->code == SourceError::IO_ERROR);
  CHECK(refused.error->native);
  CHECK(refused.bytesWritten == 0);
  CHECK(output.read() == "original");

  auto provider = take(FileDocumentProvider::create(output.path));
  auto fileSource = take(ByteDocumentSource::create(provider, f.budget));
  auto sameSource = take(DocumentFileSink::create(output.path));
  const auto sourceRefused = worker([&] {
    return exportDocument(fileSource, sameSource);
  });

  REQUIRE(sourceRefused.error);
  CHECK(sourceRefused.error->code == SourceError::IO_ERROR);
  CHECK(sourceRefused.bytesWritten == 0);
  CHECK(output.read() == "original");

  for (int mode = 0; mode != 4; ++mode) {
    OutputPath missing;
    auto sink = take(DocumentFileSink::create(missing.path));
    std::stop_source stop;
    DocumentByteRange range;
    auto source = f.source;

    if (mode == 0) {
      source.reset();
    } else if (mode == 1) {
      range = {9, 9};
    } else if (mode == 2) {
      f.input->metadataError = failure(SourceError::IO_ERROR);
    } else {
      f.input->metadataError.reset();
      stop.request_stop();
    }

    const auto failed = worker([&] {
      return exportDocument(source, sink, range, {}, stop.get_token());
    });

    REQUIRE(failed.error);
    CHECK_FALSE(std::filesystem::exists(missing.path));
  }
}

TEST_CASE("file sink creates a successful empty export and closes preserved partial output on cancellation") {
  OutputPath empty;
  auto file = take(DocumentFileSink::create(empty.path));
  Fixture f("");
  REQUIRE(worker([&] {
            return exportDocument(f.source, file);
          }).complete);
  CHECK(std::filesystem::exists(empty.path));
  CHECK(std::filesystem::file_size(empty.path) == 0);

  OutputPath partial;
  auto partialFile = take(DocumentFileSink::create(partial.path));

  class CancellingSink final : public DocumentExportSink {
  public:
    std::shared_ptr<DocumentFileSink> file;
    std::stop_source stop;

    DocumentWriteReply write(std::span<const std::byte> bytes, std::stop_token token) override {
      auto reply = file->write(bytes, token);
      stop.request_stop();

      return reply;
    }

    std::expected<void, DocumentError> finish(std::stop_token token) override {
      return file->finish(token);
    }
  };

  auto sink = std::make_shared<CancellingSink>();
  sink->file = partialFile;
  Fixture bytes("abcdef", 2);
  const auto result = worker([&] {
    return exportDocument(bytes.source, sink, {}, {}, sink->stop.get_token());
  });

  REQUIRE(result.error);
  CHECK(result.error->code == SourceError::CANCELLED);
  CHECK(result.bytesWritten == 2);
  CHECK(result.writtenCountExact);
  CHECK(partial.read() == "ab");
  CHECK(partialFile->finish({}));
  CHECK(partialFile->write({}, {}).error->code == SourceError::CLOSED);

  auto native = empty.path.native();
  native.push_back({});
  native.push_back(static_cast<std::filesystem::path::value_type>('x'));
  const auto invalid = DocumentFileSink::create(std::filesystem::path(std::move(native)));

  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code == SourceError::INVALID_ARGUMENT);

  OutputPath unstarted;
  auto unopened = take(DocumentFileSink::create(unstarted.path));
  CHECK(unopened->finish({}));
  CHECK_FALSE(std::filesystem::exists(unstarted.path));
}
