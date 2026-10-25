// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/background.hpp>
#include <portals/detail/find_search.hpp>
#include <portals/ui_find_bar.hpp>
#include <portals/ui_scene.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace portals {
namespace detail {
struct FindPublication {
  std::shared_ptr<const FindResult> result;
  std::unique_ptr<GraphemeText> summary;
  std::size_t index{};
};

struct FindRecord {
  WidgetTree *tree{};
  std::weak_ptr<const void> lifetime;
  Node<FindBar> field;
  Node<Widget> editor;
  Node<Label> summary;
  WidthPolicy policy{};
  FindLimits limits{};
  std::shared_ptr<const FindResult> result;
  std::unique_ptr<FindPublication> pending;
  std::shared_ptr<std::function<void(FindMatch)>> selected;
  std::size_t index{};
  bool active{true};
  bool busy{};
  bool readOnly{};
  bool dirty{};
  EditMode mode{};
  std::optional<TreeError> lastFailure;
  std::size_t preparedQueries{};
  std::shared_ptr<ui::detail::SceneActivity> activity;
  std::function<void(std::shared_ptr<const FindResult>)> requestModel;
  std::function<bool()> modelCurrent;
  bool modelPending{};
};

struct FindAccess {
  // This private label always keeps its original policy and empty decorations. Commit only
  // prepared text by noexcept assignment: no value-parameter move construction or parsing.
  static std::expected<void, TreeError> installSummary(const std::shared_ptr<FindRecord> &record,
                                                       FindPublication &value) {
    std::expected<void, TreeError> result;
    auto edited = record->tree->edit(record->summary, [&](Label &label) {
      if (label.text_.policy() != value.summary->policy()) {
        result = std::unexpected(TreeError::INVALID_ARGUMENT);
        return;
      }
      if (label.text_ != *value.summary) {
        result = record->tree->invalidate(record->summary, Invalidation::LAYOUT);
        if (!result) {
          return;
        }
        label.text_ = std::move(*value.summary);
      }
      record->result = value.result;
      record->index = value.index;
    });
    return edited ? result : edited;
  }

  static FindMatch match(std::shared_ptr<const FindResult> result, std::size_t index) {
    FindMatch value;
    value.result_ = std::move(result);
    value.index_ = index;

    return value;
  }

  static bool current(const FindMatch &value, const std::shared_ptr<const FindResult> &result) noexcept {
    return value && value.result_ == result;
  }

  static const FindDocument &document(const FindMatch &value) noexcept {
    const auto &range = value.result_->ranges[value.index_];

    return *(range.document ? range.document : value.result_->document);
  }
};
} // namespace detail

FindMatch::operator bool() const noexcept {
  return result_ && index_ < result_->ranges.size();
}

std::size_t FindMatch::begin() const noexcept {
  return *this ? result_->ranges[index_].begin : 0;
}

std::size_t FindMatch::end() const noexcept {
  return *this ? result_->ranges[index_].end : 0;
}

std::size_t FindMatch::ordinal() const noexcept {
  return *this ? index_ + 1 : 0;
}

RowId FindMatch::rowId() const noexcept {
  return *this ? result_->ranges[index_].row : RowId{};
}

std::uint64_t FindMatch::sourceRevision() const noexcept {
  return result_ && result_->source ? result_->source->revision : 0;
}

std::uint64_t FindMatch::queryGeneration() const noexcept {
  return result_ ? result_->generation : 0;
}

std::size_t FindMatch::storedCount() const noexcept {
  return result_ ? result_->ranges.size() : 0;
}

bool FindMatch::truncated() const noexcept {
  return result_ && result_->truncated;
}

std::string_view FindMatch::text() const noexcept {
  return *this ? detail::FindAccess::document(*this).text->bytes().substr(begin(), end() - begin())
               : std::string_view{};
}

std::string_view FindMatch::context(std::size_t maxBytes) const noexcept {
  if (!*this || end() - begin() > maxBytes) {
    return {};
  }

  const auto &document = detail::FindAccess::document(*this);
  const auto bytes = document.text->bytes();
  const auto clusters = document.text->graphemes();
  const auto budget = std::min(maxBytes, bytes.size());
  const auto before = (budget - (end() - begin())) / 2;
  const auto desiredBegin = begin() > before ? begin() - before : 0;
  const auto first = std::ranges::lower_bound(clusters, desiredBegin, {}, &Grapheme::byte_begin);
  const auto start = first == clusters.end() ? begin() : first->byte_begin;
  const auto desiredEnd = start + std::min(budget, bytes.size() - start);
  const auto after = std::ranges::upper_bound(clusters, desiredEnd, {}, &Grapheme::byte_end);
  const auto finish = after == clusters.begin() ? end() : std::prev(after)->byte_end;

  return bytes.substr(start, finish - start);
}

FindBar::FindBar(FindBarAppearance appearance)
    : Column(appearance.layout, appearance.policy), record_(std::make_shared<detail::FindRecord>()) {
}

FindBar::~FindBar() {
  record_->active = false;

  if (record_->activity) {
    record_->activity->cancel();
  }
}

Node<Widget> FindBar::editor() const {
  return record_->editor;
}

FindMatch FindBar::match() const {
  return detail::FindAccess::match(record_->result, record_->index);
}
} // namespace portals

namespace portals::ui {
namespace {
using portals::detail::FindAccess;
using portals::detail::FindDocument;
using portals::detail::FindPublication;
using portals::detail::FindRecord;
using portals::detail::FindResult;

ManagedError findError(Node<Widget> node, TreeError cause) {
  return ManagedError{ManagedPhase::TEXT, node.id(), cause, {}, {}};
}

bool validLimits(const FindLimits &limits, const LineEditOptions &editor) {
  return limits.maxDocumentBytes <= FindLimits::MAX_DOCUMENT_BYTES && limits.maxMatches > 0 &&
         limits.maxMatches <= FindLimits::MAX_MATCHES && editor.limits.max_input_bytes <= FindLimits::MAX_QUERY_BYTES;
}

std::expected<std::shared_ptr<const FindDocument>, ManagedError>
makeDocument(std::string_view bytes, WidthPolicy policy, FindLimits limits, Node<Widget> node) {
  const TextLimits textLimits{limits.maxDocumentBytes, limits.maxDocumentBytes, limits.maxDocumentBytes,
                              limits.maxDocumentBytes * 2};
  auto parsed = GraphemeText::from(bytes, policy, textLimits);

  if (!parsed) {
    auto failure = findError(node, TreeError::INVALID_ARGUMENT);
    failure.text = parsed.error();

    return std::unexpected(failure);
  }

  auto document = std::make_shared<FindDocument>();
  document->text = std::make_unique<GraphemeText>(std::move(*parsed));
  document->boundaries.resize(bytes.size() + 1);
  document->boundaries[0] = 1;

  for (const auto &cluster : document->text->graphemes()) {
    document->boundaries[cluster.byte_end] = 1;
  }

  return std::shared_ptr<const FindDocument>(std::move(document));
}

// Prefix matching is linear even for repetitive input; byte matches need two cached boundary tests.
std::shared_ptr<const FindResult> search(std::shared_ptr<const FindDocument> document, std::string_view query,
                                         std::size_t cap) {
  auto result = std::make_shared<FindResult>();
  result->document = std::move(document);
  result->query.assign(query);

  if (query.empty()) {
    return result;
  }

  std::vector<std::size_t> prefix(query.size(), 0);

  for (std::size_t i = 1, matched = 0; i < query.size(); ++i) {
    while (matched && query[i] != query[matched]) {
      matched = prefix[matched - 1];
    }
    if (query[i] == query[matched]) {
      ++matched;
    }
    prefix[i] = matched;
  }

  const auto bytes = result->document->text->bytes();
  const auto &boundaries = result->document->boundaries;

  for (std::size_t i = 0, matched = 0; i < bytes.size(); ++i) {
    while (matched && bytes[i] != query[matched]) {
      matched = prefix[matched - 1];
    }
    if (bytes[i] == query[matched]) {
      ++matched;
    }
    if (matched != query.size()) {
      continue;
    }

    const auto end = i + 1;
    const auto begin = end - query.size();
    matched = prefix[matched - 1];

    if (!boundaries[begin] || !boundaries[end]) {
      continue;
    }
    if (result->ranges.size() == cap) {
      result->truncated = true;
      break;
    }
    result->ranges.push_back({begin, end});
  }

  return result;
}

std::unique_ptr<FindPublication> presentation(std::shared_ptr<const FindResult> result, std::size_t index,
                                              WidthPolicy policy, bool dirty = false, std::string_view notice = {}) {
  auto value = std::make_unique<FindPublication>();
  value->result = std::move(result);
  value->index = index;
  auto match = FindAccess::match(value->result, index);
  std::string caption(0, '\0');

  if (dirty) {
    caption = "Previous search; press Enter | ";
  }
  if (!notice.empty()) {
    caption += notice;
  } else if (value->result->query.empty()) {
    caption += "Type a query";
  } else if (!match) {
    caption += "No matches";
  } else {
    caption += std::to_string(match.ordinal());
    caption += " of ";
    caption += std::to_string(match.storedCount());
    caption += match.truncated() ? "+ (first matches): " : ": ";
    const auto context = match.context();
    caption += context.empty() ? std::string_view("Selected match exceeds preview budget") : context;
  }

  auto text = GraphemeText::from(caption, policy);

  // All data comes from an admitted document and fixed ASCII; no new interpretation policy is introduced.
  if (!text) {
    throw std::logic_error("Prepared FindBar preview violates admitted text policy");
  }
  value->summary = std::make_unique<GraphemeText>(std::move(*text));

  return value;
}

bool alive(const std::shared_ptr<FindRecord> &record) noexcept {
  return record->active && !record->lifetime.expired();
}

std::expected<void, TreeError> install(const std::shared_ptr<FindRecord> &record, FindPublication &value) {
  if (!alive(record)) {
    return std::unexpected(TreeError::STALE_NODE);
  }

  return FindAccess::installSummary(record, value);
}

struct FindGuard {
  std::shared_ptr<FindRecord> record;

  explicit FindGuard(std::shared_ptr<FindRecord> value) : record(std::move(value)) {
    record->busy = true;
  }

  ~FindGuard() {
    record->busy = false;
  }
};

void selected(const std::shared_ptr<FindRecord> &record) {
  auto callback = record->selected;

  if (alive(record) && callback && *callback) {
    (*callback)(FindAccess::match(record->result, record->index));
  }
}

class FindSearchJob final : public BackgroundJob {
public:
  FindSearchJob(std::shared_ptr<const portals::detail::FindSearchInput> input,
                Publisher<portals::detail::FindSearchCompletion> publisher)
      : completion_(std::make_shared<portals::detail::FindSearchCompletion>()), publisher_(std::move(publisher)) {
    completion_->input = std::move(input);
  }

  void run(std::stop_token stop) noexcept override {
    completion_->result = portals::detail::searchModel(*completion_->input, stop);
  }

  std::expected<void, AsyncError> publish() override {
    auto result = publisher_.publish(completion_);

    if (result) {
      return {};
    }

    switch (result.error()) {
    case SchedulerError::WOULD_BLOCK:
      return std::unexpected(AsyncError::WOULD_BLOCK);
    case SchedulerError::NO_MEMORY:
      return std::unexpected(AsyncError::NO_MEMORY);
    default:
      return std::unexpected(AsyncError::CLOSED);
    }
  }

private:
  std::shared_ptr<portals::detail::FindSearchCompletion> completion_;
  Publisher<portals::detail::FindSearchCompletion> publisher_;
};

class FindModelActivity final : public detail::SceneActivity, public std::enable_shared_from_this<FindModelActivity> {
public:
  FindModelActivity(ManagedScene &scene, const std::shared_ptr<FindRecord> &record,
                    std::shared_ptr<portals::detail::FindModelSource> source, FindSearchOptions options,
                    std::function<void(FindSearchError)> failed, std::uint64_t session)
      : scene_(scene), record_(record), binding_(std::move(source)), options_(options), failed_(std::move(failed)),
        session_(session) {
  }

  bool active() const noexcept override {
    auto record = record_.lock();

    return !cancelled_ && record && alive(record);
  }

  void cancel() noexcept override {
    cancelled_ = true;

    if (latest_) {
      latest_->cancel();
    }

    (void)connection_.subscription.disconnect();
    completion_.reset();
  }

  void request(std::shared_ptr<const FindResult> query) {
    if (cancelled_) {
      return;
    }

    if (latest_) {
      latest_->cancel();
    }

    completion_.reset();
    needsSearch_ = true;

    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      failTree(TreeError::IDENTITY_EXHAUSTED);
      cancel();

      return;
    }

    ++generation_;

    if (auto record = record_.lock()) {
      record->modelPending = !query->query.empty();
    }
  }

  bool current() const noexcept {
    auto record = record_.lock();

    return active() && record->result->source == source_ && record->result->session == session_ &&
           record->result->generation == generation_ && !record->modelPending;
  }

  std::expected<void, TreeError> connect() {
    auto record = record_.lock();
    std::expected<SchedulerConnection<portals::detail::FindSearchCompletion>, SchedulerError> connection;
    auto scoped = record->tree->withScope(record->field, [&](const UiScope &scope) {
      connection = detail::SceneAccess::application(scene_).scheduler().connect<portals::detail::FindSearchCompletion>(
        scope, [weak = weak_from_this()](std::shared_ptr<const portals::detail::FindSearchCompletion> value) {
          if (auto activity = weak.lock(); activity && activity->active()) {
            if (value->input->session != activity->session_ || value->input->generation != activity->generation_) {
              return;
            }

            auto record = activity->record_.lock();

            activity->completion_ = std::move(value);
            auto invalidated = record->tree->invalidate(record->field, Invalidation::LAYOUT);

            if (!invalidated) {
              activity->failTree(invalidated.error());
            }
          }
        });
    });

    if (!scoped) {
      return std::unexpected(scoped.error());
    }

    if (!connection) {
      return std::unexpected(connection.error() == SchedulerError::NO_MEMORY     ? TreeError::NO_MEMORY
                             : connection.error() == SchedulerError::CLOSED      ? TreeError::CLOSED
                             : connection.error() == SchedulerError::WOULD_BLOCK ? TreeError::WOULD_BLOCK
                                                                                 : TreeError::INVALID_ARGUMENT);
    }

    connection_ = std::move(*connection);

    return {};
  }

  void service() noexcept override {
    if (!active()) {
      cancel();

      return;
    }

    if (latest_) {
      try {
        auto result = latest_->service();

        if (!result) {
          failAdmission(result.error());
        }
      } catch (const std::bad_alloc &) {
        failTree(TreeError::NO_MEMORY);
      } catch (...) {
        failTree(TreeError::CALLBACK_FAILED);
      }
    }
  }

  void prepared() noexcept override {
    if (!active()) {
      cancel();

      return;
    }

    try {
      auto captured = binding_->capture(scene_);

      if (!captured) {
        failTree(captured.error());
        cancel();

        return;
      }

      auto record = record_.lock();

      if (source_ != *captured) {
        source_ = *captured;
        auto empty = std::make_shared<FindResult>();

        empty->query = record->result->query;
        request(empty);

        auto shown = presentation(empty, 0, record->policy, record->dirty, empty->query.empty() ? "" : "Searching...");
        auto installed = install(record, *shown);

        if (!installed) {
          failTree(installed.error());

          return;
        }
      }

      if (needsSearch_) {
        start(record);
      }

      auto completion = std::exchange(completion_, {});

      if (!completion || completion->input->source != source_ || completion->input->session != session_ ||
          completion->input->generation != generation_ || completion->input->query != record->result->query ||
          settled_ == generation_) {
        return;
      }

      settled_ = generation_;
      record->modelPending = false;
      FindGuard guard(record);

      if (!completion->result) {
        auto shown = presentation(record->result, 0, record->policy, record->dirty, "Search failed");
        auto installed = install(record, *shown);

        if (!installed) {
          failTree(installed.error());
        }

        if (active() && failed_) {
          failed_(completion->result.error());
        }

        return;
      }

      auto shown = presentation(*completion->result, 0, record->policy, record->dirty);
      auto installed = install(record, *shown);

      if (!installed) {
        failTree(installed.error());

        return;
      }

      selected(record);
    } catch (const std::bad_alloc &) {
      failTree(TreeError::NO_MEMORY);
    } catch (...) {
      failTree(TreeError::CALLBACK_FAILED);
    }
  }

private:
  void start(const std::shared_ptr<FindRecord> &record) {
    needsSearch_ = false;

    if (record->result->query.empty()) {
      record->modelPending = false;

      return;
    }

    auto executor = detail::SceneAccess::application(scene_).background();

    if (!executor) {
      failTree(TreeError::INVALID_ARGUMENT);

      return;
    }

    if (!latest_) {
      latest_ = std::make_unique<BackgroundLatest>(*executor);
    }

    admissionFailure_ = presentation(record->result, 0, record->policy, false, "Search failed");

    auto input = std::make_shared<portals::detail::FindSearchInput>();

    input->source = source_;
    input->query = record->result->query;
    input->options = options_;
    input->policy = record->policy;
    input->session = session_;
    input->generation = generation_;

    auto admitted = latest_->replace(std::make_unique<FindSearchJob>(input, connection_.publisher));

    if (!admitted) {
      failAdmission(admitted.error());
    }
  }

  void failAdmission(BackgroundError error) noexcept {
    auto record = record_.lock();

    // Retire admission before an observer can remove the bar or close its scene.
    // The failure caption was prepared before submission, so NO_MEMORY handling allocates no text.
    // Retire this failed intent, while keeping the live source available for a new query or pin.
    if (latest_) {
      latest_->cancel();
    }

    completion_.reset();
    needsSearch_ = false;
    settled_ = generation_;

    if (!record || !alive(record)) {
      return;
    }

    record->modelPending = false;
    admissionFailure_->result = record->result;
    admissionFailure_->index = record->index;

    (void)install(record, *admissionFailure_);

    auto failure = findError(record->field, error == BackgroundError::NO_MEMORY ? TreeError::NO_MEMORY
                                            : error == BackgroundError::CLOSED  ? TreeError::CLOSED
                                                                                : TreeError::INVALID_ARGUMENT);

    failure.background = error;
    failure.operation = "ui::findBar/background";
    (void)scene_.reportError(std::move(failure));
  }

  void failTree(TreeError error) noexcept {
    if (auto record = record_.lock(); record && alive(record)) {
      (void)scene_.reportError(findError(record->field, error));
    }
  }

  ManagedScene &scene_;
  std::weak_ptr<FindRecord> record_;
  std::shared_ptr<portals::detail::FindModelSource> binding_;
  FindSearchOptions options_;
  std::function<void(FindSearchError)> failed_;
  std::unique_ptr<FindPublication> admissionFailure_;
  std::shared_ptr<const portals::detail::FindWorkerSource> source_;
  std::shared_ptr<const portals::detail::FindSearchCompletion> completion_;
  SchedulerConnection<portals::detail::FindSearchCompletion> connection_;
  std::unique_ptr<BackgroundLatest> latest_;
  std::uint64_t session_{};
  std::uint64_t generation_{};
  std::uint64_t settled_{};
  bool needsSearch_{};
  bool cancelled_{};
};

std::expected<std::unique_ptr<FindPublication>, ManagedError>
prepareFindBarInputs(const ControlSettings &settings, WidthPolicy policy, Node<Widget> parent,
                     std::string_view document, std::string_view initial, FindBarConfig &config, bool model = false) {
  config.limits.maxDocumentBytes = settings.findMaxDocumentBytes.value_or(config.limits.maxDocumentBytes);
  config.limits.maxMatches = settings.findMaxMatches.value_or(config.limits.maxMatches);
  config.previousLabel = settings.findPreviousLabel.value_or(config.previousLabel);
  config.nextLabel = settings.findNextLabel.value_or(config.nextLabel);
  config.clearLabel = settings.findClearLabel.value_or(config.clearLabel);
  config.appearance.layout = detail::controlLinearOptions(settings, config.appearance.layout);
  auto effective = detail::controlLineEditOptions(settings, config.editor.options, policy, {}, parent);

  if (!effective) {
    return std::unexpected(effective.error());
  }
  if ((!model && !validLimits(config.limits, *effective)) ||
      effective->limits.max_input_bytes > FindLimits::MAX_QUERY_BYTES ||
      effective->displayMode != LineEditDisplayMode::NORMAL ||
      (model && !portals::detail::validFindSearchOptions(config.search))) {
    return std::unexpected(findError(parent, TreeError::INVALID_ARGUMENT));
  }
  config.editor.options = *effective;
  if (config.editor.policy.width.mode == SizeMode::AUTOMATIC) {
    config.editor.policy.width.mode = SizeMode::FLEX;
  }

  if (model) {
    auto query = settings.text.value_or(TextValue(initial)).resolve(policy, effective->limits);

    if (!query) {
      return std::unexpected(findError(parent, TreeError::INVALID_ARGUMENT));
    }

    auto result = std::make_shared<FindResult>();

    result->query.assign(query->bytes());
    config.search.maxMatches = settings.findMaxMatches.value_or(config.search.maxMatches);

    if (!portals::detail::validFindSearchOptions(config.search)) {
      return std::unexpected(findError(parent, TreeError::INVALID_ARGUMENT));
    }

    return presentation(std::move(result), 0, policy, false, query->bytes().empty() ? "" : "Searching...");
  }

  const TextLimits sourceLimits{config.limits.maxDocumentBytes, config.limits.maxDocumentBytes,
                                config.limits.maxDocumentBytes, config.limits.maxDocumentBytes * 2};
  const auto sourceValue = settings.findDocument.value_or(TextValue(document));
  auto source = sourceValue.resolve(policy, sourceLimits);
  const auto queryValue = settings.text.value_or(TextValue(initial));
  auto query = queryValue.resolve(policy, effective->limits);

  if (!source || !query) {
    auto failure = findError(parent, TreeError::INVALID_ARGUMENT);
    const auto &error = !source ? source.error() : query.error();
    if (const auto *text = std::get_if<TextError>(&error)) {
      failure.text = *text;
    } else {
      failure.resource = std::get<TextResourceError>(error);
    }
    return std::unexpected(failure);
  }

  auto stored = makeDocument(source->bytes(), policy, config.limits, parent);

  if (!stored) {
    return std::unexpected(stored.error());
  }

  auto initialResult = search(std::move(*stored), query->bytes(), config.limits.maxMatches);
  return presentation(initialResult, 0, policy);
}

bool prepareQueryPublication(const std::shared_ptr<std::weak_ptr<FindRecord>> &bridge,
                             const std::shared_ptr<std::function<bool(std::string_view)>> &legacyValidate,
                             std::string_view candidate) {
  auto record = bridge->lock();

  if (!record || !alive(record)) {
    return true; // Let the original editor alive() boundary return its original typed stale error.
  }
  if (record->busy) {
    throw std::logic_error("Reentrant FindBar publication");
  }
  record->busy = true;
  record->pending.reset();

  try {
    if (*legacyValidate && !(*legacyValidate)(candidate)) {
      return false;
    }
    if (!alive(record)) {
      return true;
    }
    if (candidate != record->result->query) {
      if (record->requestModel) {
        auto result = std::make_shared<FindResult>();

        result->query.assign(candidate);
        const auto notice = candidate.empty() ? "" : "Searching...";

        record->pending = presentation(std::move(result), 0, record->policy, false, notice);
      } else {
        record->pending =
          presentation(search(record->result->document, candidate, record->limits.maxMatches), 0, record->policy);
      }
    } else if (record->dirty) {
      record->pending = presentation(record->result, record->index, record->policy);
    }
    if (record->pending) {
      ++record->preparedQueries;
    }
    return true;
  } catch (const std::length_error &) {
    record->busy = false;
    throw std::bad_alloc();
  } catch (...) {
    record->busy = false;
    throw;
  }
}

std::expected<void, TreeError>
finishQueryPublication(const std::shared_ptr<std::weak_ptr<FindRecord>> &bridge, LineEditResult &receipt,
                       const std::function<std::expected<void, TreeError>(LineEditResult)> &legacy) {
  auto record = bridge->lock();

  if (!record || !alive(record)) {
    return legacy ? legacy(std::move(receipt)) : std::expected<void, TreeError>{};
  }

  FindGuard guard(record);
  record->lastFailure = receipt.status == LineEditStatus::FAILED ? receipt.error : std::nullopt;
  auto pending = std::move(record->pending);
  std::expected<void, TreeError> updated;
  bool changed{};

  try {
    if (receipt.status == LineEditStatus::COMMITTED && pending) {
      changed = pending->result != record->result || pending->index != record->index;
      updated = install(record, *pending);
      if (!updated && alive(record)) {
        // The editor already accepted this query. A removed or externally modified
        // private label cannot restore the old search or change subsequent source queries.
        record->result = pending->result;
        record->index = pending->index;
      }
      record->dirty = false;

      if (record->requestModel && changed) {
        record->requestModel(record->result);
        changed = false; // The accepted editor query is not a published model search result.
      }
    } else if (receipt.status == LineEditStatus::EDITED) {
      record->dirty = true;
      auto hint = presentation(record->result, record->index, record->policy, true);
      updated = install(record, *hint);
    } else if (receipt.status == LineEditStatus::CANCELLED) {
      record->dirty = false;
      auto hint = presentation(record->result, record->index, record->policy);
      updated = install(record, *hint);
    } else if (receipt.status == LineEditStatus::FAILED || receipt.status == LineEditStatus::CONFLICT ||
               receipt.status == LineEditStatus::VALIDATION_FAILED) {
      record->dirty = true;
    }
  } catch (const std::bad_alloc &) {
    updated = std::unexpected(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    updated = std::unexpected(TreeError::NO_MEMORY);
  }

  auto delivered = legacy ? legacy(std::move(receipt)) : std::expected<void, TreeError>{};

  if (updated && delivered && changed && alive(record)) {
    selected(record);
  }

  return !updated ? updated : delivered;
}

std::expected<void, ManagedError> mountFindBarChildren(ManagedScene &scene, Node<FindBar> bar,
                                                       detail::MountContext &context,
                                                       const std::shared_ptr<FindRecord> &record, FindBarConfig &config,
                                                       const Ref<FindBar> &self, const FindPublication &prepared) {
  auto toolbar = detail::Access::mount(hbox(lineEdit(std::string_view(prepared.result->query), std::move(config.editor))
                                              .classes("find-bar-query")
                                              .focusDefault(),
                                            button(config.previousLabel, config.appearance.navigationStyle)
                                              .onClick([self] {
                                                self.previous();
                                              })
                                              .classes("find-bar-previous"),
                                            button(config.nextLabel, config.appearance.navigationStyle)
                                              .onClick([self] {
                                                self.next();
                                              })
                                              .classes("find-bar-next"),
                                            button(config.clearLabel, config.appearance.navigationStyle)
                                              .onClick([self] {
                                                self.clear();
                                              })
                                              .classes("find-bar-clear")),
                                       scene, bar, context);

  if (!toolbar) {
    return std::unexpected(toolbar.error());
  }

  auto children = record->tree->inspect(*toolbar);

  if (!children || children->children.empty()) {
    return std::unexpected(findError(bar, children ? TreeError::INVALID_ARGUMENT : children.error()));
  }
  record->editor = children->children.front();

  auto summary = detail::Access::mount(
    label(prepared.summary->bytes(), config.appearance.summaryStyle).classes("find-bar-summary"), scene, bar, context);

  if (!summary) {
    return std::unexpected(summary.error());
  }
  record->summary = *summary;
  return {};
}

} // namespace
} // namespace portals::ui

namespace portals::ui {
std::expected<std::shared_ptr<portals::detail::FindRecord>, ManagedError>
detail::Access::findRecord(ManagedScene &scene, Node<Widget> node) {
  std::shared_ptr<portals::detail::FindRecord> record;
  auto read = scene.app_.tree().read(node, [&](const Widget &widget) {
    if (const auto *bar = dynamic_cast<const FindBar *>(&widget)) {
      record = bar->record_;
    }
  });

  if (!read || !record) {
    return std::unexpected(findError(node, read ? TreeError::INVALID_ARGUMENT : read.error()));
  }

  return record;
}

std::expected<std::size_t, ManagedError> detail::Access::findPreparedQueries(ManagedScene &scene, Node<Widget> node) {
  auto found = findRecord(scene, node);
  if (!found) {
    return std::unexpected(found.error());
  }
  return (*found)->preparedQueries;
}

std::expected<FindMatch, ManagedError> detail::Access::findMatch(ManagedScene &scene, Node<Widget> node) {
  auto found = findRecord(scene, node);

  return found ? std::expected<FindMatch, ManagedError>(FindAccess::match((*found)->result, (*found)->index))
               : std::unexpected(found.error());
}

std::expected<bool, ManagedError> detail::Access::findCurrent(ManagedScene &scene, Node<Widget> node,
                                                              const FindMatch &value) {
  auto found = findRecord(scene, node);

  return found ? std::expected<bool, ManagedError>(FindAccess::current(value, (*found)->result) &&
                                                   (!(*found)->modelCurrent || (*found)->modelCurrent()))
               : std::unexpected(found.error());
}

std::expected<FindMatch, ManagedError> detail::Access::findNavigate(ManagedScene &scene, Node<Widget> node,
                                                                    FindDirection direction) {
  auto found = findRecord(scene, node);

  if (!found) {
    return std::unexpected(found.error());
  }

  auto record = *found;
  auto admitted = scene.app_.tree().edit(node, [](Widget &) {
  });

  if (!admitted) {
    return std::unexpected(findError(node, admitted.error()));
  }
  if (record->busy || *scene.textMutating_) {
    return std::unexpected(findError(node, TreeError::WOULD_BLOCK));
  }
  if (direction != FindDirection::NEXT && direction != FindDirection::PREVIOUS) {
    return std::unexpected(findError(node, TreeError::INVALID_ARGUMENT));
  }

  auto eligible = scene.app_.tree().inputEligible(node);

  if (!eligible || !*eligible) {
    return std::unexpected(findError(node, eligible ? TreeError::WOULD_BLOCK : eligible.error()));
  }

  const auto count = record->result->ranges.size();

  if (record->modelCurrent && !record->modelCurrent()) {
    return FindMatch{};
  }

  if (count <= 1) {
    return FindAccess::match(record->result, record->index);
  }

  FindGuard guard(record);

  try {
    const auto index = direction == FindDirection::NEXT ? (record->index + 1 == count ? 0 : record->index + 1)
                                                        : (record->index == 0 ? count - 1 : record->index - 1);
    auto next = presentation(record->result, index, record->policy, record->dirty);
    auto updated = install(record, *next);

    if (!updated) {
      return std::unexpected(findError(node, updated.error()));
    }

    auto result = FindAccess::match(record->result, record->index);
    selected(record);

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(findError(node, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(findError(node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(findError(node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> detail::Access::findDocument(ManagedScene &scene, Node<Widget> node,
                                                               std::string_view bytes) {
  auto found = findRecord(scene, node);

  if (!found) {
    return std::unexpected(found.error());
  }

  auto record = *found;

  if (record->requestModel) {
    return std::unexpected(findError(node, TreeError::INVALID_ARGUMENT));
  }
  auto admitted = scene.app_.tree().edit(node, [](Widget &) {
  });

  if (!admitted) {
    return std::unexpected(findError(node, admitted.error()));
  }
  if (record->busy || *scene.textMutating_) {
    return std::unexpected(findError(node, TreeError::WOULD_BLOCK));
  }

  FindGuard guard(record);

  try {
    auto document = makeDocument(bytes, record->policy, record->limits, node);

    if (!document) {
      return std::unexpected(document.error());
    }

    auto next = presentation(search(std::move(*document), record->result->query, record->limits.maxMatches), 0,
                             record->policy, record->dirty);
    auto updated = install(record, *next);

    if (!updated) {
      return std::unexpected(findError(node, updated.error()));
    }
    selected(record);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(findError(node, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(findError(node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(findError(node, TreeError::CALLBACK_FAILED));
  }
}

Ref<LineEdit> detail::findEditor(const std::shared_ptr<RefTarget> &target) {
  Ref<LineEdit> reference;
  auto scene = eventScene(target);
  auto record = scene ? Access::findRecord(**scene, target->node)
                      : std::expected<std::shared_ptr<FindRecord>, ManagedError>(std::unexpected(scene.error()));
  auto published = record ? NamedAccess::publish(reference, **scene, (*record)->editor)
                          : std::expected<void, ManagedError>(std::unexpected(record.error()));

  if (!published) {
    NamedAccess::reject(reference, published.error());
  }

  return reference;
}

std::expected<FindMatch, ManagedError> detail::findNavigate(const std::shared_ptr<RefTarget> &target,
                                                            FindDirection direction) {
  auto scene = eventScene(target);

  return scene ? Access::findNavigate(**scene, target->node, direction) : std::unexpected(scene.error());
}

std::expected<FindMatch, ManagedError> detail::findMatch(const std::shared_ptr<RefTarget> &target) {
  auto scene = eventScene(target);

  return scene ? Access::findMatch(**scene, target->node) : std::unexpected(scene.error());
}

std::expected<bool, ManagedError> detail::findCurrent(const std::shared_ptr<RefTarget> &target,
                                                      const FindMatch &value) {
  auto scene = eventScene(target);

  return scene ? Access::findCurrent(**scene, target->node, value) : std::unexpected(scene.error());
}

std::expected<void, ManagedError> detail::findDocument(const std::shared_ptr<RefTarget> &target,
                                                       std::string_view bytes) {
  auto scene = eventScene(target);

  return scene ? Access::findDocument(**scene, target->node, bytes) : std::unexpected(scene.error());
}
} // namespace portals::ui

namespace portals::ui {
std::pair<std::expected<void, TreeError>, bool> detail::Access::findClear(ManagedScene &scene, Node<Widget> node,
                                                                          std::source_location origin) {
  auto found = findRecord(scene, node);

  if (!found) {
    return {std::unexpected(std::get<TreeError>(found.error().cause)), false};
  }

  auto record = *found;
  auto admitted = scene.app_.tree().edit(node, [](Widget &) {
  });

  if (!admitted) {
    return {std::unexpected(admitted.error()), false};
  }
  if (record->busy || *scene.textMutating_ || record->readOnly) {
    return {std::unexpected(TreeError::WOULD_BLOCK), false};
  }

  auto eligible = scene.app_.tree().inputEligible(node);

  if (!eligible || !*eligible) {
    return {std::unexpected(eligible ? TreeError::WOULD_BLOCK : eligible.error()), false};
  }

  auto cleared = clearFindEditor(scene, record->editor, origin);

  if (!cleared.first || !alive(record) || record->mode == EditMode::IMMEDIATE) {
    return cleared;
  }

  bool dirty{};
  auto read = record->tree->read(record->editor, [&](const Widget &editor) {
    dirty = dynamic_cast<const LineEdit &>(editor).dirty();
  });

  if (!read || !dirty) {
    return {read, false};
  }

  record->lastFailure.reset();
  auto confirmed = scene.app_.router().invoke(record->editor, ActionEvent{portals::Action::ACTIVATE});

  if (!confirmed) {
    return {std::unexpected(confirmed.error()), false};
  }
  if (record->lastFailure) {
    // ManagedScene reported the original FAILED receipt before this wrapper observed it.
    return {std::unexpected(*record->lastFailure), true};
  }

  return {std::expected<void, TreeError>{}, false};
}

Description<FindBar> detail::Access::findBar(std::string document, std::string initial, FindBarConfig config,
                                             std::shared_ptr<portals::detail::FindModelSource> source) {
  Ref<FindBar> self;
  auto bridge = std::make_shared<std::weak_ptr<FindRecord>>();
  ControlSettings arguments;
  arguments.text = TextValue(initial);
  arguments.findDocument = TextValue(document);

  auto recipe =
    cold<FindBar>([document = std::move(document), initial = std::move(initial), config = std::move(config), source,
                   self, bridge](ManagedScene &scene, Node<Widget> parent,
                                 MountContext &context) mutable -> std::expected<Node<FindBar>, ManagedError> {
      const auto settings = context.controlSettings ? *context.controlSettings : ControlSettings{};
      auto prepared = prepareFindBarInputs(settings, scene.widthPolicy(), parent, document, initial, config,
                                           static_cast<bool>(source));
      if (!prepared) {
        return std::unexpected(prepared.error());
      }
      auto shown = std::move(*prepared);
      auto bar = scene.emplace<FindBar>(parent, config.appearance);

      if (!bar) {
        return bar;
      }
      if (context.root == Node<Widget>{}) {
        context.root = *bar;
      }

      auto record = *findRecord(scene, *bar);
      record->tree = &scene.app_.tree();
      record->lifetime = lifetime(scene);
      record->field = *bar;
      record->policy = scene.widthPolicy();
      record->limits = config.limits;
      record->result = shown->result;
      record->readOnly = config.editor.options.readOnly;
      record->mode = config.editor.options.editMode;

      if (source && config.selected) {
        config.selected = detail::currentSceneCallback(scene.facadeContext(), std::move(config.selected));
      }

      record->selected = std::make_shared<std::function<void(FindMatch)>>(std::move(config.selected));
      *bridge = record;

      auto scope = scene.focusScope(*bar);

      if (!scope) {
        return std::unexpected(scope.error());
      }

      auto legacyValidate = std::make_shared<std::function<bool(std::string_view)>>(std::move(config.editor.validate));
      config.editor.validate = [bridge, legacyValidate](std::string_view candidate) {
        return prepareQueryPublication(bridge, legacyValidate, candidate);
      };
      config.editor.result =
        [bridge, legacy = std::move(config.editor.result)](LineEditResult receipt) -> std::expected<void, TreeError> {
        return finishQueryPublication(bridge, receipt, legacy);
      };

      auto mounted = mountFindBarChildren(scene, *bar, context, record, config, self, *shown);
      if (!mounted) {
        return std::unexpected(mounted.error());
      }

      if (source) {
        if (config.searchFailed) {
          config.searchFailed = detail::currentSceneCallback(scene.facadeContext(), std::move(config.searchFailed));
        }

        auto required = detail::SceneAccess::requireBackground(scene);

        if (!required) {
          return std::unexpected(required.error());
        }

        auto attached = source->attach(scene);

        if (!attached) {
          return std::unexpected(attached.error());
        }

        auto session = portals::detail::modelSession();

        if (!session) {
          return std::unexpected(findError(*bar, session.error()));
        }

        auto activity = std::make_shared<FindModelActivity>(scene, record, std::move(source), config.search,
                                                            std::move(config.searchFailed), *session);
        auto connected = activity->connect();

        if (!connected) {
          return std::unexpected(findError(*bar, connected.error()));
        }

        auto admitted = detail::SceneAccess::admitActivity(scene, activity);

        if (!admitted) {
          return std::unexpected(admitted.error());
        }

        record->activity = activity;
        record->requestModel = [weak = std::weak_ptr(activity)](std::shared_ptr<const FindResult> query) {
          if (auto live = weak.lock()) {
            live->request(std::move(query));
          }
        };
        record->modelCurrent = [weak = std::weak_ptr(activity)] {
          auto live = weak.lock();

          return live && live->current();
        };
        activity->request(record->result);
      }

      return *bar;
    });

  return controlArguments(configuredControl(std::move(recipe), "FindBar"), std::move(arguments)).reference(self);
}

Description<FindBar> findBar(std::string_view document, std::string_view initial, FindBarConfig config) {
  const bool configurationValid = validLimits(config.limits, config.editor.options);
  if (!configurationValid || document.size() > config.limits.maxDocumentBytes ||
      initial.size() > config.editor.options.limits.max_input_bytes) {
    auto failed = detail::Access::cold<FindBar>(
      [configurationValid](ManagedScene &, Node<Widget> parent,
                           detail::MountContext &) -> std::expected<Node<FindBar>, ManagedError> {
        auto failure = findError(parent, TreeError::INVALID_ARGUMENT);
        if (configurationValid) {
          failure.text = TextError{TextErrorKind::INPUT_LIMIT, 0};
        }
        return std::unexpected(failure);
      });
    return std::move(failed);
  }

  return detail::Access::findBar(std::string(document), std::string(initial), std::move(config));
}
} // namespace portals::ui
