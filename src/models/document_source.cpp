// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_source.hpp>
#include <portals/log_file.hpp>

#include <cstring>
#include <limits>

namespace portals {
namespace {
constexpr std::size_t MIN_ENVELOPE_BYTES = 4096;
constexpr auto MAX_OFFSET = std::numeric_limits<std::uint64_t>::max();

DocumentError failure(SourceError code) {
  return {code, {}, std::nullopt};
}

DocumentError fileFailure(FileLogFailure cause) {
  auto code = SourceError::IO_ERROR;

  switch (cause.error) {
  case FileLogError::INVALID_ARGUMENT:
    code = SourceError::INVALID_ARGUMENT;
    break;
  case FileLogError::FILE_CHANGED:
    code = SourceError::REVISION_CHANGED;
    break;
  case FileLogError::CANCELLED:
    code = SourceError::CANCELLED;
    break;
  case FileLogError::NO_MEMORY:
    code = SourceError::NO_MEMORY;
    break;
  default:
    break;
  }

  return {code, std::error_code(static_cast<int>(cause.nativeCode), std::system_category()), cause};
}

bool validRevision(DocumentRevision revision) {
  return revision.source.instance && revision.source.epoch && revision.dataRevision;
}
} // namespace

namespace detail {
struct DocumentPageData {
  std::shared_ptr<SourceArena> arena;
  DocumentReadRequest request;
  std::byte *data{};
  std::size_t count{};
  bool finalEnd{};
};
} // namespace detail

MemoryDocumentProvider::MemoryDocumentProvider(std::shared_ptr<const std::string> text, std::uint64_t revision)
    : text_(std::move(text)), revision_(revision) {
}

ByteDocumentCapabilities MemoryDocumentProvider::capabilities() const noexcept {
  return {true, true};
}

std::expected<ByteDocumentMetadata, DocumentError> MemoryDocumentProvider::metadata(std::stop_token stop) {
  if (stop.stop_requested()) {
    return std::unexpected(failure(SourceError::CANCELLED));
  }

  if (!text_ || !revision_) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }

  return ByteDocumentMetadata{revision_, text_->size()};
}

std::expected<ByteReadResult, DocumentError>
MemoryDocumentProvider::read(const ByteReadCommand &command, std::span<std::byte> destination, std::stop_token stop) {
  auto info = metadata(stop);

  if (!info) {
    return std::unexpected(info.error());
  }

  if (command.dataRevision != revision_) {
    return std::unexpected(failure(SourceError::REVISION_CHANGED));
  }

  if (command.offset > text_->size()) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }

  const auto remaining = text_->size() - static_cast<std::size_t>(command.offset);
  const auto count = std::min(destination.size(), remaining);

  if (count) {
    std::memcpy(destination.data(), text_->data() + static_cast<std::size_t>(command.offset), count);
  }

  return ByteReadResult{count, count == remaining};
}

FileDocumentProvider::FileDocumentProvider(std::shared_ptr<FileLogModel> model) : model_(std::move(model)) {
}

std::expected<std::shared_ptr<FileDocumentProvider>, DocumentError>
FileDocumentProvider::create(std::filesystem::path path, std::size_t ioBlockBytes) {
  FileLogOptions options;

  options.ioBlockBytes = ioBlockBytes;

  auto model = FileLogModel::create(std::move(path), options);

  if (!model) {
    return std::unexpected(fileFailure(model.error()));
  }

  try {
    return std::shared_ptr<FileDocumentProvider>(new FileDocumentProvider(std::move(*model)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  }
}

ByteDocumentCapabilities FileDocumentProvider::capabilities() const noexcept {
  return {true, false};
}

std::expected<ByteDocumentMetadata, DocumentError> FileDocumentProvider::metadata(std::stop_token stop) {
  auto observed = model_->metadata(stop);

  if (!observed) {
    return std::unexpected(fileFailure(observed.error()));
  }

  if (!revision_ || observed->generation != fileGeneration_ || observed->byteExtent != byteExtent_) {
    if (revision_ == MAX_OFFSET) {
      return std::unexpected(failure(SourceError::IDENTITY_EXHAUSTED));
    }

    ++revision_;
    fileGeneration_ = observed->generation;
    byteExtent_ = observed->byteExtent;
  }

  return ByteDocumentMetadata{revision_, byteExtent_};
}

std::expected<ByteReadResult, DocumentError>
FileDocumentProvider::read(const ByteReadCommand &command, std::span<std::byte> destination, std::stop_token stop) {
  auto before = metadata(stop);

  if (!before) {
    return std::unexpected(before.error());
  }

  if (command.dataRevision != before->dataRevision) {
    return std::unexpected(failure(SourceError::REVISION_CHANGED));
  }

  auto bounded = destination.first(std::min(destination.size(), model_->options().ioBlockBytes));
  auto count = model_->readBytes(command.offset, bounded, stop);

  if (!count) {
    return std::unexpected(fileFailure(count.error()));
  }

  auto after = metadata(stop);

  if (!after) {
    return std::unexpected(after.error());
  }

  if (command.dataRevision != after->dataRevision) {
    return std::unexpected(failure(SourceError::REVISION_CHANGED));
  }

  if (command.offset > *after->byteExtent || *count > *after->byteExtent - command.offset) {
    return std::unexpected(failure(SourceError::INVALID_REPLY));
  }

  return ByteReadResult{*count, command.offset + *count == *after->byteExtent};
}

DocumentBytePage::DocumentBytePage(std::shared_ptr<const detail::DocumentPageData> data) : data_(std::move(data)) {
}

std::span<const std::byte> DocumentBytePage::bytes() const noexcept {
  return data_ ? std::span<const std::byte>(data_->data, data_->count) : std::span<const std::byte>{};
}

DocumentRevision DocumentBytePage::revision() const noexcept {
  return data_ ? data_->request.revision : DocumentRevision{};
}

std::uint64_t DocumentBytePage::offset() const noexcept {
  return data_ ? data_->request.offset : 0;
}

std::uint64_t DocumentBytePage::generation() const noexcept {
  return data_ ? data_->request.generation : 0;
}

bool DocumentBytePage::finalEnd() const noexcept {
  return data_ && data_->finalEnd;
}

ByteDocumentSource::ByteDocumentSource(std::shared_ptr<ByteDocumentProvider> provider,
                                       std::shared_ptr<SourceBudget> budget, SourceCredit credit,
                                       DocumentSourceOptions options, SourceStamp stamp)
    : provider_(std::move(provider)), budget_(std::move(budget)), credit_(std::move(credit)), options_(options),
      stamp_(stamp), capabilities_(provider_->capabilities()), ownerThread_(std::this_thread::get_id()) {
}

std::expected<std::shared_ptr<ByteDocumentSource>, DocumentError>
ByteDocumentSource::create(std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget,
                           DocumentSourceOptions options) {
  static_assert(sizeof(ByteDocumentSource) < MIN_ENVELOPE_BYTES / 2);

  if (!provider || !budget || !options.maxPageBytes || options.envelopeBytes < MIN_ENVELOPE_BYTES ||
      options.maxPageBytes > std::numeric_limits<std::size_t>::max() - SourceArena::METADATA_BYTES) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }

  auto stamp = detail::windowSourceStamp();

  if (!stamp) {
    return std::unexpected(failure(stamp.error()));
  }

  auto credit = budget->reserve({options.envelopeBytes, 0, 0, 1});

  if (!credit) {
    return std::unexpected(failure(credit.error()));
  }

  try {
    return std::shared_ptr<ByteDocumentSource>(
      new ByteDocumentSource(std::move(provider), std::move(budget), std::move(*credit), options, *stamp));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  }
}

SourceStamp ByteDocumentSource::stamp() const noexcept {
  return stamp_;
}

ByteDocumentCapabilities ByteDocumentSource::capabilities() const noexcept {
  return capabilities_;
}

std::size_t ByteDocumentSource::maxPageBytes() const noexcept {
  return options_.maxPageBytes;
}

std::expected<DocumentMetadata, DocumentError> ByteDocumentSource::inspect(std::stop_token stop) {
  if (std::this_thread::get_id() == ownerThread_) {
    return std::unexpected(failure(SourceError::WRONG_THREAD));
  }

  if (stop.stop_requested()) {
    return std::unexpected(failure(SourceError::CANCELLED));
  }

  std::unique_lock providerLock(providerMutex_);

  if (stop.stop_requested()) {
    return std::unexpected(failure(SourceError::CANCELLED));
  }

  auto work = budget_->reserve({options_.envelopeBytes, 0, 1, 0});

  if (!work) {
    auto error = failure(work.error());
    error.budgetDemand = SourceBudgetUsage{options_.envelopeBytes, 0, 1, 0};
    return std::unexpected(error);
  }

  try {
    auto info = provider_->metadata(stop);

    if (stop.stop_requested()) {
      return std::unexpected(failure(SourceError::CANCELLED));
    }

    if (!info) {
      return std::unexpected(info.error());
    }

    if (!info->dataRevision) {
      return std::unexpected(failure(SourceError::INVALID_REPLY));
    }

    return DocumentMetadata{{stamp_, info->dataRevision}, info->byteExtent, capabilities_};
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}

std::expected<DocumentBytePage, DocumentError> ByteDocumentSource::read(DocumentReadRequest request,
                                                                        std::stop_token stop) {
  if (std::this_thread::get_id() == ownerThread_) {
    return std::unexpected(failure(SourceError::WRONG_THREAD));
  }

  if (request.revision.source != stamp_) {
    return std::unexpected(failure(SourceError::STALE_SOURCE));
  }

  if (!request.revision.dataRevision || !request.maxBytes || request.maxBytes > options_.maxPageBytes) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }

  if (request.maxBytes > MAX_OFFSET - request.offset) {
    return std::unexpected(failure(SourceError::POSITION_EXHAUSTED));
  }

  if (stop.stop_requested()) {
    return std::unexpected(failure(SourceError::CANCELLED));
  }

  std::unique_lock providerLock(providerMutex_);

  if (stop.stop_requested()) {
    return std::unexpected(failure(SourceError::CANCELLED));
  }

  auto work = budget_->reserve({options_.envelopeBytes, 0, 1, 0});

  if (!work) {
    auto error = failure(work.error());
    error.budgetDemand = SourceBudgetUsage{options_.envelopeBytes, 0, 1, 0};
    return std::unexpected(error);
  }

  auto credit = budget_->reserve({request.maxBytes + SourceArena::METADATA_BYTES, 1, 0, 0});

  if (!credit) {
    auto error = failure(credit.error());
    error.budgetDemand = SourceBudgetUsage{request.maxBytes + SourceArena::METADATA_BYTES, 1, 0, 0};
    return std::unexpected(error);
  }

  auto arena = SourceArena::create(std::move(*credit));

  if (!arena) {
    return std::unexpected(failure(arena.error()));
  }

  try {
    static_assert(sizeof(detail::DocumentPageData) < SourceArena::METADATA_BYTES / 2);
    auto data = std::make_shared<detail::DocumentPageData>();

    data->arena = std::move(*arena);
    data->request = request;
    data->data = static_cast<std::byte *>(data->arena->allocate(request.maxBytes, alignof(std::byte)));

    auto result =
      provider_->read({request.revision.dataRevision, request.offset}, {data->data, request.maxBytes}, stop);

    if (stop.stop_requested()) {
      return std::unexpected(failure(SourceError::CANCELLED));
    }

    if (!result) {
      return std::unexpected(result.error());
    }

    if (result->bytesRead > request.maxBytes) {
      return std::unexpected(failure(SourceError::INVALID_REPLY));
    }

    if (!result->bytesRead && !result->finalEnd) {
      return std::unexpected(failure(SourceError::NO_PROGRESS));
    }

    data->count = result->bytesRead;
    data->finalEnd = result->finalEnd;

    return DocumentBytePage(std::move(data));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(failure(SourceError::CALLBACK_FAILED));
  }
}

SparseDocumentIndex::SparseDocumentIndex(std::shared_ptr<SourceArena> arena, DocumentRevision revision,
                                         DocumentIndexOptions options)
    : arena_(std::move(arena)), revision_(revision), options_(options), interval_(options.checkpointBytes) {
  checkpoints_ = static_cast<DocumentCheckpoint *>(
    arena_->allocate(options.maxCheckpoints * sizeof(DocumentCheckpoint), alignof(DocumentCheckpoint)));

  std::construct_at(checkpoints_, DocumentCheckpoint{});
}

SparseDocumentIndex::~SparseDocumentIndex() = default;

std::expected<std::unique_ptr<SparseDocumentIndex>, DocumentError>
SparseDocumentIndex::create(std::shared_ptr<SourceBudget> budget, DocumentRevision revision,
                            DocumentIndexOptions options) {
  if (!budget || !validRevision(revision) || options.maxCheckpoints < 2 || !options.checkpointBytes ||
      !options.scanBytes ||
      options.maxCheckpoints >
        (std::numeric_limits<std::size_t>::max() - SourceArena::METADATA_BYTES) / sizeof(DocumentCheckpoint)) {
    return std::unexpected(failure(SourceError::INVALID_ARGUMENT));
  }

  if (options.policy != DocumentLinePolicy::RAW_BYTES && options.policy != DocumentLinePolicy::UTF8) {
    return std::unexpected(failure(SourceError::UNSUPPORTED));
  }

  auto credit =
    budget->reserve({options.maxCheckpoints * sizeof(DocumentCheckpoint) + SourceArena::METADATA_BYTES, 0, 0, 1});

  if (!credit) {
    return std::unexpected(failure(credit.error()));
  }

  auto arena = SourceArena::create(std::move(*credit));

  if (!arena) {
    return std::unexpected(failure(arena.error()));
  }

  try {
    static_assert(sizeof(SparseDocumentIndex) < SourceArena::METADATA_BYTES / 2);

    return std::unique_ptr<SparseDocumentIndex>(new SparseDocumentIndex(std::move(*arena), revision, options));
  } catch (const std::bad_alloc &) {
    return std::unexpected(failure(SourceError::NO_MEMORY));
  }
}

DocumentRevision SparseDocumentIndex::revision() const noexcept {
  return revision_;
}

DocumentLinePolicy SparseDocumentIndex::policy() const noexcept {
  return options_.policy;
}

DocumentIndexProgress SparseDocumentIndex::progress() const noexcept {
  return {prefix_.offset, prefix_.line, prefix_.ended ? std::optional<std::uint64_t>(prefix_.line + 1) : std::nullopt,
          checkpointCount_};
}

DocumentCheckpoint SparseDocumentIndex::checkpointBeforeByte(std::uint64_t offset) const noexcept {
  auto result = checkpoints_[0];

  for (std::size_t i = 1; i < checkpointCount_ && checkpoints_[i].offset <= offset; ++i) {
    result = checkpoints_[i];
  }

  return result;
}

DocumentCheckpoint SparseDocumentIndex::checkpointBeforeLine(std::uint64_t line) const noexcept {
  auto result = checkpoints_[0];

  for (std::size_t i = 1; i < checkpointCount_ && checkpoints_[i].line <= line; ++i) {
    result = checkpoints_[i];
  }

  return result;
}

void SparseDocumentIndex::addCheckpoint(DocumentCheckpoint checkpoint) noexcept {
  if (checkpoint.offset < checkpoints_[checkpointCount_ - 1].offset ||
      checkpoint.offset - checkpoints_[checkpointCount_ - 1].offset < interval_) {
    return;
  }

  if (checkpointCount_ == options_.maxCheckpoints) {
    std::size_t retained = 1;

    for (std::size_t i = 2; i < checkpointCount_; i += 2) {
      checkpoints_[retained++] = checkpoints_[i];
    }

    checkpointCount_ = retained;
    interval_ = interval_ > MAX_OFFSET / 2 ? MAX_OFFSET : interval_ * 2;

    if (checkpoint.offset - checkpoints_[checkpointCount_ - 1].offset < interval_) {
      return;
    }
  }

  std::construct_at(checkpoints_ + checkpointCount_, checkpoint);
  ++checkpointCount_;
}

std::expected<void, DocumentError> SparseDocumentIndex::nextLine(Cursor &cursor, std::uint64_t start,
                                                                 bool extendPrefix) {
  if (cursor.line >= MAX_OFFSET - 1) {
    return std::unexpected(failure(SourceError::POSITION_EXHAUSTED));
  }

  ++cursor.line;
  cursor.lineStart = start;
  cursor.pendingCR = false;

  if (extendPrefix) {
    addCheckpoint({cursor.line, start});
  }

  return {};
}

std::expected<void, DocumentError> SparseDocumentIndex::scan(ByteDocumentSource &source, Cursor &cursor,
                                                             std::optional<std::uint64_t> target, bool extendPrefix,
                                                             std::stop_token stop) {
  if (source.stamp() != revision_.source) {
    return std::unexpected(failure(SourceError::STALE_SOURCE));
  }

  if (stop.stop_requested()) {
    return std::unexpected(failure(SourceError::CANCELLED));
  }

  if (cursor.ended || (target && cursor.line == *target)) {
    return {};
  }

  if (!source.capabilities().seekable) {
    return std::unexpected(failure(SourceError::UNSUPPORTED));
  }

  auto page = source.read({revision_, cursor.offset, std::min(options_.scanBytes, source.maxPageBytes()), 0}, stop);

  if (!page) {
    return std::unexpected(page.error());
  }

  for (const auto byte : page->bytes()) {
    if (stop.stop_requested()) {
      return std::unexpected(failure(SourceError::CANCELLED));
    }

    const auto value = std::to_integer<unsigned char>(byte);

    if (cursor.pendingCR) {
      if (value == '\n') {
        ++cursor.offset;
      }

      auto result = nextLine(cursor, cursor.offset, extendPrefix);

      if (!result) {
        return result;
      }

      if ((target && cursor.line == *target) || value == '\n') {
        if (target && cursor.line == *target) {
          return {};
        }

        continue;
      }
    }

    ++cursor.offset;

    if (value == '\r') {
      cursor.pendingCR = true;
    } else if (value == '\n') {
      auto result = nextLine(cursor, cursor.offset, extendPrefix);

      if (!result) {
        return result;
      }

      if (target && cursor.line == *target) {
        return {};
      }
    }
  }

  if (page->finalEnd()) {
    if (cursor.pendingCR) {
      auto result = nextLine(cursor, cursor.offset, extendPrefix);

      if (!result) {
        return result;
      }
    }

    cursor.ended = true;
  }

  return {};
}

std::expected<void, DocumentError> SparseDocumentIndex::validate(ByteDocumentSource &source, std::stop_token stop) {
  if (source.stamp() != revision_.source) {
    return std::unexpected(failure(SourceError::STALE_SOURCE));
  }

  auto metadata = source.inspect(stop);

  if (!metadata) {
    return std::unexpected(metadata.error());
  }

  if (metadata->revision != revision_) {
    return std::unexpected(failure(SourceError::REVISION_CHANGED));
  }

  return {};
}

std::expected<DocumentIndexProgress, DocumentError> SparseDocumentIndex::advance(ByteDocumentSource &source,
                                                                                 std::stop_token stop) {
  auto valid = validate(source, stop);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  auto result = scan(source, prefix_, std::nullopt, true, stop);

  if (!result) {
    return std::unexpected(result.error());
  }

  return progress();
}

std::expected<DocumentLineLocation, DocumentError>
SparseDocumentIndex::locateLine(ByteDocumentSource &source, std::uint64_t line, std::stop_token stop) {
  auto valid = validate(source, stop);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  if (prefix_.ended && line > prefix_.line) {
    return DocumentLineLocation{DocumentLocationState::UNKNOWN, {prefix_.line, prefix_.lineStart}};
  }

  if (!lookupTarget_ || *lookupTarget_ != line) {
    auto anchor = checkpointBeforeLine(line);

    lookup_ = {anchor.offset, anchor.line, anchor.offset, false, false};
    lookupTarget_ = line;

    if (prefix_.line <= line && prefix_.line >= lookup_.line) {
      lookup_ = prefix_;
    }
  }

  auto result = scan(source, lookup_, line, true, stop);

  if (!result) {
    return std::unexpected(result.error());
  }

  if (lookup_.offset > prefix_.offset || (lookup_.offset == prefix_.offset && lookup_.ended)) {
    prefix_ = lookup_;
  }

  const auto state = lookup_.line == line ? DocumentLocationState::EXACT
                     : lookup_.ended      ? DocumentLocationState::UNKNOWN
                                          : DocumentLocationState::PENDING;

  return DocumentLineLocation{state, {lookup_.line, lookup_.lineStart}};
}
} // namespace portals
