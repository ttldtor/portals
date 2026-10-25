// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/find_model_source.hpp>
#include <portals/search.hpp>
#include <portals/worker_storage.hpp>

#include <memory_resource>
#include <stop_token>

namespace portals::detail {
/// @cond INTERNAL
struct SearchBudgetExceeded {};

// Count BEFORE upstream allocation, including simultaneous old/new vector capacities.
class SearchMemory final : public std::pmr::memory_resource {
public:
  explicit SearchMemory(std::size_t cap) : cap_(cap) {
  }

  std::size_t used() const noexcept {
    return used_;
  }

  std::size_t peak() const noexcept {
    return peak_;
  }

private:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override;
  void do_deallocate(void *data, std::size_t bytes, std::size_t alignment) override;

  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  std::size_t cap_{}, used_{}, peak_{};
};

struct SearchUnit {
  std::uint32_t byteFirst{}, byteCount{};
  std::uint32_t originalFirst{}, originalLast{};
  bool whitespace{};
};

struct SearchTerm {
  std::uint32_t first{}, count{}, row{};
  bool token{};
};

struct SearchIndexRow {
  RowId id;
  std::uint32_t textFirst{}, textCount{};
};

struct SearchNode {
  std::uint32_t term{}, depth{}, terminalFirst{}, terminalCount{}, childrenFirst{}, childrenCount{};
  std::uint32_t minLength{}, minOrdinal{};
  bool minToken{};
};

class SearchIndex final : public WorkerStorageValue {
public:
  SearchIndex(SearchSourceIdentity identity, SearchOptions options);
  std::size_t bytes() const noexcept;
  SearchSourceIdentity identity;
  SearchOptions options;
  SearchMemory memory;
  std::pmr::string original;
  std::pmr::string normalized;
  std::pmr::vector<SearchUnit> units;
  std::pmr::vector<SearchTerm> terms;
  std::pmr::vector<SearchIndexRow> rows;
  std::pmr::vector<SearchNode> nodes;
  std::pmr::vector<std::uint32_t> children;
};

bool validSearchOptions(const SearchOptions &) noexcept;
std::expected<std::unique_ptr<SearchIndex>, SearchError>
buildSearchIndex(const FindWorkerSource &, SearchSourceIdentity, SearchOptions, std::stop_token);
std::expected<std::shared_ptr<const SearchResult>, SearchError>
querySearchIndex(const SearchIndex &, std::string_view, std::uint64_t generation, std::stop_token);
/// @endcond
} // namespace portals::detail
