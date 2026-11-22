// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/gzip_document.hpp>
#include <portals/ui_document_view.hpp>

namespace portals::ui {
/** @param compressed Owning seekable gzip byte provider; do not access it concurrently outside this wrapper.
 * @param options Bounded decompressor and checkpoint storage.
 * @param budget Shared source/view root, or null for SDK defaults.
 * @return Cold logical-byte descriptor; creation failures are collected by the receiving scene.
 * Link the optional portals::gzip target. No metadata, file I/O or decompression runs here.
 */
DocumentSource gzip(std::shared_ptr<ByteDocumentProvider> compressed, GzipDocumentOptions options = {},
                    std::shared_ptr<SourceBudget> budget = {});

/** @param path Owned UTF-8 path, independent of the file extension.
 * @param options Bounded decompressor and checkpoint storage.
 * @param budget Shared source/view root, or null for SDK defaults.
 * @return Cold gzip descriptor. Worker reads validate gzip contents and report corruption instead of clean EOF.
 */
DocumentSource gzipFile(std::string_view path, GzipDocumentOptions options = {},
                        std::shared_ptr<SourceBudget> budget = {});
} // namespace portals::ui
