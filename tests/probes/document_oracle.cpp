// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
// Internal acceptance probe. Invoke --help; generated inputs come from document_corpus.py.
// Reads bounded worker windows, emits JSON on stdout, never changes the input or OS caches.
#include "../../src/widgets/document_view_state.hpp"
#include "document_probe_metrics.hpp"
#include <lyra/lyra.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef PORTALS_PROBE_HAS_GZIP
#  include <portals/gzip_document.hpp>
#endif

namespace {
using namespace portals;
using namespace portals::detail;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t MIB = 1024 * 1024;
constexpr std::uint64_t FOUR_GIB = 4096 * MIB;
constexpr std::size_t RECORD_BYTES = 256;

struct Options {
  std::string file, kind{"plain"}, pattern{"sql"};
  unsigned sizeMiB{1}, longLineMiB{}, views{1}, evictions{40}, timeoutMs{60000};
  bool exact{}, check{}, help{};
};

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

template <class T, class E> T take(std::expected<T, E> value) {
  require(static_cast<bool>(value), "checked document operation failed");
  return std::move(*value);
}

struct Oracle {
  std::string unit, prefix, suffix;
  std::vector<SemanticRole> roles;
  std::uint64_t longLine{}, extent{};
  DocumentLanguage language{};

  explicit Oracle(const Options &options) : longLine(options.longLineMiB * MIB), extent(options.sizeMiB * MIB) {
    const bool sql = options.pattern == "sql";
    language = sql ? DocumentLanguage::MYSQL_LEXICAL : DocumentLanguage::MARKDOWN_SOURCE;
    unit = sql ? "INSERT INTO events VALUES (42, 'Práha 界'); -- chunk 00000000 bounded source"
               : "# Document00000000\ntext *bold* `code`\n```sql\nSELECT 'Práha 界';\n```\n";
    unit.append(RECORD_BYTES - unit.size() - 1, ' ');
    unit += '\n';
    prefix = sql ? "INSERT INTO events VALUES ('" : "# ";
    suffix = sql ? "');\n" : "\n";
    roles.assign(unit.size(), SemanticRole::NORMAL);
    std::array<DecodedScalar, RECORD_BYTES> decoded{};
    DocumentDecoder decoder({.explicitEncoding = DocumentEncoding::UTF8});
    const auto bytes = std::as_bytes(std::span(unit.data(), unit.size()));
    const auto step = decoder.feed(bytes, 0, true, decoded);
    require(step.status == DocumentDecodeStatus::DONE && step.inputConsumed == bytes.size(), "oracle decode failed");
    DocumentHighlighter lexer({.language = language});
    std::array<TokenRun, 512> runs{};
    const auto colored = lexer.feed(std::span(decoded).first(step.outputProduced), runs, true);
    require(colored.status == DocumentHighlightStatus::DONE && colored.inputConsumed == step.outputProduced,
            "oracle lexer failed");
    for (const auto &run : std::span(runs).first(colored.outputProduced)) {
      require(run.byteOffset + run.byteLength <= roles.size(), "oracle role range invalid");
      std::fill_n(roles.begin() + static_cast<std::ptrdiff_t>(run.byteOffset), static_cast<std::size_t>(run.byteLength),
                  run.role);
    }
  }

  char byte(std::uint64_t offset) const {
    require(offset < extent, "frame exceeds declared fixture extent");
    if (offset >= longLine) {
      const auto local = static_cast<std::size_t>((offset - longLine) % unit.size());
      const auto marker = unit.find("00000000");
      if (local >= marker && local - marker < 8) {
        auto index = (offset - longLine) / MIB;
        for (std::size_t digit = local - marker; digit < 7; ++digit) {
          index /= 10;
        }
        return static_cast<char>('0' + index % 10);
      }
      return unit[local];
    }
    if (offset < prefix.size()) {
      return prefix[static_cast<std::size_t>(offset)];
    }
    if (offset >= longLine - suffix.size()) {
      return suffix[static_cast<std::size_t>(offset - (longLine - suffix.size()))];
    }
    return 'x';
  }

  SemanticRole role(std::uint64_t offset) const {
    if (offset >= longLine) {
      return roles[static_cast<std::size_t>((offset - longLine) % roles.size())];
    }
    if (language == DocumentLanguage::MARKDOWN_SOURCE) {
      return offset == longLine - 1 ? SemanticRole::NORMAL : SemanticRole::HEADING;
    }
    if (offset < prefix.size() - 1) {
      return roles[static_cast<std::size_t>(offset)];
    }
    if (offset < longLine - 3) {
      return SemanticRole::STRING;
    }
    if (offset == longLine - 1) {
      return SemanticRole::NORMAL;
    }
    // The default MySQL client terminator is markup; the preceding parenthesis is SQL punctuation.
    return offset == longLine - 2 ? SemanticRole::MARKUP : SemanticRole::OPERATOR;
  }
};

class Counted final : public ByteDocumentProvider {
public:
  explicit Counted(std::shared_ptr<ByteDocumentProvider> input)
      : input_(std::move(input)), owner_(std::this_thread::get_id()) {
  }

  std::uint64_t reads{}, bytes{}, metadataCalls{}, ownerIo{};

  std::size_t pinnedBudgetBytes(const SourceBudget &root) const noexcept override {
    return input_->pinnedBudgetBytes(root);
  }

  bool tryReleaseOptionalStorage() noexcept override {
    return input_->tryReleaseOptionalStorage();
  }

  std::expected<ByteDocumentMetadata, DocumentError> discoverExtent(std::stop_token stop) override {
    ++metadataCalls;
    ownerIo += std::this_thread::get_id() == owner_;
    return input_->discoverExtent(stop);
  }

  ByteDocumentCapabilities capabilities() const noexcept override {
    return input_->capabilities();
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token stop) override {
    ++metadataCalls;
    ownerIo += std::this_thread::get_id() == owner_;
    return input_->metadata(stop);
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> output,
                                                    std::stop_token stop) override {
    ++reads;
    ownerIo += std::this_thread::get_id() == owner_;
    auto result = input_->read(command, output, stop);
    if (result) {
      bytes += result->bytesRead;
    }
    return result;
  }

private:
  std::shared_ptr<ByteDocumentProvider> input_;
  std::thread::id owner_;
};

struct View {
  std::shared_ptr<const DocumentScan> scan;
  std::shared_ptr<const DocumentPreparedFrame> frame;
};

struct Stage {
  std::string name;
  std::uint64_t target{}, firstNs{}, settledNs{}, turns{}, textBytes{}, maps{}, exactMaps{}, mismatches{}, peakBudget{};
  std::uint64_t logicalReadBytes{}, physicalReadBytes{};
  DocumentSyntaxState syntax{};
  DocumentSyntaxAccuracy accuracy{};
  bool eof{}, autoPreviewWithoutFrame{};
  DocumentSyntaxState frameSyntax{};
};

void verifyNavigation(const DocumentPreparedFrame &frame, std::uint64_t target, bool tail, bool exact,
                      bool producedFrame, std::uint64_t extent) {
  require(producedFrame, "stage did not produce its own frame");
  if (tail) {
    require(frame.finalEndVisible && frame.visibleByteEnd == extent, "End did not reach confirmed EOF");
  } else {
    require(frame.status.byteOffset == target, "requested byte anchor was not preserved");
    require(frame.status.visibleByteOffset <= target && frame.visibleByteEnd > target,
            "rendered window does not contain requested byte");
    if (!target) {
      require(frame.status.visibleByteOffset == 0, "Home did not reach source origin");
    }
  }
  if (exact) {
    require(frame.status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT &&
              frame.status.highlighting == DocumentSyntaxState::READY,
            "explicit exact recovery did not finish exactly");
  }
}

void checkNavigationContracts() {
  DocumentPreparedFrame valid;
  valid.status.byteOffset = 256;
  valid.status.visibleByteOffset = 256;
  valid.visibleByteEnd = 512;
  valid.status.syntaxAccuracy = DocumentSyntaxAccuracy::EXACT;
  valid.status.highlighting = DocumentSyntaxState::READY;
  verifyNavigation(valid, 256, false, true, true, 1024);
  unsigned rejected{};
  const auto mustReject = [&](auto operation) {
    try {
      operation();
    } catch (const std::runtime_error &) {
      ++rejected;
      return;
    }
    throw std::runtime_error("negative oracle contract was accepted");
  };
  mustReject([&] {
    verifyNavigation(valid, 256, false, false, false, 1024);
  });
  mustReject([&] {
    verifyNavigation(valid, 768, false, false, true, 1024);
  });
  mustReject([&] {
    verifyNavigation(valid, 0, true, false, true, 1024);
  });
  valid.status.syntaxAccuracy = DocumentSyntaxAccuracy::PREVIEW;
  mustReject([&] {
    verifyNavigation(valid, 256, false, true, true, 1024);
  });
  require(rejected == 4, "negative oracle checks did not complete");
}

void verify(const View &view, const Oracle &oracle, Stage &stage) {
  require(view.frame && view.frame->rows.size() == view.frame->anchors.size(), "frame/anchor mismatch");
  const auto &frame = *view.frame;
  for (std::size_t row = 0; row < frame.rows.size(); ++row) {
    auto offset = frame.anchors[row];
    for (char value : frame.rows[row].bytes()) {
      ++stage.textBytes;
      require(value == oracle.byte(offset++), "rendered bytes differ from fixture oracle");
    }
  }
  if (frame.status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT) {
    require(frame.text && frame.text->rows.size() == frame.rows.size(), "EXACT frame lacks rendered role rows");
    for (std::size_t row = 0; row < frame.rows.size(); ++row) {
      const auto clusters = frame.rows[row].graphemes();
      for (std::size_t cluster = 0; cluster < clusters.size(); ++cluster) {
        auto actual = SemanticRole::NORMAL;
        for (const auto &run : frame.text->rows[row]) {
          if (cluster >= run.first && cluster - run.first < run.count) {
            actual = run.role;
            break;
          }
        }
        const auto expected = oracle.role(frame.anchors[row] + clusters[cluster].byte_begin);
        if (actual != expected) {
          throw std::runtime_error("rendered role mismatch in " + stage.name + " at " +
                                   std::to_string(frame.anchors[row] + clusters[cluster].byte_begin) + " actual " +
                                   std::to_string(static_cast<int>(actual)) + " expected " +
                                   std::to_string(static_cast<int>(expected)));
        }
      }
    }
  }
  if (view.scan && view.scan->highlight) {
    for (const auto &map : view.scan->highlight->maps) {
      if (std::ranges::find(frame.anchors, map.rowOffset) == frame.anchors.end()) {
        continue;
      }
      ++stage.maps;
      if (map.classified) {
        ++stage.exactMaps;
        if (map.role != oracle.role(map.sourceOffset)) {
          throw std::runtime_error("source map mismatch in " + stage.name + " at " + std::to_string(map.sourceOffset) +
                                   " row " + std::to_string(map.rowOffset) + " rendered " +
                                   std::to_string(map.renderedOffset) + " actual " +
                                   std::to_string(static_cast<int>(map.role)) + " expected " +
                                   std::to_string(static_cast<int>(oracle.role(map.sourceOffset))));
        }
      }
    }
  }
  require(stage.mismatches == 0, "source-qualified colors differ from sequential oracle");
  if (frame.status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT) {
    require(stage.maps > 0 && stage.exactMaps == stage.maps, "EXACT frame has unknown source roles");
  }
  stage.syntax = frame.status.highlighting;
  stage.accuracy = frame.status.syntaxAccuracy;
  stage.eof = frame.finalEndVisible;
  if (stage.eof) {
    require(frame.visibleByteEnd == oracle.extent, "EOF differs from fixture extent");
  }
}

Stage prepare(std::string name, std::uint64_t anchor, bool tail, bool exact, View &view,
              const std::shared_ptr<ByteDocumentSource> &source, const std::shared_ptr<SourceBudget> &budget,
              const Oracle &oracle, const Options &options, std::stop_token stop) {
  Stage stage;
  stage.name = std::move(name);
  stage.target = anchor;
  DocumentPrepareIntent intent;
  intent.viewport = options.views == 1 ? CellSize{120, 40} : CellSize{60, 20};
  intent.anchor = anchor;
  intent.tail = tail;
  intent.config.language = oracle.language;
  intent.config.syntax = exact ? DocumentSyntaxPolicy::EXACT : DocumentSyntaxPolicy::AUTO;
  const auto start = Clock::now();
  bool producedFrame{};
  while (!stop.stop_requested()) {
    intent.resume = stage.turns != 0;
    intent.retainPreview = view.scan && view.scan->highlight && view.scan->highlight->preview;
    const auto maps = view.scan && view.scan->highlight ? view.scan->highlight->maxMaps : 0;
    const auto amount = take(
      documentPreparationBytes(intent.viewport, intent.config, {.textMaps = maps, .preview = intent.retainPreview}));
    auto credit = take(budget->reserve({amount, 0, 1, 0}));
    auto result = prepareDocument(source, intent, view.scan, ++stage.turns, std::move(credit), stop);
    stage.peakBudget = std::max<std::uint64_t>(stage.peakBudget, budget->usage().bytes);
    if (result->error) {
      throw std::runtime_error("worker preparation error " + std::to_string(static_cast<int>(result->error->code)));
    }
    view.scan = result->scan;
    if (result->frame) {
      producedFrame = true;
      view.frame = result->frame;
      if (!stage.firstNs) {
        stage.firstNs = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
      }
      if (result->continueWork && view.frame->status.highlighting == DocumentSyntaxState::PENDING) {
        intent.kind = DocumentPreparationKind::HIGHLIGHT;
      }
    }
    if (!result->continueWork) {
      require(static_cast<bool>(view.frame), "completed without a frame");
      verifyNavigation(*view.frame, anchor, tail, exact, producedFrame, oracle.extent);
      // AUTO exhaustion publishes terminal state separately from the retained preview frame.
      // Mirror that one production transition without weakening explicit EXACT or accepting a stale base.
      stage.autoPreviewWithoutFrame = !exact && !result->frame && result->baseFrame == view.frame &&
                                      intent.kind == DocumentPreparationKind::HIGHLIGHT &&
                                      result->syntax == DocumentSyntaxState::PREVIEW &&
                                      view.frame->status.highlighting == DocumentSyntaxState::PENDING &&
                                      result->syntaxAccuracy == view.frame->status.syntaxAccuracy;
      require(stage.autoPreviewWithoutFrame || (result->syntax == view.frame->status.highlighting &&
                                                result->syntaxAccuracy == view.frame->status.syntaxAccuracy),
              "terminal preparation metadata does not describe the published frame");
      stage.settledNs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
      verify(view, oracle, stage);
      stage.frameSyntax = view.frame->status.highlighting;
      stage.syntax = result->syntax;
      stage.accuracy = result->syntaxAccuracy;
      return stage;
    }
  }
  throw std::runtime_error("qualification deadline/cancellation reached");
}

void optional(std::ostream &out, std::optional<std::uint64_t> value) {
  if (value) {
    out << *value;
  } else {
    out << "null";
  }
}

int run(int argc, const char *const *argv) {
  Options options;
  auto cli =
    lyra::help(options.help) | lyra::opt(options.file, "path")["--file"]("Existing generated fixture, read only") |
    lyra::opt(options.kind, "kind")["--kind"]("plain or gzip (default: plain)") |
    lyra::opt(options.pattern, "pattern")["--pattern"]("sql or markdown corpus pattern (default: sql)") |
    lyra::opt(options.sizeMiB, "MiB")["--size-mib"]("Expected logical extent, 1..8192 (default: 1)") |
    lyra::opt(options.longLineMiB, "MiB")["--long-line-mib"]("Generated first line size, 0 disables (default: 0)") |
    lyra::opt(options.views, "count")["--views"]("Retained worker view states, 1 or 4 (default: 1)") |
    lyra::opt(options.evictions, "count")["--evictions"]("Additional distinct anchors, 0..64 (default: 40)") |
    lyra::opt(options.timeoutMs, "ms")["--timeout-ms"]("Whole operation deadline 1..1800000 ms (default: 60000)") |
    lyra::opt(options.exact)["--exact"](
      "Explicit full-prefix exact recovery at final sampled anchor; may scan the entire prefix");
  cli |= lyra::opt(options.check)["--check"]("Run negative navigation/exact oracle checks without files");
  const auto parsed = cli.parse({argc, argv});
  if (options.help) {
    std::cout
      << cli
      << "\nRead-only worker preparation and paged-vs-sequential periodic corpus oracle.\n"
         "Inputs must come from tests/probes/document_corpus.py with matching pattern/size/long-line.\n"
         "128 MiB shared reservations; total viewport120x40, or four60x20 windows. AUTO colors may be preview.\n"
         "Reports bounded JSON on stdout; no input changes, output files or OS-cache flushing.\n"
         "Three repetitions must be separate processes. This is not a whole-Application or terminal-pixel "
         "measurement.\n"
         "Exit0: complete/help,2: invalid CLI,1: timeout/runtime/oracle failure. Help requires no inputs.\n";
    return 0;
  }
  if (parsed && options.check) {
    checkNavigationContracts();
    std::cout << "{\"complete\":true,\"negative_contracts\":4}\n";
    return 0;
  }
  if (!parsed || options.file.empty() || (options.kind != "plain" && options.kind != "gzip") ||
      (options.pattern != "sql" && options.pattern != "markdown") || !options.sizeMiB || options.sizeMiB > 8192 ||
      options.longLineMiB > options.sizeMiB || (options.views != 1 && options.views != 4) || options.evictions > 64 ||
      !options.timeoutMs || options.timeoutMs > 1800000) {
    std::cerr << "Invalid arguments; use --help\n";
    return 2;
  }
  const Oracle oracle(options);
  auto budget = take(SourceBudget::create({.maxBytes = 128 * MIB}));
  const auto rootBaseline = budget->usage();
  std::weak_ptr<SourceBudget> weakBudget = budget;
  const std::u8string path(reinterpret_cast<const char8_t *>(options.file.data()), options.file.size());
  auto physical = std::make_shared<Counted>(take(FileDocumentProvider::create(std::filesystem::path(path))));
  std::shared_ptr<ByteDocumentProvider> provider = physical;
  if (options.kind == "gzip") {
#ifdef PORTALS_PROBE_HAS_GZIP
    provider = take(GzipDocumentProvider::create(provider, budget));
#else
    std::cerr << "gzip is not enabled in this build\n";
    return 2;
#endif
  }
  auto logical = std::make_shared<Counted>(std::move(provider));
  auto source = take(ByteDocumentSource::create(logical, budget));
  std::vector<Stage> stages;
  stages.reserve((options.evictions + 10) * options.views);
  std::vector<View> views(options.views);
  std::exception_ptr error;
  const auto baseline = document_probe::processMemory();
  std::thread worker([&] {
    try {
      std::stop_source cancellation;
      std::jthread deadline([&](std::stop_token done) {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        condition.wait_for(lock, done, std::chrono::milliseconds(options.timeoutMs), [] {
          return false;
        });
        if (!done.stop_requested()) {
          cancellation.request_stop();
        }
      });
      const auto work = [&](const char *name, std::uint64_t anchor, bool tail = false, bool exact = false) {
        for (auto &view : views) {
          const auto beforeLogical = logical->bytes, beforePhysical = physical->bytes;
          auto stage =
            prepare(name, anchor, tail, exact, view, source, budget, oracle, options, cancellation.get_token());
          stage.logicalReadBytes = logical->bytes - beforeLogical;
          stage.physicalReadBytes = physical->bytes - beforePhysical;
          stages.push_back(std::move(stage));
        }
      };
      work("initial", 0);
      work("seek_middle", oracle.extent / 2);
      work("seek_back", 256);
      if (oracle.extent > FOUR_GIB + 256) {
        work("before_4gib", FOUR_GIB - 128);
        work("after_4gib", FOUR_GIB + 128);
      }
      for (unsigned step = 0; step < options.evictions; ++step) {
        work("retained_checkpoint_cycle",
             ((step + 1) * (oracle.extent / (options.evictions + 2))) / RECORD_BYTES * RECORD_BYTES);
      }
      work("end", 0, true);
      if (options.exact) {
        work("explicit_exact", oracle.extent / 2, false, true);
      }
      work("home", 0);
    } catch (...) {
      error = std::current_exception();
    }
  });
  worker.join();
  const auto finalMemory = document_probe::processMemory();
  const auto ownerIo = physical->ownerIo + logical->ownerIo;
  views.clear();
  source.reset();
  logical.reset();
  physical.reset();
  const auto after = budget->usage();
  budget.reset();
  bool complete = !error && ownerIo == 0 && after.bytes == rootBaseline.bytes && !after.pages && !after.work &&
                  !after.consumers && weakBudget.expired();
  std::string failure;
  if (error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &caught) {
      failure = caught.what();
    }
  }
  std::cout << "{\"probe\":\"worker_document_oracle\",\"complete\":" << (complete ? "true" : "false")
            << ",\"failure\":" << std::quoted(failure) << ",\"expected_extent\":" << oracle.extent
            << ",\"views\":" << options.views << ",\"budget_after_release_bytes\":" << after.bytes
            << ",\"root_baseline_bytes\":" << rootBaseline.bytes
            << ",\"root_destroyed\":" << (weakBudget.expired() ? "true" : "false")
            << ",\"owner_thread_io_calls\":" << ownerIo << ",\"baseline_resident_bytes\":";
  optional(std::cout, baseline.residentBytes);
  std::cout << ",\"peak_resident_bytes\":";
  optional(std::cout, finalMemory.peakResidentBytes);
  std::cout << ",\"peak_private_bytes\":";
  optional(std::cout, finalMemory.peakPrivateBytes);
  std::cout << ",\"stages\":[";
  for (std::size_t i = 0; i < stages.size(); ++i) {
    const auto &stage = stages[i];
    if (i) {
      std::cout << ',';
    }
    std::cout << "{\"name\":" << std::quoted(stage.name) << ",\"anchor\":" << stage.target
              << ",\"first_frame_ns\":" << stage.firstNs << ",\"settled_ns\":" << stage.settledNs
              << ",\"worker_turns\":" << stage.turns << ",\"verified_text_bytes\":" << stage.textBytes
              << ",\"visible_source_maps\":" << stage.maps << ",\"verified_exact_maps\":" << stage.exactMaps
              << ",\"color_mismatches\":" << stage.mismatches << ",\"syntax_state\":" << static_cast<int>(stage.syntax)
              << ",\"syntax_accuracy\":" << static_cast<int>(stage.accuracy)
              << ",\"retained_frame_syntax_state\":" << static_cast<int>(stage.frameSyntax)
              << ",\"auto_preview_without_new_frame\":" << (stage.autoPreviewWithoutFrame ? "true" : "false")
              << ",\"eof\":" << (stage.eof ? "true" : "false") << ",\"sampled_budget_peak_bytes\":" << stage.peakBudget
              << ",\"logical_admitted_bytes\":" << stage.logicalReadBytes
              << ",\"physical_admitted_bytes\":" << stage.physicalReadBytes << '}';
  }
  std::cout << "]}\n";
  return complete ? 0 : 1;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> strings;
  std::vector<const char *> arguments;
  for (int i = 0; i < argc; ++i) {
    const auto value = std::filesystem::path(argv[i]).u8string();
    strings.emplace_back(reinterpret_cast<const char *>(value.data()), value.size());
  }
  for (const auto &value : strings) {
    arguments.push_back(value.c_str());
  }
#else
int main(int argc, char **argv) {
  const auto arguments = argv;
#endif
  try {
#ifdef _WIN32
    return run(argc, arguments.data());
#else
    return run(argc, arguments);
#endif
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
