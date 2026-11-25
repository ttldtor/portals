// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/navigation_checkpoint.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace portals;
using Error = NavigationCheckpointError;
using Role = NavigationCheckpointRole;
constexpr ModelResource RESOURCE{{41, 1}, 1};
constexpr ModelResource REOPENED{{42, 1}, 1};

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result.has_value());

  return std::move(*result);
}

ModelBookmark bookmark(std::uint64_t offset, PositionKind kind = PositionKind::TEXT,
                       PositionAffinity affinity = PositionAffinity::FORWARD, ModelResource resource = RESOURCE) {
  ModelPosition position{.kind = kind, .offset = offset, .revision = 1, .affinity = affinity};

  if (kind == PositionKind::ITEM) {
    position.row = {offset + 1, 1};
  }

  return take(ModelBookmark::create(resource, position));
}

class TestCodec final : public NavigationBookmarkCodec {
public:
  NavigationCodecDescriptor descriptor() const noexcept override {
    if (onDescriptor) {
      onDescriptor();
    }

    return {name, version, key};
  }

  ModelResource resource() const noexcept override {
    if (onResource) {
      onResource();
    }

    return domain;
  }

  std::expected<std::size_t, Error> encode(const NavigationCheckpointEntry &entry, std::span<std::byte> output,
                                           std::stop_token) const override {
    ++encodes;

    if (onEncode) {
      onEncode();
    }

    if (failure) {
      return std::unexpected(*failure);
    }

    if (overreport) {
      return output.size() + 1;
    }

    if (output.size() < sizeof(std::uint64_t)) {
      return std::unexpected(Error::TOO_LARGE);
    }

    // Test provider's explicit portable coordinate in one immutable tiny document.
    auto offset = entry.bookmark.position().offset;

    for (std::size_t index = 0; index != sizeof(offset); ++index) {
      output[index] = static_cast<std::byte>(offset & 0xffU);
      offset >>= 8;
    }

    return sizeof(offset);
  }

  std::expected<ModelBookmark, Error> decode(std::span<const std::byte> input, PositionKind kind,
                                             PositionAffinity affinity, Role, std::stop_token) const override {
    ++decodes;

    if (onDecode) {
      onDecode();
    }

    if (failure) {
      return std::unexpected(*failure);
    }

    if (input.size() != sizeof(std::uint64_t)) {
      return std::unexpected(Error::CORRUPT_INPUT);
    }

    std::uint64_t offset{};

    for (std::size_t index = 0; index != sizeof(offset); ++index) {
      offset |= std::to_integer<std::uint64_t>(input[index]) << (index * 8);
    }

    if (invalidBookmark) {
      return ModelBookmark{};
    }

    const auto decodedDomain = foreignBookmark || domain == ModelResource{} ? RESOURCE : domain;
    const auto decodedKind = wrongKind ? PositionKind::GAP : kind;
    const auto decodedAffinity = wrongAffinity ? PositionAffinity::BACKWARD : affinity;

    return bookmark(offset, decodedKind, decodedAffinity, decodedDomain);
  }

  std::string_view name{"test.immutable-position"};
  std::string_view key{"fixture:document/content-v1"};
  std::uint32_t version{1};
  mutable ModelResource domain{RESOURCE};
  mutable int encodes{}, decodes{};
  std::optional<Error> failure;
  bool overreport{}, invalidBookmark{}, foreignBookmark{}, wrongKind{}, wrongAffinity{};
  std::function<void()> onDescriptor, onResource, onEncode, onDecode;
};

class MemoryStorage final : public NavigationCheckpointStorage {
public:
  std::expected<std::size_t, Error> read(std::span<std::byte> output, std::stop_token) override {
    ++reads;
    calledThread = std::this_thread::get_id();

    if (onRead) {
      onRead();
    }

    if (failure) {
      return std::unexpected(*failure);
    }

    if (overreport) {
      return output.size() + 1;
    }

    if (data.size() > output.size()) {
      return std::unexpected(Error::TOO_LARGE);
    }

    std::ranges::copy(data, output.begin());

    return data.size();
  }

  std::expected<void, Error> write(std::span<const std::byte> input, std::stop_token) override {
    ++writes;
    calledThread = std::this_thread::get_id();

    if (onWrite) {
      onWrite();
    }

    if (failure) {
      return std::unexpected(*failure);
    }

    data.assign(input.begin(), input.end());

    return {};
  }

  std::vector<std::byte> data;
  std::optional<Error> failure;
  std::function<void()> onRead, onWrite;
  std::thread::id calledThread;
  bool overreport{};
  int reads{}, writes{};
};

NavigationCheckpoint capture(TestCodec &codec, std::uint64_t offset = 17) {
  const std::array entries{NavigationCheckpointEntry{Role::READING_ANCHOR, bookmark(offset)}};

  return take(NavigationCheckpoint::capture(entries, codec));
}

std::vector<std::byte> copyBytes(const NavigationCheckpoint &checkpoint) {
  const auto bytes = checkpoint.bytes();

  return {bytes.begin(), bytes.end()};
}
} // namespace

TEST_CASE("checkpoint round trip preserves channels and requalifies a reopened resource") {
  TestCodec original;
  const std::array entries{
    NavigationCheckpointEntry{Role::CURRENT, bookmark(1, PositionKind::ITEM)},
    NavigationCheckpointEntry{Role::READING_ANCHOR, bookmark(17, PositionKind::TEXT, PositionAffinity::BACKWARD)},
    NavigationCheckpointEntry{Role::SELECTION, bookmark(4, PositionKind::GAP)},
    NavigationCheckpointEntry{Role::SELECTION, bookmark(5, PositionKind::ITEM)},
    NavigationCheckpointEntry{Role::EDIT_ANCHOR, bookmark(8)}};
  auto checkpoint = take(NavigationCheckpoint::capture(entries, original));
  auto bytes = copyBytes(checkpoint);
  auto parsed = take(NavigationCheckpoint::parse(bytes));
  std::ranges::fill(bytes, std::byte{});
  checkpoint = {};
  TestCodec reopened;
  reopened.domain = REOPENED;
  const auto restored = take(parsed.restore(reopened));

  REQUIRE(restored.size() == entries.size());

  for (std::size_t index = 0; index < entries.size(); ++index) {
    CHECK(restored[index].role == entries[index].role);
    CHECK(restored[index].bookmark.resource() == REOPENED);
    CHECK(restored[index].bookmark.position().kind == entries[index].bookmark.position().kind);
    CHECK(restored[index].bookmark.position().affinity == entries[index].bookmark.position().affinity);
    CHECK(restored[index].bookmark.position().offset == entries[index].bookmark.position().offset);
  }

  CHECK(restored.back().role == Role::EDIT_ANCHOR);
  CHECK(original.encodes == 5);
  CHECK(reopened.decodes == 5);
}

TEST_CASE("default and empty checkpoints are distinct") {
  NavigationCheckpoint empty;
  TestCodec codec;
  CHECK(empty.bytes().empty());
  CHECK(empty.restore(codec).error() == Error::CORRUPT_INPUT);
  auto checkpoint = take(NavigationCheckpoint::capture({}, codec));
  CHECK_FALSE(checkpoint.bytes().empty());
  CHECK(take(checkpoint.restore(codec)).empty());
  CHECK(codec.encodes == 0);
  CHECK(codec.decodes == 0);
}

TEST_CASE("untrusted envelope preflight rejects every truncation and accidental byte change") {
  TestCodec codec;
  const auto checkpoint = capture(codec);
  const auto bytes = copyBytes(checkpoint);

  for (std::size_t length = 0; length < bytes.size(); ++length) {
    CAPTURE(length);
    CHECK_FALSE(NavigationCheckpoint::parse(std::span(bytes).first(length)));
  }

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    auto corrupt = bytes;
    corrupt[index] ^= std::byte{0x80};
    CAPTURE(index);
    CHECK_FALSE(NavigationCheckpoint::parse(corrupt));
  }

  auto trailing = bytes;
  trailing.push_back(std::byte{});
  CHECK(NavigationCheckpoint::parse(trailing).error() == Error::CORRUPT_INPUT);
  CHECK(codec.decodes == 0);
}

TEST_CASE("schema codec version and portable resource mismatches are typed before provider work") {
  TestCodec codec;
  const auto checkpoint = capture(codec);
  auto bytes = copyBytes(checkpoint);
  bytes[8] = std::byte{2};
  CHECK(NavigationCheckpoint::parse(bytes).error() == Error::UNSUPPORTED_VERSION);
  codec.name = "test.other";
  CHECK(checkpoint.restore(codec).error() == Error::UNSUPPORTED_CODEC);
  codec.name = "test.immutable-position";
  codec.version = 2;
  CHECK(checkpoint.restore(codec).error() == Error::UNSUPPORTED_CODEC_VERSION);
  codec.version = 1;
  codec.key = "fixture:document/content-v2";
  CHECK(checkpoint.restore(codec).error() == Error::WRONG_RESOURCE);
  CHECK(codec.decodes == 0);
}

TEST_CASE("capture and import obey each independent bounded metadata limit") {
  TestCodec codec;
  const auto checkpoint = capture(codec);
  NavigationCheckpointOptions options;
  options.maxBytes = checkpoint.bytes().size() - 1;
  CHECK(NavigationCheckpoint::parse(checkpoint.bytes(), options).error() == Error::TOO_LARGE);
  options = {};
  options.maxEntryBytes = 7;
  CHECK(NavigationCheckpoint::parse(checkpoint.bytes(), options).error() == Error::TOO_LARGE);
  options = {};
  options.maxIdentityBytes = 2;
  CHECK(NavigationCheckpoint::parse(checkpoint.bytes(), options).error() == Error::TOO_LARGE);
  const std::array entries{NavigationCheckpointEntry{Role::CURRENT, bookmark(2)},
                           NavigationCheckpointEntry{Role::READING_ANCHOR, bookmark(3)}};
  options = {};
  options.maxEntries = 1;
  CHECK(NavigationCheckpoint::capture(entries, codec, options).error() == Error::TOO_LARGE);
  options = {};
  options.maxBytes = 32;
  CHECK(NavigationCheckpoint::capture(entries, codec, options).error() == Error::TOO_LARGE);
  options = {};
  options.maxEntryBytes = 7;
  CHECK(NavigationCheckpoint::capture(entries, codec, options).error() == Error::TOO_LARGE);
  options = {};
  options.maxBytes = 0;
  CHECK(NavigationCheckpoint::parse(checkpoint.bytes(), options).error() == Error::INVALID_ARGUMENT);
  options = {};
  options.bookmarks.maxBytes = 1;
  CHECK(NavigationCheckpoint::capture(entries, codec, options).error() == Error::TOO_LARGE);
  CHECK(checkpoint.restore(codec, options).error() == Error::TOO_LARGE);
}

TEST_CASE("invalid or foreign bookmarks cannot cross the codec seam") {
  TestCodec codec;
  std::array entries{NavigationCheckpointEntry{Role::CURRENT, {}}};
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::INVALID_ARGUMENT);
  entries[0] = {Role::CURRENT, bookmark(3, PositionKind::TEXT, PositionAffinity::FORWARD, REOPENED)};
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::WRONG_RESOURCE);
  entries[0] = {static_cast<Role>(99), bookmark(3)};
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::INVALID_ARGUMENT);
  CHECK(codec.encodes == 0);
  const auto checkpoint = capture(codec);
  codec.domain = REOPENED;
  codec.foreignBookmark = true;
  CHECK(checkpoint.restore(codec).error() == Error::CORRUPT_INPUT);
  codec.foreignBookmark = false;
  codec.invalidBookmark = true;
  CHECK(checkpoint.restore(codec).error() == Error::CORRUPT_INPUT);
  codec.invalidBookmark = false;
  codec.wrongKind = true;
  CHECK(checkpoint.restore(codec).error() == Error::CORRUPT_INPUT);
  codec.wrongKind = false;
  codec.wrongAffinity = true;
  CHECK(checkpoint.restore(codec).error() == Error::CORRUPT_INPUT);
}

TEST_CASE("codec failures exceptions and invalid written counts preserve checkpoint bytes") {
  TestCodec codec;
  const auto checkpoint = capture(codec);
  const auto original = copyBytes(checkpoint);
  codec.failure = Error::UNSUPPORTED;
  CHECK(checkpoint.restore(codec).error() == Error::UNSUPPORTED);
  const std::array entries{NavigationCheckpointEntry{Role::CURRENT, bookmark(5)}};
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::UNSUPPORTED);
  codec.failure = {};
  codec.overreport = true;
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::CORRUPT_INPUT);
  codec.overreport = false;
  codec.onDecode = [] {
    throw std::runtime_error("provider failed");
  };
  CHECK(checkpoint.restore(codec).error() == Error::PROVIDER_FAILURE);
  codec.onEncode = [] {
    throw std::bad_alloc();
  };
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::NO_MEMORY);
  CHECK(copyBytes(checkpoint) == original);
}

TEST_CASE("cancellation and close are checked before and after each provider callback") {
  TestCodec codec;
  const auto checkpoint = capture(codec);
  std::stop_source stopped;
  stopped.request_stop();
  const std::array entries{NavigationCheckpointEntry{Role::CURRENT, bookmark(5)}};
  CHECK(NavigationCheckpoint::capture(entries, codec, {}, stopped.get_token()).error() == Error::CANCELLED);
  CHECK(NavigationCheckpoint::parse(checkpoint.bytes(), {}, stopped.get_token()).error() == Error::CANCELLED);
  CHECK(checkpoint.restore(codec, {}, stopped.get_token()).error() == Error::CANCELLED);
  CHECK(codec.encodes == 1);
  CHECK(codec.decodes == 0);
  std::stop_source during;
  codec.onDecode = [&] {
    during.request_stop();
  };
  CHECK(checkpoint.restore(codec, {}, during.get_token()).error() == Error::CANCELLED);
  codec.onDecode = [&] {
    codec.domain = {};
  };
  CHECK(checkpoint.restore(codec).error() == Error::CLOSED);
  codec.domain = RESOURCE;
  codec.onEncode = [&] {
    codec.domain = REOPENED;
  };
  CHECK(NavigationCheckpoint::capture(entries, codec).error() == Error::WRONG_RESOURCE);
}

TEST_CASE("callback reentry cannot invalidate checkpoint input or capture entries") {
  TestCodec codec;
  std::vector<NavigationCheckpointEntry> entries{{Role::CURRENT, bookmark(2)}, {Role::READING_ANCHOR, bookmark(8)}};
  codec.onEncode = [&] {
    entries.clear();
    entries.shrink_to_fit();
  };
  auto checkpoint = take(NavigationCheckpoint::capture(entries, codec));
  codec.onEncode = {};
  codec.onDecode = [&] {
    checkpoint = {};
  };
  auto restored = take(checkpoint.restore(codec));
  REQUIRE(restored.size() == 2);
  CHECK(restored[0].bookmark.position().offset == 2);
  CHECK(restored[1].bookmark.position().offset == 8);
  CHECK(checkpoint.bytes().empty());
}

TEST_CASE("capture owns entries before descriptor and resource callbacks") {
  TestCodec codec;
  std::array entries{NavigationCheckpointEntry{Role::READING_ANCHOR, bookmark(7)}};
  codec.onDescriptor = [&] {
    entries[0].bookmark = {};
  };
  auto checkpoint = take(NavigationCheckpoint::capture(entries, codec));
  codec.onDescriptor = {};
  CHECK(take(checkpoint.restore(codec))[0].bookmark.position().offset == 7);

  entries[0].bookmark = bookmark(9);
  codec.onResource = [&] {
    entries[0].bookmark = {};
  };
  checkpoint = take(NavigationCheckpoint::capture(entries, codec));
  codec.onResource = {};
  CHECK(take(checkpoint.restore(codec))[0].bookmark.position().offset == 9);

  std::vector<NavigationCheckpointEntry> temporary{{Role::CURRENT, bookmark(11)}};
  codec.onDescriptor = [&] {
    temporary.clear();
    temporary.shrink_to_fit();
  };
  checkpoint = take(NavigationCheckpoint::capture(temporary, codec));
  codec.onDescriptor = {};
  CHECK(take(checkpoint.restore(codec))[0].bookmark.position().offset == 11);
}

TEST_CASE("cancellation from descriptor and final resource getters rejects staged success") {
  TestCodec codec;
  const auto empty = take(NavigationCheckpoint::capture({}, codec));
  const auto checkpoint = capture(codec);
  std::stop_source emptyStop;
  codec.onResource = [&] {
    emptyStop.request_stop();
  };
  CHECK(empty.restore(codec, {}, emptyStop.get_token()).error() == Error::CANCELLED);
  codec.onResource = {};
  std::stop_source descriptorStop;
  codec.onDescriptor = [&] {
    descriptorStop.request_stop();
  };
  CHECK(checkpoint.restore(codec, {}, descriptorStop.get_token()).error() == Error::CANCELLED);
  codec.onDescriptor = {};
  std::stop_source finalStop;
  int resourceCalls{};
  codec.onResource = [&] {
    if (++resourceCalls == 2) {
      finalStop.request_stop();
    }
  };
  CHECK(checkpoint.restore(codec, {}, finalStop.get_token()).error() == Error::CANCELLED);
  CHECK(resourceCalls == 2);
}

TEST_CASE("optional storage is explicit synchronous and failures do not erase memory state") {
  TestCodec codec;
  const auto checkpoint = capture(codec);
  const auto before = copyBytes(checkpoint);
  MemoryStorage storage;
  REQUIRE(saveNavigationCheckpoint(checkpoint, storage));
  CHECK(storage.calledThread == std::this_thread::get_id());
  const auto loaded = take(loadNavigationCheckpoint(storage));
  CHECK(copyBytes(loaded) == before);
  storage.failure = Error::STORAGE_FAILURE;
  CHECK(saveNavigationCheckpoint(checkpoint, storage).error() == Error::STORAGE_FAILURE);
  CHECK(loadNavigationCheckpoint(storage).error() == Error::STORAGE_FAILURE);
  storage.failure = Error::CLOSED;
  CHECK(loadNavigationCheckpoint(storage).error() == Error::CLOSED);
  CHECK(copyBytes(checkpoint) == before);
  CHECK(take(checkpoint.restore(codec))[0].bookmark.position().offset == 17);
}

TEST_CASE("storage overflow cancellation exceptions and callback replacement are bounded") {
  TestCodec codec;
  auto checkpoint = capture(codec);
  const auto before = copyBytes(checkpoint);
  MemoryStorage storage;
  storage.overreport = true;
  CHECK(loadNavigationCheckpoint(storage).error() == Error::TOO_LARGE);
  storage.overreport = false;
  storage.onRead = [] {
    throw std::runtime_error("storage failed");
  };
  CHECK(loadNavigationCheckpoint(storage).error() == Error::PROVIDER_FAILURE);
  storage.onRead = {};
  std::stop_source stop;
  storage.onRead = [&] {
    stop.request_stop();
  };
  CHECK(loadNavigationCheckpoint(storage, {}, stop.get_token()).error() == Error::CANCELLED);
  storage.onRead = {};
  storage.onWrite = [&] {
    checkpoint = {};
  };
  REQUIRE(saveNavigationCheckpoint(checkpoint, storage));
  CHECK(storage.data == before);
  CHECK(checkpoint.bytes().empty());
  CHECK(saveNavigationCheckpoint(checkpoint, storage).error() == Error::INVALID_ARGUMENT);
}
