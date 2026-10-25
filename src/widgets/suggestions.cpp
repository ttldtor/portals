// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/background.hpp>
#include <portals/detail/search_index.hpp>
#include <portals/ui_scene.hpp>
#include <portals/ui_suggestions.hpp>

#include <atomic>

namespace portals::ui::detail {
namespace {
using portals::detail::SearchIndex;
std::atomic<std::uint64_t> nextSession{1};

std::uint64_t issueSession() noexcept {
  auto value = nextSession.load();

  while (value && value != std::numeric_limits<std::uint64_t>::max()) {
    if (nextSession.compare_exchange_weak(value, value + 1)) {
      return value;
    }
  }

  return 0;
}

ManagedError failure(Node<Widget> node, TreeError error) {
  return {ManagedPhase::STATE, node.id(), error, {}, {}};
}

SearchError backgroundFailure(BackgroundError error) noexcept {
  switch (error) {
  case BackgroundError::NO_MEMORY:
    return SearchError::NO_MEMORY;
  case BackgroundError::LIMIT_EXCEEDED:
    return SearchError::LIMIT_EXCEEDED;
  case BackgroundError::CALLBACK_FAILED:
    return SearchError::CALLBACK_FAILED;
  case BackgroundError::CLOSED:
    return SearchError::CANCELLED;
  case BackgroundError::UNAVAILABLE:
    return SearchError::UNAVAILABLE;
  default:
    return SearchError::INVALID_ARGUMENT;
  }
}

AsyncError publicationFailure(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::CLOSED:
    return AsyncError::CLOSED;
  case SchedulerError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::WRONG_THREAD:
    return AsyncError::WRONG_THREAD;
  case SchedulerError::IDENTITY_EXHAUSTED:
    return AsyncError::IDENTITY_EXHAUSTED;
  case SchedulerError::CALLBACK_FAILED:
    return AsyncError::CALLBACK_FAILED;
  default:
    return AsyncError::INVALID_ARGUMENT;
  }
}

struct PublicationState {
  // Only a terminal publication failure crosses this light channel. Zero is empty.
  std::atomic<int> failure{};

  std::optional<AsyncError> take() noexcept {
    const auto value = failure.exchange(0);
    return value ? std::optional(static_cast<AsyncError>(value - 1)) : std::nullopt;
  }
};

class SearchSnapshot final : public ModelSnapshot<SearchItem> {
public:
  SearchSnapshot(std::uint64_t revision, std::shared_ptr<const SearchResult> value)
      : ModelSnapshot(revision, value->rows.size()), result(std::move(value)) {
  }

  ModelRows<SearchItem> read(RowRange range) const override {
    return {RangeState::READY, std::span(result->rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    for (std::size_t position = 0; position < result->rows.size(); ++position) {
      if (result->rows[position].id == id) {
        return {RowLookupState::FOUND, position};
      }
    }

    return {RowLookupState::NOT_FOUND, 0};
  }

  std::shared_ptr<const SearchResult> result;
};

struct PendingTransportFailure {
  SearchSourceIdentity source;
  std::uint64_t generation{};
  AsyncError error;
};

struct Completion {
  SearchSourceIdentity source;
  std::uint64_t generation{}, storageEpoch{};
  bool build{}, blocked{}, rebuild{};
  std::optional<SearchError> error;
  std::optional<BackgroundError> background;
  std::shared_ptr<const SearchResult> result;
};

class SearchJob : public BackgroundJob {
public:
  SearchJob(BackgroundWorkerStorage &storage, WorkerStorageId slot, Publisher<Completion> publisher,
            SearchSourceIdentity source, std::shared_ptr<PublicationState> publication)
      : storage_(storage), slot_(slot), publisher_(std::move(publisher)), completion_(std::make_shared<Completion>()),
        publication_(std::move(publication)) {
    completion_->source = std::move(source);
  }

  std::expected<void, AsyncError> publish() override {
    auto result = publisher_.publish(completion_);

    if (result) {
      return {};
    }
    const auto error = publicationFailure(result.error());
    if (error != AsyncError::WOULD_BLOCK && error != AsyncError::NO_MEMORY) {
      publication_->failure.store(static_cast<int>(error) + 1);
      publisher_.wake();
    }
    return std::unexpected(error);
  }

protected:
  BackgroundWorkerStorage &storage_;
  WorkerStorageId slot_;
  Publisher<Completion> publisher_;
  std::shared_ptr<Completion> completion_;
  std::shared_ptr<PublicationState> publication_;
};

class BuildJob final : public SearchJob {
public:
  BuildJob(BackgroundWorkerStorage &storage, WorkerStorageId slot, Publisher<Completion> publisher,
           SearchSourceIdentity key, std::shared_ptr<const portals::detail::FindWorkerSource> source,
           SearchOptions options, std::shared_ptr<PublicationState> publication)
      : SearchJob(storage, slot, std::move(publisher), std::move(key), std::move(publication)),
        source_(std::move(source)), options_(options) {
    completion_->build = true;
  }

  void run(std::stop_token stop) noexcept override {
    try {
      bool cached{};
      auto read =
        storage_.withRead(slot_, [&](const WorkerStorageValue &value) -> std::expected<void, BackgroundError> {
          const auto *index = dynamic_cast<const SearchIndex *>(&value);
          cached = index && index->identity == completion_->source;
          return {};
        });

      if (read && cached) {
        return;
      }
      completion_->storageEpoch = storage_.changeEpoch();
      auto built = storage_.withBuild(slot_, options_.maxIndexBytes, options_.maxBuildScratchBytes,
                                      [&](WorkerStorageBuild &builder) -> std::expected<void, BackgroundError> {
                                        auto index = portals::detail::buildSearchIndex(*source_, completion_->source,
                                                                                       options_, stop);

                                        if (!index) {
                                          completion_->error = index.error();
                                          return std::unexpected(BackgroundError::CALLBACK_FAILED);
                                        }
                                        const auto bytes = (*index)->bytes();
                                        std::unique_ptr<WorkerStorageValue> value = std::move(*index);

                                        return builder.commit(value, bytes);
                                      });

      if (!built && !completion_->error) {
        if (built.error() == BackgroundError::WOULD_BLOCK) {
          completion_->blocked = true;
        } else {
          completion_->error = backgroundFailure(built.error());
          completion_->background = built.error();
        }
      }
    } catch (const std::bad_alloc &) {
      completion_->error = SearchError::NO_MEMORY;
    } catch (...) {
      completion_->error = SearchError::CALLBACK_FAILED;
    }
  }

private:
  std::shared_ptr<const portals::detail::FindWorkerSource> source_;
  SearchOptions options_;
};

class QueryJob final : public SearchJob {
public:
  QueryJob(BackgroundWorkerStorage &storage, WorkerStorageId slot, Publisher<Completion> publisher,
           SearchSourceIdentity source, std::string_view query, std::uint64_t generation,
           std::shared_ptr<PublicationState> publication)
      : SearchJob(storage, slot, std::move(publisher), std::move(source), std::move(publication)), query_(query) {
    completion_->generation = generation;
  }

  void run(std::stop_token stop) noexcept override {
    try {
      auto read =
        storage_.withRead(slot_, [&](const WorkerStorageValue &value) -> std::expected<void, BackgroundError> {
          const auto *index = dynamic_cast<const SearchIndex *>(&value);

          if (!index || !(index->identity == completion_->source)) {
            completion_->rebuild = true;
            return {};
          }
          auto result = portals::detail::querySearchIndex(*index, query_, completion_->generation, stop);

          if (result) {
            completion_->result = std::move(*result);
          } else {
            completion_->error = result.error();
          }
          return {};
        });
      // The read guard has ended before publication or a rejected-publication retry.
      if (!read) {
        if (read.error() == BackgroundError::WOULD_BLOCK) {
          completion_->rebuild = true;
        } else {
          completion_->error = backgroundFailure(read.error());
          completion_->background = read.error();
        }
      }
    } catch (const std::bad_alloc &) {
      completion_->error = SearchError::NO_MEMORY;
    } catch (...) {
      completion_->error = SearchError::CALLBACK_FAILED;
    }
  }

private:
  std::string query_;
};
} // namespace

class SuggestionsActivity final : public SceneActivity, public std::enable_shared_from_this<SuggestionsActivity> {
public:
  SuggestionsActivity(ManagedScene &scene, Node<VirtualList<SearchItem>> node, TextSource query,
                      std::shared_ptr<portals::detail::FindModelSource> binding, ManagedModel<SearchItem> model,
                      Publisher<ModelSnapshot<SearchItem>> publisher, SuggestionsConfig config, std::uint64_t session)
      : scene_(scene), node_(node), lifetime_(Access::lifetime(scene)), querySource_(std::move(query)),
        binding_(std::move(binding)), model_(std::move(model)), publisher_(std::move(publisher)),
        config_(std::move(config)), session_(session), owner_(std::this_thread::get_id()) {
  }

  ~SuggestionsActivity() override {
    cancel();
  }

  bool active() const noexcept override {
    return !cancelled_ && !lifetime_.expired() && bool(SceneAccess::application(scene_).tree().inspect(node_));
  }

  void cancel() noexcept override {
    cancelled_ = true;
    if (buildLatest_) {
      buildLatest_->cancel();
    }
    if (queryLatest_) {
      queryLatest_->cancel();
    }
    lease_.retire();
    (void)connection_.subscription.disconnect();
    buildCompletion_.reset();
    queryCompletion_.reset();
    buildPublication_.reset();
    queryPublication_.reset();
    pendingSnapshot_.reset();
    pendingTransport_.reset();
    ready_.reset();
  }

  std::expected<void, ManagedError> connect() {
    auto &app = SceneAccess::application(scene_);
    std::expected<SchedulerConnection<Completion>, SchedulerError> connected;
    auto scoped = app.tree().withScope(node_, [&](const UiScope &scope) {
      connected =
        app.scheduler().connect<Completion>(scope, [weak = weak_from_this()](std::shared_ptr<const Completion> value) {
          if (auto self = weak.lock(); self && self->active() && value->source == self->key_) {
            if (value->build) {
              self->buildCompletion_ = std::move(value);
            } else if (value->generation == self->generation_) {
              self->queryCompletion_ = std::move(value);
            } else {
              return;
            }
            auto invalidated =
              SceneAccess::application(self->scene_).tree().invalidate(self->node_, Invalidation::LAYOUT);

            if (!invalidated) {
              self->failTree(invalidated.error());
            }
          }
        });
    });

    if (!scoped || !connected) {
      return std::unexpected(failure(node_, !scoped ? scoped.error()
                                            : connected.error() == SchedulerError::NO_MEMORY
                                              ? TreeError::NO_MEMORY
                                              : TreeError::INVALID_ARGUMENT));
    }
    connection_ = std::move(*connected);

    return {};
  }

  void activate(RowId row) noexcept {
    if (ready_) {
      activation_ = std::pair(generation_, row);
    }
  }

  std::expected<bool, ManagedError> current(const SearchResult &result) {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(failure(node_, TreeError::WRONG_THREAD));
    }
    if (!active()) {
      return std::unexpected(failure(node_, TreeError::CLOSED));
    }
    std::string query = query_;
    bool oversized = oversized_;
    auto changed = Access::readSearchText(scene_, querySource_, config_.search.maxQueryBytes, query, oversized);

    if (!changed) {
      return std::unexpected(changed.error());
    }
    auto source = binding_->capture(scene_);

    if (!source) {
      return std::unexpected(failure(node_, source.error()));
    }

    return ready_.get() == &result && !*changed && !oversized && result.source == key_ &&
           result.generation == generation_ && result.query == query_ && source_ == *source;
  }

  void service() noexcept override {
    if (!active()) {
      cancel();
      return;
    }
    try {
      publishPending();
      const auto executor = SceneAccess::application(scene_).background();
      const bool searchable = !query_.empty() && !oversized_ && !(failedBuild_ && settled_ == generation_);
      const bool admitting = searchable && (building_ || (scheduled_ == generation_ && settled_ != generation_));
      const bool capacity = admitting && executor && executor->capacityEpoch() != capacityEpoch_;
      const bool storage = storage_ && ((registerBlocked_ && storage_->changeEpoch() != registerEpoch_) ||
                                        (buildBlocked_ && !building_ && storage_->changeEpoch() != buildEpoch_));
      const bool terminal =
        pendingTransport_.has_value() || (searchable && ((buildPublication_ && buildPublication_->failure.load()) ||
                                                         (queryPublication_ && queryPublication_->failure.load())));
      if ((capacity || (searchable && storage) || terminal) && !readinessRequested_) {
        readinessRequested_ = true;
        const auto invalidated = SceneAccess::application(scene_).tree().invalidate(node_, Invalidation::LAYOUT);
        if (!invalidated) {
          failTree(invalidated.error());
        }
      }
    } catch (const std::bad_alloc &) {
      failTree(TreeError::NO_MEMORY);
    } catch (...) {
      failTree(TreeError::CALLBACK_FAILED);
    }
  }

  void prepared() noexcept override {
    readinessRequested_ = false;
    if (!active()) {
      cancel();
      return;
    }
    try {
      auto source = binding_->capture(scene_);

      if (!source) {
        failTree(source.error());
        cancel();
        return;
      }
      const bool sourceChanged = source_ != *source;
      auto queryChanged =
        Access::readSearchText(scene_, querySource_, config_.search.maxQueryBytes, query_, oversized_);

      if (!queryChanged) {
        (void)scene_.reportError(queryChanged.error());
        cancel();
        return;
      }
      if (sourceChanged) {
        source_ = *source;
        key_ = {source_->pin, source_->pin.get(), source_->revision, session_, 1, scene_.widthPolicy()};
        if (buildLatest_) {
          buildLatest_->cancel();
        }
        buildCompletion_.reset();
        buildPublication_.reset();
        cached_ = false;
        building_ = false;
        buildBlocked_ = false;
      }
      if (sourceChanged || *queryChanged || !generation_) {
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
          failTree(TreeError::IDENTITY_EXHAUSTED);
          cancel();
          return;
        }
        ++generation_;
        if (queryLatest_) {
          queryLatest_->cancel();
        }
        queryCompletion_.reset();
        queryPublication_.reset();
        ready_.reset();
        pendingTransport_.reset();
        activation_.reset();
        scheduled_ = 0;
        settled_ = 0;
        failedBuild_ = false;
        takeTree(Access::suppressSearchList(scene_, node_, true));
        auto empty = std::make_shared<SearchResult>();
        empty->source = key_;
        empty->query = query_;
        empty->generation = generation_;
        queueResult(std::move(empty));
      }
      publishPending();
      if (!active()) {
        cancel();
        return;
      }
      if (pendingTransport_) {
        const auto transport = std::exchange(pendingTransport_, {});
        if (transport->source == key_ && transport->generation == generation_) {
          failTransport(transport->error);
          return;
        }
      }
      if (oversized_) {
        if (settled_ != generation_) {
          failDomain(SearchError::LIMIT_EXCEEDED);
        }
        return;
      }
      if (query_.empty()) {
        setStatus("Type to search");
        return;
      }
      auto executor = SceneAccess::application(scene_).background();

      if (!executor || !executor->workerStorage()) {
        if (settled_ != generation_) {
          failDomain(SearchError::UNAVAILABLE);
        }
        return;
      }
      storage_ = executor->workerStorage();
      capacityEpoch_ = executor->capacityEpoch();
      if (failedBuild_ && settled_ == generation_) {
        return;
      }

      if (!lease_.id().value && (!registerBlocked_ || storage_->changeEpoch() != registerEpoch_)) {
        registerEpoch_ = storage_->changeEpoch();
        auto lease = storage_->registerSlot();

        if (!lease) {
          if (lease.error() == BackgroundError::WOULD_BLOCK) {
            registerBlocked_ = true;
            setStatus("Waiting for search capacity");
          } else {
            failAdmission(lease.error());
          }
          return;
        }
        lease_ = std::move(*lease);
        registerBlocked_ = false;
        buildLatest_ = std::make_unique<BackgroundLatest>(*executor);
        queryLatest_ = std::make_unique<BackgroundLatest>(*executor);
      }
      if (!lease_.id().value) {
        return;
      }
      // Admission and terminal delivery are serviced only after the shared source
      // pin and current input have fenced/replaced the previous intent.
      if (!serviceLatest()) {
        return;
      }
      auto built = std::exchange(buildCompletion_, {});

      if (built && built->source == key_) {
        building_ = false;
        if (built->error) {
          failedBuild_ = true;
          if (built->background) {
            failAdmission(*built->background);
          } else {
            failDomain(*built->error);
          }
        } else if (built->blocked) {
          buildBlocked_ = true;
          buildEpoch_ = built->storageEpoch;
        } else {
          cached_ = true;
          buildBlocked_ = false;
        }
      }
      if (!active()) {
        cancel();
        return;
      }
      auto completion = std::exchange(queryCompletion_, {});

      if (completion && completion->source == key_ && completion->generation == generation_ &&
          settled_ != generation_) {
        if (completion->rebuild) {
          cached_ = false;
          scheduled_ = 0;
        } else if (completion->error) {
          if (completion->background) {
            failAdmission(*completion->background);
          } else {
            failDomain(*completion->error);
          }
        } else if (completion->result && completion->result->query == query_) {
          settled_ = generation_;
          queueResult(completion->result);
          setStatus(completion->result->rows.empty() ? "No matches"
                    : completion->result->hasMore    ? "Results (more available)"
                                                     : "Results ready");
        }
      }
      if (!active()) {
        cancel();
        return;
      }
      if (!cached_ && !building_ && !failedBuild_ && (!buildBlocked_ || storage_->changeEpoch() != buildEpoch_)) {
        building_ = true;
        buildBlocked_ = false;
        setStatus("Building search index");
        buildPublication_ = std::make_shared<PublicationState>();
        auto admitted = buildLatest_->replace(std::make_unique<BuildJob>(
          *storage_, lease_.id(), connection_.publisher, key_, source_, config_.search, buildPublication_));

        if (!admitted) {
          building_ = false;
          failedBuild_ = true;
          failAdmission(admitted.error());
        }
      }
      if (cached_ && scheduled_ != generation_ && settled_ != generation_) {
        scheduled_ = generation_;
        setStatus("Searching");
        queryPublication_ = std::make_shared<PublicationState>();
        auto admitted = queryLatest_->replace(std::make_unique<QueryJob>(*storage_, lease_.id(), connection_.publisher,
                                                                         key_, query_, generation_, queryPublication_));

        if (!admitted) {
          failAdmission(admitted.error());
        }
      }
      if (!active()) {
        cancel();
        return;
      }
      auto pin = portals::detail::ManagedModelAccess::snapshot(model_, scene_.root().id());

      if (pin) {
        auto installed = std::dynamic_pointer_cast<const SearchSnapshot>(*pin);

        if (installed && installed->result->source == key_ && installed->result->generation == generation_ &&
            installed->result->query == query_ && settled_ == generation_) {
          ready_ = installed->result;
          takeTree(Access::suppressSearchList(scene_, node_, false));
        }
      }
      const auto activation = std::exchange(activation_, {});

      if (activation && activation->first == generation_ && ready_ && config_.activated) {
        for (std::size_t position = 0; position < ready_->rows.size(); ++position) {
          if (ready_->rows[position].id != activation->second) {
            continue;
          }
          auto valid = current(*ready_);

          if (valid && *valid) {
            config_.activated(SearchSelection(ready_, position, weak_from_this(), owner_));
          } else if (!valid) {
            (void)scene_.reportError(valid.error());
          }
          break;
        }
      }
    } catch (const std::bad_alloc &) {
      failTree(TreeError::NO_MEMORY);
    } catch (...) {
      failTree(TreeError::CALLBACK_FAILED);
    }
  }

private:
  bool serviceLatest() {
    for (const auto &publication : {buildPublication_, queryPublication_}) {
      if (publication) {
        if (auto error = publication->take()) {
          failTransport(*error);
          return false;
        }
      }
    }
    for (auto latest : {buildLatest_.get(), queryLatest_.get()}) {
      if (latest) {
        auto admitted = latest->service();
        if (!admitted) {
          failAdmission(admitted.error());
          return false;
        }
      }
    }
    return active();
  }

  void retireFailedIntent() noexcept {
    if (buildLatest_) {
      buildLatest_->cancel();
    }
    if (queryLatest_) {
      queryLatest_->cancel();
    }
    buildPublication_.reset();
    queryPublication_.reset();
    buildCompletion_.reset();
    queryCompletion_.reset();
    pendingSnapshot_.reset();
    pendingTransport_.reset();
    ready_.reset();
    building_ = false;
    failedBuild_ = true;
    settled_ = generation_;
    (void)Access::suppressSearchList(scene_, node_, true);
  }

  void failAdmission(BackgroundError error) noexcept {
    retireFailedIntent();
    auto diagnostic = failure(node_, error == BackgroundError::NO_MEMORY         ? TreeError::NO_MEMORY
                                     : error == BackgroundError::CLOSED          ? TreeError::CLOSED
                                     : error == BackgroundError::WRONG_THREAD    ? TreeError::WRONG_THREAD
                                     : error == BackgroundError::CALLBACK_FAILED ? TreeError::CALLBACK_FAILED
                                                                                 : TreeError::INVALID_ARGUMENT);
    diagnostic.background = error;
    diagnostic.operation = "ui::suggestions/background";
    (void)scene_.reportError(std::move(diagnostic));
    try {
      if (active()) {
        setStatus("Search failed; query preserved");
      }
    } catch (...) {
    }
  }

  void failTransport(AsyncError error) noexcept {
    retireFailedIntent();
    auto diagnostic = failure(node_, error == AsyncError::NO_MEMORY            ? TreeError::NO_MEMORY
                                     : error == AsyncError::CLOSED             ? TreeError::CLOSED
                                     : error == AsyncError::WRONG_THREAD       ? TreeError::WRONG_THREAD
                                     : error == AsyncError::IDENTITY_EXHAUSTED ? TreeError::IDENTITY_EXHAUSTED
                                     : error == AsyncError::CALLBACK_FAILED    ? TreeError::CALLBACK_FAILED
                                                                               : TreeError::INVALID_ARGUMENT);
    diagnostic.async = error;
    diagnostic.operation = "ui::suggestions/publication";
    (void)scene_.reportError(std::move(diagnostic));
    try {
      if (active()) {
        setStatus("Search failed; query preserved");
      }
    } catch (...) {
    }
  }

  void takeTree(std::expected<void, ManagedError> result) {
    if (!result) {
      (void)scene_.reportError(result.error());
    }
  }

  void failTree(TreeError error) noexcept {
    if (active()) {
      (void)scene_.reportError(failure(node_, error));
    }
  }

  void setStatus(std::string_view value) {
    if (status_ == value) {
      return;
    }
    status_ = value;
    if (config_.status.node().id().generation) {
      config_.status.text() = value;
    }
  }

  void failDomain(SearchError error) {
    settled_ = generation_;
    ready_.reset();
    takeTree(Access::suppressSearchList(scene_, node_, true));
    setStatus("Search failed; query preserved");
    if (active() && config_.failed) {
      config_.failed(error);
    }
  }

  void queueResult(std::shared_ptr<const SearchResult> result) {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
      failTree(TreeError::IDENTITY_EXHAUSTED);
      return;
    }
    pendingSnapshot_ = std::make_shared<const SearchSnapshot>(++revision_, std::move(result));
    publishPending();
  }

  void publishPending() {
    if (!pendingSnapshot_) {
      return;
    }
    auto result = publisher_.publish(pendingSnapshot_);

    if (result) {
      pendingSnapshot_.reset();
    } else if (result.error() != SchedulerError::WOULD_BLOCK && result.error() != SchedulerError::NO_MEMORY) {
      pendingTransport_ = PendingTransportFailure{
        pendingSnapshot_->result->source, pendingSnapshot_->result->generation, publicationFailure(result.error())};
      pendingSnapshot_.reset();
    }
  }

  ManagedScene &scene_;
  Node<VirtualList<SearchItem>> node_;
  std::weak_ptr<const void> lifetime_;
  TextSource querySource_;
  std::shared_ptr<portals::detail::FindModelSource> binding_;
  ManagedModel<SearchItem> model_;
  Publisher<ModelSnapshot<SearchItem>> publisher_;
  SuggestionsConfig config_;
  std::shared_ptr<const portals::detail::FindWorkerSource> source_;
  SearchSourceIdentity key_;
  std::string query_ = std::string(0, '\0'), status_ = std::string(0, '\0');
  std::uint64_t session_{}, generation_{}, scheduled_{}, settled_{}, revision_{1};
  std::uint64_t registerEpoch_{}, buildEpoch_{}, capacityEpoch_{};
  std::thread::id owner_;
  BackgroundWorkerStorage *storage_{};
  WorkerStorageLease lease_;
  std::unique_ptr<BackgroundLatest> buildLatest_, queryLatest_;
  SchedulerConnection<Completion> connection_;
  std::shared_ptr<const Completion> buildCompletion_, queryCompletion_;
  std::shared_ptr<PublicationState> buildPublication_, queryPublication_;
  std::shared_ptr<const SearchSnapshot> pendingSnapshot_;
  std::optional<PendingTransportFailure> pendingTransport_;
  std::shared_ptr<const SearchResult> ready_;
  std::optional<std::pair<std::uint64_t, RowId>> activation_;
  bool oversized_{}, registerBlocked_{}, building_{}, cached_{}, buildBlocked_{}, failedBuild_{}, cancelled_{},
    readinessRequested_{};
};

std::expected<TextSource, ManagedError> Access::resolveSearchText(ManagedScene &scene, const TextSource &source,
                                                                  MountContext &context) {
  auto target = captionTarget(source.target_, context);
  auto owner = eventScene(target);

  if (!owner) {
    return std::unexpected(owner.error());
  }
  if (*owner != &scene) {
    return std::unexpected(failure(target->node, TreeError::INVALID_ARGUMENT));
  }

  return TextSource(std::move(target), source.kind_);
}

std::expected<bool, ManagedError> Access::readSearchText(ManagedScene &scene, const TextSource &source, std::size_t cap,
                                                         std::string &value, bool &oversized) {
  auto owner = eventScene(source.target_);

  if (!owner) {
    return std::unexpected(owner.error());
  }
  if (*owner != &scene) {
    return std::unexpected(failure(source.target_->node, TreeError::INVALID_ARGUMENT));
  }
  bool changed{}, nextOversized{};
  std::optional<std::string> next;
  const auto read = [&](std::string_view bytes) {
    nextOversized = bytes.size() > cap;
    changed = nextOversized != oversized || (!nextOversized && bytes != value);
    if (changed && !nextOversized) {
      next.emplace(bytes);
    }
  };

  if (source.kind_ == TextSource::Kind::COMMITTED) {
    auto entry = scene.find(source.target_->draft != Node<LineEdit>{} ? Node<Widget>(source.target_->draft)
                                                                      : source.target_->node);

    if (!entry || !entry->textModel) {
      return std::unexpected(failure(source.target_->node, TreeError::INVALID_ARGUMENT));
    }
    auto snapshot = entry->textModel->snapshot();

    if (!snapshot) {
      return std::unexpected(failure(source.target_->node, snapshot.error()));
    }
    read((*snapshot)->value);
  } else {
    const auto copy = [&](const auto &widget) {
      read(widget.text().bytes());
    };
    auto copied = source.target_->draft != Node<LineEdit>{} ? scene.app_.tree().read(source.target_->draft, copy)
                                                            : scene.app_.tree().read(source.target_->caption, copy);

    if (!copied) {
      return std::unexpected(failure(source.target_->node, copied.error()));
    }
  }
  if (next) {
    value.swap(*next);
  }
  if (nextOversized) {
    value.clear();
  }
  oversized = nextOversized;

  return changed;
}

std::expected<void, ManagedError> Access::configureSearchList(ManagedScene &scene, Node<VirtualList<SearchItem>> node,
                                                              std::shared_ptr<void> service,
                                                              std::function<void(RowId)> activate,
                                                              CellStyle highlight) {
  auto changed = scene.app_.tree().edit(node, [&](VirtualList<SearchItem> &view) {
    view.preparedDelegate_ = [](const ModelRow<SearchItem> &row) {
      return row.value.presentation;
    };
    view.activation_ = std::move(activate);
    view.service_ = std::move(service);
    view.options_.highlight = highlight;
    view.preparedSuppressed_ = true;
    view.clearMissingSelection_ = true;
  });

  return changed ? std::expected<void, ManagedError>{} : std::unexpected(failure(node, changed.error()));
}

std::expected<void, ManagedError> Access::suppressSearchList(ManagedScene &scene, Node<VirtualList<SearchItem>> node,
                                                             bool value) {
  bool changed{};
  auto edited = scene.app_.tree().edit(node, [&](VirtualList<SearchItem> &view) {
    changed = view.preparedSuppressed_ != value;
    view.preparedSuppressed_ = value;
    if (value) {
      view.selected_.reset();
    }
  });

  if (!edited) {
    return std::unexpected(failure(node, edited.error()));
  }
  if (changed) {
    auto invalidated = scene.app_.tree().invalidate(node, Invalidation::PAINT);
    if (!invalidated) {
      return std::unexpected(failure(node, invalidated.error()));
    }
  }

  return {};
}

Description<VirtualList<SearchItem>> Access::suggestions(TextSource query,
                                                         std::shared_ptr<portals::detail::FindModelSource> binding,
                                                         SuggestionsConfig config) {
  return configuredControl(
    cold<VirtualList<SearchItem>>([query = std::move(query), binding = std::move(binding), config = std::move(config)](
                                    ManagedScene &scene, Node<Widget> parent, MountContext &context) mutable
                                    -> std::expected<Node<VirtualList<SearchItem>>, ManagedError> {
      if (!binding || !portals::detail::validSearchOptions(config.search)) {
        return std::unexpected(failure(parent, TreeError::INVALID_ARGUMENT));
      }
      const auto modelCount = scene.models_.size();
      context.rollback.push_back([&scene, modelCount] {
        while (scene.models_.size() > modelCount) {
          scene.models_.back()->retire();
          scene.models_.pop_back();
        }
      });
      auto allowed = SceneAccess::requireBackground(scene);
      if (!allowed) {
        return std::unexpected(allowed.error());
      }
      auto attached = binding->attach(scene);
      if (!attached) {
        return std::unexpected(attached.error());
      }
      auto empty = std::make_shared<const SearchResult>();
      auto model = scene.model<SearchItem>(std::make_shared<const SearchSnapshot>(1, empty));
      if (!model) {
        return std::unexpected(model.error());
      }
      auto publisher = model->publisher();
      if (!publisher) {
        return std::unexpected(failure(parent, publisher.error()));
      }
      config.options = controlVirtualListOptions(*context.controlSettings, config.options);
      config.options.width_policy = scene.widthPolicy();
      auto node = scene.virtualList<SearchItem>(
        parent, *model,
        [](const ModelRow<SearchItem> &row) {
          return row.value.text();
        },
        config.options, config.policy);
      if (!node) {
        return node;
      }
      if (config.activated) {
        config.activated = currentSceneCallback(scene.facadeContext(), std::move(config.activated));
      }
      if (config.failed) {
        config.failed = currentSceneCallback(scene.facadeContext(), std::move(config.failed));
      }
      context.finalize.push_back([&scene, &context, node = *node, query = std::move(query),
                                  binding = std::move(binding), model = *model, publisher = std::move(*publisher),
                                  config = std::move(config)]() mutable -> std::expected<void, ManagedError> {
        auto resolved = resolveSearchText(scene, query, context);
        if (!resolved) {
          return std::unexpected(resolved.error());
        }
        const auto session = issueSession();
        if (!session) {
          return std::unexpected(failure(node, TreeError::IDENTITY_EXHAUSTED));
        }
        const auto highlight = config.options.highlight;
        auto activity = std::make_shared<SuggestionsActivity>(scene, node, std::move(*resolved), std::move(binding),
                                                              model, std::move(publisher), std::move(config), session);
        auto connected = activity->connect();
        if (!connected) {
          return connected;
        }
        auto configured = configureSearchList(
          scene, node, activity,
          [weak = std::weak_ptr(activity)](RowId row) {
            if (auto current = weak.lock()) {
              current->activate(row);
            }
          },
          highlight);
        if (!configured) {
          return configured;
        }

        return SceneAccess::admitActivity(scene, std::move(activity));
      });

      return node;
    }),
    "VirtualList");
}
} // namespace portals::ui::detail

namespace portals {
std::expected<bool, ManagedError> SearchSelection::current() const {
  if (thread_ != std::this_thread::get_id()) {
    return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::WRONG_THREAD, {}, {}});
  }
  auto owner = owner_.lock();

  try {
    return owner ? owner->current(*result_)
                 : std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::CLOSED, {}, {}});
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::CALLBACK_FAILED, {}, {}});
  }
}
} // namespace portals
