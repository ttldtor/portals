// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_export.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <utility>
#ifdef _WIN32
#  include <io.h>
#  include <sys/stat.h>
#else
#  include <unistd.h>
#endif

namespace portals {
namespace {
constexpr std::size_t EXPORT_WORK_BYTES = 4096;
constexpr std::size_t COPY_METADATA_BYTES = 4096;

DocumentError error(SourceError code) {
  return {code, {}, {}};
}

DocumentError nativeError() {
  return {SourceError::IO_ERROR, std::error_code(errno, std::generic_category()), {}};
}

DocumentError withOverlap(ByteDocumentSource &source, DocumentError failure, SourceBudgetUsage owned) {
  if (!failure.budgetDemand && !failure.budgetPeak) {
    return failure;
  }

  auto peak = failure.budgetPeak ? *failure.budgetPeak : *failure.budgetDemand;
  constexpr auto MAX = std::numeric_limits<std::size_t>::max();

  if (owned.bytes > MAX - peak.bytes || owned.pages > MAX - peak.pages || owned.work > MAX - peak.work) {
    failure.code = SourceError::PAGE_TOO_LARGE;

    return failure;
  }

  peak.bytes += owned.bytes;
  peak.pages += owned.pages;
  peak.work += owned.work;
  failure.budgetPeak = peak;

  const auto cap = source.budget()->options();
  const auto pinned = source.pinnedBudgetBytes();
  const auto base = source.envelopeBytes() + SourceBudget::ROOT_METADATA_BYTES;

  if (peak.bytes > cap.maxBytes || pinned > cap.maxBytes - peak.bytes || base > cap.maxBytes - peak.bytes - pinned ||
      peak.pages > cap.maxPages || peak.work > cap.maxWork || peak.consumers >= cap.maxConsumers) {
    failure.code = SourceError::PAGE_TOO_LARGE;
  }

  return failure;
}

void validateRange(DocumentByteRange range, std::optional<std::uint64_t> extent) {
  if ((range.end && *range.end < range.begin) ||
      (extent && (range.begin > *extent || (range.end && *range.end > *extent)))) {
    throw error(SourceError::INVALID_ARGUMENT);
  }
}

void accepted(std::uint64_t &total, std::size_t count) {
  if (count > std::numeric_limits<std::uint64_t>::max() - total) {
    throw error(SourceError::POSITION_EXHAUSTED);
  }

  total += count;
}

DocumentError readFailure(ByteDocumentSource &source, DocumentError failure) {
  // A page reservation can fail while the source's read-work envelope is live.
  // Provider failures already carry their complete enclosing source peak.
  if (!failure.budgetPeak && failure.budgetDemand && failure.budgetDemand->pages) {
    auto peak = *failure.budgetDemand;

    if (peak.bytes > std::numeric_limits<std::size_t>::max() - source.envelopeBytes()) {
      failure.code = SourceError::PAGE_TOO_LARGE;

      return failure;
    }

    peak.bytes += source.envelopeBytes();
    ++peak.work;
    failure.budgetPeak = peak;
  }

  return failure;
}

std::expected<DocumentMetadata, DocumentError> inspectRevision(ByteDocumentSource &source, DocumentRevision revision,
                                                               std::stop_token stop) {
  auto metadata = source.inspect(stop);

  if (metadata && metadata->revision != revision) {
    return std::unexpected(error(SourceError::REVISION_CHANGED));
  }

  return metadata;
}

std::optional<std::uint64_t> probeBegin(ByteDocumentSource &source, const DocumentMetadata &metadata,
                                        DocumentByteRange range, std::uint64_t &bytesRead, std::stop_token stop) {
  validateRange(range, metadata.byteExtent);

  if (metadata.byteExtent || !range.begin) {
    return metadata.byteExtent;
  }

  if (!source.capabilities().seekable) {
    throw error(SourceError::UNSUPPORTED);
  }

  auto page = source.read({metadata.revision, range.begin - 1, 1, 1}, stop);

  if (!page) {
    throw readFailure(source, page.error());
  }

  accepted(bytesRead, page->bytes().size());

  if (page->bytes().size() != 1) {
    throw error(SourceError::INVALID_ARGUMENT);
  }

  auto current = inspectRevision(source, metadata.revision, stop);

  if (!current) {
    throw withOverlap(source, current.error(), {1 + SourceArena::METADATA_BYTES, 1, 0, 0});
  }

  auto extent = current->byteExtent;

  if (page->finalEnd()) {
    if (extent && *extent != range.begin) {
      throw error(SourceError::INVALID_REPLY);
    }

    extent = range.begin;
  }

  validateRange(range, extent);

  return extent;
}

void closeDescriptor(int descriptor) noexcept {
#ifdef _WIN32
  (void)_close(descriptor);
#else
  (void)::close(descriptor);
#endif
}
} // namespace

DocumentFileSink::DocumentFileSink(std::filesystem::path path) : path_(std::move(path)) {
}

std::expected<std::shared_ptr<DocumentFileSink>, DocumentError> DocumentFileSink::create(std::filesystem::path path) {
  if (path.empty() || std::find(path.native().begin(), path.native().end(), std::filesystem::path::value_type{}) !=
                        path.native().end()) {
    return std::unexpected(error(SourceError::INVALID_ARGUMENT));
  }

  try {
    return std::shared_ptr<DocumentFileSink>(new DocumentFileSink(std::move(path)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(SourceError::NO_MEMORY));
  }
}

DocumentFileSink::~DocumentFileSink() {
  if (descriptor_ >= 0) {
    closeDescriptor(descriptor_);
  }
}

std::expected<void, DocumentError> DocumentFileSink::open() {
  if (finished_) {
    return std::unexpected(error(SourceError::CLOSED));
  }

  if (descriptor_ >= 0) {
    return {};
  }

#ifdef _WIN32
  descriptor_ = _wopen(path_.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
  descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif

  if (descriptor_ < 0) {
    return std::unexpected(nativeError());
  }

  return {};
}

DocumentWriteReply DocumentFileSink::write(std::span<const std::byte> bytes, std::stop_token stop) {
  if (stop.stop_requested()) {
    return {0, error(SourceError::CANCELLED)};
  }

  auto opened = open();

  if (!opened) {
    return {0, opened.error()};
  }

  if (bytes.empty()) {
    return {};
  }

  const auto count = std::min<std::size_t>(bytes.size(), INT_MAX);

  for (;;) {
#ifdef _WIN32
    const auto written = _write(descriptor_, bytes.data(), static_cast<unsigned>(count));
#else
    const auto written = ::write(descriptor_, bytes.data(), count);
#endif

    if (written >= 0) {
      return {static_cast<std::size_t>(written), {}};
    }

    if (errno != EINTR) {
      return {0, nativeError()};
    }

    if (stop.stop_requested()) {
      return {0, error(SourceError::CANCELLED)};
    }
  }
}

std::expected<void, DocumentError> DocumentFileSink::finish(std::stop_token stop) {
  (void)stop;

  if (finished_) {
    return {};
  }

  finished_ = true;

  if (descriptor_ < 0) {
    return {};
  }

  const auto descriptor = std::exchange(descriptor_, -1);
#ifdef _WIN32
  const auto status = _close(descriptor);
#else
  const auto status = ::close(descriptor);
#endif

  if (status < 0) {
    return std::unexpected(nativeError());
  }

  return {};
}

namespace {
struct ExportOperation {
  ByteDocumentSource &source;
  DocumentExportSink &sink;
  DocumentExportResult &result;
  DocumentExportOptions options;
  std::stop_token stop;
  SourceCredit work;
  std::optional<DocumentBytePage> page;
  std::size_t pageBytes{};
  std::optional<std::uint64_t> initialExtent;
  bool writing{};
  bool ended{};

  SourceBudgetUsage owned() const noexcept {
    auto usage = work.usage();

    usage.bytes += pageBytes;
    usage.pages += page.has_value();

    return usage;
  }

  DocumentError qualify(DocumentError failure) const {
    return withOverlap(source, std::move(failure), owned());
  }

  DocumentMetadata inspect() {
    auto current = inspectRevision(source, result.revision, stop);

    if (!current) {
      throw current.error();
    }

    if (initialExtent && current->byteExtent && initialExtent != current->byteExtent) {
      throw error(SourceError::INVALID_REPLY);
    }

    validateRange(result.range, current->byteExtent);

    return *current;
  }

  void write(std::span<const std::byte> bytes, bool empty = false) {
    do {
      if (stop.stop_requested()) {
        throw error(SourceError::CANCELLED);
      }

      writing = true;

      const auto reply = sink.write(bytes, stop);

      writing = false;

      if (reply.written > bytes.size()) {
        result.writtenCountExact = false;
        throw error(SourceError::INVALID_REPLY);
      }

      if (!reply.written && !reply.error && !empty) {
        result.writtenCountExact = false;

        throw error(SourceError::INVALID_REPLY);
      }

      if (reply.written > std::numeric_limits<std::uint64_t>::max() - result.bytesWritten) {
        result.writtenCountExact = false;
        throw error(SourceError::POSITION_EXHAUSTED);
      }

      result.bytesWritten += reply.written;
      bytes = bytes.subspan(reply.written);

      if (reply.error) {
        throw *reply.error;
      }
    } while (!bytes.empty());
  }

  void run() {
    auto metadata = source.inspect(stop);

    if (!metadata) {
      throw metadata.error();
    }

    result.revision = metadata->revision;
    initialExtent = metadata->byteExtent;
    validateRange(result.range, initialExtent);

    auto reserved = source.budget()->reserve({EXPORT_WORK_BYTES, 0, 1, 0});

    if (!reserved) {
      auto failure = error(reserved.error());

      failure.budgetDemand = SourceBudgetUsage{EXPORT_WORK_BYTES, 0, 1, 0};

      throw failure;
    }

    work = std::move(*reserved);

    const auto extent = probeBegin(source, *metadata, result.range, result.bytesRead, stop);

    if (!result.range.end) {
      result.range.end = extent;
    }

    auto offset = result.range.begin;

    while (!result.range.end || offset < *result.range.end) {
      if (stop.stop_requested()) {
        throw error(SourceError::CANCELLED);
      }

      const auto remaining =
        result.range.end ? *result.range.end - offset : std::numeric_limits<std::uint64_t>::max() - offset;
      const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min(options.blockBytes, source.maxPageBytes()), remaining));

      if (!count) {
        throw error(SourceError::POSITION_EXHAUSTED);
      }

      auto read = source.read({result.revision, offset, count, 1}, stop);

      if (!read) {
        throw readFailure(source, read.error());
      }

      page = std::move(*read);
      pageBytes = count + SourceArena::METADATA_BYTES;
      accepted(result.bytesRead, page->bytes().size());

      auto end = offset;

      accepted(end, page->bytes().size());

      const auto current = inspect();

      if ((current.byteExtent && end > *current.byteExtent) ||
          (page->finalEnd() && current.byteExtent && end != *current.byteExtent)) {
        throw error(SourceError::INVALID_REPLY);
      }

      if (page->finalEnd() && result.range.end && end < *result.range.end) {
        throw error(initialExtent ? SourceError::INVALID_REPLY : SourceError::INVALID_ARGUMENT);
      }

      if (!page->bytes().empty()) {
        write(page->bytes());
      }

      offset = end;
      ended = page->finalEnd();
      page.reset();
      pageBytes = 0;

      if (ended) {
        if (!result.range.end) {
          result.range.end = offset;
        }

        break;
      }
    }

    // A successful empty operation explicitly begins output. Cleanup alone
    // never creates a file after validation, source or admission failures.
    if (!result.bytesWritten) {
      write({}, true);
    }

    const auto current = inspect();

    if ((ended && current.byteExtent && offset != *current.byteExtent) || !result.range.end) {
      throw error(SourceError::INVALID_REPLY);
    }
  }
};
} // namespace

DocumentExportResult exportDocument(std::shared_ptr<ByteDocumentSource> source,
                                    std::shared_ptr<DocumentExportSink> sink, DocumentByteRange range,
                                    DocumentExportOptions options, std::stop_token stop) {
  DocumentExportResult result;

  result.range = range;

  std::optional<ExportOperation> operation;

  try {
    if (!source || !sink || !options.blockBytes || (range.end && *range.end < range.begin)) {
      throw error(SourceError::INVALID_ARGUMENT);
    }

    operation.emplace(ExportOperation{*source, *sink, result, options, stop, {}, {}, 0, {}, false, false});
    operation->run();
  } catch (const DocumentError &failure) {
    result.error = operation ? operation->qualify(failure) : failure;
  } catch (const std::bad_alloc &) {
    result.error = error(SourceError::NO_MEMORY);
  } catch (...) {
    result.error = error(SourceError::CALLBACK_FAILED);
  }

  if (operation && operation->writing) {
    result.writtenCountExact = false;
  }

  if (sink) {
    try {
      auto finished = sink->finish(stop);

      if (!finished && !result.error) {
        result.error = operation ? operation->qualify(finished.error()) : finished.error();
      }
    } catch (const std::bad_alloc &) {
      if (!result.error) {
        result.error = error(SourceError::NO_MEMORY);
      }
    } catch (...) {
      if (!result.error) {
        result.error = error(SourceError::CALLBACK_FAILED);
      }
    }
  }

  result.complete = !result.error;

  return result;
}

std::expected<std::shared_ptr<const DocumentCopy>, DocumentError>
copyDocumentBytes(std::shared_ptr<ByteDocumentSource> source, DocumentByteRange range, std::size_t maxBytes,
                  std::stop_token stop) {
  if (!source || !maxBytes || (range.end && *range.end < range.begin)) {
    return std::unexpected(error(SourceError::INVALID_ARGUMENT));
  }

  if (range.end && *range.end - range.begin > maxBytes) {
    return std::unexpected(error(SourceError::PAGE_TOO_LARGE));
  }

  std::shared_ptr<DocumentCopy> output;
  SourceBudgetUsage owned{};
  std::optional<DocumentBytePage> page;
  std::size_t pageBytes{};
  const auto qualify = [&](DocumentError failure) {
    auto usage = owned;

    usage.bytes += pageBytes;
    usage.pages += page.has_value();

    return withOverlap(*source, std::move(failure), usage);
  };

  try {
    auto metadata = source->inspect(stop);

    if (!metadata) {
      throw metadata.error();
    }

    if (!range.end) {
      range.end = metadata->byteExtent;
    }

    if (!range.end) {
      throw error(SourceError::UNSUPPORTED);
    }

    validateRange(range, metadata->byteExtent);

    const auto size = *range.end - range.begin;
    const auto nativeBytes = COPY_METADATA_BYTES + sizeof(DocumentCopy);

    if (size > maxBytes || size > std::numeric_limits<std::size_t>::max() - nativeBytes - SourceArena::METADATA_BYTES) {
      throw error(SourceError::PAGE_TOO_LARGE);
    }

    std::uint64_t probeBytes{};

    (void)probeBegin(*source, *metadata, range, probeBytes, stop);

    const auto payloadBytes = std::max<std::size_t>(static_cast<std::size_t>(size), 1) + SourceArena::METADATA_BYTES;
    const auto demand = SourceBudgetUsage{nativeBytes + payloadBytes, 1, 0, 0};
    auto reserved = source->budget()->reserve(demand);

    if (!reserved) {
      auto failure = error(reserved.error());

      failure.budgetDemand = demand;

      throw failure;
    }

    owned = demand;

    auto payload = reserved->split({payloadBytes, 1, 0, 0});

    if (!payload) {
      throw error(payload.error());
    }

    auto arena = SourceArena::create(std::move(*payload));

    if (!arena) {
      throw error(arena.error());
    }

    output = std::shared_ptr<DocumentCopy>(new DocumentCopy());
    output->credit_ = std::move(*reserved);
    output->arena_ = std::move(*arena);
    output->revision_ = metadata->revision;

    auto *data =
      size ? static_cast<std::byte *>(output->arena_->allocate(static_cast<std::size_t>(size), alignof(std::byte)))
           : nullptr;

    output->bytes_ = {data, static_cast<std::size_t>(size)};

    auto offset = range.begin;
    std::size_t copied{};

    while (offset < *range.end) {
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(source->maxPageBytes(), *range.end - offset));
      auto read = source->read({metadata->revision, offset, count, 1}, stop);

      if (!read) {
        throw readFailure(*source, read.error());
      }

      page = std::move(*read);
      pageBytes = count + SourceArena::METADATA_BYTES;

      const auto current = inspectRevision(*source, metadata->revision, stop);

      if (!current) {
        throw current.error();
      }

      validateRange(range, current->byteExtent);

      auto end = offset;

      accepted(end, page->bytes().size());

      if ((metadata->byteExtent && current->byteExtent && metadata->byteExtent != current->byteExtent) ||
          (current->byteExtent && end > *current->byteExtent) ||
          (page->finalEnd() && current->byteExtent && end != *current->byteExtent)) {
        throw error(SourceError::INVALID_REPLY);
      }

      if (page->bytes().empty() || (page->finalEnd() && end < *range.end)) {
        throw error(metadata->byteExtent ? SourceError::INVALID_REPLY : SourceError::INVALID_ARGUMENT);
      }

      std::memcpy(data + copied, page->bytes().data(), page->bytes().size());
      copied += page->bytes().size();
      offset = end;
      page.reset();
      pageBytes = 0;
    }

    auto current = inspectRevision(*source, metadata->revision, stop);

    if (!current) {
      throw current.error();
    }

    validateRange(range, current->byteExtent);

    if (metadata->byteExtent && current->byteExtent && metadata->byteExtent != current->byteExtent) {
      throw error(SourceError::INVALID_REPLY);
    }

    return std::shared_ptr<const DocumentCopy>(std::move(output));
  } catch (const DocumentError &failure) {
    return std::unexpected(qualify(failure));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(SourceError::CALLBACK_FAILED));
  }
}
} // namespace portals
