// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>
#include <portals/ui_ref.hpp>

#include "action_state.hpp"

#include <algorithm>
#include <limits>

namespace portals {
namespace {
using detail::NotificationRecord;
using detail::ToastCandidate;

using detail::ToastCommand;
using enum detail::ToastCommand;

struct ToastStaging {
  NotificationRecord &record;
  bool active{true};

  explicit ToastStaging(NotificationRecord &value) : record(value) {
    record.staging = true;
  }

  ~ToastStaging() {
    if (active) {
      record.staging = false;
    }
  }

  void finish() noexcept {
    record.staging = false;
    active = false;
  }
};

void increment(std::uint64_t &value) noexcept {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

constexpr bool validOptions(const NotificationOptions &value) noexcept {
  return value.capacity && value.maxVisible && value.maxVisible <= value.capacity && value.maximumWidth > 0 &&
         value.margin >= 0 && value.gap >= 0 && value.actionLabelBytes && value.dedupKeyBytes && value.maxPaintCells &&
         value.textLimits.max_input_bytes && value.textLimits.max_cluster_bytes && value.textLimits.max_graphemes &&
         value.textLimits.max_cells &&
         (value.placement == ToastPlacement::TOP_START || value.placement == ToastPlacement::TOP_END ||
          value.placement == ToastPlacement::BOTTOM_START || value.placement == ToastPlacement::BOTTOM_END) &&
         (value.overflow == ToastOverflowPolicy::DROP_OLDEST_PENDING ||
          value.overflow == ToastOverflowPolicy::REJECT_NEW);
}

constexpr bool validSeverity(ToastSeverity value) noexcept {
  return value == ToastSeverity::INFO || value == ToastSeverity::SUCCESS || value == ToastSeverity::WARNING ||
         value == ToastSeverity::FAILURE;
}

ManagedError toastFailure(Node<Widget> node, TreeError cause, ManagedPhase phase = ManagedPhase::INPUT) {
  return {phase, node.id(), cause, {}, {}};
}

ManagedError schedulerFailure(Node<Widget> node, SchedulerError cause, ManagedPhase phase) {
  auto result = toastFailure(node, TreeError::CALLBACK_FAILED, phase);
  result.cause = ApplicationError{phase == ManagedPhase::INPUT    ? ApplicationPhase::DISPATCH
                                  : phase == ManagedPhase::LAYOUT ? ApplicationPhase::RENDER
                                                                  : ApplicationPhase::PREPARE,
                                  cause,
                                  {}};
  return result;
}

constexpr std::expected<SchedulerTime, TreeError> addTime(SchedulerTime now, SchedulerTime interval) noexcept {
  if (interval > std::numeric_limits<SchedulerTime>::max() - now) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  return now + interval;
}

static_assert(validSeverity(ToastSeverity::INFO));
static_assert(!validSeverity(static_cast<ToastSeverity>(-1)));
static_assert(addTime(100, 500).value() == 600);
static_assert(!addTime(std::numeric_limits<SchedulerTime>::max(), 1));

void copyCandidate(const NotificationRecord &record, ToastCandidate &candidate) {
  candidate.expectedVersion = record.version;
  candidate.visible = record.visible;
  candidate.reducedMotion = record.reducedMotion;
  candidate.dropped = record.dropped;
  candidate.evicted = record.evicted;
  candidate.nextId = record.nextId;
  candidate.entries.reserve(record.options->capacity);
  candidate.entries.assign(record.entries.begin(), record.entries.end());
}

bool sameEntries(const std::vector<detail::ToastEntry> &left, const std::vector<detail::ToastEntry> &right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](const auto &a, const auto &b) {
           return a.id == b.id && a.payload == b.payload && a.deadline == b.deadline &&
                  a.firstPresented == b.firstPresented;
         });
}

std::expected<GraphemeText, ManagedError> resolveToastText(const TextValue &value, const NotificationRecord &record,
                                                           WidthPolicy policy, TextLimits limits, ManagedPhase phase) {
  auto resolved = record.options->textResolver ? value.resolve(policy, record.options->textResolver, limits)
                                               : value.resolve(policy, limits);
  if (!resolved) {
    auto failure = toastFailure(record.root, TreeError::INVALID_ARGUMENT, phase);
    if (const auto text = std::get_if<TextError>(&resolved.error())) {
      failure.text = *text;
      if (text->kind == TextErrorKind::INPUT_LIMIT || text->kind == TextErrorKind::CLUSTER_LIMIT ||
          text->kind == TextErrorKind::GRAPHEME_LIMIT || text->kind == TextErrorKind::CELL_LIMIT) {
        failure.cause = TreeError::NODE_LIMIT;
      }
    } else {
      failure.resource = std::get<TextResourceError>(resolved.error());
    }
    return std::unexpected(failure);
  }
  return std::move(*resolved);
}

GraphemeText summary(const GraphemeText &text, std::int32_t width, const GraphemeText &ellipsis) {
  auto bytes = text.bytes();
  bytes = bytes.substr(0, bytes.find_first_of("\r\n"));
  const TextLimits limits{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(),
                          std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::int32_t>::max()};
  auto first = GraphemeText::from(bytes, text.policy(), limits).value();
  width = std::max(0, width);
  if (first.measure() <= width) {
    return first;
  }
  const auto clipped = first.clipPrefix(std::max(0, width - ellipsis.measure()));
  std::string result(first.bytes().substr(0, clipped.byte_end));
  if (ellipsis.measure() <= width) {
    result.append(ellipsis.bytes());
  }
  return GraphemeText::from(result, text.policy(), limits).value();
}
} // namespace

bool EventRouter::notificationHit(Node<Widget> node) const {
  const auto record = state_->notifications.lock();
  if (!record || record->owner != std::this_thread::get_id() || !record->ready || record->retired ||
      record->lifetime.expired() || !record->visible || node == Node<Widget>{}) {
    return false;
  }
  // Full original identities are checked by inputEligible/within; coordinates never classify a toast.
  const auto root = state_->tree.inputEligible(record->root);
  const auto target = state_->tree.inputEligible(node);
  return root && *root && target && *target && within(record->root, node);
}

bool ManagedScene::notificationEligible(const std::shared_ptr<NotificationRecord> &record, bool geometry) const {
  if (owner_ != std::this_thread::get_id() || closed_ || record->retired || !record->ready ||
      record->lifetime.expired()) {
    return false;
  }
  auto eligible = app_.tree().inputEligible(record->root);
  if (!eligible || !*eligible || app_.router().modalRoot() != Node<Widget>{}) {
    return false;
  }
  if (!geometry) {
    return true;
  }
  auto observed = app_.tree().observeGeometry(record->root);
  return observed && observed->geometry.valid &&
         !observed->geometry.surface.intersection(observed->geometry.clip).empty();
}

std::expected<std::shared_ptr<NotificationRecord>, ManagedError>
ManagedScene::makeNotifications(Node<Widget> wrapper, Node<Widget> body, const NotificationOptions &options,
                                std::vector<Node<Widget>> &created) {
  auto allowed = boundary(ManagedPhase::BIND, wrapper);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  retireNotifications();
  auto existing = app_.router().state_->notifications.lock();
  if (!validOptions(options) || (existing && !existing->retired) || !find(wrapper) || !find(body)) {
    return std::unexpected(error(ManagedPhase::BIND, wrapper, TreeError::INVALID_ARGUMENT));
  }
  const auto dialogs = app_.router().state_->dialogs.lock();
  if (dialogs && !dialogs->retired && !app_.router().within(dialogs->body, wrapper)) {
    return std::unexpected(error(ManagedPhase::BIND, wrapper, TreeError::INVALID_ARGUMENT));
  }
  try {
    if (created.max_size() - created.size() < 2 || options.maxVisible > created.max_size() - created.size() - 2) {
      return std::unexpected(error(ManagedPhase::BIND, wrapper, TreeError::NODE_LIMIT));
    }
    created.reserve(created.size() + options.maxVisible + 2);
    auto record = std::make_shared<NotificationRecord>();
    record->scene = this;
    record->lifetime = lifetime_;
    record->wrapper = wrapper;
    record->body = body;
    record->options = std::make_shared<const NotificationOptions>(options);
    record->reducedMotion = options.reducedMotion;
    record->entries.reserve(options.capacity);
    record->runningActions.reserve(options.capacity);
    record->slots.reserve(options.maxVisible);
    constexpr std::array<const char *, 8> CLASSES{"toast-info",    "toast-success", "toast-warning", "toast-failure",
                                                  "toast-message", "toast-action",  "toast-dismiss", "toast-indicator"};
    for (std::size_t i = 0; i != CLASSES.size(); ++i) {
      record->targets[i].type = "Toast";
      record->targets[i].classes.emplace_back(CLASSES[i]);
    }
    const auto glyph = [&](std::string_view value) {
      return std::make_shared<const GraphemeText>(GraphemeText::from(value, widthPolicy()).value());
    };
    record->ellipsis = glyph("…");
    if (record->ellipsis->measure() != 1) {
      record->ellipsis = glyph(".");
    }
    record->dismiss = glyph("x");
    constexpr std::array<const char *, 4> GLYPHS{"i", "+", "!", "x"};
    for (std::size_t i = 0; i != GLYPHS.size(); ++i) {
      record->severity[i] = glyph(GLYPHS[i]);
    }
    auto layer = emplace<detail::ToastLayer>(wrapper, record);
    if (!layer) {
      return std::unexpected(layer.error());
    }
    record->root = *layer;
    created.push_back(*layer);
    for (std::size_t i = 0; i != options.maxVisible; ++i) {
      auto slot = std::make_shared<detail::ToastSlot>();
      auto row = emplace<detail::ToastRow>(*layer, slot, record);
      if (!row) {
        return std::unexpected(row.error());
      }
      slot->node = *row;
      created.push_back(*row);
      auto binding = app_.router().bind(
        *row,
        [weak = std::weak_ptr(record), slot](detail::ToastRow &, const RoutedEvent &event) {
          const auto record = weak.lock();
          if (!record || record->owner != std::this_thread::get_id() || record->lifetime.expired() || record->retired ||
              !record->ready || !slot->picture) {
            return EventReply::IGNORED;
          }
          const auto mouse = std::get_if<MouseInputEvent>(&event);
          const auto action = std::get_if<ActionEvent>(&event);
          bool activate = action && action->action == Action::ACTIVATE;
          bool dismiss = action && action->action == Action::CANCEL;
          if (mouse && mouse->button == MouseButton::LEFT && mouse->action == MouseAction::PRESS) {
            const auto geometry = record->scene->app_.tree().observeGeometry(slot->node);
            if (!geometry || !geometry->geometry.valid) {
              return EventReply::IGNORED;
            }
            const auto origin = geometry->geometry.surface.origin();
            dismiss = mouse->row == origin.y && mouse->column == geometry->geometry.surface.right() - 1;
            activate = mouse->row == origin.y + 1 && static_cast<bool>(slot->picture->payload->action);
          }
          if (!activate && !dismiss) {
            return EventReply::IGNORED;
          }
          const auto id = slot->picture->id;
          auto &scene = *record->scene;
          auto result = scene.toastCommand(record, activate ? ACTIVATE : DISMISS, id, false);
          if (!result && !record->lifetime.expired()) {
            scene.reportShortError(result.error(), activate ? "Toast.activate" : "Toast.dismiss", {}, "Toast",
                                   std::source_location::current(), true);
          }
          return EventReply::HANDLED;
        },
        false);
      if (!binding) {
        return std::unexpected(error(ManagedPhase::BIND, *row, binding.error()));
      }
      find(*row)->binding = std::move(*binding);
      record->slots.push_back(std::move(slot));
    }
    auto current = app_.router().state_->notifications.lock();
    auto currentHost = app_.router().state_->dialogs.lock();
    if ((current && !current->retired) || record->retired) {
      return std::unexpected(error(ManagedPhase::BIND, record->root, TreeError::INVALID_ARGUMENT));
    }
    if (currentHost && !currentHost->retired && !app_.router().within(currentHost->body, wrapper)) {
      return std::unexpected(error(ManagedPhase::BIND, record->root, TreeError::INVALID_ARGUMENT));
    }
    const auto &routing = *app_.router().state_;
    if (routing.registrations.size() + routing.scopes.size() + !routing.dialogs.expired() >=
        routing.options.max_registrations) {
      return std::unexpected(error(ManagedPhase::BIND, record->root, TreeError::NODE_LIMIT));
    }
    if (const auto binding = app_.router().registration(record->root); binding && binding->focusable) {
      return std::unexpected(error(ManagedPhase::BIND, record->root, TreeError::INVALID_ARGUMENT));
    }
    for (const auto &slot : record->slots) {
      auto live = app_.tree().inputParent(slot->node);
      if (!live || *live != record->root) {
        return std::unexpected(
          error(ManagedPhase::BIND, record->root, live ? TreeError::INVALID_ARGUMENT : live.error()));
      }
    }
    auto rootParent = app_.tree().inputParent(record->root);
    auto contentParent = app_.tree().inputParent(body);
    if (!rootParent || !contentParent || *rootParent != wrapper || *contentParent != wrapper) {
      return std::unexpected(error(ManagedPhase::BIND, record->root,
                                   !rootParent      ? rootParent.error()
                                   : !contentParent ? contentParent.error()
                                                    : TreeError::INVALID_ARGUMENT));
    }
    notifications_ = record;
    app_.router().state_->notifications = record;
    return record;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, wrapper, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(error(ManagedPhase::BIND, wrapper, TreeError::NODE_LIMIT));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, wrapper, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError>
ManagedScene::finalizeNotifications(const std::shared_ptr<NotificationRecord> &record) {
  auto allowed = boundary(ManagedPhase::BIND, record->root);
  if (!allowed) {
    return allowed;
  }
  if (notifications_ != record || record->retired || app_.router().state_->notifications.lock() != record) {
    return std::unexpected(error(ManagedPhase::BIND, record->root, TreeError::STALE_NODE));
  }
  auto parent = app_.tree().inputParent(record->root);
  auto bodyParent = app_.tree().inputParent(record->body);
  if (!parent || !bodyParent || *parent != record->wrapper || *bodyParent != record->wrapper) {
    return std::unexpected(error(ManagedPhase::BIND, record->root,
                                 !parent       ? parent.error()
                                 : !bodyParent ? bodyParent.error()
                                               : TreeError::INVALID_ARGUMENT));
  }
  const auto host = app_.router().state_->dialogs.lock();
  if (host && !host->retired && !app_.router().within(host->body, record->wrapper)) {
    return std::unexpected(error(ManagedPhase::BIND, record->root, TreeError::INVALID_ARGUMENT));
  }
  for (const auto &slot : record->slots) {
    auto rowParent = app_.tree().inputParent(slot->node);
    if (!rowParent || *rowParent != record->root) {
      return std::unexpected(
        error(ManagedPhase::BIND, record->root, rowParent ? TreeError::INVALID_ARGUMENT : rowParent.error()));
    }
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::stageNotifications(const std::shared_ptr<NotificationRecord> &record,
                                                                   ToastCandidate &candidate, ManagedPhase phase,
                                                                   bool firstPresentation) {
  if (owner_ != std::this_thread::get_id() || closed_ || record->retired) {
    return std::unexpected(
      error(phase, record->root, owner_ != std::this_thread::get_id() ? TreeError::WRONG_THREAD : TreeError::CLOSED));
  }
  // Public callers already passed boundary(); internal prepareViews deliberately holds preparing_.
  auto allowed = app_.tree().withScope(record->root, [](const UiScope &) {
  });
  if (!allowed) {
    return std::unexpected(error(phase, record->root, allowed.error()));
  }
  auto input = app_.tree().inputRevision(record->root);
  auto geometryRevision = app_.tree().geometryRevision();
  auto rootGeometry = app_.tree().observeGeometry(record->root);
  if (!input || !geometryRevision || !rootGeometry) {
    return std::unexpected(error(phase, record->root,
                                 !input              ? input.error()
                                 : !geometryRevision ? geometryRevision.error()
                                                     : rootGeometry.error()));
  }
  if (!*input || !*geometryRevision) {
    return std::unexpected(error(phase, record->root, TreeError::IDENTITY_EXHAUSTED));
  }
  candidate.inputRevision = *input;
  candidate.geometryRevision = *geometryRevision;
  candidate.styleRevision = styles_.revision();
  const auto now = app_.now();
  const auto &options = *record->options;
  auto width = options.maximumWidth;
  if (rootGeometry->geometry.valid) {
    const auto available = rootGeometry->geometry.surface.size().width;
    const auto margin = std::min(options.margin, std::max(0, available - 1) / 2);
    width = std::min(width, available - 2 * margin);
  }
  candidate.width = width;
  const bool eligible = candidate.visible && notificationEligible(record, true);
  candidate.eligible = eligible;
  candidate.previousMarker = record->timerMarker;
  candidate.previousTimerStarted = record->timerMarker && record->timerMarker->started;
  candidate.previousRegistrationAlive = record->timerMarker && record->timerMarker->registrationAlive;
  candidate.observations.reserve(record->slots.size());
  candidate.assignments.reserve(record->slots.size());
  for (const auto &slot : record->slots) {
    auto revision = app_.tree().inputRevision(slot->node);
    auto slotEligible = app_.tree().inputEligible(slot->node);
    auto observed = app_.tree().observeGeometry(slot->node);

    if (!revision && revision.error() == TreeError::STALE_NODE && !slotEligible &&
        slotEligible.error() == TreeError::STALE_NODE && !observed && observed.error() == TreeError::STALE_NODE) {
      candidate.observations.push_back({{}, 0, false, false});
      candidate.assignments.push_back(slot->assignment);

      continue;
    }

    if (!revision || !slotEligible || !observed) {
      return std::unexpected(error(phase, record->root,
                                   !revision       ? revision.error()
                                   : !slotEligible ? slotEligible.error()
                                                   : observed.error()));
    }

    if (!*revision) {
      return std::unexpected(error(phase, record->root, TreeError::IDENTITY_EXHAUSTED));
    }

    candidate.observations.push_back({observed->geometry, *revision, *slotEligible});
    candidate.assignments.push_back(slot->assignment);
  }
  // Final-frame exposure is attached to an exact row assignment, never merely to the large plane.
  if (firstPresentation && eligible) {
    for (std::size_t i = 0; i != record->slots.size(); ++i) {
      const auto &slot = record->slots[i];
      const auto &observation = candidate.observations[i];
      const auto &geometry = observation.geometry;
      if (!slot->picture || !observation.eligible || !geometry.valid ||
          geometry.surface.intersection(geometry.clip).empty()) {
        continue;
      }
      auto entry = std::ranges::find_if(candidate.entries, [&](const auto &entry) {
        return entry.id == slot->picture->id;
      });
      if (entry == candidate.entries.end() || entry->firstPresented) {
        continue;
      }
      if (entry->payload->options.duration) {
        const auto deadline = addTime(now, *entry->payload->options.duration);
        if (!deadline) {
          return std::unexpected(error(phase, record->root, deadline.error()));
        }
        entry->deadline = *deadline;
      }
      entry->firstPresented = true;
    }
  }
  // Absolute deadlines survive hidden and resize-demoted periods. Reappearance reconciles before paint.
  if (!firstPresentation && candidate.visible && notificationEligible(record, false)) {
    std::size_t index{};
    std::erase_if(candidate.entries, [&](const auto &entry) {
      const auto current = index++;
      const bool slotEligible = current >= candidate.observations.size() || candidate.observations[current].eligible;
      return slotEligible && entry.deadline && *entry.deadline <= now;
    });
  }
  const auto selected = std::min(options.maxVisible, candidate.entries.size());
  candidate.pictures.reserve(record->slots.size());
  const auto nearest = [&](SchedulerTime deadline) {
    if (!candidate.deadline || deadline < *candidate.deadline) {
      candidate.deadline = deadline;
    }
  };
  if (eligible) {
    for (std::size_t i = 0; i != candidate.entries.size(); ++i) {
      const auto &entry = candidate.entries[i];
      const bool slotEligible = i >= candidate.observations.size() || candidate.observations[i].eligible;
      if (slotEligible && entry.deadline) {
        nearest(*entry.deadline);
      }
    }
  }
  for (std::size_t i = 0; i != record->slots.size(); ++i) {
    if (!candidate.visible || i >= selected) {
      candidate.pictures.emplace_back();
      candidate.geometryChanged |= static_cast<bool>(record->slots[i]->picture);
      continue;
    }
    const auto &entry = candidate.entries[i];
    const auto &payload = *entry.payload;
    const auto height = payload.action ? 2 : 1;
    if (std::uint64_t(width) * std::uint64_t(height) > options.maxPaintCells) {
      return std::unexpected(error(phase, record->root, TreeError::WOULD_BLOCK));
    }
    const auto &old = record->slots[i]->picture;
    const bool sameAssignment = old && old->id == entry.id;
    bool alert = sameAssignment && old->alert && !candidate.reducedMotion;
    const auto &indicator = payload.options.indicator;
    const auto &observation = candidate.observations[i];
    const bool positive =
      observation.geometry.valid && !observation.geometry.surface.intersection(observation.geometry.clip).empty();
    if (indicator.enabled && indicator.alternateBackground && !candidate.reducedMotion && eligible &&
        observation.eligible && positive && sameAssignment) {
      alert = firstPresentation ? old->alert : ((now / indicator.period) % 2) != 0;
      if (entry.firstPresented) {
        const auto tick = addTime(now, indicator.period - now % indicator.period);
        if (!tick) {
          return std::unexpected(error(phase, record->root, tick.error()));
        }
        nearest(*tick);
      }
    }
    const bool samePicture =
      sameAssignment && old->width == width && old->alert == alert && record->styleRevision == candidate.styleRevision;
    if (samePicture) {
      candidate.pictures.push_back(old);
      continue;
    }
    const auto messageWidth = std::max(0, width - 3);
    auto message = summary(payload.data->text, messageWidth, *record->ellipsis);
    auto action = payload.data->actionLabel ? summary(*payload.data->actionLabel, width, *record->ellipsis)
                                            : GraphemeText::from("", widthPolicy()).value();
    auto picture =
      std::make_shared<detail::ToastPresentation>(entry.id, entry.payload, std::move(message), std::move(action));
    picture->width = width;
    picture->alert = alert;
    picture->styles = notificationStyles(record, payload, record->slots[i]->node.id(), alert);
    candidate.pictures.push_back(std::move(picture));
    candidate.geometryChanged |= !sameAssignment || (old && old->width != width);
    candidate.paintChanged = true;
  }
  candidate.geometryChanged |= candidate.visible != record->visible;
  candidate.replaceTimer =
    candidate.deadline != record->timerDeadline ||
    (record->timerMarker && (record->timerMarker->started || !record->timerMarker->registrationAlive));
  if (candidate.replaceTimer && candidate.deadline) {
    if (record->timerGeneration == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(error(phase, record->root, TreeError::IDENTITY_EXHAUSTED));
    }
    candidate.marker = std::make_shared<detail::ToastTimerMarker>();
    candidate.marker->generation = record->timerGeneration + 1;
    std::expected<void, ManagedError> registered;
    const auto marker = candidate.marker;
    auto registration = std::make_shared<detail::ToastTimerLifetime>(marker);
    auto scoped = app_.tree().withScope(record->root, [&](const UiScope &scope) {
      auto timer =
        app_.scheduler().at(scope, *candidate.deadline, [weak = std::weak_ptr(record), marker, registration] {
          (void)registration;
          marker->started = true;
          const auto record = weak.lock();
          if (!record || record->owner != std::this_thread::get_id() || record->lifetime.expired() ||
              !marker->committed || record->timerMarker != marker || record->timerGeneration != marker->generation ||
              record->retired || !record->ready) {
            return;
          }
          record->scene->notificationTimer(weak, marker);
        });
      if (!timer) {
        registered = std::unexpected(schedulerFailure(record->root, timer.error(), phase));
      } else {
        candidate.timer = std::move(*timer);
      }
    });
    if (!scoped || !registered) {
      return !scoped ? std::unexpected(error(phase, record->root, scoped.error())) : registered;
    }
    registration.reset();
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::commitNotifications(const std::shared_ptr<NotificationRecord> &record,
                                                                    ToastCandidate &candidate, ManagedPhase phase) {
  std::expected<void, ManagedError> result;

  auto edited = app_.tree().edit(record->root, [&](Widget &) {
    // This edit is the final allocation-capable borrow. Everything after invalidate is scalar/pointer publication.
    auto input = app_.tree().inputRevision(record->root);
    auto geometry = app_.tree().geometryRevision();

    if (closed_ || record->retired || notifications_ != record || !input || !geometry ||
        *input != candidate.inputRevision || *geometry != candidate.geometryRevision ||
        record->version != candidate.expectedVersion || styles_.revision() != candidate.styleRevision ||
        (candidate.marker && (candidate.marker->started || !candidate.marker->registrationAlive)) ||
        record->timerMarker != candidate.previousMarker ||
        (record->timerMarker && record->timerMarker->started != candidate.previousTimerStarted) ||
        (record->timerMarker && record->timerMarker->registrationAlive != candidate.previousRegistrationAlive) ||
        (candidate.visible && notificationEligible(record, true)) != candidate.eligible) {
      result = std::unexpected(error(phase, record->root,
                                     closed_ || record->retired ? TreeError::CLOSED
                                     : !input                   ? input.error()
                                                                : TreeError::WOULD_BLOCK));

      return;
    }

    for (std::size_t i = 0; i != record->slots.size(); ++i) {
      auto live = app_.tree().inputRevision(record->slots[i]->node);
      auto eligible = app_.tree().inputEligible(record->slots[i]->node);
      const auto &observation = candidate.observations[i];
      const bool matches =
        observation.available
          ? live && eligible && *live == observation.inputRevision && *eligible == observation.eligible
          : !live && live.error() == TreeError::STALE_NODE && !eligible && eligible.error() == TreeError::STALE_NODE;

      if (!matches || record->slots[i]->assignment != candidate.assignments[i]) {
        result = std::unexpected(error(phase, record->root,
                                       !live       ? live.error()
                                       : !eligible ? eligible.error()
                                                   : TreeError::WOULD_BLOCK));

        return;
      }

      const bool replacing = record->slots[i]->picture != candidate.pictures[i];

      if (replacing && record->slots[i]->assignment == std::numeric_limits<std::uint64_t>::max()) {
        result = std::unexpected(error(phase, record->root, TreeError::IDENTITY_EXHAUSTED));

        return;
      }
    }

    if (candidate.actionSlot) {
      const auto &slot = *candidate.actionSlot;
      auto live = app_.tree().inputRevision(slot.node);
      auto eligible = app_.tree().inputEligible(slot.node);

      if (!live || !eligible || !*eligible || *live != candidate.actionInputRevision ||
          slot.assignment != candidate.actionAssignment || !slot.picture || slot.picture->id != candidate.actionId) {
        result = std::unexpected(error(phase, record->root,
                                       !live       ? live.error()
                                       : !eligible ? eligible.error()
                                                   : TreeError::WOULD_BLOCK));

        return;
      }
    }

    if (record->version == std::numeric_limits<std::uint64_t>::max()) {
      result = std::unexpected(error(phase, record->root, TreeError::IDENTITY_EXHAUSTED));

      return;
    }

    const auto invalidation = candidate.geometryChanged ? Invalidation::LAYOUT
                              : candidate.paintChanged  ? Invalidation::PAINT
                                                        : Invalidation::NONE;

    if (invalidation != Invalidation::NONE) {
      auto dirty = app_.tree().invalidate(record->root, invalidation);

      if (!dirty) {
        result = std::unexpected(error(phase, record->root, dirty.error()));

        return;
      }
    }

    record->entries.swap(candidate.entries);

    for (std::size_t i = 0; i != record->slots.size(); ++i) {
      auto &slot = *record->slots[i];

      if (slot.picture != candidate.pictures[i]) {
        ++slot.assignment;
        slot.picture.swap(candidate.pictures[i]);
      }
    }

    record->visible = candidate.visible;
    record->reducedMotion = candidate.reducedMotion;
    record->dropped = candidate.dropped;
    record->evicted = candidate.evicted;
    record->nextId = candidate.nextId;
    record->width = candidate.width;
    record->styleRevision = candidate.styleRevision;
    ++record->version;

    if (candidate.replaceTimer) {
      if (candidate.marker) {
        candidate.marker->committed = true;
        record->timerGeneration = candidate.marker->generation;
      }

      // Candidate now owns the retired token and drawing owners; their destruction occurs after the borrow.
      std::swap(record->timer, candidate.timer);
      record->timerMarker.swap(candidate.marker);
      record->timerDeadline = candidate.deadline;
    }
  });

  return !edited ? std::unexpected(error(phase, record->root, edited.error())) : result;
}

std::expected<ToastPostReceipt, ManagedError> ManagedScene::postToast(const std::shared_ptr<NotificationRecord> &record,
                                                                      const ui::ToastMessage &message) {
  auto allowed = boundary(ManagedPhase::INPUT, record->root);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (record->staging) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::WOULD_BLOCK));
  }

  ToastStaging staging(*record);

  try {
    ToastCandidate candidate;

    copyCandidate(*record, candidate);

    const auto &options = *record->options;
    const auto &request = message.options_;

    if (!validSeverity(request.severity) || (request.duration && !*request.duration) ||
        request.dedupKey.size() > options.dedupKeyBytes || (request.indicator.enabled && !request.indicator.period) ||
        static_cast<bool>(message.action_) != message.label_.has_value()) {
      return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
    }

    auto text = resolveToastText(message.text_, *record, widthPolicy(), options.textLimits, ManagedPhase::INPUT);

    if (!text) {
      return std::unexpected(text.error());
    }

    std::optional<GraphemeText> action;

    if (message.label_) {
      auto limits = options.textLimits;
      limits.max_input_bytes = options.actionLabelBytes;
      auto label = resolveToastText(*message.label_, *record, widthPolicy(), limits, ManagedPhase::INPUT);
      if (!label) {
        return std::unexpected(label.error());
      }
      if (label->bytes().empty() || label->bytes().find_first_of("\r\n") != std::string_view::npos) {
        return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
      }
      action.emplace(std::move(*label));
    }

    std::shared_ptr<const GraphemeText> indicator;

    if (request.indicator.enabled) {
      auto glyph =
        resolveToastText(request.indicator.glyph, *record, widthPolicy(), options.textLimits, ManagedPhase::INPUT);
      if (!glyph) {
        return std::unexpected(glyph.error());
      }
      if (glyph->graphemes().size() != 1 || glyph->measure() != 1) {
        return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
      }
      indicator = std::make_shared<const GraphemeText>(std::move(*glyph));
    }

    const auto now = app_.now();

    std::erase_if(candidate.entries, [now](const auto &entry) {
      return entry.deadline && *entry.deadline <= now;
    });

    ToastPostReceipt receipt;
    auto duplicate = request.dedupKey.empty() ? candidate.entries.end()
                                              : std::ranges::find_if(candidate.entries, [&](const auto &entry) {
                                                  return entry.payload->options.dedupKey == request.dedupKey;
                                                });

    if (duplicate != candidate.entries.end()) {
      receipt.status = ToastPostStatus::DEDUPLICATED;
      receipt.id = duplicate->id;
      if (options.refreshDeduplicated && duplicate->firstPresented && duplicate->payload->options.duration) {
        auto deadline = addTime(now, *duplicate->payload->options.duration);
        if (!deadline) {
          return std::unexpected(error(ManagedPhase::INPUT, record->root, deadline.error()));
        }
        duplicate->deadline = *deadline;
      }
    } else {
      if (candidate.entries.size() == options.capacity) {
        if (options.overflow == ToastOverflowPolicy::REJECT_NEW) {
          return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::NODE_LIMIT));
        }
        auto pending = candidate.entries.begin() +
                       static_cast<std::ptrdiff_t>(std::min(options.maxVisible, candidate.entries.size()));
        auto victim = std::find_if(pending, candidate.entries.end(), [&](const auto &entry) {
          return entry.payload->options.duration &&
                 std::ranges::find(record->runningActions, entry.id) == record->runningActions.end();
        });
        if (victim == candidate.entries.end()) {
          receipt.status = ToastPostStatus::DROPPED;
          increment(candidate.dropped);
        } else {
          receipt.evictedId = victim->id;
          candidate.entries.erase(victim);
          increment(candidate.evicted);
        }
      }
      if (receipt.status != ToastPostStatus::DROPPED) {
        if (!candidate.nextId || candidate.nextId == std::numeric_limits<std::uint64_t>::max()) {
          return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::IDENTITY_EXHAUSTED));
        }
        const ToastId id{record->root.id(), candidate.nextId++};
        const auto data = std::make_shared<const ToastData>(std::move(*text), std::move(action), request.severity);
        const auto callback = message.action_ ? std::make_shared<ui::ToastMessage::Action>(*message.action_) : nullptr;
        const auto payload = std::make_shared<const detail::ToastPayload>(data, request, callback, indicator);

        candidate.entries.push_back({id, payload, {}, false});
        receipt.id = id;
        receipt.status = candidate.entries.size() <= options.maxVisible ? ToastPostStatus::ACCEPTED_VISIBLE
                                                                        : ToastPostStatus::ACCEPTED_QUEUED;
      }
    }

    auto staged = stageNotifications(record, candidate, ManagedPhase::INPUT);

    if (!staged) {
      return std::unexpected(staged.error());
    }

    auto committed = commitNotifications(record, candidate, ManagedPhase::INPUT);

    staging.finish();

    return committed ? std::expected<ToastPostReceipt, ManagedError>(receipt) : std::unexpected(committed.error());
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::NODE_LIMIT));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> ManagedScene::activateToast(const std::shared_ptr<NotificationRecord> &record,
                                                              ToastId id) {
  if (std::ranges::find(record->runningActions, id) != record->runningActions.end() ||
      record->runningActions.size() == record->options->capacity) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::WOULD_BLOCK));
  }

  const auto found = std::ranges::find_if(record->entries, [&](const auto &entry) {
    return entry.id == id;
  });

  if (found == record->entries.end()) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::STALE_NODE));
  }

  const auto action = found->payload->action;

  if (!action || !*action || !record->visible || !notificationEligible(record, true)) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
  }

  const auto slot = std::ranges::find_if(record->slots, [&](const auto &slot) {
    return slot->picture && slot->picture->id == id;
  });

  if (slot == record->slots.end()) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
  }

  const auto assignedSlot = *slot;
  const auto assignment = assignedSlot->assignment;
  auto revision = app_.tree().inputRevision(assignedSlot->node);
  auto eligible = app_.tree().inputEligible(assignedSlot->node);
  auto geometry = app_.tree().observeGeometry(assignedSlot->node);

  if (!revision || !eligible || !*eligible || !*revision) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root,
                                 !revision    ? revision.error()
                                 : !eligible  ? eligible.error()
                                 : !*revision ? TreeError::IDENTITY_EXHAUSTED
                                              : TreeError::INVALID_ARGUMENT));
  }

  if (!geometry || !geometry->geometry.valid ||
      geometry->geometry.surface.intersection(geometry->geometry.clip).empty()) {
    return std::unexpected(
      error(ManagedPhase::INPUT, record->root, geometry ? TreeError::INVALID_GEOMETRY : geometry.error()));
  }

  const ToastActionEvent event{id, found->payload->data};
  const bool dismiss = found->payload->options.dismissOnAction;
  ToastStaging staging(*record);

  try {
    ToastCandidate candidate;

    candidate.actionSlot = assignedSlot;
    candidate.actionId = id;
    candidate.actionInputRevision = *revision;
    candidate.actionAssignment = assignment;
    copyCandidate(*record, candidate);

    if (dismiss) {
      std::erase_if(candidate.entries, [&](const auto &entry) {
        return entry.id == id;
      });
    }

    auto prepared = stageNotifications(record, candidate, ManagedPhase::INPUT);

    if (!prepared) {
      return prepared;
    }

    auto committed = commitNotifications(record, candidate, ManagedPhase::INPUT);

    staging.finish();

    if (!committed) {
      return committed;
    }

    // All owners needed by user entry were pinned before dismissal; no row is touched after entry.
    record->runningActions.push_back(id);

    struct Running {
      NotificationRecord &record;

      ~Running() {
        record.runningActions.pop_back();
      }
    } running{*record};

    ui::detail::SetupFailurePause setup(facadeContext());
    ui::detail::CurrentSceneScope current(facadeContext());
    auto admitted = actionBoundary();

    if (!admitted) {
      return std::unexpected(error(ManagedPhase::INPUT, record->root, admitted.error()));
    }

    ActionAdmission admission(*this);
    auto result = (*action)(event);

    return result ? std::expected<void, ManagedError>{}
                  : std::unexpected(error(ManagedPhase::INPUT, record->root, result.error()));
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> ManagedScene::toastCommand(const std::shared_ptr<NotificationRecord> &record,
                                                             detail::ToastCommand command, std::optional<ToastId> id,
                                                             bool flag) {
  auto allowed = boundary(ManagedPhase::INPUT, record->root);
  if (!allowed) {
    return allowed;
  }
  if (record->staging) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::WOULD_BLOCK));
  }
  if (id && id->center != record->root.id()) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::WRONG_SESSION));
  }
  if (command == ACTIVATE || command == DISMISS) {
    if (!id) {
      if (!record->visible || !notificationEligible(record, true)) {
        return {};
      }
      for (auto slot = record->slots.rbegin(); slot != record->slots.rend(); ++slot) {
        if (!(*slot)->picture || (command == ACTIVATE && !(*slot)->picture->payload->action)) {
          continue;
        }
        auto eligible = app_.tree().inputEligible((*slot)->node);
        auto geometry = app_.tree().observeGeometry((*slot)->node);
        if (eligible && *eligible && geometry && geometry->geometry.valid &&
            !geometry->geometry.surface.intersection(geometry->geometry.clip).empty()) {
          id = (*slot)->picture->id;
          break;
        }
      }
      if (!id) {
        return {};
      }
    }
    if (command == ACTIVATE) {
      return activateToast(record, *id);
    }
    if (app_.router().modalRoot() != Node<Widget>{}) {
      return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
    }
    if (std::ranges::none_of(record->entries, [&](const auto &entry) {
          return entry.id == *id;
        })) {
      return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::STALE_NODE));
    }
  }
  if ((command == CLEAR && record->entries.empty()) || (command == VISIBLE && record->visible == flag) ||
      (command == REDUCED_MOTION && record->reducedMotion == flag)) {
    return {};
  }
  ToastStaging staging(*record);
  try {
    ToastCandidate candidate;
    copyCandidate(*record, candidate);
    switch (command) {
    case DISMISS:
      std::erase_if(candidate.entries, [&](const auto &entry) {
        return entry.id == *id;
      });
      break;
    case CLEAR:
      candidate.entries.clear();
      break;
    case VISIBLE:
      candidate.visible = flag;
      break;
    case REDUCED_MOTION:
      candidate.reducedMotion = flag;
      break;
    default:
      return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::INVALID_ARGUMENT));
    }
    auto prepared = stageNotifications(record, candidate, ManagedPhase::INPUT);
    if (!prepared) {
      return prepared;
    }
    auto committed = commitNotifications(record, candidate, ManagedPhase::INPUT);
    staging.finish();
    return committed;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::CALLBACK_FAILED));
  }
}

std::expected<std::shared_ptr<const NotificationSnapshot>, ManagedError>
ManagedScene::notificationSnapshot(const std::shared_ptr<NotificationRecord> &record) const {
  auto allowed = boundary(ManagedPhase::INPUT, record->root);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  try {
    std::vector<detail::ToastEntry> entries(0);
    entries.reserve(record->options->capacity);
    entries.assign(record->entries.begin(), record->entries.end());
    auto snapshot = std::make_shared<NotificationSnapshot>();
    snapshot->items.reserve(entries.size());
    snapshot->visible = record->visible;
    snapshot->reducedMotion = record->reducedMotion;
    snapshot->dropped = record->dropped;
    snapshot->evicted = record->evicted;
    for (std::size_t i = 0; i != entries.size(); ++i) {
      const auto &entry = entries[i];
      std::optional<CellRect> rectangle;
      for (const auto &slot : record->slots) {
        if (slot->picture && slot->picture->id == entry.id) {
          auto geometry = app_.tree().observeGeometry(slot->node);
          if (geometry && geometry->geometry.valid) {
            rectangle = geometry->geometry.surface;
          }
          break;
        }
      }
      snapshot->items.push_back(
        {entry.id, entry.payload->data,
         i < record->options->maxVisible ? ToastPostStatus::ACCEPTED_VISIBLE : ToastPostStatus::ACCEPTED_QUEUED,
         rectangle, entry.firstPresented, entry.deadline});
    }
    return snapshot;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::INPUT, record->root, TreeError::NO_MEMORY));
  }
}

std::expected<void, TreeError> ManagedScene::prepareNotifications(bool firstPresentation) {
  retireNotifications();
  const auto record = notifications_;
  if (!record || record->retired || !record->ready || record->staging ||
      (record->entries.empty() && !record->timerDeadline)) {
    return {};
  }
  // Idle/static centers perform no allocation and register no polling work. Only an actual
  // geometry/style/exposure/deadline/phase change enters the fallible staging path.
  const auto now = app_.now();
  const auto rootGeometry = app_.tree().observeGeometry(record->root);
  const bool eligible = record->visible && notificationEligible(record, true);
  const bool reconcile = record->visible && notificationEligible(record, false);
  auto width = record->options->maximumWidth;
  if (rootGeometry && rootGeometry->geometry.valid) {
    const auto available = rootGeometry->geometry.surface.size().width;
    const auto margin = std::min(record->options->margin, std::max(0, available - 1) / 2);
    width = std::min(width, available - 2 * margin);
  }
  bool needed = width != record->width || record->styleRevision != styles_.revision() ||
                (record->timerMarker && (record->timerMarker->started || !record->timerMarker->registrationAlive));
  std::optional<SchedulerTime> next;
  const auto nearest = [&](SchedulerTime deadline) {
    if (!next || deadline < *next) {
      next = deadline;
    }
  };
  for (std::size_t i = 0; i != record->entries.size(); ++i) {
    const auto &entry = record->entries[i];
    const auto slotEligible = i >= record->slots.size() ? std::expected<bool, TreeError>(true)
                                                        : app_.tree().inputEligible(record->slots[i]->node);
    needed |=
      !firstPresentation && reconcile && slotEligible && *slotEligible && entry.deadline && *entry.deadline <= now;
    if (eligible && slotEligible && *slotEligible && entry.deadline) {
      nearest(*entry.deadline);
    }
  }
  for (const auto &slot : record->slots) {
    if (!slot->picture || !eligible) {
      continue;
    }
    const auto slotEligible = app_.tree().inputEligible(slot->node);
    const auto geometry = app_.tree().observeGeometry(slot->node);
    if (!slotEligible || !*slotEligible || !geometry || !geometry->geometry.valid ||
        geometry->geometry.surface.intersection(geometry->geometry.clip).empty()) {
      continue;
    }
    const auto entry = std::ranges::find_if(record->entries, [&](const auto &entry) {
      return entry.id == slot->picture->id;
    });
    if (entry == record->entries.end()) {
      needed = true;
      continue;
    }
    needed |= firstPresentation && !entry->firstPresented;
    const auto &indicator = entry->payload->options.indicator;
    if (indicator.enabled && indicator.alternateBackground && !record->reducedMotion) {
      if (!firstPresentation) {
        needed |= slot->picture->alert != (((now / indicator.period) % 2) != 0);
      }
      if (entry->firstPresented) {
        const auto tick = addTime(now, indicator.period - now % indicator.period);
        if (tick) {
          nearest(*tick);
        } else {
          needed = true;
        }
      }
    }
  }
  needed |= next != record->timerDeadline;
  if (!needed) {
    return {};
  }
  const auto phase = firstPresentation ? ManagedPhase::LAYOUT : ManagedPhase::PREPARE;
  ToastStaging staging(*record);
  try {
    ToastCandidate candidate;
    copyCandidate(*record, candidate);
    auto prepared = stageNotifications(record, candidate, phase, firstPresentation);
    if (prepared && (candidate.geometryChanged || candidate.paintChanged || candidate.replaceTimer ||
                     !sameEntries(record->entries, candidate.entries) || candidate.width != record->width ||
                     candidate.styleRevision != record->styleRevision)) {
      prepared = commitNotifications(record, candidate, phase);
    }
    staging.finish();
    if (!prepared) {
      prepared.error().operation = firstPresentation ? "Toast.firstPresentation" : "Toast.prepare";
      remember(prepared.error());
      const auto *cause = std::get_if<TreeError>(&prepared.error().cause);
      return std::unexpected(cause ? *cause : TreeError::CALLBACK_FAILED);
    }
    return {};
  } catch (const std::bad_alloc &) {
    remember(error(phase, record->root, TreeError::NO_MEMORY));
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    remember(error(phase, record->root, TreeError::CALLBACK_FAILED));
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

void ManagedScene::notificationTimer(const std::weak_ptr<NotificationRecord> &weak,
                                     const std::shared_ptr<detail::ToastTimerMarker> &marker) {
  const auto record = weak.lock();
  if (!record || record->owner != std::this_thread::get_id() || record->lifetime.expired() || record->retired ||
      !record->ready || record->timerMarker != marker || record->timerGeneration != marker->generation ||
      !marker->committed || record->staging || closed_) {
    return;
  }
  if (!record->visible || !notificationEligible(record, true)) {
    record->timer = {};
    record->timerMarker.reset();
    record->timerDeadline.reset();
    return;
  }
  auto result = prepareNotifications();
  if (!result && !record->lifetime.expired()) {
    // prepareNotifications preserves the full primary error. The existing runner observes failure_.
    app_.requestStop();
  }
}

void ManagedScene::retireNotifications() noexcept {
  const auto record = notifications_;
  if (!record) {
    return;
  }
  auto live = app_.tree().inputRevision(record->root);
  if (closed_ || record->retired || !live) {
    record->retired = true;
    record->ready = false;
    record->timerMarker.reset();
    record->timerDeadline.reset();
    record->timer = {};
    notifications_.reset();
  }
}

std::array<CellStyle, 5> ManagedScene::notificationStyles(const std::shared_ptr<NotificationRecord> &record,
                                                          const detail::ToastPayload &payload, NodeId node,
                                                          bool alert) const noexcept {
  std::array<CellStyle, 5> result{};
  const auto severity = static_cast<std::size_t>(payload.data->severity);
  auto defaults = styles_.resolve(styles_.sheet_, styles_.layerState_, record->targets[severity], {}, node);
  result[0] = defaults.normal;
  for (std::size_t part = 1; part != 4; ++part) {
    result[part] =
      styles_.resolve(styles_.sheet_, styles_.layerState_, record->targets[part + 3], defaults, node).normal;
  }
  defaults.normal.foreground = payload.options.indicator.foreground;
  if (alert) {
    defaults.normal.background = payload.options.indicator.alertBackground;
  }
  result[4] = styles_.resolve(styles_.sheet_, styles_.layerState_, record->targets[7], defaults, node).normal;
  return result;
}

Invalidation ManagedScene::invalidateNotificationStyles() noexcept {
  const auto record = notifications_;
  if (!record || record->retired || !record->ready || !record->visible) {
    return Invalidation::NONE;
  }
  for (const auto &slot : record->slots) {
    const auto &picture = slot->picture;
    if (picture && picture->styles != notificationStyles(record, *picture->payload, slot->node.id(), picture->alert)) {
      // Engine publication has ended its frame borrow. Live notification roots admit this no-allocation
      // invalidation; independently removed/closed roots have no presentation to wake.
      auto invalidated = app_.tree().invalidate(record->root, Invalidation::PAINT);
      return invalidated ? Invalidation::PAINT : Invalidation::NONE;
    }
  }
  return Invalidation::NONE;
}
} // namespace portals

namespace portals::ui::detail {
std::expected<void, ManagedError> NotificationAccess::reserve(const NotificationCenter &center) {
  const auto &target = center.target_;
  if (!target || target->owner != std::this_thread::get_id()) {
    return std::unexpected(toastFailure({}, target ? TreeError::WRONG_THREAD : TreeError::CLOSED, ManagedPhase::BIND));
  }
  if (target->reserved || target->published) {
    return std::unexpected(toastFailure({}, TreeError::INVALID_ARGUMENT, ManagedPhase::BIND));
  }
  target->reserved = true;
  return {};
}

void NotificationAccess::release(const NotificationCenter &center) noexcept {
  if (center.target_ && center.target_->owner == std::this_thread::get_id()) {
    center.target_->reserved = false;
  }
}

void NotificationAccess::publish(const NotificationCenter &center,
                                 const std::shared_ptr<portals::detail::NotificationRecord> &record) noexcept {
  center.target_->record = record;
  center.target_->scene = record->scene;
  center.target_->original = record->root.id();
  center.target_->lifetime = record->lifetime;
  center.target_->published = true;
  record->ready = true;
}

std::expected<std::shared_ptr<portals::detail::NotificationRecord>, ManagedError>
NotificationAccess::lock(const NotificationCenter &center) {
  const auto &target = center.target_;
  if (target && target->owner != std::this_thread::get_id()) {
    return std::unexpected(toastFailure({}, TreeError::WRONG_THREAD));
  }
  if (!target || !target->published || target->lifetime.expired()) {
    return std::unexpected(
      ManagedError{ManagedPhase::INPUT, target ? target->original : NodeId{}, TreeError::CLOSED, {}, {}});
  }
  if (target->scene->closed_) {
    return std::unexpected(ManagedError{ManagedPhase::INPUT, target->original, TreeError::CLOSED, {}, {}});
  }
  auto record = target->record.lock();
  if (!record || record->retired || record->root.id() != target->original) {
    return std::unexpected(ManagedError{ManagedPhase::INPUT, target->original, TreeError::STALE_NODE, {}, {}});
  }
  auto allowed = record->scene->boundary(ManagedPhase::INPUT, record->root);
  return allowed ? std::expected<std::shared_ptr<portals::detail::NotificationRecord>, ManagedError>(record)
                 : std::unexpected(allowed.error());
}

DialogCommandResult NotificationAccess::result(std::expected<void, ManagedError> value) {
  return DialogCommandResult(std::move(value));
}

void NotificationAccess::report(const NotificationCenter &center, ManagedError &failure, const char *operation,
                                std::source_location origin) noexcept {
  failure.operation = operation;
  failure.operationOrigin = origin;
  const auto &target = center.target_;
  if (!target || target->owner != std::this_thread::get_id() || target->lifetime.expired()) {
    return;
  }
  auto &scene = *target->scene;
  if (!scene.closed_ && !ManagedScene::focusReported(failure)) {
    scene.reportShortError(failure, operation, {}, "Toast", origin, scene.actionAdmitted_);
  }
}

DialogCommandResult NotificationAccess::command(const NotificationCenter &center, portals::detail::ToastCommand command,
                                                std::optional<ToastId> id, bool flag, std::source_location origin) {
  auto record = lock(center);
  auto value = record ? (*record)->scene->toastCommand(*record, command, id, flag)
                      : std::expected<void, ManagedError>(std::unexpected(record.error()));
  constexpr std::array<const char *, 5> OPERATIONS{"Toast.dismiss", "Toast.activate", "Toast.clear", "Toast.setVisible",
                                                   "Toast.setReducedMotion"};
  if (!value) {
    report(center, value.error(), OPERATIONS[static_cast<std::size_t>(command)], origin);
  }
  return result(std::move(value));
}
} // namespace portals::ui::detail

namespace portals::ui {
NotificationCenter::NotificationCenter() : target_(std::make_shared<detail::NotificationTarget>()) {
}

std::expected<ToastPostReceipt, ManagedError> NotificationCenter::post(const ToastMessage &message,
                                                                       std::source_location origin) const {
  auto record = detail::NotificationAccess::lock(*this);
  auto result = record ? (*record)->scene->postToast(*record, message)
                       : std::expected<ToastPostReceipt, ManagedError>(std::unexpected(record.error()));
  if (!result) {
    detail::NotificationAccess::report(*this, result.error(), "Toast.post", origin);
  }
  return result;
}

DialogCommandResult NotificationCenter::show(const ToastMessage &message, std::source_location origin) const {
  try {
    auto posted = post(message, origin);
    return detail::NotificationAccess::result(posted ? std::expected<void, ManagedError>{}
                                                     : std::unexpected(posted.error()));
  } catch (const std::bad_alloc &) {
    auto record = detail::NotificationAccess::lock(*this);
    auto failure = record ? toastFailure((*record)->root, TreeError::NO_MEMORY) : record.error();
    detail::NotificationAccess::report(*this, failure, "Toast.show", origin);
    return detail::NotificationAccess::result(std::unexpected(failure));
  }
}

DialogCommandResult NotificationCenter::show(const TextValue &text, std::source_location origin) const {
  try {
    return show(toast(text), origin);
  } catch (const std::bad_alloc &) {
    auto record = detail::NotificationAccess::lock(*this);
    auto failure = record ? toastFailure((*record)->root, TreeError::NO_MEMORY) : record.error();
    detail::NotificationAccess::report(*this, failure, "Toast.show", origin);
    return detail::NotificationAccess::result(std::unexpected(failure));
  }
}

DialogCommandResult NotificationCenter::show(std::string_view text, std::source_location origin) const {
  try {
    return show(TextValue(text), origin);
  } catch (const std::bad_alloc &) {
    auto record = detail::NotificationAccess::lock(*this);
    auto failure = record ? toastFailure((*record)->root, TreeError::NO_MEMORY) : record.error();
    detail::NotificationAccess::report(*this, failure, "Toast.show", origin);
    return detail::NotificationAccess::result(std::unexpected(failure));
  }
}

DialogCommandResult NotificationCenter::dismiss(ToastId id, std::source_location origin) const {
  return detail::NotificationAccess::command(*this, DISMISS, id, false, origin);
}

DialogCommandResult NotificationCenter::dismissLatest(std::source_location origin) const {
  return detail::NotificationAccess::command(*this, DISMISS, {}, false, origin);
}

DialogCommandResult NotificationCenter::activate(ToastId id, std::source_location origin) const {
  return detail::NotificationAccess::command(*this, ACTIVATE, id, false, origin);
}

DialogCommandResult NotificationCenter::activateLatest(std::source_location origin) const {
  return detail::NotificationAccess::command(*this, ACTIVATE, {}, false, origin);
}

DialogCommandResult NotificationCenter::clear(std::source_location origin) const {
  return detail::NotificationAccess::command(*this, CLEAR, {}, false, origin);
}

DialogCommandResult NotificationCenter::setVisible(bool visible, std::source_location origin) const {
  return detail::NotificationAccess::command(*this, VISIBLE, {}, visible, origin);
}

DialogCommandResult NotificationCenter::setReducedMotion(bool reduced, std::source_location origin) const {
  return detail::NotificationAccess::command(*this, REDUCED_MOTION, {}, reduced, origin);
}

std::expected<std::shared_ptr<const NotificationSnapshot>, ManagedError> NotificationCenter::snapshot() const {
  auto record = detail::NotificationAccess::lock(*this);
  auto result =
    record ? (*record)->scene->notificationSnapshot(*record)
           : std::expected<std::shared_ptr<const NotificationSnapshot>, ManagedError>(std::unexpected(record.error()));
  if (!result) {
    result.error().operation = "Toast.snapshot";
  }
  return result;
}
} // namespace portals::ui
