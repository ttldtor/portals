// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/gzip_document.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace portals {
namespace {
constexpr auto MAX_OFFSET = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t PROVIDER_METADATA_BYTES = 4096;
constexpr std::size_t CHECKPOINT_METADATA_BYTES = 64;

class GzipCategory final : public std::error_category {
public:
  const char *name() const noexcept override {
    return "portals.gzip";
  }

  std::string message(int value) const override {
    switch (static_cast<GzipError>(value)) {
    case GzipError::INVALID_HEADER:
      return "Invalid gzip header";
    case GzipError::CORRUPT_DATA:
      return "Corrupt gzip data";
    case GzipError::CHECKSUM_MISMATCH:
      return "Gzip checksum mismatch";
    case GzipError::SIZE_MISMATCH:
      return "Gzip member size mismatch";
    case GzipError::TRUNCATED:
      return "Truncated gzip member";
    case GzipError::TRAILING_DATA:
      return "Non-gzip trailing data";
    case GzipError::HEADER_LIMIT:
      return "Gzip header exceeds its configured limit";
    case GzipError::INFLATE_LIMIT:
      return "Gzip inflater allocations exceed their configured limit";
    }
    return "Unknown gzip error";
  }
};

DocumentError failure(SourceError code) {
  return {code, {}, {}};
}

DocumentError failure(GzipError cause) {
  const bool limit = cause == GzipError::HEADER_LIMIT || cause == GzipError::INFLATE_LIMIT;
  return {limit ? SourceError::PAGE_TOO_LARGE : SourceError::IO_ERROR, make_error_code(cause), {}};
}

void cancelled(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw failure(SourceError::CANCELLED);
  }
}

std::uint64_t advance(std::uint64_t offset, std::size_t bytes) {
  if (bytes > MAX_OFFSET - offset) {
    throw failure(SourceError::POSITION_EXHAUSTED);
  }
  return offset + bytes;
}

struct AllocationLedger {
  struct alignas(std::max_align_t) Header {
    SourceCredit credit;
    AllocationLedger *ledger{};
    std::size_t bytes{};
    bool optional{};
  };

  std::shared_ptr<SourceBudget> budget;
  std::size_t limit{};
  std::size_t live{};
  std::size_t mandatory{};
  std::size_t fixed{};
  bool optionalAllocation{};
  void *evictionOwner{};
  void (*evict)(void *) noexcept {};
  const void *protectedCheckpoint{};
  std::optional<DocumentError> error;

  static voidpf allocate(voidpf opaque, uInt items, uInt unit) noexcept {
    auto &ledger = *static_cast<AllocationLedger *>(opaque);
    try {
      constexpr auto MAX = std::numeric_limits<std::size_t>::max();
      if (unit && items > (MAX - sizeof(Header)) / unit) {
        ledger.error = failure(GzipError::INFLATE_LIMIT);
        return nullptr;
      }
      const auto amount = sizeof(Header) + static_cast<std::size_t>(items) * unit;
      const auto capacity = ledger.budget->options().maxBytes;
      const auto rootUsage = ledger.budget->usage().bytes;
      if (!ledger.optionalAllocation && ledger.evict &&
          (amount > ledger.limit - ledger.live || amount > capacity - rootUsage)) {
        ledger.evict(ledger.evictionOwner);
      }
      if (amount > ledger.limit - ledger.live) {
        ledger.error = failure(GzipError::INFLATE_LIMIT);
        return nullptr;
      }
      // Own fixed storage and the live active inflater cannot be released by
      // a capacity wake. Distinguish this impossible overlap from other users'
      // temporary root pressure; optional cache states are evicted above.
      const auto pinned = ledger.fixed + ledger.mandatory + SourceBudget::ROOT_METADATA_BYTES;
      if (!ledger.optionalAllocation && (pinned > capacity || amount > capacity - pinned)) {
        ledger.error = failure(SourceError::PAGE_TOO_LARGE);
        ledger.error->budgetDemand = SourceBudgetUsage{amount, 0, 0, 0};
        return nullptr;
      }
      auto credit = ledger.budget->reserve({amount, 0, 0, 0});
      if (!credit) {
        ledger.error = failure(credit.error());
        ledger.error->budgetDemand = SourceBudgetUsage{amount, 0, 0, 0};
        if (!ledger.optionalAllocation) {
          ledger.error->budgetPeak = SourceBudgetUsage{ledger.mandatory + amount, 0, 0, 0};
        }
        return nullptr;
      }
      auto *storage = std::malloc(amount);
      if (!storage) {
        ledger.error = failure(SourceError::NO_MEMORY);
        return nullptr;
      }
      auto *header = ::new (storage) Header{std::move(*credit), &ledger, amount, ledger.optionalAllocation};
      ledger.live += amount;
      if (!header->optional) {
        ledger.mandatory += amount;
      }
      return header + 1;
    } catch (...) {
      ledger.error = failure(SourceError::NO_MEMORY);
      return nullptr;
    }
  }

  static void release(voidpf, voidpf memory) noexcept {
    if (!memory) {
      return;
    }
    auto *header = static_cast<Header *>(memory) - 1;
    auto credit = std::move(header->credit);
    header->ledger->live -= header->bytes;
    if (!header->optional) {
      header->ledger->mandatory -= header->bytes;
    }
    header->~Header();
    std::free(header);
    // The backing block is gone before the independent credit is returned.
  }

  DocumentError lastError() const {
    return error.value_or(failure(SourceError::NO_MEMORY));
  }
};

// zlib's internal state points back to this exact z_stream address. These
// objects are never copied/moved; a checkpoint owns a stable heap node.
struct Inflater {
  z_stream stream{};
  AllocationLedger &ledger;

  explicit Inflater(AllocationLedger &owner) : ledger(owner) {
    configure();
  }

  Inflater(const Inflater &) = delete;
  Inflater &operator=(const Inflater &) = delete;

  ~Inflater() {
    clear();
  }

  void configure() noexcept {
    stream.zalloc = &AllocationLedger::allocate;
    stream.zfree = &AllocationLedger::release;
    stream.opaque = &ledger;
  }

  void clear() noexcept {
    if (stream.state) {
      (void)inflateEnd(&stream);
    }
    stream = {};
    configure();
  }

  void detachOutput() noexcept {
    stream.next_out = nullptr;
    stream.avail_out = 0;
  }

  void initialize() {
    ledger.error.reset();
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
      throw ledger.lastError();
    }
  }
};

struct Restart {
  Inflater inflater;
  std::uint64_t compressed{};
  std::uint64_t logical{};
  std::uint64_t members{};
  bool memberEnded{};

  explicit Restart(AllocationLedger &ledger) : inflater(ledger) {
  }
};
} // namespace

std::error_code make_error_code(GzipError error) noexcept {
  static GzipCategory category;
  return {static_cast<int>(error), category};
}

struct GzipDocumentProvider::State {
  SourceCredit credit; // Payload and zlib states die before the fixed reservation.
  std::shared_ptr<ByteDocumentProvider> compressed;
  AllocationLedger ledger;
  GzipDocumentOptions options;
  std::mutex mutex;
  std::atomic<bool> trimRequested{};
  std::vector<std::byte> input;
  std::vector<std::byte> output;
  std::vector<std::byte> replay;
  Inflater active;
  std::vector<std::unique_ptr<Restart>> checkpoints;
  std::optional<std::uint64_t> revision;
  std::optional<std::uint64_t> physicalExtent;
  std::optional<std::uint64_t> logicalExtent;
  std::optional<DocumentError> terminalError;
  std::uint64_t compressedOffset{};
  std::uint64_t logicalOffset{};
  std::uint64_t members{};
  std::uint64_t nextCheckpoint{};
  std::size_t headerBytes{};
  std::size_t replaySize{};
  std::size_t replayHead{};
  bool physicalEnd{};
  bool headerPending{true};
  bool memberEnded{};
  bool changed{};

  State(SourceCredit reserved, std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget,
        GzipDocumentOptions selected)
      : credit(std::move(reserved)), compressed(std::move(provider)),
        ledger{.budget = std::move(budget), .limit = selected.maxInflateBytes}, options(selected),
        input(selected.compressedBlockBytes), output(selected.inflateOutputBytes), replay(selected.replayBytes),
        active(ledger), checkpoints(0), nextCheckpoint(selected.checkpointStrideBytes) {
    checkpoints.reserve(options.maxCheckpoints);
    ledger.fixed = credit.usage().bytes;
    ledger.evictionOwner = this;
    ledger.evict = [](void *owner) noexcept {
      static_cast<State *>(owner)->evictCheckpoints();
    };
  }

  void evictCheckpoints() noexcept {
    for (auto point = checkpoints.begin(); point != checkpoints.end();) {
      if (point->get() != ledger.protectedCheckpoint) {
        point = checkpoints.erase(point);
      } else {
        ++point;
      }
    }
  }

  bool releaseOptionalStorage() noexcept {
    const bool released = !checkpoints.empty() || active.stream.state;
    checkpoints.clear();
    active.clear();
    // Explicit-offset reads can rebuild the inflater from the compressed source.
    // Replay, logical/member cursors, metadata and errors remain unchanged.
    return released;
  }

  bool tryTrim() noexcept {
    try {
      std::unique_lock lock(mutex, std::try_to_lock);
      if (!lock.owns_lock() || !trimRequested.exchange(false, std::memory_order_acq_rel)) {
        return false;
      }
      return releaseOptionalStorage();
    } catch (...) {
      return false;
    }
  }

  struct OperationFinish {
    State &state;
    std::unique_lock<std::mutex> &lock;

    ~OperationFinish() {
      if (state.trimRequested.exchange(false, std::memory_order_acq_rel)) {
        (void)state.releaseOptionalStorage();
      }
      lock.unlock();
      // Close the request-between-final-check-and-unlock race. If another
      // worker won the mutex, its own finish guard inherits the pending request.
      if (state.trimRequested.load(std::memory_order_acquire)) {
        (void)state.tryTrim();
      }
    }
  };

  void checkSource(std::stop_token stop) {
    cancelled(stop);
    if (changed) {
      throw failure(SourceError::REVISION_CHANGED);
    }
    auto info = compressed->metadata(stop);
    cancelled(stop);
    if (!info) {
      throw info.error();
    }
    if (!info->dataRevision) {
      throw failure(SourceError::INVALID_REPLY);
    }
    if (revision && (*revision != info->dataRevision ||
                     (physicalExtent && info->byteExtent && physicalExtent != info->byteExtent))) {
      changed = true;
      checkpoints.clear();
      active.clear();
      logicalExtent.reset();
      replaySize = 0;
      throw failure(SourceError::REVISION_CHANGED);
    }
    revision = info->dataRevision;
    if (info->byteExtent) {
      physicalExtent = info->byteExtent;
    }
  }

  void refill(std::stop_token stop) {
    auto &stream = active.stream;
    if (stream.avail_in || physicalEnd) {
      return;
    }
    cancelled(stop);
    auto page = compressed->read({*revision, compressedOffset}, input, stop);
    cancelled(stop);
    if (!page) {
      throw page.error();
    }
    if (page->bytesRead > input.size() || (!page->bytesRead && !page->finalEnd)) {
      throw failure(page->bytesRead > input.size() ? SourceError::INVALID_REPLY : SourceError::NO_PROGRESS);
    }
    checkSource(stop);
    const auto end = advance(compressedOffset, page->bytesRead);
    if (physicalExtent && (end > *physicalExtent || (page->finalEnd && end != *physicalExtent))) {
      throw failure(SourceError::INVALID_REPLY);
    }
    stream.next_in = reinterpret_cast<Bytef *>(input.data());
    stream.avail_in = static_cast<uInt>(page->bytesRead);
    physicalEnd = page->finalEnd;
  }

  void appendReplay(std::span<const std::byte> bytes) noexcept {
    if (replay.empty()) {
      return;
    }
    if (bytes.size() >= replay.size()) {
      bytes = bytes.last(replay.size());
      replayHead = replaySize = 0;
    }
    const auto dropped = bytes.size() > replay.size() - replaySize ? bytes.size() - (replay.size() - replaySize) : 0;
    replayHead = (replayHead + dropped) % replay.size();
    replaySize -= dropped;
    const auto index = (replayHead + replaySize) % replay.size();
    const auto first = std::min(bytes.size(), replay.size() - index);
    std::memcpy(replay.data() + index, bytes.data(), first);
    std::memcpy(replay.data(), bytes.data() + first, bytes.size() - first);
    replaySize += bytes.size();
  }

  std::size_t copyReplay(std::uint64_t offset, std::span<std::byte> destination) const noexcept {
    if (replay.empty() || offset >= logicalOffset || logicalOffset - offset > replaySize) {
      return 0;
    }
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), logicalOffset - offset));
    const auto index = (replayHead + replaySize - static_cast<std::size_t>(logicalOffset - offset)) % replay.size();
    const auto first = std::min(count, replay.size() - index);
    std::memcpy(destination.data(), replay.data() + index, first);
    std::memcpy(destination.data() + first, replay.data(), count - first);
    return count;
  }

  const Restart *restartPoint(std::uint64_t target) const noexcept {
    const Restart *selected{};
    for (const auto &point : checkpoints) {
      if (point->logical <= target && (!selected || point->logical > selected->logical)) {
        selected = point.get();
      }
    }
    return selected;
  }

  void restart(std::uint64_t target) {
    const auto *selected = restartPoint(target);
    active.clear();
    ledger.error.reset();
    if (selected) {
      ledger.protectedCheckpoint = selected;
      const auto copied = inflateCopy(&active.stream, const_cast<z_stream *>(&selected->inflater.stream));
      ledger.protectedCheckpoint = nullptr;
      if (copied != Z_OK) {
        // A cached snapshot plus its active copy may not fit even though one
        // inflater does. Release the cache and replay from the original stream.
        active.clear();
        selected = nullptr;
        checkpoints.clear();
        active.initialize();
      }
    } else {
      active.initialize();
    }
    // The deep state owns history/bits/CRC. Unconsumed compressed bytes are
    // reread from the consumed position; no snapshot retains an input buffer.
    active.stream.next_in = nullptr;
    active.stream.avail_in = 0;
    active.detachOutput();
    compressedOffset = selected ? selected->compressed : 0;
    logicalOffset = selected ? selected->logical : 0;
    members = selected ? selected->members : 0;
    memberEnded = selected && selected->memberEnded;
    headerPending = !selected;
    headerBytes = 0;
    physicalEnd = false;
    replaySize = replayHead = 0;
    nextCheckpoint = options.checkpointStrideBytes > MAX_OFFSET - logicalOffset
                       ? MAX_OFFSET
                       : logicalOffset + options.checkpointStrideBytes;
  }

  void checkpoint() noexcept {
    if (!options.maxCheckpoints || headerPending || logicalOffset < nextCheckpoint) {
      return;
    }
    nextCheckpoint = options.checkpointStrideBytes > MAX_OFFSET - logicalOffset
                       ? MAX_OFFSET
                       : logicalOffset + options.checkpointStrideBytes;
    try {
      if (checkpoints.size() == options.maxCheckpoints) {
        checkpoints.erase(checkpoints.begin());
      }
      auto point = std::make_unique<Restart>(ledger);
      ledger.error.reset();
      ledger.optionalAllocation = true;
      const auto copied = inflateCopy(&point->inflater.stream, &active.stream);
      ledger.optionalAllocation = false;
      if (copied != Z_OK) {
        return;
      }
      point->inflater.stream.next_in = nullptr;
      point->inflater.stream.avail_in = 0;
      point->inflater.detachOutput();
      point->compressed = compressedOffset;
      point->logical = logicalOffset;
      point->members = members;
      point->memberEnded = memberEnded;
      checkpoints.push_back(std::move(point));
    } catch (...) {
      // A cache admission failure does not invalidate the live inflater.
    }
  }

  [[noreturn]] void corrupt(int result) {
    const std::string_view message = active.stream.msg ? active.stream.msg : "";
    auto cause = GzipError::CORRUPT_DATA;
    if (message == "incorrect data check" || message == "header crc mismatch") {
      cause = GzipError::CHECKSUM_MISMATCH;
    } else if (message == "incorrect length check") {
      cause = GzipError::SIZE_MISMATCH;
    } else if (headerPending) {
      cause = members ? GzipError::TRAILING_DATA : GzipError::INVALID_HEADER;
    }
    if (result == Z_MEM_ERROR) {
      const auto error = ledger.lastError();
      active.clear();
      throw error;
    }
    terminalError = failure(cause);
    throw *terminalError;
  }

  ByteReadResult read(std::uint64_t target, std::span<std::byte> destination, std::stop_token stop) {
    if (terminalError) {
      throw *terminalError;
    }
    if (logicalExtent && target >= *logicalExtent) {
      return {0, true};
    }
    if (const auto count = copyReplay(target, destination)) {
      return {count, logicalExtent && target + count == *logicalExtent};
    }
    const auto *point = restartPoint(target);
    if (!active.stream.state || target < logicalOffset || (point && point->logical > logicalOffset)) {
      restart(target);
    }
    std::size_t copied{};
    while (copied < destination.size()) {
      cancelled(stop);
      refill(stop);
      auto &stream = active.stream;
      if (memberEnded) {
        if (!stream.avail_in && physicalEnd) {
          logicalExtent = logicalOffset;
          return {copied, true};
        }
        if (inflateReset2(&stream, MAX_WBITS + 16) != Z_OK) {
          throw failure(SourceError::CALLBACK_FAILED);
        }
        memberEnded = false;
        headerPending = true;
        headerBytes = 0;
      }
      const auto before = stream.avail_in;
      auto admitted = before;
      if (headerPending) {
        if (headerBytes == options.maxHeaderBytes) {
          terminalError = failure(GzipError::HEADER_LIMIT);
          throw *terminalError;
        }
        admitted = static_cast<uInt>(std::min<std::size_t>(before, options.maxHeaderBytes - headerBytes));
        stream.avail_in = admitted;
      }
      const auto maximum = logicalOffset < target ? std::min<std::uint64_t>(output.size(), target - logicalOffset)
                                                  : std::min(output.size(), destination.size() - copied);
      stream.next_out = reinterpret_cast<Bytef *>(output.data());
      stream.avail_out = static_cast<uInt>(maximum);
      ledger.error.reset();
      const auto result = inflate(&stream, headerPending ? Z_BLOCK : Z_NO_FLUSH);
      const auto consumed = static_cast<std::size_t>(admitted - stream.avail_in);
      stream.avail_in += before - admitted;
      const auto produced = static_cast<std::size_t>(maximum - stream.avail_out);
      active.detachOutput();
      compressedOffset = advance(compressedOffset, consumed);
      const auto first = logicalOffset;
      logicalOffset = advance(logicalOffset, produced);
      if (headerPending) {
        if (consumed > options.maxHeaderBytes - std::min(headerBytes, options.maxHeaderBytes)) {
          terminalError = failure(GzipError::HEADER_LIMIT);
          throw *terminalError;
        }
        headerBytes += consumed;
        if (stream.data_type & 128) {
          headerPending = false;
        }
      }
      if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END) {
        corrupt(result);
      }
      appendReplay(std::span(output).first(produced));
      if (first >= target && produced) {
        std::memcpy(destination.data() + copied, output.data(), produced);
        copied += produced;
      }
      if (result == Z_STREAM_END) {
        members = advance(members, 1);
        memberEnded = true;
      }
      cancelled(stop);
      checkpoint();
      if (!consumed && !produced && result != Z_STREAM_END && !(stream.data_type & 128)) {
        if (!stream.avail_in && physicalEnd) {
          terminalError = failure(!members && !compressedOffset ? GzipError::INVALID_HEADER : GzipError::TRUNCATED);
          throw *terminalError;
        }
        throw failure(SourceError::NO_PROGRESS);
      }
    }
    return {copied, false};
  }
};

GzipDocumentProvider::GzipDocumentProvider(std::unique_ptr<State> state) : state_(std::move(state)) {
}

GzipDocumentProvider::~GzipDocumentProvider() = default;

std::expected<std::shared_ptr<GzipDocumentProvider>, DocumentError>
GzipDocumentProvider::create(std::shared_ptr<ByteDocumentProvider> compressed, std::shared_ptr<SourceBudget> budget,
                             GzipDocumentOptions options) {
  constexpr auto SAFE_LIMIT = std::numeric_limits<std::size_t>::max() / 1024;
  if (!compressed || !budget || !options.compressedBlockBytes || options.compressedBlockBytes > 64 * 1024 ||
      !options.inflateOutputBytes || options.inflateOutputBytes > std::numeric_limits<uInt>::max() ||
      options.inflateOutputBytes > SAFE_LIMIT || !options.checkpointStrideBytes || !options.maxInflateBytes ||
      options.maxInflateBytes > SAFE_LIMIT || !options.maxHeaderBytes || options.maxHeaderBytes > SAFE_LIMIT ||
      options.replayBytes > SAFE_LIMIT || options.maxCheckpoints > GzipDocumentOptions::MAX_CHECKPOINTS) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }
  if (!compressed->capabilities().seekable) {
    return std::unexpected(failure(SourceError::UNSUPPORTED));
  }
  const auto amount =
    PROVIDER_METADATA_BYTES + sizeof(State) + options.compressedBlockBytes + options.inflateOutputBytes +
    options.replayBytes +
    options.maxCheckpoints * (sizeof(Restart) + sizeof(std::unique_ptr<Restart>) + CHECKPOINT_METADATA_BYTES);
  auto credit = budget->reserve({amount, 0, 0, 0});
  if (!credit) {
    auto error = failure(credit.error());
    error.budgetDemand = SourceBudgetUsage{amount, 0, 0, 0};
    return std::unexpected(error);
  }
  try {
    auto state = std::make_unique<State>(std::move(*credit), std::move(compressed), std::move(budget), options);
    return std::shared_ptr<GzipDocumentProvider>(new GzipDocumentProvider(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}

ByteDocumentCapabilities GzipDocumentProvider::capabilities() const noexcept {
  return {true, true};
}

std::size_t GzipDocumentProvider::pinnedBudgetBytes(const SourceBudget &root) const noexcept {
  return state_->ledger.budget.get() == &root ? state_->ledger.fixed : 0;
}

bool GzipDocumentProvider::tryReleaseOptionalStorage() noexcept {
  state_->trimRequested.store(true, std::memory_order_release);
  return state_->tryTrim();
}

std::expected<ByteDocumentMetadata, DocumentError> GzipDocumentProvider::metadata(std::stop_token stop) {
  try {
    cancelled(stop);
    std::unique_lock lock(state_->mutex);
    State::OperationFinish finish{*state_, lock};
    state_->checkSource(stop);
    return ByteDocumentMetadata{1, state_->logicalExtent};
  } catch (const DocumentError &error) {
    return std::unexpected(error);
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}

std::expected<ByteDocumentMetadata, DocumentError> GzipDocumentProvider::discoverExtent(std::stop_token stop) {
  try {
    cancelled(stop);
    std::unique_lock lock(state_->mutex);
    State::OperationFinish finish{*state_, lock};
    state_->checkSource(stop);
    // All representable logical output precedes this private discard target.
    // The same checked counters, member validation and cancellation loop apply.
    std::array<std::byte, 1> discard{};
    const auto result = state_->read(MAX_OFFSET, discard, stop);
    if (!result.finalEnd || !state_->logicalExtent) {
      throw failure(SourceError::CALLBACK_FAILED);
    }
    state_->checkSource(stop);
    return ByteDocumentMetadata{1, state_->logicalExtent};
  } catch (const DocumentError &error) {
    return std::unexpected(error);
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}

std::expected<ByteReadResult, DocumentError>
GzipDocumentProvider::read(const ByteReadCommand &command, std::span<std::byte> destination, std::stop_token stop) {
  try {
    cancelled(stop);
    std::unique_lock lock(state_->mutex);
    State::OperationFinish finish{*state_, lock};
    state_->checkSource(stop);
    if (command.dataRevision != 1) {
      throw failure(SourceError::REVISION_CHANGED);
    }
    if (destination.empty()) {
      throw failure(SourceError::INVALID_ARGUMENT);
    }
    (void)advance(command.offset, destination.size());
    const auto result = state_->read(command.offset, destination, stop);
    state_->checkSource(stop);
    return result;
  } catch (const DocumentError &error) {
    return std::unexpected(error);
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}
} // namespace portals
