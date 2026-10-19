// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "dialog_state.hpp"
#include <portals/message_box.hpp>

namespace portals::detail {
struct MessageResultHolder {
  std::optional<MessageResult> value;
};
struct MessageLease;
struct MessageRequest;

struct MessageRecord {
  std::shared_ptr<DialogRecord> dialog;
  std::array<Node<Button>, 3> buttons{};
  Node<Widget> paragraph;
  Node<Widget> actions;
  std::size_t count{};
  std::uint64_t nextId{};
  std::shared_ptr<MessageRequest> current;
  std::shared_ptr<const MessageResult> last;
};

struct MessageRequest {
  std::weak_ptr<MessageRecord> record;
  Node<Widget> dialog;
  Node<Widget> owner;
  std::uint64_t id{};
  std::shared_ptr<MessageResultHolder> holder;
  std::shared_ptr<const MessageResult> result;
  std::function<EditorCommandResult(const MessageResult &)> callback;
  std::source_location origin;
  SchedulerConnection<MessageResult> connection;
  AsyncReply<MessageResult> reply;
  AsyncInvocation invocation;
  std::weak_ptr<AsyncControl> context;
  std::weak_ptr<MessageLease> lease;
  std::optional<MessageChoice> choosing;
  bool async{};
  bool reserved{};
  bool starting{};
  bool queued{};
  bool admitted{};
  bool done{};
  bool released{};
  bool ownerError{};
  bool dialogError{};
  bool transportReported{};

  template <class Value> void latch(Value &&value) noexcept {
    if (result) {
      return;
    }
    static_assert(std::is_nothrow_constructible_v<MessageResult, Value>);
    holder->value.emplace(std::forward<Value>(value));
    std::shared_ptr<const MessageResult> immutable(holder, &*holder->value);
    holder.reset();
    result = std::move(immutable);
    if (auto live = record.lock(); live && live->current.get() == this) {
      live->last = result;
    }
  }
};

struct MessageLease {
  std::weak_ptr<MessageRequest> request;
  Publisher<int> wake;

  ~MessageLease() {
    if (auto live = request.lock()) {
      live->released = true;
    }
    wake.wake();
  }
};

class MessageParagraph final : public SizedWidget {
public:
  MessageParagraph(GraphemeText text, std::size_t maximum);
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  std::expected<void, TreeError> arrange(CellRect local) override;
  std::expected<void, TreeError> paint(PaintContext &context) override;

private:
  GraphemeText text_;
  std::size_t maximum_;
  std::optional<std::int32_t> width_;
  std::vector<std::optional<GraphemeText>> lines_ = std::vector<std::optional<GraphemeText>>(0);
  std::int32_t measuredWidth_{};
  CellRect bounds_{};
};
} // namespace portals::detail
