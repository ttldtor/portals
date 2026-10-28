// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/background.hpp>
#include <portals/detail/search_index.hpp>
#include <portals/ui_combo_box.hpp>
#include <portals/ui_dialogs.hpp>

#include "dialog_state.hpp"

#include <atomic>

namespace portals::detail {
struct ChoiceValue {
  SearchSourceIdentity source;
  RowId row;
  std::uint64_t index{};
  std::shared_ptr<const PreparedListText> label;
};

struct ChoiceRecord {
  Node<LineEdit> editor;
  Node<Dialog> popup;
  Node<VirtualList<SearchItem>> browse;
  Node<VirtualList<SearchItem>> search;
  ChoiceSelection selected;
  std::shared_ptr<ui::detail::SceneActivity> activity;
  std::function<std::expected<void, ManagedError>(bool)> open;
  std::function<std::expected<void, ManagedError>(std::uint64_t)> selectIndex;
  std::function<std::expected<void, ManagedError>(ChoiceSelection)> selectValue;
  std::function<std::expected<void, ManagedError>()> clear;
  std::function<std::expected<void, ManagedError>(Node<OverlayHost>)> mountPopup;
  std::vector<std::shared_ptr<std::function<void(ChoiceSelection)>>> handlers;
  bool active{true};
};

struct ChoiceAccess {
  static std::shared_ptr<ChoiceRecord> record(ComboBox &widget) {
    return widget.record_;
  }

  static std::shared_ptr<ChoiceRecord> record(const ComboBox &widget) {
    return widget.record_;
  }

  static const std::shared_ptr<const ChoiceValue> &value(const ChoiceSelection &selection) {
    return selection.value_;
  }

  static ChoiceSelection selection(std::shared_ptr<const ChoiceValue> value) {
    ChoiceSelection result;
    result.value_ = std::move(value);

    return result;
  }
};
} // namespace portals::detail

namespace portals {
ChoiceSelection::operator bool() const noexcept {
  return bool(value_);
}

RowId ChoiceSelection::rowId() const noexcept {
  return value_ ? value_->row : RowId{};
}

std::uint64_t ChoiceSelection::index() const noexcept {
  return value_ ? value_->index : 0;
}

std::string_view ChoiceSelection::text() const noexcept {
  return value_ ? value_->label->text.bytes() : std::string_view{};
}

std::uint64_t ChoiceSelection::sourceRevision() const noexcept {
  return value_ ? value_->source.revision : 0;
}

ComboBox::ComboBox(LinearOptions layout, SizePolicy policy)
    : Row(layout, policy), record_(std::make_shared<detail::ChoiceRecord>()) {
}

ComboBox::~ComboBox() {
  record_->active = false;
  if (record_->activity) {
    record_->activity->cancel();
  }
}

Node<LineEdit> ComboBox::editor() const {
  return record_->editor;
}

Node<Dialog> ComboBox::popup() const {
  return record_->popup;
}

ChoiceSelection ComboBox::selection() const {
  return record_->selected;
}
} // namespace portals

namespace portals::ui::detail {
namespace {
using portals::detail::ChoiceAccess;
using portals::detail::ChoiceRecord;
using portals::detail::ChoiceValue;
using portals::detail::FindModelSource;
using portals::detail::FindWorkerSource;

std::atomic<std::uint64_t> nextChoiceSession{1};

ManagedError choiceError(Node<Widget> node, TreeError error) {
  return {ManagedPhase::STATE, node.id(), error, {}, {}};
}

TreeError backgroundCause(BackgroundError error) noexcept {
  switch (error) {
  case BackgroundError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case BackgroundError::CLOSED:
    return TreeError::CLOSED;
  case BackgroundError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case BackgroundError::CALLBACK_FAILED:
    return TreeError::CALLBACK_FAILED;
  case BackgroundError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}

std::uint64_t issueChoiceSession() noexcept {
  auto value = nextChoiceSession.load();

  while (value && value != std::numeric_limits<std::uint64_t>::max()) {
    if (nextChoiceSession.compare_exchange_weak(value, value + 1)) {
      return value;
    }
  }

  return 0;
}

AsyncError publicationError(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::CLOSED:
    return AsyncError::CLOSED;
  case SchedulerError::WRONG_THREAD:
    return AsyncError::WRONG_THREAD;
  case SchedulerError::CALLBACK_FAILED:
    return AsyncError::CALLBACK_FAILED;
  default:
    return AsyncError::INVALID_ARGUMENT;
  }
}

// Both browse and indexed completion attach this single ordinary model adapter.
class AttachedChoiceSource final : public FindModelSource {
public:
  explicit AttachedChoiceSource(std::shared_ptr<FindModelSource> source) : source_(std::move(source)) {
  }

  std::expected<void, ManagedError> attach(ManagedScene &scene) override {
    if (attached_) {
      return {};
    }
    auto result = source_->attach(scene);
    attached_ = result.has_value();

    return result;
  }

  std::expected<std::shared_ptr<const FindWorkerSource>, TreeError> capture(ManagedScene &scene) override {
    return source_->capture(scene);
  }

private:
  std::shared_ptr<FindModelSource> source_;
  bool attached_{};
};

class ChoicePage final : public ModelSnapshot<SearchItem> {
public:
  ChoicePage(std::uint64_t revision, std::uint64_t count, RowRange available = {},
             std::shared_ptr<const SearchResult> rows = {})
      : ModelSnapshot(revision, count), available_(available), rows_(std::move(rows)) {
  }

  ModelRows<SearchItem> read(RowRange range) const override {
    if (!range.count) {
      return {RangeState::READY, {}, {}};
    }
    if (rows_ && range.first >= available_.first && range.first - available_.first <= available_.count &&
        range.count <= available_.count - (range.first - available_.first)) {
      return {RangeState::READY,
              std::span(rows_->rows).subspan(static_cast<std::size_t>(range.first - available_.first), range.count),
              {}};
    }

    return {};
  }

private:
  RowRange available_;
  std::shared_ptr<const SearchResult> rows_;
};

struct ChoiceCompletion {
  SearchSourceIdentity source;
  std::uint64_t generation{};
  bool selection{};
  std::optional<SearchError> error;
  std::optional<ModelRequest<SearchItem>> request;
  std::shared_ptr<const SearchResult> rows;
  std::shared_ptr<const ChoicePage> page;
};

struct ChoicePublication {
  std::atomic<int> failure{};
};

class ChoiceJob final : public BackgroundJob {
public:
  ChoiceJob(std::shared_ptr<const FindWorkerSource> source, SearchSourceIdentity key, RowRange range,
            SearchOptions limits, std::size_t scratchBytes, Publisher<ChoiceCompletion> publisher,
            std::uint64_t generation, std::optional<ModelRequest<SearchItem>> request,
            std::shared_ptr<ChoicePublication> publication)
      : source_(std::move(source)), range_(range), limits_(limits), scratchBytes_(scratchBytes),
        publisher_(std::move(publisher)), publication_(std::move(publication)),
        completion_(std::make_shared<ChoiceCompletion>()) {
    completion_->source = std::move(key);
    completion_->generation = generation;
    completion_->selection = !request;
    completion_->request = std::move(request);
  }

  void run(std::stop_token stop) noexcept override {
    try {
      if (stop.stop_requested() || (completion_->request && completion_->request->stopRequested())) {
        completion_->error = SearchError::CANCELLED;
        return;
      }
      portals::detail::SearchMemory memory(scratchBytes_);

      struct MetadataCharge {
        portals::detail::ProjectionMemory &memory;
        std::size_t bytes{};

        ~MetadataCharge() {
          memory.releaseExternal(bytes);
        }
      } metadata{memory};

      if (range_.count > scratchBytes_ / sizeof(portals::detail::FindRowView)) {
        completion_->error = SearchError::LIMIT_EXCEEDED;
        return;
      }
      std::vector<portals::detail::FindRowView> input(0);
      input.reserve(range_.count);
      memory.chargeExternal(input.capacity() * sizeof(portals::detail::FindRowView));
      metadata.bytes = input.capacity() * sizeof(portals::detail::FindRowView);
      auto read = source_->read(range_, input, &memory);

      if (!read) {
        completion_->error = read.error() == FindSearchError::NO_MEMORY         ? SearchError::NO_MEMORY
                             : read.error() == FindSearchError::CALLBACK_FAILED ? SearchError::CALLBACK_FAILED
                             : read.error() == FindSearchError::NOT_READY       ? SearchError::NOT_READY
                             : read.error() == FindSearchError::LIMIT_EXCEEDED  ? SearchError::LIMIT_EXCEEDED
                                                                                : SearchError::INVALID_ARGUMENT;
        return;
      }
      auto rows = std::make_shared<SearchResult>();
      rows->source = completion_->source;
      rows->generation = completion_->generation;
      std::size_t bytes{};

      for (const auto &row : input) {
        if (stop.stop_requested() || (completion_->request && completion_->request->stopRequested())) {
          completion_->error = SearchError::CANCELLED;
          return;
        }
        if (row.text.size() > limits_.maxRowBytes || bytes > limits_.maxProjectedBytes ||
            row.text.size() > limits_.maxProjectedBytes - bytes) {
          completion_->error = SearchError::LIMIT_EXCEEDED;
          return;
        }
        bytes += row.text.size();
        auto text = GraphemeText::from(
          row.text, completion_->source.width,
          TextLimits{limits_.maxRowBytes, limits_.maxRowBytes, limits_.maxRowGraphemes, limits_.maxRowBytes * 2});

        if (!text) {
          completion_->error = SearchError::INVALID_TEXT;
          return;
        }
        SearchItem item;
        auto presentation = std::make_shared<portals::detail::PreparedListText>(std::move(*text));
        presentation->runs.push_back({presentation->text, 0, false});
        item.presentation = std::move(presentation);
        item.rank.row = row.id;
        item.rank.sourceOrdinal = row.ordinal;
        rows->rows.push_back({row.id, std::move(item)});
      }
      completion_->rows = rows;
      if (completion_->request) {
        completion_->page = std::make_shared<const ChoicePage>(completion_->request->dataRevision(), source_->rowCount,
                                                               range_, std::move(rows));
      }
    } catch (const portals::detail::SearchBudgetExceeded &) {
      completion_->error = SearchError::LIMIT_EXCEEDED;
    } catch (const std::bad_alloc &) {
      completion_->error = SearchError::NO_MEMORY;
    } catch (...) {
      completion_->error = SearchError::CALLBACK_FAILED;
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto published = publisher_.publish(completion_);

    if (published) {
      return {};
    }
    const auto error = publicationError(published.error());
    if (error != AsyncError::WOULD_BLOCK && error != AsyncError::NO_MEMORY) {
      publication_->failure.store(static_cast<int>(error) + 1);
      publisher_.wake();
    }

    return std::unexpected(error);
  }

private:
  std::shared_ptr<const FindWorkerSource> source_;
  RowRange range_;
  SearchOptions limits_;
  std::size_t scratchBytes_{};
  Publisher<ChoiceCompletion> publisher_;
  std::shared_ptr<ChoicePublication> publication_;
  std::shared_ptr<ChoiceCompletion> completion_;
};

class ChoiceActivity final : public SceneActivity, public std::enable_shared_from_this<ChoiceActivity> {
public:
  ChoiceActivity(ManagedScene &scene, Node<ComboBox> node, std::weak_ptr<ChoiceRecord> record,
                 std::shared_ptr<FindModelSource> source, ComboBoxConfig config, Ref<LineEdit> editor, bool readOnly,
                 std::uint64_t session)
      : scene_(scene), node_(node), lifetime_(Access::lifetime(scene)), record_(std::move(record)),
        binding_(std::move(source)), config_(std::move(config)), editor_(std::move(editor)), readOnly_(readOnly),
        session_(session), initial_(config_.initial) {
  }

  ~ChoiceActivity() override {
    cancel();
  }

  bool active() const noexcept override {
    const auto record = record_.lock();
    return !cancelled_ && !lifetime_.expired() && record && record->active &&
           bool(SceneAccess::application(scene_).tree().withScope(node_, [](const UiScope &) {
           }));
  }

  bool eligible() const {
    if (!active()) {
      return false;
    }
    auto eligible = SceneAccess::application(scene_).tree().inputEligible(node_);

    return eligible && *eligible;
  }

  bool opened() const {
    auto record = record_.lock();
    return record && record->popup != Node<Dialog>{} && active() && Access::choicePopupOpened(scene_, record->popup);
  }

  bool searching() const {
    return eligible() && opened() && !queryText_.empty();
  }

  void cancel() noexcept override {
    if (cancelled_) {
      return;
    }
    cancelled_ = true;
    if (browseLatest_) {
      browseLatest_->cancel();
    }
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    if (browseRequest_) {
      browseRequest_->cancel();
    }
    (void)connection_.subscription.disconnect();
    browseCompletion_.reset();
    selectCompletion_.reset();
    pendingBase_.reset();
    pendingIndex_.reset();
    pendingValue_ = {};
    source_.reset();
    if (!lifetime_.expired()) {
      Access::cancelChoicePopup(scene_, this);
    }
  }

  void attachPopup(ManagedModel<SearchItem> browse, ManagedModel<SearchItem> searchModel,
                   Publisher<ModelSnapshot<SearchItem>> publisher, Ref<LineEdit> query,
                   Ref<VirtualList<SearchItem>> list, Ref<VirtualList<SearchItem>> search, Node<Dialog> popup,
                   std::function<void()> modelLease) {
    browseModel_ = std::move(browse);
    searchModel_ = std::move(searchModel);
    browsePublisher_ = std::move(publisher);
    query_ = std::move(query);
    list_ = std::move(list);
    search_ = std::move(search);
    popup_ = popup;
    modelLease_ = std::move(modelLease);
    queryText_.clear();
    if (source_) {
      pendingBase_ = std::make_shared<const ChoicePage>(++browseRevision_, source_->rowCount);
    }
  }

  std::expected<void, ManagedError> dismissPopup() override {
    return open(false);
  }

  std::expected<void, ManagedError> commitPopup() override try {
    if (!eligible()) {
      openPending_ = false;
      navigationPending_ = false;
      Access::cancelChoicePopup(scene_, this);
      if (popup_ != Node<Dialog>{}) {
        return cleanupPopup();
      }
      return {};
    }
    if (!openPending_) {
      return finishNavigation();
    }
    auto claimed = Access::claimChoicePopup(scene_, shared_from_this());
    if (!claimed) {
      openPending_ = false;
      navigationPending_ = false;
      Access::cancelChoicePopup(scene_, this);
      return claimed;
    }
    auto host = Access::choiceLayerHost(scene_, node_);
    if (!host) {
      openPending_ = false;
      navigationPending_ = false;
      Access::cancelChoicePopup(scene_, this);
      return std::unexpected(host.error());
    }
    auto record = record_.lock();
    if (record->popup == Node<Dialog>{}) {
      auto mounted = record->mountPopup(*host);
      if (!mounted) {
        openPending_ = false;
        Access::cancelChoicePopup(scene_, this);
        mounted.error().operation = "ComboBox.mountPopup";
        return mounted;
      }
      return {};
    }
    auto geometry = SceneAccess::application(scene_).tree().inspect(node_);
    if (!geometry || !geometry->geometry.valid) {
      return {};
    }
    openPending_ = false;
    Access::cancelChoicePopup(scene_, this);
    auto result = scene_.openDialog(record->popup);
    if (!result) {
      auto removed = cleanupPopup();
      if (!removed) {
        result.error().cleanup = std::get<TreeError>(removed.error().cause);
      }
      result.error().operation = "ComboBox.openPopup";
      return result;
    }
    auto &app = SceneAccess::application(scene_);
    auto focused =
      app.router().setFocus(readOnly_ ? (config_.searchable ? Node<Widget>(query_.node()) : Node<Widget>(list_.node()))
                                      : Node<Widget>(editor_.node()));
    if (!focused) {
      return std::unexpected(choiceError(node_, focused.error()));
    }
    return finishNavigation();
  } catch (const std::bad_alloc &) {
    return popupFailure(TreeError::NO_MEMORY);
  } catch (...) {
    return popupFailure(TreeError::CALLBACK_FAILED);
  }

  void wakePopup() noexcept override {
    if (openPending_) {
      connection_.publisher.wake();
    } else if (navigationPending_ && queryText_ == navigationQuery_ && opened()) {
      try {
        auto row = Access::choiceFirstRow(scene_, queryText_.empty() ? list_.node() : search_.node());
        if (row && *row) {
          connection_.publisher.wake();
        }
      } catch (...) {
        // The pending command remains retryable on the next ordinary UI cycle.
      }
    }
  }

  void retire() noexcept override {
    (void)tryRetire();
  }

  bool tryRetire() noexcept override {
    if (lifetime_.expired()) {
      return true;
    }
    try {
      auto result = cleanupPopup();
      if (!result) {
        result.error().operation = "ComboBox.retirePopup";
        (void)scene_.reportError(result.error());
      }
      return bool(result) || popup_ == Node<Dialog>{};
    } catch (const std::bad_alloc &) {
      retirementFailed(TreeError::NO_MEMORY);
    } catch (...) {
      retirementFailed(TreeError::CALLBACK_FAILED);
    }
    return popup_ == Node<Dialog>{};
  }

  std::expected<void, ManagedError> connect() {
    auto &app = SceneAccess::application(scene_);
    std::expected<SchedulerConnection<ChoiceCompletion>, SchedulerError> connected;
    auto scoped = app.tree().withScope(node_, [&](const UiScope &scope) {
      connected = app.scheduler().connect<ChoiceCompletion>(
        scope, [weak = weak_from_this()](std::shared_ptr<const ChoiceCompletion> value) {
          auto self = weak.lock();
          if (!self || !self->active() || value->source != self->key_) {
            return;
          }
          if (value->selection) {
            if (value->generation != self->selectGeneration_) {
              return;
            }
            self->selectCompletion_ = std::move(value);
          } else {
            if (value->generation != self->browseGeneration_ || !self->opened() || !self->eligible()) {
              return;
            }
            self->browseCompletion_ = std::move(value);
          }
          self->take(SceneAccess::application(self->scene_).tree().invalidate(self->node_, Invalidation::LAYOUT));
        });
    });
    if (!scoped || !connected) {
      return std::unexpected(choiceError(node_, !scoped ? scoped.error()
                                                : connected.error() == SchedulerError::NO_MEMORY
                                                  ? TreeError::NO_MEMORY
                                                  : TreeError::INVALID_ARGUMENT));
    }
    connection_ = std::move(*connected);

    return {};
  }

  std::expected<void, ManagedError> open(bool value) {
    if (!active()) {
      return std::unexpected(choiceError(node_, TreeError::CLOSED));
    }
    auto record = record_.lock();
    if (!value) {
      openPending_ = false;
      navigationPending_ = false;
      acceptPending_ = false;
      Access::cancelChoicePopup(scene_, this);
      stopBrowse();
      if (record->popup == Node<Dialog>{}) {
        return {};
      }
      auto focus = SceneAccess::application(scene_).router().focused();
      if (!focus) {
        return std::unexpected(choiceError(node_, focus.error()));
      }
      if (Access::choiceWithin(scene_, record->popup, *focus) && eligible()) {
        auto restored = SceneAccess::application(scene_).router().setFocus(editor_.node());
        if (!restored) {
          return std::unexpected(choiceError(node_, restored.error()));
        }
      }
      auto dismissed = scene_.dismissDialog(record->popup);
      if (!dismissed) {
        return dismissed;
      }
      return cleanupPopup();
    }
    if (!eligible()) {
      return std::unexpected(choiceError(node_, TreeError::WOULD_BLOCK));
    }
    if (openPending_ || opened()) {
      return {};
    }
    openPending_ = true;
    Access::requestChoicePopup(scene_, shared_from_this());
    return {};
  }

  std::expected<void, ManagedError> selectIndex(std::uint64_t index) {
    if (!active()) {
      return std::unexpected(choiceError(node_, TreeError::CLOSED));
    }
    auto captured = binding_->capture(scene_);
    if (!captured) {
      return std::unexpected(choiceError(node_, captured.error()));
    }
    updateSource(*captured);
    if (!active() || !source_) {
      return std::unexpected(choiceError(node_, TreeError::CLOSED));
    }
    if (index >= source_->rowCount) {
      return std::unexpected(choiceError(node_, TreeError::INVALID_ARGUMENT));
    }
    if (selectGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(choiceError(node_, TreeError::IDENTITY_EXHAUSTED));
    }
    auto revision = Access::choiceDraftRevision(scene_, editor_.node());
    if (!revision) {
      return std::unexpected(revision.error());
    }
    ++selectGeneration_;
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    selectCompletion_.reset();
    pendingValue_ = {};
    pendingDraftRevision_ = *revision;
    pendingIndex_ = index;
    return startSelection();
  }

  std::expected<void, ManagedError> selectValue(ChoiceSelection value) {
    if (!value || !active()) {
      return std::unexpected(choiceError(node_, TreeError::INVALID_ARGUMENT));
    }
    auto captured = binding_->capture(scene_);
    if (!captured) {
      return std::unexpected(choiceError(node_, captured.error()));
    }
    if (!source_ || *captured != source_ || ChoiceAccess::value(value)->source != key_) {
      return std::unexpected(choiceError(node_, TreeError::STALE_NODE));
    }
    auto revision = Access::choiceDraftRevision(scene_, editor_.node());
    if (!revision) {
      return std::unexpected(revision.error());
    }
    if (selectGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(choiceError(node_, TreeError::IDENTITY_EXHAUSTED));
    }
    ++selectGeneration_;
    pendingIndex_.reset();
    selectCompletion_.reset();
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    pendingDraftRevision_ = *revision;
    pendingValue_ = std::move(value);
    return {};
  }

  std::expected<void, ManagedError> accept(ChoiceSelection value) {
    if (!value || !active()) {
      return std::unexpected(choiceError(node_, TreeError::INVALID_ARGUMENT));
    }
    const auto &data = ChoiceAccess::value(value);
    auto captured = binding_->capture(scene_);
    if (!captured) {
      return std::unexpected(choiceError(node_, captured.error()));
    }
    if (!source_ || *captured != source_ || data->source != key_) {
      return std::unexpected(choiceError(node_, TreeError::STALE_NODE));
    }
    if (selectGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(choiceError(node_, TreeError::IDENTITY_EXHAUSTED));
    }
    const auto intent = ++selectGeneration_;
    pendingIndex_.reset();
    pendingValue_ = {};
    pendingDraftRevision_.reset();
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    auto applied = editor_.text().set(value.text()).result();
    if (!applied || !active()) {
      return applied;
    }
    if (intent != selectGeneration_) {
      return std::unexpected(choiceError(node_, TreeError::WOULD_BLOCK));
    }
    auto &app = SceneAccess::application(scene_);
    auto confirmed = app.router().invoke(editor_.node(), {portals::Action::ACTIVATE});
    if (!confirmed) {
      return std::unexpected(choiceError(node_, confirmed.error()));
    }
    if (!active()) {
      return {};
    }
    if (intent != selectGeneration_) {
      return std::unexpected(choiceError(node_, TreeError::WOULD_BLOCK));
    }
    bool committed{};
    auto checked = app.tree().read(editor_.node(), [&](const LineEdit &editor) {
      committed = !editor.dirty() && editor.text().bytes() == value.text();
    });
    if (!checked || !committed) {
      return std::unexpected(choiceError(node_, checked ? TreeError::WOULD_BLOCK : checked.error()));
    }
    auto record = record_.lock();
    const bool changed = !record->selected || ChoiceAccess::value(record->selected)->source != data->source ||
                         record->selected.rowId() != value.rowId();
    record->selected = value;
    auto dismissed = open(false);
    if (!dismissed) {
      return dismissed;
    }
    if (changed) {
      notify(value);
    }

    return {};
  }

  std::expected<void, ManagedError> clear() {
    if (!active()) {
      return std::unexpected(choiceError(node_, TreeError::CLOSED));
    }
    auto record = record_.lock();
    if (selectGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(choiceError(node_, TreeError::IDENTITY_EXHAUSTED));
    }
    ++selectGeneration_;
    pendingIndex_.reset();
    pendingValue_ = {};
    pendingDraftRevision_.reset();
    selectCompletion_.reset();
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    if (!record->selected) {
      return {};
    }
    record->selected = {};
    notify({});
    return {};
  }

  void searchActivated(SearchSelection value) {
    auto current = value.current();
    if (!current || !*current || !active() || !searching()) {
      return;
    }
    auto text = GraphemeText::from(value.text(), scene_.widthPolicy());
    if (!text) {
      failDomain(SearchError::INVALID_TEXT);
      return;
    }
    auto selected = std::make_shared<ChoiceValue>();
    selected->source = key_;
    selected->row = value.rowId();
    selected->index = value.index();
    selected->label = std::make_shared<const portals::detail::PreparedListText>(std::move(*text));
    take(accept(ChoiceAccess::selection(std::move(selected))));
  }

  void activate(RowId row) {
    activation_ = row;
  }

  void providerFailed(SearchError error) {
    failDomain(error);
  }

  std::expected<void, TreeError> browse(const std::optional<ModelRequest<SearchItem>> &request) {
    if (!request) {
      return {};
    }
    if (!eligible() || !opened() || !queryText_.empty() || !source_) {
      request->cancel();
      return {};
    }
    if (request->range().count > config_.pageRows) {
      request->cancel();
      failDomain(SearchError::LIMIT_EXCEEDED);
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (browseGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    ++browseGeneration_;
    browseRequest_ = *request;
    browsePublication_ = std::make_shared<ChoicePublication>();
    auto executor = SceneAccess::application(scene_).background();
    if (!executor) {
      request->cancel();
      failDomain(SearchError::UNAVAILABLE);
      return {};
    }
    if (!browseLatest_) {
      browseLatest_ = std::make_unique<BackgroundLatest>(*executor);
    }
    auto admitted = browseLatest_->replace(
      std::make_unique<ChoiceJob>(source_, key_, request->range(), config_.search, config_.browseScratchBytes,
                                  connection_.publisher, browseGeneration_, *request, browsePublication_));
    if (!admitted) {
      stopBrowse();
      return backgroundFailed(admitted.error());
    }

    return {};
  }

  void service() noexcept override {
    cycle(false);
  }

  void prepared() noexcept override {
    cycle(true);
  }

  std::expected<bool, TreeError> input(const InputEvent &event) override {
    const auto *key = std::get_if<KeyEvent>(&event);
    const auto *mouse = std::get_if<MouseInputEvent>(&event);
    const bool editing =
      !readOnly_ && (std::holds_alternative<TextInputEvent>(event) || std::holds_alternative<PasteInputEvent>(event));
    const bool command =
      key && key->action != KeyAction::RELEASE &&
      (key->key == InputKey::ESCAPE || key->key == InputKey::DOWN || key->key == InputKey::TAB ||
       key->key == InputKey::ENTER || key->key == InputKey::UP || key->key == InputKey::HOME ||
       key->key == InputKey::END || key->key == InputKey::PAGE_UP || key->key == InputKey::PAGE_DOWN);
    const bool clicking = mouse && mouse->button == MouseButton::LEFT && mouse->action == MouseAction::PRESS;
    if (!editing && !command && !clicking) {
      return false;
    }
    if (!eligible()) {
      return false;
    }
    auto &app = SceneAccess::application(scene_);
    auto focus = app.router().focused();
    if (!focus) {
      return std::unexpected(focus.error());
    }
    auto record = record_.lock();
    if (!record || !active()) {
      return false;
    }
    if (clicking && opened()) {
      auto hit = app.router().hitTest({mouse->column, mouse->row});
      if (!hit) {
        return std::unexpected(hit.error());
      }
      if (!Access::choiceWithin(scene_, node_, *hit) && !Access::choiceWithin(scene_, record->popup, *hit)) {
        auto dismissed = open(false);
        if (!dismissed) {
          return std::unexpected(std::get<TreeError>(dismissed.error().cause));
        }
      }
      return false;
    }
    const bool ownFocus = Access::choiceWithin(scene_, node_, *focus);
    const bool popupFocus = Access::choiceWithin(scene_, record->popup, *focus);
    if ((!ownFocus && !popupFocus)) {
      return false;
    }
    if (editing) {
      navigationPending_ = false;
      acceptPending_ = false;
      if (!openPending_) {
        Access::cancelChoicePopup(scene_, this);
      }
      if (popupFocus) {
        auto focused = app.router().setFocus(editor_.node());
        if (!focused) {
          return std::unexpected(focused.error());
        }
      }
      autoOpenAfterEdit_ = true;
      return false;
    }
    if (!key) {
      return false;
    }
    if ((opened() || openPending_) && key->key == InputKey::ESCAPE && key->modifiers == InputModifiers{}) {
      auto closed = open(false);
      return closed ? std::expected<bool, TreeError>(true) : std::unexpected(std::get<TreeError>(closed.error().cause));
    }
    if (navigationPending_ && key->modifiers == InputModifiers{} &&
        (key->key == InputKey::HOME || key->key == InputKey::END || key->key == InputKey::UP ||
         key->key == InputKey::PAGE_UP || key->key == InputKey::PAGE_DOWN)) {
      if (key->key == InputKey::HOME || key->key == InputKey::END) {
        navigationSteps_ = 0;
        navigationFromEnd_ = key->key == InputKey::END;
        navigationPages_ = 0;
      } else if (key->key == InputKey::UP) {
        if (navigationFromEnd_) {
          if (navigationSteps_ != std::numeric_limits<std::uint64_t>::max()) {
            ++navigationSteps_;
          }
        } else if (navigationSteps_) {
          --navigationSteps_;
        }
      } else {
        const auto delta = key->key == InputKey::PAGE_DOWN ? 1 : -1;
        if ((delta > 0 && navigationPages_ < std::numeric_limits<std::int32_t>::max()) ||
            (delta < 0 && navigationPages_ > std::numeric_limits<std::int32_t>::min())) {
          navigationPages_ += delta;
        }
      }
      Access::requestChoicePopup(scene_, shared_from_this());
      return true;
    }
    if (opened() && (*focus == list_.node() || *focus == search_.node()) && key->modifiers == InputModifiers{} &&
        (key->key == InputKey::DOWN || key->key == InputKey::UP || key->key == InputKey::HOME ||
         key->key == InputKey::END || key->key == InputKey::PAGE_DOWN || key->key == InputKey::PAGE_UP)) {
      const auto action = key->key == InputKey::DOWN || key->key == InputKey::PAGE_DOWN ? portals::Action::MOVE_DOWN
                          : key->key == InputKey::UP || key->key == InputKey::PAGE_UP   ? portals::Action::MOVE_UP
                          : key->key == InputKey::HOME                                  ? portals::Action::MOVE_HOME
                                                                                        : portals::Action::MOVE_END;
      const auto page = key->key == InputKey::PAGE_DOWN ? 1 : key->key == InputKey::PAGE_UP ? -1 : 0;
      auto position =
        Access::choiceMoveNavigation(scene_, queryText_.empty() ? list_.node() : search_.node(), action, page);
      if (!position) {
        return std::unexpected(std::get<TreeError>(position.error().cause));
      }
      if (*position) {
        navigationSteps_ = **position;
        navigationFromEnd_ = false;
        navigationPages_ = 0;
        navigationSource_ = key_;
        navigationQuery_ = queryText_;
        navigationPending_ = true;
        Access::requestChoicePopup(scene_, shared_from_this());
      }
      return bool(*position);
    }
    if (key->key == InputKey::DOWN && !key->modifiers.control && !key->modifiers.shift && !key->modifiers.super &&
        (ownFocus || *focus == query_.node())) {
      auto query = (readOnly_ && opened() ? query_ : editor_).text().get();
      if (!query) {
        return std::unexpected(std::get<TreeError>(query.error().cause));
      }
      const auto nextQuery = readOnly_ && !opened() ? std::string{} : std::move(*query);
      if (navigationPending_ && navigationSource_ == key_ && navigationQuery_ == nextQuery) {
        if (navigationFromEnd_) {
          if (navigationSteps_) {
            --navigationSteps_;
          }
        } else if (navigationSteps_ != std::numeric_limits<std::uint64_t>::max()) {
          ++navigationSteps_;
        }
      } else {
        navigationFromEnd_ = false;
        navigationPages_ = 0;
        navigationSteps_ =
          nextQuery.empty() && record->selected && ChoiceAccess::value(record->selected)->source == key_
            ? record->selected.index()
            : 0;
      }
      navigationQuery_ = nextQuery;
      navigationSource_ = key_;
      navigationPending_ = true;
      acceptPending_ = false;
      auto result = open(true);
      if (result) {
        Access::requestChoicePopup(scene_, shared_from_this());
        result = finishNavigation();
      }
      return result ? std::expected<bool, TreeError>(true) : std::unexpected(std::get<TreeError>(result.error().cause));
    }
    if (navigationPending_ && key->key == InputKey::ENTER && key->modifiers == InputModifiers{}) {
      acceptPending_ = true;
      return true;
    }
    if ((opened() || openPending_) && key->key == InputKey::TAB) {
      auto closed = open(false);
      if (!closed) {
        return std::unexpected(std::get<TreeError>(closed.error().cause));
      }
    }
    return false;
  }

private:
  void retirementFailed(TreeError cause) noexcept {
    try {
      auto error = choiceError(node_, cause);
      error.operation = "ComboBox.retirePopup";
      (void)scene_.reportError(error);
    } catch (...) {
      // Failed retirement stays observed and retries only on an ordinary existing UI cycle.
    }
  }

  std::expected<void, ManagedError> popupFailure(TreeError cause) noexcept {
    openPending_ = false;
    navigationPending_ = false;
    acceptPending_ = false;
    Access::cancelChoicePopup(scene_, this);
    auto error = choiceError(node_, cause);
    error.operation = "ComboBox.openPopup";
    try {
      auto removed = cleanupPopup();
      if (!removed) {
        error.cleanup = std::get<TreeError>(removed.error().cause);
      }
    } catch (...) {
      error.cleanup = TreeError::NO_MEMORY;
    }
    return std::unexpected(error);
  }

  std::expected<void, ManagedError> finishNavigation() {
    if (!navigationPending_) {
      if (!openPending_) {
        Access::cancelChoicePopup(scene_, this);
      }
      return {};
    }
    if (navigationSource_ != key_) {
      navigationPending_ = false;
      acceptPending_ = false;
      Access::cancelChoicePopup(scene_, this);
      return {};
    }
    Access::requestChoicePopup(scene_, shared_from_this());
    if (!opened() || queryText_ != navigationQuery_) {
      return {};
    }
    auto target = queryText_.empty() ? list_.node() : search_.node();
    auto first = Access::choiceNavigationRow(scene_, target, navigationSteps_, navigationFromEnd_, navigationPages_);
    if (!first) {
      return std::unexpected(first.error());
    }
    if (!*first) {
      return {};
    }
    auto &app = SceneAccess::application(scene_);
    auto selected = setVirtualListSelection(app.tree(), target, *first);
    if (!selected) {
      return std::unexpected(choiceError(target, selected.error()));
    }
    auto focused = app.router().setFocus(target);
    if (!focused) {
      return std::unexpected(choiceError(target, focused.error()));
    }
    navigationPending_ = false;
    Access::cancelChoicePopup(scene_, this);
    if (std::exchange(acceptPending_, false)) {
      auto accepted = app.router().invoke(target, {portals::Action::ACTIVATE});
      if (!accepted) {
        return std::unexpected(choiceError(target, accepted.error()));
      }
    }
    return {};
  }

  std::expected<void, ManagedError> cleanupPopup() {
    stopBrowse();
    auto record = record_.lock();
    auto popup = record ? record->popup : popup_;
    std::expected<void, ManagedError> removed;
    if (popup != Node<Dialog>{}) {
      auto live = SceneAccess::application(scene_).tree().withScope(popup, [](const UiScope &) {
      });
      if (live) {
        removed = scene_.remove(popup);
        // Keep the coherent owner/link and private models when removal did not retire the node.
        if (!removed && SceneAccess::application(scene_).tree().withScope(popup, [](const UiScope &) {
            })) {
          return removed;
        }
      } else if (live.error() != TreeError::STALE_NODE && live.error() != TreeError::CLOSED) {
        return std::unexpected(choiceError(popup, live.error()));
      }
    }
    if (record) {
      record->popup = {};
    }
    popup_ = {};
    Access::retireChoiceModel(scene_, browseModel_);
    Access::retireChoiceModel(scene_, searchModel_);
    modelLease_ = {};
    browsePublisher_ = {};
    pendingBase_.reset();
    if (active()) {
      (void)Access::choiceCompanion(scene_, node_, {});
    }
    return removed;
  }

  void notify(ChoiceSelection value) {
    auto record = record_.lock();
    const auto count = record->handlers.size();
    for (std::size_t index = 0; index < count && active(); ++index) {
      auto handler = record->handlers[index];
      (*handler)(value);
      if (ChoiceAccess::value(record->selected) != ChoiceAccess::value(value)) {
        break;
      }
    }
  }

  void take(std::expected<void, TreeError> result) {
    if (!result && !lifetime_.expired()) {
      (void)scene_.reportError(choiceError(node_, result.error()));
    }
  }

  void take(std::expected<void, ManagedError> result) {
    if (!result && !lifetime_.expired()) {
      (void)scene_.reportError(result.error());
    }
  }

  std::expected<void, TreeError> backgroundFailed(BackgroundError error) {
    auto failure = choiceError(node_, backgroundCause(error));
    failure.background = error;
    failure.operation = "ComboBox.background";
    (void)scene_.reportError(failure);

    return {};
  }

  void failDomain(SearchError error) {
    if (error == SearchError::CANCELLED || !active()) {
      return;
    }
    auto diagnostic = choiceError(node_, error == SearchError::NO_MEMORY         ? TreeError::NO_MEMORY
                                         : error == SearchError::CALLBACK_FAILED ? TreeError::CALLBACK_FAILED
                                                                                 : TreeError::INVALID_ARGUMENT);
    diagnostic.search = error;
    diagnostic.operation = "ComboBox.provider";
    (void)scene_.reportError(diagnostic);
    if (active() && config_.failed) {
      try {
        config_.failed(error);
      } catch (const std::bad_alloc &) {
        diagnostic.cause = TreeError::NO_MEMORY;
        diagnostic.operation = "ComboBox.failedCallback";
        (void)scene_.reportError(diagnostic);
      } catch (...) {
        diagnostic.cause = TreeError::CALLBACK_FAILED;
        diagnostic.operation = "ComboBox.failedCallback";
        (void)scene_.reportError(diagnostic);
      }
    }
  }

  void stopBrowse() noexcept {
    if (browseLatest_) {
      browseLatest_->cancel();
    }
    if (browseRequest_) {
      browseRequest_->cancel();
    }
    browseRequest_.reset();
    browseCompletion_.reset();
    browsePublication_.reset();
    activation_.reset();
  }

  void updateSource(std::shared_ptr<const FindWorkerSource> source) {
    if (source_ == source) {
      return;
    }
    stopBrowse();
    navigationPending_ = false;
    acceptPending_ = false;
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    selectCompletion_.reset();
    pendingIndex_.reset();
    pendingValue_ = {};
    pendingDraftRevision_.reset();
    source_ = std::move(source);
    key_ = {source_->pin, source_->pin.get(), source_->revision, session_, 1, scene_.widthPolicy()};
    auto record = record_.lock();
    const bool changed = bool(record->selected);
    record->selected = {};
    if (changed) {
      notify({});
    }
    if (!active()) {
      return;
    }
    if (browseRevision_ == std::numeric_limits<std::uint64_t>::max()) {
      take(std::unexpected(TreeError::IDENTITY_EXHAUSTED));
      return;
    }
    if (opened()) {
      pendingBase_ = std::make_shared<const ChoicePage>(++browseRevision_, source_->rowCount);
    }
  }

  std::expected<void, ManagedError> startSelection() try {
    if (!pendingIndex_ || !source_) {
      return {};
    }
    auto executor = SceneAccess::application(scene_).background();
    if (!executor) {
      pendingIndex_.reset();
      return std::unexpected(choiceError(node_, TreeError::CLOSED));
    }
    if (!selectLatest_) {
      selectLatest_ = std::make_unique<BackgroundLatest>(*executor);
    }
    selectPublication_ = std::make_shared<ChoicePublication>();
    auto admitted = selectLatest_->replace(std::make_unique<ChoiceJob>(
      source_, key_, RowRange{*pendingIndex_, 1}, config_.search, config_.browseScratchBytes, connection_.publisher,
      selectGeneration_, std::nullopt, selectPublication_));
    if (!admitted) {
      selectLatest_->cancel();
      selectPublication_.reset();
      pendingIndex_.reset();
      auto failure = choiceError(node_, backgroundCause(admitted.error()));
      failure.background = admitted.error();
      return std::unexpected(failure);
    }
    pendingIndex_.reset();
    return {};
  } catch (const std::bad_alloc &) {
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    pendingIndex_.reset();
    return std::unexpected(choiceError(node_, TreeError::NO_MEMORY));
  } catch (...) {
    if (selectLatest_) {
      selectLatest_->cancel();
    }
    pendingIndex_.reset();
    return std::unexpected(choiceError(node_, TreeError::CALLBACK_FAILED));
  }

  bool selectionDraftCurrent() {
    if (!pendingDraftRevision_) {
      return true;
    }
    auto revision = Access::choiceDraftRevision(scene_, editor_.node());
    if (!revision) {
      take(std::unexpected(revision.error()));
      return false;
    }
    return *revision == *pendingDraftRevision_;
  }

  void finishSavedSelection() {
    if (!pendingValue_) {
      return;
    }
    auto value = std::exchange(pendingValue_, {});
    if (!selectionDraftCurrent()) {
      pendingDraftRevision_.reset();
      return;
    }
    take(accept(std::move(value)));
  }

  void finishSelection() {
    const auto completion = std::exchange(selectCompletion_, {});
    if (!completion) {
      return;
    }
    if (!selectionDraftCurrent()) {
      pendingDraftRevision_.reset();
      return;
    }
    if (completion->error) {
      failDomain(*completion->error);
      return;
    }
    if (!completion->rows || completion->rows->rows.size() != 1) {
      failDomain(SearchError::INVALID_ARGUMENT);
      return;
    }
    const auto &row = completion->rows->rows.front();
    auto selected = std::make_shared<ChoiceValue>();
    selected->source = key_;
    selected->row = row.id;
    selected->index = row.value.rank.sourceOrdinal;
    selected->label = row.value.presentation;
    take(accept(ChoiceAccess::selection(std::move(selected))));
  }

  void finishBrowse() {
    if (!browseCompletion_) {
      return;
    }
    if (browseCompletion_->error) {
      const auto completion = std::exchange(browseCompletion_, {});
      if (completion->request) {
        completion->request->cancel();
      }
      failDomain(*completion->error);
      return;
    }
    auto sent = browseCompletion_->request->complete(browseCompletion_->page);
    if (sent || (sent.error() != SchedulerError::WOULD_BLOCK && sent.error() != SchedulerError::NO_MEMORY)) {
      browseCompletion_.reset();
    }
  }

  void finishActivation() {
    auto row = std::exchange(activation_, {});
    if (!row || !opened() || !queryText_.empty()) {
      return;
    }
    auto value = Access::choiceRow(scene_, list_.node(), *row);
    if (!value) {
      take(std::unexpected(value.error()));
      return;
    }
    if (!*value) {
      return;
    }
    auto selected = std::make_shared<ChoiceValue>();
    selected->source = key_;
    selected->row = (*value)->id;
    selected->index = (*value)->value.rank.sourceOrdinal;
    selected->label = (*value)->value.presentation;
    take(accept(ChoiceAccess::selection(std::move(selected))));
  }

  void serviceLatest() {
    for (auto publication : {browsePublication_, selectPublication_}) {
      if (!publication) {
        continue;
      }
      const auto failure = publication->failure.exchange(0);
      if (failure) {
        auto error = choiceError(node_, TreeError::CLOSED);
        error.async = static_cast<AsyncError>(failure - 1);
        error.operation = "ComboBox.publication";
        (void)scene_.reportError(error);
      }
    }
    for (auto latest : {browseLatest_.get(), selectLatest_.get()}) {
      if (latest) {
        auto result = latest->service();
        if (!result) {
          latest->cancel();
          if (latest == browseLatest_.get()) {
            stopBrowse();
          } else {
            pendingIndex_.reset();
            selectCompletion_.reset();
            selectPublication_.reset();
          }
          (void)backgroundFailed(result.error());
        }
      }
    }
  }

  void cycle(bool prepared) noexcept {
    if (!active()) {
      cancel();
      return;
    }
    try {
      if (prepared) {
        auto captured = binding_->capture(scene_);
        if (!captured) {
          take(std::unexpected(choiceError(node_, captured.error())));
          return;
        }
        updateSource(*captured);
      }
      if (pendingBase_) {
        auto published = browsePublisher_.publish(pendingBase_);
        if (published) {
          pendingBase_.reset();
        }
      }
      if (!eligible()) {
        stopBrowse();
        if (opened()) {
          take(open(false));
        }
        return;
      }
      if (initial_ && source_) {
        auto index = std::exchange(initial_, {});
        take(selectIndex(*index));
      }
      take(startSelection());
      serviceLatest();
      if (!prepared) {
        finishSavedSelection();
        finishSelection();
      }
      if (!active()) {
        return;
      }
      if (std::exchange(autoOpenAfterEdit_, false) && !opened()) {
        take(open(true));
      }
      if (!opened()) {
        stopBrowse();
        return;
      }
      auto query = (readOnly_ ? query_ : editor_).text().get();
      if (!query) {
        take(std::unexpected(query.error()));
        return;
      }
      if (*query != queryText_) {
        queryText_ = std::move(*query);
        stopBrowse();
        take(SceneAccess::application(scene_).tree().setVisible(list_.node(), queryText_.empty()));
        take(SceneAccess::application(scene_).tree().setVisible(search_.node(), !queryText_.empty()));
      }
      finishBrowse();
      finishActivation();
    } catch (const std::bad_alloc &) {
      take(std::unexpected(choiceError(node_, TreeError::NO_MEMORY)));
    } catch (...) {
      take(std::unexpected(choiceError(node_, TreeError::CALLBACK_FAILED)));
    }
  }

  ManagedScene &scene_;
  Node<ComboBox> node_;
  std::weak_ptr<const void> lifetime_;
  std::weak_ptr<ChoiceRecord> record_;
  std::shared_ptr<FindModelSource> binding_;
  ManagedModel<SearchItem> browseModel_, searchModel_;
  Publisher<ModelSnapshot<SearchItem>> browsePublisher_;
  ComboBoxConfig config_;
  Ref<LineEdit> editor_, query_;
  Ref<VirtualList<SearchItem>> list_, search_;
  bool readOnly_{}, cancelled_{}, openPending_{}, autoOpenAfterEdit_{}, navigationPending_{}, acceptPending_{};
  std::string navigationQuery_;
  std::uint64_t navigationSteps_{};
  bool navigationFromEnd_{};
  std::int64_t navigationPages_{};
  SearchSourceIdentity navigationSource_;
  Node<Dialog> popup_;
  std::function<void()> modelLease_;
  std::uint64_t session_{}, browseRevision_{1}, browseGeneration_{}, selectGeneration_{};
  SearchSourceIdentity key_;
  std::shared_ptr<const FindWorkerSource> source_;
  std::string queryText_;
  SchedulerConnection<ChoiceCompletion> connection_;
  std::unique_ptr<BackgroundLatest> browseLatest_, selectLatest_;
  std::shared_ptr<ChoicePublication> browsePublication_, selectPublication_;
  std::shared_ptr<const ChoiceCompletion> browseCompletion_, selectCompletion_;
  std::shared_ptr<const ChoicePage> pendingBase_;
  std::optional<ModelRequest<SearchItem>> browseRequest_;
  std::optional<std::uint64_t> pendingIndex_, initial_;
  std::optional<std::uint64_t> pendingDraftRevision_;
  ChoiceSelection pendingValue_;
  std::optional<RowId> activation_;
};
} // namespace

std::expected<void, ManagedError> Access::configureChoiceList(ManagedScene &scene, Node<VirtualList<SearchItem>> node,
                                                              std::shared_ptr<void> activity,
                                                              std::function<void(RowId)> activated) {
  auto result = SceneAccess::application(scene).tree().edit(node, [&](VirtualList<SearchItem> &list) {
    list.service_ = std::move(activity);
    list.activation_ = std::move(activated);
    list.clickActivates_ = true;
    list.preparedDelegate_ = [](const ModelRow<SearchItem> &row) {
      return row.value.presentation;
    };
  });

  return result ? std::expected<void, ManagedError>{} : std::unexpected(choiceError(node, result.error()));
}

std::expected<std::optional<ModelRow<SearchItem>>, ManagedError>
Access::choiceRow(ManagedScene &scene, Node<VirtualList<SearchItem>> node, RowId id) {
  std::optional<ModelRow<SearchItem>> result;
  auto read = SceneAccess::application(scene).tree().read(node, [&](const VirtualList<SearchItem> &list) {
    if (!list.prepared_ || list.prepared_->range.state() != RangeState::READY) {
      return;
    }
    for (const auto &row : list.prepared_->range.rows()) {
      if (row.id == id) {
        result = row;
        break;
      }
    }
  });

  return read ? std::expected<std::optional<ModelRow<SearchItem>>, ManagedError>(std::move(result))
              : std::unexpected(choiceError(node, read.error()));
}

std::expected<std::optional<RowId>, ManagedError> Access::choiceFirstRow(ManagedScene &scene,
                                                                         Node<VirtualList<SearchItem>> node) {
  std::optional<RowId> result;
  auto read = scene.app_.tree().read(node, [&](const VirtualList<SearchItem> &list) {
    if (!list.preparedSuppressed_ && !list.dirty_ && list.prepared_ &&
        list.prepared_->range.state() == RangeState::READY && !list.prepared_->range.rows().empty()) {
      result = list.prepared_->range.rows().front().id;
    }
  });
  return read ? std::expected<std::optional<RowId>, ManagedError>(result)
              : std::unexpected(choiceError(node, read.error()));
}

std::expected<std::optional<RowId>, ManagedError> Access::choiceNavigationRow(ManagedScene &scene,
                                                                              Node<VirtualList<SearchItem>> node,
                                                                              std::uint64_t steps, bool fromEnd,
                                                                              std::int64_t pages) {
  std::optional<RowId> result;
  bool changed{};
  auto edited = scene.app_.tree().edit(node, [&](VirtualList<SearchItem> &list) {
    if (list.preparedSuppressed_ || !list.prepared_ || !list.prepared_->frame.rowCount()) {
      return;
    }
    const auto last = list.prepared_->frame.rowCount() - 1;
    auto position = fromEnd ? last - std::min(steps, last) : std::min(steps, last);
    const auto visible = list.prepared_->window.visible;
    const auto pageRows = std::max<std::uint64_t>(1, visible.count);
    if (pages > 0) {
      const auto count = static_cast<std::uint64_t>(pages);
      position += count > (last - position) / pageRows ? last - position : count * pageRows;
    } else if (pages < 0) {
      const auto count = static_cast<std::uint64_t>(-pages);
      position -= count > position / pageRows ? position : count * pageRows;
    }
    if (position < visible.first || position - visible.first >= visible.count) {
      if (!list.intent_ || *list.intent_ != position) {
        list.choose(position);
        changed = true;
      }
      return;
    }
    if (list.dirty_ || list.prepared_->range.state() != RangeState::READY) {
      return;
    }
    const auto range = list.prepared_->range.range();
    if (position >= range.first && position - range.first < list.prepared_->range.rows().size()) {
      result = list.prepared_->range.rows()[static_cast<std::size_t>(position - range.first)].id;
    }
  });
  if (!edited) {
    return std::unexpected(choiceError(node, edited.error()));
  }
  if (changed) {
    auto invalidated = scene.app_.tree().invalidate(node, Invalidation::LAYOUT);
    if (!invalidated) {
      return std::unexpected(choiceError(node, invalidated.error()));
    }
  }
  return result;
}

std::expected<std::optional<std::uint64_t>, ManagedError>
Access::choiceMoveNavigation(ManagedScene &scene, Node<VirtualList<SearchItem>> node, portals::Action action,
                             int pageDirection) {
  std::optional<std::uint64_t> result;
  auto edited = scene.app_.tree().edit(node, [&](VirtualList<SearchItem> &list) {
    if (!list.preparedSuppressed_ && list.action(action) == EventReply::HANDLED) {
      result = list.intent_;
      if (pageDirection && result) {
        const auto last = list.prepared_->frame.rowCount() - 1;
        const auto extra = std::max<std::uint64_t>(1, list.prepared_->window.visible.count) - 1;
        *result = pageDirection > 0 ? *result + std::min(extra, last - *result) : *result - std::min(extra, *result);
        list.choose(*result);
      }
    }
  });
  if (!edited) {
    return std::unexpected(choiceError(node, edited.error()));
  }
  if (result) {
    auto invalidated = scene.app_.tree().invalidate(node, Invalidation::LAYOUT);
    if (!invalidated) {
      return std::unexpected(choiceError(node, invalidated.error()));
    }
  }
  return result;
}

std::expected<void, ManagedError> Access::choiceClickActivation(ManagedScene &scene,
                                                                Node<VirtualList<SearchItem>> node) {
  auto edited = scene.app_.tree().edit(node, [](VirtualList<SearchItem> &list) {
    list.clickActivates_ = true;
  });
  return edited ? std::expected<void, ManagedError>{} : std::unexpected(choiceError(node, edited.error()));
}

std::expected<std::uint64_t, ManagedError> Access::choiceDraftRevision(ManagedScene &scene, Node<LineEdit> node) {
  auto entry = scene.find(node);
  auto binding = entry ? std::get_if<LineEditBinding>(&entry->binding) : nullptr;
  auto revision = binding ? binding->draftRevision() : std::unexpected(TreeError::INVALID_ARGUMENT);
  return revision ? std::expected<std::uint64_t, ManagedError>(*revision)
                  : std::unexpected(choiceError(node, revision.error()));
}

std::expected<void, ManagedError> Access::bindChoiceTrigger(ManagedScene &scene, Node<Button> node,
                                                            std::function<std::expected<void, ManagedError>()> action) {
  auto entry = scene.bindingEntry(node);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  auto bound = scene.app_.router().bind(
    node,
    [&scene, action = std::move(action)](Button &, const RoutedEvent &event) -> EventReply {
      const auto *mouse = std::get_if<MouseInputEvent>(&event);
      const auto *command = std::get_if<ActionEvent>(&event);
      if ((!mouse || mouse->button != MouseButton::LEFT || mouse->action != MouseAction::PRESS) &&
          (!command || command->action != portals::Action::ACTIVATE)) {
        return EventReply::IGNORED;
      }
      auto result = action();
      if (!result) {
        (void)scene.reportError(result.error());
      }
      return EventReply::HANDLED;
    },
    false);
  if (!bound) {
    return std::unexpected(choiceError(node, bound.error()));
  }
  (*entry)->binding.emplace<ActionBinding>(std::move(*bound));
  return {};
}

std::expected<Node<OverlayHost>, ManagedError> Access::choiceLayerHost(ManagedScene &scene, Node<Widget> root) {
  const auto &host = scene.dialogs_;
  if (!host || host->retired || !host->ready ||
      (host->root != root && !choiceWithin(scene, root, host->root) && !choiceWithin(scene, host->body, root))) {
    return std::unexpected(choiceError(root, TreeError::INVALID_ARGUMENT));
  }

  return host->root;
}

std::expected<void, ManagedError> Access::claimChoicePopup(ManagedScene &scene,
                                                           std::shared_ptr<SceneActivity> activity) {
  const auto host = scene.dialogs_;
  if (!host || host->retired || !host->ready) {
    return std::unexpected(choiceError({}, TreeError::INVALID_ARGUMENT));
  }
  if (auto previous = host->choiceOwner.lock(); previous && previous != activity) {
    auto dismissed = previous->dismissPopup();
    if (!dismissed) {
      return dismissed;
    }
  }
  host->choiceOwner = std::move(activity);
  return {};
}

bool Access::choicePopupOpened(ManagedScene &scene, Node<Dialog> popup) {
  auto record = scene.findDialog(popup);
  return record && record->open;
}

namespace {
struct ChoiceModels {
  ManagedScene &scene;
  std::weak_ptr<const void> lifetime;
  ManagedModel<SearchItem> browse, search;
  ManagedState<std::string> query;

  ~ChoiceModels() {
    if (!lifetime.expired()) {
      Access::retireChoiceModel(scene, browse);
      Access::retireChoiceModel(scene, search);
      Access::retireChoiceState(scene, query);
    }
  }
};

std::expected<void, ManagedError>
mountChoicePopup(ManagedScene &scene, Node<OverlayHost> host, Node<ComboBox> root, std::shared_ptr<ChoiceRecord> record,
                 std::shared_ptr<FindModelSource> binding, ComboBoxConfig config, Ref<LineEdit> editor,
                 std::shared_ptr<std::weak_ptr<ChoiceActivity>> bridge, ControlContext controls, bool readOnly) {
  static constexpr std::size_t BROWSE_OVERSCAN = 2;
  Ref<LineEdit> query;
  Ref<VirtualList<SearchItem>> list, search;
  auto models = std::make_shared<ChoiceModels>(scene, Access::lifetime(scene));
  auto popupRecipe = Access::cold<Dialog>([&, config, models, query, list, search](
                                            ManagedScene &owner, Node<Widget> parent, MountContext &context) mutable
                                            -> std::expected<Node<Dialog>, ManagedError> {
    auto popup = owner.emplace<Dialog>(
      parent, DialogOptions{
                .mode = DialogMode::NON_MODAL,
                .placement = DialogPlacement::BELOW_START,
                .maximumPanelSize = {config.popupWidth, config.popupRows + 2 + (readOnly && config.searchable ? 1 : 0)},
                .viewportMargin = 0,
                .anchorGap = 0});
    if (!popup) {
      return popup;
    }
    if (context.root == Node<Widget>{}) {
      context.root = *popup;
    }
    auto browseModel = owner.model<SearchItem>(std::make_shared<const ChoicePage>(1, 0),
                                               ModelOptions{.max_range_rows = config.pageRows});
    if (!browseModel) {
      return std::unexpected(browseModel.error());
    }
    models->browse = *browseModel;
    auto searchModel = owner.model<SearchItem>(std::make_shared<const ChoicePage>(1, 0));
    if (!searchModel) {
      return std::unexpected(searchModel.error());
    }
    models->search = *searchModel;
    auto queryState = owner.state(std::string{});
    if (!queryState) {
      return std::unexpected(queryState.error());
    }
    models->query = *queryState;
    auto browseRecipe =
      Access::configuredControl(
        Access::cold<VirtualList<SearchItem>>(
          [model = *browseModel, bridge](ManagedScene &owner, Node<Widget> parent,
                                         MountContext &) -> std::expected<Node<VirtualList<SearchItem>>, ManagedError> {
            return owner.virtualList<SearchItem>(
              parent, model,
              [](const ModelRow<SearchItem> &row) {
                return row.value.text();
              },
              VirtualListOptions{.overscan_rows = BROWSE_OVERSCAN, .width_policy = owner.widthPolicy()}, {},
              [bridge](const ModelFrame<SearchItem> &, const std::optional<ModelRequest<SearchItem>> &request) {
                if (auto activity = bridge->lock()) {
                  return activity->browse(request);
                }
                if (request) {
                  request->cancel();
                }
                return std::expected<void, TreeError>{};
              });
          }),
        "VirtualList")
        .reference(list)
        .height(config.popupRows)
        .classes("choice-list")
        .setup([record](ManagedScene &, Node<VirtualList<SearchItem>> node) -> std::expected<void, ManagedError> {
          record->browse = node;
          return {};
        });
    SuggestionsConfig suggestionsConfig;
    suggestionsConfig.search = config.search;
    suggestionsConfig.resultModel = *searchModel;
    suggestionsConfig.activated = [bridge](SearchSelection selected) {
      if (auto activity = bridge->lock()) {
        activity->searchActivated(std::move(selected));
      }
    };
    suggestionsConfig.failed = [bridge](SearchError error) {
      if (auto activity = bridge->lock()) {
        activity->providerFailed(error);
      }
    };
    suggestionsConfig.enabled = [bridge] {
      auto activity = bridge->lock();
      return activity && activity->searching();
    };
    auto searchRecipe =
      virtualList(search, SuggestionsRecipe{(readOnly ? query : editor).draftText(), binding},
                  std::move(suggestionsConfig))
        .height(config.popupRows)
        .visible(false)
        .classes("choice-completion")
        .setup([record](ManagedScene &, Node<VirtualList<SearchItem>> node) -> std::expected<void, ManagedError> {
          record->search = node;
          return {};
        });
    auto content = column(lineEdit(query, *queryState, LineEditConfig{.options = {.limits = {4096, 4096, 4096, 8192}}})
                            .visible(readOnly && config.searchable)
                            .classes("choice-search"),
                          std::move(browseRecipe), std::move(searchRecipe));
    auto panel = Access::dialogPanel(owner, *popup, {});
    if (!panel) {
      return std::unexpected(panel.error());
    }
    auto body = Access::mount(std::move(content), owner, *panel, context);
    if (!body) {
      return std::unexpected(body.error());
    }
    auto bound = owner.dialog(*popup, *panel, *body);
    if (!bound) {
      return std::unexpected(bound.error());
    }
    auto styled = owner.style(*popup, StyleTarget{.type = "Dialog", .classes = {"choice-popup"}});
    if (!styled) {
      return std::unexpected(styled.error());
    }
    context.finalize.push_back([&owner, record, bridge] {
      auto current = bridge->lock();
      auto configured = Access::configureChoiceList(owner, record->browse, current, [bridge](RowId row) {
        if (auto activity = bridge->lock()) {
          activity->activate(row);
        }
      });
      return configured ? Access::choiceClickActivation(owner, record->search) : configured;
    });
    return popup;
  });
  auto popup = mountAt(scene, host, std::move(popupRecipe), std::move(controls));
  if (!popup) {
    return std::unexpected(popup.error());
  }
  const auto fail = [&](ManagedError error) -> std::expected<void, ManagedError> {
    auto removed = scene.remove(*popup);
    if (!removed) {
      error.cleanup = std::get<TreeError>(removed.error().cause);
    }
    return std::unexpected(std::move(error));
  };
  auto anchored = scene.dialogAnchor(*popup, root, DialogPlacement::BELOW_START);
  if (!anchored) {
    return fail(anchored.error());
  }
  auto companion = Access::choiceCompanion(scene, root, *popup);
  if (!companion) {
    return fail(companion.error());
  }
  auto publisher = models->browse.publisher();
  if (!publisher) {
    return fail(choiceError(root, publisher.error()));
  }
  auto activity = bridge->lock();
  if (!activity) {
    return fail(choiceError(root, TreeError::CLOSED));
  }
  record->popup = *popup;
  std::function<void()> lease = [models] {
  };
  activity->attachPopup(models->browse, models->search, std::move(*publisher), query, list, search, *popup,
                        std::move(lease));
  return {};
}

template <class T> Description<T> makeChoiceRecipe(std::shared_ptr<FindModelSource> source, ComboBoxConfig config) {
  auto binding = std::make_shared<AttachedChoiceSource>(std::move(source));
  auto recipe = Access::cold<T>([binding = std::move(binding), config = std::move(config)](
                                  ManagedScene &scene, Node<Widget> parent,
                                  MountContext &context) mutable -> std::expected<Node<T>, ManagedError> {
    constexpr bool READ_ONLY = std::same_as<T, DropDown>;
    constexpr std::size_t MAX_PAGE_ROWS = 256, BROWSE_OVERSCAN = 2;

    if (context.controlSettings && context.controlSettings->searchStrategy) {
      config.search.strategy = *context.controlSettings->searchStrategy;
    }

    if (!config.pageRows || config.pageRows > MAX_PAGE_ROWS || !config.browseScratchBytes || config.popupRows <= 0 ||
        config.popupWidth <= 0 || static_cast<std::size_t>(config.popupRows) + 2 * BROWSE_OVERSCAN > config.pageRows ||
        !portals::detail::validSearchOptions(config.search) ||
        config.editor.options.displayMode != LineEditDisplayMode::NORMAL) {
      return std::unexpected(choiceError(parent, TreeError::INVALID_ARGUMENT));
    }
    Access::choiceModels(scene, context);
    auto allowed = SceneAccess::requireBackground(scene);
    if (!allowed) {
      return std::unexpected(allowed.error());
    }
    auto attached = binding->attach(scene);
    if (!attached) {
      return std::unexpected(attached.error());
    }
    auto root = scene.emplace<T>(parent);
    if (!root) {
      return root;
    }
    if (context.root == Node<Widget>{}) {
      context.root = *root;
    }
    std::shared_ptr<ChoiceRecord> record;
    auto &app = SceneAccess::application(scene);
    auto read = app.tree().read(*root, [&](const T &widget) {
      record = ChoiceAccess::record(widget);
    });
    if (!read) {
      return std::unexpected(choiceError(*root, read.error()));
    }
    Ref<LineEdit> editor;
    auto bridge = std::make_shared<std::weak_ptr<ChoiceActivity>>();
    auto editorConfig = config.editor;
    editorConfig.options.readOnly = READ_ONLY;
    if (context.controlSettings) {
      auto options =
        controlLineEditOptions(*context.controlSettings, editorConfig.options, scene.widthPolicy(), {}, *root);
      if (!options) {
        return std::unexpected(options.error());
      }
      editorConfig.options = std::move(*options);
      editorConfig.options.readOnly = READ_ONLY;
    }
    // Completion projects the draft to a worker; protected presentation must never enter that path.
    if (editorConfig.options.displayMode != LineEditDisplayMode::NORMAL) {
      return std::unexpected(choiceError(*root, TreeError::INVALID_ARGUMENT));
    }
    const auto initial =
      context.controlSettings && context.controlSettings->text ? *context.controlSettings->text : TextValue("");
    auto input = Access::mount(lineEdit(editor, initial, std::move(editorConfig)).flexX().classes("choice-editor"),
                               scene, *root, context);
    if (!input) {
      return std::unexpected(input.error());
    }
    record->editor = *input;
    auto trigger = Access::mount(button("v").classes("choice-trigger"), scene, *root, context);
    if (!trigger) {
      return std::unexpected(trigger.error());
    }
    context.finalize.push_back([&scene, trigger = *trigger, bridge]() -> std::expected<void, ManagedError> {
      return Access::bindChoiceTrigger(scene, trigger, [bridge]() -> std::expected<void, ManagedError> {
        if (auto activity = bridge->lock()) {
          return activity->open(!activity->opened());
        }
        return std::unexpected(choiceError({}, TreeError::CLOSED));
      });
    });
    record->mountPopup = [&scene, root = Node<ComboBox>(*root), weak = std::weak_ptr(record), binding, config, editor,
                          bridge, controls = context.controls](Node<OverlayHost> host) {
      auto record = weak.lock();
      return record ? mountChoicePopup(scene, host, root, record, binding, config, editor, bridge, controls, READ_ONLY)
                    : std::expected<void, ManagedError>(std::unexpected(choiceError(root, TreeError::CLOSED)));
    };
    context.finalize.push_back([&scene, root = Node<ComboBox>(*root), record, binding, config = std::move(config),
                                editor, bridge, READ_ONLY]() mutable -> std::expected<void, ManagedError> {
      const auto session = issueChoiceSession();
      if (!session) {
        return std::unexpected(choiceError(root, TreeError::IDENTITY_EXHAUSTED));
      }
      auto host = Access::choiceLayerHost(scene, root);
      if (!host) {
        return std::unexpected(host.error());
      }
      auto activity =
        std::make_shared<ChoiceActivity>(scene, root, record, binding, std::move(config), editor, READ_ONLY, session);
      *bridge = activity;
      record->activity = activity;
      record->open = [weak = std::weak_ptr(activity)](bool open) -> std::expected<void, ManagedError> {
        if (auto current = weak.lock()) {
          return current->open(open);
        }
        return std::unexpected(choiceError({}, TreeError::CLOSED));
      };
      record->selectIndex = [weak = std::weak_ptr(activity)](std::uint64_t index) -> std::expected<void, ManagedError> {
        if (auto current = weak.lock()) {
          return current->selectIndex(index);
        }
        return std::unexpected(choiceError({}, TreeError::CLOSED));
      };
      record->selectValue = [weak =
                               std::weak_ptr(activity)](ChoiceSelection value) -> std::expected<void, ManagedError> {
        if (auto current = weak.lock()) {
          return current->selectValue(std::move(value));
        }
        return std::unexpected(choiceError({}, TreeError::CLOSED));
      };
      record->clear = [weak = std::weak_ptr(activity)]() -> std::expected<void, ManagedError> {
        if (auto current = weak.lock()) {
          return current->clear();
        }
        return std::unexpected(choiceError({}, TreeError::CLOSED));
      };
      auto connected = activity->connect();
      if (!connected) {
        return connected;
      }
      return SceneAccess::admitActivity(scene, std::move(activity));
    });
    return root;
  });
  return Access::choiceHost(
           Access::configuredControl(std::move(recipe), std::same_as<T, DropDown> ? "DropDown" : "ComboBox"))
    .classes(std::same_as<T, DropDown> ? "drop-down" : "combo-box");
}
} // namespace

Description<ComboBox> comboBoxRecipe(std::shared_ptr<FindModelSource> source, ComboBoxConfig config) {
  return makeChoiceRecipe<ComboBox>(std::move(source), std::move(config));
}

Description<DropDown> dropDownRecipe(std::shared_ptr<FindModelSource> source, ComboBoxConfig config) {
  return makeChoiceRecipe<DropDown>(std::move(source), std::move(config));
}

std::shared_ptr<FindModelSource> choiceStrings(std::initializer_list<std::string_view> values) {
  static constexpr std::size_t MAX_ROWS = 4096, MAX_LABEL_BYTES = 4096;
  std::expected<RowCollection<std::string>, TreeError> rows = std::unexpected(TreeError::NODE_LIMIT);
  if (values.size() <= MAX_ROWS) {
    std::vector<ModelRow<std::string>> owned;
    owned.reserve(values.size());
    for (auto value : values) {
      if (value.size() > MAX_LABEL_BYTES) {
        break;
      }
      owned.push_back({{owned.size() + 1, 1}, std::string(value)});
    }
    if (owned.size() == values.size()) {
      rows = RowCollection<std::string>::from(owned);
    }
  }
  auto project = [](const std::string &text) -> std::string_view {
    return text;
  };
  return std::make_shared<FindCollectionSourceOf<std::string, decltype(project)>>(std::move(rows), project);
}

namespace {
std::expected<std::shared_ptr<ChoiceRecord>, ManagedError> refChoiceRecord(const std::shared_ptr<RefTarget> &target) {
  auto scene = eventScene(target);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  std::shared_ptr<ChoiceRecord> record;
  auto checked = SceneAccess::application(**scene).tree().read(target->node, [&](const Widget &widget) {
    if (auto *choice = dynamic_cast<const ComboBox *>(&widget)) {
      record = ChoiceAccess::record(*choice);
    }
  });
  if (!checked || !record || !record->active) {
    return std::unexpected(choiceError(target->node, !checked ? checked.error() : TreeError::INVALID_ARGUMENT));
  }

  return record;
}

template <class Operation>
std::expected<void, ManagedError> choiceCommand(const std::shared_ptr<RefTarget> &target, Operation operation,
                                                std::source_location origin) {
  std::expected<void, ManagedError> result;
  try {
    auto record = refChoiceRecord(target);
    result = record ? operation(**record) : std::unexpected(record.error());
  } catch (const std::bad_alloc &) {
    result = std::unexpected(choiceError({}, TreeError::NO_MEMORY));
  } catch (...) {
    result = std::unexpected(choiceError({}, TreeError::CALLBACK_FAILED));
  }
  if (!result) {
    reportEventError(target, result.error(), origin);
  }

  return result;
}
} // namespace

Ref<LineEdit> choiceEditor(const std::shared_ptr<RefTarget> &target) {
  Ref<LineEdit> reference;
  auto record = refChoiceRecord(target);
  auto scene = eventScene(target);
  auto published = record ? NamedAccess::publish(reference, **scene, (*record)->editor)
                          : std::expected<void, ManagedError>(std::unexpected(record.error()));
  if (!published) {
    NamedAccess::reject(reference, published.error());
  }

  return reference;
}

std::expected<ChoiceSelection, ManagedError> choiceSelection(const std::shared_ptr<RefTarget> &target) {
  auto record = refChoiceRecord(target);
  return record ? std::expected<ChoiceSelection, ManagedError>((*record)->selected) : std::unexpected(record.error());
}

std::expected<bool, ManagedError> choiceOpened(const std::shared_ptr<RefTarget> &target) {
  auto record = refChoiceRecord(target);
  if (!record) {
    return std::unexpected(record.error());
  }
  if ((*record)->popup == Node<Dialog>{}) {
    return false;
  }
  return target->scene->dialogOpened((*record)->popup);
}

std::expected<void, ManagedError> choiceOpen(const std::shared_ptr<RefTarget> &target, bool open,
                                             std::source_location origin) {
  return choiceCommand(
    target,
    [open](ChoiceRecord &record) {
      return record.open(open);
    },
    origin);
}

std::expected<void, ManagedError> choiceSelect(const std::shared_ptr<RefTarget> &target, std::uint64_t index,
                                               std::source_location origin) {
  return choiceCommand(
    target,
    [index](ChoiceRecord &record) {
      return record.selectIndex(index);
    },
    origin);
}

std::expected<void, ManagedError> choiceSelect(const std::shared_ptr<RefTarget> &target, ChoiceSelection value,
                                               std::source_location origin) {
  return choiceCommand(
    target,
    [value = std::move(value)](ChoiceRecord &record) mutable {
      return record.selectValue(std::move(value));
    },
    origin);
}

std::expected<void, ManagedError> choiceClear(const std::shared_ptr<RefTarget> &target, std::source_location origin) {
  return choiceCommand(
    target,
    [](ChoiceRecord &record) -> std::expected<void, ManagedError> {
      return record.clear();
    },
    origin);
}

std::expected<void, ManagedError> choiceSelectHandler(const std::shared_ptr<RefTarget> &target,
                                                      std::function<void(ChoiceSelection)> callback,
                                                      std::source_location origin) {
  static constexpr std::size_t MAX_HANDLERS = 64;
  return choiceCommand(
    target,
    [callback = std::move(callback)](ChoiceRecord &record) mutable -> std::expected<void, ManagedError> {
      if (record.handlers.size() >= MAX_HANDLERS) {
        return std::unexpected(choiceError({}, TreeError::NODE_LIMIT));
      }
      record.handlers.push_back(std::make_shared<std::function<void(ChoiceSelection)>>(std::move(callback)));
      return {};
    },
    origin);
}
} // namespace portals::ui::detail
