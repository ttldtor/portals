// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/find_search.hpp>

namespace portals::detail {
namespace {
constexpr std::size_t ROW_READ_BATCH = 256;
constexpr std::size_t CANCEL_BYTE_STRIDE = 4096;

std::expected<std::shared_ptr<const FindDocument>, FindSearchError>
prepareRow(std::string_view bytes, WidthPolicy policy, std::size_t maxBytes) {
  const TextLimits limits{maxBytes, maxBytes, maxBytes, maxBytes * 2};
  auto text = GraphemeText::from(bytes, policy, limits);

  if (!text) {
    return std::unexpected(FindSearchError::INVALID_TEXT);
  }

  auto document = std::make_shared<FindDocument>();

  document->text = std::make_unique<GraphemeText>(std::move(*text));
  document->boundaries.resize(bytes.size() + 1);
  document->boundaries[0] = 1;

  for (const auto &cluster : document->text->graphemes()) {
    document->boundaries[cluster.byte_end] = 1;
  }

  return document;
}

std::expected<void, FindSearchError> scanRow(FindResult &result, const FindRowView &row, const FindSearchInput &input,
                                             std::span<const std::size_t> prefix, std::stop_token stop) {
  auto document = prepareRow(row.text, input.policy, input.options.maxRowBytes);

  if (!document) {
    return std::unexpected(document.error());
  }

  const auto bytes = (*document)->text->bytes();
  const auto query = std::string_view(input.query);

  for (std::size_t offset = 0, matched = 0; offset < bytes.size(); ++offset) {
    if (offset % CANCEL_BYTE_STRIDE == 0 && stop.stop_requested()) {
      return std::unexpected(FindSearchError::CANCELLED);
    }

    while (matched && bytes[offset] != query[matched]) {
      matched = prefix[matched - 1];
    }

    if (bytes[offset] == query[matched]) {
      ++matched;
    }

    if (matched != query.size()) {
      continue;
    }

    const auto end = offset + 1;
    const auto begin = end - query.size();

    matched = prefix[matched - 1];

    if (!(*document)->boundaries[begin] || !(*document)->boundaries[end]) {
      continue;
    }

    if (result.ranges.size() == input.options.maxMatches) {
      result.truncated = true;

      return {};
    }

    result.ranges.push_back({begin, end, *document, row.id, row.ordinal});
  }

  return {};
}
} // namespace

bool validFindSearchOptions(const FindSearchOptions &options) noexcept {
  // Leave room for the per-row terminator and GraphemeText's two-byte accounting ceiling.
  return options.maxSourceRows && options.maxProjectedBytes && options.maxRowBytes && options.maxQueryBytes &&
         options.maxMatches && options.maxRowBytes <= std::numeric_limits<std::size_t>::max() / 2 &&
         options.maxRowBytes <= options.maxProjectedBytes && options.maxQueryBytes <= FindLimits::MAX_QUERY_BYTES &&
         options.maxMatches <= FindLimits::MAX_MATCHES;
}

std::expected<std::shared_ptr<const FindResult>, FindSearchError> searchModel(const FindSearchInput &input,
                                                                              std::stop_token stop) {
  if (!input.source || !input.session || !input.generation || !validFindSearchOptions(input.options)) {
    return std::unexpected(FindSearchError::INVALID_ARGUMENT);
  }

  if (input.source->rowCount > input.options.maxSourceRows || input.query.size() > input.options.maxQueryBytes) {
    return std::unexpected(FindSearchError::LIMIT_EXCEEDED);
  }

  try {
    auto result = std::make_shared<FindResult>();

    result->query = input.query;
    result->source = input.source;
    result->session = input.session;
    result->generation = input.generation;

    if (input.query.empty()) {
      return result;
    }

    std::vector<std::size_t> prefix(input.query.size(), 0);

    for (std::size_t index = 1, matched = 0; index < input.query.size(); ++index) {
      while (matched && input.query[index] != input.query[matched]) {
        matched = prefix[matched - 1];
      }

      if (input.query[index] == input.query[matched]) {
        ++matched;
      }

      prefix[index] = matched;
    }

    std::vector<FindRowView> rows(0);
    std::size_t projectedBytes{};

    rows.reserve(ROW_READ_BATCH);

    for (std::uint64_t first = 0; first < input.source->rowCount; first += rows.size()) {
      if (stop.stop_requested()) {
        return std::unexpected(FindSearchError::CANCELLED);
      }

      const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(ROW_READ_BATCH, input.source->rowCount - first));
      auto read = input.source->read({first, count}, rows);

      if (!read) {
        return std::unexpected(read.error());
      }

      if (rows.size() != count) {
        return std::unexpected(FindSearchError::INVALID_ARGUMENT);
      }

      for (const auto &row : rows) {
        if (stop.stop_requested()) {
          return std::unexpected(FindSearchError::CANCELLED);
        }

        if (row.text.size() > input.options.maxRowBytes ||
            row.text.size() > input.options.maxProjectedBytes - projectedBytes) {
          return std::unexpected(FindSearchError::LIMIT_EXCEEDED);
        }

        projectedBytes += row.text.size();

        auto scanned = scanRow(*result, row, input, prefix, stop);

        if (!scanned) {
          return std::unexpected(scanned.error());
        }

        if (result->truncated) {
          return result;
        }
      }
    }

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(FindSearchError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(FindSearchError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(FindSearchError::CALLBACK_FAILED);
  }
}
} // namespace portals::detail
