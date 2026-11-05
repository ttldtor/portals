// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
// Acceptance probe, invoked by: python tests/probes/log_view_memory.py --exe BUILD/log_view_memory.exe
// Standalone help: log_view_memory --help. Input: filled fixed-record UTF-8 file. Output: raw JSON samples.
// Exit codes: 0 accepted local correctness/budget checks, 1 runtime/check failure, 2 invalid arguments.
// The probe creates only its JSON output; it does not create/delete input files or modify OS settings.
#include "../../src/widgets/external_log_view.hpp"
#include "../../src/widgets/log_view_state.hpp"
#include <portals/ui.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include <psapi.h>
#else
#  include <sys/resource.h>
#endif

namespace {
using namespace portals;
using Clock = std::chrono::steady_clock;
constexpr CellSize VIEWPORT{80, 24};
constexpr std::uint64_t ROOT_LIMIT = 8 * 1024 * 1024;
constexpr std::size_t MAX_SAMPLES = 4096;
constexpr unsigned PAGE_STEPS = 100;
constexpr auto NAVIGATION_TIMEOUT = std::chrono::seconds(30);
constexpr std::string_view PRESENT = "PRTLS_320_NEAR_END_SENTINEL";
constexpr std::string_view ABSENT = "PRTLS_320_ABSENT_SENTINEL";

template <class T, class E> T checked(std::expected<T, E> result, const char *operation) {
  if (!result) {
    throw std::runtime_error(operation);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Memory {
  std::uint64_t resident{};
  std::uint64_t peakResident{};
  std::uint64_t privateBytes{};
  std::optional<std::uint64_t> peakPrivate;
};

Memory memory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                               sizeof(counters)) != 0,
          "GetProcessMemoryInfo failed");

  return {counters.WorkingSetSize, counters.PeakWorkingSetSize, counters.PrivateUsage, counters.PeakPagefileUsage};
#else
  std::ifstream status("/proc/self/status");
  require(status.is_open(), "Linux /proc/self/status is required");
  Memory result;
  std::string line;

  while (std::getline(status, line)) {
    std::istringstream fields(line);
    std::string name;
    std::uint64_t kib{};
    fields >> name >> kib;

    if (name == "VmRSS:") {
      result.resident = kib * 1024;
    } else if (name == "VmHWM:") {
      result.peakResident = kib * 1024;
    }
  }

  // Linux exposes no exact Windows PrivateUsage equivalent. Count private writable virtual mappings,
  // including their nonresident capacity, and identify this conservative proxy explicitly in JSON.
  std::ifstream mappings("/proc/self/smaps");
  require(mappings.is_open(), "Linux /proc/self/smaps is required");
  bool privateWritable{};

  while (std::getline(mappings, line)) {
    if (line.find('-') < line.find(' ')) {
      std::istringstream fields(line);
      std::string range;
      std::string permissions;
      fields >> range >> permissions;
      privateWritable = permissions.size() == 4 && permissions[1] == 'w' && permissions[3] == 'p';
    } else if (privateWritable && line.starts_with("Size:")) {
      std::istringstream fields(line.substr(5));
      std::uint64_t kib{};
      fields >> kib;
      result.privateBytes += kib * 1024;
    }
  }

  require(result.resident && result.peakResident && result.privateBytes, "Linux memory counters unavailable");

  return result;
#endif
}

struct Sample {
  std::string stage;
  std::uint64_t milliseconds{};
  Memory process;
  SourceBudgetUsage source;
  std::size_t preparedRows{};
  std::size_t preparedBytes{};
  std::size_t preparedRecords{};
  std::size_t recordBytes{};
  std::size_t preparedCreditBytes{};
  std::uint64_t frameHash{};
  std::uint64_t visibleByte{};
  std::uint64_t firstRow{};
  std::uint64_t generation{};
  bool presentationSettled{};
  std::optional<std::uint64_t> paintNs;
  std::optional<std::uint64_t> renderNs;
};

struct PaintSink final : PaintContext {
  std::vector<RenderCommand> commands;

  PaintSink() {
    commands.reserve(2048);
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    if (commands.size() + values.size() > commands.capacity()) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    for (const auto &value : values) {
      commands.emplace_back(value);
    }

    return {};
  }
};

struct QueryState {
  std::atomic<bool> done{};
  std::optional<FileLogMetadata> metadata;
  std::optional<FileLogSearchResult> result;
  std::optional<FileLogFailure> error;
  std::uint64_t steps{};
};

class QueryJob final : public BackgroundJob {
public:
  QueryJob(std::shared_ptr<FileLogModel> model, std::shared_ptr<QueryState> result, std::string_view pattern)
      : model_(std::move(model)), result_(std::move(result)), pattern_(pattern) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      auto metadata = model_->metadata(stop);

      if (!metadata) {
        result_->error = metadata.error();

        return;
      }

      result_->metadata = *metadata;

      if (pattern_.empty()) {
        return;
      }

      std::uint64_t offset{};

      do {
        auto step = model_->findLiteral(pattern_, offset, model_->options().maxSearchBytes, stop);
        ++result_->steps;

        if (!step) {
          result_->error = step.error();

          return;
        }

        result_->result = *step;

        if (step->matchByteOffset || step->reachedEnd) {
          return;
        }

        if (step->nextByteOffset <= offset || step->generation != metadata->generation) {
          result_->error = FileLogFailure{FileLogError::FILE_CHANGED, 0, FileLogOperation::SEARCH};

          return;
        }

        offset = step->nextByteOffset;
      } while (!stop.stop_requested());

      result_->error = FileLogFailure{FileLogError::CANCELLED, 0, FileLogOperation::SEARCH};
    } catch (...) {
      result_->error = FileLogFailure{FileLogError::NO_MEMORY, 0, FileLogOperation::SEARCH};
    }
  }

  std::expected<void, AsyncError> publish() override {
    result_->done.store(true, std::memory_order_release);

    return {};
  }

private:
  std::shared_ptr<FileLogModel> model_;
  std::shared_ptr<QueryState> result_;
  std::string_view pattern_;
};

class Harness {
public:
  explicit Harness(const std::string &path) {
    samples.reserve(MAX_SAMPLES);
    pool = checked(BackgroundPool::create(), "BackgroundPool::create");
    app = checked(Application::create({.backgroundExecutor = pool.get()}), "Application::create");
    scene = checked(ManagedScene::create(*app), "ManagedScene::create");
    source = ui::logFile(path);
    model = source.fileModel();
    require(bool(model), "file descriptor creation failed");
    node = checked(ui::mount(*scene, ui::logView(source, {.followTail = false})), "ui::mount logView");
    checked(scene->focus(node), "focus logView");
    budget = source.budget();
    require(bool(budget) && budget->options().maxBytes == ROOT_LIMIT, "expected default 8 MiB root");
    started = Clock::now();
    capture("bootstrap");
    require(samples.back().source.work == 0 && samples.back().source.pages == 0 && samples.back().preparedRows == 0,
            "bootstrap must precede the initial worker/read/prepare");
  }

  ~Harness() {
    scene.reset();
    app.reset();

    if (pool) {
      pool->requestStop();
      pool->join();
    }
  }

  void pump() {
    checked(app->scheduler().dispatch(), "scheduler dispatch");
    // Match Application's host loop: publication retries are serviced after dispatch and before scene work.
    pool->service();
    checked(scene->afterDispatch(*app), "scene afterDispatch");
    checked(scene->prepare(*app, VIEWPORT), "scene prepare");
    auto bounds = checked(CellRect::from({}, VIEWPORT), "viewport bounds");

    for (unsigned pass = 0; pass != 3; ++pass) {
      checked(app->tree().consumeFrameRequest(), "consume frame");
      std::optional<LayoutSnapshot> layout;
      checked(app->tree().frame([&] {
        checked(layoutTree(app->tree(), app->root(), bounds, bounds), "layoutTree");
        layout.emplace(checked(LayoutSnapshot::capture(app->tree(), app->root(), LayoutSnapshotProfile::GEOMETRY),
                               "layout capture"));
      }),
              "layout frame");
      checked(scene->prepareViews(*app, *layout), "prepareViews");

      if (checked(app->tree().consumeFrameRequest(), "consume layout") != Invalidation::LAYOUT) {
        break;
      }
    }

    checked(scene->completedLayout(*app), "completedLayout");
    auto diagnostics = checked(scene->diagnostics(), "diagnostics");
    require(diagnostics.errors.empty(), "scene reported an external LogView error");
  }

  Sample snapshot(std::string stage = {}) {
    Sample result;
    result.stage = std::move(stage);
    result.milliseconds =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    result.process = memory();
    result.source = budget->usage();
    require(result.source.bytes <= ROOT_LIMIT && result.source.pages <= 32 && result.source.work <= 4,
            "SourceBudget hard limit exceeded");

    checked(app->tree().read(node,
                             [&](const LogView &widget) {
                               auto external = detail::LogViewAccess::state(widget).external;

                               if (!external || !external->frame) {
                                 return;
                               }

                               const auto &frame = *external->frame;
                               // A native scroll can update presentation in prepareViews but leave its activity
                               // notification for the next dispatch. Do not inject the next key in that gap.
                               result.presentationSettled = frame.width == external->viewport.width &&
                                                            !external->navigationChanged &&
                                                            !external->requestedOffset && !external->positionRequest &&
                                                            !external->byteRequest && !external->tailRequested;
                               result.preparedRows = frame.rows.size();
                               result.preparedRecords = frame.records;
                               result.recordBytes = frame.bytes;
                               result.preparedCreditBytes = frame.credit.usage().bytes;

                               for (const auto &row : frame.rows) {
                                 result.preparedBytes += row->bytes().size();
                               }

                               if (frame.footer) {
                                 result.preparedBytes += frame.footer->bytes().size();
                               }

                               if (frame.alternateFooter) {
                                 result.preparedBytes += frame.alternateFooter->bytes().size();
                               }

                               if (external->offset < frame.anchors.size()) {
                                 const auto &anchor = frame.anchors[static_cast<std::size_t>(external->offset)];
                                 result.firstRow = anchor.row.value;
                                 result.generation = anchor.row.generation;
                                 result.visibleByte = anchor.row.value - 1 + anchor.firstByte;
                               }
                             }),
            "read external frame");

    if (result.preparedRows) {
      sink.commands.clear();
      const auto paintStarted = Clock::now();
      checked(app->tree().frame([&] {
        checked(app->tree().paint(node, sink), "paint actual LogView");
      }),
              "paint frame");
      result.paintNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - paintStarted).count());
      const auto renderStarted = Clock::now();
      auto rendered = checked(renderFullFrame(VIEWPORT, sink.commands), "render actual frame");
      result.renderNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - renderStarted).count());
      std::uint64_t hash = 1469598103934665603ULL;

      for (std::int32_t y = 0; y < VIEWPORT.height; ++y) {
        for (std::int32_t x = 0; x < VIEWPORT.width; ++x) {
          auto cell = checked(rendered.at({x, y}), "rendered cell");

          for (auto byte : cell.glyph) {
            hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
          }
        }
      }

      result.frameHash = hash;
    }

    return result;
  }

  void capture(std::string stage) {
    require(samples.size() < MAX_SAMPLES, "fixed raw sample capacity exhausted");
    samples.push_back(snapshot(std::move(stage)));
  }

  template <class Predicate>
  void until(std::string_view stage, Predicate predicate, Clock::duration timeout = NAVIGATION_TIMEOUT) {
    auto deadline = Clock::now() + timeout;

    do {
      pump();

      if (predicate()) {
        return;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < deadline);

    const auto last = snapshot();
    std::ostringstream message;
    message << "bounded UI/worker deadline exceeded at " << stage << ": rows=" << last.preparedRows
            << " records=" << last.preparedRecords << " visible_byte=" << last.visibleByte
            << " first_row=" << last.firstRow << " generation=" << last.generation
            << " presentation_settled=" << last.presentationSettled << " frame_hash=" << last.frameHash
            << " source_bytes=" << last.source.bytes << " pages=" << last.source.pages << " work=" << last.source.work;
    checked(app->tree().read(node,
                             [&](const LogView &widget) {
                               const auto external = detail::LogViewAccess::state(widget).external;

                               if (external) {
                                 message << " visible=" << external->visible << " viewport=" << external->viewport.width
                                         << 'x' << external->viewport.height << " offset=" << external->offset;

                                 if (external->frame) {
                                   message << " before=" << external->frame->before.token[0]
                                           << " after=" << external->frame->after.token[0];
                                 }
                               }
                             }),
            "deadline frame diagnostics");

    throw std::runtime_error(message.str());
  }

  void firstWindow() {
    std::cerr << "stage first_window begin\n";
    until("first_window", [&] {
      const auto current = snapshot();

      return current.preparedRows > 0 && current.presentationSettled && budget->usage().work == 0;
    });
    capture("first_window");
    require(samples.back().visibleByte == 0, "first window did not show file head");
    std::cerr << "stage first_window complete\n";
  }

  void seek(std::uint64_t byte, std::string stage) {
    auto previous = snapshot();
    checked(seekLogByte(app->tree(), node, byte), "seekLogByte");
    std::cerr << "stage " << stage << " begin target_byte=" << byte << '\n';
    until(stage, [&] {
      auto current = snapshot();

      return current.firstRow == byte + 1 && current.visibleByte == byte && current.presentationSettled &&
             budget->usage().work == 0;
    });
    capture(std::move(stage));

    if (previous.visibleByte != byte) {
      require(previous.frameHash != samples.back().frameHash, "seek did not change actual rendered frame");
    }
  }

  void pages(bool forward, const std::string &stage) {
    std::cerr << "stage " << stage << " begin\n";
    for (unsigned i = 0; i < PAGE_STEPS; ++i) {
      auto previous = snapshot();
      KeyEvent key{forward ? InputKey::PAGE_DOWN : InputKey::PAGE_UP};

      if (!checked(scene->input(*app, key), "scene key input")) {
        checked(app->router().dispatch(key), "native key route");
      }

      const auto step = stage + "_" + std::to_string(i);
      until(step, [&] {
        auto current = snapshot();
        auto advanced =
          forward ? current.visibleByte > previous.visibleByte : current.visibleByte < previous.visibleByte;

        return advanced && current.frameHash != previous.frameHash && current.presentationSettled &&
               budget->usage().work == 0;
      });
      capture(step);

      if ((i + 1) % 10 == 0) {
        std::cerr << "stage " << stage << ' ' << i + 1 << '/' << PAGE_STEPS << '\n';
      }
    }
  }

  std::shared_ptr<QueryState> query(std::string_view pattern, const std::string &stage) {
    auto result = std::make_shared<QueryState>();
    std::unique_ptr<BackgroundJob> job = std::make_unique<QueryJob>(model, result, pattern);
    auto ticket = checked(pool->submit(job), "submit rich-model operation");
    auto nextSample = Clock::now() + std::chrono::seconds(1);
    std::cerr << "stage " << stage << " begin\n";
    until(
      stage,
      [&] {
        if (Clock::now() >= nextSample) {
          capture(stage + "_running");
          nextSample = Clock::now() + std::chrono::seconds(1);
        }

        return result->done.load(std::memory_order_acquire);
      },
      std::chrono::minutes(15));
    require(!result->error, "rich-model worker operation failed");
    capture(stage);

    return result;
  }

  std::unique_ptr<BackgroundPool> pool;
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;
  ui::LogSource source;
  std::shared_ptr<FileLogModel> model;
  std::shared_ptr<SourceBudget> budget;
  Node<LogView> node;
  PaintSink sink;
  Clock::time_point started;
  std::vector<Sample> samples;
};

void writeJson(const std::string &output, const Harness &harness, std::uint64_t extent, std::size_t recordBytes,
               std::uint64_t presentSteps, std::uint64_t absentSteps) {
  std::ofstream json(output, std::ios::binary);
  require(json.is_open(), "cannot open output JSON");
  json
    << "{\"schema\":1,\"status\":\"PASS\",\"viewport\":[80,24],\"root_limit_bytes\":" << ROOT_LIMIT
    << ",\"record_bytes\":" << recordBytes << ",\"file_bytes\":" << extent
    << ",\"source_index_bytes\":0,\"source_index_kind\":\"none\",\"sample_capacity\":" << MAX_SAMPLES
    << ",\"page_steps_per_direction\":" << PAGE_STEPS << ",\"search_present_steps\":" << presentSteps
    << ",\"search_absent_steps\":" << absentSteps
    << ",\"frame_timing\":{\"units\":\"nanoseconds\",\"scenario\":\"headless manual snapshot; one LogView at 80x24\","
       "\"physical_terminal\":false,\"baseline\":null,\"hash_outside_timers\":true},"
       "\"units\":\"bytes\",\"private_metric\":\""
#ifdef _WIN32
    << "windows_PrivateUsage\",\"peak_private_metric\":\"windows_PeakPagefileUsage\""
#else
    << "linux_private_writable_virtual_mapping_size\",\"peak_private_metric\":null"
#endif
    << ",\"compiler\":\""
#ifdef _MSC_VER
    << "MSVC " << _MSC_FULL_VER
#elif defined(__clang__)
    << "Clang " << __clang_version__
#else
    << "GCC " << __VERSION__
#endif
    << "\",\"samples\":[";
  bool comma{};

  for (const auto &sample : harness.samples) {
    json << (std::exchange(comma, true) ? "," : "") << "{\"stage\":\"" << sample.stage
         << "\",\"elapsed_ms\":" << sample.milliseconds << ",\"resident_bytes\":" << sample.process.resident
         << ",\"peak_resident_bytes\":" << sample.process.peakResident
         << ",\"private_bytes\":" << sample.process.privateBytes << ",\"peak_private_bytes\":";

    if (sample.process.peakPrivate) {
      json << *sample.process.peakPrivate;
    } else {
      json << "null";
    }

    json << ",\"source_bytes\":" << sample.source.bytes << ",\"source_pages\":" << sample.source.pages
         << ",\"source_work\":" << sample.source.work << ",\"source_consumers\":" << sample.source.consumers
         << ",\"prepared_rows\":" << sample.preparedRows << ",\"prepared_bytes\":" << sample.preparedBytes
         << ",\"prepared_records\":" << sample.preparedRecords << ",\"frame_hash\":" << sample.frameHash
         << ",\"record_payload_bytes\":" << sample.recordBytes
         << ",\"prepared_credit_bytes\":" << sample.preparedCreditBytes << ",\"visible_byte\":" << sample.visibleByte
         << ",\"first_row_id\":" << sample.firstRow << ",\"file_generation\":" << sample.generation << ",\"paint_ns\":";

    if (sample.paintNs) {
      json << *sample.paintNs;
    } else {
      json << "null";
    }

    json << ",\"render_ns\":";

    if (sample.renderNs) {
      json << *sample.renderNs;
    } else {
      json << "null";
    }

    json << '}';
  }

  json << "]}\n";
  require(json.good(), "writing output JSON failed");
}
} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
      std::cout << "Usage: log_view_memory --file PATH --record-bytes 128 --output JSON\n"
                   "Measures the real external LogView and existing BackgroundPool at 80x24.\n"
                   "Input must be a filled UTF-8 regular file of fixed records, including LF.\n"
                   "Runs 100 PageDown/PageUp turns twice, aligned seeks and full-file searches.\n"
                   "Default record bytes: 128. Navigation deadline: 30s; each search: 15min.\n"
                   "Writes raw process/source/prepared-frame samples to JSON; never edits input.\n"
                   "Exit 0: correctness/source budget passed; 1: runtime/check; 2: arguments.\n"
                   "Cross-size private/peak gates are evaluated by log_view_memory.py.\n";

      return 0;
    }
  }

  std::string file;
  std::string output;
  std::size_t recordBytes = 128;

  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc) {
      std::cerr << "Missing option value; use --help\n";

      return 2;
    }

    std::string_view option(argv[i]);

    if (option == "--file") {
      file = argv[i + 1];
    } else if (option == "--output") {
      output = argv[i + 1];
    } else if (option == "--record-bytes") {
      auto value = std::string_view(argv[i + 1]);
      auto parsed = std::from_chars(value.data(), value.data() + value.size(), recordBytes);

      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return 2;
      }
    } else {
      std::cerr << "Unknown option; use --help\n";

      return 2;
    }
  }

  if (file.empty() || output.empty() || recordBytes < 64 || recordBytes > 32768) {
    std::cerr << "A file, output and record bytes in [64,32768] are required; use --help\n";

    return 2;
  }

  try {
    require(!std::filesystem::exists(output), "refusing to overwrite an existing output path");
    Harness harness(file);
    harness.firstWindow();
    auto metadata = harness.query({}, "metadata");
    auto extent = metadata->metadata->byteExtent;
    require(extent >= recordBytes * 4096 && extent % recordBytes == 0, "input is too small or not record aligned");
    harness.pages(true, "forward");
    harness.pages(false, "backward");
    harness.seek(0, "seek_head");
    harness.seek(extent / 2 / recordBytes * recordBytes, "seek_middle");
    harness.seek(extent - recordBytes, "seek_tail");
    harness.seek(0, "repeat_head");
    harness.pages(true, "repeat_forward");
    harness.pages(false, "repeat_backward");
    auto present = harness.query(PRESENT, "search_present");
    require(present->result && present->result->matchByteOffset &&
              *present->result->matchByteOffset >= extent - 2 * recordBytes,
            "near-end sentinel was not found");
    auto absent = harness.query(ABSENT, "search_absent");
    require(absent->result && !absent->result->matchByteOffset && absent->result->reachedEnd,
            "absent search did not reach the full source boundary");
    harness.capture("warm_final");
    writeJson(output, harness, extent, recordBytes, present->steps, absent->steps);

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "log_view_memory: " << error.what() << '\n';

    return 1;
  }
}
