// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/examples/replay/terminal.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <portals/application.hpp>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace {
using namespace portals;

void check(bool value, std::source_location where = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("runtime session check line " + std::to_string(where.line()));
  }
}

template <class T, class E> T take(std::expected<T, E> value) {
  check(bool(value));

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

class ReadyTerminal final : public ApplicationReadyTerminal {
public:
  SchedulerClock clock() noexcept {
    return {[](void *context) noexcept {
              return static_cast<ReadyTerminal *>(context)->time;
            },
            this};
  }

  TerminalRead poll(std::stop_token) override {
    ++polls;

    if (input.empty()) {
      return {TerminalStatus::DEADLINE};
    }

    auto batch = std::move(input.front());
    input.pop_front();

    return batch;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{12, 3};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    check(pending.empty());
    ++submissions;
    pending = std::move(bytes);

    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    ++flushes;

    if (failFlush) {
      return {TerminalStatus::FAILED, 0, pending.size(), TerminalError{TerminalErrorKind::OUTPUT_UNCERTAIN, 91}};
    }

    const auto count = std::min(chunk, pending.size());
    output.append(pending, 0, count);
    pending.erase(0, count);

    return {pending.empty() ? TerminalStatus::PROGRESS : TerminalStatus::DEADLINE, count, pending.size(), {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    ++closes;

    if (failClose) {
      return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 17});
    }

    return {};
  }

  void bytes(std::string value) {
    input.push_back({TerminalStatus::PROGRESS, std::move(value)});
  }

  void eof() {
    input.push_back({TerminalStatus::END_OF_INPUT});
  }

  SchedulerTime time{};
  std::deque<TerminalRead> input;
  std::string output, pending;
  std::size_t chunk{std::numeric_limits<std::size_t>::max()}, polls{}, flushes{}, submissions{}, closes{};
  bool failFlush{}, failClose{};
};

class Scene final : public ApplicationScene {
public:
  std::expected<bool, TreeError> input(Application &app, const InputEvent &event) override {
    events.push_back(event);

    if (reenter) {
      auto nested = app.step();
      check(!nested && std::get<ApplicationFailure>(nested.error().cause) == ApplicationFailure::REENTRANT_OPERATION);
      ++rejections;
    }

    if (stopOnInput) {
      app.requestStop();
    }

    take(app.tree().invalidate(app.root(), Invalidation::PAINT));

    return true;
  }

  void stop(Application &) override {
    ++stops;
  }

  std::vector<InputEvent> events;
  std::size_t stops{}, rejections{};
  bool stopOnInput{}, reenter{};
};

void idleAndLifecycle() {
  ReadyTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  Scene scene;
  check(!app->step());
  take(app->start(scene, terminal));
  auto idle = take(app->step());
  check(!idle.finished && idle.stats.frames == 1 && terminal.polls == 1 && terminal.flushes == 1);
  idle = take(app->step());
  check(!idle.finished && idle.stats.frames == 1 && terminal.polls == 2 && terminal.flushes == 1);
  check(!app->start(scene, terminal) && !app->run(scene));

  std::thread wrong([&] {
    auto rejected = app->step();
    check(!rejected && std::get<TreeError>(rejected.error().cause) == TreeError::WRONG_THREAD);
  });
  wrong.join();
  check(terminal.closes == 0 && scene.stops == 0);
  app->requestStop();
  auto stopped = take(app->step());
  check(stopped.finished && terminal.closes == 1 && scene.stops == 1);
  check(take(app->step()).finished && terminal.closes == 1 && scene.stops == 1);
}

void decoderAndBudget() {
  ReadyTerminal terminal;
  ApplicationOptions options;
  options.scheduler.control_capacity = options.scheduler.control_budget = 1;
  options.inputBudget = 2;
  auto app = take(Application::create(options, terminal.clock()));
  Scene scene;
  take(app->start(scene, terminal));
  terminal.bytes("abcdef");
  take(app->step());
  terminal.bytes("\xe7\x95");
  terminal.bytes("\x8c");
  terminal.bytes("\x1b");

  for (std::size_t i = 0; i < 40; ++i) {
    take(app->step());
  }

  terminal.time = 1000;
  take(app->step());
  terminal.eof();
  bool done{};

  for (std::size_t i = 0; i < 40 && !done; ++i) {
    done = take(app->step()).finished;
  }

  check(done && scene.events.size() == 8 && terminal.closes == 1);
  check(std::get<TextInputEvent>(scene.events[6]).scalar == U'界');
  check(std::get<KeyEvent>(scene.events[7]).key == InputKey::ESCAPE);
  check(app->scheduler().metrics().control.rejected > 0);
}

void frameCapAndEofCutoff() {
  ReadyTerminal terminal;
  auto app = take(Application::create({}, terminal.clock()));
  Scene scene;
  take(app->start(scene, terminal));
  check(take(app->step()).stats.frames == 1);
  terminal.time = 1;
  terminal.bytes("x");
  check(take(app->step()).stats.frames == 1);
  terminal.time = 33;
  check(take(app->step()).stats.frames == 1);
  terminal.time = 34;
  check(take(app->step()).stats.frames == 2);
  terminal.time = 35;
  terminal.bytes("y");
  take(app->step());
  terminal.eof();
  auto ended = take(app->step());
  check(ended.finished && ended.stats.frames == 2 && ended.stats.inputs == 2);
}

void partialAndFailures() {
  ReadyTerminal terminal;
  terminal.chunk = 2;
  auto app = take(Application::create({}, terminal.clock()));
  Scene scene;
  take(app->start(scene, terminal));
  auto first = take(app->step());
  check(first.stats.bytes == 2 && !terminal.pending.empty());
  terminal.eof();
  bool done{};

  for (std::size_t i = 0; i < 1000 && !done; ++i) {
    done = take(app->step()).finished;
  }

  check(done && terminal.submissions == 1 && terminal.pending.empty() && terminal.closes == 1);
  check(app->stats().bytes == terminal.output.size());

  ReadyTerminal failed;
  failed.failFlush = failed.failClose = true;
  auto failing = take(Application::create({}, failed.clock()));
  Scene failureScene;
  take(failing->start(failureScene, failed));
  auto error = failing->step();
  check(!error && error.error().phase == ApplicationPhase::FLUSH && error.error().restoration);
  check(std::get<TerminalError>(error.error().cause).kind == TerminalErrorKind::OUTPUT_UNCERTAIN);
  check(!failing->step() && failed.flushes == 1 && failed.closes == 1 && failureScene.stops == 1);
}

void cancellationAndDestruction() {
  ReadyTerminal terminal;
  Scene scene;
  {
    auto app = take(Application::create({}, terminal.clock()));
    take(app->start(scene, terminal));
    take(app->step());
  }
  check(terminal.closes == 1 && scene.stops == 1);

  ReadyTerminal cancelled;
  auto app = take(Application::create({}, cancelled.clock()));
  Scene cancelledScene;
  std::stop_source stop;
  stop.request_stop();
  take(app->start(cancelledScene, cancelled, stop.get_token()));
  check(take(app->step()).finished && cancelled.polls == 0 && cancelled.closes == 1 && cancelledScene.stops == 1);

  ReadyTerminal later;
  auto laterApp = take(Application::create({}, later.clock()));
  Scene laterScene;
  std::stop_source laterStop;
  take(laterApp->start(laterScene, later, laterStop.get_token()));
  take(laterApp->step());
  laterStop.request_stop();
  check(take(laterApp->step()).finished && later.polls == 1 && later.closes == 1 && laterScene.stops == 1);

  ReadyTerminal callback;
  auto nested = take(Application::create({}, callback.clock()));
  Scene nestedScene;
  nestedScene.reenter = nestedScene.stopOnInput = true;
  take(nested->start(nestedScene, callback));
  callback.bytes("x");
  check(take(nested->step()).finished && nestedScene.rejections == 1 && callback.closes == 1);
}

void nativeTraceEquivalence() {
  example::ReplayTerminal legacy({{50, "ab", {}}, {100, "c", CellSize{20, 4}}}, 200, {12, 3});
  auto native = take(Application::create({}, legacy.clock()));
  Scene nativeScene;
  const auto nativeStats = take(native->run(nativeScene, legacy));
  ReadyTerminal ready;
  auto stepped = take(Application::create({}, ready.clock()));
  Scene steppedScene;
  take(stepped->start(steppedScene, ready));
  ApplicationStep result;

  for (ready.time = 10; ready.time <= 200; ready.time += 10) {
    if (ready.time == 50) {
      ready.bytes("ab");
    } else if (ready.time == 100) {
      ready.input.push_back({TerminalStatus::PROGRESS, "c", {}, CellSize{20, 4}});
    } else if (ready.time == 200) {
      ready.eof();
    }

    result = take(stepped->step());
  }

  check(result.finished && result.stats.inputs == nativeStats.inputs && result.stats.frames == nativeStats.frames);
  check(result.stats.bytes == nativeStats.bytes && ready.output == legacy.output);
  check(nativeScene.events == steppedScene.events && nativeScene.stops == steppedScene.stops);
}
} // namespace

int main() {
  idleAndLifecycle();
  decoderAndBudget();
  frameCapAndEofCutoff();
  partialAndFailures();
  cancellationAndDestruction();
  nativeTraceEquivalence();
}
