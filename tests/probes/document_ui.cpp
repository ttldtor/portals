// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
// Internal qualification executable. Run portals_document_ui_probe --help for its bounded workload.
#include <portals/ui.hpp>
#include <portals/ui_lyra.hpp>
#ifdef PORTALS_PROBE_HAS_GZIP
#  include <portals/gzip_document.hpp>
#endif
#include "document_probe_metrics.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace {
using namespace portals;
using document_probe::ProcessMemory;
using document_probe::Samples;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t MIB = 1024 * 1024;
constexpr std::uint64_t FOUR_GIB = 4096 * MIB;
constexpr std::size_t PHASE_COUNT = 11;
constexpr std::uint64_t TIMER_MS = 10;

struct Options {
  std::string file, kind{"plain"}, language{"plain"};
  unsigned views{1}, budgetMiB{128}, timeoutMs{60000}, samples{8192};
  unsigned tailWorkTurns{DocumentViewConfig{}.tailWorkTurns};
  bool idle{}, help{};
};

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("checked probe operation rejected");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

std::uint64_t elapsedNs(Clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

std::filesystem::path literalPath(std::string_view utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// The adapter retains only the current owning output transaction, never a VT transcript.
class SinkTerminal final : public ApplicationTerminal {
public:
  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    while (!stop.stop_requested() && Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TerminalRead result;
    result.status = stop.stop_requested() ? TerminalStatus::STOPPED : TerminalStatus::DEADLINE;

    return result;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{120, 40};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    if (!pending_.empty()) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_PENDING});
    }

    pending_ = std::move(bytes);

    return {};
  }

  TerminalWrite flush(TerminalDeadline, std::stop_token) override {
    const auto count = pending_.size();
    outputBytes += count;
    pending_.clear();

    return {TerminalStatus::PROGRESS, count, 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    pending_.clear();
    closed = true;

    return {};
  }

  std::uint64_t outputBytes{};
  bool closed{};

private:
  std::string pending_;
};

// Both wrappers share the original provider. The outer one measures logical admission;
// the inner one measures physical compressed reads when gzip is selected.
class CountedProvider final : public ByteDocumentProvider, public std::enable_shared_from_this<CountedProvider> {
public:
  explicit CountedProvider(std::shared_ptr<ByteDocumentProvider> provider) : provider_(std::move(provider)) {
  }

  ByteDocumentCapabilities capabilities() const noexcept override {
    return provider_->capabilities();
  }

  std::size_t pinnedBudgetBytes(const SourceBudget &root) const noexcept override {
    return provider_->pinnedBudgetBytes(root);
  }

  bool tryReleaseOptionalStorage() noexcept override {
    return provider_->tryReleaseOptionalStorage();
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token stop) override {
    checkThread();
    ++metadataCalls;

    return provider_->metadata(stop);
  }

  std::expected<ByteDocumentMetadata, DocumentError> discoverExtent(std::stop_token stop) override {
    checkThread();
    ++discoveryCalls;

    return provider_->discoverExtent(stop);
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token stop) override {
    checkThread();
    ++readCalls;
    auto result = provider_->read(command, destination, stop);

    if (result) {
      admittedBytes += result->bytesRead;
    }

    return result;
  }

  std::atomic<std::uint64_t> readCalls{}, admittedBytes{}, metadataCalls{}, discoveryCalls{}, uiIoViolations{};

  class ReadSession final : public ByteDocumentReadSession {
  public:
    ReadSession(std::shared_ptr<CountedProvider> owner, std::unique_ptr<ByteDocumentReadSession> session)
        : owner_(std::move(owner)), session_(std::move(session)) {
    }

    ByteDocumentMetadata metadata() const noexcept override {
      return session_->metadata();
    }

    std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                      std::stop_token stop) override {
      owner_->sessionThreadCheck();
      ++owner_->readCalls;
      auto result = session_->read(command, destination, stop);
      if (result) {
        owner_->admittedBytes += result->bytesRead;
      }

      return result;
    }

    std::expected<void, DocumentError> seal(std::stop_token stop) override {
      owner_->sessionThreadCheck();
      ++owner_->metadataCalls;
      return session_->seal(stop);
    }

  private:
    std::shared_ptr<CountedProvider> owner_;
    std::unique_ptr<ByteDocumentReadSession> session_;
  };

  std::expected<std::unique_ptr<ByteDocumentReadSession>, DocumentError>
  openReadSession(std::stop_token stop) override {
    sessionThreadCheck();
    ++metadataCalls;
    auto opened = provider_->openReadSession(stop);
    if (!opened) {
      return std::unexpected(opened.error());
    }

    return std::make_unique<ReadSession>(shared_from_this(), std::move(*opened));
  }

private:
  void sessionThreadCheck() {
    checkThread();
  }

  void checkThread() {
    if (std::this_thread::get_id() == ui_) {
      ++uiIoViolations;
    }
  }

  std::shared_ptr<ByteDocumentProvider> provider_;
  const std::thread::id ui_ = std::this_thread::get_id();
};

struct Measurements {
  explicit Measurements(std::size_t capacity) : timerLateness(capacity) {
    phases.reserve(PHASE_COUNT);

    for (std::size_t index = 0; index < PHASE_COUNT; ++index) {
      phases.emplace_back(capacity);
    }
  }

  static void observe(void *context, ApplicationPhase phase, std::chrono::nanoseconds elapsed) {
    auto &self = *static_cast<Measurements *>(context);
    self.phases[static_cast<std::size_t>(phase)].add(static_cast<std::uint64_t>(elapsed.count()));

    if (phase == ApplicationPhase::SUBMIT) {
      // Application observes SUBMIT only after FrameWriter::submit succeeds. This
      // records accepted output, which can still be the initial loading frame.
      ++self.frames;

      if (!self.firstAcceptedOutputFrameNs) {
        self.firstAcceptedOutputFrameNs = elapsedNs(self.origin);
      }
    }
  }

  Clock::time_point origin{Clock::now()};
  std::vector<Samples> phases;
  Samples timerLateness;
  std::uint64_t frames{}, timerCallbacks{}, budgetPeakBytes{};
  std::optional<std::uint64_t> firstAcceptedOutputFrameNs;
  std::optional<std::uint64_t> initialTextReadyObservedNs, initialTextReadyObservationUncertaintyNs;
};

struct Stage {
  std::string_view name;
  std::uint64_t offset{};
  std::optional<std::uint64_t> firstOutputFrameObservedNs, settledNs;
  std::array<DocumentViewStatus, 4> status{};
  unsigned observedViews{};
  bool started{}, completed{}, skipped{}, commandRejected{};
};

class Workload {
public:
  Workload(const Options &options, Measurements &metrics, std::shared_ptr<SourceBudget> budget)
      : options_(options), metrics_(metrics), budget_(std::move(budget)) {
    stages = {{"initial"},
              {"scroll_rows"},
              {"page_down"},
              {"home"},
              {"seek_4096", 4096},
              {"end"},
              {"before_4gib", FOUR_GIB - 128},
              {"at_4gib", FOUR_GIB},
              {"after_4gib", FOUR_GIB + 128},
              {"home_again"}};
  }

  void begin(Application &application) {
    app_ = &application;
    scope_ = take(app_->scheduler().makeScope());
    stageStarted_ = Clock::now();
    framesBefore_ = metrics_.frames;
    stages[0].started = true;
    arm();
  }

  void cancelTimer() {
    timer_ = {};
    scope_ = {};
    budget_.reset();
  }

  std::array<ui::Ref<DocumentView>, 4> readers;
  std::vector<Stage> stages;
  bool complete{}, failed{}, timedOut{};

private:
  void arm() {
    due_ = app_->now() + TIMER_MS;
    timer_ = take(app_->scheduler().at(scope_, due_, [this] {
      tick();
    }));
  }

  void tick() {
    ++metrics_.timerCallbacks;
    metrics_.timerLateness.add((app_->now() > due_ ? app_->now() - due_ : 0) * 1000000);
    metrics_.budgetPeakBytes = std::max<std::uint64_t>(metrics_.budgetPeakBytes, budget_->usage().bytes);

    if (elapsedNs(metrics_.origin) >= static_cast<std::uint64_t>(options_.timeoutMs) * 1000000) {
      timedOut = true;
      app_->requestStop();

      return;
    }

    auto &stage = stages[current_];
    bool ready = metrics_.frames > framesBefore_;

    if (ready && !stage.firstOutputFrameObservedNs) {
      stage.firstOutputFrameObservedNs = elapsedNs(stageStarted_);
    }

    for (unsigned index = 0; index < (options_.idle ? 0 : options_.views); ++index) {
      auto status = readers[index].status();

      if (!status) {
        failed = true;
        app_->requestStop();

        return;
      }

      stage.status[index] = *status;
      stage.observedViews = index + 1;
      if (status->byteExtent) {
        extent_ = status->byteExtent;
      }
      ready = ready && !status->preparing && !status->waiting && !status->backpressured;

      if (status->error) {
        failed = true;
        app_->requestStop();

        return;
      }
    }

    if (current_ == 0 && !options_.idle) {
      const auto observedNs = elapsedNs(metrics_.origin);

      if (ready) {
        // Observe readiness from the ordinary scheduler timer, never by reentering
        // Application from its observer. The sampling gap bounds uncertainty;
        // this is not a timestamp of the first rendered document-content frame.
        metrics_.initialTextReadyObservedNs = observedNs;
        metrics_.initialTextReadyObservationUncertaintyNs = observedNs - lastInitialObservationNs_;
      } else {
        lastInitialObservationNs_ = observedNs;
      }
    }

    if (!ready) {
      arm();

      return;
    }

    stage.completed = true;
    stage.settledNs = elapsedNs(stageStarted_);

    if (options_.idle || ++current_ == stages.size()) {
      complete = true;
      app_->requestStop();

      return;
    }

    // Navigation is admitted only after the previous intent produced a prepared frame.
    while (current_ < stages.size() && shouldSkip(stages[current_])) {
      stages[current_++].skipped = true;
    }

    if (current_ == stages.size()) {
      complete = true;
      app_->requestStop();

      return;
    }

    stageStarted_ = Clock::now();
    framesBefore_ = metrics_.frames;
    stages[current_].started = true;

    for (unsigned index = 0; index < options_.views; ++index) {
      if (!navigate(readers[index])) {
        stages[current_].commandRejected = true;
        failed = true;
        app_->requestStop();

        return;
      }
    }

    arm();
  }

  bool shouldSkip(const Stage &stage) const {
    if (!stage.offset) {
      return false;
    }

    return !extent_ || stage.offset >= *extent_;
  }

  std::expected<void, ManagedError> navigate(const ui::Ref<DocumentView> &reader) {
    switch (current_) {
    case 1:
      return reader.scrollRows(1);
    case 2:
      return reader.scrollPages(1);
    case 3:
    case 9:
      return reader.goHome();
    case 5:
      return reader.goEnd();
    default:
      return reader.seekByte(stages[current_].offset);
    }
  }

  const Options &options_;
  Measurements &metrics_;
  std::shared_ptr<SourceBudget> budget_;
  Application *app_{};
  UiScope scope_;
  Subscription timer_;
  Clock::time_point stageStarted_{};
  std::uint64_t due_{}, framesBefore_{}, lastInitialObservationNs_{};
  std::size_t current_{};
  std::optional<std::uint64_t> extent_;
};

void optionalNumber(std::ostream &out, const std::optional<std::uint64_t> &value) {
  if (value) {
    out << *value;
  } else {
    out << "null";
  }
}

void memoryJson(std::ostream &out, const ProcessMemory &memory) {
  out << "{\"private_bytes\":";
  optionalNumber(out, memory.privateBytes);
  out << ",\"resident_bytes\":";
  optionalNumber(out, memory.residentBytes);
  out << ",\"peak_private_bytes\":";
  optionalNumber(out, memory.peakPrivateBytes);
  out << ",\"peak_resident_bytes\":";
  optionalNumber(out, memory.peakResidentBytes);
  out << ",\"cpu_ns\":";
  optionalNumber(out, memory.cpuNs);
  out << ",\"error\":" << memory.error << '}';
}

void samplesJson(std::ostream &out, const Samples &samples) {
  const bool valid = samples.retained() != 0 && samples.complete();
  out << "{\"count\":" << samples.count() << ",\"retained_prefix\":" << samples.retained()
      << ",\"overflow\":" << samples.count() - samples.retained() << ",\"valid\":" << valid << ",\"p50_ns\":";
  optionalNumber(out, valid ? std::optional(samples.percentile(.50)) : std::nullopt);
  out << ",\"p95_ns\":";
  optionalNumber(out, valid ? std::optional(samples.percentile(.95)) : std::nullopt);
  out << '}';
}

void providerJson(std::ostream &out, const std::shared_ptr<CountedProvider> &provider) {
  out << "{\"read_calls\":" << (provider ? provider->readCalls.load() : 0)
      << ",\"admitted_bytes\":" << (provider ? provider->admittedBytes.load() : 0)
      << ",\"metadata_calls\":" << (provider ? provider->metadataCalls.load() : 0)
      << ",\"extent_discovery_calls\":" << (provider ? provider->discoveryCalls.load() : 0)
      << ",\"ui_io_violations\":" << (provider ? provider->uiIoViolations.load() : 0) << '}';
}

void statusJson(std::ostream &out, const DocumentViewStatus &status) {
  out << "{\"requested_byte\":" << status.byteOffset << ",\"visible_byte\":" << status.visibleByteOffset
      << ",\"extent\":";
  optionalNumber(out, status.byteExtent);
  out << ",\"preparing\":" << status.preparing << ",\"waiting\":" << status.waiting
      << ",\"backpressured\":" << status.backpressured << ",\"final_end\":" << status.finalEnd
      << ",\"text_ready\":" << (!status.preparing && !status.waiting && !status.backpressured && !status.error)
      << ",\"error\":" << (status.error ? static_cast<int>(status.error->code) : -1)
      << ",\"syntax_state\":" << static_cast<int>(status.highlighting)
      << ",\"syntax_accuracy\":" << static_cast<int>(status.syntaxAccuracy)
      << ",\"syntax_exact\":" << (status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT) << '}';
}

int run(const Options &options) {
  const auto baseline = document_probe::processMemory();
  Measurements metrics(options.samples);
  auto budget = take(SourceBudget::create({static_cast<std::size_t>(options.budgetMiB) * MIB, 64, 8, 16}));
  std::shared_ptr<CountedProvider> physical, logical;
  ui::DocumentSource source;

  if (!options.idle) {
    const auto path = literalPath(options.file);
    physical = std::make_shared<CountedProvider>(take(FileDocumentProvider::create(path)));
    std::shared_ptr<ByteDocumentProvider> provider = physical;

#ifdef PORTALS_PROBE_HAS_GZIP
    if (options.kind == "gzip") {
      provider = take(GzipDocumentProvider::create(provider, budget));
    }
#endif
    logical = std::make_shared<CountedProvider>(provider);
    source = ui::document(logical, budget);
  }

  Workload workload(options, metrics, budget);
  SinkTerminal terminal;
  ui::SceneOptions sceneOptions;
  sceneOptions.application.observer = {Measurements::observe, &metrics};
  DocumentViewConfig config;
  config.tailWorkTurns = static_cast<std::uint8_t>(options.tailWorkTurns);
  config.language = options.language == "mysql"      ? DocumentLanguage::MYSQL_LEXICAL
                    : options.language == "markdown" ? DocumentLanguage::MARKDOWN_SOURCE
                                                     : DocumentLanguage::PLAIN;
  auto highest = baseline;
  std::atomic<std::uint64_t> samplerTicks{};
  ProcessMemory final;
  int applicationExit{};

  {
    // This is the production facade and its default executor, with one shared cold descriptor.
    auto scene = [&]() -> ui::Scene {
      if (options.idle) {
        return ui::scene(ui::label("Document qualification idle baseline"), sceneOptions);
      }

      if (options.views == 1) {
        return ui::scene(ui::viewer(workload.readers[0], source, config).width(120).height(40), sceneOptions);
      }

      return ui::scene(ui::vbox(ui::hbox(ui::viewer(workload.readers[0], source, config).width(60).height(20),
                                         ui::viewer(workload.readers[1], source, config).width(60).height(20)),
                                ui::hbox(ui::viewer(workload.readers[2], source, config).width(60).height(20),
                                         ui::viewer(workload.readers[3], source, config).width(60).height(20))),
                       sceneOptions);
    }();

    scene.configure([&](ManagedScene &, Application &application) {
      workload.begin(application);
    });
    scene.onStop([&] {
      workload.cancelTimer();
    });
    std::jthread sampler([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        const auto memory = document_probe::processMemory();
        const auto maximum = [](auto &target, const auto &value) {
          if (value && (!target || *value > *target)) {
            target = value;
          }
        };

        maximum(highest.privateBytes, memory.privateBytes);
        maximum(highest.residentBytes, memory.residentBytes);
        maximum(highest.peakPrivateBytes, memory.peakPrivateBytes);
        maximum(highest.peakResidentBytes, memory.peakResidentBytes);
        highest.cpuNs = memory.cpuNs;
        if (memory.error) {
          highest.error = memory.error;
        }
        ++samplerTicks;
        std::this_thread::sleep_for(std::chrono::milliseconds(TIMER_MS));
      }
    });

    applicationExit = scene.runApp(terminal);
    sampler.request_stop();
    sampler.join();
    final = document_probe::processMemory();
  }

  std::ostringstream providerReport;
  providerReport << "\"logical_provider\":";
  providerJson(providerReport, logical);
  providerReport << ",\"physical_provider\":";
  providerJson(providerReport, physical);
  const bool workerIoOnly =
    (!logical || !logical->uiIoViolations.load()) && (!physical || !physical->uiIoViolations.load());

  // Scene and worker cleanup has completed before descriptor/provider release is measured.
  source = {};
  logical.reset();
  physical.reset();
  const auto released = budget->usage();
  // SourceBudget's constructor retains ROOT_METADATA_BYTES until the root itself dies
  // (src/models/source_budget.cpp). Verify returned credits and destruction separately.
  const auto outstandingBytes = released.bytes - SourceBudget::ROOT_METADATA_BYTES;
  const std::weak_ptr<SourceBudget> finalRoot = budget;
  budget.reset();
  const bool rootReleased = finalRoot.expired();
  const auto afterCleanup = document_probe::processMemory();
  const bool success = !applicationExit && workload.complete && !workload.failed && !workload.timedOut &&
                       terminal.closed && outstandingBytes == 0 && released.pages == 0 && released.work == 0 &&
                       released.consumers == 0 && rootReleased && workerIoOnly;
  std::cout << std::boolalpha << "{\"probe\":\"whole_application_document\",\"complete\":" << success
            << ",\"application_exit\":" << applicationExit << ",\"timeout\":" << workload.timedOut
            << ",\"workload_failed\":" << workload.failed << ",\"idle\":" << options.idle
            << ",\"views\":" << (options.idle ? 0 : options.views) << ",\"kind\":\"" << options.kind
            << "\",\"language\":\"" << options.language
            << "\",\"viewport\":[120,40],\"budget_limit_bytes\":" << options.budgetMiB * MIB
            << ",\"preparation_quantum_bytes\":" << config.workBytes << ",\"tail_work_turns\":" << options.tailWorkTurns
            << ",\"budget_peak_sampled_bytes\":" << metrics.budgetPeakBytes
            << ",\"budget_after_cleanup_bytes\":" << released.bytes << ",\"accepted_frames\":" << metrics.frames
            << ",\"budget_root_metadata_bytes\":" << SourceBudget::ROOT_METADATA_BYTES
            << ",\"budget_outstanding_credit_bytes\":" << outstandingBytes
            << ",\"budget_root_released\":" << rootReleased << ",\"budget_after_cleanup_pages\":" << released.pages
            << ",\"budget_after_cleanup_work\":" << released.work
            << ",\"budget_after_cleanup_consumers\":" << released.consumers
            << ",\"output_bytes\":" << terminal.outputBytes << ",\"private_memory_label\":\""
            << document_probe::PRIVATE_MEMORY_LABEL << "\",\"peak_private_memory_label\":\""
            << document_probe::PEAK_PRIVATE_MEMORY_LABEL << '"' << ",\"first_accepted_output_frame_ns\":";
  optionalNumber(std::cout, metrics.firstAcceptedOutputFrameNs);
  std::cout << ",\"wall_ns\":" << elapsedNs(metrics.origin) << ',' << providerReport.str() << ",\"memory_baseline\":";
  memoryJson(std::cout, baseline);
  std::cout << ",\"initial_text_ready_observed_ns\":";
  optionalNumber(std::cout, metrics.initialTextReadyObservedNs);
  std::cout << ",\"initial_text_ready_observation_uncertainty_ns\":";
  optionalNumber(std::cout, metrics.initialTextReadyObservationUncertaintyNs);
  std::cout << ",\"memory_final_before_scene_destruction\":";
  memoryJson(std::cout, final);
  std::cout << ",\"memory_highest_sampled\":";
  memoryJson(std::cout, highest);
  std::cout << ",\"memory_after_cleanup\":";
  memoryJson(std::cout, afterCleanup);
  std::cout << ",\"memory_samples\":" << samplerTicks << ",\"timer_lateness\":";
  samplesJson(std::cout, metrics.timerLateness);
  std::cout << ",\"phases\":{\"";
  constexpr std::array<std::string_view, PHASE_COUNT> PHASE_NAMES = {
    "create", "open", "input", "dispatch", "prepare", "render", "submit", "flush", "stop", "close", "control_latency"};

  for (std::size_t index = 0; index < PHASE_COUNT; ++index) {
    if (index) {
      std::cout << ",\"";
    }
    std::cout << PHASE_NAMES[index] << "\":";
    samplesJson(std::cout, metrics.phases[index]);
  }

  std::cout << "},\"planned_stage_count\":" << workload.stages.size() << ",\"stages\":[";

  for (std::size_t index = 0; index < workload.stages.size(); ++index) {
    const auto &stage = workload.stages[index];
    if (index) {
      std::cout << ',';
    }
    std::cout << "{\"name\":\"" << stage.name << "\",\"started\":" << stage.started
              << ",\"completed\":" << stage.completed << ",\"skipped\":" << stage.skipped
              << ",\"command_rejected\":" << stage.commandRejected
              << ",\"status_observed\":" << (stage.observedViews != 0) << ",\"target_byte\":" << stage.offset
              << ",\"first_output_frame_observed_ns\":";
    optionalNumber(std::cout, stage.firstOutputFrameObservedNs);
    std::cout << ",\"settled_ns\":";
    optionalNumber(std::cout, stage.settledNs);
    std::cout << ",\"views\":[";

    for (unsigned view = 0; view < stage.observedViews; ++view) {
      if (view) {
        std::cout << ',';
      }
      statusJson(std::cout, stage.status[view]);
    }

    std::cout << "]}";
  }

  std::cout << "],\"limitations\":[\"headless sink measures accepted output, not physical display\","
               "\"first accepted output frame can show loading; initial text readiness is timer-observed, not a frame "
               "timestamp\","
               "\"stage output-frame and settled timings are scheduler-timer observations\","
               "\"budget peak is sampled at UI timer callbacks\",\"phase percentiles use bounded retained prefix\","
               "\"text readiness does not require globally exact syntax\",\"no UI_TASK phase is exposed\","
               "\"Linux private commit and peak private commit are unavailable\","
               "\"source-byte oracles are performed by the separate document probe\"]}\n";

  return success ? 0 : 1;
}
} // namespace

/** @brief Run a finite read-only Application qualification, without opening a native terminal.
 * @param argc Argument count. @param argv UTF-8 literal paths and Lyra options.
 * @return 0 for help/complete workload, 2 for invalid CLI/input, 1 for incomplete/runtime failure.
 */
int probeMain(int argc, char **argv) {
  Options options;
  auto cli =
    ::lyra::cli() |
    ::lyra::help(options.help)
      .description(
        "Measure a real Application::run with managed DocumentViews and production workers. "
        "Read-only UTF-8 literal file paths; no environment/home expansion. Headless fixed 120x40 "
        "terminal discards output after counting. JSON on stdout; no corpus or output files created. "
        "Defaults: plain source/language, 1 view, 128 MiB shared source budget, 60000 ms deadline, "
        "8192 retained prefix samples per phase. Exit: 0 complete/help, 2 CLI/input, 1 incomplete/error/timeout. "
        "--help performs no file I/O, application construction or terminal work.") |
    ::lyra::opt(options.file, "path")["--file"]("Existing read-only file (required unless --idle)") |
    ::lyra::opt(options.kind, "kind")["--kind"]("plain or gzip").choices("plain", "gzip") |
    ::lyra::opt(options.language, "language")["--language"]("plain, mysql or markdown")
      .choices("plain", "mysql", "markdown") |
    ::lyra::opt(options.views, "count")["--views"]("1 or 4 views").choices(1u, 4u) |
    ::lyra::opt(options.budgetMiB, "MiB")["--budget-mib"]("Shared source limit, 1..4096 MiB") |
    ::lyra::opt(options.timeoutMs, "ms")["--timeout-ms"]("Workload deadline, 1..3600000 ms") |
    ::lyra::opt(options.samples, "count")["--samples"]("Retained samples per metric, 1..1000000") |
    ::lyra::opt(options.tailWorkTurns,
                "count")["--tail-work-turns"]("Managed tail quanta per job, 1..255; 1 disables batching") |
    ::lyra::opt(options.idle)["--idle"]("Mount the same Application with no DocumentViews for memory baseline");
  const auto parsed = cli.parse({argc, argv});

  if (!parsed) {
    std::cerr << parsed.message() << '\n';

    return 2;
  }

  if (options.help) {
    std::cout << cli << '\n';

    return 0;
  }

  if (!options.budgetMiB || options.budgetMiB > 4096 || !options.timeoutMs || options.timeoutMs > 3600000 ||
      !options.samples || options.samples > 1000000 || !options.tailWorkTurns ||
      options.tailWorkTurns > std::numeric_limits<std::uint8_t>::max() || (!options.idle && options.file.empty())) {
    std::cerr << "Invalid bounds or missing --file; use --help.\n";

    return 2;
  }

#ifndef PORTALS_PROBE_HAS_GZIP
  if (!options.idle && options.kind == "gzip") {
    std::cerr << "This probe was built without optional gzip support.\n";

    return 2;
  }
#endif
  try {
    if (!options.idle && !std::filesystem::is_regular_file(literalPath(options.file))) {
      std::cerr << "--file must name an existing regular file.\n";

      return 2;
    }

    return run(options);
  } catch (const std::filesystem::filesystem_error &error) {
    std::cerr << error.what() << '\n';

    return 2;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}

#ifdef _WIN32
/** @brief Preserve native Windows literal Unicode paths before passing arguments to Lyra.
 * @param argc Number of native arguments. @param argv Native UTF-16 arguments borrowed for this call.
 * @return The probe result, or two when an argument cannot be encoded as valid UTF-8.
 */
int wmain(int argc, wchar_t **argv) {
  try {
    std::vector<std::string> storage;
    std::vector<char *> arguments;
    storage.reserve(static_cast<std::size_t>(argc));
    arguments.reserve(static_cast<std::size_t>(argc));

    for (int index = 0; index < argc; ++index) {
      const auto required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, nullptr, 0, nullptr, nullptr);

      if (!required) {
        std::cerr << "Command-line argument is not valid Unicode.\n";

        return 2;
      }

      std::string encoded(static_cast<std::size_t>(required), '\0');

      if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, encoded.data(), required, nullptr,
                               nullptr)) {
        std::cerr << "Command-line UTF-8 encoding failed.\n";

        return 2;
      }

      encoded.pop_back();
      storage.push_back(std::move(encoded));
    }

    for (auto &argument : storage) {
      arguments.push_back(argument.data());
    }

    return probeMain(argc, arguments.data());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
#else
/** @brief Use the platform UTF-8 argument convention.
 * @param argc Argument count. @param argv Borrowed UTF-8 argument strings.
 * @return The probe result.
 */
int main(int argc, char **argv) {
  return probeMain(argc, argv);
}
#endif
