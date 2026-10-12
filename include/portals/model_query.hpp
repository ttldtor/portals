// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model.hpp>

#include <concepts>
#include <tuple>

namespace portals {
/** @brief Domain query evaluation failures, separate from scheduler acceptance. */
enum class QueryError {
  INVALID_ARGUMENT, ///< Invalid page, limits, or malformed provider result.
  LIMIT_EXCEEDED,   ///< Source scan or comparison budget is exhausted.
  NOT_READY,        ///< Local source is pending or failed; no I/O is initiated.
  STALE_SOURCE,     ///< Result does not have the exact requested source revision.
  CANCELLED,        ///< Cooperative cancellation was observed.
  NO_MEMORY,        ///< Evaluation could not allocate its bounded storage.
  CALLBACK_FAILED   ///< Domain predicate, comparator, copy, or source read threw.
};

/** @brief Configurable finite query budgets; all values must be positive. */
struct QueryOptions {
  std::size_t maxInFlight{4};          ///< Reservations, including cancelled tokens still owned by workers.
  std::size_t maxOutputRows{256};      ///< Maximum requested and returned page size, in rows.
  std::size_t maxSourceRows{65536};    ///< Maximum rows examined by local snapshot evaluation.
  std::size_t maxComparisons{1048576}; ///< Maximum comparator invocations per local evaluation.
};

/** @brief Owning domain criteria, exact indexed source, and explicit output page.
 * This value performs no I/O. Q and its reachable data must be immutable while shared,
 * own all worker-visible storage, and support destruction on the last owner's thread.
 * @tparam T Immutable domain row type satisfying ModelSnapshot<T>'s ownership contract.
 * @tparam Q Owning domain criteria type; no library-defined query language is required.
 */
template <class T, class Q> struct Query {
  std::shared_ptr<const ModelSnapshot<T>> source; ///< Exact immutable source pin retained by the work token.
  Q criteria;                                     ///< Owning domain value interpreted by the provider.
  RowRange page{0, 256};                          ///< Page within the filtered/sorted result; default first 256 rows.
};

/** @brief Provider's owning page at the exact source revision, before projection publication.
 * @tparam T Immutable domain row type satisfying ModelSnapshot<T>'s ownership contract.
 */
template <class T> struct QueryPage {
  std::shared_ptr<const ModelSnapshot<T>> snapshot; ///< Contiguous page; revision equals requested source revision.
  std::uint64_t totalRows{};                        ///< Logical filtered row count before paging.
};

/// @cond INTERNAL
namespace detail {
struct QueryEvaluationAbort {
  QueryError error;
};

inline TreeError queryTreeError(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::CLOSED:
    return TreeError::CLOSED;
  case SchedulerError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case SchedulerError::CALLBACK_FAILED:
    return TreeError::CALLBACK_FAILED;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}

inline bool validQueryOptions(QueryOptions options) noexcept {
  return options.maxInFlight && options.maxOutputRows && options.maxSourceRows && options.maxComparisons;
}

template <class T> struct LocalQueryRows final : ModelSnapshot<T> {
  LocalQueryRows(std::shared_ptr<const ModelSnapshot<T>> original, std::vector<ModelRow<T>> value)
      : ModelSnapshot<T>(original->revision(), value.size()), source(std::move(original)), rows(std::move(value)) {
    identities.reserve(rows.size());

    for (std::size_t index = 0; index < rows.size(); ++index) {
      identities.emplace_back(rows[index].id, index);
    }

    std::ranges::sort(identities, [](const auto &left, const auto &right) {
      return std::tie(left.first.value, left.first.generation) < std::tie(right.first.value, right.first.generation);
    });
  }

  ModelRows<T> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    auto found = std::lower_bound(identities.begin(), identities.end(), id, [](const auto &entry, RowId wanted) {
      return std::tie(entry.first.value, entry.first.generation) < std::tie(wanted.value, wanted.generation);
    });

    return found == identities.end() || found->first != id ? RowLookup{RowLookupState::NOT_FOUND, 0}
                                                           : RowLookup{RowLookupState::FOUND, found->second};
  }

  std::shared_ptr<const ModelSnapshot<T>> source;
  std::vector<ModelRow<T>> rows;
  std::vector<std::pair<RowId, std::size_t>> identities;
};
} // namespace detail

/// @endcond

/** @brief Explicit local filter, stable sort, and page; never dispatches or starts provider I/O.
 * Source scan, comparisons and output are independently bounded. Equal sort keys retain
 * source order. IDs and generations are copied unchanged. T is copied only for output rows.
 * Callbacks must be bounded, worker-safe and must not retain borrowed row/criteria arguments.
 * @tparam T Copy-constructible immutable domain row satisfying the snapshot ownership contract.
 * @tparam Q Owning domain criteria type passed by const reference to both callbacks.
 * @tparam Filter Callable bool(const T&, const Q&) selecting a row.
 * @tparam Compare Strict weak ordering callable bool(const T&, const T&, const Q&).
 * @tparam Stop Callable bool() used for cooperative cancellation; never joins work.
 * @param query Exact source, owning criteria and desired page.
 * @param filter Predicate invoked at most maxSourceRows times.
 * @param compare Ordering invoked at most maxComparisons times.
 * @param options Positive finite work and output limits.
 * @param stop Cancellation predicate checked during scan, comparison and copy.
 * @return Owning exact-source page or typed evaluation failure; exceptions are translated.
 */
template <std::copy_constructible T, class Q, class Filter, class Compare, class Stop = std::function<bool()>>
std::expected<QueryPage<T>, QueryError> transformSnapshot(
  const Query<T, Q> &query, Filter filter, Compare compare, QueryOptions options = {}, Stop stop = [] {
    return false;
  }) {
  if (!query.source || !detail::validQueryOptions(options) || query.page.count > options.maxOutputRows) {
    return std::unexpected(QueryError::INVALID_ARGUMENT);
  }

  if (query.source->rowCount() > options.maxSourceRows) {
    return std::unexpected(QueryError::LIMIT_EXCEEDED);
  }

  try {
    if (stop()) {
      return std::unexpected(QueryError::CANCELLED);
    }

    auto input =
      ModelRange<T>::read(query.source, {0, static_cast<std::size_t>(query.source->rowCount())}, options.maxSourceRows);

    if (!input) {
      return std::unexpected(input.error() == TreeError::NO_MEMORY         ? QueryError::NO_MEMORY
                             : input.error() == TreeError::CALLBACK_FAILED ? QueryError::CALLBACK_FAILED
                                                                           : QueryError::INVALID_ARGUMENT);
    }

    if (input->state() != RangeState::READY) {
      return std::unexpected(QueryError::NOT_READY);
    }

    std::vector<std::size_t> indices;

    indices.reserve(input->rows().size());

    for (std::size_t index = 0; index < input->rows().size(); ++index) {
      if (stop()) {
        return std::unexpected(QueryError::CANCELLED);
      }

      if (filter(input->rows()[index].value, query.criteria)) {
        indices.push_back(index);
      }
    }

    std::size_t comparisons{};

    std::stable_sort(indices.begin(), indices.end(), [&](auto left, auto right) {
      if (stop()) {
        throw detail::QueryEvaluationAbort{QueryError::CANCELLED};
      }

      if (comparisons == options.maxComparisons) {
        throw detail::QueryEvaluationAbort{QueryError::LIMIT_EXCEEDED};
      }

      ++comparisons;

      return compare(input->rows()[left].value, input->rows()[right].value, query.criteria);
    });

    const auto first = static_cast<std::size_t>(std::min<std::uint64_t>(query.page.first, indices.size()));
    const auto count = std::min(query.page.count, indices.size() - first);
    std::vector<ModelRow<T>> output;

    output.reserve(count);

    for (std::size_t offset = 0; offset < count; ++offset) {
      if (stop()) {
        return std::unexpected(QueryError::CANCELLED);
      }

      output.push_back(input->rows()[indices[first + offset]]);
    }

    return QueryPage<T>{std::make_shared<const detail::LocalQueryRows<T>>(query.source, std::move(output)),
                        indices.size()};
  } catch (detail::QueryEvaluationAbort abort) {
    return std::unexpected(abort.error);
  } catch (const std::bad_alloc &) {
    return std::unexpected(QueryError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(QueryError::CALLBACK_FAILED);
  }
}

/** @brief Derived model snapshot with distinct projection revision and exact source provenance.
 * Retains the source pin and provider result for every borrowed row. Construction does not
 * validate the page; QueryController validates worker results before creating this wrapper.
 * @tparam T Immutable domain row type satisfying ModelSnapshot<T>'s ownership contract.
 */
template <class T> class QuerySnapshot final : public ModelSnapshot<T> {
public:
  /** @brief Retain immutable source and result owners under a new projection revision.
   * @param projectionRevision Monotonic derived model revision; never a source revision.
   * @param sourceIdentity Nonzero fixed provider-session identity.
   * @param queryGeneration Accepted query generation; zero denotes the initial unqueried source.
   * @param source Exact immutable source pin, non-null.
   * @param result Exact-source page and total filtered row count, with non-null snapshot.
   * @param page Requested interval within the filtered/sorted source.
   */
  QuerySnapshot(std::uint64_t projectionRevision, std::uint64_t sourceIdentity, std::uint64_t queryGeneration,
                std::shared_ptr<const ModelSnapshot<T>> source, QueryPage<T> result, RowRange page)
      : ModelSnapshot<T>(projectionRevision, result.snapshot->rowCount()), source_(std::move(source)),
        result_(std::move(result)), sourceIdentity_(sourceIdentity), queryGeneration_(queryGeneration), page_(page) {
  }

  /** @brief Read only the already materialized page.
   * @param range Checked interval in page-local model coordinates, not source coordinates.
   * @return Borrowed rows from the retained immutable result snapshot.
   */
  ModelRows<T> read(RowRange range) const override {
    return result_.snapshot->read(range);
  }

  /** @brief Resolve identity in the page-local result.
   * @param id Exact stable source identity and generation.
   * @return Provider's bounded identity lookup capability and page-local index.
   */
  RowLookup locateRow(RowId id) const override {
    return result_.snapshot->locateRow(id);
  }

  /** @brief Original exact source revision.
   * @return Source pin's revision; never the derived ModelSnapshot revision.
   */
  std::uint64_t sourceRevision() const noexcept {
    return source_->revision();
  }

  /** @brief Fixed provider session identity.
   * @return Nonzero identity issued for this query controller.
   */
  std::uint64_t sourceIdentity() const noexcept {
    return sourceIdentity_;
  }

  /** @brief Query generation represented by the page.
   * @return Accepted request generation, or zero for initial source data.
   */
  std::uint64_t queryGeneration() const noexcept {
    return queryGeneration_;
  }

  /** @brief Original source owner.
   * @return Shared exact immutable source pin, safe to retain beyond controller lifetime.
   */
  std::shared_ptr<const ModelSnapshot<T>> source() const noexcept {
    return source_;
  }

  /** @brief Logical filtered result size before paging.
   * @return Total rows reported by the provider and validated against source size.
   */
  std::uint64_t totalRows() const noexcept {
    return result_.totalRows;
  }

  /** @brief Query page in filtered-result coordinates.
   * @return Original requested page; model reads use page-local coordinates starting at zero.
   */
  RowRange page() const noexcept {
    return page_;
  }

private:
  std::shared_ptr<const ModelSnapshot<T>> source_;
  QueryPage<T> result_;
  std::uint64_t sourceIdentity_{};
  std::uint64_t queryGeneration_{};
  RowRange page_;
};

/// @cond INTERNAL
namespace detail {
template <class T, class Q> struct QueryCompletion {
  std::shared_ptr<ModelLease> lease;
  std::shared_ptr<const Query<T, Q>> query;
  std::uint64_t sourceIdentity{};
  std::uint64_t generation{};
  std::expected<QueryPage<T>, QueryError> result;
};
} // namespace detail

/// @endcond

/** @brief Copyable worker token owning criteria, source pin and one bounded reservation.
 * Cancellation never joins. Complete queues ownership; acceptance is not UI application.
 * Retain and retry the same result after rejected publication. No UI references are retained.
 * @tparam T Immutable domain row satisfying the snapshot destruction contract on any releasing thread.
 * @tparam Q Owning immutable criteria supporting destruction on any releasing thread.
 */
template <class T, class Q> class QueryRequest {
public:
  /** @brief Empty cancelled token. */
  QueryRequest() = default;

  /** @brief Retain the same work reservation and immutable owners.
   * @param other Token whose ownership is shared.
   */
  QueryRequest(const QueryRequest &other) = default;

  /** @brief Transfer immutable owners and reservation.
   * @param other Token emptied by the move.
   */
  QueryRequest(QueryRequest &&other) noexcept = default;

  /** @brief Release criteria before the reservation; the final criteria deleter still sees reserved work. */
  ~QueryRequest() {
    query_.reset();
    lease_.reset();
  }

  /** @brief Commit all replacement fields before releasing old storage, which may reenter.
   * @param other Replacement token passed by value.
   * @return This token after replacement; avoid using it if a deleter destroyed its owner.
   */
  QueryRequest &operator=(QueryRequest other) noexcept {
    std::swap(query_, other.query_);
    std::swap(lease_, other.lease_);
    std::swap(publisher_, other.publisher_);
    std::swap(options_, other.options_);
    std::swap(sourceIdentity_, other.sourceIdentity_);
    std::swap(generation_, other.generation_);

    return *this;
  }

  /** @brief Access owning query data while retaining this token.
   * @return Non-null shared immutable query, or null for an empty token.
   */
  std::shared_ptr<const Query<T, Q>> query() const noexcept {
    return query_;
  }

  /** @brief Evaluation budgets captured when submitted.
   * @return Positive finite limits; defaults for an empty token.
   */
  QueryOptions options() const noexcept {
    return options_;
  }

  /** @brief Check cancellation without touching UI.
   * @return True for an empty token, superseded query, or closed controller.
   */
  bool stopRequested() const noexcept {
    return !lease_ || lease_->cancelled.load() || !lease_->lifetime->alive.load();
  }

  /** @brief Cooperatively cancel this work; its reservation remains until the last owner dies. */
  void cancel() const noexcept {
    if (lease_) {
      lease_->cancelled.store(true);
    }
  }

  /** @brief Validate and queue an owning result or typed failure from any worker thread.
   * @param result Exact-source page, or domain evaluation failure; caller may retain and retry on rejection.
   * @return Queue acceptance, CLOSED for stopped work, INVALID_ARGUMENT for shape/revision mismatch,
   * WOULD_BLOCK for an already submitted token or queue capacity, or NO_MEMORY.
   */
  std::expected<PublishResult, SchedulerError> complete(std::expected<QueryPage<T>, QueryError> result) const {
    auto retained = *this;

    return retained.completeOwned(std::move(result));
  }

private:
  std::expected<PublishResult, SchedulerError> completeOwned(std::expected<QueryPage<T>, QueryError> result) const {
    if (stopRequested()) {
      return std::unexpected(SchedulerError::CLOSED);
    }

    if (result) {
      const auto &value = *result;
      const auto total = value.totalRows;
      const auto count =
        query_->page.first < total ? std::min<std::uint64_t>(query_->page.count, total - query_->page.first) : 0;

      if (!value.snapshot || value.snapshot->revision() != query_->source->revision() ||
          total > query_->source->rowCount() || value.snapshot->rowCount() != count) {
        return std::unexpected(SchedulerError::INVALID_ARGUMENT);
      }

      auto rows = ModelRange<T>::read(value.snapshot, {0, static_cast<std::size_t>(count)}, options_.maxOutputRows);

      if (!rows) {
        return std::unexpected(rows.error() == TreeError::NO_MEMORY         ? SchedulerError::NO_MEMORY
                               : rows.error() == TreeError::CALLBACK_FAILED ? SchedulerError::CALLBACK_FAILED
                                                                            : SchedulerError::INVALID_ARGUMENT);
      }

      if (rows->state() != RangeState::READY) {
        return std::unexpected(SchedulerError::INVALID_ARGUMENT);
      }
    }

    if (stopRequested()) {
      return std::unexpected(SchedulerError::CLOSED);
    }

    if (lease_->submitted.exchange(true)) {
      return std::unexpected(SchedulerError::WOULD_BLOCK);
    }

    try {
      auto event = std::make_shared<const detail::QueryCompletion<T, Q>>(lease_, query_, sourceIdentity_, generation_,
                                                                         std::move(result));
      auto accepted = publisher_.publish(std::move(event));

      if (!accepted) {
        lease_->submitted.store(false);
      }

      return accepted;
    } catch (const std::bad_alloc &) {
      lease_->submitted.store(false);

      return std::unexpected(SchedulerError::NO_MEMORY);
    }
  }

private:
  template <class, class> friend class QueryController;
  std::shared_ptr<const Query<T, Q>> query_;
  std::shared_ptr<detail::ModelLease> lease_;
  Publisher<detail::QueryCompletion<T, Q>> publisher_;
  QueryOptions options_;
  std::uint64_t sourceIdentity_{};
  std::uint64_t generation_{};
};

/** @brief Provider owned by a UI query controller; workers retain only query tokens.
 * @tparam T Immutable source/output row satisfying the snapshot ownership contract.
 * @tparam Q Owning domain criteria interpreted by the provider.
 */
template <class T, class Q> class QueryProvider {
public:
  /** @brief Reclaim provider resources on UI; outstanding worker tokens remain independently owned. */
  virtual ~QueryProvider() = default;

  /** @brief Begin explicit provider-side work on UI; do not dispatch, block, or retain UI references.
   * Providers must retain rejected completion values and retry after backpressure.
   * @param request Owning worker-safe token; copy it to retain work beyond this call.
   */
  virtual void start(QueryRequest<T, Q> request) = 0;

  /** @brief Retry provider-retained rejected completion values on UI; default has no retained work.
   * @return Success or original scheduler publication failure; never dispatches or joins.
   */
  virtual std::expected<void, SchedulerError> retry() {
    return {};
  }
};

/** @brief Explicit in-memory provider using the bounded local transform, with rejection recovery.
 * Evaluates synchronously on UI. Use a delayed provider for work that must leave UI.
 * Callback captures must own immutable storage; callbacks cannot retain borrowed arguments.
 * @tparam T Copy-constructible immutable domain row satisfying snapshot ownership.
 * @tparam Q Owning immutable domain criteria.
 * @tparam Filter Callable bool(const T&, const Q&) selecting rows.
 * @tparam Compare Strict weak ordering callable bool(const T&, const T&, const Q&).
 */
template <std::copy_constructible T, class Q, class Filter, class Compare>
class MemoryQueryProvider final : public QueryProvider<T, Q> {
public:
  /** @brief Own domain callbacks.
   * @param filter Bounded row predicate, invoked by the local transform.
   * @param compare Bounded strict weak ordering, invoked by the local transform.
   */
  MemoryQueryProvider(Filter filter, Compare compare) : filter_(std::move(filter)), compare_(std::move(compare)) {
  }

  /** @brief Evaluate and queue the exact query; retain a rejected notification for retry.
   * @param request Owning query token; a superseded retained notification is discarded.
   */
  void start(QueryRequest<T, Q> request) override {
    auto value =
      transformSnapshot(*request.query(), std::ref(filter_), std::ref(compare_), request.options(), [&request] {
        return request.stopRequested();
      });

    if (request.stopRequested()) {
      return;
    }

    // Commit a complete new pending value before old criteria/storage can reenter.
    // Assignment to an engaged optional would retire the old token mid-assignment.
    auto retired = std::exchange(pending_, {});

    pending_.emplace(Pending{std::move(request), std::move(value)});
    (void)retry();
  }

  /** @brief Retry the same result after queue rejection; closed/cancelled retained work is discarded.
   * @return Success or original scheduler rejection; no evaluation is repeated.
   */
  std::expected<void, SchedulerError> retry() override {
    if (!pending_) {
      return {};
    }

    auto current = std::exchange(pending_, {});

    if (current->request.stopRequested()) {
      return {};
    }

    auto accepted = current->request.complete(current->result);

    if (!accepted) {
      if (!pending_ && !current->request.stopRequested()) {
        pending_ = std::move(current);
      }

      return std::unexpected(accepted.error());
    }

    return {};
  }

private:
  struct Pending {
    QueryRequest<T, Q> request;
    std::expected<QueryPage<T>, QueryError> result;
  };

  Filter filter_;
  Compare compare_;
  std::optional<Pending> pending_;
};

/** @brief UI-owned adapter coordinating a fixed provider session and one long-lived Model.
 * Operations require the creating UI thread and a live tree context, outside frames.
 * Release/close before WidgetTree. Criteria and token owners are worker-safe; controller is not.
 * Model data revisions are projection revisions; source provenance is exposed separately.
 * @tparam T Immutable domain row satisfying the snapshot ownership contract.
 * @tparam Q Owning immutable domain criteria supporting destruction on any releasing thread.
 */
template <class T, class Q> class QueryController {
private:
  struct State {
    State(WidgetTree &value, Node<Widget> node) : tree(value), target(node) {
    }

    std::expected<void, TreeError> allowed() const {
      if (owner != std::this_thread::get_id()) {
        return std::unexpected(TreeError::WRONG_THREAD);
      }

      if (!lifetime->alive.load()) {
        return std::unexpected(TreeError::CLOSED);
      }

      return tree.withScope(target, [](const UiScope &) {
      });
    }

    std::expected<void, TreeError> retry() {
      std::shared_ptr<const detail::QueryCompletion<T, Q>> retiredCompletion;

      if (completion) {
        if (projectionRevision == std::numeric_limits<std::uint64_t>::max()) {
          notificationError = TreeError::IDENTITY_EXHAUSTED;

          return std::unexpected(*notificationError);
        }

        try {
          auto next = std::make_shared<const QuerySnapshot<T>>(projectionRevision + 1, sourceIdentity, generation,
                                                               completion->query->source, *completion->result,
                                                               completion->query->page);
          retiredCompletion = std::move(completion);

          ++projectionRevision;
          pending = std::move(next);
          queryError.reset();
        } catch (const std::bad_alloc &) {
          notificationError = TreeError::NO_MEMORY;

          return std::unexpected(*notificationError);
        }
      }

      if (!pending) {
        return {};
      }

      auto frame = model->pin();

      if (!frame) {
        return std::unexpected(frame.error());
      }

      if (frame->dataRevision() == pending->revision()) {
        auto retired = std::move(applied);

        applied = std::exchange(pending, {});
        notificationError.reset();

        return {};
      }

      auto accepted = model->publisher().publish(pending);

      if (!accepted) {
        notificationError = detail::queryTreeError(accepted.error());

        return std::unexpected(*notificationError);
      }

      notificationError.reset();

      return {};
    }

    WidgetTree &tree;
    Node<Widget> target;
    const std::thread::id owner{std::this_thread::get_id()};
    QueryOptions options;
    std::shared_ptr<detail::ModelLifetime> lifetime = std::make_shared<detail::ModelLifetime>();
    std::weak_ptr<detail::ModelLease> active;
    std::shared_ptr<QueryProvider<T, Q>> provider;
    std::unique_ptr<Model<T>> model;
    std::shared_ptr<const detail::QueryCompletion<T, Q>> completion;
    std::shared_ptr<const QuerySnapshot<T>> pending;
    std::shared_ptr<const QuerySnapshot<T>> applied;
    std::optional<QueryError> queryError;
    std::optional<TreeError> notificationError;
    std::uint64_t sourceIdentity{};
    std::uint64_t sourceRevision{};
    std::uint64_t generation{};
    std::uint64_t projectionRevision{1};
  };

public:
  /** @brief Create an unqueried source projection and existing bounded scheduler endpoints.
   * @param tree UI tree which must outlive this controller.
   * @param context Live owner node for all completion/publication endpoints.
   * @param source Initial immutable source pin, non-null; its values are initially presented unchanged.
   * @param provider Non-null owned provider, destroyed on UI after close.
   * @param options Positive finite query limits, captured into each token.
   * @param modelOptions Existing model interval/consumer/work limits, unchanged by query limits.
   * @return UI controller, tree/thread/frame error, INVALID_ARGUMENT, IDENTITY_EXHAUSTED, or NO_MEMORY.
   */
  static std::expected<std::unique_ptr<QueryController>, TreeError>
  create(WidgetTree &tree, Node<Widget> context, std::shared_ptr<const ModelSnapshot<T>> source,
         std::unique_ptr<QueryProvider<T, Q>> provider, QueryOptions options = {}, ModelOptions modelOptions = {}) {
    if (!source || !provider || !detail::validQueryOptions(options) ||
        source->rowCount() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto allowed = tree.withScope(context, [](const UiScope &) {
    });
    auto identity = detail::modelSession();

    if (!allowed || !identity) {
      return std::unexpected(!allowed ? allowed.error() : identity.error());
    }

    try {
      auto state = std::make_shared<State>(tree, context);

      state->options = options;
      state->sourceIdentity = *identity;
      state->sourceRevision = source->revision();
      state->provider = std::move(provider);
      state->applied =
        std::make_shared<const QuerySnapshot<T>>(1, *identity, 0, source, QueryPage<T>{source, source->rowCount()},
                                                 RowRange{0, static_cast<std::size_t>(source->rowCount())});

      auto model = Model<T>::create(tree, context, state->applied, modelOptions);

      if (!model) {
        return std::unexpected(model.error());
      }

      state->model = std::move(*model);

      auto connection = tree.template connect<detail::QueryCompletion<T, Q>>(
        context, [weak = std::weak_ptr(state)](Widget &, std::shared_ptr<const detail::QueryCompletion<T, Q>> event) {
          auto state = weak.lock();

          if (!state || !state->lifetime->alive.load() || event->lease->cancelled.load() ||
              event->sourceIdentity != state->sourceIdentity || event->generation != state->generation ||
              event->query->source->revision() != state->sourceRevision) {
            return;
          }

          if (!event->result) {
            state->queryError = event->result.error();

            return;
          }

          // Retain accepted ownership before allocating the derived wrapper. A failed
          // UI allocation is recoverable even after the producer drops every token.
          state->completion = std::move(event);
          (void)state->retry();
        });

      if (!connection) {
        return std::unexpected(connection.error());
      }

      return std::unique_ptr<QueryController>(new QueryController(std::move(state), std::move(*connection)));
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    }
  }

  /** @brief Close on UI without joining workers; retained frames and rows stay immutable. */
  ~QueryController() {
    if (state_->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    (void)close();
  }

  /** @brief Submit explicit provider work; supersedes the previous generation after reservation succeeds.
   * @param query Owning criteria, exact source pin and bounded page; older source revisions are rejected.
   * @return Worker token, tree/thread/frame error, INVALID_ARGUMENT, WOULD_BLOCK, NO_MEMORY,
   * IDENTITY_EXHAUSTED, CALLBACK_FAILED if provider start throws, or CLOSED if provider start
   * closes/cancels this token or synchronously submits a newer generation. Context removal
   * returns its original tree error and cancels this token.
   */
  std::expected<QueryRequest<T, Q>, TreeError> submit(Query<T, Q> query) {
    auto state = state_;
    auto allowed = state->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    // Only one derived publication may await observation. This bounds retained
    // provenance and prevents a queued projection from losing its exact wrapper.
    auto frame = state->model->pin();

    if (!frame) {
      return std::unexpected(frame.error());
    }

    std::shared_ptr<const QuerySnapshot<T>> retired;

    if (state->completion) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    if (state->pending) {
      if (frame->dataRevision() != state->pending->revision()) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }

      retired = std::move(state->applied);
      state->applied = std::exchange(state->pending, {});
    }

    if (!query.source || query.page.count > state->options.maxOutputRows ||
        query.source->revision() < state->sourceRevision) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if (state->lifetime->outstanding.load() >= state->options.maxInFlight) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    if (state->generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    QueryRequest<T, Q> request;
    auto publisher = connection_.publisher;
    auto provider = state->provider;
    const auto entryGeneration = state->generation;
    std::shared_ptr<detail::ModelLease> previous;

    try {
      request.query_ = std::make_shared<const Query<T, Q>>(std::move(query));

      // Moving domain criteria may reenter or destroy this controller. From here
      // onward use retained state/endpoints only, and revalidate before reserving.
      auto constructedAllowed = state->allowed();

      if (!constructedAllowed) {
        return std::unexpected(constructedAllowed.error());
      }

      if (state->generation != entryGeneration) {
        return std::unexpected(TreeError::CLOSED);
      }

      if (request.query_->source->revision() < state->sourceRevision) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      if (state->completion || state->pending || state->lifetime->outstanding.load() >= state->options.maxInFlight) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }

      request.lease_ = std::make_shared<detail::ModelLease>(state->lifetime, request.query_);
      request.publisher_ = std::move(publisher);
      request.sourceIdentity_ = state->sourceIdentity;
      request.generation_ = state->generation + 1;
      request.options_ = state->options;

      previous = state->active.lock();

      if (previous) {
        previous->cancelled.store(true);
      }

      state->active = request.lease_;
      state->generation = request.generation_;
      state->sourceRevision = request.query_->source->revision();
      state->queryError.reset();
      provider->start(request);

      // Retirement can execute criteria/source deleters. Check validity only after
      // releasing these old owners, with the new generation already committed.
      previous.reset();
      retired.reset();
      frame = std::unexpected(TreeError::CLOSED);

      auto stillAllowed = state->allowed();

      if (!stillAllowed) {
        request.cancel();

        return std::unexpected(stillAllowed.error());
      }

      if (state->generation != request.generation_ || request.stopRequested()) {
        request.cancel();

        return std::unexpected(TreeError::CLOSED);
      }

      return request;
    } catch (const std::bad_alloc &) {
      request.cancel();

      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      request.cancel();

      if (request.generation_ == state->generation) {
        state->queryError = QueryError::CALLBACK_FAILED;
      }

      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @brief Pin the existing model; query queue acceptance may still be awaiting application.
   * @return Immutable frame or UI/thread/tree/closed error.
   */
  std::expected<ModelFrame<T>, TreeError> pin() const {
    auto allowed = state_->allowed();

    return allowed ? state_->model->pin() : std::unexpected(allowed.error());
  }

  /** @brief Open an unchanged low-level model interval consumer.
   * @return Consumer owned independently of query generations, or original model/tree error.
   */
  std::expected<ModelConsumer<T>, TreeError> openRange() {
    auto allowed = state_->allowed();

    return allowed ? state_->model->openRange() : std::unexpected(allowed.error());
  }

  /** @brief Retry retained notification and detect exact projection application; UI-only.
   * Call after dispatch and on WOULD_BLOCK/NO_MEMORY recovery; acceptance still needs a later dispatch.
   * @return Success for accepted/observed notification, or original tree/model/scheduler error.
   */
  std::expected<void, TreeError> retryPublication() {
    auto state = state_;
    auto allowed = state->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto provider = state->provider;

    try {
      auto retried = provider->retry();

      if (!retried) {
        state->notificationError = detail::queryTreeError(retried.error());

        return std::unexpected(*state->notificationError);
      }
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    if (!state->lifetime->alive.load()) {
      return std::unexpected(TreeError::CLOSED);
    }

    return state->retry();
  }

  /** @brief Inspect provenance of the model's currently applied projection.
   * @return Exact applied immutable wrapper, or original UI/thread/tree error.
   */
  std::expected<std::shared_ptr<const QuerySnapshot<T>>, TreeError> snapshot() {
    auto state = state_;
    auto allowed = state->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto frame = state->model->pin();

    if (!frame) {
      return std::unexpected(frame.error());
    }

    std::shared_ptr<const QuerySnapshot<T>> retired;

    if (state->pending && frame->dataRevision() == state->pending->revision()) {
      retired = std::move(state->applied);
      state->applied = std::exchange(state->pending, {});
    }

    auto result = state->applied;

    return result;
  }

  /** @brief Inspect latest typed provider evaluation failure.
   * @return Optional typed query error, or original UI/thread/tree error.
   */
  std::expected<std::optional<QueryError>, TreeError> lastQueryError() const {
    auto allowed = state_->allowed();

    return allowed ? std::expected<std::optional<QueryError>, TreeError>(state_->queryError)
                   : std::unexpected(allowed.error());
  }

  /** @brief Inspect notification failure first, then existing model UI application failure.
   * @return Sticky original failure, an empty optional, or UI/thread/tree error.
   */
  std::expected<std::optional<TreeError>, TreeError> lastError() const {
    auto allowed = state_->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    return state_->notificationError ? std::expected<std::optional<TreeError>, TreeError>(state_->notificationError)
                                     : state_->model->lastError();
  }

  /** @brief Idempotent UI closure; workers retain source/criteria but receive stop requests.
   * @return Success after disconnect, or WRONG_THREAD leaving ownership unchanged.
   */
  std::expected<void, TreeError> close() {
    auto state = state_;

    if (state->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }

    state->lifetime->alive.store(false);

    if (auto request = state->active.lock()) {
      request->cancelled.store(true);
    }

    (void)connection_.subscription.disconnect();
    state->model.reset();
    state->provider.reset();
    state->pending.reset();
    state->completion.reset();

    return {};
  }

private:
  QueryController(std::shared_ptr<State> state, SchedulerConnection<detail::QueryCompletion<T, Q>> connection)
      : state_(std::move(state)), connection_(std::move(connection)) {
  }

  std::shared_ptr<State> state_;
  SchedulerConnection<detail::QueryCompletion<T, Q>> connection_;
};
} // namespace portals
