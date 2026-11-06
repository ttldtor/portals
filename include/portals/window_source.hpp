// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/row_id.hpp>
#include <portals/source_budget.hpp>
#include <portals/widget.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace portals {
/** @brief Immutable source namespace; replacement sessions must share their old budget root. */
struct SourceStamp {
  std::uint64_t instance{}; ///< Nonzero process-unique session identity.
  std::uint64_t epoch{};    ///< Nonzero namespace fixed at creation.
  /** @param other Namespace to compare.
   * @return True when both fields match.
   */
  bool operator==(const SourceStamp &other) const noexcept = default;
};

/** @brief Owning opaque gap token; no arithmetic, row count or pointer semantics. */
struct SourceCursor {
  SourceStamp source;                   ///< Issuing immutable namespace.
  std::array<std::uint64_t, 2> token{}; ///< Provider-validated gap representation.
  /** @param other Token to compare.
   * @return True when namespace and payload match.
   */
  bool operator==(const SourceCursor &other) const noexcept = default;
};

/** @brief Requested traversal; returned rows remain in display order. */
enum class SourceDirection {
  FORWARD, ///< Read after the requested gap.
  BACKWARD ///< Read before the requested gap.
};
/** @brief Entry operation; ENTRY does not promise a global origin. */
enum class SourceAnchorKind {
  ENTRY,  ///< Provider entry, without a global-origin promise.
  CURSOR, ///< Valid gap token in this immutable session.
  TAIL,   ///< Capability-gated available-tail window.
  ROW     ///< Capability-gated stable-identity lookup.
};
/** @brief State beyond a ready page in the requested direction. */
enum class SourceEdge {
  MORE,           ///< Continuation exists; short/empty pages do not imply end.
  TEMPORARY_TAIL, ///< Current forward tail; wait for readiness or user demand.
  FINAL_END       ///< Definitive boundary in this direction for this session.
};

/** @brief Owning navigation intent, including optional capability-gated operations. */
struct SourceAnchor {
  SourceAnchorKind kind{SourceAnchorKind::ENTRY}; ///< Operation to resolve on a worker.
  SourceCursor cursor;                            ///< Used for CURSOR only.
  RowId row;                                      ///< Used for ROW only.
};

/** @brief Immutable navigation capabilities of the delivery channel, not of the complete model. */
struct SourceCapabilities {
  bool backward{}; ///< Read before a gap without an unbounded UI scan.
  bool seekRow{};  ///< Resolve stable row identity using a bounded worker operation.
  bool tail{};     ///< Locate available tail on a worker.
};

/** @brief Positive bounded storage/work defaults, independent of logical source length. */
struct WindowSourceOptions {
  std::uint64_t initialRevision{1};       ///< Positive initial provider readiness/data revision.
  std::size_t pageBytes{256 * 1024};      ///< Entire page reservation, including metadata allowance.
  std::size_t maxRowBytes{64 * 1024};     ///< Inline row plus newly allocated transitive payload.
  std::size_t maxPageRows{1024};          ///< Largest provider command.
  std::size_t workspaceBytes{128 * 1024}; ///< Reserved worker workspace.
  std::size_t envelopeBytes{4096};        ///< Work/queue/control-block metadata allowance.
};

/** @brief Immutable worker command; maxRows is an upper bound, never an extent. */
struct SourceReadCommand {
  SourceStamp source;                                  ///< Expected session namespace.
  std::uint64_t dataRevision{};                        ///< Exact requested readiness/data revision.
  SourceAnchor anchor;                                 ///< Latest bounded navigation intent.
  SourceDirection direction{SourceDirection::FORWARD}; ///< Traversal direction.
  std::size_t maxRows{};                               ///< Positive remaining demand, capped per page.
};

/** @brief Immutable typed row; use arena storage for all dynamically owned payload.
 * @tparam T Worker-destructible value with no mutable aliases while published.
 */
template <class T> struct WindowRow {
  RowId id;                    ///< Stable provider identity, independent of position.
  std::uint64_t valueRevision; ///< Positive conditional-write identity.
  T value;                     ///< Transitively immutable inline or arena-accounted payload.

  /** @tparam Args Direct constructor arguments; no intermediate T is moved.
   * @param identity Stable row identity.
   * @param revision Positive value revision.
   * @param args Arguments forwarded to T in its final storage.
   */
  template <class... Args>
  WindowRow(RowId identity, std::uint64_t revision, Args &&...args)
      : id(identity), valueRevision(revision), value(std::forward<Args>(args)...) {
  }
};

/// @cond INTERNAL
namespace detail {
inline SourceError sourceTreeError(TreeError error) {
  switch (error) {
  case TreeError::INVALID_ARGUMENT:
    return SourceError::INVALID_ARGUMENT;
  case TreeError::WRONG_THREAD:
    return SourceError::WRONG_THREAD;
  case TreeError::CLOSED:
    return SourceError::CLOSED;
  case TreeError::WOULD_BLOCK:
    return SourceError::WOULD_BLOCK;
  case TreeError::NO_MEMORY:
    return SourceError::NO_MEMORY;
  case TreeError::IDENTITY_EXHAUSTED:
    return SourceError::IDENTITY_EXHAUSTED;
  case TreeError::CALLBACK_FAILED:
    return SourceError::CALLBACK_FAILED;
  default:
    return SourceError::CONTEXT_ERROR;
  }
}

std::expected<SourceStamp, SourceError> windowSourceStamp();

template <class T> struct WindowPageData {
  explicit WindowPageData(std::shared_ptr<SourceArena> storage) : arena(std::move(storage)) {
  }

  ~WindowPageData() {
    while (count) {
      std::destroy_at(rows + --count);
    }
  }

  std::shared_ptr<SourceArena> arena;
  WindowRow<T> *rows{};
  std::size_t count{};
  SourceCursor before;
  SourceCursor after;
  std::uint64_t revision{};
};
} // namespace detail

/// @endcond

template <class T> class PageBuilder;

/** @brief Cheap owning page; copying never copies T or allocates.
 * @tparam T Immutable worker-destructible row value.
 */
template <class T> class WindowPage {
public:
  WindowPage() = default; ///< Empty owning page.

  /** @return Rows borrowed for this handle's lifetime. */
  std::span<const WindowRow<T>> rows() const noexcept {
    return data_ ? std::span<const WindowRow<T>>(data_->rows, data_->count) : std::span<const WindowRow<T>>{};
  }

  /** @return Gap preceding this page, or an invalid cursor when empty. */
  SourceCursor before() const noexcept {
    return data_ ? data_->before : SourceCursor{};
  }

  /** @return Gap following this page, or an invalid cursor when empty. */
  SourceCursor after() const noexcept {
    return data_ ? data_->after : SourceCursor{};
  }

  /** @return Immutable page revision, or zero for an empty handle. */
  std::uint64_t dataRevision() const noexcept {
    return data_ ? data_->revision : 0;
  }

private:
  friend class PageBuilder<T>;

  explicit WindowPage(std::shared_ptr<const detail::WindowPageData<T>> data) : data_(std::move(data)) {
  }

  std::shared_ptr<const detail::WindowPageData<T>> data_;
};

/** @brief Distinct provider states; a short ready page never implies end. */
enum class SourceReadState {
  READY,               ///< Owning page and explicit edge are available.
  PENDING,             ///< Data is not ready; no row count or EOF is inferred.
  HISTORY_UNAVAILABLE, ///< A formerly valid gap expired; recovery is explicit.
  FAILED               ///< Typed failure, without replacement rows.
};

/** @brief Owning result with fixed metadata; no optional/variant deep copies.
 * @tparam T Immutable row value; page storage is shared.
 */
template <class T> struct SourceReadReply {
  SourceReadState state{SourceReadState::PENDING}; ///< Explicit readiness/end-independent result.
  WindowPage<T> page;                              ///< Valid for READY only.
  SourceEdge edge{SourceEdge::MORE};               ///< Boundary for READY only.
  std::uint64_t observedRevision{};                ///< PENDING must match the command revision.
  std::optional<SourceCursor> recovery;            ///< Optional explicit HISTORY_UNAVAILABLE alternative.
  SourceError error{SourceError::INVALID_REPLY};   ///< FAILED diagnostic.
};

/** @brief One reserved worker-only builder. Never expose its writable resource after publication.
 * @tparam T Nothrow-destructible row; construct dynamic payload using resource(), not an external allocator.
 */
template <class T> class PageBuilder {
public:
  /** @return Reserved monotonic page resource; no upstream fallback or UI use. */
  std::pmr::memory_resource *resource() noexcept {
    return data_->arena.get();
  }

  /** @return Separately reserved temporary worker memory; no borrow may escape load. */
  std::pmr::memory_resource *workspace() noexcept {
    return workspace_.get();
  }

  /** @tparam Args Constructor arguments for T; dynamic storage must use resource().
   * @param id Positive stable row identity.
   * @param revision Positive row value revision.
   * @param args Arguments forwarded directly to the in-place value constructor.
   * @return Success or a typed row/page/allocation failure; no row is appended on failure.
   */
  template <class... Args> std::expected<void, SourceError> emplace(RowId id, std::uint64_t revision, Args &&...args) {
    if (sealed_ || !id.value || !id.generation || !revision || data_->count >= command_.maxRows) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (sizeof(WindowRow<T>) > maxRowBytes_) {
      return std::unexpected(SourceError::ROW_TOO_LARGE);
    }

    auto used = data_->arena->used();
    auto row = data_->rows + data_->count;

    try {
      std::construct_at(row, id, revision, std::forward<Args>(args)...);
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::PAGE_TOO_LARGE);
    } catch (...) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }

    if (data_->arena->used() - used > maxRowBytes_ - sizeof(WindowRow<T>)) {
      std::destroy_at(row);

      return std::unexpected(SourceError::ROW_TOO_LARGE);
    }

    ++data_->count;

    return {};
  }

  /** @param before Resolved gap before the returned rows.
   * @param after Resolved gap after the returned rows.
   * @param edge Explicit continuation/end state in the command direction.
   * @return Immutable owning page, or INVALID_REPLY on repeated seal/foreign stamp.
   */
  std::expected<SourceReadReply<T>, SourceError> ready(SourceCursor before, SourceCursor after, SourceEdge edge) {
    if (sealed_ || before.source != command_.source || after.source != command_.source) {
      return std::unexpected(SourceError::INVALID_REPLY);
    }

    data_->before = before;
    data_->after = after;
    data_->revision = command_.dataRevision;
    sealed_ = true;
    SourceReadReply<T> result;
    result.state = SourceReadState::READY;
    result.page = WindowPage<T>(data_);
    result.edge = edge;

    return result;
  }

private:
  template <class> friend class WindowRequest;

  PageBuilder(SourceReadCommand command, std::shared_ptr<SourceArena> arena, std::shared_ptr<SourceArena> workspace,
              std::size_t maxRowBytes)
      : command_(command), workspace_(std::move(workspace)), maxRowBytes_(maxRowBytes) {
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(sizeof(detail::WindowPageData<T>) < SourceArena::METADATA_BYTES / 2);
    data_ = std::make_shared<detail::WindowPageData<T>>(std::move(arena));

    if (command.maxRows > data_->arena->capacity() / sizeof(WindowRow<T>)) {
      throw std::bad_alloc();
    }

    data_->rows = static_cast<WindowRow<T> *>(
      data_->arena->allocate(command.maxRows * sizeof(WindowRow<T>), alignof(WindowRow<T>)));
  }

  SourceReadCommand command_;
  std::shared_ptr<detail::WindowPageData<T>> data_;
  std::shared_ptr<SourceArena> workspace_;
  std::size_t maxRowBytes_;
  bool sealed_{};
};

/** @brief Worker provider; the source captures capabilities once and never calls load on UI.
 * @tparam T Immutable row value whose transitive dynamic page storage uses the builder resource.
 */
template <class T> class WindowProvider {
public:
  virtual ~WindowProvider() = default; ///< Safe final destruction on any releasing worker.
  /** @return Immutable session capabilities; no allocation or I/O. */
  virtual SourceCapabilities capabilities() const noexcept = 0;
  /** @param command Immutable bounded command borrowed for this invocation.
   * @param out Reserved page and temporary resources; do not retain writable aliases.
   * @param stop Cooperative cancellation; return promptly when the underlying operation permits.
   * @return Owning ready/pending/history reply or typed failure.
   */
  virtual std::expected<SourceReadReply<T>, SourceError> load(const SourceReadCommand &command, PageBuilder<T> &out,
                                                              std::stop_token stop) = 0;
};
/** @brief Current consumer outcome; ready pins remain valid through every transition. */
enum class WindowStatus {
  EMPTY,               ///< No explicit navigation has started.
  LOADING,             ///< Admitted work needs worker dispatch or delivery.
  READY,               ///< Layout demand is filled; pin retains its explicit edge.
  PENDING,             ///< Readiness is pending; previous pin remains valid.
  TEMPORARY_TAIL,      ///< Unfilled window reached the current forward tail.
  FINAL_END,           ///< Unfilled window reached a definitive boundary.
  HISTORY_UNAVAILABLE, ///< Requested history expired; recovery is not automatic.
  FAILED               ///< lastError identifies failure; committed pin remains valid.
};

/// @cond INTERNAL
namespace detail {
template <class T> struct WindowState;
template <class T> struct WindowConsumerState;
template <class T> struct WindowWork;
struct WindowRequestAccess;

template <class T> struct WindowFrameData {
  SourceCredit credit;
  std::array<WindowPage<T>, 4> pages;
  std::size_t count{};
  std::size_t rows{};
  SourceCursor continuation;
  SourceEdge edge{SourceEdge::MORE};
  std::uint64_t revision{};
};

template <class T> struct WindowCompletion {
  std::shared_ptr<WindowWork<T>> work;
  SourceReadReply<T> reply;
};

struct WindowNotification {
  SourceCredit credit;
  SourceStamp source;
  std::uint64_t revision;
};

// Only scheduler callback captures own this marker. Node/scope retirement releases it
// before WidgetTree destruction, so weak-marker validation precedes every raw-tree access.
struct WindowScopeMarker {};

template <class T> struct WindowWork {
  SourceCredit credit;
  SourceCredit pageCredit;
  std::shared_ptr<SourceArena> workspace;
  std::shared_ptr<WindowProvider<T>> provider;
  std::shared_ptr<std::atomic<bool>> alive;
  std::weak_ptr<WindowScopeMarker> marker;
  std::weak_ptr<WindowConsumerState<T>> consumer;
  Publisher<WindowCompletion<T>> publisher;
  SourceReadCommand command;
  WindowSourceOptions options;
  std::thread::id uiThread;
  std::uint64_t generation{};
  std::stop_source stop;
  std::atomic<bool> loaded{};
  std::atomic<bool> submitted{};
};
} // namespace detail

/// @endcond

/** @brief Owning immutable committed window, containing at most four bounded pages.
 * @tparam T Immutable row value; copying a pin never copies rows or allocates.
 */
template <class T> class WindowPin {
public:
  WindowPin() = default; ///< Empty pin.

  /** @return Number of pages in this local window, never a source extent. */
  std::size_t pageCount() const noexcept {
    return data_ ? data_->count : 0;
  }

  /** @return Number of rows in this local window, never a source extent. */
  std::size_t size() const noexcept {
    return data_ ? data_->rows : 0;
  }

  /** @param index Page in [0,pageCount()).
   * @return Owning page, or an empty page for an invalid index.
   */
  WindowPage<T> page(std::size_t index) const noexcept {
    return data_ && index < data_->count ? data_->pages[index] : WindowPage<T>{};
  }

  /** @return Continuation in the requested direction; invalid for an empty pin. */
  SourceCursor continuation() const noexcept {
    return data_ ? data_->continuation : SourceCursor{};
  }

  /** @return Explicit edge at continuation; never inferred from size(). */
  SourceEdge edge() const noexcept {
    return data_ ? data_->edge : SourceEdge::MORE;
  }

  /** @return Immutable data/readiness revision, or zero for an empty pin. */
  std::uint64_t dataRevision() const noexcept {
    return data_ ? data_->revision : 0;
  }

private:
  template <class> friend class WindowConsumer;

  explicit WindowPin(std::shared_ptr<const detail::WindowFrameData<T>> data) : data_(std::move(data)) {
  }

  std::shared_ptr<const detail::WindowFrameData<T>> data_;
};

/** @brief Copyable worker token. Dispatch load() through the existing background executor.
 * The library owns no worker/runtime. Copies share cancellation and one reservation; final release
 * returns capacity after the provider, workspace, results and queued owners have stopped using it.
 * @tparam T Immutable worker-destructible row value.
 */
template <class T> class WindowRequest {
public:
  WindowRequest() = default; ///< Empty cancelled request.

  /** @return Immutable command; a default token has a zero command. */
  SourceReadCommand command() const noexcept {
    return work_ ? work_->command : SourceReadCommand{};
  }

  /** @return True after cancellation/source closure or for an empty token. */
  bool stopRequested() const noexcept {
    return !work_ || !work_->alive->load() || work_->marker.expired() || work_->stop.stop_requested();
  }

  /** @brief Cooperative cancellation only; does not release shared capacity or join.
   * Registered stop callbacks execute synchronously and must be short and worker-safe.
   */
  void cancel() const noexcept {
    if (work_) {
      work_->stop.request_stop();
    }
  }

  /** @param external Optional host cancellation, combined with this request's cancellation.
   * @return Owning reply/error. UI invocation is rejected; each token generation loads once.
   */
  std::expected<SourceReadReply<T>, SourceError> load(std::stop_token external = {}) const {
    return loadUsing(
      [](WindowProvider<T> &provider, const SourceReadCommand &command, PageBuilder<T> &out, std::stop_token stop) {
        return provider.load(command, out, stop);
      },
      external);
  }

  /** @param reply Owning load result; retry the same result after WOULD_BLOCK/NO_MEMORY.
   * @return Existing queue admission, never UI application; one accepted publication per generation.
   */
  std::expected<PublishResult, SchedulerError>
  complete(const std::expected<SourceReadReply<T>, SourceError> &reply) const {
    auto work = work_;

    if (stopRequested()) {
      return std::unexpected(SchedulerError::CLOSED);
    }

    bool expected = false;

    if (!work->submitted.compare_exchange_strong(expected, true)) {
      return std::unexpected(SchedulerError::CLOSED);
    }

    try {
      SourceReadReply<T> value;

      if (reply) {
        value = *reply;
      } else {
        value.state = SourceReadState::FAILED;
        value.error = reply.error();
      }

      auto event = std::make_shared<const detail::WindowCompletion<T>>(work, std::move(value));
      auto published = work->publisher.publish(std::move(event));

      if (!published) {
        work->submitted.store(false);
      }

      return published;
    } catch (const std::bad_alloc &) {
      work->submitted.store(false);

      return std::unexpected(SchedulerError::NO_MEMORY);
    }
  }

private:
  friend struct detail::WindowRequestAccess;
  template <class> friend class WindowConsumer;

  template <class Loader>
  std::expected<SourceReadReply<T>, SourceError> loadUsing(Loader &&loader, std::stop_token external) const {
    auto work = work_;

    if (stopRequested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    if (work->uiThread == std::this_thread::get_id()) {
      return std::unexpected(SourceError::WRONG_THREAD);
    }

    if (work->loaded.exchange(true)) {
      return std::unexpected(SourceError::ALREADY_COMPLETED);
    }

    std::stop_callback stop(external, [work] {
      work->stop.request_stop();
    });

    try {
      auto arena = SourceArena::create(std::move(work->pageCredit));

      if (!arena) {
        return std::unexpected(arena.error());
      }

      auto temporary = work->credit.split({work->options.workspaceBytes, 0, 0, 0});
      auto workspace = SourceArena::create(std::move(*temporary));

      if (!workspace) {
        return std::unexpected(workspace.error());
      }

      work->workspace = *workspace;
      PageBuilder<T> out(work->command, std::move(*arena), *workspace, work->options.maxRowBytes);
      auto reply = std::forward<Loader>(loader)(*work->provider, work->command, out, work->stop.get_token());

      if (stopRequested()) {
        return std::unexpected(SourceError::CANCELLED);
      }

      return reply;
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }
  }

  explicit WindowRequest(std::shared_ptr<detail::WindowWork<T>> work) : work_(std::move(work)) {
  }

  std::shared_ptr<detail::WindowWork<T>> work_;
};

/// @cond INTERNAL
namespace detail {
// Trusted adapters preserve richer per-call failures on their owning worker job.
// The loader receives the actual captured provider and shares public load()'s
// affinity, single-load, cancellation and reservation gates without a side channel.
struct WindowRequestAccess {
  template <class T, class Loader>
  static std::expected<SourceReadReply<T>, SourceError> load(const WindowRequest<T> &request, Loader &&loader,
                                                             std::stop_token external = {}) {
    return request.loadUsing(std::forward<Loader>(loader), external);
  }
};

template <class T> struct WindowConsumerState {
  SourceCredit credit;
  std::shared_ptr<WindowState<T>> source;
  std::shared_ptr<const WindowFrameData<T>> current;
  std::shared_ptr<WindowWork<T>> pending;
  std::weak_ptr<WindowWork<T>> active;
  std::array<WindowPage<T>, 4> pages;
  std::array<SourceCursor, 5> visited;
  SourceAnchor intent;
  SourceDirection direction{SourceDirection::FORWARD};
  std::uint64_t generation{};
  std::uint64_t desiredRevision{1};
  std::uint64_t attemptedRevision{};
  std::uint64_t fillRevision{};
  std::size_t target{};
  std::size_t accumulated{};
  std::size_t pageCount{};
  std::size_t visitedCount{};
  std::size_t loads{};
  WindowStatus status{WindowStatus::EMPTY};
  std::optional<SourceError> error;
  std::optional<SourceCursor> recovery;
  bool retryNeeded{};
  bool closed{};
};

template <class T> struct WindowState {
  WindowState(WidgetTree &value, Node<Widget> node, std::size_t consumers)
      : tree(value), target(node), slots(consumers) {
  }

  std::expected<void, SourceError> allowed() const {
    if (owner != std::this_thread::get_id()) {
      return std::unexpected(SourceError::WRONG_THREAD);
    }

    if (!alive->load() || marker.expired()) {
      return std::unexpected(SourceError::CLOSED);
    }

    auto result = tree.withScope(target, [](const UiScope &) {
    });

    if (!result) {
      return std::unexpected(sourceTreeError(result.error()));
    }

    return {};
  }

  SourceCredit credit;
  WidgetTree &tree;
  Node<Widget> target;
  const std::thread::id owner{std::this_thread::get_id()};
  std::shared_ptr<SourceBudget> budget;
  std::shared_ptr<WindowProvider<T>> provider;
  std::shared_ptr<std::atomic<bool>> alive;
  std::weak_ptr<WindowScopeMarker> marker;
  std::vector<std::weak_ptr<WindowConsumerState<T>>> slots;
  SourceStamp stamp;
  SourceCapabilities capabilities;
  WindowSourceOptions options;
  Publisher<WindowCompletion<T>> publisher;
  std::mutex notificationMutex;
  std::uint64_t publishedRevision{1};
  std::uint64_t revision{1};
};

template <class T>
std::expected<std::shared_ptr<WindowWork<T>>, SourceError>
admitWindowWork(const std::shared_ptr<WindowConsumerState<T>> &consumer, SourceReadCommand command,
                std::uint64_t generation) {
  auto source = consumer->source;
  const auto &options = source->options;
  auto total = options.pageBytes;

  if (options.workspaceBytes > std::numeric_limits<std::size_t>::max() - total ||
      options.envelopeBytes > std::numeric_limits<std::size_t>::max() - total - options.workspaceBytes) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  total += options.workspaceBytes + options.envelopeBytes;
  auto credit = source->budget->reserve({total, 1, 1, 0});

  if (!credit) {
    return std::unexpected(credit.error());
  }

  try {
    auto work = std::make_shared<WindowWork<T>>();
    auto page = credit->split({options.pageBytes, 1, 0, 0});
    work->credit = std::move(*credit);
    work->pageCredit = std::move(*page);
    work->provider = source->provider;
    work->alive = source->alive;
    work->marker = source->marker;
    work->consumer = consumer;
    work->publisher = source->publisher;
    work->options = options;
    work->uiThread = source->owner;
    work->generation = generation;
    work->command = command;
    work->command.maxRows =
      std::min(command.maxRows, (options.pageBytes - SourceArena::METADATA_BYTES) / sizeof(WindowRow<T>));

    return work;
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

template <class T> void failWindow(WindowConsumerState<T> &consumer, SourceError error) {
  consumer.status = WindowStatus::FAILED;
  consumer.error = error;
  consumer.pageCount = 0;
  consumer.visitedCount = 0;
  consumer.accumulated = 0;
  consumer.pending.reset();
  consumer.pages = {};
}

template <class T> void restartWindow(const std::shared_ptr<WindowConsumerState<T>> &consumer) {
  if (consumer->closed || !consumer->target || !consumer->retryNeeded ||
      consumer->desiredRevision <= consumer->attemptedRevision) {
    return;
  }

  consumer->attemptedRevision = consumer->desiredRevision;
  consumer->fillRevision = consumer->desiredRevision;
  consumer->retryNeeded = false;
  consumer->pages = {};
  consumer->pageCount = 0;
  consumer->visitedCount = 0;
  consumer->accumulated = 0;
  consumer->loads = 0;
  SourceReadCommand command{consumer->source->stamp, consumer->fillRevision, consumer->intent, consumer->direction,
                            std::min(consumer->source->options.maxPageRows, consumer->target)};
  auto work = admitWindowWork(consumer, command, consumer->generation);

  if (!work) {
    failWindow(*consumer, work.error());

    return;
  }

  if (consumer->closed || !consumer->source->allowed()) {
    return;
  }

  consumer->active = *work;
  consumer->pending = std::move(*work);
  consumer->status = WindowStatus::LOADING;
}

template <class T>
std::expected<void, SourceError> commitWindow(const std::shared_ptr<WindowConsumerState<T>> &consumer,
                                              SourceCursor continuation, SourceEdge edge) {
  auto source = consumer->source;
  auto credit = source->budget->reserve({SourceArena::METADATA_BYTES, 0, 0, 0});

  if (!credit) {
    return std::unexpected(credit.error());
  }

  try {
    auto next = std::make_shared<WindowFrameData<T>>();
    next->credit = std::move(*credit);
    for (std::size_t i = 0; i < consumer->pageCount; ++i) {
      auto index = consumer->direction == SourceDirection::BACKWARD ? consumer->pageCount - i - 1 : i;
      next->pages[i] = consumer->pages[index];
    }
    next->count = consumer->pageCount;
    next->rows = consumer->accumulated;
    next->continuation = continuation;
    next->edge = edge;
    next->revision = consumer->fillRevision;
    auto allowed = source->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto invalidated = source->tree.invalidate(source->target, Invalidation::LAYOUT);

    if (!invalidated) {
      return std::unexpected(sourceTreeError(invalidated.error()));
    }

    auto previous = std::exchange(consumer->current, std::move(next));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

template <class T>
void applyWindowCompletion(const std::shared_ptr<WindowState<T>> &source, const WindowCompletion<T> &event) {
  auto consumer = event.work->consumer.lock();

  if (!consumer || consumer->closed || consumer->generation != event.work->generation || !source->allowed()) {
    return;
  }

  // Pending/error transitions and a newly admitted continuation also need an application cycle.
  auto invalidated = source->tree.invalidate(source->target, Invalidation::LAYOUT);

  if (!invalidated) {
    failWindow(*consumer, sourceTreeError(invalidated.error()));

    return;
  }

  consumer->active.reset();

  if (event.work->stop.stop_requested()) {
    consumer->retryNeeded = false;
    failWindow(*consumer, SourceError::CANCELLED);

    return;
  }

  if (event.work->command.dataRevision != consumer->desiredRevision ||
      (event.reply.state == SourceReadState::FAILED && event.reply.error == SourceError::REVISION_CHANGED)) {
    if (consumer->desiredRevision <= consumer->attemptedRevision) {
      failWindow(*consumer, SourceError::REVISION_CHANGED);

      return;
    }

    restartWindow(consumer);

    return;
  }

  const auto &reply = event.reply;

  if (reply.state != SourceReadState::READY) {
    if (reply.state == SourceReadState::PENDING && reply.observedRevision == consumer->fillRevision) {
      consumer->status = WindowStatus::PENDING;
    } else if (reply.state == SourceReadState::HISTORY_UNAVAILABLE &&
               (!reply.recovery || reply.recovery->source == source->stamp)) {
      consumer->status = WindowStatus::HISTORY_UNAVAILABLE;
      consumer->recovery = reply.recovery;
    } else {
      failWindow(*consumer, reply.state == SourceReadState::FAILED ? reply.error : SourceError::INVALID_REPLY);
    }

    restartWindow(consumer);

    return;
  }

  const auto rows = reply.page.rows();
  auto before = reply.page.before();
  auto after = reply.page.after();
  auto input = consumer->direction == SourceDirection::FORWARD ? before : after;
  auto continuation = consumer->direction == SourceDirection::FORWARD ? after : before;

  if (before.source != source->stamp || after.source != source->stamp ||
      reply.page.dataRevision() != consumer->fillRevision || rows.size() > event.work->command.maxRows ||
      (event.work->command.anchor.kind == SourceAnchorKind::CURSOR && input != event.work->command.anchor.cursor) ||
      (reply.edge != SourceEdge::MORE && reply.edge != SourceEdge::TEMPORARY_TAIL &&
       reply.edge != SourceEdge::FINAL_END) ||
      (reply.edge == SourceEdge::TEMPORARY_TAIL && consumer->direction != SourceDirection::FORWARD)) {
    failWindow(*consumer, SourceError::INVALID_REPLY);

    return;
  }

  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!rows[i].id.value || !rows[i].id.generation || !rows[i].valueRevision) {
      failWindow(*consumer, SourceError::INVALID_REPLY);

      return;
    }

    bool duplicate = std::ranges::any_of(rows.first(i), [&](const auto &row) {
      return row.id == rows[i].id;
    });

    for (std::size_t page = 0; !duplicate && page < consumer->pageCount; ++page) {
      duplicate = std::ranges::any_of(consumer->pages[page].rows(), [&](const auto &row) {
        return row.id == rows[i].id;
      });
    }

    if (duplicate) {
      failWindow(*consumer, SourceError::INVALID_REPLY);

      return;
    }
  }

  if (!consumer->visitedCount) {
    consumer->visited[consumer->visitedCount++] = input;
  }

  if (reply.edge == SourceEdge::MORE) {
    if (continuation == input) {
      failWindow(*consumer, SourceError::NO_PROGRESS);

      return;
    }

    if (std::ranges::find(std::span(consumer->visited).first(consumer->visitedCount), continuation) !=
        std::span(consumer->visited).first(consumer->visitedCount).end()) {
      failWindow(*consumer, SourceError::CURSOR_CYCLE);

      return;
    }

    consumer->visited[consumer->visitedCount++] = continuation;
  }

  consumer->pages[consumer->pageCount++] = reply.page;
  consumer->accumulated += rows.size();
  ++consumer->loads;
  bool fulfilled = consumer->accumulated >= consumer->target;

  if (fulfilled || reply.edge != SourceEdge::MORE || consumer->loads == 4) {
    auto committed = commitWindow(consumer, continuation, reply.edge);

    if (!committed) {
      failWindow(*consumer, committed.error());

      return;
    }

    if (fulfilled) {
      consumer->status = WindowStatus::READY;
    } else if (reply.edge == SourceEdge::FINAL_END) {
      consumer->status = WindowStatus::FINAL_END;
    } else if (reply.edge == SourceEdge::TEMPORARY_TAIL) {
      consumer->status = WindowStatus::TEMPORARY_TAIL;
    } else {
      failWindow(*consumer, SourceError::CONTINUATION_LIMIT);
    }

    restartWindow(consumer);

    return;
  }

  SourceAnchor anchor;
  anchor.kind = SourceAnchorKind::CURSOR;
  anchor.cursor = continuation;
  SourceReadCommand command{source->stamp, consumer->fillRevision, anchor, consumer->direction,
                            std::min(source->options.maxPageRows, consumer->target - consumer->accumulated)};
  auto next = admitWindowWork(consumer, command, consumer->generation);

  if (!next) {
    failWindow(*consumer, next.error());

    return;
  }

  if (consumer->closed || !source->allowed()) {
    return;
  }

  consumer->active = *next;
  consumer->pending = std::move(*next);
}
} // namespace detail

/// @endcond

/** @brief Move-only UI consumer. Fetch continuation/retry tokens with takeRequest(), then dispatch on a worker.
 * @tparam T Immutable row value; pins remain readable after consumer/source destruction.
 */
template <class T> class WindowConsumer {
public:
  WindowConsumer() = default; ///< Empty closed consumer.

  ~WindowConsumer() {
    close();
  } ///< Cancel without waiting; outstanding owners retain credit.

  /** @param other UI owner to transfer and leave empty; foreign-thread transfer terminates. */
  WindowConsumer(WindowConsumer &&other) noexcept : state_(std::move(other.state_)) {
    if (state_ && state_->source->owner != std::this_thread::get_id()) {
      std::terminate();
    }
  } ///< Transfer on UI.

  /** @param other Replacement UI owner.
   * @return This owner; old consumer closes after replacement.
   */
  WindowConsumer &operator=(WindowConsumer other) noexcept {
    state_.swap(other.state_);

    return *this;
  }

  WindowConsumer(const WindowConsumer &) = delete;

  /** @return Success after cancelling the current navigation intent without closing this consumer.
   * Later readiness notifications do not restart it; an explicit request is required.
   */
  std::expected<void, SourceError> cancel() {
    auto state = state_;

    if (!state || state->closed) {
      return std::unexpected(SourceError::CLOSED);
    }

    auto allowed = state->source->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto invalidated = state->source->tree.invalidate(state->source->target, Invalidation::LAYOUT);

    if (!invalidated) {
      return std::unexpected(detail::sourceTreeError(invalidated.error()));
    }

    state->target = 0;
    state->retryNeeded = false;

    if (state->generation != std::numeric_limits<std::uint64_t>::max()) {
      ++state->generation;
    }

    auto work = state->active.lock();
    detail::failWindow(*state, SourceError::CANCELLED);

    if (work) {
      work->stop.request_stop();
    }

    return {};
  }

  /** @brief Close on UI; no implicit restart follows explicit cancellation. */
  void close() noexcept {
    auto state = std::move(state_);

    if (!state) {
      return;
    }

    if (state->source->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    state->closed = true;
    state->retryNeeded = false;

    if (auto work = state->active.lock()) {
      work->stop.request_stop();
    }

    state->pending.reset();
  }

  /** @param anchor Provider entry or supported gap/seek operation.
   * @param direction Requested traversal direction.
   * @param viewportRows Positive layout-derived target, including desired overscan.
   * @return First admitted worker token; rejection preserves the previous navigation/window.
   */
  std::expected<WindowRequest<T>, SourceError> request(SourceAnchor anchor, SourceDirection direction,
                                                       std::size_t viewportRows) {
    auto state = state_;

    if (!state || state->closed) {
      return std::unexpected(SourceError::CLOSED);
    }

    auto source = state->source;
    auto allowed = source->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (!viewportRows || (direction != SourceDirection::FORWARD && direction != SourceDirection::BACKWARD) ||
        (anchor.kind != SourceAnchorKind::ENTRY && anchor.kind != SourceAnchorKind::CURSOR &&
         anchor.kind != SourceAnchorKind::TAIL && anchor.kind != SourceAnchorKind::ROW)) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if ((direction == SourceDirection::BACKWARD && !source->capabilities.backward) ||
        (anchor.kind == SourceAnchorKind::TAIL && !source->capabilities.tail) ||
        (anchor.kind == SourceAnchorKind::ROW && !source->capabilities.seekRow)) {
      return std::unexpected(SourceError::UNSUPPORTED);
    }

    if (anchor.kind == SourceAnchorKind::CURSOR && anchor.cursor.source != source->stamp) {
      return std::unexpected(SourceError::STALE_SOURCE);
    }

    if (anchor.kind == SourceAnchorKind::ROW && (!anchor.row.value || !anchor.row.generation)) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (state->generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
    }

    // Reserve the replacement before changing intent or cancelling the old generation.
    auto previousGeneration = state->generation;
    SourceReadCommand command{source->stamp, source->revision, anchor, direction,
                              std::min(source->options.maxPageRows, viewportRows)};
    auto work = detail::admitWindowWork(state, command, previousGeneration + 1);

    if (!work) {
      return std::unexpected(work.error());
    }

    if (state->closed || !source->allowed()) {
      return std::unexpected(SourceError::CLOSED);
    }

    if (state->generation != previousGeneration) {
      return std::unexpected(SourceError::WOULD_BLOCK);
    }

    auto invalidated = source->tree.invalidate(source->target, Invalidation::LAYOUT);

    if (!invalidated) {
      return std::unexpected(detail::sourceTreeError(invalidated.error()));
    }

    (*work)->consumer = state;

    auto previous = state->active.lock();

    state->intent = anchor;
    state->direction = direction;
    state->target = viewportRows;
    ++state->generation;
    state->fillRevision = source->revision;
    state->attemptedRevision = source->revision;
    state->desiredRevision = source->revision;
    state->retryNeeded = false;
    state->pages = {};
    state->pageCount = 0;
    state->visitedCount = 0;
    state->loads = 0;
    state->accumulated = 0;
    state->error.reset();
    state->recovery.reset();
    state->pending.reset();
    state->active = *work;
    state->status = WindowStatus::LOADING;

    if (previous) {
      // A stop callback may destroy the public consumer/source; no member access follows it.
      previous->stop.request_stop();
    }

    return WindowRequest<T>(std::move(*work));
  }

  /** @return Next admitted continuation/latest-readiness token, or empty; never starts a worker. */
  std::expected<std::optional<WindowRequest<T>>, SourceError> takeRequest() {
    if (!state_ || state_->closed) {
      return std::unexpected(SourceError::CLOSED);
    }

    auto allowed = state_->source->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (!state_->pending) {
      return std::optional<WindowRequest<T>>{};
    }

    return std::optional<WindowRequest<T>>(WindowRequest<T>(std::exchange(state_->pending, {})));
  }

  /** @return Cheap last committed pin; pending/errors never replace its valid rows. */
  std::expected<WindowPin<T>, SourceError> pin() const {
    if (!state_) {
      return std::unexpected(SourceError::CLOSED);
    }

    if (state_->source->owner != std::this_thread::get_id()) {
      return std::unexpected(SourceError::WRONG_THREAD);
    }

    return WindowPin<T>(state_->current);
  }

  /** @return Current outcome on UI, or CLOSED/WRONG_THREAD. */
  std::expected<WindowStatus, SourceError> status() const {
    if (!state_) {
      return std::unexpected(SourceError::CLOSED);
    }

    if (state_->source->owner != std::this_thread::get_id()) {
      return std::unexpected(SourceError::WRONG_THREAD);
    }

    return state_->status;
  }

  /** @return Sticky last protocol/admission failure on UI, or CLOSED/WRONG_THREAD. */
  std::expected<std::optional<SourceError>, SourceError> lastError() const {
    auto result = status();

    if (!result) {
      return std::unexpected(result.error());
    }

    return state_->error;
  }

  /** @return Explicit unavailable-history recovery, never an automatically substituted cursor. */
  std::expected<std::optional<SourceCursor>, SourceError> recovery() const {
    auto result = status();

    if (!result) {
      return std::unexpected(result.error());
    }

    return state_->recovery;
  }

private:
  template <class> friend class WindowSource;

  explicit WindowConsumer(std::shared_ptr<detail::WindowConsumerState<T>> state) : state_(std::move(state)) {
  }

  std::shared_ptr<detail::WindowConsumerState<T>> state_;
};

/** @brief Copyable worker endpoint; retains only worker-safe state, never a UI source owner.
 * @tparam T Immutable source row value; source closure rejects late notifications.
 */
template <class T> class WindowNotifier {
public:
  WindowNotifier() = default; ///< Empty closed endpoint.

  /** @param source Exact immutable source stamp; a different namespace is rejected.
   * @param revision New positive readiness/data revision, applied later on UI.
   * @return Existing coalesced queue admission, not proof of UI application.
   */
  std::expected<PublishResult, SourceError> notifyReady(SourceStamp source, std::uint64_t revision) const {
    auto state = state_;
    auto publisher = publisher_;

    if (!state) {
      return std::unexpected(SourceError::CLOSED);
    }

    if (source != state->stamp) {
      return std::unexpected(SourceError::STALE_SOURCE);
    }

    if (!revision) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (!state || !state->alive->load() || state->marker.expired()) {
      return std::unexpected(SourceError::CLOSED);
    }

    // Serial admission prevents an older producer from replacing a newer queued revision.
    // Only successfully admitted notifications advance this producer-side watermark.
    std::lock_guard lock(state->notificationMutex);

    if (state->publishedRevision == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(SourceError::IDENTITY_EXHAUSTED);
    }

    if (revision <= state->publishedRevision) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    auto credit = state->budget->reserve({SourceArena::METADATA_BYTES, 0, 0, 0});

    if (!credit) {
      return std::unexpected(credit.error());
    }

    try {
      auto event = std::make_shared<const detail::WindowNotification>(std::move(*credit), source, revision);
      auto queued = publisher.publish(std::move(event));

      if (!queued) {
        return std::unexpected(queued.error() == SchedulerError::WOULD_BLOCK ? SourceError::WOULD_BLOCK
                                                                             : SourceError::CLOSED);
      }

      state->publishedRevision = revision;

      return *queued;
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    }
  }

private:
  template <class> friend class WindowSource;

  WindowNotifier(std::shared_ptr<detail::WindowState<T>> state, Publisher<detail::WindowNotification> publisher)
      : state_(std::move(state)), publisher_(std::move(publisher)) {
  }

  std::shared_ptr<detail::WindowState<T>> state_;
  Publisher<detail::WindowNotification> publisher_;
};

/** @brief Tree-scoped source using only existing scheduler queues; no total/origin/runtime is required.
 * @tparam T Immutable row value with worker-safe final destruction and accounted dynamic page payload.
 */
template <class T> class WindowSource {
public:
  /** @param tree UI tree; node scope retirement is checked before every later raw-tree access.
   * @param context Node controlling delivery lifetime and frame admission.
   * @param provider Worker-safe provider; external provider/domain storage is outside page accounting.
   * @param options Positive page/work limits.
   * @param budget Shared root; pass the old root for replacement sessions, null creates a default root.
   * @return New immutable session, or typed setup/allocation/capability failure.
   */
  static std::expected<std::unique_ptr<WindowSource>, SourceError> create(WidgetTree &tree, Node<Widget> context,
                                                                          std::shared_ptr<WindowProvider<T>> provider,
                                                                          WindowSourceOptions options = {},
                                                                          std::shared_ptr<SourceBudget> budget = {}) {
    if (!provider || !options.initialRevision || options.pageBytes <= SourceArena::METADATA_BYTES ||
        !options.maxPageRows || !options.maxRowBytes || options.workspaceBytes <= SourceArena::METADATA_BYTES ||
        options.envelopeBytes < SourceArena::METADATA_BYTES) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (sizeof(WindowRow<T>) > options.maxRowBytes ||
        sizeof(WindowRow<T>) > options.pageBytes - SourceArena::METADATA_BYTES) {
      return std::unexpected(SourceError::ROW_TOO_LARGE);
    }

    auto checked = tree.withScope(context, [](const UiScope &) {
    });

    if (!checked) {
      return std::unexpected(detail::sourceTreeError(checked.error()));
    }

    if (!budget) {
      auto created = SourceBudget::create();

      if (!created) {
        return std::unexpected(created.error());
      }

      budget = std::move(*created);
    }

    auto stamp = detail::windowSourceStamp();

    if (!stamp) {
      return std::unexpected(stamp.error());
    }

    auto count = budget->options().maxConsumers;

    if (count > (std::numeric_limits<std::size_t>::max() - SourceArena::METADATA_BYTES) /
                  sizeof(std::weak_ptr<detail::WindowConsumerState<T>>)) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    auto credit = budget->reserve(
      {SourceArena::METADATA_BYTES + count * sizeof(std::weak_ptr<detail::WindowConsumerState<T>>), 0, 0, 0});

    if (!credit) {
      return std::unexpected(credit.error());
    }

    try {
      auto state = std::make_shared<detail::WindowState<T>>(tree, context, count);
      state->credit = std::move(*credit);
      state->budget = budget;
      state->stamp = *stamp;
      state->options = options;
      state->revision = options.initialRevision;
      state->publishedRevision = options.initialRevision;
      state->provider = std::move(provider);
      state->capabilities = state->provider->capabilities();
      state->alive = std::make_shared<std::atomic<bool>>(true);

      auto marker = std::make_shared<detail::WindowScopeMarker>();
      state->marker = marker;
      auto complete = tree.template connect<detail::WindowCompletion<T>>(
        context,
        [weak = std::weak_ptr(state), marker](Widget &, std::shared_ptr<const detail::WindowCompletion<T>> event) {
          if (auto source = weak.lock()) {
            detail::applyWindowCompletion(source, *event);
          }
        });

      if (!complete) {
        return std::unexpected(detail::sourceTreeError(complete.error()));
      }

      state->publisher = complete->publisher;
      auto notifications = tree.template connect<detail::WindowNotification>(
        context,
        [weak = std::weak_ptr(state), marker](Widget &, std::shared_ptr<const detail::WindowNotification> event) {
          auto source = weak.lock();

          if (!source || !source->allowed() || event->source != source->stamp || event->revision <= source->revision) {
            return;
          }

          source->revision = event->revision;

          for (const auto &slot : source->slots) {
            auto consumer = slot.lock();

            if (!consumer || consumer->closed) {
              continue;
            }

            consumer->desiredRevision = event->revision;
            consumer->retryNeeded = true;

            if (consumer->active.expired()) {
              detail::restartWindow(consumer);
            }
          }

          if (source->allowed()) {
            (void)source->tree.invalidate(source->target, Invalidation::LAYOUT);
          }
        },
        SchedulerLane::DATA, SchedulerOverflow::REPLACE_LATEST);

      if (!notifications) {
        return std::unexpected(detail::sourceTreeError(notifications.error()));
      }

      return std::unique_ptr<WindowSource>(
        new WindowSource(std::move(state), std::move(*complete), std::move(*notifications)));
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    }
  }

  ~WindowSource() {
    close();
  } ///< Close on UI without joining workers.

  WindowSource(const WindowSource &) = delete;
  WindowSource &operator=(const WindowSource &) = delete;

  /** @return Immutable session identity, safe to copy on UI. */
  SourceStamp stamp() const noexcept {
    return state_->stamp;
  }

  /** @return Latest applied readiness/data revision, WRONG_THREAD, or CLOSED.
   * Reading the scalar on UI performs no provider work or I/O. Queued notifications
   * become visible only after the existing scheduler applies them.
   */
  std::expected<std::uint64_t, SourceError> dataRevision() const {
    auto allowed = state_->allowed();

    return allowed ? std::expected<std::uint64_t, SourceError>(state_->revision) : std::unexpected(allowed.error());
  }

  /** @return Shared credit root for related sources/replacements. */
  std::shared_ptr<SourceBudget> budget() const noexcept {
    return state_->budget;
  }

  /** @return Captured immutable capabilities. */
  SourceCapabilities capabilities() const noexcept {
    return state_->capabilities;
  }

  /** @return Independent UI consumer or a capacity/context error. */
  std::expected<WindowConsumer<T>, SourceError> openConsumer() {
    auto state = state_;
    auto allowed = state->allowed();

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto slot = std::ranges::find_if(state->slots, [](const auto &item) {
      return item.expired();
    });

    if (slot == state->slots.end()) {
      return std::unexpected(SourceError::WOULD_BLOCK);
    }

    auto credit =
      state->budget->reserve({sizeof(detail::WindowConsumerState<T>) + SourceArena::METADATA_BYTES, 0, 0, 1});

    if (!credit) {
      return std::unexpected(credit.error());
    }

    try {
      auto consumer = std::make_shared<detail::WindowConsumerState<T>>();
      consumer->credit = std::move(*credit);
      consumer->source = state;
      consumer->desiredRevision = state->revision;
      auto checked = state->allowed();

      if (!checked) {
        return std::unexpected(checked.error());
      }

      slot = std::ranges::find_if(state->slots, [](const auto &item) {
        return item.expired();
      });

      if (slot == state->slots.end()) {
        return std::unexpected(SourceError::WOULD_BLOCK);
      }

      *slot = consumer;

      return WindowConsumer<T>(std::move(consumer));
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    }
  }

  /** @return Worker-safe readiness endpoint; obtain on UI before dispatch, safe after source destruction. */
  WindowNotifier<T> notifier() const noexcept {
    return WindowNotifier<T>(state_, notifications_.publisher);
  }

  /** @param source Exact immutable session stamp.
   * @param revision Strictly newer readiness/data revision.
   * @return Queue admission only; use notifier() when a worker may outlive this object.
   */
  std::expected<PublishResult, SourceError> notifyReady(SourceStamp source, std::uint64_t revision) const {
    return notifier().notifyReady(source, revision);
  }

  /** @brief Retire admission/delivery on UI; old immutable owners remain charged until final release. */
  void close() noexcept {
    auto state = state_;

    if (state->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    if (!state->alive->exchange(false)) {
      return;
    }

    auto completion = std::move(completion_);
    auto notifications = std::move(notifications_);
    (void)completion.subscription.disconnect();
    (void)notifications.subscription.disconnect();

    for (const auto &slot : state->slots) {
      if (auto consumer = slot.lock()) {
        consumer->retryNeeded = false;

        if (auto work = consumer->active.lock()) {
          work->stop.request_stop();
        }

        consumer->pending.reset();
      }
    }
  }

private:
  WindowSource(std::shared_ptr<detail::WindowState<T>> state,
               SchedulerConnection<detail::WindowCompletion<T>> completion,
               SchedulerConnection<detail::WindowNotification> notifications)
      : state_(std::move(state)), completion_(std::move(completion)), notifications_(std::move(notifications)) {
  }

  std::shared_ptr<detail::WindowState<T>> state_;
  SchedulerConnection<detail::WindowCompletion<T>> completion_;
  SchedulerConnection<detail::WindowNotification> notifications_;
};
} // namespace portals
