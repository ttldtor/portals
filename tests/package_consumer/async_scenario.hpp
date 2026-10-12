// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/async.hpp>
#include <portals/controls.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace async_example {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("example setup/operation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

inline void check(bool condition) {
  if (!condition) {
    throw std::runtime_error("example behavior differs from contract");
  }
}

inline std::string errorName(AsyncError error) {
  switch (error) {
  case AsyncError::CLOSED:
    return "closed";
  case AsyncError::WOULD_BLOCK:
    return "blocked";
  case AsyncError::CALLBACK_FAILED:
    return "provider-error";
  case AsyncError::CANCELLED:
    return "cancelled";
  case AsyncError::NO_MEMORY:
    return "no-memory";
  default:
    return "other-error";
  }
}

inline AsyncError mapError(SchedulerError error) {
  switch (error) {
  case SchedulerError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::WRONG_THREAD:
    return AsyncError::WRONG_THREAD;
  case SchedulerError::IDENTITY_EXHAUSTED:
    return AsyncError::IDENTITY_EXHAUSTED;
  default:
    return AsyncError::CLOSED;
  }
}

inline AsyncError mapError(TreeError error) {
  switch (error) {
  case TreeError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case TreeError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case TreeError::WRONG_THREAD:
    return AsyncError::WRONG_THREAD;
  case TreeError::CALLBACK_FAILED:
    return AsyncError::CALLBACK_FAILED;
  default:
    return AsyncError::CLOSED;
  }
}

using Result = std::expected<std::shared_ptr<const int>, AsyncError>;
using Completion = std::function<std::expected<PublishResult, AsyncError>(Result)>;

/** Bounded delayed provider; failed publication retains the exact owner for explicit retry. */
struct Provider {
  static constexpr std::size_t REQUEST_CAPACITY = 8;

  struct Request {
    Completion complete;
    std::optional<Result> pending;
  };

  std::array<std::optional<Request>, REQUEST_CAPACITY> requests;
  std::size_t count{};

  void start(Completion complete) {
    if (count == requests.size()) {
      throw std::runtime_error("finite provider request budget");
    }

    requests[count++].emplace(std::move(complete), std::nullopt);
  }

  std::expected<PublishResult, AsyncError> send(std::size_t index, Result value) {
    auto &request = *requests.at(index);
    request.pending = std::move(value);

    return retry(index);
  }

  std::expected<PublishResult, AsyncError> retry(std::size_t index) {
    auto &request = *requests.at(index);
    check(request.pending.has_value());
    auto sent = request.complete(*request.pending);

    if (sent || sent.error() == AsyncError::CLOSED) {
      request.pending.reset();
    }

    return sent;
  }

  void close() {
    for (auto &request : requests) {
      request.reset();
    }
  }
};

struct View {
  WidgetTree &tree;
  Node<Button> button;
  std::vector<std::string> trace;
  std::vector<int> values;

  void apply(Result result) {
    if (!result) {
      trace.push_back(errorName(result.error()));

      return;
    }

    values.push_back(**result);
    take(setControlText(tree, button, take(GraphemeText::from(std::to_string(**result)))));
    trace.push_back("value=" + std::to_string(**result));
  }
};

struct Harness {
  SchedulerTime now{};
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  std::unique_ptr<EventRouter> router;
  Node<Widget> root;
  Node<Button> button;
  std::shared_ptr<Provider> provider = std::make_shared<Provider>();
  std::shared_ptr<View> view;
  SchedulerConnection<int> noise;

  Harness() {
    SchedulerOptions limits;
    limits.data_capacity = 1;
    limits.data_budget = 1;
    limits.timer_budget = 1;
    limits.subscription_capacity = 8;
    auto clock = [](void *context) noexcept {
      return *static_cast<SchedulerTime *>(context);
    };
    scheduler = take(Scheduler::create(limits, {clock, &now}));
    tree = take(WidgetTree::create(*scheduler));
    root = take(tree->root());
    button = take(tree->emplace<Button>(root, take(GraphemeText::from("Load"))));
    router = take(EventRouter::create(*tree));
    view = std::make_shared<View>(*tree, button);
    take(tree->withScope(root, [&](const UiScope &scope) {
      noise = take(scheduler->connect<int>(scope, [](auto) {
      }));
    }));
  }

  void click() {
    take(router->invoke(button, {Action::ACTIVATE}));
  }

  void dispatchAt(SchedulerTime time) {
    now = time;
    take(scheduler->dispatch());
  }

  void cleanup() {
    provider->close();
    take(noise.subscription.disconnect());
    take(scheduler->stop());
  }
};

/** Complete common replay and cleanup; each Form supplies a real button binding and async workflow. */
template <class Form> void scenario() {
  Harness h;
  Form form(h);
  h.click();
  h.click();
  check(h.provider->count == 0);
  h.dispatchAt(10);
  check(h.provider->count == 1);
  take(h.noise.publisher.publish(std::make_shared<const int>(0)));
  auto rejected = h.provider->send(0, std::make_shared<const int>(7));
  check(!rejected && rejected.error() == AsyncError::WOULD_BLOCK);
  h.view->trace.push_back("queue-blocked");
  h.dispatchAt(10);
  take(h.provider->retry(0));
  check(h.view->values.empty());
  h.dispatchAt(10);
  check(h.view->values == std::vector<int>{7});
  check(form.outcome().status == UiTaskStatus::SUCCEEDED);

  h.click();
  h.dispatchAt(20);
  take(h.provider->send(1, std::unexpected(AsyncError::CALLBACK_FAILED)));
  h.dispatchAt(20);
  check(form.outcome().status == UiTaskStatus::FAILED);
  check(form.outcome().error == AsyncError::CALLBACK_FAILED);
  h.click();
  h.dispatchAt(30);
  h.click();
  form.restart = UiTaskRestart::CANCEL_PREVIOUS;
  h.click();
  auto late = h.provider->send(2, std::make_shared<const int>(100));
  check(!late && late.error() == AsyncError::CLOSED);
  h.view->trace.push_back("replaced-late-closed");
  h.dispatchAt(40);
  std::expected<PublishResult, AsyncError> sent = std::unexpected(AsyncError::CLOSED);
  std::thread worker([&] {
    sent = h.provider->send(3, std::make_shared<const int>(9));
  });
  worker.join();
  take(sent);
  check(h.view->values == std::vector<int>{7});
  h.dispatchAt(40);
  check(h.view->values == std::vector<int>{7, 9});
  h.click();
  h.dispatchAt(50);
  take(h.tree->remove(h.button));
  check(form.outcome().status == UiTaskStatus::CANCELLED);
  check(form.outcome().error == AsyncError::CLOSED);
  auto removed = h.provider->send(4, std::make_shared<const int>(200));
  check(!removed && removed.error() == AsyncError::CLOSED);
  h.view->trace.push_back("removed-late-closed");
  form.close();
  h.cleanup();
  check(h.scheduler->metrics().data.pending == 0);
  h.view->trace.push_back("cleanup");

  // Independent explicit close while data is pending; outcome survives the live context.
  Harness other;
  Form second(other);
  other.click();
  other.dispatchAt(10);
  second.close();
  check(second.outcome().status == UiTaskStatus::CANCELLED);
  check(second.outcome().error == AsyncError::CLOSED);
  auto closed = other.provider->send(0, std::make_shared<const int>(300));
  check(!closed && closed.error() == AsyncError::CLOSED);
  other.cleanup();
  h.view->trace.push_back("closed-late-closed");

  for (const auto &line : h.view->trace) {
    std::cout << line << '\n';
  }
}

template <class Form> int run() {
  try {
    scenario<Form>();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
} // namespace async_example
