// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_find_binding.hpp"
#include "external_log_view.hpp"
#include "log_view_state.hpp"
#include <portals/detail/ui_recipe_access.hpp>
#include <portals/ui_log_view.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>
#include <limits>

namespace portals::ui::detail {
namespace {
TreeError sourceTreeError(SourceError error) noexcept {
  return error == SourceError::NO_MEMORY     ? TreeError::NO_MEMORY
         : error == SourceError::WOULD_BLOCK ? TreeError::WOULD_BLOCK
                                             : TreeError::INVALID_ARGUMENT;
}

DocumentError recordReadFailure(const ByteDocumentSource &source, DocumentError error) noexcept {
  // Source provider failures already include their enclosing read workspace.
  // A page reservation refusal returns before that qualification is attached.
  if (!error.budgetPeak && error.budgetDemand && error.budgetDemand->pages) {
    auto peak = *error.budgetDemand;
    constexpr auto MAX = std::numeric_limits<std::size_t>::max();

    if (peak.bytes > MAX - source.envelopeBytes() || peak.work == MAX) {
      error.code = SourceError::PAGE_TOO_LARGE;

      return error;
    }

    peak.bytes += source.envelopeBytes();
    ++peak.work;
    error.budgetPeak = peak;
  }

  return error;
}

// A file-log seek consumes a record start, whereas the search reports the exact
// selected bytes. Prove that start on the worker without materializing the line.
std::expected<std::uint64_t, DocumentError> recordStart(const std::shared_ptr<ByteDocumentSource> &source,
                                                        const DocumentSearchMatch &match, std::stop_token stop) {
  auto cursor = match.byteOffset;
  bool skipCr{};

  if (cursor) {
    auto current = source->read({match.revision, cursor, 1, 1}, stop);

    if (!current) {
      return std::unexpected(recordReadFailure(*source, current.error()));
    }

    skipCr = !current->bytes().empty() && current->bytes().front() == std::byte{'\n'};
  }

  while (cursor) {
    if (stop.stop_requested()) {
      return std::unexpected(DocumentError{SourceError::CANCELLED, {}, {}});
    }

    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(cursor, source->maxPageBytes()));
    const auto begin = cursor - count;
    auto page = source->read({match.revision, begin, count, 1}, stop);

    if (!page) {
      return std::unexpected(recordReadFailure(*source, page.error()));
    }

    if (page->bytes().size() != count) {
      return std::unexpected(DocumentError{SourceError::INVALID_REPLY, {}, {}});
    }

    for (std::size_t index = count; index; --index) {
      const auto byte = page->bytes()[index - 1];

      if (byte == std::byte{'\n'} || (byte == std::byte{'\r'} && !skipCr)) {
        return begin + index;
      }

      skipCr = false;
    }

    cursor = begin;
  }

  return 0;
}

class FileLogFindTarget final : public DocumentFindTarget {
public:
  explicit FileLogFindTarget(Ref<LogView> reference) : reference_(std::move(reference)) {
  }

  std::expected<std::optional<DocumentFindSnapshot>, TreeError> capture(ManagedScene &scene) override {
    const auto node = reference_.node();

    if (!node.id().session) {
      return std::optional<DocumentFindSnapshot>{};
    }

    auto &tree = SceneAccess::application(scene).tree();
    std::shared_ptr<portals::detail::ExternalLogState> state;
    std::uint64_t navigation{};
    const auto read = tree.read(node, [&](const LogView &widget) {
      state = portals::detail::LogViewAccess::state(widget).external;
      const auto intent = portals::detail::ScrollWidgetAccess::state(widget).intent.lock();

      navigation = intent ? intent->serial : state ? state->intentSerial : 0;
    });

    if (!read) {
      return std::unexpected(read.error());
    }

    const auto binding = state ? portals::detail::externalLogSource(*state) : nullptr;
    unsupported_ = !binding || !binding->file || !binding->budget;

    if (unsupported_) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if (binding_ != binding || (generation_ && state->fileGeneration && generation_ != state->fileGeneration)) {
      auto provider = FileDocumentProvider::create(binding->file);

      if (!provider) {
        return std::unexpected(sourceTreeError(provider.error().code));
      }

      auto source =
        ByteDocumentSource::create(*provider, binding->budget, {.maxPageBytes = binding->file->options().ioBlockBytes});

      if (!source) {
        return std::unexpected(sourceTreeError(source.error().code));
      }

      binding_ = binding;
      generation_ = state->fileGeneration;
      source_ = std::move(*source);
    }

    DocumentFindSnapshot snapshot;

    snapshot.source = source_;
    snapshot.decoder.hint = DocumentEncoding::UTF8;
    snapshot.navigation = navigation;
    snapshot.locate = [source = source_, model = binding->file](
                        const DocumentSearchMatch &match,
                        std::stop_token stop) -> std::expected<DocumentFindLocation, DocumentError> {
      const auto start = recordStart(source, match, stop);

      if (!start) {
        return std::unexpected(start.error());
      }

      const auto file = model->metadata(stop);

      if (!file) {
        const auto code = file.error().error == FileLogError::CANCELLED      ? SourceError::CANCELLED
                          : file.error().error == FileLogError::NO_MEMORY    ? SourceError::NO_MEMORY
                          : file.error().error == FileLogError::FILE_CHANGED ? SourceError::REVISION_CHANGED
                                                                             : SourceError::IO_ERROR;
        DocumentError failure{code, {}, file.error()};
        return std::unexpected(failure);
      }

      const auto observed = source->inspect(stop);

      if (!observed) {
        return std::unexpected(observed.error());
      }

      if (observed->revision != match.revision) {
        return std::unexpected(DocumentError{SourceError::REVISION_CHANGED, {}, {}});
      }

      return DocumentFindLocation{*start, file->generation};
    };
    generation_ = state->fileGeneration;

    return std::optional<DocumentFindSnapshot>{std::move(snapshot)};
  }

  std::expected<void, ManagedError> prepareLayout(ManagedScene &scene) override {
    const auto node = reference_.node();

    if (!node.id().session) {
      return {};
    }

    std::shared_ptr<portals::detail::ExternalLogState> state;
    auto read = SceneAccess::application(scene).tree().read(node, [&](const LogView &view) {
      state = portals::detail::LogViewAccess::state(view).external;
    });

    if (!read) {
      return std::unexpected(error(node, read.error()));
    }

    if (!state || state->search) {
      return {};
    }

    auto extension = std::make_unique<portals::detail::ExternalLogSearch>();
    auto roles = SceneAccess::registerPreparedRoles(scene, node, {"match"});

    if (!roles) {
      return std::unexpected(roles.error());
    }

    extension->roles = std::move(*roles);
    state->search = std::move(extension);

    return {};
  }

  std::expected<void, ManagedError> select(ManagedScene &scene, const DocumentFindSnapshot &captured,
                                           std::optional<DocumentFindSelection> selection, bool reveal) override {
    auto current = capture(scene);
    const auto node = reference_.node();

    if (!current) {
      return std::unexpected(error(node, current.error()));
    }

    if (!*current || (*current)->source != captured.source) {
      return {};
    }

    if (selection && selection->fileGeneration && generation_ && *selection->fileGeneration != generation_) {
      // The first metadata reply may already describe a replacement file.
      // Retire this namespace before another current()/prepare can reuse it.
      source_.reset();
      binding_.reset();
      auto failure = error(node, TreeError::INVALID_ARGUMENT);
      failure.source = SourceError::REVISION_CHANGED;
      return std::unexpected(failure);
    }

    if (selection && selection->revealByte > selection->range.begin) {
      return std::unexpected(error(node, TreeError::INVALID_ARGUMENT));
    }

    const bool revealCurrent = reveal && (*current)->navigation == captured.navigation;
    auto &tree = SceneAccess::application(scene).tree();
    const auto edited = tree.edit(node, [&](LogView &widget) {
      auto &state = portals::detail::LogViewAccess::state(widget).external;

      if (state && state->search) {
        state->search->selection = selection ? std::optional(selection->range) : std::nullopt;
        state->search->source = captured.source;
        state->search->fileGeneration = selection && selection->fileGeneration ? *selection->fileGeneration : 0;
        state->search->pendingReveal =
          selection && revealCurrent
            ? std::optional(std::pair{selection->revealByte, selection->range.begin - selection->revealByte})
            : std::nullopt;
      }
    });

    if (!edited) {
      return std::unexpected(error(node, edited.error()));
    }

    const auto invalidated = tree.invalidate(node, Invalidation::PAINT);

    if (!invalidated) {
      return std::unexpected(error(node, invalidated.error()));
    }

    if (!selection || !revealCurrent) {
      return {};
    }

    const auto moved = seekLogByte(SceneAccess::application(scene).tree(), node, selection->revealByte);

    return moved ? std::expected<void, ManagedError>{} : std::unexpected(error(node, sourceTreeError(moved.error())));
  }

  std::optional<DocumentOperationFailure> failure() const override {
    return unsupported_ ? std::optional(DocumentOperationFailure{DocumentError{SourceError::UNSUPPORTED, {}, {}}, {}})
                        : std::nullopt;
  }

private:
  bool unsupported_{};
  Ref<LogView> reference_;
  std::shared_ptr<const LogSourceData> binding_;
  std::shared_ptr<ByteDocumentSource> source_;
  std::uint64_t generation_{};
};
} // namespace
} // namespace portals::ui::detail

namespace portals::ui {
Description<FindBar> findBar(Ref<LogView> target, std::string_view initialQuery, FindBarConfig config,
                             DocumentSearchOptions search) {
  return detail::documentFindBar(std::make_shared<detail::FileLogFindTarget>(std::move(target)), initialQuery,
                                 std::move(config), search);
}

Description<FindBar> findBar(Ref<FindBar> reference, Ref<LogView> target, std::string_view initialQuery,
                             FindBarConfig config, DocumentSearchOptions search) {
  return findBar(std::move(target), initialQuery, std::move(config), search).reference(std::move(reference));
}
} // namespace portals::ui
