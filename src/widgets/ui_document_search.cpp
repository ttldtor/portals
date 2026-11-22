// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_find_binding.hpp"
#include "document_view_state.hpp"
#include "find_bar_state.hpp"
#include <portals/detail/ui_recipe_access.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>
#include <limits>

namespace portals::ui::detail {
namespace {
using portals::detail::FindAccess;
using portals::detail::FindDocument;
using portals::detail::FindRecord;
using portals::detail::FindResult;

bool sameDecoder(const DocumentDecoderOptions &left, const DocumentDecoderOptions &right) noexcept {
  return left.explicitEncoding == right.explicitEncoding && left.hint == right.hint && left.policy == right.policy &&
         left.workLimit == right.workLimit;
}

bool sameBinding(const DocumentFindSnapshot &left, const DocumentFindSnapshot &right) noexcept {
  return left.source == right.source && sameDecoder(left.decoder, right.decoder);
}

ManagedError searchError(NodeId node, TreeError cause) {
  auto result = ManagedError{ManagedPhase::PREPARE, node, cause, {}, {}};
  result.operation = "ui::findBar/document";

  return result;
}

DocumentError withSearchOverlap(ByteDocumentSource &source, DocumentError error,
                                std::initializer_list<SourceBudgetUsage> owners) noexcept {
  if (!error.budgetDemand && !error.budgetPeak) {
    return error;
  }

  auto peak = error.budgetPeak ? *error.budgetPeak : *error.budgetDemand;
  constexpr auto MAX = std::numeric_limits<std::size_t>::max();

  for (const auto &owned : owners) {
    if (owned.bytes > MAX - peak.bytes || owned.pages > MAX - peak.pages || owned.work > MAX - peak.work ||
        owned.consumers > MAX - peak.consumers) {
      error.code = SourceError::PAGE_TOO_LARGE;

      return error;
    }

    peak.bytes += owned.bytes;
    peak.pages += owned.pages;
    peak.work += owned.work;
    peak.consumers += owned.consumers;
  }

  error.budgetPeak = peak;

  const auto cap = source.budget()->options();
  const auto pinned = source.pinnedBudgetBytes();
  const auto base = source.envelopeBytes() + SourceBudget::ROOT_METADATA_BYTES;

  if (peak.bytes > cap.maxBytes || pinned > cap.maxBytes - peak.bytes || base > cap.maxBytes - peak.bytes - pinned ||
      peak.pages > cap.maxPages || peak.work > cap.maxWork || peak.consumers >= cap.maxConsumers) {
    error.code = SourceError::PAGE_TOO_LARGE;
  }

  return error;
}

struct BudgetedDocument {
  SourceCredit credit;
  FindDocument document;
};

struct PreparedResult {
  std::shared_ptr<const FindResult> result;
  SourceBudgetUsage usage;
};

struct SearchRequest {
  DocumentFindSnapshot target;
  DocumentSearchOptions options;
  WidthPolicy policy;
  std::string query = std::string(0, '\0');
  std::shared_ptr<DocumentSearchSession> session;
  DocumentSearchDirection direction{DocumentSearchDirection::NEXT};
  std::uint64_t anchor{};
  std::uint64_t generation{};
  bool restart{true};
  std::shared_ptr<const DocumentSearchMatch> ready;
};

struct SearchCompletion {
  std::shared_ptr<const SearchRequest> input;
  std::shared_ptr<DocumentSearchSession> session;
  std::expected<DocumentSearchStep, DocumentSearchFailure> step{
    std::unexpected(DocumentSearchFailure{DocumentError{SourceError::CANCELLED, {}, {}}, {}})};
  std::shared_ptr<const FindResult> result;
  SourceBudgetUsage resultUsage;
  DocumentFindLocation location;
  std::shared_ptr<const DocumentSearchMatch> ready;
};

std::expected<PreparedResult, DocumentSearchFailure>
prepareResult(const SearchRequest &input, const DocumentSearchMatch &match, SourceBudgetUsage retained) {
  const auto bytes = match.snippet.empty() ? std::string_view(match.text) : std::string_view(match.snippet);
  constexpr std::size_t METADATA_BYTES = 2048;
  constexpr std::size_t BYTES_PER_SOURCE_BYTE = 96;
  constexpr auto MAX = std::numeric_limits<std::size_t>::max();

  if (bytes.size() > (MAX - METADATA_BYTES - input.query.size()) / BYTES_PER_SOURCE_BYTE) {
    return std::unexpected(DocumentSearchFailure{DocumentError{SourceError::PAGE_TOO_LARGE, {}, {}}, {}});
  }

  const auto amount = METADATA_BYTES + input.query.size() + bytes.size() * BYTES_PER_SOURCE_BYTE;
  auto reserved = input.target.source->budget()->reserve({amount, 0, 0, 0});

  if (!reserved) {
    auto error = DocumentError{reserved.error(), {}, {}};
    error.budgetDemand = SourceBudgetUsage{amount, 0, 0, 0};
    error = withSearchOverlap(*input.target.source, std::move(error), {retained, {match.retainedBytes(), 0, 0, 0}});

    return std::unexpected(DocumentSearchFailure{error, {}});
  }

  auto owner = std::make_shared<BudgetedDocument>();

  owner->credit = std::move(*reserved);

  const auto limit = std::max<std::size_t>(1, bytes.size());
  auto parsed = GraphemeText::from(bytes, input.policy, {limit, limit, limit, limit * 2});

  if (!parsed) {
    return std::unexpected(DocumentSearchFailure{DocumentError{SourceError::INVALID_ARGUMENT, {}, {}}, {}});
  }

  owner->document.text = std::make_unique<GraphemeText>(std::move(*parsed));
  owner->document.boundaries.resize(bytes.size() + 1);
  owner->document.boundaries[0] = 1;

  for (const auto &cluster : owner->document.text->graphemes()) {
    owner->document.boundaries[cluster.byte_end] = 1;
  }

  auto result = std::make_shared<FindResult>();

  result->document = std::shared_ptr<const FindDocument>(owner, &owner->document);
  result->query = input.query;
  result->generation = input.generation;
  result->session = 1;
  result->byteMatch = std::make_unique<const portals::detail::FindByteMatch>(
    portals::detail::FindByteMatch{match.byteOffset, match.byteLength, match.revision.dataRevision});

  const auto begin = match.snippet.empty() ? 0 : match.snippetMatchOffset;
  const auto length = match.snippet.empty() ? match.text.size() : match.snippetMatchLength;
  result->ranges.push_back({begin, begin + length, {}, {}, 0});

  return PreparedResult{std::shared_ptr<const FindResult>(std::move(result)), {amount, 0, 0, 0}};
}

class SearchJob final : public BackgroundJob {
public:
  SearchJob(std::shared_ptr<const SearchRequest> input, Publisher<SearchCompletion> publisher)
      : output_(std::make_shared<SearchCompletion>()), publisher_(std::move(publisher)) {
    output_->input = std::move(input);
  }

  void run(std::stop_token stop) noexcept override {
    try {
      const auto &input = *output_->input;
      output_->session = input.session;

      if (!output_->session) {
        auto created =
          DocumentSearchSession::create(input.target.source, input.query, input.target.decoder, input.options);

        if (!created) {
          output_->step = std::unexpected(created.error());

          return;
        }

        output_->session = std::move(*created);
      }

      if (input.restart) {
        auto started = output_->session->start(input.direction, input.anchor);

        if (!started) {
          output_->step = std::unexpected(started.error());

          return;
        }
      }

      if (input.ready) {
        output_->ready = input.ready;
        auto metadata = input.target.source->inspect(stop);

        if (!metadata || metadata->revision != input.ready->revision) {
          output_->step = std::unexpected(DocumentSearchFailure{
            metadata ? DocumentError{SourceError::REVISION_CHANGED, {}, {}}
                     : withSearchOverlap(*input.target.source, metadata.error(),
                                         {output_->session->retainedUsage(), {input.ready->retainedBytes(), 0, 0, 0}}),
            {}});

          return;
        }

        output_->step = DocumentSearchStep{DocumentSearchState::FOUND, input.ready->revision, input.ready->byteOffset,
                                           metadata->byteExtent, input.ready};
      } else {
        output_->step = output_->session->advance(stop);
      }

      if (!output_->step || output_->step->state != DocumentSearchState::FOUND) {
        return;
      }

      output_->ready = output_->step->match;
      auto result = prepareResult(input, *output_->ready, output_->session->retainedUsage());

      if (!result) {
        output_->step = std::unexpected(result.error());

        return;
      }

      output_->result = std::move(result->result);
      output_->resultUsage = result->usage;
      output_->location.byteOffset = output_->step->match->byteOffset;

      if (input.target.locate) {
        auto located = input.target.locate(*output_->step->match, stop);

        if (!located) {
          auto error = withSearchOverlap(
            *input.target.source, located.error(),
            {output_->session->retainedUsage(), {output_->ready->retainedBytes(), 0, 0, 0}, output_->resultUsage});
          output_->step = std::unexpected(DocumentSearchFailure{std::move(error), {}});
          output_->result.reset();

          return;
        }

        output_->location = *located;
      }
    } catch (const std::bad_alloc &) {
      output_->step = std::unexpected(DocumentSearchFailure{DocumentError{SourceError::NO_MEMORY, {}, {}}, {}});
    } catch (...) {
      output_->step = std::unexpected(DocumentSearchFailure{DocumentError{SourceError::CALLBACK_FAILED, {}, {}}, {}});
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto published = publisher_.publish(output_);

    if (published) {
      return {};
    }

    return std::unexpected(published.error() == SchedulerError::WOULD_BLOCK ? AsyncError::WOULD_BLOCK
                           : published.error() == SchedulerError::NO_MEMORY ? AsyncError::NO_MEMORY
                                                                            : AsyncError::CLOSED);
  }

private:
  std::shared_ptr<SearchCompletion> output_;
  Publisher<SearchCompletion> publisher_;
};

class SearchActivity final : public SceneActivity, public std::enable_shared_from_this<SearchActivity> {
public:
  SearchActivity(ManagedScene &scene, std::shared_ptr<FindRecord> record, std::shared_ptr<DocumentFindTarget> target,
                 DocumentSearchOptions options)
      : scene_(scene), record_(record), target_(std::move(target)), options_(options) {
  }

  bool active() const noexcept override {
    const auto record = record_.lock();

    return !cancelled_ && record && record->active && !record->lifetime.expired();
  }

  void cancel() noexcept override {
    cancelled_ = true;

    if (latest_) {
      latest_->cancel();
    }

    (void)connection_.subscription.disconnect();
    (void)capacityConnection_.subscription.disconnect();
    capacity_.reset();
    completion_.reset();
    session_.reset();
  }

  std::expected<void, TreeError> connect() {
    const auto record = record_.lock();
    std::expected<SchedulerConnection<SearchCompletion>, SchedulerError> connected;
    auto scoped = record->tree->withScope(record->field, [&](const UiScope &scope) {
      connected = SceneAccess::application(scene_).scheduler().connect<SearchCompletion>(
        scope, [weak = weak_from_this()](std::shared_ptr<const SearchCompletion> completion) {
          if (auto live = weak.lock(); live && live->active() && completion->input->generation == live->generation_) {
            live->completion_ = std::move(completion);

            if (auto record = live->record_.lock()) {
              const auto invalidated = record->tree->invalidate(record->field, Invalidation::LAYOUT);

              if (!invalidated) {
                live->fail(invalidated.error());
              }
            }
          }
        });
    });

    if (!scoped) {
      return std::unexpected(scoped.error());
    }

    if (!connected) {
      return std::unexpected(connected.error() == SchedulerError::NO_MEMORY ? TreeError::NO_MEMORY
                                                                            : TreeError::INVALID_ARGUMENT);
    }

    connection_ = std::move(*connected);
    std::expected<SchedulerConnection<int>, SchedulerError> capacity;
    scoped = record->tree->withScope(record->field, [&](const UiScope &scope) {
      capacity = SceneAccess::application(scene_).scheduler().connect<int>(scope, [weak = weak_from_this()](auto) {
        if (auto live = weak.lock(); live && live->active()) {
          if (auto record = live->record_.lock()) {
            const auto invalidated = record->tree->invalidate(record->field, Invalidation::LAYOUT);

            if (!invalidated) {
              live->fail(invalidated.error());
            }
          }
        }
      });
    });

    if (!scoped || !capacity) {
      return std::unexpected(!scoped                                         ? scoped.error()
                             : capacity.error() == SchedulerError::NO_MEMORY ? TreeError::NO_MEMORY
                                                                             : TreeError::INVALID_ARGUMENT);
    }

    capacityConnection_ = std::move(*capacity);

    return {};
  }

  void request(std::shared_ptr<const FindResult> query) {
    if (!active()) {
      return;
    }

    if (!retireRequest()) {
      return;
    }

    requested_ = !query->query.empty();
    direction_ = DocumentSearchDirection::NEXT;
    anchor_ = 0;
    restart_ = true;
    wrapped_ = false;
    originNavigation_.reset();
    clearSelection_ = true;

    if (auto record = record_.lock()) {
      record->modelPending = requested_;
    }
  }

  std::expected<FindMatch, TreeError> navigate(FindDirection direction) {
    auto record = record_.lock();

    if (!active()) {
      return std::unexpected(TreeError::STALE_NODE);
    }

    if (record->modelPending || requested_) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    if (record->result->query.empty()) {
      return FindMatch{};
    }

    const auto invalidated = record->tree->invalidate(record->field, Invalidation::LAYOUT);

    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }

    const auto previous = direction_;
    direction_ = direction == FindDirection::NEXT ? DocumentSearchDirection::NEXT : DocumentSearchDirection::PREVIOUS;
    const auto match = FindAccess::match(record->result, record->index);
    const auto offset = match.sourceByteOffset();
    anchor_ = direction_ == DocumentSearchDirection::PREVIOUS
                ? offset.value_or(std::numeric_limits<std::uint64_t>::max())
              : offset ? *offset + 1
                       : 0;
    restart_ = !session_ || direction_ == DocumentSearchDirection::PREVIOUS || previous != direction_;
    wrapped_ = false;
    requested_ = true;
    record->modelPending = true;
    originNavigation_.reset();

    return FindMatch{};
  }

  bool current() const noexcept {
    auto record = record_.lock();

    if (!active() || record->modelPending || record->result->generation != generation_ || !snapshot_) {
      return false;
    }

    try {
      auto captured = target_->capture(scene_);

      return captured && *captured && sameBinding(*snapshot_, **captured) &&
             (!snapshot_->revision || !(**captured).revision || *snapshot_->revision == *(**captured).revision);
    } catch (...) {
      return false;
    }
  }

  void service() noexcept override {
    if (!active()) {
      cancel();

      return;
    }

    try {
      if (latest_) {
        auto serviced = latest_->service();

        if (!serviced) {
          fail(serviced.error() == BackgroundError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::CLOSED);
        }
      }
    } catch (const std::bad_alloc &) {
      fail(TreeError::NO_MEMORY);
    } catch (...) {
      fail(TreeError::CALLBACK_FAILED);
    }
  }

  void layoutPrepared(const LayoutSnapshot &) noexcept override {
    if (!active()) {
      return;
    }

    try {
      auto prepared = target_->prepareLayout(scene_);

      if (!prepared) {
        failManaged(prepared.error());
        cancel();
      }
    } catch (const std::bad_alloc &) {
      fail(TreeError::NO_MEMORY);
    } catch (...) {
      fail(TreeError::CALLBACK_FAILED);
    }
  }

  void prepared() noexcept override {
    if (!active()) {
      cancel();

      return;
    }

    try {
      prepare();
    } catch (const std::bad_alloc &) {
      fail(TreeError::NO_MEMORY);
    } catch (...) {
      fail(TreeError::CALLBACK_FAILED);
    }
  }

private:
  bool retireRequest() noexcept {
    if (latest_) {
      latest_->cancel();
    }

    completion_.reset();
    session_.reset();
    working_ = false;
    capacityWaiting_ = false;
    ready_.reset();

    if (capacity_) {
      capacity_->disarm();
    }

    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      fail(TreeError::IDENTITY_EXHAUSTED);
      cancel();

      return false;
    }

    ++generation_;

    return true;
  }

  void clear(const std::shared_ptr<FindRecord> &record, std::string_view notice) {
    auto empty = std::make_shared<FindResult>();
    empty->query = record->result->query;
    empty->generation = generation_;
    empty->session = 1;
    record->modelPending = false;
    auto published = publishFindResult(record, std::move(empty), notice, false);

    if (!published) {
      fail(published.error());
    }
  }

  void prepare() {
    auto captured = target_->capture(scene_);

    if (!captured) {
      fail(captured.error(), target_->failure());
      cancel();

      return;
    }

    if (!*captured) {
      return;
    }

    auto record = record_.lock();
    const bool changedRevision =
      snapshot_ && snapshot_->revision && (**captured).revision && *snapshot_->revision != *(**captured).revision;
    const bool changed = snapshot_ && (!sameBinding(*snapshot_, **captured) || changedRevision);

    if (changed) {
      if (!retireRequest()) {
        return;
      }

      requested_ = false;
      clear(record, "Source changed; choose Next to search");
      capacity_.reset();
    }

    snapshot_ = std::move(**captured);

    if (!capacity_) {
      auto observed = snapshot_->source->budget()->observeCapacity(capacityConnection_.publisher);

      if (!observed) {
        auto failure =
          searchError(record->field.id(),
                      observed.error() == SourceError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT);
        failure.source = observed.error();
        failManaged(std::move(failure));
        cancel();

        return;
      }

      capacity_.emplace(std::move(*observed));
    }

    if (capacityWaiting_) {
      if (!capacity_ || !capacity_->takeReady()) {
        return;
      }

      capacityWaiting_ = false;
    }

    if (changed || clearSelection_) {
      clearSelection_ = false;
      auto cleared = target_->select(scene_, *snapshot_, {}, false);

      if (!cleared) {
        failManaged(cleared.error());
      }
    }

    if (completion_ && !acceptCompletion(record)) {
      return;
    }

    if (requested_ && !working_ && !record->result->query.empty()) {
      start(record);
    }
  }

  bool acceptCompletion(const std::shared_ptr<FindRecord> &record) {
    auto completion = std::exchange(completion_, {});
    working_ = false;

    if (!sameBinding(*snapshot_, completion->input->target)) {
      return false;
    }

    session_ = completion->session;

    if (!completion->step) {
      if (completion->step.error().error.code == SourceError::WOULD_BLOCK) {
        waitForCapacity(record, completion);

        return false;
      }

      requested_ = false;
      record->modelPending = false;
      session_.reset();
      auto error = searchError(record->field.id(), TreeError::INVALID_ARGUMENT);
      error.source = completion->step.error().error.code;
      error.logFile = completion->step.error().error.fileFailure;

      if (error.source != SourceError::NO_MEMORY) {
        try {
          error.document = std::make_shared<const DocumentOperationFailure>(completion->step.error());
        } catch (const std::bad_alloc &) {
          // Context allocation must not replace the original source/decode cause.
        }
      }

      clear(record, "Search failed");
      auto cleared = target_->select(scene_, *snapshot_, {}, false);

      if (!cleared) {
        failManaged(cleared.error());
      }

      failManaged(std::move(error));

      return false;
    }

    if (snapshot_->revision && snapshot_->revision->dataRevision &&
        *snapshot_->revision != completion->step->revision) {
      if (!retireRequest()) {
        return false;
      }

      requested_ = false;
      clear(record, "Source changed; choose Next to search");

      return false;
    }

    if (completion->step->state == DocumentSearchState::PENDING) {
      requested_ = true;
      restart_ = false;
    } else if (completion->step->state == DocumentSearchState::END && !wrapped_) {
      requested_ = true;
      wrapped_ = true;
      restart_ = true;
      anchor_ = direction_ == DocumentSearchDirection::NEXT ? 0 : std::numeric_limits<std::uint64_t>::max();
    } else if (completion->step->state == DocumentSearchState::END) {
      requested_ = false;
      record->modelPending = false;
      clear(record, "No matches");
      auto cleared = target_->select(scene_, *snapshot_, {}, false);

      if (!cleared) {
        failManaged(cleared.error());
      }
    } else {
      requested_ = false;
      record->modelPending = false;
      const auto &match = *completion->step->match;
      const auto selected =
        DocumentFindSelection{{match.revision, match.byteOffset, match.byteOffset + match.byteLength},
                              completion->location.byteOffset,
                              completion->location.fileGeneration};
      const bool reveal = snapshot_->navigation == completion->input->target.navigation;
      auto applied = target_->select(scene_, completion->input->target, selected, reveal);

      if (!applied) {
        failManaged(applied.error());

        return false;
      }

      const auto published = publishFindResult(record, std::move(completion->result), "Found", true);

      if (!published) {
        fail(published.error());
      }

      return false;
    }

    return true;
  }

  void start(const std::shared_ptr<FindRecord> &record) {
    auto executor = SceneAccess::application(scene_).background();

    if (!executor) {
      fail(TreeError::INVALID_ARGUMENT);

      return;
    }

    if (!latest_) {
      latest_ = std::make_unique<BackgroundLatest>(*executor);
    }

    auto input = std::make_shared<SearchRequest>();

    if (!originNavigation_) {
      originNavigation_ = snapshot_->navigation;
    }

    input->target = *snapshot_;
    input->target.navigation = *originNavigation_;
    input->options = options_;
    input->policy = record->policy;
    input->query = record->result->query;
    input->generation = generation_;
    input->session = session_;
    input->direction = direction_;
    input->anchor = anchor_;
    input->restart = restart_;
    input->ready = ready_;

    auto admitted = latest_->replace(std::make_unique<SearchJob>(std::move(input), connection_.publisher));

    if (!admitted) {
      fail(admitted.error() == BackgroundError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::CLOSED);

      return;
    }

    requested_ = false;
    ready_.reset();
    working_ = true;
    record->modelPending = true;
  }

  void waitForCapacity(const std::shared_ptr<FindRecord> &record,
                       const std::shared_ptr<const SearchCompletion> &completion) {
    const auto &error = completion->step.error().error;
    requested_ = false;
    working_ = false;
    ready_ = completion->ready;
    restart_ = !session_;

    if (!error.budgetDemand) {
      record->modelPending = false;
      clear(record, "Waiting for source; choose Next to retry");

      return;
    }

    if (!capacity_) {
      auto observed = snapshot_->source->budget()->observeCapacity(capacityConnection_.publisher);

      if (!observed) {
        fail(observed.error() == SourceError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT);

        return;
      }

      capacity_.emplace(std::move(*observed));
    }

    // Drop this bar's old prepared preview before computing mandatory overlap.
    // Snapshots retained by clients remain explicit independent budget pins.
    clear(record, "Waiting for memory");
    auto peak = error.budgetPeak.value_or(*error.budgetDemand);
    const auto source = snapshot_->source;
    const auto fixed = SourceBudget::ROOT_METADATA_BYTES + SourceBudget::CAPACITY_OBSERVER_BYTES;
    const auto pinned = source->pinnedBudgetBytes();
    const auto envelope = source->envelopeBytes();
    constexpr auto MAX = std::numeric_limits<std::size_t>::max();

    if (peak.bytes > MAX - fixed || peak.bytes + fixed > MAX - pinned || peak.bytes + fixed + pinned > MAX - envelope) {
      fail(TreeError::INVALID_ARGUMENT);

      return;
    }

    peak.bytes += fixed + pinned + envelope;
    auto armed = capacity_->arm(peak);

    if (!armed) {
      auto failure = searchError(record->field.id(), TreeError::INVALID_ARGUMENT);
      failure.source = armed.error();
      failManaged(std::move(failure));

      return;
    }

    capacityWaiting_ = true;
    requested_ = true;
    record->modelPending = true;
  }

  void fail(TreeError error, std::optional<DocumentOperationFailure> detail = {}) noexcept {
    auto record = record_.lock();

    if (record && active()) {
      record->modelPending = false;
      requested_ = false;
      working_ = false;
      capacityWaiting_ = false;
      completion_.reset();
      session_.reset();
      ready_.reset();

      if (latest_) {
        latest_->cancel();
      }

      if (capacity_) {
        capacity_->disarm();
      }

      if (generation_ != std::numeric_limits<std::uint64_t>::max()) {
        ++generation_;
      }

      auto failure = searchError(record->field.id(), error);

      if (detail) {
        failure.source = detail->error.code;
        failure.logFile = detail->error.fileFailure;

        if (detail->error.code != SourceError::NO_MEMORY) {
          try {
            failure.document = std::make_shared<const DocumentOperationFailure>(*detail);
          } catch (const std::bad_alloc &) {
            // Optional context never replaces the original scalar cause.
          }
        }
      }

      failManaged(std::move(failure));
    }
  }

  void failManaged(ManagedError error) noexcept {
    if (active()) {
      (void)scene_.reportError(std::move(error));
    }
  }

  ManagedScene &scene_;
  std::weak_ptr<FindRecord> record_;
  std::shared_ptr<DocumentFindTarget> target_;
  DocumentSearchOptions options_;
  std::optional<DocumentFindSnapshot> snapshot_;
  std::shared_ptr<DocumentSearchSession> session_;
  std::shared_ptr<const SearchCompletion> completion_;
  SchedulerConnection<SearchCompletion> connection_;
  SchedulerConnection<int> capacityConnection_;
  std::optional<SourceCapacityObserver> capacity_;
  std::shared_ptr<const DocumentSearchMatch> ready_;
  std::unique_ptr<BackgroundLatest> latest_;
  std::optional<std::uint64_t> originNavigation_;
  std::uint64_t generation_{};
  DocumentSearchDirection direction_{DocumentSearchDirection::NEXT};
  std::uint64_t anchor_{};
  bool restart_{true};
  bool requested_{};
  bool working_{};
  bool wrapped_{};
  bool cancelled_{};
  bool clearSelection_{};
  bool capacityWaiting_{};
};

struct Binding final : portals::detail::FindExternalBinding {
  std::shared_ptr<DocumentFindTarget> target;
  DocumentSearchOptions options;

  std::expected<std::shared_ptr<SceneActivity>, ManagedError>
  attach(ManagedScene &scene, const std::shared_ptr<FindRecord> &record) override {
    if (!target || !options.maxQueryBytes || options.maxQueryBytes > FindLimits::MAX_QUERY_BYTES ||
        !options.workBytes || !options.maxSnippetBytes) {
      return std::unexpected(searchError(record->field.id(), TreeError::INVALID_ARGUMENT));
    }

    const auto required = SceneAccess::requireBackground(scene);

    if (!required) {
      return std::unexpected(required.error());
    }

    auto activity = std::make_shared<SearchActivity>(scene, record, target, options);
    auto connected = activity->connect();

    if (!connected) {
      return std::unexpected(searchError(record->field.id(), connected.error()));
    }

    auto admitted = SceneAccess::admitActivity(scene, activity);

    if (!admitted) {
      return std::unexpected(admitted.error());
    }

    const auto weak = std::weak_ptr(activity);
    record->requestModel = [weak](std::shared_ptr<const FindResult> query) {
      if (auto live = weak.lock()) {
        live->request(std::move(query));
      }
    };
    record->modelCurrent = [weak] {
      const auto live = weak.lock();

      return live && live->current();
    };
    record->external = std::make_unique<portals::detail::FindExternalNavigation>();
    record->external->request = [weak](FindDirection direction) -> std::expected<FindMatch, TreeError> {
      const auto live = weak.lock();

      return live ? live->navigate(direction)
                  : std::expected<FindMatch, TreeError>(std::unexpected(TreeError::STALE_NODE));
    };
    activity->request(record->result);

    return std::shared_ptr<SceneActivity>(std::move(activity));
  }
};

class ViewerTarget final : public DocumentFindTarget {
public:
  explicit ViewerTarget(Ref<DocumentView> reference) : reference_(std::move(reference)) {
  }

  std::expected<std::optional<DocumentFindSnapshot>, TreeError> capture(ManagedScene &scene) override {
    const auto node = reference_.node();

    if (!node.id().session) {
      return std::optional<DocumentFindSnapshot>{};
    }

    DocumentFindSnapshot snapshot;
    auto read = SceneAccess::application(scene).tree().read(node, [&](const DocumentView &view) {
      const auto &state = portals::detail::DocumentViewAccess::state(view);
      snapshot.source = state.source;
      snapshot.decoder = state.config.decoder;
      snapshot.navigation = state.serial;

      if (state.status.revision.dataRevision) {
        snapshot.revision = state.status.revision;
      }
    });

    if (!read) {
      return std::unexpected(read.error());
    }

    return snapshot.source ? std::optional<DocumentFindSnapshot>(std::move(snapshot))
                           : std::optional<DocumentFindSnapshot>{};
  }

  std::expected<void, ManagedError> select(ManagedScene &scene, const DocumentFindSnapshot &captured,
                                           std::optional<DocumentFindSelection> selection, bool reveal) override {
    auto current = capture(scene);

    if (!current) {
      return std::unexpected(searchError(reference_.node().id(), current.error()));
    }

    if (!*current || !sameBinding(**current, captured)) {
      return {};
    }

    reveal = reveal && (**current).navigation == captured.navigation;

    return portals::detail::setDocumentSearchSelection(
      scene, reference_.node(), selection ? std::optional(selection->range) : std::nullopt, reveal);
  }

private:
  Ref<DocumentView> reference_;
};
} // namespace

Description<FindBar> documentFindBar(std::shared_ptr<DocumentFindTarget> target, std::string_view initial,
                                     FindBarConfig config, DocumentSearchOptions search) {
  auto binding = std::make_shared<Binding>();

  binding->target = std::move(target);
  binding->options = search;

  return Access::findBar({}, std::string(initial), std::move(config), {}, std::move(binding));
}
} // namespace portals::ui::detail

namespace portals::ui {
Description<FindBar> findBar(Ref<DocumentView> target, std::string_view initial, FindBarConfig config,
                             DocumentSearchOptions search) {
  return detail::documentFindBar(std::make_shared<detail::ViewerTarget>(std::move(target)), initial, std::move(config),
                                 search);
}

Description<FindBar> findBar(Ref<FindBar> reference, Ref<DocumentView> target, std::string_view initial,
                             FindBarConfig config, DocumentSearchOptions search) {
  return findBar(std::move(target), initial, std::move(config), search).reference(std::move(reference));
}
} // namespace portals::ui
