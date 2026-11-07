// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_state.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <portals/ui.hpp>

namespace portals::ui::detail {
struct DocumentSourceData {
  SourceCredit credit;
  std::shared_ptr<ByteDocumentSource> source;
  std::shared_ptr<ByteDocumentProvider> provider;
  std::shared_ptr<SourceBudget> budget;
  DocumentSourceOptions options;
};

struct DocumentSourceAccess {
  static DocumentSource make(std::shared_ptr<ByteDocumentSource> source, std::optional<DocumentError> error = {}) {
    DocumentSource descriptor;
    descriptor.error_ = error;
    if (source) {
      auto credit = source->budget()->reserve({4096, 0, 0, 0});
      if (!credit) {
        descriptor.error_ = DocumentError{credit.error(), {}, {}};
        return descriptor;
      }
      descriptor.data_ = std::make_shared<DocumentSourceData>();
      descriptor.data_->credit = std::move(*credit);
      descriptor.data_->budget = source->budget();
      descriptor.data_->source = std::move(source);
    }
    return descriptor;
  }

  static DocumentSource provider(std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget,
                                 DocumentSourceOptions options) {
    DocumentSource descriptor;
    auto credit = budget->reserve({4096, 0, 0, 0});
    if (!credit) {
      descriptor.error_ = DocumentError{credit.error(), {}, {}};
      return descriptor;
    }
    descriptor.data_ = std::make_shared<DocumentSourceData>();
    descriptor.data_->credit = std::move(*credit);
    descriptor.data_->provider = std::move(provider);
    descriptor.data_->budget = std::move(budget);
    descriptor.data_->options = options;
    return descriptor;
  }

  static std::optional<DocumentError> error(const DocumentSource &source) noexcept {
    return source.error_;
  }

  static std::expected<std::shared_ptr<ByteDocumentSource>, DocumentError> resolve(const DocumentSource &descriptor) {
    if (!descriptor.data_) {
      return std::unexpected(descriptor.error_.value_or(DocumentError{SourceError::INVALID_ARGUMENT, {}, {}}));
    }
    auto &data = *descriptor.data_;
    if (!data.source) {
      auto created = ByteDocumentSource::create(data.provider, data.budget, data.options);
      if (!created) {
        return std::unexpected(created.error());
      }
      data.source = *created;
    }
    return data.source;
  }
};
} // namespace portals::ui::detail

namespace portals::detail {
namespace {
constexpr std::size_t VIEW_METADATA_BYTES = 64 * 1024;
constexpr std::size_t BOOKMARK_COUNT = 16;

std::optional<std::int64_t> checkedRowSum(std::int64_t first, std::int64_t second) noexcept {
  constexpr auto MIN = std::numeric_limits<std::int64_t>::min();
  constexpr auto MAX = std::numeric_limits<std::int64_t>::max();

  if ((second > 0 && first > MAX - second) || (second < 0 && first < MIN - second)) {
    return std::nullopt;
  }

  return first + second;
}

ManagedError documentFailure(Node<DocumentView> node, DocumentError error) {
  const auto bridge = error.code == SourceError::NO_MEMORY      ? TreeError::NO_MEMORY
                      : error.code == SourceError::WRONG_THREAD ? TreeError::WRONG_THREAD
                      : error.code == SourceError::CLOSED       ? TreeError::CLOSED
                      : error.code == SourceError::WOULD_BLOCK  ? TreeError::WOULD_BLOCK
                                                                : TreeError::INVALID_ARGUMENT;
  ManagedError failure{ManagedPhase::PREPARE, node.id(), bridge, {}, {}};
  failure.source = error.code;
  failure.logFile = error.fileFailure;
  failure.operation = "DocumentView source preparation";
  return failure;
}

AsyncError documentAsync(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::CLOSED:
    return AsyncError::CLOSED;
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  default:
    return AsyncError::CANCELLED;
  }
}

bool validConfig(const DocumentViewConfig &config) noexcept {
  constexpr auto SAFE_LIMIT = std::numeric_limits<std::size_t>::max() / 1024;
  const auto validEncoding = [](DocumentEncoding encoding) {
    return encoding >= DocumentEncoding::UNRESOLVED && encoding <= DocumentEncoding::WINDOWS1252;
  };
  return validEncoding(config.decoder.explicitEncoding) && validEncoding(config.decoder.hint) &&
         (config.decoder.policy == DocumentDecodePolicy::REPLACE ||
          config.decoder.policy == DocumentDecodePolicy::STRICT_MODE) &&
         config.maxFrameCells && config.maxFrameCells <= SAFE_LIMIT && config.maxFrameBytes &&
         config.maxFrameBytes <= SAFE_LIMIT && config.workBytes && config.workBytes <= SAFE_LIMIT &&
         config.decoder.workLimit && config.decoder.policy != DocumentDecodePolicy::ESCAPE;
}

class DocumentJob final : public BackgroundJob {
public:
  DocumentJob(std::shared_ptr<ByteDocumentSource> source, DocumentPrepareIntent intent,
              std::shared_ptr<const DocumentScan> previous, std::uint64_t generation, SourceCredit credit,
              Publisher<UiDeliveryEnvelope> publisher)
      : source_(std::move(source)), intent_(intent), previous_(std::move(previous)), generation_(generation),
        credit_(std::move(credit)), publisher_(std::move(publisher)) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      completion_ = prepareDocument(source_, intent_, std::move(previous_), generation_, std::move(credit_), stop);
    } catch (...) {
      // The precharged result envelope itself may fail to allocate. Publication
      // uses the preallocated fallback so the UI does not remain busy forever.
      fallback_->error = DocumentError{SourceError::NO_MEMORY, {}, {}};
      fallback_->generation = generation_;
      fallback_->credit = std::move(credit_);
      completion_ = fallback_;
    }
    previous_.reset();
  }

  std::expected<void, AsyncError> publish() override {
    if (stopToken().stop_requested()) {
      return std::unexpected(AsyncError::CANCELLED);
    }
    auto posted = postUiDelivery(publisher_, completion_, false);
    return posted ? std::expected<void, AsyncError>{} : std::unexpected(documentAsync(posted.error()));
  }

private:
  std::shared_ptr<ByteDocumentSource> source_;
  DocumentPrepareIntent intent_;
  std::shared_ptr<const DocumentScan> previous_;
  std::uint64_t generation_;
  SourceCredit credit_;
  Publisher<UiDeliveryEnvelope> publisher_;
  std::shared_ptr<DocumentPreparation> fallback_ = std::make_shared<DocumentPreparation>();
  std::shared_ptr<const DocumentPreparation> completion_;
};

struct DocumentBookmark {
  std::weak_ptr<ByteDocumentSource> source;
  std::uint64_t anchor{};
  std::optional<DocumentLayoutCheckpoint> checkpoint;
  DocumentDecoderOptions decoder;
  std::int32_t width{};
  bool wrap{};
};

class DocumentActivity final : public ui::detail::SceneActivity, public std::enable_shared_from_this<DocumentActivity> {
public:
  DocumentActivity(ManagedScene &scene, Node<DocumentView> node, std::weak_ptr<const void> lifetime,
                   std::weak_ptr<DocumentViewState> state, SourceCredit credit)
      : credit_(std::move(credit)), scene_(scene), app_(ui::detail::SceneAccess::application(scene)), node_(node),
        lifetime_(std::move(lifetime)), state_(std::move(state)) {
  }

  ~DocumentActivity() override {
    cancel();
  }

  bool active() const noexcept override {
    return !closed_ && !lifetime_.expired() && !state_.expired();
  }

  std::expected<void, ManagedError> connect(const std::shared_ptr<SourceBudget> &budget) {
    auto connection = app_.tree().connect<UiDeliveryEnvelope>(
      node_, [weak = weak_from_this()](Widget &, std::shared_ptr<const UiDeliveryEnvelope> envelope) {
        if (auto activity = weak.lock(); activity && activity->active()) {
          const auto result = std::static_pointer_cast<const DocumentPreparation>(envelope->payload);
          if (result && result->generation == activity->generation_) {
            activity->completion_ = result;
            (void)activity->app_.tree().invalidate(activity->node_, Invalidation::LAYOUT);
          }
        }
        envelope->complete({});
      });
    if (!connection) {
      return std::unexpected(documentFailure(node_, {SourceError::NO_MEMORY, {}, {}}));
    }
    auto wake = app_.tree().connect<int>(node_, [](Widget &, std::shared_ptr<const int>) {
    });
    if (!wake) {
      return std::unexpected(documentFailure(node_, {SourceError::NO_MEMORY, {}, {}}));
    }
    auto observer = budget->observeCapacity(wake->publisher);
    if (!observer) {
      return std::unexpected(documentFailure(node_, {observer.error(), {}, {}}));
    }
    connection_ = std::move(*connection);
    capacityConnection_ = std::move(*wake);
    capacity_ = std::move(*observer);
    return {};
  }

  void cancel() noexcept override {
    if (closed_) {
      return;
    }
    closed_ = true;
    capacity_.disarm();
    capacity_ = {};
    (void)capacityConnection_.subscription.disconnect();
    (void)connection_.subscription.disconnect();
    if (latest_) {
      latest_->cancel();
    }
    completion_.reset();
    scan_.reset();
  }

  void service() noexcept override {
    if (!active()) {
      cancel();
      return;
    }
    if (capacity_.takeReady()) {
      capacityBlocked_ = false;
      if (auto state = state_.lock()) {
        state->status.backpressured = false;
      }
      (void)app_.tree().invalidate(node_, Invalidation::LAYOUT);
    }
    if (latest_) {
      auto result = latest_->service();
      if (!result) {
        fail(
          {result.error() == BackgroundError::NO_MEMORY ? SourceError::NO_MEMORY : SourceError::CONTEXT_ERROR, {}, {}});
      }
    }
  }

  void layoutPrepared(const LayoutSnapshot &layout) noexcept override {
    if (!active()) {
      cancel();
      return;
    }
    try {
      prepare(layout);
    } catch (const std::bad_alloc &) {
      fail({SourceError::NO_MEMORY, {}, {}});
    } catch (...) {
      fail({SourceError::CALLBACK_FAILED, {}, {}});
    }
  }

  std::expected<bool, TreeError> input(const InputEvent &event) override {
    auto state = state_.lock();
    if (!active() || !state || !visible_) {
      return false;
    }
    bool focused{};
    bool enabled{};
    bool arranged{};
    CellSize inputViewport;
    ScrollAreaOptions navigation;
    auto read = app_.tree().read(node_, [&](const DocumentView &widget) {
      const auto &scroll = ScrollWidgetAccess::state(widget);
      focused = scroll.focused;
      enabled = scroll.enabled;
      arranged = scroll.arranged;
      inputViewport = scroll.geometry.viewport;
      navigation = scroll.area;
    });
    if (!read) {
      return std::unexpected(read.error());
    }

    if (!enabled || navigation.readOnly) {
      return false;
    }
    const auto *key = std::get_if<KeyEvent>(&event);
    const auto *mouse = std::get_if<MouseInputEvent>(&event);

    if (mouse && (mouse->action == MouseAction::PRESS || mouse->action == MouseAction::RELEASE)) {
      dragByteRange_.reset();
    }

    std::int64_t delta{};
    bool recognized{};
    bool absolute{};
    if (key && focused && key->action != KeyAction::RELEASE && !key->modifiers.alt && !key->modifiers.super &&
        !key->modifiers.shift && (!key->modifiers.control || key->key == InputKey::HOME || key->key == InputKey::END)) {
      const auto height = static_cast<std::uint64_t>(viewport_.height);
      const auto page =
        static_cast<std::int64_t>(height > navigation.pageOverlap ? height - navigation.pageOverlap : 1);
      const auto step = static_cast<std::int64_t>(
        std::min<std::uint64_t>(navigation.keyStep, std::numeric_limits<std::int64_t>::max()));
      switch (key->key) {
      case InputKey::UP:
        delta = -step;
        break;
      case InputKey::DOWN:
        delta = step;
        break;
      case InputKey::PAGE_UP:
        delta = -page;
        break;
      case InputKey::PAGE_DOWN:
        delta = page;
        break;
      case InputKey::HOME:
        absolute = true;
        state->anchor = 0;
        state->rowDelta = 0;
        state->tail = false;
        recognized = true;
        break;
      case InputKey::END:
        absolute = true;
        state->rowDelta = 0;
        state->tail = true;
        recognized = true;
        break;
      case InputKey::LEFT:
        if (state->config.wrap) {
          return false;
        }
        state->horizontal = state->horizontal ? state->horizontal - 1 : 0;
        recognized = true;
        break;
      case InputKey::RIGHT:
        if (state->config.wrap) {
          return false;
        }
        if (state->horizontal <
            std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(viewport_.width)) {
          ++state->horizontal;
        }
        recognized = true;
        break;
      case InputKey::F5:
        recognized = true;
        break;
      default:
        break;
      }
    }
    if (mouse && mouse->action == MouseAction::SCROLL && !mouse->modifiers.alt && !mouse->modifiers.control &&
        !mouse->modifiers.shift && !mouse->modifiers.super) {
      auto node = app_.tree().inspect(node_);
      if (!node) {
        return std::unexpected(node.error());
      }
      if (node->geometry.clip.contains({mouse->column, mouse->row}) && std::isfinite(mouse->wheelDelta) &&
          mouse->wheelDelta > 0 &&
          mouse->wheelDelta <= std::numeric_limits<std::int32_t>::max() / navigation.wheelStep) {
        const auto step = static_cast<std::int64_t>(std::ceil(mouse->wheelDelta * navigation.wheelStep));
        delta = mouse->button == MouseButton::WHEEL_UP ? -step : mouse->button == MouseButton::WHEEL_DOWN ? step : 0;
      }
    }

    if (delta) {
      const bool sameTarget = navigationSerial_ == state->serial;
      const auto pending = checkedRowSum(sameTarget ? pendingRows_ : 0, delta);
      const auto outstanding = pending ? checkedRowSum(*pending, sameTarget ? inFlightRows_ : 0) : std::nullopt;

      if (!outstanding) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      auto bounded = *outstanding;
      const auto &frame = state->frame;
      const bool committedFrame = sameTarget && arranged && frame && !state->refresh && state->serial == serial_ &&
                                  (!commandPending_ || inFlightRows_) && frame->viewport == inputViewport &&
                                  frame->status.byteOffset == state->anchor && !frame->anchors.empty() &&
                                  frame->anchors.front() == state->anchor &&
                                  frame->status.revision.source == state->source->stamp() &&
                                  (!scan_ || scan_->metadata.revision == frame->status.revision);

      if (committedFrame) {
        // Clamp the outstanding target, including the active batch, only against
        // this exact committed frame. Absolute/geometry changes cannot borrow it.
        if (state->anchor == 0) {
          bounded = std::max<std::int64_t>(bounded, 0);
        }
        if (frame->finalEndVisible) {
          bounded = std::min<std::int64_t>(bounded, 0);
        }
      }

      const auto queued = checkedRowSum(bounded, sameTarget ? -inFlightRows_ : 0);
      if (!queued) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      if (!sameTarget) {
        clearRelative(state->serial);
        state->rowDelta = 0;
      }

      pendingRows_ = *queued;
      if (state->status.waiting || state->status.error) {
        // Explicit input may retry a stopped provider; idle preparation may not.
        pendingRows_ = bounded;
        inFlightRows_ = 0;
        state->rowDelta = 0;
        state->refresh = true;
        state->status.preparing = true;
      } else if (!commandPending_ && !state->refresh && state->serial == serial_) {
        state->status.preparing = pendingRows_ != 0;
      }
      (void)app_.tree().invalidate(node_, Invalidation::LAYOUT);
      return true;
    }

    if (!recognized) {
      return false;
    }

    if (absolute || navigationSerial_ != state->serial) {
      clearRelative(state->serial);
    } else {
      restoreRelative();
    }

    state->rowDelta = 0;
    state->refresh = true;
    state->status.preparing = true;
    ++state->serial;
    navigationSerial_ = state->serial;
    (void)app_.tree().invalidate(node_, Invalidation::LAYOUT);
    return true;
  }

  std::expected<void, ManagedError> replace(std::shared_ptr<ByteDocumentSource> source) {
    auto state = state_.lock();
    if (!active() || !state) {
      return std::unexpected(documentFailure(node_, {SourceError::CLOSED, {}, {}}));
    }
    if (state->source == source) {
      return {};
    }
    SourceCapacityObserver replacementObserver;
    const bool changedBudget = state->source->budget() != source->budget();
    if (changedBudget) {
      auto observed = source->budget()->observeCapacity(capacityConnection_.publisher);
      if (!observed) {
        return std::unexpected(documentFailure(node_, {observed.error(), {}, {}}));
      }
      replacementObserver = std::move(*observed);
    }
    auto reserved = source->budget()->reserve({VIEW_METADATA_BYTES, 0, 0, 1});
    if (!reserved) {
      return std::unexpected(documentFailure(node_, {reserved.error(), {}, {}}));
    }
    DocumentBookmark bookmark;
    bookmark.source = state->source;
    bookmark.anchor = state->anchor;
    if (scan_) {
      bookmark.decoder = scan_->decoderOptions;
      bookmark.width = scan_->layoutWidth;
      bookmark.wrap = scan_->wrap;
      for (std::size_t index = 0; index < scan_->checkpointCount; ++index) {
        const auto &point = scan_->checkpoints[index];
        if (point.offset <= state->anchor && (!bookmark.checkpoint || point.offset > bookmark.checkpoint->offset)) {
          bookmark.checkpoint = point;
        }
      }
    }
    const auto existing = std::ranges::find_if(bookmarks_, [&](const DocumentBookmark &saved) {
      const auto owner = saved.source.lock();

      return owner && owner->stamp() == state->source->stamp();
    });

    if (existing != bookmarks_.end()) {
      *existing = std::move(bookmark);
    } else {
      bookmarks_[bookmarkCursor_] = std::move(bookmark);
      bookmarkCursor_ = (bookmarkCursor_ + 1) % BOOKMARK_COUNT;
    }
    std::uint64_t anchor{};
    restored_.reset();
    for (const auto &saved : bookmarks_) {
      const auto owner = saved.source.lock();

      if (owner && owner->stamp() == source->stamp()) {
        anchor = saved.anchor;
        if (saved.decoder.explicitEncoding == state->config.decoder.explicitEncoding &&
            saved.decoder.hint == state->config.decoder.hint && saved.decoder.policy == state->config.decoder.policy &&
            saved.wrap == state->config.wrap && saved.width == viewport_.width - (state->config.lineNumbers ? 9 : 0)) {
          restored_ = saved.checkpoint;
        }
      }
    }
    supersede();
    scan_.reset();
    dragByteRange_.reset();
    if (changedBudget) {
      capacity_ = std::move(replacementObserver);
    }
    state->source = std::move(source);
    state->anchor = anchor;
    state->rowDelta = 0;
    state->tail = false;
    state->frame.reset();
    state->status = {};
    state->refresh = true;
    ++state->serial;
    clearRelative(state->serial);
    credit_ = std::move(*reserved);
    (void)app_.tree().invalidate(node_, Invalidation::LAYOUT);
    return {};
  }

  void presentationReflow(std::uint64_t previousSerial, std::uint64_t nextSerial) noexcept {
    if (navigationSerial_ != previousSerial) {
      clearRelative(previousSerial);
    } else {
      restoreRelative();
    }
    navigationSerial_ = nextSerial;
  }

private:
  void clearRelative(std::uint64_t serial) noexcept {
    pendingRows_ = 0;
    inFlightRows_ = 0;
    navigationSerial_ = serial;
  }

  void restoreRelative() noexcept {
    // Input checked pending + in-flight together before accepting each event.
    pendingRows_ += inFlightRows_;
    inFlightRows_ = 0;
  }

  void beginRelative(DocumentViewState &state) {
    if (commandPending_ || busy_ || continuation_ || capacityBlocked_ || state.status.error || !pendingRows_ ||
        !state.frame) {
      return;
    }

    if (pendingRows_ < 0 && state.anchor == 0) {
      pendingRows_ = 0;
    } else if (pendingRows_ > 0 && state.frame->finalEndVisible) {
      pendingRows_ = 0;
    }

    if (!pendingRows_) {
      state.status.preparing = false;
      return;
    }

    // Backward preparation retains at most one viewport of preceding rows.
    // Drain rather than enlarging that retained window with the event count.
    const auto limit = static_cast<std::int64_t>(std::max(1, viewport_.height - 1));
    const auto batch = std::clamp(pendingRows_, -limit, limit);
    pendingRows_ -= batch;
    inFlightRows_ = batch;
    state.rowDelta = batch;
    state.tail = false;
    state.status.preparing = true;
    ++state.serial;
    navigationSerial_ = state.serial;
    serial_ = state.serial;
    supersede();
    intent_ = {state.config, state.width, viewport_, state.anchor, state.horizontal, batch, false};
    commandPending_ = true;
  }

  void supersede() noexcept {
    ++generation_;
    if (latest_) {
      latest_->cancel();
    }
    busy_ = false;
    completion_.reset();
    continuation_ = false;
    commandPending_ = false;
    capacity_.disarm();
    capacityBlocked_ = false;
    waitingDemand_ = {};
  }

  void fail(DocumentError error) noexcept {
    auto state = state_.lock();
    if (!active() || !state) {
      return;
    }
    busy_ = false;
    capacity_.disarm();
    state->status.error = error;
    state->status.preparing = false;
    if (latest_) {
      latest_->cancel();
    }
    // reportError may synchronously destroy this scene or remove this node.
    (void)scene_.reportError(documentFailure(node_, error));
  }

  void prepare(const LayoutSnapshot &snapshot) {
    auto state = state_.lock();
    const auto layout = snapshot.geometry(node_);
    if (!state || !layout || !layout->scrollArea) {
      cancel();
      return;
    }
    if (navigationSerial_ != state->serial) {
      dragByteRange_.reset();
      clearRelative(state->serial);
      state->rowDelta = 0;
    }

    const auto &area = *layout->scrollArea;
    bool visible{};
    if (layout->visible && layout->geometry.valid) {
      const auto local = CellRect::from(area.placement.viewportOrigin, area.placement.viewport);
      if (local) {
        const auto surface = local->translated(layout->geometry.surface.origin());
        visible = surface && !surface->intersection(layout->geometry.clip).empty();
      }
    }
    CellSize viewport;
    std::uint64_t scrollSerial{};
    std::uint64_t scrollOffset{};
    std::uint64_t scrollMaximum{};
    bool dragging{};
    const auto read = app_.tree().read(node_, [&](const DocumentView &widget) {
      const auto &scroll = ScrollWidgetAccess::state(widget);
      viewport = area.placement.viewport;
      dragging = scroll.drag && scroll.drag->axis == ScrollAxis::VERTICAL && scroll.capture.connected();
      if (const auto intent = scroll.intent.lock()) {
        scrollSerial = intent->serial;
      }
      if (const auto position = scroll.controller->snapshot()) {
        scrollOffset = position->vertical.offset.raster;
        scrollMaximum = scrollRange(position->vertical.metrics).maximum;
      }
    });
    if (!read) {
      cancel();
      return;
    }
    visible = visible && viewport.width > 0 && viewport.height > 0;
    if (!visible) {
      if (visible_ || busy_) {
        restoreRelative();
        state->rowDelta = 0;
        supersede();
        state->refresh = true;
      }
      visible_ = false;
      return;
    }
    visible_ = true;

    if (!state->roles.snapshot() && (!roleFailureSerial_ || *roleFailureSerial_ != state->serial)) {
      auto roles = ui::detail::SceneAccess::registerPreparedRoles(scene_, node_, {"body", "gutter", "position"});
      if (!roles) {
        roleFailureSerial_ = state->serial;
        // Queue one plain fallback turn before a diagnostic callback may retire
        // this activity/node/scene. Do not access any of them after reporting.
        (void)app_.tree().invalidate(node_, Invalidation::LAYOUT);
        (void)scene_.reportError(roles.error());
        return;
      }
      state->roles = std::move(*roles);
      roleFailureSerial_.reset();
    }

    const auto extent = state->frame ? state->frame->status.byteExtent : std::nullopt;

    if (!dragging) {
      dragByteRange_.reset();
    } else if (!dragByteRange_ && state->frame) {
      // Keep one byte projection for the gesture: newly prepared pages may
      // contain different byte spans while the pointer and scroll metrics stay fixed.
      dragByteRange_ = documentScrollByteRange(*state->frame);
    }

    const bool byteCommand = scrollSerial != scrollSerial_ && extent && scrollMaximum;
    if (byteCommand) {
      const auto range = dragByteRange_.value_or(documentScrollByteRange(*state->frame));
      state->anchor = scrollOffset >= scrollMaximum
                        ? *extent
                        : scrollMultiplyDivide(std::min(scrollOffset, scrollMaximum), range, scrollMaximum).quotient;
      state->rowDelta = 0;
      state->tail = scrollOffset >= scrollMaximum;
      ++state->serial;
      clearRelative(state->serial);
      state->refresh = true;
    } else if (!dragging && extent && *extent && scrollMaximum) {
      const auto &frame = state->frame;
      const bool visibleEnd = frame && frame->finalEndVisible && !state->status.waiting && !state->status.error &&
                              frame->status.byteOffset == state->anchor && frame->viewport == viewport &&
                              frame->status.revision.source == state->source->stamp();
      const auto range = documentScrollByteRange(*frame);
      const auto offset = visibleEnd ? scrollMaximum
                          : range ? scrollMultiplyDivide(std::min(state->anchor, range), scrollMaximum, range).quotient
                                  : 0;
      if (offset != scrollOffset) {
        if (!synchronizeBar(offset)) {
          return;
        }
      }
    }
    scrollSerial_ = scrollSerial;
    if (viewport.width != viewport_.width || viewport.height != viewport_.height || state->serial != serial_ ||
        state->refresh) {
      if (inFlightRows_) {
        restoreRelative();
        state->rowDelta = 0;
      }

      const bool resumeWaiting = state->status.waiting && scan_ && state->anchor == intent_.anchor &&
                                 state->horizontal == intent_.horizontal && state->rowDelta == intent_.rowDelta &&
                                 state->tail == intent_.tail;
      supersede();
      viewport_ = viewport;
      serial_ = state->serial;
      state->refresh = false;
      state->status.preparing = true;
      state->status.error.reset();
      state->status.backpressured = false;
      state->status.logicalLine.reset();
      state->status.finalEnd = false;
      state->status.waiting = false;
      state->status.byteOffset = state->anchor;
      intent_ = {state->config, state->width, viewport, state->anchor, state->horizontal, state->rowDelta, state->tail};
      intent_.seed = std::exchange(restored_, {});
      intent_.resume = resumeWaiting;
      commandPending_ = true;
    }

    if (completion_) {
      auto completed = std::exchange(completion_, {});
      busy_ = false;
      if (completed->error) {
        if (completed->error->code == SourceError::WOULD_BLOCK && completed->error->budgetDemand) {
          waitingDemand_ = *completed->error->budgetDemand;
          completed.reset();
          armCapacity(state);
          return;
        }
        fail(*completed->error);
        return;
      }
      scan_ = completed->scan;
      if (completed->providerWaiting) {
        state->status.waiting = true;
        state->status.preparing = false;
        state->status.backpressured = false;
        return;
      }
      continuation_ = completed->continueWork;
      state->status.preparing = completed->continueWork;
      if (completed->frame) {
        if (inFlightRows_ && completed->frame->anchors.empty()) {
          fail({SourceError::NO_PROGRESS, {}, {}});
          return;
        }

        const auto previousAnchor = state->anchor;
        const auto batch = std::exchange(inFlightRows_, 0);
        commandPending_ = false;
        state->frame = completed->frame;
        state->status = completed->frame->status;
        state->rowDelta = 0;
        if (!state->frame->anchors.empty() && (intent_.tail || intent_.rowDelta)) {
          state->anchor = state->frame->anchors.front();
          state->status.byteOffset = state->anchor;
        }

        if (batch && state->anchor == previousAnchor) {
          const bool knownBoundary = batch < 0 ? state->anchor == 0 : state->frame->finalEndVisible;
          if (!knownBoundary) {
            inFlightRows_ = batch;
            fail({SourceError::NO_PROGRESS, {}, {}});
            return;
          }
          if ((batch < 0 && pendingRows_ < 0) || (batch > 0 && pendingRows_ > 0)) {
            pendingRows_ = 0;
          }
        }

        (void)app_.tree().invalidate(node_, Invalidation::LAYOUT);
      }
    }
    beginRelative(*state);
    if (busy_ || capacityBlocked_ || state->status.error || (!state->status.preparing && !continuation_)) {
      return;
    }
    dispatch(state);
  }

  bool synchronizeBar(std::uint64_t offset) {
    std::expected<void, TreeError> changed;
    const auto edited = app_.tree().edit(node_, [&](DocumentView &widget) {
      auto &scroll = ScrollWidgetAccess::state(widget);
      auto plan = ScrollControllerAccess::stage(*scroll.lease);
      if (!plan) {
        changed = std::unexpected(plan.error());
        return;
      }
      changed = ScrollControllerAccess::offset(*plan, ScrollAxis::VERTICAL, {offset, 0});
      if (!changed) {
        return;
      }
      const auto committed = commitScroll(app_.tree(), node_, scroll, std::move(*plan), ScrollPublication::PREPARATION);
      if (!committed) {
        changed = std::unexpected(committed.error());
      }
    });
    if (!edited || !changed) {
      fail({!edited && edited.error() == TreeError::NO_MEMORY ? SourceError::NO_MEMORY : SourceError::CONTEXT_ERROR,
            {},
            {}});
      return false;
    }
    return true;
  }

  std::expected<SourceBudgetUsage, SourceError> nextPeak(const DocumentViewState &state) const {
    const auto amount = documentPreparationBytes(viewport_, state.config);
    if (!amount) {
      return std::unexpected(amount.error());
    }
    const auto work = *amount;
    constexpr auto MAX = std::numeric_limits<std::size_t>::max();
    const auto envelope = state.source->envelopeBytes();
    const auto page = state.source->maxPageBytes() + SourceArena::METADATA_BYTES;

    if (envelope > MAX - page || waitingDemand_.work == MAX) {
      return std::unexpected(SourceError::PAGE_TOO_LARGE);
    }

    const auto extra = std::max({work, envelope + page, waitingDemand_.bytes});
    if (work > MAX - extra) {
      return std::unexpected(SourceError::PAGE_TOO_LARGE);
    }

    SourceBudgetUsage peak{work + extra, std::max<std::size_t>(1, waitingDemand_.pages),
                           1 + std::max<std::size_t>(1, waitingDemand_.work), waitingDemand_.consumers};
    const auto options = state.source->budget()->options();
    std::size_t owned = credit_.usage().bytes;
    const auto includeOwned = [&](std::size_t bytes) {
      if (bytes > options.maxBytes - std::min(owned, options.maxBytes)) {
        return false;
      }
      owned += bytes;
      return true;
    };

    // These private retained owners cannot disappear while this view is waiting.
    // Reject impossible overlap instead of promising a capacity event that cannot occur.
    if (!includeOwned(SourceBudget::ROOT_METADATA_BYTES + SourceBudget::CAPACITY_OBSERVER_BYTES) ||
        !includeOwned(envelope) || (state.frame && !includeOwned(state.frame->credit.usage().bytes)) ||
        (scan_ && !includeOwned(scan_->reservation->usage().bytes)) ||
        peak.bytes > options.maxBytes - std::min(owned, options.maxBytes) ||
        peak.consumers > options.maxConsumers - credit_.usage().consumers) {
      return std::unexpected(SourceError::PAGE_TOO_LARGE);
    }

    return peak;
  }

  void armCapacity(const std::shared_ptr<DocumentViewState> &state) {
    const auto peak = nextPeak(*state);
    if (!peak) {
      fail({peak.error(), {}, {}});
      return;
    }
    auto armed = capacity_.arm(*peak);
    if (!armed) {
      fail({armed.error(), {}, {}});
      return;
    }
    capacityBlocked_ = true;
    state->status.backpressured = true;
  }

  void dispatch(const std::shared_ptr<DocumentViewState> &state) {
    const auto width = static_cast<std::size_t>(viewport_.width);
    const auto height = static_cast<std::size_t>(viewport_.height);
    if (width > state->config.maxFrameCells / height) {
      fail({SourceError::PAGE_TOO_LARGE, {}, {}});
      return;
    }
    auto *executor = app_.background();
    if (!executor) {
      fail({SourceError::CONTEXT_ERROR, {}, {}});
      return;
    }
    if (state->source->budget()->options().maxWork < 2) {
      fail({SourceError::PAGE_TOO_LARGE, {}, {}});
      return;
    }
    const auto reserved = documentPreparationBytes(viewport_, state->config);
    if (!reserved) {
      fail({reserved.error(), {}, {}});
      return;
    }
    const auto amount = *reserved;
    const auto peak = nextPeak(*state);
    if (!peak) {
      fail({peak.error(), {}, {}});
      return;
    }
    auto credit = capacity_.reserve({amount, 0, 1, 0}, *peak);
    if (!credit) {
      if (credit.error() == SourceError::WOULD_BLOCK) {
        armCapacity(state);
        return;
      }
      fail({credit.error(), {}, {}});
      return;
    }
    if (!latest_) {
      latest_ = std::make_unique<BackgroundLatest>(*executor);
    }
    intent_.resume = continuation_ || intent_.resume;
    auto previous = std::exchange(scan_, {});
    auto job = std::make_unique<DocumentJob>(state->source, intent_, std::move(previous), generation_,
                                             std::move(*credit), connection_.publisher);
    auto admitted = latest_->replace(std::move(job));
    if (!admitted) {
      fail(
        {admitted.error() == BackgroundError::NO_MEMORY ? SourceError::NO_MEMORY : SourceError::CONTEXT_ERROR, {}, {}});
      return;
    }
    busy_ = true;
    continuation_ = false;
  }

  SourceCredit credit_;
  ManagedScene &scene_;
  Application &app_;
  Node<DocumentView> node_;
  std::weak_ptr<const void> lifetime_;
  std::weak_ptr<DocumentViewState> state_;
  SchedulerConnection<UiDeliveryEnvelope> connection_;
  SchedulerConnection<int> capacityConnection_;
  SourceCapacityObserver capacity_;
  std::unique_ptr<BackgroundLatest> latest_;
  std::shared_ptr<const DocumentPreparation> completion_;
  std::shared_ptr<const DocumentScan> scan_;
  std::array<DocumentBookmark, BOOKMARK_COUNT> bookmarks_{};
  std::size_t bookmarkCursor_{};
  DocumentPrepareIntent intent_;
  std::optional<DocumentLayoutCheckpoint> restored_;
  bool capacityBlocked_{};
  SourceBudgetUsage waitingDemand_;
  CellSize viewport_;
  std::optional<std::uint64_t> roleFailureSerial_;
  std::uint64_t generation_{};
  std::uint64_t serial_{};
  std::uint64_t scrollSerial_{};
  std::optional<std::uint64_t> dragByteRange_;
  std::uint64_t navigationSerial_{};
  std::int64_t pendingRows_{};
  std::int64_t inFlightRows_{};
  bool commandPending_{};
  bool visible_{};
  bool busy_{};
  bool continuation_{};
  bool closed_{};
};

std::expected<void, ManagedError> attachDocument(ManagedScene &scene, Node<DocumentView> node,
                                                 ui::DocumentSource source) {
  const auto resolved = ui::detail::DocumentSourceAccess::resolve(source);
  if (!resolved) {
    return std::unexpected(documentFailure(node, resolved.error()));
  }
  const auto byteSource = *resolved;
  auto allowed = ui::detail::SceneAccess::requireBackground(scene);
  if (!allowed) {
    return allowed;
  }
  auto credit = byteSource->budget()->reserve({VIEW_METADATA_BYTES, 0, 0, 1});
  if (!credit) {
    return std::unexpected(documentFailure(node, {credit.error(), {}, {}}));
  }
  std::shared_ptr<DocumentViewState> state;
  auto &app = ui::detail::SceneAccess::application(scene);
  auto read = app.tree().edit(node, [&](DocumentView &widget) {
    state = DocumentViewAccess::shared(widget);
  });
  if (!read) {
    return std::unexpected(ui::detail::error(node, read.error()));
  }
  auto activity =
    std::make_shared<DocumentActivity>(scene, node, ui::detail::Access::lifetime(scene), state, std::move(*credit));
  auto connected = activity->connect(byteSource->budget());
  if (!connected) {
    return connected;
  }
  auto admitted = ui::detail::SceneAccess::admitActivity(scene, activity);
  if (!admitted) {
    return admitted;
  }
  state->source = byteSource;
  state->activity = std::move(activity);
  return {};
}
} // namespace
} // namespace portals::detail

namespace portals::ui {
std::shared_ptr<ByteDocumentSource> DocumentSource::byteSource() const noexcept {
  return data_ ? data_->source : nullptr;
}

std::shared_ptr<SourceBudget> DocumentSource::budget() const noexcept {
  return data_ ? data_->budget : nullptr;
}

DocumentSource document(std::shared_ptr<ByteDocumentSource> source) {
  return detail::DocumentSourceAccess::make(std::move(source));
}

DocumentSource document(std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget,
                        DocumentSourceOptions options) {
  if (!budget) {
    auto created = SourceBudget::create();
    if (!created) {
      return detail::DocumentSourceAccess::make({}, DocumentError{created.error(), {}, {}});
    }
    budget = *created;
  }
  return detail::DocumentSourceAccess::provider(std::move(provider), std::move(budget), options);
}

DocumentSource document(std::shared_ptr<const std::string> text, std::shared_ptr<SourceBudget> budget) {
  return document(std::make_shared<MemoryDocumentProvider>(std::move(text)), std::move(budget));
}

DocumentSource documentFile(std::string_view path) {
  try {
    auto source = FileDocumentProvider::create(
      std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(path.data()), path.size())));
    return source ? document(*source) : detail::DocumentSourceAccess::make({}, source.error());
  } catch (const std::bad_alloc &) {
    return detail::DocumentSourceAccess::make({}, DocumentError{SourceError::NO_MEMORY, {}, {}});
  } catch (...) {
    return detail::DocumentSourceAccess::make({}, DocumentError{SourceError::INVALID_ARGUMENT, {}, {}});
  }
}

Description<DocumentView> viewer(DocumentSource source, DocumentViewConfig config, ControlStyle style) {
  auto recipe = detail::Access::cold<DocumentView>(
    [source = std::move(source), config,
     style](ManagedScene &scene, Node<Widget> parent,
            detail::MountContext &context) -> std::expected<Node<DocumentView>, ManagedError> {
      auto effectiveConfig = config;
      const auto &document = context.controlSettings->documentSettings();
      effectiveConfig.lineNumbers = document.lineNumbers.value_or(config.lineNumbers);
      effectiveConfig.wrap = document.wrap.value_or(config.wrap);
      effectiveConfig.decoder.explicitEncoding = document.encoding.value_or(config.decoder.explicitEncoding);
      if (!portals::detail::validConfig(effectiveConfig)) {
        return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
      }
      std::optional<FrameOptions> base = context.frameOptions ? std::optional(*context.frameOptions) : FrameOptions{};
      auto frame = detail::controlFrameOptions(*context.controlSettings, std::move(base), scene.widthPolicy());
      if (!frame) {
        return std::unexpected(frame.error());
      }
      if (context.frameSettings) {
        frame = detail::controlFrameOptions(*context.frameSettings, std::move(*frame), scene.widthPolicy());
      }
      if (!frame || !*frame) {
        return std::unexpected(frame ? detail::error(parent, TreeError::INVALID_ARGUMENT) : frame.error());
      }
      const auto scroll =
        detail::controlScrollAreaOptions(*context.controlSettings, portals::detail::documentScrollOptions());
      if (!scroll) {
        return std::unexpected(detail::error(parent, scroll.error()));
      }
      auto node =
        scene.emplace<DocumentView>(parent, scene.widthPolicy(), effectiveConfig, style, std::move(**frame), *scroll);
      if (!node) {
        return node;
      }
      auto attached = portals::detail::attachDocument(scene, *node, source);
      if (!attached) {
        const auto failure = attached.error();
        (void)scene.remove(*node);
        return std::unexpected(failure);
      }
      return node;
    });
  return detail::Access::configuredControl(std::move(recipe), "DocumentView");
}

Description<DocumentView> viewer(Ref<DocumentView> reference, DocumentSource source, DocumentViewConfig config,
                                 ControlStyle style) {
  return std::move(viewer(std::move(source), config, style)).reference(std::move(reference));
}

Description<DocumentView> viewer(std::string_view path, DocumentViewConfig config, ControlStyle style) {
  return viewer(documentFile(path), config, style);
}

Description<DocumentView> viewer(Ref<DocumentView> reference, std::string_view path, DocumentViewConfig config,
                                 ControlStyle style) {
  return viewer(std::move(reference), documentFile(path), config, style);
}
} // namespace portals::ui

namespace portals {
std::expected<void, ManagedError> setDocumentModel(ManagedScene &scene, Node<DocumentView> node,
                                                   ui::DocumentSource source) {
  auto &tree = ui::detail::SceneAccess::application(scene).tree();

  std::shared_ptr<ui::detail::SceneActivity> activity;
  const auto read = tree.read(node, [&](const DocumentView &widget) {
    activity = detail::DocumentViewAccess::state(widget).activity;
  });
  if (!read || !activity) {
    return std::unexpected(ui::detail::error(node, read ? TreeError::INVALID_ARGUMENT : read.error()));
  }
  const auto resolved = ui::detail::DocumentSourceAccess::resolve(source);
  if (!resolved) {
    return std::unexpected(detail::documentFailure(node, resolved.error()));
  }
  return std::static_pointer_cast<detail::DocumentActivity>(activity)->replace(*resolved);
}

namespace {
template <class Change>
std::expected<void, ManagedError> configureDocument(ManagedScene &scene, Node<DocumentView> node, Change change) {
  auto &tree = ui::detail::SceneAccess::application(scene).tree();
  bool changed{};
  auto edited = tree.edit(node, [&](DocumentView &widget) {
    auto &state = detail::DocumentViewAccess::state(widget);
    changed = change(state.config);
    if (!changed) {
      return;
    }

    const auto previousSerial = state.serial;
    state.refresh = true;
    state.rowDelta = 0;
    ++state.serial;
    if (state.activity) {
      std::static_pointer_cast<detail::DocumentActivity>(state.activity)
        ->presentationReflow(previousSerial, state.serial);
    }
  });
  if (!edited) {
    return std::unexpected(ui::detail::error(node, edited.error()));
  }
  if (!changed) {
    return {};
  }

  auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);
  return invalidated ? std::expected<void, ManagedError>{}
                     : std::unexpected(ui::detail::error(node, invalidated.error()));
}
} // namespace

std::expected<void, ManagedError> setDocumentLineNumbers(ManagedScene &scene, Node<DocumentView> node, bool enabled) {
  return configureDocument(scene, node, [enabled](DocumentViewConfig &config) {
    if (config.lineNumbers == enabled) {
      return false;
    }
    config.lineNumbers = enabled;
    return true;
  });
}

std::expected<void, ManagedError> setDocumentWrap(ManagedScene &scene, Node<DocumentView> node, bool enabled) {
  return configureDocument(scene, node, [enabled](DocumentViewConfig &config) {
    if (config.wrap == enabled) {
      return false;
    }
    config.wrap = enabled;
    return true;
  });
}

std::expected<void, ManagedError> setDocumentEncoding(ManagedScene &scene, Node<DocumentView> node,
                                                      DocumentEncoding encoding) {
  if (encoding < DocumentEncoding::UNRESOLVED || encoding > DocumentEncoding::WINDOWS1252) {
    return std::unexpected(ui::detail::error(node, TreeError::INVALID_ARGUMENT));
  }
  return configureDocument(scene, node, [encoding](DocumentViewConfig &config) {
    if (config.decoder.explicitEncoding == encoding) {
      return false;
    }
    config.decoder.explicitEncoding = encoding;
    return true;
  });
}
} // namespace portals

namespace portals::ui::detail {
std::expected<void, ManagedError> setDocumentModel(const std::shared_ptr<RefTarget> &target, Node<DocumentView> node,
                                                   const DocumentSource &source, std::source_location origin) {
  auto scene = eventScene(target);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  auto result = portals::setDocumentModel(**scene, node, source);
  if (!result) {
    result.error().registrationOrigin = origin;
  }
  return result;
}

std::expected<void, ManagedError> documentLineNumbers(const std::shared_ptr<RefTarget> &target,
                                                      std::shared_ptr<const Node<DocumentView>> node,
                                                      std::optional<bool> enabled, std::source_location origin) {
  auto scene = eventScene(target);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  if (!node) {
    return std::unexpected(error({}, TreeError::INVALID_ARGUMENT));
  }

  auto result = portals::configureDocument(**scene, *node, [enabled](DocumentViewConfig &config) {
    const auto next = enabled.value_or(!config.lineNumbers);
    if (next == config.lineNumbers) {
      return false;
    }
    config.lineNumbers = next;
    return true;
  });
  if (!result) {
    result.error().registrationOrigin = origin;
  }
  return result;
}

std::expected<void, ManagedError> seekDocument(const std::shared_ptr<RefTarget> &target, Node<DocumentView> node,
                                               std::uint64_t offset, bool refresh, std::source_location origin) {
  auto scene = eventScene(target);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  auto &tree = SceneAccess::application(**scene).tree();
  auto result = refresh ? refreshDocument(tree, node) : seekDocumentByte(tree, node, offset);
  if (result) {
    return {};
  }
  auto failure = portals::detail::documentFailure(node, {result.error(), {}, {}});
  failure.registrationOrigin = origin;
  return std::unexpected(failure);
}

std::expected<DocumentViewStatus, ManagedError> documentStatus(const std::shared_ptr<RefTarget> &target,
                                                               Node<DocumentView> node) {
  auto scene = eventScene(target);
  if (!scene) {
    return std::unexpected(scene.error());
  }
  auto result = documentViewStatus(SceneAccess::application(**scene).tree(), node);
  return result ? std::expected<DocumentViewStatus, ManagedError>(*result)
                : std::unexpected(error(node, result.error()));
}
} // namespace portals::ui::detail
