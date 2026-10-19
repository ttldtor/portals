// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/toast.hpp>
#include <portals/layout.hpp>
#include <array>

namespace portals::detail {
struct NotificationRecord;

struct ToastPayload {
  std::shared_ptr<const ToastData> data;
  ToastOptions options;
  std::shared_ptr<std::function<std::expected<void, TreeError>(const ToastActionEvent &)>> action;
  std::shared_ptr<const GraphemeText> indicator;
};

struct ToastEntry {
  ToastId id;
  std::shared_ptr<const ToastPayload> payload;
  std::optional<SchedulerTime> deadline;
  bool firstPresented{};
};

struct ToastPresentation {
  ToastId id;
  std::shared_ptr<const ToastPayload> payload;
  GraphemeText summary;
  GraphemeText action;
  // Panel, message, action, dismiss and indicator values form one no-fail publication.
  std::array<CellStyle, 5> styles{};
  std::int32_t width{};
  bool alert{};
};

// Widgets pin only immutable drawing data, never scene pointers or action registrations.
struct ToastSlot {
  Node<Widget> node;
  std::shared_ptr<const ToastPresentation> picture;
  std::uint64_t assignment{};
};

struct ToastTimerMarker {
  std::uint64_t generation{};
  bool started{};
  bool committed{};
  bool registrationAlive{true};
};

struct ToastTimerLifetime {
  std::shared_ptr<ToastTimerMarker> marker;

  ~ToastTimerLifetime() {
    marker->registrationAlive = false;
  }
};

struct NotificationRecord {
  const std::thread::id owner = std::this_thread::get_id();
  ManagedScene *scene{};
  std::weak_ptr<const void> lifetime;
  Node<Widget> wrapper;
  Node<Widget> body;
  Node<Widget> root;
  std::shared_ptr<const NotificationOptions> options;
  std::vector<ToastEntry> entries = std::vector<ToastEntry>(0);
  std::vector<std::shared_ptr<ToastSlot>> slots = std::vector<std::shared_ptr<ToastSlot>>(0);
  std::array<StyleTarget, 8> targets;
  std::shared_ptr<const GraphemeText> ellipsis;
  std::shared_ptr<const GraphemeText> dismiss;
  std::array<std::shared_ptr<const GraphemeText>, 4> severity;
  Subscription timer;
  std::shared_ptr<ToastTimerMarker> timerMarker;
  std::optional<SchedulerTime> timerDeadline;
  std::vector<ToastId> runningActions = std::vector<ToastId>(0);
  std::uint64_t version{1};
  std::uint64_t nextId{1};
  std::uint64_t timerGeneration{1};
  std::uint64_t styleRevision{};
  std::uint64_t dropped{};
  std::uint64_t evicted{};
  std::int32_t width{-1};
  bool visible{true};
  bool reducedMotion{};
  bool staging{};
  bool ready{};
  bool retired{};
};

class ToastLayer : public Widget, public Layout {
public:
  explicit ToastLayer(std::shared_ptr<NotificationRecord> record) : record_(std::move(record)) {
  }

  void unmount() override;
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;

private:
  std::shared_ptr<NotificationRecord> record_;
};

class ToastRow : public Widget {
public:
  ToastRow(std::shared_ptr<ToastSlot> slot, std::shared_ptr<NotificationRecord> record)
      : slot_(std::move(slot)), record_(std::move(record)) {
  }

  std::expected<CellSize, TreeError> measure(CellSize available) override;
  std::expected<void, TreeError> arrange(CellRect local) override;
  std::expected<void, TreeError> paint(PaintContext &context) override;

private:
  std::shared_ptr<ToastSlot> slot_;
  std::shared_ptr<NotificationRecord> record_;
  CellRect bounds_;
};

struct ToastSlotObservation {
  NodeGeometry geometry;
  std::uint64_t inputRevision{};
  bool eligible{};
  bool available{true};
};

struct ToastCandidate {
  std::vector<ToastEntry> entries = std::vector<ToastEntry>(0);
  std::vector<std::shared_ptr<const ToastPresentation>> pictures =
    std::vector<std::shared_ptr<const ToastPresentation>>(0);
  std::vector<ToastSlotObservation> observations = std::vector<ToastSlotObservation>(0);
  std::vector<std::uint64_t> assignments = std::vector<std::uint64_t>(0);
  std::shared_ptr<ToastSlot> actionSlot;
  ToastId actionId;
  std::uint64_t actionInputRevision{};
  std::uint64_t actionAssignment{};
  Subscription timer;
  std::shared_ptr<ToastTimerMarker> marker;
  std::shared_ptr<ToastTimerMarker> previousMarker;
  std::optional<SchedulerTime> deadline;
  std::uint64_t expectedVersion{};
  std::uint64_t inputRevision{};
  std::uint64_t geometryRevision{};
  std::uint64_t styleRevision{};
  std::uint64_t dropped{};
  std::uint64_t evicted{};
  std::uint64_t nextId{};
  std::int32_t width{};
  bool visible{};
  bool reducedMotion{};
  bool replaceTimer{};
  bool geometryChanged{};
  bool paintChanged{};
  bool previousTimerStarted{};
  bool previousRegistrationAlive{};
  bool eligible{};
};
} // namespace portals::detail

namespace portals::ui::detail {
struct NotificationTarget {
  const std::thread::id owner = std::this_thread::get_id();
  ManagedScene *scene{};
  std::weak_ptr<portals::detail::NotificationRecord> record;
  std::weak_ptr<const void> lifetime;
  NodeId original{};
  bool published{};
  bool reserved{};
};

} // namespace portals::ui::detail
