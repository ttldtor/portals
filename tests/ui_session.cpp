// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../examples/gallery/fluent_form.hpp"
#include <portals/ui.hpp>

#include <deque>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace {
using namespace portals;

void check(bool value, std::source_location where = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("ui session check line " + std::to_string(where.line()));
  }
}

template <class T, class E> T take(std::expected<T, E> value) {
  check(bool(value));

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

template <class T> void rejected(std::expected<T, ManagedError> value, TreeError cause) {
  check(!value);

  const auto *actual = std::get_if<TreeError>(&value.error().cause);

  check(actual && *actual == cause);
}

void noCurrent() {
  check(ui::detail::CurrentSceneScope::snapshot().expired());
  rejected(ui::detail::SceneAccess::currentRef().quit(), TreeError::CLOSED);
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

    auto result = std::move(input.front());

    input.pop_front();

    return result;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{80, 24};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    check(pending.empty());
    pending = std::move(bytes);
    ++submissions;

    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    const auto count = pending.size();

    output += pending;
    pending.clear();

    return {TerminalStatus::PROGRESS, count, 0, {}};
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

  SchedulerTime time{};
  std::deque<TerminalRead> input;
  std::string pending, output;
  int polls{}, submissions{}, closes{};
  bool failClose{};
};

ui::SceneOptions policies(ReadyTerminal &terminal) {
  ui::SceneOptions result;

  result.clock = terminal.clock();

  return result;
}

void progressAndContext() {
  ReadyTerminal terminal;
  ui::Ref<Label> label;
  auto scene = ui::scene(ui::label(label, "session"), policies(terminal));
  ManagedScene *managed{};
  int stops{}, keys{};

  scene.configure([&](ManagedScene &owner) {
    managed = &owner;
  });
  scene.onStop([&] {
    ++stops;
    noCurrent();
    check(take(label.text().get()) == "session");
  });
  scene.onKey("q", [&] {
    ++keys;
    check(!ui::detail::CurrentSceneScope::snapshot().expired());
    rejected(scene.step(), TreeError::WOULD_BLOCK);
    rejected(managed->step(), TreeError::WOULD_BLOCK);
    rejected(scene.start(terminal), TreeError::WOULD_BLOCK);
    rejected(managed->start(terminal), TreeError::WOULD_BLOCK);
    rejected(scene.close(), TreeError::WOULD_BLOCK);
    rejected(managed->close(), TreeError::WOULD_BLOCK);
    ui::quit();
  });
  rejected(scene.step(), TreeError::INVALID_ARGUMENT);
  take(scene.start(terminal));
  noCurrent();
  rejected(scene.close(), TreeError::WOULD_BLOCK);
  rejected(managed->close(), TreeError::WOULD_BLOCK);
  rejected(scene.start(terminal), TreeError::WOULD_BLOCK);
  check(!take(scene.step()).finished);
  check(terminal.submissions == 1 && !terminal.output.empty());
  check(!take(scene.step()).finished);
  check(terminal.submissions == 1);
  noCurrent();

  std::thread worker([&] {
    rejected(scene.step(), TreeError::WRONG_THREAD);
    rejected(scene.start(terminal), TreeError::WRONG_THREAD);
    rejected(managed->step(), TreeError::WRONG_THREAD);
  });

  worker.join();
  terminal.bytes("q");

  auto finished = take(scene.step());

  check(finished.finished && stops == 1 && keys == 1 && terminal.closes == 1);
  check(take(scene.step()).stats.iterations == finished.stats.iterations);
  check(take(managed->step()).finished);
  check(stops == 1 && terminal.closes == 1);
  take(scene.checked());
  take(scene.close());
  rejected(scene.start(terminal), TreeError::CLOSED);
  noCurrent();
}

void activeDestructionAndMove() {
  ReadyTerminal terminal;
  int stops{};

  {
    auto original = ui::scene(ui::label("move"), policies(terminal));

    original.onStop([&] {
      ++stops;
      noCurrent();
    });
    take(original.start(terminal));
    check(!take(original.step()).finished);

    auto moved = std::move(original);

    rejected(original.step(), TreeError::CLOSED);
    check(!take(moved.step()).finished);
    noCurrent();
  }

  check(stops == 1 && terminal.closes == 1);
  noCurrent();

  ReadyTerminal directTerminal;
  auto app = take(Application::create({}, directTerminal.clock()));

  {
    auto managed = take(ManagedScene::create(*app));

    take(managed->start(directTerminal));
    check(!take(managed->step()).finished);
  }

  check(directTerminal.closes == 1);
}

void errorsAndCancellation() {
  ReadyTerminal terminal;
  int stops{}, reports{};
  auto scene = ui::scene(ui::label("failure"), policies(terminal));

  scene.onStop([&] {
    ++stops;
    noCurrent();
  });
  scene.onError([&](const ManagedDiagnostic &) {
    ++reports;
  });
  scene.onKey("x", [] {
    throw std::runtime_error("callback");
  });
  take(scene.start(terminal));
  take(scene.step());
  terminal.failClose = true;
  terminal.bytes("x");

  auto failed = scene.step();

  check(!failed && reports == 1 && stops == 1 && terminal.closes == 1);
  check(failed.error().application.has_value());
  check(!scene.step() && reports == 1 && stops == 1 && terminal.closes == 1);
  noCurrent();

  ReadyTerminal cancelledTerminal;
  std::stop_source cancellation;
  int cancelledStops{};
  auto cancelled = ui::scene(ui::label("cancelled"), policies(cancelledTerminal));

  cancelled.onStop([&] {
    ++cancelledStops;
  });
  cancellation.request_stop();
  take(cancelled.start(cancelledTerminal, cancellation.get_token()));
  check(take(cancelled.step()).finished);
  check(cancelledStops == 1 && cancelledTerminal.closes == 1 && cancelledTerminal.polls == 0);
}

void setupAndPrepareFailures() {
  for (const bool duringPrepare : {false, true}) {
    ReadyTerminal terminal;
    int stops{}, reports{};
    auto description = ui::label("failure").onPrepare([](Label &, Node<Widget>) -> std::expected<void, TreeError> {
      throw std::runtime_error("prepare");
    });
    auto scene = ui::scene(std::move(description), policies(terminal));

    scene.onStop([&] {
      ++stops;
      noCurrent();
    });
    scene.onError([&](const ManagedDiagnostic &) {
      ++reports;
    });

    if (!duringPrepare) {
      scene.configure([](ManagedScene &) {
        throw std::runtime_error("setup");
      });
    }

    auto started = scene.start(terminal);

    if (duringPrepare) {
      take(started);

      auto result = scene.step();

      check(!result && stops == 1 && reports == 1 && terminal.closes == 1);
      check(!scene.step() && reports == 1 && terminal.closes == 1);
    } else {
      check(!started && stops == 1 && reports == 1 && terminal.closes == 0);
      check(!scene.step() && !scene.start(terminal));
      check(stops == 1 && reports == 1 && terminal.closes == 0);
    }

    noCurrent();
  }
}

void galleryReuse() {
  ReadyTerminal terminal;
  example::Options options;
  auto scene = gallery::form(ui::OptionsRef(options), {}, policies(terminal));

  take(scene.start(terminal));
  check(!take(scene.step()).finished);
  check(terminal.output.find("Portals gallery") != std::string::npos);
  terminal.bytes("\x1b[21~");
  check(take(scene.step()).finished);
  check(terminal.closes == 1);
  noCurrent();
}
} // namespace

int main() {
  progressAndContext();
  activeDestructionAndMove();
  errorsAndCancellation();
  setupAndPrepareFailures();
  galleryReuse();
}
