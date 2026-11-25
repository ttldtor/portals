// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/navigation_checkpoint.hpp>
#include <portals/ui.hpp>
#include <portals/ui_text_document.hpp>

#include <array>

namespace checkpoint_example {
using namespace portals;

// This codec is only for this exact immutable ASCII document, version one.
// Changing its contents requires a new portable resource key and a migration policy.
inline constexpr std::string_view NOTE = "One document. Two openings. The same reading place.";
inline constexpr std::size_t READING_OFFSET = 27;

class NoteCodec final : public NavigationBookmarkCodec {
public:
  explicit NoteCodec(ModelResource resource) : resource_(resource) {
  }

  NavigationCodecDescriptor descriptor() const noexcept override {
    return {"example-ascii-offset", 1, "example:immutable-note:v1"};
  }

  ModelResource resource() const noexcept override {
    return resource_;
  }

  std::expected<std::size_t, NavigationCheckpointError>
  encode(const NavigationCheckpointEntry &entry, std::span<std::byte> output, std::stop_token) const override {
    const auto point = entry.bookmark.position();
    if (point.kind != PositionKind::TEXT || point.revision != 1 || point.offset > NOTE.size()) {
      return std::unexpected(NavigationCheckpointError::UNSUPPORTED);
    }

    if (output.empty()) {
      return std::unexpected(NavigationCheckpointError::TOO_LARGE);
    }

    output.front() = static_cast<std::byte>(point.offset);

    return 1;
  }

  std::expected<ModelBookmark, NavigationCheckpointError> decode(std::span<const std::byte> payload, PositionKind kind,
                                                                 PositionAffinity affinity, NavigationCheckpointRole,
                                                                 std::stop_token) const override {
    if (kind != PositionKind::TEXT || payload.size() != 1 || std::to_integer<unsigned>(payload.front()) > NOTE.size()) {
      return std::unexpected(NavigationCheckpointError::CORRUPT_INPUT);
    }

    // Every byte boundary in this fixed ASCII document is a grapheme boundary.
    return ModelBookmark::create(resource_,
                                 {PositionKind::TEXT, {}, std::to_integer<unsigned>(payload.front()), 1, affinity})
      .transform_error([](SourceError error) {
        return error == SourceError::NO_MEMORY ? NavigationCheckpointError::NO_MEMORY
                                               : NavigationCheckpointError::CORRUPT_INPUT;
      });
  }

private:
  ModelResource resource_;
};

inline ManagedError commandError(SourceError error) {
  return {ManagedPhase::STATE,
          {},
          error == SourceError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT,
          {},
          {}};
}

inline ManagedError commandError(NavigationCheckpointError error) {
  return {ManagedPhase::STATE,
          {},
          error == NavigationCheckpointError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT,
          {},
          {}};
}

// The app uses only immutable revision-one documents; no file or source data is exported.
inline std::expected<void, ManagedError> seek(const ui::FiniteTextDocument &note, std::size_t offset) {
  auto point = note.bookmark(offset);
  if (!point) {
    return std::unexpected(commandError(point.error()));
  }

  return note.restore(*point);
}

inline std::expected<void, ManagedError> remember(const ui::FiniteTextDocument &note, NavigationCheckpoint &saved) {
  auto value = note.source().value().snapshot();
  if (!value) {
    return std::unexpected(value.error());
  }

  if ((*value)->value.revision != 1 || (*value)->value.text != NOTE) {
    return std::unexpected(commandError(NavigationCheckpointError::WRONG_RESOURCE));
  }

  auto point = note.bookmark((*value)->value.selection.cursor);
  if (!point) {
    return std::unexpected(commandError(point.error()));
  }

  const std::array entries{NavigationCheckpointEntry{NavigationCheckpointRole::READING_ANCHOR, *point}};
  auto checkpoint = NavigationCheckpoint::capture(entries, NoteCodec(point->resource()));
  if (!checkpoint) {
    return std::unexpected(commandError(checkpoint.error()));
  }

  saved = std::move(*checkpoint);

  return {};
}

inline std::expected<void, ManagedError> resume(const ui::FiniteTextDocument &note, const NavigationCheckpoint &saved) {
  auto value = note.source().value().snapshot();
  if (!value) {
    return std::unexpected(value.error());
  }

  if ((*value)->value.revision != 1 || (*value)->value.text != NOTE) {
    return std::unexpected(commandError(NavigationCheckpointError::WRONG_RESOURCE));
  }

  auto origin = note.bookmark(0);
  if (!origin) {
    return std::unexpected(commandError(origin.error()));
  }

  // parse copies the bytes as a new owner, as when bytes arrive from application storage.
  auto parsed = NavigationCheckpoint::parse(saved.bytes());
  if (!parsed) {
    return std::unexpected(commandError(parsed.error()));
  }

  auto positions = parsed->restore(NoteCodec(origin->resource()));
  if (!positions) {
    return std::unexpected(commandError(positions.error()));
  }

  if (positions->size() != 1 || positions->front().role != NavigationCheckpointRole::READING_ANCHOR) {
    return std::unexpected(commandError(NavigationCheckpointError::CORRUPT_INPUT));
  }

  return note.restore(positions->front().bookmark);
}
} // namespace checkpoint_example
