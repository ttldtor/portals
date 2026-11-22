// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_gzip.hpp>

namespace portals::ui {
DocumentSource gzip(std::shared_ptr<ByteDocumentProvider> compressed, GzipDocumentOptions options,
                    std::shared_ptr<SourceBudget> budget) {
  try {
    if (!budget) {
      auto created = SourceBudget::create();

      if (!created) {
        return document(std::unexpected(DocumentError{created.error(), {}, {}}));
      }

      budget = std::move(*created);
    }

    auto provider = GzipDocumentProvider::create(std::move(compressed), budget, options);

    if (!provider) {
      return document(std::unexpected(provider.error()));
    }

    return document(std::move(*provider), std::move(budget));
  } catch (const std::bad_alloc &) {
    return document(std::unexpected(DocumentError{SourceError::NO_MEMORY, {}, {}}));
  } catch (...) {
    return document(std::unexpected(DocumentError{SourceError::INVALID_ARGUMENT, {}, {}}));
  }
}

DocumentSource gzipFile(std::string_view path, GzipDocumentOptions options, std::shared_ptr<SourceBudget> budget) {
  try {
    auto compressed = FileDocumentProvider::create(
      std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(path.data()), path.size())));

    if (!compressed) {
      return document(std::unexpected(compressed.error()));
    }

    return gzip(std::move(*compressed), options, std::move(budget));
  } catch (const std::bad_alloc &) {
    return document(std::unexpected(DocumentError{SourceError::NO_MEMORY, {}, {}}));
  } catch (...) {
    return document(std::unexpected(DocumentError{SourceError::INVALID_ARGUMENT, {}, {}}));
  }
}
} // namespace portals::ui
