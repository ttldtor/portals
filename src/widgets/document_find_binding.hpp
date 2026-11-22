// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "document_view_search.hpp"
#include <portals/ui_document_search.hpp>

namespace portals::ui::detail {
struct DocumentFindLocation {
  std::uint64_t byteOffset{};
  std::optional<std::uint64_t> fileGeneration;
};

// UI snapshot owns only worker-safe data. The locator must never capture a Ref,
// scene, tree or widget; file-backed views use it to prove a record-start anchor.
struct DocumentFindSnapshot {
  std::shared_ptr<ByteDocumentSource> source;
  DocumentDecoderOptions decoder;
  std::uint64_t navigation{};
  std::optional<DocumentRevision> revision;
  std::function<std::expected<DocumentFindLocation, DocumentError>(const DocumentSearchMatch &, std::stop_token)>
    locate;
};

struct DocumentFindSelection {
  portals::detail::DocumentSearchSelection range;
  std::uint64_t revealByte{};
  std::optional<std::uint64_t> fileGeneration;
};

struct DocumentFindTarget {
  virtual ~DocumentFindTarget() = default;

  // Preserve a capability/source failure without changing the common tree bridge.
  virtual std::optional<DocumentOperationFailure> failure() const {
    return {};
  }

  // Optional style participation belongs to the prepared-layout phase, after
  // style metadata is flushed. capture also runs during ordinary UI reads.
  virtual std::expected<void, ManagedError> prepareLayout(ManagedScene &) {
    return {};
  }

  // Empty means not mounted yet. An admitted but retired/foreign target fails.
  virtual std::expected<std::optional<DocumentFindSnapshot>, TreeError> capture(ManagedScene &scene) = 0;
  // Recheck source/decoder identity before mutation. reveal is already qualified
  // by navigation intent, but the target must recheck that token as well.
  virtual std::expected<void, ManagedError> select(ManagedScene &scene, const DocumentFindSnapshot &captured,
                                                   std::optional<DocumentFindSelection> selection, bool reveal) = 0;
};

Description<FindBar> documentFindBar(std::shared_ptr<DocumentFindTarget> target, std::string_view initial,
                                     FindBarConfig config, DocumentSearchOptions search);
} // namespace portals::ui::detail
