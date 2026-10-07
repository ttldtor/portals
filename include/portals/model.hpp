// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace portals {
/** @brief Provider-assigned row identity, independent of position; increment generation on reuse. */
struct RowId {
  std::uint64_t value{};      ///< Nonzero identity within one model.
  std::uint64_t generation{}; ///< Nonzero generation; never wrap or reuse an exhausted pair.
  bool operator==(const RowId &) const noexcept = default; ///< Compare both identity components.
};
/** @brief Provider-assigned column identity; typed row accessors define column values. */
struct ColumnId {
  std::uint64_t value{};                                      ///< Nonzero identity within one model.
  std::uint64_t generation{};                                 ///< Nonzero generation, independent of visual order.
  bool operator==(const ColumnId &) const noexcept = default; ///< Compare identity.
};
/** @brief Half-open logical row interval; arithmetic is checked before provider access. */
struct RowRange {
  std::uint64_t first{}; ///< First logical row.
  std::size_t count{};   ///< Number of rows, bounded independently of logical model size.
  bool operator==(const RowRange &) const noexcept = default; ///< Compare interval.
};
/** @brief Observable asynchronous range readiness. */
enum class RangeState {
  pending,
  ready,
  error
};
/** @brief A typed domain row and its stable identity; a table uses a typed record as T. */
template <class T> struct ModelRow {
  RowId id; ///< Stable provider identity, never the row index.
  T value;  ///< Typed value; all reachable data must be immutable while published.
};
/** @brief Provider result borrowing from its immutable snapshot. */
template <class T> struct ModelRows {
  RangeState state{RangeState::pending}; ///< Pending never blocks or initiates I/O.
  std::span<const ModelRow<T>> rows;     ///< Exactly the requested count when ready; otherwise empty.
  std::string_view error;                ///< Snapshot-owned description when error; otherwise empty.
};
/** @brief Immutable list/table provider storage, independent of view/source lifetime.
 * Implement read as bounded in-memory access, without I/O, waiting or domain mutation.
 * Every ready range of one data revision must agree, including overlapping ranges from
 * different snapshot objects. Shared ownership does not make mutable aliases safe.
 * Snapshots and transitively owned T values must support destruction on the last owner's
 * thread; UI reentry from a destructor requires a still-live UI context.
 */
template <class T> class ModelSnapshot {
public:
  ModelSnapshot(std::uint64_t revision, std::uint64_t rows) : revision_(revision), rows_(rows) {
  } ///< Define immutable metadata.
  virtual ~ModelSnapshot() = default; ///< Reclaim storage after all ranges, frames and workers release it.
  std::uint64_t revision() const noexcept {
    return revision_;
  } ///< Provider data revision; strictly increasing on replacement.
  std::uint64_t row_count() const noexcept {
    return rows_;
  } ///< Logical count, without materializing rows.
  virtual ModelRows<T> read(RowRange range) const = 0; ///< Borrow a checked interval; may return pending/error.
private:
  const std::uint64_t revision_;
  const std::uint64_t rows_;
};
/** @brief Stable identity of a scoped range consumer, distinct even for equal row intervals. */
struct ModelRangeId {
  std::uint64_t session{};                                        ///< Process-unique model session.
  std::uint64_t value{};                                          ///< Never-reused consumer identity.
  bool operator==(const ModelRangeId &) const noexcept = default; ///< Compare source and consumer.
};
/** @brief Finite per-model storage and work budgets; all fields must be positive. */
struct ModelOptions {
  std::size_t max_range_rows{4096}; ///< Largest single read/request.
  std::size_t max_consumers{32};    ///< Live scoped range slots, regardless of readiness.
  std::size_t max_in_flight{64};    ///< Request owners, including cancelled and queued work not yet released.
};
/** @brief Independent owner for borrowed rows and provider error text. */
template <class T> class ModelRange {
public:
  ModelRange() = default; ///< Empty pending result.

  ModelRange(const ModelRange &) = default;     ///< Retain the same immutable ownership.
  ModelRange(ModelRange &&) noexcept = default; ///< Transfer ownership without releasing old storage.
  /** @brief Replace all fields before releasing old storage, whose destructor may reenter. */
  ModelRange &operator=(ModelRange other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(range_, other.range_);
    std::swap(data_, other.data_);

    return *this;
  }

  RangeState state() const noexcept {
    return data_.state;
  } ///< Read readiness without loading.
  std::span<const ModelRow<T>> rows() const noexcept {
    return data_.rows;
  } ///< Borrow until this owner is released.
  std::string_view error() const noexcept {
    return data_.error;
  } ///< Borrow error text for this owner's lifetime.
  RowRange range() const noexcept {
    return range_;
  } ///< Logical interval of this result.
  std::uint64_t data_revision() const noexcept {
    return owner_ ? owner_->revision() : 0;
  } ///< Provider revision.
  /** @brief Read only memory and retain its snapshot. Bounds/provider contract failures are explicit.
   * Allocation exceptions become no_memory, other provider exceptions callback_failed.
   */
  static std::expected<ModelRange, TreeError> read(std::shared_ptr<const ModelSnapshot<T>> owner, RowRange range,
                                                   std::size_t max_rows) {
    if (!owner || range.count > max_rows || range.first > owner->row_count() ||
        range.count > owner->row_count() - range.first) {
      return std::unexpected(TreeError::invalid_argument);
    }

    try {
      auto rows = owner->read(range);

      if ((rows.state == RangeState::ready && (rows.rows.size() != range.count || !rows.error.empty())) ||
          (rows.state != RangeState::ready && !rows.rows.empty()) ||
          (rows.state == RangeState::pending && !rows.error.empty()) ||
          (rows.state != RangeState::ready && rows.state != RangeState::pending && rows.state != RangeState::error)) {
        return std::unexpected(TreeError::invalid_argument);
      }

      return ModelRange(std::move(owner), range, rows);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    } catch (...) {
      return std::unexpected(TreeError::callback_failed);
    }
  }

private:
  template <class> friend class ModelConsumer;
  template <class> friend class Model;
  ModelRange(std::shared_ptr<const ModelSnapshot<T>> owner, RowRange range, ModelRows<T> data)
      : owner_(std::move(owner)), range_(range), data_(data) {
  }

  std::shared_ptr<const ModelSnapshot<T>> owner_;
  RowRange range_;
  ModelRows<T> data_;
};
/// @cond INTERNAL
namespace detail {
std::expected<std::uint64_t, TreeError> model_session();
struct ModelLifetime {
  std::atomic<bool> alive{true};
  std::atomic<std::size_t> outstanding{};
};
struct ModelLease {
  ModelLease(std::shared_ptr<ModelLifetime> life, std::shared_ptr<const void> storage)
      : lifetime(std::move(life)), storage(std::move(storage)) {
    lifetime->outstanding.fetch_add(1);
  }

  ~ModelLease() {
    // Reentrant storage destruction must still see the outstanding reservation.
    storage.reset();
    lifetime->outstanding.fetch_sub(1);
  }

  std::shared_ptr<ModelLifetime> lifetime;
  std::shared_ptr<const void> storage;
  std::atomic<bool> cancelled{};
  std::atomic<bool> submitted{};
};
template <class T> struct ModelEntry {
  ModelRangeId id;
  std::uint64_t generation{};
  ModelRange<T> result;
  std::weak_ptr<ModelLease> work;
};
template <class T> struct ModelFrameData {
  explicit ModelFrameData(std::shared_ptr<const ModelSnapshot<T>> initial) : snapshot(std::move(initial)) {
  }

  std::shared_ptr<const ModelSnapshot<T>> snapshot;
  std::vector<ModelEntry<T>> entries = std::vector<ModelEntry<T>>(0);
  std::uint64_t revision{};
};
template <class T> struct ModelCompletion {
  ModelRangeId id;
  std::uint64_t generation{};
  std::shared_ptr<ModelLease> lease;
  ModelRange<T> result;
};
template <class T> struct ModelState {
  ModelState(WidgetTree &value, Node<Widget> node) : tree(value), target(node) {
  }

  std::expected<void, TreeError> allowed() const {
    if (owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    if (!lifetime->alive.load()) {
      return std::unexpected(TreeError::closed);
    }

    return tree.with_scope(target, [](const UiScope &) {
    });
  }

  std::expected<std::shared_ptr<ModelFrameData<T>>, TreeError> next() const {
    if (current->revision == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::identity_exhausted);
    }

    try {
      auto result = std::make_shared<ModelFrameData<T>>(*current);

      ++result->revision;

      return result;
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    }
  }

  std::expected<void, TreeError> commit(std::shared_ptr<ModelFrameData<T>> next) {
    auto invalidated = tree.invalidate(target, Invalidation::layout);

    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }

    // Keep old storage until the new state and all bookkeeping are committed.
    auto previous = std::exchange(current, std::move(next));

    return {};
  }

  WidgetTree &tree;
  Node<Widget> target;
  const std::thread::id owner{std::this_thread::get_id()};
  ModelOptions options;
  std::uint64_t session{};
  std::uint64_t identity{};
  std::vector<ModelRangeId> retired = std::vector<ModelRangeId>(0);
  std::shared_ptr<ModelLifetime> lifetime = std::make_shared<ModelLifetime>();
  std::shared_ptr<const ModelFrameData<T>> current;
  Publisher<ModelCompletion<T>> publisher;
  std::optional<TreeError> application_error;
};
} // namespace detail
/// @endcond
/** @brief One immutable model revision retained by the frame owner for all its views.
 * Obtain once before WidgetTree::frame, pass the same value to every view, and retain it
 * until borrowed rendering finishes. Different models require application transactions.
 */
template <class T> class ModelFrame {
public:
  ModelFrame(const ModelFrame &) = default;     ///< Retain the same immutable ownership.
  ModelFrame(ModelFrame &&) noexcept = default; ///< Transfer ownership without releasing old storage.
  /** @brief Replace all fields before releasing old storage, whose destructor may reenter. */
  ModelFrame &operator=(ModelFrame other) noexcept {
    std::swap(data_, other.data_);
    std::swap(max_rows_, other.max_rows_);

    return *this;
  }

  std::uint64_t revision() const noexcept {
    return data_->revision;
  } ///< UI publication revision, including range readiness.
  std::uint64_t data_revision() const noexcept {
    return data_->snapshot->revision();
  } ///< Provider data revision.
  std::uint64_t row_count() const noexcept {
    return data_->snapshot->row_count();
  } ///< Logical rows.
  /** @brief Read a consumer's exact result; stale/foreign consumer returns stale_node. No spatial merge. */
  std::expected<ModelRange<T>, TreeError> range(ModelRangeId id) const {
    auto entry = std::find_if(data_->entries.begin(), data_->entries.end(), [id](const auto &value) {
      return value.id == id;
    });

    if (entry == data_->entries.end()) {
      return std::unexpected(TreeError::stale_node);
    }

    return entry->result;
  }

  /** @brief Direct bounded read from the base snapshot, without asynchronous requests or overlays. */
  std::expected<ModelRange<T>, TreeError> read(RowRange range) const {
    return ModelRange<T>::read(data_->snapshot, range, max_rows_);
  }

private:
  template <class> friend class Model;
  ModelFrame(std::shared_ptr<const detail::ModelFrameData<T>> data, std::size_t max_rows)
      : data_(std::move(data)), max_rows_(max_rows) {
  }

  std::shared_ptr<const detail::ModelFrameData<T>> data_;
  std::size_t max_rows_;
};
/** @brief Copyable worker token; retains work storage and one reservation until its last owner dies.
 * Cancellation is a stop request, never a join. Worker code must finish/release its own
 * owners. Completion queues an owning result; no view callback runs on the worker.
 */
template <class T> class ModelRequest {
public:
  ModelRequest() = default; ///< Empty cancelled token.

  ModelRequest(const ModelRequest &) = default;     ///< Retain the same immutable ownership.
  ModelRequest(ModelRequest &&) noexcept = default; ///< Transfer ownership without releasing old storage.
  /** @brief Replace all fields before releasing old storage, whose destructor may reenter. */
  ModelRequest &operator=(ModelRequest other) noexcept {
    std::swap(id_, other.id_);
    std::swap(generation_, other.generation_);
    std::swap(revision_, other.revision_);
    std::swap(row_count_, other.row_count_);
    std::swap(range_, other.range_);
    std::swap(lease_, other.lease_);
    std::swap(publisher_, other.publisher_);

    return *this;
  }

  RowRange range() const noexcept {
    return range_;
  } ///< Requested rows.
  std::uint64_t data_revision() const noexcept {
    return revision_;
  } ///< Required provider revision.
  bool stop_requested() const noexcept {
    return !lease_ || lease_->cancelled.load() || !lease_->lifetime->alive.load();
  } ///< Worker-safe cooperative stop query.
  void cancel() const noexcept {
    if (lease_) {
      lease_->cancelled.store(true);
    }
  } ///< Cancel only this generation; does not release its work budget.
  /** @brief Queue ready/error storage from the requested data revision; pending is invalid.
   * Exactly one successful submission per generation. Full mailbox returns would_block
   * and permits retry; closed means cancellation/source closure/already submitted.
   * Snapshot metadata must match the original revision and logical row count.
   */
  std::expected<PublishResult, SchedulerError> complete(std::shared_ptr<const ModelSnapshot<T>> snapshot) const {
    auto request = *this;

    if (request.stop_requested()) {
      return std::unexpected(SchedulerError::closed);
    }

    if (!snapshot || snapshot->revision() != request.revision_ || snapshot->row_count() != request.row_count_) {
      return std::unexpected(SchedulerError::invalid_argument);
    }

    auto result = ModelRange<T>::read(std::move(snapshot), request.range_, request.range_.count);

    if (!result || result->state() == RangeState::pending) {
      if (!result && result.error() == TreeError::no_memory) {
        return std::unexpected(SchedulerError::no_memory);
      }

      if (!result && result.error() == TreeError::callback_failed) {
        return std::unexpected(SchedulerError::callback_failed);
      }

      return std::unexpected(SchedulerError::invalid_argument);
    }

    bool expected = false;

    if (!request.lease_->submitted.compare_exchange_strong(expected, true)) {
      return std::unexpected(SchedulerError::closed);
    }

    try {
      auto event = std::make_shared<const detail::ModelCompletion<T>>(request.id_, request.generation_, request.lease_,
                                                                      std::move(*result));
      auto published = request.publisher_.publish(std::move(event));

      if (!published) {
        request.lease_->submitted.store(false);
      }

      return published;
    } catch (const std::bad_alloc &) {
      request.lease_->submitted.store(false);

      return std::unexpected(SchedulerError::no_memory);
    }
  }

private:
  template <class> friend class ModelConsumer;
  ModelRangeId id_;
  std::uint64_t generation_{};
  std::uint64_t revision_{};
  std::uint64_t row_count_{};
  RowRange range_;
  std::shared_ptr<detail::ModelLease> lease_;
  Publisher<detail::ModelCompletion<T>> publisher_;
};
/** @brief Move-only UI-owned range slot. Equal/overlapping consumers are independent.
 * Close/destruction retires only this consumer; outstanding worker reservations survive.
 * Model and tree may close first. Destruction/move assignment on another thread terminates.
 */
template <class T> class ModelConsumer {
public:
  ModelConsumer() = default; ///< Empty consumer.
  ~ModelConsumer() {
    retire();
  } ///< Logically retire without waiting for workers.
  ModelConsumer(ModelConsumer &&other) noexcept : state_(std::move(other.state_)), id_(std::exchange(other.id_, {})) {
    if (state_ && state_->owner != std::this_thread::get_id()) {
      std::terminate();
    }
  } ///< Transfer on UI.
  ModelConsumer &operator=(ModelConsumer other) noexcept {
    if (state_ && state_->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    std::swap(state_, other.state_);
    std::swap(id_, other.id_);

    return *this;
  } ///< Transfer on UI; retire old ownership only after the full replacement is complete.
  ModelConsumer(const ModelConsumer &) = delete;
  ModelRangeId id() const noexcept {
    return id_;
  } ///< Stable handle for ModelFrame::range.
  /** @brief UI-only request or replace this slot's work; never starts a worker or reads provider data.
   * Caller schedules the returned owning token. A null work owner retains the base snapshot.
   * Oversized/out-of-bounds ranges are invalid_argument; live work budget is would_block.
   * All rejected requests leave this consumer unchanged. Mutations during frame are rejected.
   */
  std::expected<ModelRequest<T>, TreeError> request(RowRange range, std::shared_ptr<const void> work_owner = {}) {
    auto state = state_;

    if (!state || id_.value == 0) {
      return std::unexpected(TreeError::closed);
    }

    auto allowed = state->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto current = state->current;
    auto entry = find(*current);

    if (entry == current->entries.end()) {
      return std::unexpected(TreeError::closed);
    }

    if (range.count > state->options.max_range_rows || range.first > current->snapshot->row_count() ||
        range.count > current->snapshot->row_count() - range.first) {
      return std::unexpected(TreeError::invalid_argument);
    }

    if (state->lifetime->outstanding.load() >= state->options.max_in_flight) {
      return std::unexpected(TreeError::would_block);
    }

    if (entry->generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::identity_exhausted);
    }

    auto next = state->next();

    if (!next) {
      return std::unexpected(next.error());
    }

    try {
      auto lease =
        std::make_shared<detail::ModelLease>(state->lifetime, work_owner ? std::move(work_owner) : current->snapshot);
      auto &updated = *std::find_if((*next)->entries.begin(), (*next)->entries.end(), [this](auto &value) {
        return value.id == id_;
      });

      ++updated.generation;
      updated.work = lease;
      updated.result = ModelRange<T>(current->snapshot, range, {});

      ModelRequest<T> result;

      result.id_ = id_;
      result.generation_ = updated.generation;
      result.revision_ = current->snapshot->revision();
      result.row_count_ = current->snapshot->row_count();
      result.range_ = range;
      result.lease_ = std::move(lease);
      result.publisher_ = state->publisher;

      auto committed = state->commit(std::move(*next));

      if (!committed) {
        return std::unexpected(committed.error());
      }

      if (auto old = entry->work.lock()) {
        old->cancelled.store(true);
      }

      return result;
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    }
  }

  /** @brief Retire this consumer on UI. Idempotent; no allocation, cancellation never joins.
   * Retained tables keep their old result; slot reuse removes the retired entry.
   */
  std::expected<void, TreeError> close() {
    if (state_ && state_->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    retire();

    return {};
  }

private:
  template <class> friend class Model;
  ModelConsumer(std::shared_ptr<detail::ModelState<T>> state, ModelRangeId id) : state_(std::move(state)), id_(id) {
  }

  auto find(const detail::ModelFrameData<T> &data) const {
    return std::find_if(data.entries.begin(), data.entries.end(), [this](const auto &value) {
      return value.id == id_;
    });
  }

  void retire() noexcept {
    auto state = std::move(state_);
    auto id = std::exchange(id_, {});

    if (!state) {
      return;
    }

    if (state->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    // Retire identity before a temporary strong lease can release reentrant user storage.
    state->retired.push_back(id);

    auto entry = std::find_if(state->current->entries.begin(), state->current->entries.end(), [id](const auto &value) {
      return value.id == id;
    });

    if (entry != state->current->entries.end()) {
      if (auto work = entry->work.lock()) {
        work->cancelled.store(true);
      }
    }
  }

  std::shared_ptr<detail::ModelState<T>> state_;
  ModelRangeId id_;
};
/** @brief UI-owned source connecting immutable providers and scoped ranges to a real WidgetTree.
 * The tree must outlive the source; use a long-lived context when views are disposable.
 * Publisher and request tokens may outlive source/tree/scheduler. Read one pin per model
 * per frame. Scheduler dispatch and source mutation belong between frames.
 */
template <class T> class Model {
public:
  /** @brief Create two bounded Scheduler endpoints in the context's scope; initial snapshot is required. */
  static std::expected<std::unique_ptr<Model>, TreeError> create(WidgetTree &tree, Node<Widget> context,
                                                                 std::shared_ptr<const ModelSnapshot<T>> initial,
                                                                 ModelOptions options = {}) {
    if (!initial || options.max_range_rows == 0 || options.max_consumers == 0 || options.max_in_flight == 0) {
      return std::unexpected(TreeError::invalid_argument);
    }

    auto allowed = tree.with_scope(context, [](const UiScope &) {
    });

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto session = detail::model_session();

    if (!session) {
      return std::unexpected(session.error());
    }

    try {
      auto state = std::make_shared<detail::ModelState<T>>(tree, context);

      state->session = *session;
      state->options = options;
      state->retired.reserve(options.max_consumers);
      state->current = std::make_shared<const detail::ModelFrameData<T>>(std::move(initial));

      auto completed = tree.template connect<detail::ModelCompletion<T>>(
        context, [weak = std::weak_ptr(state)](Widget &, std::shared_ptr<const detail::ModelCompletion<T>> value) {
          auto state = weak.lock();

          if (!state || !state->lifetime->alive.load() || value->lease->cancelled.load()) {
            return;
          }

          auto previous = state->current;
          auto entry = std::find_if(previous->entries.begin(), previous->entries.end(), [&](const auto &item) {
            return item.id == value->id;
          });

          if (entry == previous->entries.end() || entry->generation != value->generation ||
              value->result.data_revision() != previous->snapshot->revision() || retired(*state, value->id)) {
            return;
          }

          auto next = state->next();

          if (!next) {
            state->application_error = next.error();

            return;
          }

          (*next)->entries[static_cast<std::size_t>(entry - previous->entries.begin())].result = value->result;

          auto result = state->commit(std::move(*next));

          if (!result) {
            state->application_error = result.error();
          }
        });

      if (!completed) {
        return std::unexpected(completed.error());
      }

      state->publisher = completed->publisher;

      auto snapshots = tree.template connect<ModelSnapshot<T>>(
        context, [weak = std::weak_ptr(state)](Widget &, std::shared_ptr<const ModelSnapshot<T>> value) {
          auto state = weak.lock();

          if (!state || !state->lifetime->alive.load() || value->revision() <= state->current->snapshot->revision()) {
            return;
          }

          auto previous = state->current;
          auto next = state->next();

          if (!next) {
            state->application_error = next.error();

            return;
          }

          (*next)->snapshot = std::move(value);

          for (auto &entry : (*next)->entries) {
            entry.result = ModelRange<T>((*next)->snapshot, {}, {});
            entry.work.reset();
          }

          auto result = state->commit(std::move(*next));

          if (!result) {
            state->application_error = result.error();

            return;
          }

          for (const auto &entry : previous->entries) {
            if (auto work = entry.work.lock()) {
              work->cancelled.store(true);
            }
          }
        });

      if (!snapshots) {
        return std::unexpected(snapshots.error());
      }

      return std::unique_ptr<Model>(new Model(std::move(state), std::move(*completed), std::move(*snapshots)));
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::invalid_argument);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    }
  }

  ~Model() {
    if (state_->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    state_->lifetime->alive.store(false);
  } ///< Close publication delivery on UI, without waiting for worker ownership.
  Model(const Model &) = delete;
  Model &operator=(const Model &) = delete;
  Publisher<ModelSnapshot<T>> publisher() const {
    return snapshots_.publisher;
  } ///< Copy worker-safe full snapshot endpoint on UI.
  /** @brief Capture one immutable frame owner on UI; never reads provider ranges or copies rows. */
  std::expected<ModelFrame<T>, TreeError> pin() const {
    if (state_->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    return ModelFrame<T>(state_->current, state_->options.max_range_rows);
  }

  /** @brief Register one independent consumer; node_limit reports its finite capacity. */
  std::expected<ModelConsumer<T>, TreeError> open_range() {
    auto state = state_;
    auto allowed = state->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (state->current->entries.size() - state->retired.size() >= state->options.max_consumers) {
      return std::unexpected(TreeError::node_limit);
    }

    if (state->identity == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::identity_exhausted);
    }

    auto previous = state->current;
    auto next = state->next();

    if (!next) {
      return std::unexpected(next.error());
    }

    try {
      std::erase_if((*next)->entries, [&](const auto &entry) {
        return retired(*state, entry.id);
      });

      ModelRangeId id{state->session, state->identity + 1};

      (*next)->entries.push_back({id, 0, ModelRange<T>((*next)->snapshot, {}, {}), {}});

      auto committed = state->commit(std::move(*next));

      if (!committed) {
        return std::unexpected(committed.error());
      }

      state->identity = id.value;
      state->retired.clear();

      return ModelConsumer<T>(std::move(state), id);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    }
  }

  std::expected<std::optional<TreeError>, TreeError> last_error() const {
    if (state_->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    return state_->application_error;
  } ///< Last UI application failure; queue acceptance alone is not proof of application.
private:
  static bool retired(const detail::ModelState<T> &state, ModelRangeId id) {
    return std::find(state.retired.begin(), state.retired.end(), id) != state.retired.end();
  }

  Model(std::shared_ptr<detail::ModelState<T>> state, SchedulerConnection<detail::ModelCompletion<T>> completion,
        SchedulerConnection<ModelSnapshot<T>> snapshots)
      : state_(std::move(state)), completion_(std::move(completion)), snapshots_(std::move(snapshots)) {
  }

  std::shared_ptr<detail::ModelState<T>> state_;
  SchedulerConnection<detail::ModelCompletion<T>> completion_;
  SchedulerConnection<ModelSnapshot<T>> snapshots_;
};
} // namespace portals
