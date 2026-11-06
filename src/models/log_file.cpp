// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/log_file.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
// Windows SDK partition definitions must precede CryptoAPI declarations.
#  include <windows.h>

#  include <wincrypt.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  if defined(__linux__) && !defined(__EMSCRIPTEN__)
#    include <monocypher.h>
#  endif
#endif

namespace portals {
namespace {
constexpr std::size_t MAX_BLOCK_BYTES = 64 * 1024;
constexpr std::size_t MAX_PATTERN_BYTES = 4096;
constexpr std::size_t MAX_BOOKMARK_WITNESS_BYTES = 1024;
constexpr std::size_t BOOKMARK_FINGERPRINT_BYTES = 32;
constexpr std::uint64_t FILE_BOOKMARK_FORMAT = 0x504C4F47424D3032;
#if defined(_WIN32)
constexpr std::uint64_t BOOKMARK_FINGERPRINT_ALGORITHM = 1; // SHA-256 through the existing system dependency.
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
constexpr std::uint64_t BOOKMARK_FINGERPRINT_ALGORITHM = 2; // BLAKE2b-256 through the existing vendored dependency.
#else
constexpr std::uint64_t BOOKMARK_FINGERPRINT_ALGORITHM = 0;
#endif

using BookmarkFingerprint = std::array<std::byte, BOOKMARK_FINGERPRINT_BYTES>;

std::expected<BookmarkFingerprint, SourceError> bookmarkFingerprint(std::span<const std::byte> prefix) noexcept {
  BookmarkFingerprint fingerprint{};

#if defined(_WIN32)
  DWORD bytes = static_cast<DWORD>(fingerprint.size());
  const BYTE empty{};
  const auto *input = prefix.empty() ? &empty : reinterpret_cast<const BYTE *>(prefix.data());

  if (!CryptHashCertificate2(L"SHA256", 0, nullptr, input, static_cast<DWORD>(prefix.size()),
                             reinterpret_cast<BYTE *>(fingerprint.data()), &bytes) ||
      bytes != fingerprint.size()) {
    return std::unexpected(SourceError::IO_ERROR);
  }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
  crypto_blake2b(reinterpret_cast<std::uint8_t *>(fingerprint.data()), fingerprint.size(),
                 reinterpret_cast<const std::uint8_t *>(prefix.data()), prefix.size());
#else
  (void)prefix;

  return std::unexpected(SourceError::UNSUPPORTED);
#endif

  return fingerprint;
}

struct FileBookmarkMetadata {
  std::uint64_t format{};
  SourceStamp instance;
  std::uint64_t identityHigh{};
  std::uint64_t identityLow{};
  std::uint64_t generation{};
  std::uint64_t extent{};
  std::uint64_t offset{};
  std::uint64_t recordBytes{};
  std::uint64_t witnessBytes{};
  std::uint64_t terminated{};
  std::uint64_t fingerprintAlgorithm{};
  BookmarkFingerprint fingerprint{};
};

FileLogFailure failure(FileLogError error, FileLogOperation operation = FileLogOperation::READ,
                       std::uint32_t nativeCode = 0) {
  return {error, nativeCode, operation};
}

struct FileInfo {
  std::uint64_t identityHigh{};
  std::uint64_t identityLow{};
  std::uint64_t byteExtent{};
  std::uint64_t modified{};
};

class NativeFile {
public:
  NativeFile() = default;
  NativeFile(const NativeFile &) = delete;
  NativeFile &operator=(const NativeFile &) = delete;

  NativeFile(NativeFile &&other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE)) {
  }

  ~NativeFile() {
    if (handle_ != INVALID_HANDLE) {
#ifdef _WIN32
      CloseHandle(handle_);
#else
      ::close(handle_);
#endif
    }
  }

  static std::expected<NativeFile, FileLogFailure> open(const std::filesystem::path &path, std::stop_token stop) {
    if (stop.stop_requested()) {
      return std::unexpected(failure(FileLogError::CANCELLED, FileLogOperation::OPEN));
    }

    NativeFile result;
#ifdef _WIN32
    result.handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    result.handle_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
#endif

    if (result.handle_ == INVALID_HANDLE) {
      return std::unexpected(failure(FileLogError::OPEN_FAILED, FileLogOperation::OPEN, nativeError()));
    }

#ifdef _WIN32
    if (GetFileType(result.handle_) != FILE_TYPE_DISK) {
      return std::unexpected(failure(FileLogError::OPEN_FAILED, FileLogOperation::OPEN, ERROR_NOT_SUPPORTED));
    }
#endif

    return result;
  }

  std::expected<FileInfo, FileLogFailure> info() const {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};

    if (!GetFileInformationByHandle(handle_, &info)) {
      return std::unexpected(failure(FileLogError::METADATA_FAILED, FileLogOperation::METADATA, nativeError()));
    }

    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      return std::unexpected(failure(FileLogError::OPEN_FAILED, FileLogOperation::OPEN, ERROR_DIRECTORY));
    }

    return FileInfo{info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
                    (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
                    (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime};
#else
    struct stat info{};

    if (::fstat(handle_, &info)) {
      return std::unexpected(failure(FileLogError::METADATA_FAILED, FileLogOperation::METADATA, nativeError()));
    }

    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
      return std::unexpected(failure(FileLogError::OPEN_FAILED, FileLogOperation::OPEN, EINVAL));
    }

#  ifdef __APPLE__
    auto modified = std::uint64_t(info.st_mtimespec.tv_sec) * 1000000000 + info.st_mtimespec.tv_nsec;
#  else
    auto modified = std::uint64_t(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
#  endif

    return FileInfo{std::uint64_t(info.st_dev), std::uint64_t(info.st_ino), std::uint64_t(info.st_size), modified};
#endif
  }

  std::expected<std::size_t, FileLogFailure> read(std::uint64_t offset, std::span<std::byte> destination,
                                                  std::stop_token stop) const {
    if (stop.stop_requested()) {
      return std::unexpected(failure(FileLogError::CANCELLED));
    }

    if (offset > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
      return std::unexpected(failure(FileLogError::INVALID_ARGUMENT));
    }

#ifdef _WIN32
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);

    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
      return std::unexpected(failure(FileLogError::READ_FAILED, FileLogOperation::READ, nativeError()));
    }

    DWORD count{};

    if (!ReadFile(handle_, destination.data(), static_cast<DWORD>(destination.size()), &count, nullptr)) {
      return std::unexpected(failure(FileLogError::READ_FAILED, FileLogOperation::READ, nativeError()));
    }

    return count;
#else
    auto count = ::pread(handle_, destination.data(), destination.size(), static_cast<off_t>(offset));

    if (count < 0) {
      return std::unexpected(failure(FileLogError::READ_FAILED, FileLogOperation::READ, nativeError()));
    }

    return static_cast<std::size_t>(count);
#endif
  }

private:
  static std::uint32_t nativeError() {
#ifdef _WIN32
    return GetLastError();
#else
    return static_cast<std::uint32_t>(errno);
#endif
  }

#ifdef _WIN32
  static inline const HANDLE INVALID_HANDLE = INVALID_HANDLE_VALUE;
  HANDLE handle_{INVALID_HANDLE};
#else
  static constexpr int INVALID_HANDLE = -1;
  int handle_{INVALID_HANDLE};
#endif
};

/** One operation owns this cache and handle; no cache or mutex is shared with UI. */
class BlockReader {
public:
  BlockReader(const NativeFile &file, std::uint64_t extent, std::span<std::byte> cache, std::stop_token stop)
      : file_(file), extent_(extent), cache_(cache), stop_(stop) {
  }

  std::expected<unsigned char, FileLogFailure> byte(std::uint64_t position) {
    if (stop_.stop_requested()) {
      return std::unexpected(failure(FileLogError::CANCELLED));
    }

    if (position >= extent_) {
      return std::unexpected(failure(FileLogError::FILE_CHANGED));
    }

    if (!count_ || position < base_ || position - base_ >= count_) {
      base_ = position - position % cache_.size();
      auto count = file_.read(
        base_, cache_.first(static_cast<std::size_t>(std::min<std::uint64_t>(cache_.size(), extent_ - base_))), stop_);

      if (!count) {
        return std::unexpected(count.error());
      }

      count_ = *count;

      if (position - base_ >= count_) {
        return std::unexpected(failure(FileLogError::FILE_CHANGED));
      }
    }

    return std::to_integer<unsigned char>(cache_[static_cast<std::size_t>(position - base_)]);
  }

  std::uint64_t extent() const noexcept {
    return extent_;
  }

private:
  const NativeFile &file_;
  std::uint64_t extent_{};
  std::span<std::byte> cache_;
  std::stop_token stop_;
  std::uint64_t base_{};
  std::size_t count_{};
};

enum class Utf8State {
  VALID,
  INVALID,
  INCOMPLETE
};

Utf8State utf8State(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    auto first = static_cast<unsigned char>(text[i++]);

    if (first < 0x80) {
      continue;
    }

    std::size_t remaining{};
    std::uint32_t value{};
    std::uint32_t minimum{};

    if (first >= 0xc2 && first <= 0xdf) {
      remaining = 1;
      value = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      remaining = 2;
      value = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      remaining = 3;
      value = first & 0x07;
      minimum = 0x10000;
    } else {
      return Utf8State::INVALID;
    }

    if (remaining > text.size() - i) {
      return Utf8State::INCOMPLETE;
    }

    while (remaining--) {
      auto next = static_cast<unsigned char>(text[i++]);

      if ((next & 0xc0) != 0x80) {
        return Utf8State::INVALID;
      }

      value = (value << 6) | (next & 0x3f);
    }

    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
      return Utf8State::INVALID;
    }
  }

  return Utf8State::VALID;
}

std::expected<bool, FileLogFailure> isBoundary(BlockReader &reader, std::uint64_t gap) {
  if (!gap || gap == reader.extent()) {
    return true;
  }

  if (gap > reader.extent()) {
    return false;
  }

  auto previous = reader.byte(gap - 1);

  if (!previous) {
    return std::unexpected(previous.error());
  }

  if (*previous == '\n') {
    return true;
  }

  if (*previous != '\r') {
    return false;
  }

  auto next = reader.byte(gap);

  if (!next) {
    return std::unexpected(next.error());
  }

  return *next != '\n';
}

std::expected<std::uint64_t, FileLogFailure> previousStart(BlockReader &reader, std::uint64_t gap,
                                                           std::size_t maxRecordBytes) {
  auto position = gap;

  if (!position) {
    return position;
  }

  auto last = reader.byte(position - 1);

  if (!last) {
    return std::unexpected(last.error());
  }

  if (*last == '\n' || *last == '\r') {
    --position;

    if (*last == '\n' && position) {
      auto previous = reader.byte(position - 1);

      if (!previous) {
        return std::unexpected(previous.error());
      }

      if (*previous == '\r') {
        --position;
      }
    }
  }

  std::size_t bytes{};

  while (position) {
    auto value = reader.byte(position - 1);

    if (!value) {
      return std::unexpected(value.error());
    }

    if (*value == '\r' || *value == '\n') {
      break;
    }

    if (++bytes > maxRecordBytes) {
      return std::unexpected(failure(FileLogError::RECORD_TOO_LONG));
    }

    --position;
  }

  return position;
}

std::expected<std::uint64_t, FileLogFailure> readRecord(BlockReader &reader, std::uint64_t position,
                                                        std::pmr::string &text, std::size_t maxRecordBytes,
                                                        bool &incompleteTail) {
  text.clear();
  bool terminated{};
  incompleteTail = false;

  while (position < reader.extent()) {
    auto value = reader.byte(position++);

    if (!value) {
      return std::unexpected(value.error());
    }

    if (*value == '\r' || *value == '\n') {
      terminated = true;
      if (*value == '\r' && position < reader.extent()) {
        auto next = reader.byte(position);

        if (!next) {
          return std::unexpected(next.error());
        }

        if (*next == '\n') {
          ++position;
        }
      }

      break;
    }

    if (text.size() == maxRecordBytes) {
      return std::unexpected(failure(FileLogError::RECORD_TOO_LONG));
    }

    text.push_back(static_cast<char>(*value));
  }

  auto state = utf8State(text);

  if (state == Utf8State::INCOMPLETE && !terminated) {
    incompleteTail = true;
  } else if (state != Utf8State::VALID) {
    return std::unexpected(failure(FileLogError::INVALID_UTF8));
  }

  return position;
}

SourceError sourceError(FileLogFailure error) {
  switch (error.error) {
  case FileLogError::CANCELLED:
    return SourceError::CANCELLED;
  case FileLogError::NO_MEMORY:
    return SourceError::NO_MEMORY;
  case FileLogError::RECORD_TOO_LONG:
    return SourceError::ROW_TOO_LARGE;
  case FileLogError::FILE_CHANGED:
    return SourceError::REVISION_CHANGED;
  case FileLogError::INVALID_ARGUMENT:
    return SourceError::INVALID_ARGUMENT;
  default:
    return SourceError::IO_ERROR;
  }
}
} // namespace

LogLine::LogLine(std::uint64_t offset, std::string_view utf8, std::pmr::memory_resource *resource)
    : byteOffset(offset), text(utf8, resource) {
}

FileLogModel::FileLogModel(std::filesystem::path path, FileLogOptions options, ModelResource resource,
                           SourceStamp instance)
    : path_(std::move(path)), options_(options), resource_(resource), instance_(instance) {
}

std::expected<std::shared_ptr<FileLogModel>, FileLogFailure>
FileLogModel::create(std::filesystem::path path, FileLogOptions options, ModelResource continuation) {
  if (path.empty() || !options.maxRecordBytes || options.maxRecordBytes > MAX_BLOCK_BYTES || !options.ioBlockBytes ||
      options.ioBlockBytes > MAX_BLOCK_BYTES || !options.maxSearchBytes || !options.maxSearchPatternBytes ||
      options.maxSearchPatternBytes > MAX_PATTERN_BYTES || options.maxSearchPatternBytes > options.maxSearchBytes) {
    return std::unexpected(failure(FileLogError::INVALID_ARGUMENT, FileLogOperation::CREATE));
  }

  try {
    auto issued = ModelResource::create();

    if (!issued) {
      return std::unexpected(
        failure(issued.error() == SourceError::NO_MEMORY ? FileLogError::NO_MEMORY : FileLogError::INVALID_ARGUMENT,
                FileLogOperation::CREATE));
    }

    const bool continuing = continuation.identity.instance || continuation.identity.epoch || continuation.lineage;

    if (continuing && (!continuation.identity.instance || !continuation.identity.epoch || !continuation.lineage)) {
      return std::unexpected(failure(FileLogError::INVALID_ARGUMENT, FileLogOperation::CREATE));
    }

    return std::shared_ptr<FileLogModel>(
      new FileLogModel(std::move(path), options, continuing ? continuation : *issued, issued->identity));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(FileLogError::NO_MEMORY, FileLogOperation::CREATE));
  }
}

SourceCapabilities FileLogModel::capabilities() const noexcept {
  return {true, true, true};
}

FileLogOptions FileLogModel::options() const noexcept {
  return options_;
}

ModelResource FileLogModel::resource() const noexcept {
  return resource_;
}

std::expected<ModelBookmark, SourceError> FileLogModel::capture(const SourceReadCommand &command,
                                                                const WindowRow<LogLine> &row, BookmarkOptions options,
                                                                std::stop_token stop) const {
  if (stop.stop_requested()) {
    return std::unexpected(SourceError::CANCELLED);
  }
  if (!command.source.instance || !command.source.epoch || !command.dataRevision || !row.id.value ||
      !row.valueRevision || row.value.byteOffset != row.id.value - 1 ||
      row.value.text.size() > options_.maxRecordBytes) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  FileBookmarkMetadata metadata;

  {
    std::lock_guard lock(mutex_);

    if (!generation_ || row.id.generation != generation_) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    metadata.identityHigh = observed_.identityHigh;
    metadata.identityLow = observed_.identityLow;
  }

  metadata.format = FILE_BOOKMARK_FORMAT;
  metadata.instance = instance_;
  metadata.generation = row.id.generation;
  metadata.extent = row.valueRevision - 1;
  metadata.offset = row.value.byteOffset;
  metadata.recordBytes = row.value.text.size();
  metadata.witnessBytes = std::min(row.value.text.size(), MAX_BOOKMARK_WITNESS_BYTES);

  if (metadata.offset > metadata.extent || metadata.recordBytes > metadata.extent - metadata.offset) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  metadata.terminated = metadata.recordBytes < metadata.extent - metadata.offset;
  auto fingerprint = bookmarkFingerprint(
    std::as_bytes(std::span<const char>(row.value.text.data(), static_cast<std::size_t>(metadata.witnessBytes))));

  if (!fingerprint) {
    return std::unexpected(fingerprint.error());
  }

  metadata.fingerprintAlgorithm = BOOKMARK_FINGERPRINT_ALGORITHM;
  metadata.fingerprint = *fingerprint;
  std::array<std::byte, sizeof(FileBookmarkMetadata)> bytes{};

  std::memcpy(bytes.data(), &metadata, sizeof(metadata));

  return ModelBookmark::create(resource_, {PositionKind::ITEM, row.id, metadata.offset, command.dataRevision}, bytes,
                               options);
}

std::expected<PositionReply, SourceError> FileLogModel::resolve(const PositionRequest &request,
                                                                std::stop_token stop) const {
  PositionReply reply{request.bookmark.resource(), request.session, request.revision, request.sequence};

  if (!request.session.instance || !request.session.epoch || !request.revision || !request.sequence) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }
  if (stop.stop_requested()) {
    return std::unexpected(SourceError::CANCELLED);
  }
  if (request.bookmark.resource() != resource_) {
    reply.state = PositionResolution::INCOMPATIBLE;

    return reply;
  }

  const auto bytes = request.bookmark.metadata();
  FileBookmarkMetadata saved;

  if (bytes.size() < sizeof(saved)) {
    return std::unexpected(SourceError::INVALID_CURSOR);
  }

  std::memcpy(&saved, bytes.data(), sizeof(saved));

  if (saved.format != FILE_BOOKMARK_FORMAT || saved.witnessBytes > MAX_BOOKMARK_WITNESS_BYTES ||
      saved.witnessBytes > saved.recordBytes || bytes.size() != sizeof(saved) || saved.offset > saved.extent ||
      saved.recordBytes > saved.extent - saved.offset || saved.terminated > 1 ||
      request.bookmark.position().kind != PositionKind::ITEM ||
      request.bookmark.position().row != RowId{saved.offset + 1, saved.generation}) {
    return std::unexpected(SourceError::INVALID_CURSOR);
  }

  if (!BOOKMARK_FINGERPRINT_ALGORITHM || saved.fingerprintAlgorithm != BOOKMARK_FINGERPRINT_ALGORITHM) {
    reply.state = PositionResolution::UNSUPPORTED;

    return reply;
  }

  auto operation = beginOperation(stop);

  if (!operation) {
    return std::unexpected(sourceError(operation.error()));
  }

  auto file = NativeFile::open(path_, stop);

  if (!file) {
    return std::unexpected(sourceError(file.error()));
  }

  auto info = file->info();

  if (!info) {
    return std::unexpected(sourceError(info.error()));
  }

  auto generation =
    observe({info->identityHigh, info->identityLow, info->byteExtent, info->modified}, *operation, stop);

  if (!generation) {
    return std::unexpected(sourceError(generation.error()));
  }
  if (info->identityHigh != saved.identityHigh || info->identityLow != saved.identityLow) {
    reply.state = PositionResolution::INCOMPATIBLE;

    return reply;
  }
  if ((saved.instance == instance_ ? saved.generation != *generation : *generation > 1) ||
      info->byteExtent < saved.extent || saved.offset >= info->byteExtent) {
    reply.state = PositionResolution::UNAVAILABLE;

    return reply;
  }

  std::array<std::byte, MAX_BOOKMARK_WITNESS_BYTES> witness{};
  auto read = file->read(saved.offset, std::span<std::byte>(witness.data(), saved.witnessBytes), stop);

  if (!read) {
    return std::unexpected(sourceError(read.error()));
  }
  if (*read != saved.witnessBytes) {
    return std::unexpected(SourceError::INVALID_CURSOR);
  }

  const auto fingerprint = bookmarkFingerprint(std::span<const std::byte>(witness.data(), *read));

  if (!fingerprint) {
    return std::unexpected(fingerprint.error());
  }
  if (*fingerprint != saved.fingerprint) {
    return std::unexpected(SourceError::INVALID_CURSOR);
  }

  const auto byteAt = [&](std::uint64_t offset) -> std::expected<unsigned char, SourceError> {
    std::byte byte{};
    auto result = file->read(offset, std::span<std::byte>(&byte, 1), stop);

    if (!result) {
      return std::unexpected(sourceError(result.error()));
    }
    if (*result != 1) {
      return std::unexpected(SourceError::INVALID_CURSOR);
    }

    return std::to_integer<unsigned char>(byte);
  };

  if (saved.offset) {
    auto previous = byteAt(saved.offset - 1);
    auto first = byteAt(saved.offset);

    if (!previous || !first) {
      return std::unexpected(previous ? first.error() : previous.error());
    }
    if (*previous != '\n' && (*previous != '\r' || *first == '\n')) {
      return std::unexpected(SourceError::INVALID_CURSOR);
    }
  }
  if (saved.terminated) {
    auto separator = byteAt(saved.offset + saved.recordBytes);

    if (!separator) {
      return std::unexpected(separator.error());
    }
    if (*separator != '\r' && *separator != '\n') {
      return std::unexpected(SourceError::INVALID_CURSOR);
    }
  }

  auto latest = file->info();

  if (!latest) {
    return std::unexpected(sourceError(latest.error()));
  }
  if (latest->byteExtent < info->byteExtent ||
      (latest->byteExtent == info->byteExtent && latest->modified != info->modified)) {
    reply.state = PositionResolution::UNAVAILABLE;

    return reply;
  }
  if (stop.stop_requested()) {
    return std::unexpected(SourceError::CANCELLED);
  }

  reply.state = PositionResolution::EXACT;
  reply.position = request.bookmark.position();
  reply.position.row = {saved.offset + 1, *generation};
  reply.position.revision = request.revision;

  return reply;
}

std::expected<std::uint64_t, FileLogFailure> FileLogModel::beginOperation(std::stop_token stop) const {
  std::lock_guard lock(mutex_);

  if (stop.stop_requested()) {
    return std::unexpected(failure(FileLogError::CANCELLED));
  }

  if (operationSequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(failure(FileLogError::FILE_CHANGED, FileLogOperation::METADATA));
  }

  return ++operationSequence_;
}

std::expected<std::uint64_t, FileLogFailure> FileLogModel::observe(Observation observation, std::uint64_t operation,
                                                                   std::stop_token stop) const {
  std::lock_guard lock(mutex_);

  if (stop.stop_requested()) {
    return std::unexpected(failure(FileLogError::CANCELLED));
  }

  // An older operation may still hold the pre-rotation handle. It must not replace the newer observation.
  if (operation < observedOperation_ &&
      (observed_.identityHigh != observation.identityHigh || observed_.identityLow != observation.identityLow ||
       observed_.byteExtent != observation.byteExtent || observed_.modified != observation.modified)) {
    return std::unexpected(failure(FileLogError::FILE_CHANGED, FileLogOperation::METADATA));
  }

  bool changed =
    generation_ && (observed_.identityHigh != observation.identityHigh ||
                    observed_.identityLow != observation.identityLow || observation.byteExtent < observed_.byteExtent ||
                    (observation.byteExtent == observed_.byteExtent && observation.modified != observed_.modified));

  if (!generation_ || changed) {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(failure(FileLogError::FILE_CHANGED, FileLogOperation::METADATA));
    }

    ++generation_;
  }

  observed_ = observation;
  observedOperation_ = std::max(operation, observedOperation_);

  return generation_;
}

void FileLogModel::recordFailure(FileLogFailure failure) noexcept {
  std::lock_guard lock(mutex_);
  failure_ = failure;
}

std::optional<FileLogFailure> FileLogModel::lastFailure() const noexcept {
  std::lock_guard lock(mutex_);

  return failure_;
}

std::expected<FileLogMetadata, FileLogFailure> FileLogModel::metadata(std::stop_token stop) {
  auto operation = beginOperation(stop);

  if (!operation) {
    return std::unexpected(operation.error());
  }

  auto file = NativeFile::open(path_, stop);

  if (!file) {
    return std::unexpected(file.error());
  }

  auto info = file->info();

  if (!info) {
    return std::unexpected(info.error());
  }

  auto generation =
    observe({info->identityHigh, info->identityLow, info->byteExtent, info->modified}, *operation, stop);

  if (!generation) {
    return std::unexpected(generation.error());
  }

  return FileLogMetadata{info->byteExtent, *generation};
}

std::expected<std::size_t, FileLogFailure>
FileLogModel::readBytes(std::uint64_t byteOffset, std::span<std::byte> destination, std::stop_token stop) {
  if (destination.size() > options_.ioBlockBytes) {
    return std::unexpected(failure(FileLogError::INVALID_ARGUMENT));
  }

  auto operation = beginOperation(stop);

  if (!operation) {
    return std::unexpected(operation.error());
  }

  auto file = NativeFile::open(path_, stop);

  if (!file) {
    return std::unexpected(file.error());
  }

  auto info = file->info();

  if (!info) {
    return std::unexpected(info.error());
  }

  auto generation =
    observe({info->identityHigh, info->identityLow, info->byteExtent, info->modified}, *operation, stop);

  if (!generation) {
    return std::unexpected(generation.error());
  }

  if (byteOffset > info->byteExtent) {
    return std::unexpected(failure(FileLogError::INVALID_ARGUMENT));
  }

  auto count = static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), info->byteExtent - byteOffset));

  return file->read(byteOffset, destination.first(count), stop);
}

std::expected<FileLogSearchResult, FileLogFailure> FileLogModel::findLiteral(std::string_view pattern,
                                                                             std::uint64_t byteOffset,
                                                                             std::size_t maxScanBytes,
                                                                             std::stop_token stop) {
  if (pattern.empty() || pattern.size() > options_.maxSearchPatternBytes || maxScanBytes < pattern.size() ||
      maxScanBytes > options_.maxSearchBytes) {
    return std::unexpected(failure(FileLogError::INVALID_ARGUMENT, FileLogOperation::SEARCH));
  }

  auto operation = beginOperation(stop);

  if (!operation) {
    return std::unexpected(operation.error());
  }

  auto file = NativeFile::open(path_, stop);

  if (!file) {
    return std::unexpected(file.error());
  }

  auto info = file->info();

  if (!info) {
    return std::unexpected(info.error());
  }

  auto generation =
    observe({info->identityHigh, info->identityLow, info->byteExtent, info->modified}, *operation, stop);

  if (!generation) {
    return std::unexpected(generation.error());
  }

  if (byteOffset > info->byteExtent) {
    return std::unexpected(failure(FileLogError::INVALID_ARGUMENT, FileLogOperation::SEARCH));
  }

  std::array<std::byte, MAX_BLOCK_BYTES> cache;
  std::array<std::size_t, MAX_PATTERN_BYTES> prefix{};
  BlockReader reader(*file, info->byteExtent, std::span(cache).first(options_.ioBlockBytes), stop);

  for (std::size_t i = 1, matched = 0; i < pattern.size(); ++i) {
    while (matched && pattern[i] != pattern[matched]) {
      matched = prefix[matched - 1];
    }

    if (pattern[i] == pattern[matched]) {
      ++matched;
    }

    prefix[i] = matched;
  }

  auto end = byteOffset + std::min<std::uint64_t>(maxScanBytes, info->byteExtent - byteOffset);
  std::size_t matched{};

  for (auto position = byteOffset; position < end; ++position) {
    auto value = reader.byte(position);

    if (!value) {
      return std::unexpected(value.error());
    }

    auto character = static_cast<char>(*value);

    while (matched && character != pattern[matched]) {
      matched = prefix[matched - 1];
    }

    if (character == pattern[matched]) {
      ++matched;
    }

    if (matched == pattern.size()) {
      return FileLogSearchResult{position + 1 - pattern.size(), position + 1, *generation,
                                 position + 1 == info->byteExtent};
    }
  }

  return FileLogSearchResult{std::nullopt, end - matched, *generation, end == info->byteExtent};
}

std::expected<SourceReadReply<LogLine>, SourceError>
FileLogModel::load(const SourceReadCommand &command, PageBuilder<LogLine> &out, std::stop_token stop) {
  auto result = loadFile(command, out, stop);

  if (!result) {
    return std::unexpected(result.error().sourceError);
  }

  return std::move(*result);
}

std::expected<SourceReadReply<LogLine>, FileLogReadFailure>
FileLogModel::loadFile(const SourceReadCommand &command, PageBuilder<LogLine> &out, std::stop_token stop) {
  auto failed = [this](FileLogFailure error) -> std::expected<SourceReadReply<LogLine>, FileLogReadFailure> {
    recordFailure(error);

    return std::unexpected(FileLogReadFailure{sourceError(error), error});
  };

  if (!command.maxRows || !command.source.instance || !command.source.epoch || !command.dataRevision) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  try {
    auto operation = beginOperation(stop);

    if (!operation) {
      return failed(operation.error());
    }

    auto file = NativeFile::open(path_, stop);

    if (!file) {
      return failed(file.error());
    }

    auto info = file->info();

    if (!info) {
      return failed(info.error());
    }

    auto generation =
      observe({info->identityHigh, info->identityLow, info->byteExtent, info->modified}, *operation, stop);

    if (!generation) {
      return failed(generation.error());
    }

    auto cache =
      std::span(static_cast<std::byte *>(out.workspace()->allocate(options_.ioBlockBytes)), options_.ioBlockBytes);
    BlockReader reader(*file, info->byteExtent, cache, stop);
    std::uint64_t gap{};
    std::uint64_t requestedGeneration = *generation;

    switch (command.anchor.kind) {
    case SourceAnchorKind::ENTRY:
      gap = command.direction == SourceDirection::BACKWARD ? info->byteExtent : 0;
      break;
    case SourceAnchorKind::TAIL:
      gap = info->byteExtent;
      break;
    case SourceAnchorKind::CURSOR:
      if (command.anchor.cursor.source != command.source) {
        return std::unexpected(SourceError::STALE_SOURCE);
      }

      gap = command.anchor.cursor.token[0];
      requestedGeneration = command.anchor.cursor.token[1];
      break;
    case SourceAnchorKind::ROW:
      if (!command.anchor.row.value) {
        return std::unexpected(SourceError::ROW_NOT_FOUND);
      }

      gap = command.anchor.row.value - 1;
      requestedGeneration = command.anchor.row.generation;
      break;
    default:
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (requestedGeneration != *generation || gap > info->byteExtent) {
      SourceReadReply<LogLine> reply;
      reply.state = SourceReadState::HISTORY_UNAVAILABLE;
      reply.recovery = SourceCursor{command.source, {0, *generation}};

      return reply;
    }

    auto boundary = isBoundary(reader, gap);

    if (!boundary) {
      return failed(boundary.error());
    }

    if (!*boundary) {
      return std::unexpected(command.anchor.kind == SourceAnchorKind::ROW ? SourceError::ROW_NOT_FOUND
                                                                          : SourceError::INVALID_CURSOR);
    }

    // The throwing overload lets debug proxy allocation report workspace exhaustion.
    std::pmr::string text("", 0, out.workspace());
    text.reserve(options_.maxRecordBytes);

    if (command.anchor.kind == SourceAnchorKind::ROW) {
      if (gap == info->byteExtent) {
        return std::unexpected(SourceError::ROW_NOT_FOUND);
      }

      if (command.direction == SourceDirection::BACKWARD) {
        bool incomplete{};
        auto after = readRecord(reader, gap, text, options_.maxRecordBytes, incomplete);

        if (!after) {
          return failed(after.error());
        }

        if (incomplete) {
          return std::unexpected(SourceError::ROW_NOT_FOUND);
        }

        gap = *after;
      }
    }

    auto backward = command.direction == SourceDirection::BACKWARD || command.anchor.kind == SourceAnchorKind::TAIL;
    auto start = gap;

    if (backward) {
      // Rows are preallocated by PageBuilder; only their strings consume additional arena bytes.
      // Reserve conservatively before walking farther back so a full page returns a suffix.
      auto arena = static_cast<SourceArena *>(out.resource());
      auto remaining = arena->capacity() - arena->used();

      for (std::size_t i = 0; i < command.maxRows && start; ++i) {
        auto previous = previousStart(reader, start, options_.maxRecordBytes);

        if (!previous) {
          return failed(previous.error());
        }

        auto bytes = start - *previous + 1;

        if (bytes > remaining) {
          if (i) {
            break;
          }

          return std::unexpected(SourceError::PAGE_TOO_LARGE);
        }

        remaining -= static_cast<std::size_t>(bytes);
        start = *previous;
      }
    }

    auto position = start;
    std::size_t rows{};
    bool incompleteTail{};

    while (position < (backward ? gap : info->byteExtent) && rows < command.maxRows) {
      auto next = readRecord(reader, position, text, options_.maxRecordBytes, incompleteTail);

      if (!next) {
        return failed(next.error());
      }

      if (incompleteTail) {
        // Keep the resumable gap at the record start until the writer completes its UTF-8 sequence.
        break;
      }

      if (position == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(SourceError::POSITION_EXHAUSTED);
      }

      auto appended = out.emplace({position + 1, *generation}, info->byteExtent + 1, position, std::string_view(text),
                                  out.resource());

      if (!appended) {
        if (rows && appended.error() == SourceError::PAGE_TOO_LARGE && !backward) {
          break;
        }

        return std::unexpected(appended.error());
      }

      position = *next;
      ++rows;
    }

    auto latest = file->info();

    if (!latest) {
      return failed(latest.error());
    }

    if (latest->byteExtent < info->byteExtent ||
        (latest->byteExtent == info->byteExtent && latest->modified != info->modified)) {
      return failed(failure(FileLogError::FILE_CHANGED));
    }

    auto edge = command.direction == SourceDirection::BACKWARD
                  ? (start ? SourceEdge::MORE : SourceEdge::FINAL_END)
                  : (position == info->byteExtent || incompleteTail ? SourceEdge::TEMPORARY_TAIL : SourceEdge::MORE);

    return out.ready({command.source, {start, *generation}}, {command.source, {position, *generation}}, edge);
  } catch (const std::bad_alloc &) {
    // loadFile allocates only in the already reserved page/workspace arenas.
    return std::unexpected(SourceError::PAGE_TOO_LARGE);
  }
}
} // namespace portals
