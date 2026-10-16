// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>

namespace {
thread_local bool countingAllocations{};
thread_local std::size_t allocationCalls{};
thread_local std::size_t requestedBytes{};
} // namespace

// Count ordinary new/new[] requests in scoped phases; exclude malloc, aligned allocations and live memory.
void *operator new(std::size_t size) {
  if (auto *memory = std::malloc(std::max<std::size_t>(size, 1))) {
    if (countingAllocations) {
      ++allocationCalls;
      requestedBytes += size;
    }

    return memory;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {
using namespace portals;
using Clock = std::chrono::steady_clock;
constexpr CellSize VIEWPORT{80, 24};
constexpr std::size_t MAX_SAMPLES = 200;
constexpr std::size_t MAX_CONTROLS = 1024;
constexpr std::size_t MAX_WARMUP = 50;
constexpr std::uint8_t FIRST_COLOR = 70;
constexpr std::uint8_t SECOND_COLOR = 90;
constexpr std::array<std::string_view, 3> UPDATE_NAMES{"equal", "masked", "visible"};
constexpr std::array<std::string_view, 3> VISIBILITY_NAMES{"visible", "hidden", "offscreen"};

void check(bool condition, std::string_view message, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::format("{} at line {}", message, where.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  check(result.has_value(), "unexpected API failure", where);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Options {
  std::vector<std::size_t> sizes{32, 128, 512};
  std::size_t samples{30};
  std::size_t warmup{5};
  bool checkOnly{};
  std::string output;
};

std::size_t number(std::string_view value, std::size_t maximum) {
  std::size_t result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);

  check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result <= maximum,
        "invalid numeric option");

  return result;
}

// Help is handled before option parsing, workload construction and output creation.
bool printHelp(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);

    if (argument != "--help" && argument != "-h") {
      continue;
    }

    if (argc != 2) {
      throw std::runtime_error("--help or -h must be the sole argument");
    }

    std::cout << R"help(Measure style-layer updates for visible, hidden and offscreen controls.
Usage: portals_style_layers_benchmark [options]
       portals_style_layers_benchmark --help | -h
Options:
  --samples N  Timed samples per case, 1..200 (default: 30).
  --warmup N   Untimed samples per case, 0..50 (default: 5).
  --rows N     Use one control count, 1..1024 (default: 32, 128, 512).
  --check      Override rows to 2, 8, samples to 3 and warmup to 1.
  --output P   Write JSON to P, truncating it (default: stdout; empty P also uses stdout).
  --help, -h   Show help as the sole argument; no workload or files.
Output: JSON to stdout or --output, with nanosecond distributions, ordinary
new/new[] calls and requested bytes for update/frame phases. Counts exclude
malloc, aligned allocations and live memory; counter overhead remains included.
Exit: 0 on success/help; 1 on invalid arguments, workload or output failure.
)help";

    return true;
  }

  return false;
}

Options parse(int argc, char **argv) {
  Options result;

  for (int i = 1; i < argc; ++i) {
    const std::string_view key(argv[i]);

    if (key == "--check") {
      result.checkOnly = true;

      continue;
    }

    check(i + 1 < argc, "missing option value");

    const std::string_view value(argv[++i]);

    if (key == "--samples") {
      result.samples = number(value, MAX_SAMPLES);
      check(result.samples > 0, "samples must be positive");
    } else if (key == "--warmup") {
      result.warmup = number(value, MAX_WARMUP);
    } else if (key == "--rows") {
      const auto count = number(value, MAX_CONTROLS);

      check(count > 0, "controls must be positive");
      result.sizes = {count};
    } else if (key == "--output") {
      result.output = value;
    } else {
      throw std::runtime_error("unknown option");
    }
  }

  if (result.checkOnly) {
    result.sizes = {2, 8};
    result.samples = 3;
    result.warmup = 1;
  }

  return result;
}

struct Counts {
  std::size_t defaults{};
  std::size_t paints{};
};

// Instrument a real extension hook; no production instrumentation or allocator estimates.
class CountedLabel final : public Label {
public:
  explicit CountedLabel(Counts &counts) : Label(take(GraphemeText::from("Style benchmark"))), counts_(counts) {
  }

  ComputedStyle styleDefaults() const override {
    ++counts_.defaults;

    return Label::styleDefaults();
  }

  std::expected<void, TreeError> paint(PaintContext &context) override {
    ++counts_.paints;

    return Label::paint(context);
  }

private:
  Counts &counts_;
};

class Surface final : public PaintContext {
public:
  NodeGeometry geometry;
  std::vector<RenderCommand> commands;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> local) override {
    for (const auto &command : local) {
      std::visit(
        [&](const auto &value) {
          const auto clip = take(value.clip.translated(geometry.surface.origin())).intersection(geometry.clip);

          if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, TextCommand>) {
            commands.emplace_back(TextCommand{take(translated(value.origin, geometry.surface.origin())), value.text,
                                              clip, value.style, value.z});
          } else {
            commands.emplace_back(
              FillCommand{take(value.area.translated(geometry.surface.origin())), clip, value.style, value.z});
          }
        },
        command);
    }

    return {};
  }
};

StyleSheet theme(std::size_t rules, std::uint8_t color, bool masked) {
  StyleSheet result;

  for (std::size_t i = 0; i < rules; ++i) {
    result.rules.push_back({{.type = "Control"},
                            masked ? StyleDeclarations{.background = RgbColor{color, 20, 30}}
                                   : StyleDeclarations{.foreground = RgbColor{color, 20, 30}}});
  }

  return result;
}

StyleSheet overrides() {
  return {{{{.type = "Control"}, {.background = RgbColor{40, 50, 60}, .bold = true}}}};
}

class Output final : public FrameOutput {
public:
  std::string pending;
  std::size_t bytes{};

  std::expected<void, TerminalError> submit(std::string value) override {
    pending = std::move(value);

    return {};
  }

  TerminalWrite flush(TerminalDeadline, std::stop_token) override {
    const auto completed = pending.size();

    bytes += completed;
    pending.clear();

    return {.completed = completed};
  }
};

struct Workload {
  Counts counts;
  Output output;
  FrameWriter writer{output};
  std::unique_ptr<Application> app{take(Application::create())};
  std::unique_ptr<ManagedScene> scene{take(ManagedScene::create(*app))};
  std::vector<Node<CountedLabel>> controls;
  bool layered;
  std::size_t renders{};

  Workload(std::size_t count, bool layers, std::size_t visibility) : layered(layers) {
    controls.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
      const auto node = take(scene->emplace<CountedLabel>(scene->root(), counts));

      take(scene->style(node, {.type = "Control"}));

      if (visibility == 1) {
        take(app->tree().setVisible(node, false));
      }

      controls.push_back(node);
    }

    if (layered) {
      take(scene->enableStyleLayers());
      take(scene->addLayer(StyleTier::APPLICATION, "application", overrides()));
      take(scene->addLayer(StyleTier::USER, "user", {{{{.type = "Control"}, {.underline = true}}}}));
    }

    take(scene->prepare(*app, VIEWPORT));
    take(app->tree().consumeFrameRequest());
  }

  void update(const StyleSheet &sheet) {
    if (layered) {
      take(scene->theme(sheet));
    } else {
      auto composed = sheet;
      const auto application = overrides();
      const StyleSheet user{{{{.type = "Control"}, {.underline = true}}}};

      composed.rules.insert(composed.rules.end(), application.rules.begin(), application.rules.end());
      composed.rules.insert(composed.rules.end(), user.rules.begin(), user.rules.end());
      take(scene->theme(composed));
    }
  }

  void verify(bool masked, std::uint8_t color) {
    for (const auto node : controls) {
      take(app->tree().read(node, [&](const CountedLabel &control) {
        const auto &value = control.computedStyle();

        check(value && value->normal.background == RgbColor{40, 50, 60} && value->normal.bold &&
                value->normal.underline,
              "override preservation failed");

        if (!masked) {
          check(value->normal.foreground == RgbColor{color, 20, 30}, "visible update missing");
        }
      }));
    }
  }

  std::size_t render(std::size_t visibility) {
    Surface surface;
    std::optional<CellBuffer> buffer;
    const auto viewport = take(CellRect::from({}, VIEWPORT));

    take(app->tree().frame([&] {
      take(app->tree().arrange(app->root(), viewport, viewport));
      take(app->tree().arrange(scene->root(), viewport, viewport));

      for (std::size_t i = 0; i < controls.size(); ++i) {
        const auto y = visibility == 2 ? VIEWPORT.height + static_cast<std::int32_t>(i)
                                       : static_cast<std::int32_t>(i % VIEWPORT.height);
        const auto local = take(CellRect::from({0, y}, {80, 1}));

        take(app->tree().arrange(controls[i], local, viewport));
        surface.geometry = take(app->tree().inspect(controls[i])).geometry;
        take(app->tree().paint(controls[i], surface));
      }

      buffer.emplace(take(renderFullFrame(VIEWPORT, surface.commands)));
      check(buffer->size() == VIEWPORT, "frame dimensions changed");
    }));
    take(writer.submit(std::move(*buffer)));
    const auto flushed = writer.flush(TerminalDeadline::max());

    check(!flushed.error && flushed.pending == 0, "frame output did not complete");
    ++renders;

    return surface.commands.size();
  }

  void close() {
    const auto root = scene->root();

    take(scene->close());
    check(!app->tree().inspect(root), "scene root survived cleanup");
    check(take(app->tree().inspect(app->root())).children.empty(), "scene nodes leaked");
  }
};

// Scope retirement also restores the counter on a failed workload; reporting runs outside the window.
class AllocationWindow {
public:
  AllocationWindow() : previous_(countingAllocations), calls_(allocationCalls), bytes_(requestedBytes) {
    countingAllocations = true;
  }

  ~AllocationWindow() {
    countingAllocations = previous_;
  }

  std::size_t calls() const {
    return allocationCalls - calls_;
  }

  std::size_t bytes() const {
    return requestedBytes - bytes_;
  }

private:
  bool previous_;
  std::size_t calls_;
  std::size_t bytes_;
};

// A check-only oracle keeps measurement instrumentation honest without adding work to timed cases.
void verifyAllocationCounter() {
  constexpr std::size_t PROBE_BYTES = 17;

  {
    AllocationWindow window;
    auto *single = ::operator new(PROBE_BYTES);
    auto *array = ::operator new[](PROBE_BYTES);

    ::operator delete(single);
    ::operator delete[](array);
    check(window.calls() == 2 && window.bytes() == 2 * PROBE_BYTES, "allocation counter differs from oracle");
  }

  check(!countingAllocations, "allocation window survived normal completion");

  try {
    AllocationWindow window;

    throw std::bad_alloc();
  } catch (const std::bad_alloc &) {
  }

  check(!countingAllocations, "allocation window survived failure");
}

template <class F> std::int64_t measure(F &&operation, std::size_t &calls, std::size_t &bytes) {
  AllocationWindow allocations;
  const auto start = Clock::now();

  operation();

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();

  calls = allocations.calls();
  bytes = allocations.bytes();

  return elapsed;
}

struct Sample {
  std::int64_t update{};
  std::int64_t frame{};
  std::size_t updateAllocations{};
  std::size_t frameAllocations{};
  std::size_t updateAllocatedBytes{};
  std::size_t frameAllocatedBytes{};
  std::size_t defaults{};
  std::size_t paints{};
  std::size_t commands{};
  std::size_t updateRedrawRequests{};
  std::size_t explicitFrames{};
  std::size_t bytes{};
  Invalidation invalidation{};
};

Sample run(std::size_t count, std::size_t rules, bool layered, std::size_t kind, std::size_t visibility) {
  Workload workload(count, layered, visibility);
  const bool masked = kind == 1;
  const auto first = theme(rules, FIRST_COLOR, masked);
  const auto second = theme(rules, kind == 0 ? FIRST_COLOR : SECOND_COLOR, masked);

  workload.update(first);
  workload.verify(masked, FIRST_COLOR);
  take(workload.app->tree().consumeFrameRequest());
  workload.render(visibility);
  take(workload.app->tree().consumeFrameRequest());
  workload.counts = {};
  workload.renders = 0;
  workload.output.bytes = 0;

  Sample sample;
  sample.update = measure(
    [&] {
      workload.update(second);
    },
    sample.updateAllocations, sample.updateAllocatedBytes);
  sample.defaults = workload.counts.defaults;
  sample.invalidation = take(workload.app->tree().consumeFrameRequest());
  workload.verify(masked, kind == 0 ? FIRST_COLOR : SECOND_COLOR);
  check(sample.invalidation == (kind == 2 ? Invalidation::PAINT : Invalidation::NONE),
        std::format("effective invalidation differs: layers={} kind={} visibility={} controls={} actual={}", layered,
                    kind, visibility, count, std::to_underlying(sample.invalidation)));
  check(sample.defaults <= count, "more than one defaults pass");

  if (kind != 0) {
    check(sample.defaults == count, "unequal update did not resolve all bindings");
  }

  // Measure an explicit full frame separately, even when the update requests no redraw.
  sample.frame = measure(
    [&] {
      sample.commands = workload.render(visibility);
    },
    sample.frameAllocations, sample.frameAllocatedBytes);
  sample.paints = workload.counts.paints;
  sample.bytes = workload.output.bytes;
  sample.explicitFrames = workload.renders;
  sample.updateRedrawRequests = sample.invalidation == Invalidation::NONE ? 0 : 1;
  check(visibility != 1 || sample.paints == 0, "hidden controls painted");
  take(workload.app->tree().consumeFrameRequest());
  check(take(workload.app->tree().consumeFrameRequest()) == Invalidation::NONE, "idle requested redraw");
  workload.close();

  return sample;
}

std::string summarize(std::vector<std::int64_t> values) {
  auto sorted = values;

  std::ranges::sort(sorted);
  check(!sorted.empty() && sorted.front() >= 0, "invalid clock sample");

  auto result = std::format("{{\"count\":{},\"p50\":{},\"p95\":{},\"samples\":[", values.size(),
                            sorted[(sorted.size() * 50 + 99) / 100 - 1], sorted[(sorted.size() * 95 + 99) / 100 - 1]);

  for (std::size_t i = 0; i < values.size(); ++i) {
    result += std::format("{}{}", i ? "," : "", values[i]);
  }

  return result + "]}";
}

std::string compiler() {
#if defined(__clang__)
  return std::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
  return std::format("MSVC {}", _MSC_FULL_VER);
#elif defined(__GNUC__)
  return std::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}

std::string execute(const Options &options) {
#ifdef PORTALS_BENCHMARK_BUILD_TYPE
  constexpr std::string_view BUILD = PORTALS_BENCHMARK_BUILD_TYPE;
#else
  constexpr std::string_view BUILD = "unspecified";
#endif
#ifdef NDEBUG
  constexpr bool ASSERTIONS = false;
#else
  constexpr bool ASSERTIONS = true;
#endif
  auto result = std::format(
    "{{\"schema\":1,\"complete\":true,\"correctness_verified\":true,\"check_only\":{},"
    "\"compiler\":\"{}\",\"build\":\"{}\",\"assertions_enabled\":{},\"pointer_bits\":{},"
    "\"clock\":\"steady_clock\",\"unit\":\"ns\",\"percentile\":\"nearest_rank\","
    "\"samples_per_case\":{},\"warmup_per_case\":{},\"allocations\":\"ordinary_new_calls_and_requested_bytes\","
    "\"frame_workload\":\"explicit arrange/paint/renderFullFrame/diff submit/flush to memory "
    "output; visible rows overlap; "
    "offscreen controls remain materialized; no virtualization\",\"cases\":[",
    options.checkOnly, compiler(), BUILD, ASSERTIONS, sizeof(void *) * 8, options.samples, options.warmup);
  bool first = true;

  for (const auto count : options.sizes) {
    for (const auto rules : {8U, 32U}) {
      for (std::size_t visibility = 0; visibility < VISIBILITY_NAMES.size(); ++visibility) {
        for (std::size_t kind = 0; kind < UPDATE_NAMES.size(); ++kind) {
          std::array<std::vector<std::int64_t>, 2> updates;
          std::array<std::vector<std::int64_t>, 2> frames;
          std::array<std::vector<std::int64_t>, 2> updateAllocations;
          std::array<std::vector<std::int64_t>, 2> frameAllocations;
          std::array<std::vector<std::int64_t>, 2> updateBytes;
          std::array<std::vector<std::int64_t>, 2> frameBytes;
          std::array<Sample, 2> last;

          for (std::size_t round = 0; round < options.samples + options.warmup; ++round) {
            for (std::size_t offset = 0; offset < 2; ++offset) {
              const auto api = (round + offset) % 2;
              last[api] = run(count, rules, api == 1, kind, visibility);

              if (round >= options.warmup) {
                updates[api].push_back(last[api].update);
                frames[api].push_back(last[api].frame);
                updateAllocations[api].push_back(static_cast<std::int64_t>(last[api].updateAllocations));
                frameAllocations[api].push_back(static_cast<std::int64_t>(last[api].frameAllocations));
                updateBytes[api].push_back(static_cast<std::int64_t>(last[api].updateAllocatedBytes));
                frameBytes[api].push_back(static_cast<std::int64_t>(last[api].frameAllocatedBytes));
              }
            }
          }

          for (std::size_t api = 0; api < 2; ++api) {
            const auto &sample = last[api];

            result += std::format(
              "{}{{\"api\":\"{}\",\"controls\":{},\"theme_rules\":{},\"visibility\":\"{}\","
              "\"operation\":\"theme_handoff\",\"update_kind\":\"{}\",\"update\":{},\"frame_"
              "render\":{},\"defaults_calls\":{},"
              "\"paint_calls\":{},\"render_commands\":{},\"effective_invalidation\":{},"
              "\"update_redraw_requests\":{},\"explicit_frame_count\":{},\"idle_redraws\":0,"
              "\"terminal_output_bytes\":{},\"update_allocations\":{},\"frame_allocations\":{},"
              "\"update_allocated_bytes\":{},\"frame_allocated_bytes\":{}}}",
              first ? "" : ",", api == 0 ? "legacy_composition" : "layers", count, rules, VISIBILITY_NAMES[visibility],
              UPDATE_NAMES[kind], summarize(updates[api]), summarize(frames[api]), sample.defaults, sample.paints,
              sample.commands, std::to_underlying(sample.invalidation), sample.updateRedrawRequests,
              sample.explicitFrames, sample.bytes, summarize(updateAllocations[api]), summarize(frameAllocations[api]),
              summarize(updateBytes[api]), summarize(frameBytes[api]));
            first = false;
          }
        }
      }
    }
  }

  return result + "]}\n";
}
} // namespace

/** @brief Measure layered-style update costs for multiple control visibility states.
 * @param argc Number of command-line arguments, including the executable name.
 * @param argv Borrowed argument strings; --help lists options, defaults and output behavior.
 * @return Zero on a complete report or help; one on argument, workload or output failure.
 */
int main(int argc, char **argv) {
  try {
    if (printHelp(argc, argv)) {
      return 0;
    }

    const auto options = parse(argc, argv);

    if (options.checkOnly) {
      verifyAllocationCounter();
    }

    const auto output = execute(options);

    if (options.output.empty()) {
      std::cout << output;
      check(static_cast<bool>(std::cout), "cannot write stdout");
    } else {
      std::ofstream file(options.output, std::ios::binary);

      file << output;
      file.close();
      check(static_cast<bool>(file), "cannot write output file");
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }

  return 0;
}
