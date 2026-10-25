// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/search_index.hpp>

#include <utf8proc.h>

#include <array>

namespace portals::detail {
void SearchMemory::chargeExternal(std::size_t bytes) {
  if (bytes > cap_ - used_) {
    throw SearchBudgetExceeded{};
  }
  used_ += bytes;
  peak_ = std::max(peak_, used_);
}

void SearchMemory::releaseExternal(std::size_t bytes) noexcept {
  used_ -= bytes;
}

void *SearchMemory::do_allocate(std::size_t bytes, std::size_t alignment) {
  if (bytes > cap_ - used_) {
    throw SearchBudgetExceeded{};
  }
  auto data = std::pmr::new_delete_resource()->allocate(bytes, alignment);
  used_ += bytes;
  peak_ = std::max(peak_, used_);

  return data;
}

void SearchMemory::do_deallocate(void *data, std::size_t bytes, std::size_t alignment) {
  std::pmr::new_delete_resource()->deallocate(data, bytes, alignment);
  used_ -= bytes;
}

SearchIndex::SearchIndex(SearchSourceIdentity key, SearchOptions limits)
    : identity(std::move(key)), options(limits), memory(limits.maxIndexBytes - sizeof(SearchIndex)),
      original(0, '\0', &memory), normalized(0, '\0', &memory), units(0, &memory), terms(0, &memory), rows(0, &memory),
      nodes(0, &memory), children(0, &memory) {
}

std::size_t SearchIndex::bytes() const noexcept {
  return sizeof(SearchIndex) + memory.used();
}

bool validSearchOptions(const SearchOptions &options) noexcept {
  constexpr auto MAX_OFFSET = std::numeric_limits<std::uint32_t>::max();

  return options.maxSourceRows && options.maxSourceRows <= MAX_OFFSET && options.maxProjectedBytes &&
         options.maxProjectedBytes <= MAX_OFFSET && options.maxRowBytes &&
         options.maxRowBytes <= options.maxProjectedBytes && options.maxRowGraphemes && options.maxQueryBytes &&
         options.maxQueryBytes <= MAX_OFFSET && options.maxQueryGraphemes && options.maxQueryGraphemes <= 1024 &&
         options.maxTermsPerRow && options.maxIndexBytes > sizeof(SearchIndex) && options.maxIndexBytes <= MAX_OFFSET &&
         options.maxBuildScratchBytes && options.topK && options.topK <= 256 && options.maxHighlightRangesPerItem &&
         options.maxHighlightRangesPerItem <= 1024 && options.maxQuerySteps && options.maxQueryScratchBytes &&
         (options.strategy == SearchStrategy::PREFIX || options.strategy == SearchStrategy::FUZZY_PREFIX);
}

namespace {
constexpr std::size_t ROW_READ_BATCH = 256;
constexpr std::size_t CANCEL_STRIDE = 1024;
constexpr unsigned NORMALIZATION = UTF8PROC_STABLE | UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE;

struct SearchAbort {
  SearchError error;
};

struct WorkCounter {
  std::stop_token stop;
  std::size_t steps{}, limit{std::numeric_limits<std::size_t>::max()};

  void step(std::size_t count = 1) {
    if (count > limit - steps) {
      throw SearchAbort{SearchError::LIMIT_EXCEEDED};
    }
    const auto previous = steps;
    steps += count;

    if ((steps / CANCEL_STRIDE != previous / CANCEL_STRIDE || previous == 0) && stop.stop_requested()) {
      throw SearchAbort{SearchError::CANCELLED};
    }
  }
};

bool whitespace(utf8proc_int32_t scalar) noexcept {
  return (scalar >= 0x09 && scalar <= 0x0d) || scalar == 0x20 || scalar == 0x85 || scalar == 0xa0 || scalar == 0x1680 ||
         (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202f ||
         scalar == 0x205f || scalar == 0x3000;
}

template <class Callback> void eachGrapheme(std::string_view bytes, WorkCounter &work, Callback callback) {
  utf8proc_int32_t previous{}, state{};
  std::size_t first{};
  bool allWhitespace = true;

  for (std::size_t offset = 0; offset < bytes.size();) {
    utf8proc_int32_t scalar{};
    const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(bytes.data() + offset),
                                        static_cast<utf8proc_ssize_t>(bytes.size() - offset), &scalar);
    work.step();

    if (count <= 0) {
      throw SearchAbort{SearchError::INVALID_TEXT};
    }
    if (offset && utf8proc_grapheme_break_stateful(previous, scalar, &state)) {
      callback(first, offset, allWhitespace);
      first = offset;
      allWhitespace = true;
    }
    allWhitespace = allWhitespace && whitespace(scalar);
    previous = scalar;
    offset += static_cast<std::size_t>(count);
  }
  if (!bytes.empty()) {
    callback(first, bytes.size(), allWhitespace);
  }
}

void normalize(std::string_view bytes, std::pmr::string &output, std::pmr::vector<SearchUnit> &units,
               std::pmr::memory_resource *scratch, std::size_t maxOriginalGraphemes, WorkCounter &work) {
  std::size_t originalCount{};

  eachGrapheme(bytes, work, [&](std::size_t first, std::size_t last, bool delimiter) {
    if (++originalCount > maxOriginalGraphemes) {
      throw SearchAbort{SearchError::LIMIT_EXCEEDED};
    }
    const auto input = reinterpret_cast<const utf8proc_uint8_t *>(bytes.data() + first);
    const auto count =
      utf8proc_decompose(input, static_cast<utf8proc_ssize_t>(last - first), nullptr, 0, NORMALIZATION);

    if (count < 0) {
      throw SearchAbort{SearchError::INVALID_TEXT};
    }
    std::pmr::vector<utf8proc_int32_t> scalars(static_cast<std::size_t>(count) + 1, 0, scratch);
    const auto decomposed =
      utf8proc_decompose(input, static_cast<utf8proc_ssize_t>(last - first), scalars.data(), count, NORMALIZATION);
    const auto encoded = utf8proc_reencode(scalars.data(), decomposed, NORMALIZATION);

    if (encoded < 0) {
      throw SearchAbort{SearchError::INVALID_TEXT};
    }
    const std::string_view folded(reinterpret_cast<const char *>(scalars.data()), static_cast<std::size_t>(encoded));

    eachGrapheme(folded, work, [&](std::size_t begin, std::size_t end, bool) {
      if (output.size() > std::numeric_limits<std::uint32_t>::max() - (end - begin)) {
        throw SearchAbort{SearchError::LIMIT_EXCEEDED};
      }
      const auto position = static_cast<std::uint32_t>(output.size());

      output.append(folded.substr(begin, end - begin));
      units.push_back({position, static_cast<std::uint32_t>(end - begin), static_cast<std::uint32_t>(first),
                       static_cast<std::uint32_t>(last), delimiter});
    });
  });
}

std::string_view unit(const SearchIndex &index, const SearchUnit &value) noexcept {
  return std::string_view(index.normalized).substr(value.byteFirst, value.byteCount);
}

std::string_view unit(const SearchIndex &index, const SearchTerm &term, std::size_t position) noexcept {
  return unit(index, index.units[term.first + position]);
}

bool termLess(const SearchIndex &index, const SearchTerm &left, const SearchTerm &right, WorkCounter &work) {
  for (std::size_t position = 0; position < std::min(left.count, right.count); ++position) {
    work.step();
    const auto compare = unit(index, left, position).compare(unit(index, right, position));

    if (compare) {
      return compare < 0;
    }
  }

  return std::tie(left.count, left.token, left.row) < std::tie(right.count, right.token, right.row);
}

void buildNodes(SearchIndex &index, std::pmr::memory_resource *scratch, WorkCounter &work) {
  struct Task {
    std::size_t first{}, last{}, parentDepth{};
    std::uint32_t node{};
  };

  // Nested source prefixes must consume the declared scratch budget, never the
  // native worker stack. Vector growth charges simultaneous old/new capacities.
  std::pmr::vector<Task> tasks(0, scratch);
  tasks.push_back({0, index.terms.size(), 0, 0});
  index.nodes.emplace_back();

  for (std::size_t cursor = 0; cursor < tasks.size(); ++cursor) {
    const auto task = tasks[cursor];
    const auto &left = index.terms[task.first];
    const auto &right = index.terms[task.last - 1];
    std::size_t depth = task.parentDepth;

    while (depth < std::min(left.count, right.count) && unit(index, left, depth) == unit(index, right, depth)) {
      ++depth;
      work.step();
    }
    SearchNode node;
    node.term = static_cast<std::uint32_t>(task.first);
    node.depth = static_cast<std::uint32_t>(depth);
    node.minLength = std::numeric_limits<std::uint32_t>::max();
    node.minOrdinal = std::numeric_limits<std::uint32_t>::max();
    node.minToken = true;

    for (std::size_t position = task.first; position < task.last; ++position) {
      const auto &term = index.terms[position];
      node.minLength = std::min(node.minLength, term.count);
      node.minOrdinal = std::min(node.minOrdinal, term.row);
      node.minToken = node.minToken && term.token;
      work.step();
    }
    auto position = task.first;
    while (position < task.last && index.terms[position].count == depth) {
      ++position;
    }
    node.terminalFirst = static_cast<std::uint32_t>(task.first);
    node.terminalCount = static_cast<std::uint32_t>(position - task.first);
    node.childrenFirst = static_cast<std::uint32_t>(index.children.size());

    while (position < task.last) {
      auto end = position + 1;
      while (end < task.last && unit(index, index.terms[position], depth) == unit(index, index.terms[end], depth)) {
        ++end;
        work.step();
      }
      const auto child = static_cast<std::uint32_t>(index.nodes.size());
      index.nodes.emplace_back();
      index.children.push_back(child);
      tasks.push_back({position, end, depth + 1, child});
      position = end;
    }
    node.childrenCount = static_cast<std::uint32_t>(index.children.size() - node.childrenFirst);
    index.nodes[task.node] = node;
  }
}

SearchError sourceError(FindSearchError error) noexcept {
  switch (error) {
  case FindSearchError::NOT_READY:
    return SearchError::NOT_READY;
  case FindSearchError::NO_MEMORY:
    return SearchError::NO_MEMORY;
  case FindSearchError::CALLBACK_FAILED:
    return SearchError::CALLBACK_FAILED;
  default:
    return SearchError::INVALID_ARGUMENT;
  }
}
} // namespace

std::expected<std::unique_ptr<SearchIndex>, SearchError> buildSearchIndex(const FindWorkerSource &source,
                                                                          SearchSourceIdentity identity,
                                                                          SearchOptions options, std::stop_token stop) {
  if (!validSearchOptions(options) || !identity.session || !identity.pin || identity.owner.expired()) {
    return std::unexpected(SearchError::INVALID_ARGUMENT);
  }
  if (source.rowCount > options.maxSourceRows) {
    return std::unexpected(SearchError::LIMIT_EXCEEDED);
  }
  constexpr auto BATCH_STORAGE_BYTES = ROW_READ_BATCH * sizeof(FindRowView);

  if (options.maxBuildScratchBytes <= BATCH_STORAGE_BYTES) {
    return std::unexpected(SearchError::LIMIT_EXCEEDED);
  }
  try {
    auto index = std::make_unique<SearchIndex>(std::move(identity), options);
    SearchMemory scratch(options.maxBuildScratchBytes);
    WorkCounter work{stop};
    std::vector<FindRowView> batch(0);
    std::pmr::vector<std::pair<RowId, std::size_t>> identities(0, &scratch);

    batch.reserve(ROW_READ_BATCH);
    // Adapter read storage is fixed to one bounded batch; it never grows during a valid read.
    if (batch.capacity() * sizeof(FindRowView) > options.maxBuildScratchBytes) {
      return std::unexpected(SearchError::LIMIT_EXCEEDED);
    }
    scratch.chargeExternal(batch.capacity() * sizeof(FindRowView));
    index->rows.reserve(static_cast<std::size_t>(source.rowCount));

    for (std::uint64_t first = 0; first < source.rowCount;) {
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(ROW_READ_BATCH, source.rowCount - first));
      auto read = source.read({first, count}, batch, &scratch);

      if (!read) {
        return std::unexpected(sourceError(read.error()));
      }
      if (batch.size() != count) {
        return std::unexpected(SearchError::INVALID_ARGUMENT);
      }
      for (const auto &row : batch) {
        work.step();

        if (!row.id.value || !row.id.generation) {
          return std::unexpected(SearchError::INVALID_ARGUMENT);
        }
        if (row.text.size() > options.maxRowBytes ||
            row.text.size() > options.maxProjectedBytes - index->original.size()) {
          return std::unexpected(SearchError::LIMIT_EXCEEDED);
        }
        const auto rowId = static_cast<std::uint32_t>(index->rows.size());
        const auto unitFirst = static_cast<std::uint32_t>(index->units.size());

        index->rows.push_back(
          {row.id, static_cast<std::uint32_t>(index->original.size()), static_cast<std::uint32_t>(row.text.size())});
        identities.emplace_back(row.id, rowId);
        index->original.append(row.text);
        normalize(row.text, index->normalized, index->units, &scratch, options.maxRowGraphemes, work);
        const auto unitEnd = static_cast<std::uint32_t>(index->units.size());
        const auto length = unitEnd - unitFirst;

        if (!length) {
          continue;
        }
        const auto rowTermFirst = index->terms.size();

        index->terms.push_back({unitFirst, length, rowId, false});
        std::size_t termCount = 1;

        for (auto position = unitFirst; position < unitEnd;) {
          if (index->units[position].whitespace) {
            ++position;
            continue;
          }
          const auto begin = position;

          while (position < unitEnd && !index->units[position].whitespace) {
            ++position;
          }
          if (begin == unitFirst && position == unitEnd) {
            continue;
          }
          const SearchTerm token{begin, position - begin, rowId, true};
          bool duplicate{};

          for (auto termId = rowTermFirst; termId < index->terms.size(); ++termId) {
            const auto &existing = index->terms[termId];

            if (existing.count == token.count) {
              duplicate = true;

              for (std::size_t offset = 0; offset < token.count; ++offset) {
                work.step();

                if (unit(*index, existing, offset) != unit(*index, token, offset)) {
                  duplicate = false;
                  break;
                }
              }
              if (duplicate) {
                break;
              }
            }
          }
          if (duplicate) {
            continue;
          }
          if (++termCount > options.maxTermsPerRow) {
            return std::unexpected(SearchError::LIMIT_EXCEEDED);
          }
          index->terms.push_back(token);
        }
      }
      first += count;
    }
    std::ranges::sort(identities, [&](const auto &left, const auto &right) {
      work.step();
      return std::tie(left.first.value, left.first.generation) < std::tie(right.first.value, right.first.generation);
    });

    for (std::size_t position = 1; position < identities.size(); ++position) {
      if (identities[position - 1].first == identities[position].first) {
        return std::unexpected(SearchError::INVALID_ARGUMENT);
      }
    }
    std::ranges::sort(index->terms, [&](const auto &left, const auto &right) {
      return termLess(*index, left, right, work);
    });

    if (!index->terms.empty()) {
      buildNodes(*index, &scratch, work);
    }
    if (stop.stop_requested()) {
      return std::unexpected(SearchError::CANCELLED);
    }

    return index;
  } catch (SearchAbort abort) {
    return std::unexpected(abort.error);
  } catch (SearchBudgetExceeded) {
    return std::unexpected(SearchError::LIMIT_EXCEEDED);
  } catch (const std::bad_alloc &) {
    return std::unexpected(SearchError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(SearchError::CALLBACK_FAILED);
  }
}

namespace {
struct Candidate {
  SearchRank rank;
  std::uint32_t node{}, term{}, terminalEnd{}, depth{};
  bool posting{};
};

struct CandidateLater {
  bool operator()(const Candidate &left, const Candidate &right) const noexcept {
    return left.rank.tuple() > right.rank.tuple();
  }
};

// Do not move a PMR container into priority_queue: MSVC's noexcept move can allocate
// an iterator proxy. A deliberately exhausted budget must remain a recoverable error.
class Frontier {
public:
  explicit Frontier(std::pmr::memory_resource *memory) : values_(0, memory) {
  }

  bool empty() const noexcept {
    return values_.empty();
  }

  const Candidate &top() const noexcept {
    return values_.front();
  }

  void pop() {
    std::pop_heap(values_.begin(), values_.end(), CandidateLater{});
    values_.pop_back();
  }

  void push(const Candidate &value) {
    values_.push_back(value);
    std::push_heap(values_.begin(), values_.end(), CandidateLater{});
  }

private:
  std::pmr::vector<Candidate> values_;
};

SearchRank lowerBound(const SearchIndex &index, std::uint32_t nodeId, std::uint8_t edits, std::uint32_t depth) {
  const auto &node = index.nodes[nodeId];

  return {edits, node.minToken, node.minLength - depth, node.minOrdinal, {}};
}

SearchRank exactRank(const SearchIndex &index, std::uint32_t termId, std::uint8_t edits, std::uint32_t depth) {
  const auto &term = index.terms[termId];

  return {edits, term.token, term.count - depth, term.row, index.rows[term.row].id};
}

void prefixRegion(const SearchIndex &index, std::string_view query, std::span<const SearchUnit> queryUnits,
                  Frontier &frontier, WorkCounter &work) {
  std::uint32_t nodeId{};
  std::size_t parentDepth{};

  for (;;) {
    const auto &node = index.nodes[nodeId];
    const auto &term = index.terms[node.term];

    for (auto depth = parentDepth; depth < std::min<std::size_t>(node.depth, queryUnits.size()); ++depth) {
      const auto &wanted = queryUnits[depth];
      work.step();

      if (unit(index, term, depth) != query.substr(wanted.byteFirst, wanted.byteCount)) {
        return;
      }
    }
    if (queryUnits.size() <= node.depth) {
      const auto depth = static_cast<std::uint32_t>(queryUnits.size());

      frontier.push({lowerBound(index, nodeId, 0, depth), nodeId, 0, 0, depth, false});
      work.step();
      return;
    }
    const auto &wanted = queryUnits[node.depth];
    const auto symbol = query.substr(wanted.byteFirst, wanted.byteCount);
    std::size_t first{}, last = node.childrenCount;

    while (first < last) {
      const auto middle = first + (last - first) / 2;
      const auto child = index.children[node.childrenFirst + middle];
      const auto compare = unit(index, index.terms[index.nodes[child].term], node.depth).compare(symbol);
      work.step();

      if (compare < 0) {
        first = middle + 1;
      } else {
        last = middle;
      }
    }
    if (first == node.childrenCount) {
      return;
    }
    const auto child = index.children[node.childrenFirst + first];

    if (unit(index, index.terms[index.nodes[child].term], node.depth) != symbol) {
      return;
    }
    parentDepth = node.depth;
    nodeId = child;
  }
}

void regions(const SearchIndex &index, std::uint32_t nodeId, std::size_t parentDepth,
             std::span<const std::uint16_t> parent, std::string_view query, std::span<const SearchUnit> queryUnits,
             unsigned edits, Frontier &frontier, std::pmr::memory_resource *scratch, WorkCounter &work) {
  const auto &node = index.nodes[nodeId];
  const auto &term = index.terms[node.term];
  std::pmr::vector<std::uint16_t> previous(parent.begin(), parent.end(), scratch);
  std::pmr::vector<std::uint16_t> current(previous.size(), 0, scratch);
  const auto last = std::min<std::size_t>(node.depth, queryUnits.size() + edits);

  for (std::size_t depth = parentDepth; depth < last; ++depth) {
    current[0] = static_cast<std::uint16_t>(depth + 1);

    for (std::size_t column = 1; column < current.size(); ++column) {
      const auto &symbol = queryUnits[column - 1];
      const bool same = unit(index, term, depth) == query.substr(symbol.byteFirst, symbol.byteCount);

      current[column] = static_cast<std::uint16_t>(std::min(
        {unsigned(previous[column]) + 1, unsigned(current[column - 1]) + 1, unsigned(previous[column - 1]) + !same}));
      work.step();
    }
    previous.swap(current);

    if (previous.back() <= edits) {
      const auto matchedDepth = static_cast<std::uint32_t>(depth + 1);

      frontier.push({lowerBound(index, nodeId, static_cast<std::uint8_t>(previous.back()), matchedDepth), nodeId, 0, 0,
                     matchedDepth, false});
      work.step();
    }
    if (*std::ranges::min_element(previous) > edits) {
      return;
    }
  }
  if (node.depth >= queryUnits.size() + edits) {
    return;
  }
  for (std::size_t child = 0; child < node.childrenCount; ++child) {
    regions(index, index.children[node.childrenFirst + child], node.depth, previous, query, queryUnits, edits, frontier,
            scratch, work);
  }
}

void highlights(const SearchIndex &index, const SearchTerm &term, std::size_t depth, std::string_view query,
                std::span<const SearchUnit> queryUnits, std::pmr::memory_resource *scratch, WorkCounter &work,
                std::vector<SearchHighlight> &result) {
  const auto columns = queryUnits.size() + 1;
  std::pmr::vector<std::uint16_t> dp((depth + 1) * columns, 0, scratch);

  for (std::size_t row = 0; row <= depth; ++row) {
    dp[row * columns] = static_cast<std::uint16_t>(row);
  }
  for (std::size_t column = 0; column < columns; ++column) {
    dp[column] = static_cast<std::uint16_t>(column);
  }
  for (std::size_t row = 1; row <= depth; ++row) {
    for (std::size_t column = 1; column < columns; ++column) {
      const auto &symbol = queryUnits[column - 1];
      const bool same = unit(index, term, row - 1) == query.substr(symbol.byteFirst, symbol.byteCount);

      dp[row * columns + column] = static_cast<std::uint16_t>(
        std::min({unsigned(dp[(row - 1) * columns + column]) + 1, unsigned(dp[row * columns + column - 1]) + 1,
                  unsigned(dp[(row - 1) * columns + column - 1]) + !same}));
      work.step();
    }
  }
  std::pmr::vector<SearchHighlight> reverse(0, scratch);
  std::size_t row = depth, column = queryUnits.size();

  while (row || column) {
    if (row && column) {
      const auto &symbol = queryUnits[column - 1];
      const bool same = unit(index, term, row - 1) == query.substr(symbol.byteFirst, symbol.byteCount);

      if (dp[row * columns + column] == dp[(row - 1) * columns + column - 1] + !same) {
        const auto &original = index.units[term.first + row - 1];
        reverse.push_back({original.originalFirst, original.originalLast});
        --row;
        --column;
        continue;
      }
    }
    if (row && dp[row * columns + column] == dp[(row - 1) * columns + column] + 1) {
      --row;
    } else {
      --column;
    }
  }
  for (auto position = reverse.rbegin(); position != reverse.rend(); ++position) {
    if (!result.empty() && position->begin <= result.back().end) {
      result.back().end = std::max(result.back().end, position->end);
    } else {
      if (result.size() == index.options.maxHighlightRangesPerItem) {
        throw SearchAbort{SearchError::LIMIT_EXCEEDED};
      }
      result.push_back(*position);
    }
  }
}

SearchItem prepareItem(const SearchIndex &index, const Candidate &candidate, std::string_view query,
                       std::span<const SearchUnit> queryUnits, std::pmr::memory_resource *scratch, WorkCounter &work) {
  const auto &term = index.terms[candidate.term];
  const auto &row = index.rows[term.row];
  const auto bytes = std::string_view(index.original).substr(row.textFirst, row.textCount);
  const TextLimits limits{index.options.maxRowBytes, index.options.maxRowBytes, index.options.maxRowGraphemes,
                          index.options.maxRowBytes * 2};
  auto text = GraphemeText::from(bytes, index.identity.width, limits);

  if (!text) {
    throw SearchAbort{SearchError::INVALID_TEXT};
  }
  SearchItem item;

  item.rank = candidate.rank;
  highlights(index, term, candidate.depth, query, queryUnits, scratch, work, item.highlights);
  auto presentation = std::make_shared<PreparedListText>(std::move(*text));
  std::size_t first{};
  std::int32_t column{};

  const auto append = [&](std::size_t last, bool marked) {
    if (first == last) {
      return;
    }
    auto run = GraphemeText::from(bytes.substr(first, last - first), index.identity.width, limits);

    if (!run) {
      throw SearchAbort{SearchError::INVALID_TEXT};
    }
    const auto width = run->measure();

    presentation->runs.push_back({std::move(*run), column, marked});
    column += width;
    first = last;
  };
  for (const auto &highlight : item.highlights) {
    append(highlight.begin, false);
    append(highlight.end, true);
  }
  append(bytes.size(), false);
  item.presentation = std::move(presentation);

  return item;
}
} // namespace

std::expected<std::shared_ptr<const SearchResult>, SearchError>
querySearchIndex(const SearchIndex &index, std::string_view query, std::uint64_t generation, std::stop_token stop) {
  if (!generation || query.size() > index.options.maxQueryBytes) {
    return std::unexpected(query.size() > index.options.maxQueryBytes ? SearchError::LIMIT_EXCEEDED
                                                                      : SearchError::INVALID_ARGUMENT);
  }
  try {
    auto result = std::make_shared<SearchResult>();

    result->source = index.identity;
    result->query = query;
    result->generation = generation;

    if (query.empty() || index.nodes.empty()) {
      return result;
    }
    SearchMemory scratch(index.options.maxQueryScratchBytes);
    WorkCounter work{stop, 0, index.options.maxQuerySteps};
    std::pmr::string normalized(0, '\0', &scratch);
    std::pmr::vector<SearchUnit> queryUnits(0, &scratch);

    normalize(query, normalized, queryUnits, &scratch, index.options.maxQueryGraphemes, work);

    if (queryUnits.size() > index.options.maxQueryGraphemes) {
      return std::unexpected(SearchError::LIMIT_EXCEEDED);
    }
    Frontier frontier(&scratch);
    std::pmr::vector<std::uint16_t> initial(queryUnits.size() + 1, 0, &scratch);

    for (std::size_t column = 0; column < initial.size(); ++column) {
      initial[column] = static_cast<std::uint16_t>(column);
    }
    const unsigned edits = index.options.strategy == SearchStrategy::FUZZY_PREFIX && queryUnits.size() >= 3 ? 1 : 0;

    if (edits) {
      regions(index, 0, 0, initial, normalized, queryUnits, edits, frontier, &scratch, work);
    } else {
      prefixRegion(index, normalized, queryUnits, frontier, work);
    }
    std::pmr::vector<std::uint32_t> seen(0, &scratch);

    while (!frontier.empty()) {
      auto candidate = frontier.top();

      frontier.pop();
      work.step();

      if (candidate.posting) {
        const auto row = index.terms[candidate.term].row;
        const auto found = std::lower_bound(seen.begin(), seen.end(), row, [&](auto value, auto wanted) {
          work.step();
          return value < wanted;
        });
        if (found == seen.end() || *found != row) {
          work.step(static_cast<std::size_t>(seen.end() - found));
          seen.insert(found, row);
          if (result->rows.size() == index.options.topK) {
            result->hasMore = true;
            break;
          }
          auto item = prepareItem(index, candidate, normalized, queryUnits, &scratch, work);

          result->rows.push_back({item.rank.row, std::move(item)});
        }
        if (++candidate.term < candidate.terminalEnd) {
          candidate.rank = exactRank(index, candidate.term, candidate.rank.edits, candidate.depth);
          frontier.push(candidate);
          work.step();
        }
        continue;
      }
      const auto &node = index.nodes[candidate.node];

      if (node.terminalCount) {
        frontier.push({exactRank(index, node.terminalFirst, candidate.rank.edits, candidate.depth), candidate.node,
                       node.terminalFirst, node.terminalFirst + node.terminalCount, candidate.depth, true});
        work.step();
      }
      for (std::size_t offset = 0; offset < node.childrenCount; ++offset) {
        const auto child = index.children[node.childrenFirst + offset];

        frontier.push(
          {lowerBound(index, child, candidate.rank.edits, candidate.depth), child, 0, 0, candidate.depth, false});
        work.step();
      }
    }
    if (stop.stop_requested()) {
      return std::unexpected(SearchError::CANCELLED);
    }
    result->querySteps = work.steps;

    return result;
  } catch (SearchAbort abort) {
    return std::unexpected(abort.error);
  } catch (SearchBudgetExceeded) {
    return std::unexpected(SearchError::LIMIT_EXCEEDED);
  } catch (const std::bad_alloc &) {
    return std::unexpected(SearchError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(SearchError::CALLBACK_FAILED);
  }
}
} // namespace portals::detail
