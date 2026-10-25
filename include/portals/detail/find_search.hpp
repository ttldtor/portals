// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/find_model_source.hpp>

#include <stop_token>

namespace portals::detail {
/// @cond INTERNAL
struct FindDocument {
  std::unique_ptr<GraphemeText> text;
  std::vector<unsigned char> boundaries = std::vector<unsigned char>(0);
};

struct FindRange {
  std::size_t begin{};
  std::size_t end{};
  std::shared_ptr<const FindDocument> document;
  RowId row;
  std::uint64_t rowOrdinal{};
};

// A search result contains ranges, not a page of model rows. Several ranges may refer to one row.
struct FindResult {
  std::shared_ptr<const FindDocument> document;
  std::string query = std::string(0, '\0');
  std::vector<FindRange> ranges = std::vector<FindRange>(0);
  bool truncated{};
  std::shared_ptr<const FindWorkerSource> source;
  std::uint64_t session{};
  std::uint64_t generation{};
};

struct FindSearchInput {
  std::shared_ptr<const FindWorkerSource> source;
  std::string query = std::string(0, '\0');
  FindSearchOptions options;
  WidthPolicy policy;
  std::uint64_t session{};
  std::uint64_t generation{};
};

struct FindSearchCompletion {
  std::shared_ptr<const FindSearchInput> input;
  std::expected<std::shared_ptr<const FindResult>, FindSearchError> result{std::unexpected(FindSearchError::CANCELLED)};
};

std::expected<std::shared_ptr<const FindResult>, FindSearchError> searchModel(const FindSearchInput &input,
                                                                              std::stop_token stop);
/// @endcond
} // namespace portals::detail
