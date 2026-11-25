// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/navigation_checkpoint.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace portals {
namespace {
using Error = NavigationCheckpointError;
constexpr std::array<std::byte, 8> MAGIC{std::byte{'P'}, std::byte{'R'}, std::byte{'T'}, std::byte{'L'},
                                         std::byte{'N'}, std::byte{'A'}, std::byte{'V'}, std::byte{0}};
constexpr std::size_t HEADER_BYTES = 32;
constexpr std::size_t ENTRY_HEADER_BYTES = 8;
constexpr std::size_t CHECKSUM_OFFSET = 28;
constexpr std::uint32_t CRC_POLYNOMIAL = 0xedb88320U;

bool validResource(ModelResource resource) noexcept {
  return resource.identity.instance && resource.identity.epoch && resource.lineage;
}

bool validRole(NavigationCheckpointRole role) noexcept {
  switch (role) {
  case NavigationCheckpointRole::CURRENT:
  case NavigationCheckpointRole::READING_ANCHOR:
  case NavigationCheckpointRole::SELECTION:
  case NavigationCheckpointRole::EDIT_ANCHOR:
    return true;
  }

  return false;
}

bool validKind(PositionKind kind) noexcept {
  return kind == PositionKind::ITEM || kind == PositionKind::GAP || kind == PositionKind::TEXT;
}

bool validAffinity(PositionAffinity affinity) noexcept {
  return affinity == PositionAffinity::FORWARD || affinity == PositionAffinity::BACKWARD;
}

bool validBookmark(const ModelBookmark &bookmark) noexcept {
  const auto position = bookmark.position();

  if (!validResource(bookmark.resource()) || !position.revision || !validKind(position.kind) ||
      !validAffinity(position.affinity)) {
    return false;
  }

  return position.kind == PositionKind::ITEM ? position.row.value && position.row.generation : position.row == RowId{};
}

bool validOptions(NavigationCheckpointOptions options) noexcept {
  constexpr auto WIRE_MAX = std::numeric_limits<std::uint32_t>::max();

  return options.maxBytes >= HEADER_BYTES && options.maxBytes <= WIRE_MAX && options.maxEntries &&
         options.maxEntries <= WIRE_MAX && options.maxEntryBytes && options.maxEntryBytes <= WIRE_MAX &&
         options.maxIdentityBytes && options.maxIdentityBytes <= WIRE_MAX && options.bookmarks.maxBytes;
}

std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t at) noexcept {
  std::uint32_t value{};

  for (unsigned shift = 0; shift != 32; shift += 8) {
    value |= std::to_integer<std::uint32_t>(bytes[at++]) << shift;
  }

  return value;
}

void writeU32(std::span<std::byte> bytes, std::size_t at, std::uint32_t value) noexcept {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes[at++] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

std::uint32_t checksum(std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index >= CHECKSUM_OFFSET && index < HEADER_BYTES) {
      continue;
    }

    crc ^= std::to_integer<std::uint32_t>(bytes[index]);

    for (unsigned bit = 0; bit != 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1U) ? CRC_POLYNOMIAL : 0U);
    }
  }

  return ~crc;
}

struct Envelope {
  std::string_view codec;
  std::string_view resource;
  std::uint32_t codecVersion{};
  std::uint32_t entries{};
  std::size_t firstEntry{};
};

struct EncodedEntry {
  NavigationCheckpointRole role;
  PositionKind kind;
  PositionAffinity affinity;
  std::span<const std::byte> payload;
  std::size_t next{};
};

std::expected<EncodedEntry, Error> readEntry(std::span<const std::byte> bytes, std::size_t at,
                                             std::size_t maxEntryBytes) {
  if (at > bytes.size() || ENTRY_HEADER_BYTES > bytes.size() - at) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  const auto role = static_cast<NavigationCheckpointRole>(std::to_integer<unsigned>(bytes[at]));
  const auto kind = static_cast<PositionKind>(std::to_integer<unsigned>(bytes[at + 1]));
  const auto affinity = static_cast<PositionAffinity>(std::to_integer<unsigned>(bytes[at + 2]));
  const auto reserved = bytes[at + 3];
  const auto length = readU32(bytes, at + 4);
  at += ENTRY_HEADER_BYTES;

  if (!validRole(role) || !validKind(kind) || !validAffinity(affinity) || reserved != std::byte{}) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  if (length > maxEntryBytes) {
    return std::unexpected(Error::TOO_LARGE);
  }

  if (length > bytes.size() - at) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  return EncodedEntry{role, kind, affinity, bytes.subspan(at, length), at + length};
}

std::expected<Envelope, Error> inspect(std::span<const std::byte> bytes, NavigationCheckpointOptions options,
                                       std::stop_token stop) {
  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  if (!validOptions(options)) {
    return std::unexpected(Error::INVALID_ARGUMENT);
  }

  if (bytes.size() > options.maxBytes) {
    return std::unexpected(Error::TOO_LARGE);
  }

  if (bytes.size() < HEADER_BYTES || !std::ranges::equal(bytes.first(MAGIC.size()), MAGIC)) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  if (readU32(bytes, 8) != NavigationCheckpoint::FORMAT_VERSION) {
    return std::unexpected(Error::UNSUPPORTED_VERSION);
  }

  const auto codecVersion = readU32(bytes, 12);
  const auto codecBytes = readU32(bytes, 16);
  const auto resourceBytes = readU32(bytes, 20);
  const auto entries = readU32(bytes, 24);

  if (!codecVersion || !codecBytes || !resourceBytes) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  if (codecBytes > options.maxIdentityBytes || resourceBytes > options.maxIdentityBytes ||
      entries > options.maxEntries) {
    return std::unexpected(Error::TOO_LARGE);
  }

  std::size_t at = HEADER_BYTES;

  if (codecBytes > bytes.size() - at) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  const std::string_view codec(reinterpret_cast<const char *>(bytes.data() + at), codecBytes);
  at += codecBytes;

  if (resourceBytes > bytes.size() - at) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  const std::string_view resource(reinterpret_cast<const char *>(bytes.data() + at), resourceBytes);
  at += resourceBytes;
  const auto firstEntry = at;

  for (std::uint32_t index = 0; index < entries; ++index) {
    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    auto entry = readEntry(bytes, at, options.maxEntryBytes);

    if (!entry) {
      return std::unexpected(entry.error());
    }

    at = entry->next;
  }

  if (at != bytes.size() || readU32(bytes, CHECKSUM_OFFSET) != checksum(bytes)) {
    return std::unexpected(Error::CORRUPT_INPUT);
  }

  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  return Envelope{codec, resource, codecVersion, entries, firstEntry};
}

std::expected<void, Error> checkDescriptor(NavigationCodecDescriptor descriptor, NavigationCheckpointOptions options) {
  if (descriptor.name.empty() || descriptor.resource.empty() || !descriptor.version) {
    return std::unexpected(Error::INVALID_ARGUMENT);
  }

  if (descriptor.name.size() > options.maxIdentityBytes || descriptor.resource.size() > options.maxIdentityBytes) {
    return std::unexpected(Error::TOO_LARGE);
  }

  return {};
}

std::expected<void, Error> checkLiveResource(const NavigationBookmarkCodec &codec, ModelResource original,
                                             std::stop_token stop) {
  const auto live = codec.resource();

  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  if (!validResource(live)) {
    return std::unexpected(Error::CLOSED);
  }

  if (live != original) {
    return std::unexpected(Error::WRONG_RESOURCE);
  }

  return {};
}

// The caller owns every entry and the output arena across all provider callouts.
std::expected<std::size_t, Error> encodeEntries(std::span<const NavigationCheckpointEntry> entries,
                                                const NavigationBookmarkCodec &codec, ModelResource resource,
                                                std::span<std::byte> output, std::size_t at,
                                                NavigationCheckpointOptions options, std::stop_token stop) {
  for (const auto &entry : entries) {
    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    if (ENTRY_HEADER_BYTES > output.size() - at) {
      return std::unexpected(Error::TOO_LARGE);
    }

    const auto position = entry.bookmark.position();
    output[at] = static_cast<std::byte>(entry.role);
    output[at + 1] = static_cast<std::byte>(position.kind);
    output[at + 2] = static_cast<std::byte>(position.affinity);
    const auto header = at;
    at += ENTRY_HEADER_BYTES;
    const auto capacity = std::min(options.maxEntryBytes, output.size() - at);
    auto written = codec.encode(entry, output.subspan(at, capacity), stop);

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    auto live = checkLiveResource(codec, resource, stop);

    if (!live) {
      return std::unexpected(live.error());
    }

    if (!written) {
      return std::unexpected(written.error());
    }

    if (*written > capacity) {
      return std::unexpected(Error::CORRUPT_INPUT);
    }

    writeU32(output, header + 4, static_cast<std::uint32_t>(*written));
    at += *written;
  }

  return at;
}
} // namespace

std::expected<NavigationCheckpoint, NavigationCheckpointError>
NavigationCheckpoint::capture(std::span<const NavigationCheckpointEntry> entries, const NavigationBookmarkCodec &codec,
                              NavigationCheckpointOptions options, std::stop_token stop) {
  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  if (!validOptions(options)) {
    return std::unexpected(Error::INVALID_ARGUMENT);
  }

  if (entries.size() > options.maxEntries) {
    return std::unexpected(Error::TOO_LARGE);
  }

  try {
    // Every virtual getter is a callout: own the input before descriptor/resource, too.
    const std::vector<NavigationCheckpointEntry> staged(entries.begin(), entries.end());
    const auto descriptor = codec.descriptor();

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    auto qualified = checkDescriptor(descriptor, options);

    if (!qualified) {
      return std::unexpected(qualified.error());
    }

    const auto resource = codec.resource();

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    if (!validResource(resource)) {
      return std::unexpected(Error::CLOSED);
    }

    for (const auto &entry : staged) {
      if (!validRole(entry.role) || !validBookmark(entry.bookmark)) {
        return std::unexpected(Error::INVALID_ARGUMENT);
      }

      if (entry.bookmark.storageBytes() > options.bookmarks.maxBytes) {
        return std::unexpected(Error::TOO_LARGE);
      }

      if (entry.bookmark.resource() != resource) {
        return std::unexpected(Error::WRONG_RESOURCE);
      }
    }

    if (descriptor.name.size() > options.maxBytes - HEADER_BYTES ||
        descriptor.resource.size() > options.maxBytes - HEADER_BYTES - descriptor.name.size()) {
      return std::unexpected(Error::TOO_LARGE);
    }

    auto output = std::make_shared<std::vector<std::byte>>(options.maxBytes);
    std::ranges::copy(MAGIC, output->begin());
    writeU32(*output, 8, FORMAT_VERSION);
    writeU32(*output, 12, descriptor.version);
    writeU32(*output, 16, static_cast<std::uint32_t>(descriptor.name.size()));
    writeU32(*output, 20, static_cast<std::uint32_t>(descriptor.resource.size()));
    writeU32(*output, 24, static_cast<std::uint32_t>(staged.size()));
    std::size_t at = HEADER_BYTES;
    std::memcpy(output->data() + at, descriptor.name.data(), descriptor.name.size());
    at += descriptor.name.size();
    std::memcpy(output->data() + at, descriptor.resource.data(), descriptor.resource.size());
    at += descriptor.resource.size();

    auto encodedEnd = encodeEntries(staged, codec, resource, *output, at, options, stop);

    if (!encodedEnd) {
      return std::unexpected(encodedEnd.error());
    }

    output->resize(*encodedEnd);
    writeU32(*output, CHECKSUM_OFFSET, checksum(*output));

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    NavigationCheckpoint checkpoint;
    checkpoint.bytes_ = std::move(output);

    return checkpoint;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (...) {
    return std::unexpected(Error::PROVIDER_FAILURE);
  }
}

std::expected<NavigationCheckpoint, NavigationCheckpointError>
NavigationCheckpoint::parse(std::span<const std::byte> bytes, NavigationCheckpointOptions options,
                            std::stop_token stop) {
  auto envelope = inspect(bytes, options, stop);

  if (!envelope) {
    return std::unexpected(envelope.error());
  }

  try {
    NavigationCheckpoint checkpoint;
    checkpoint.bytes_ = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    return checkpoint;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(Error::NO_MEMORY);
  }
}

std::expected<std::vector<NavigationCheckpointEntry>, NavigationCheckpointError>
NavigationCheckpoint::restore(const NavigationBookmarkCodec &codec, NavigationCheckpointOptions options,
                              std::stop_token stop) const {
  // A callback may replace the caller's checkpoint; retain its immutable bytes locally.
  const auto owner = bytes_;
  const auto input = owner ? std::span<const std::byte>(*owner) : std::span<const std::byte>{};
  auto envelope = inspect(input, options, stop);

  if (!envelope) {
    return std::unexpected(envelope.error());
  }

  const auto descriptor = codec.descriptor();

  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  auto qualified = checkDescriptor(descriptor, options);

  if (!qualified) {
    return std::unexpected(qualified.error());
  }

  if (descriptor.name != envelope->codec) {
    return std::unexpected(Error::UNSUPPORTED_CODEC);
  }

  if (descriptor.version != envelope->codecVersion) {
    return std::unexpected(Error::UNSUPPORTED_CODEC_VERSION);
  }

  if (descriptor.resource != envelope->resource) {
    return std::unexpected(Error::WRONG_RESOURCE);
  }

  const auto resource = codec.resource();

  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  if (!validResource(resource)) {
    return std::unexpected(Error::CLOSED);
  }

  try {
    std::vector<NavigationCheckpointEntry> restored;
    restored.reserve(envelope->entries);
    auto at = envelope->firstEntry;

    for (std::uint32_t index = 0; index < envelope->entries; ++index) {
      if (stop.stop_requested()) {
        return std::unexpected(Error::CANCELLED);
      }

      auto entry = readEntry(input, at, options.maxEntryBytes);

      if (!entry) {
        return std::unexpected(entry.error());
      }

      auto bookmark = codec.decode(entry->payload, entry->kind, entry->affinity, entry->role, stop);

      if (stop.stop_requested()) {
        return std::unexpected(Error::CANCELLED);
      }

      auto live = checkLiveResource(codec, resource, stop);

      if (!live) {
        return std::unexpected(live.error());
      }

      if (!bookmark) {
        return std::unexpected(bookmark.error());
      }

      if (!validBookmark(*bookmark) || bookmark->resource() != resource || bookmark->position().kind != entry->kind ||
          bookmark->position().affinity != entry->affinity) {
        return std::unexpected(Error::CORRUPT_INPUT);
      }

      if (bookmark->storageBytes() > options.bookmarks.maxBytes) {
        return std::unexpected(Error::TOO_LARGE);
      }

      restored.push_back({entry->role, std::move(*bookmark)});
      at = entry->next;
    }

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    return restored;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (...) {
    return std::unexpected(Error::PROVIDER_FAILURE);
  }
}

std::span<const std::byte> NavigationCheckpoint::bytes() const noexcept {
  return bytes_ ? std::span<const std::byte>(*bytes_) : std::span<const std::byte>{};
}

std::expected<NavigationCheckpoint, NavigationCheckpointError>
loadNavigationCheckpoint(NavigationCheckpointStorage &storage, NavigationCheckpointOptions options,
                         std::stop_token stop) {
  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  if (!validOptions(options)) {
    return std::unexpected(Error::INVALID_ARGUMENT);
  }

  try {
    std::vector<std::byte> buffer(options.maxBytes);
    auto read = storage.read(buffer, stop);

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    if (!read) {
      return std::unexpected(read.error());
    }

    if (*read > buffer.size()) {
      return std::unexpected(Error::TOO_LARGE);
    }

    return NavigationCheckpoint::parse(std::span(buffer).first(*read), options, stop);
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (...) {
    return std::unexpected(Error::PROVIDER_FAILURE);
  }
}

std::expected<void, NavigationCheckpointError> saveNavigationCheckpoint(const NavigationCheckpoint &checkpoint,
                                                                        NavigationCheckpointStorage &storage,
                                                                        std::stop_token stop) {
  if (stop.stop_requested()) {
    return std::unexpected(Error::CANCELLED);
  }

  // Pin the immutable value before entering storage, which may replace its caller's handle.
  const auto owner = checkpoint;

  if (owner.bytes().empty()) {
    return std::unexpected(Error::INVALID_ARGUMENT);
  }

  try {
    auto written = storage.write(owner.bytes(), stop);

    if (stop.stop_requested()) {
      return std::unexpected(Error::CANCELLED);
    }

    return written;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::NO_MEMORY);
  } catch (...) {
    return std::unexpected(Error::PROVIDER_FAILURE);
  }
}
} // namespace portals
