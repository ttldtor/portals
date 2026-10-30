// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/examples/replay/terminal.hpp"
#include <doctest.h>
#include <portals/ui_style.hpp>

using namespace portals;
namespace sc = style::ct;

TEST_CASE("fractional scrollbar umbrella mounts in a plain lazy scene without a theme") {
  auto owner =
    ui::scene(ui::scrollArea(ui::label("ordinary content")).scrollbar("fractional-block").width(8).height(3));
  REQUIRE(owner.prepareChecked());
}

TEST_CASE("fractional scrollbar PSS admission preserves independent leaves") {
  auto owner = ui::scene(ui::scrollArea(ui::label("ordinary content")).scrollbar("fractional-block")) |
               ui::style("ScrollArea{scrollbar-track-pattern:minimal;scrollbar-thumb-pattern:fractional-block;}");
  REQUIRE(owner.prepareChecked());
  auto invalid =
    ui::scene(ui::scrollArea(ui::label("ordinary content")).scrollbar("fractional-block").scrollTrack("ascii"));
  CHECK_FALSE(invalid.prepareChecked());
}

TEST_CASE("fractional scrollbar resource names use the same prepared skin admission") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  auto owner =
    ui::scene("scene{ScrollArea{scrollbar.pattern=FRACTIONAL_BLOCK Label{text=\"ordinary content\"}}}", options);
  REQUIRE(owner.prepareChecked());
  auto invalid = ui::scene(
    "scene{ScrollArea{scrollbar{pattern=FRACTIONAL_BLOCK trackPattern=ASCII} Label{text=\"body\"}}}", options);
  CHECK_FALSE(invalid.prepareChecked());
}

#include <array>
#include <atomic>
#include <fstream>
#include <new>
#include <thread>

namespace fault {
thread_local std::ptrdiff_t remaining = -1;
thread_local std::size_t largest{};
thread_local bool measure{};
thread_local void (*hook)(){};
thread_local ui::Scene *scene{};
thread_local bool closeBlocked{};
thread_local std::function<void()> reentry;

struct Allocation {
  void *memory{};
  std::size_t bytes{};
};

constexpr std::size_t ALLOCATION_LEDGER_SLOTS = 2048;
thread_local std::array<Allocation, ALLOCATION_LEDGER_SLOTS> allocations{};
thread_local bool retain{};
thread_local bool overflow{};
thread_local std::size_t minimumBytes{};
thread_local std::size_t retainedBytes{};

void record(void *memory, std::size_t size) noexcept {
  if (!retain || size < minimumBytes) {
    return;
  }

  for (auto &allocation : allocations) {
    if (!allocation.memory) {
      allocation = {memory, size};
      retainedBytes += size;

      return;
    }
  }

  overflow = true;
}

void release(void *memory) noexcept {
  if (!retain) {
    return;
  }

  for (auto &allocation : allocations) {
    if (allocation.memory == memory && memory) {
      retainedBytes -= allocation.bytes;
      allocation = {};

      return;
    }
  }
}

struct RetentionScope {
  explicit RetentionScope(std::size_t threshold) noexcept {
    allocations = {};
    minimumBytes = threshold;
    retainedBytes = 0;
    overflow = false;
    retain = true;
  }

  ~RetentionScope() {
    retain = false;
    allocations = {};
  }
};
} // namespace fault

void *operator new(std::size_t size) {
  if (fault::hook) {
    std::exchange(fault::hook, nullptr)();
  }

  if (fault::measure) {
    fault::largest = std::max(fault::largest, size);
  }

  if (fault::remaining == 0) {
    throw std::bad_alloc();
  }

  if (fault::remaining > 0) {
    --fault::remaining;
  }

  if (auto *memory = std::malloc(size ? size : 1)) {
    fault::record(memory, size);

    return memory;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

// Route nonthrowing allocations through the same fault and retention ledger.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return ::operator new(size, std::nothrow);
}

void operator delete(void *memory) noexcept {
  fault::release(memory);
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  fault::release(memory);
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  fault::release(memory);
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  fault::release(memory);
  std::free(memory);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

namespace {
bool treeCause(const ManagedError &failure, TreeError cause) {
  if (const auto *tree = std::get_if<TreeError>(&failure.cause)) {
    return *tree == cause;
  }

  if (const auto *style = std::get_if<StyleError>(&failure.cause)) {
    return style->cause == cause;
  }

  return false;
}

struct TempFile {
  std::filesystem::path path;

  explicit TempFile(std::string_view bytes) {
    static std::atomic<unsigned> sequence{};
    const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto name = "portals-ui-style-" + std::to_string(epoch) + "-" + std::to_string(sequence.fetch_add(1)) + "-";
    const auto utf8 = name + "\xd1\x81\xd1\x82\xd0\xb8\xd0\xbb\xd1\x8c.pss";

    path = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output);
  }

  ~TempFile() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
  }
};

struct Defaults final : Widget {
  int &calls;

  explicit Defaults(int &count) : calls(count) {
  }

  ComputedStyle styleDefaults() const override {
    ++calls;

    return {};
  }
};

constexpr auto WIDTH = sc::sheet(sc::parameters(sc::parameter<"width">(std::int32_t{2})),
                                 sc::rule(sc::selector(), sc::minWidth(sc::token<"width">())));
constexpr auto COMPACT = sc::pss("* { bold: true; }", "compact.pss");
} // namespace

constexpr auto LIVE =
  sc::sheet(sc::parameters(sc::parameter<"bold">(false)), sc::rule(sc::selector(), sc::bold(sc::token<"bold">())));

TEST_CASE("nothrow test allocations preserve fault injection and retention accounting") {
  constexpr std::size_t ALLOCATION_BYTES = 37;
  bool scalarAllocated{};
  bool arrayAllocated{};
  std::size_t retainedAfterAllocation{};
  std::size_t retainedAfterRelease{};

  {
    fault::RetentionScope tracking(ALLOCATION_BYTES);
    auto *scalar = ::operator new(ALLOCATION_BYTES, std::nothrow);
    auto *array = ::operator new[](ALLOCATION_BYTES, std::nothrow);

    scalarAllocated = scalar != nullptr;
    arrayAllocated = array != nullptr;
    retainedAfterAllocation = fault::retainedBytes;
    ::operator delete(scalar);
    ::operator delete[](array, std::nothrow);
    retainedAfterRelease = fault::retainedBytes;
  }

  fault::remaining = 0;
  auto *failedScalar = ::operator new(ALLOCATION_BYTES, std::nothrow);
  auto *failedArray = ::operator new[](ALLOCATION_BYTES, std::nothrow);

  fault::remaining = -1;
  ::operator delete(failedScalar, std::nothrow);
  ::operator delete[](failedArray);
  CHECK(scalarAllocated);
  CHECK(arrayAllocated);
  CHECK(retainedAfterAllocation == 2 * ALLOCATION_BYTES);
  CHECK(retainedAfterRelease == 0);
  CHECK(failedScalar == nullptr);
  CHECK(failedArray == nullptr);
}

TEST_CASE("deferred owning style preserves identity and lvalue owner") {
  const auto stage = ui::style(std::string("* { bold: true; }"));
  auto owner = ui::scene(ui::label("ready"));
  static_assert(std::same_as<decltype(owner | stage), ui::Scene &>);
  CHECK(&(owner | stage) == &owner);
  REQUIRE(owner.prepareChecked());
  auto managed = owner.managed();
  REQUIRE(managed);
  auto sources = managed->get().styleSources();
  REQUIRE(sources);
  REQUIRE(sources->size() == 3);
  CHECK(sources->back().document);
  owner | stage;
  auto repeated = managed->get().styleSources();
  REQUIRE(repeated);
  REQUIRE(repeated->size() == 3);
  CHECK(managed->get().validateStyleLayer(sources->back().handle));
}

TEST_CASE("weak live observation closes before user captures retire") {
  const auto stage = ui::liveTheme(LIVE);
  const auto live = stage.ref();
  CHECK_FALSE(live.get<"bold">());
  auto owner = ui::scene(ui::label("ready"));
  owner | stage;
  REQUIRE(owner.prepareChecked());
  REQUIRE(live.get<"bold">());
  CHECK_FALSE(*live.get<"bold">());
  REQUIRE(live.set<"bold">(true));
  CHECK(*live.get<"bold">());
  REQUIRE(owner.close());
  auto closed = live.get<"bold">();
  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}

TEST_CASE("factory errors before scene construction reach the final sink") {
  int reports{};
  ui::RunAppOptions options;
  options.report = [&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    CHECK(std::get<TreeError>(report.failure->cause) == TreeError::CALLBACK_FAILED);
  };
  const auto status = ui::runApp(
    []() -> ui::Description<Label> {
      throw 42;
    },
    options);
  CHECK(status == 1);
  CHECK(reports == 1);
}

TEST_CASE("method free and pipe sources preserve checked owning provenance") {
  auto parsed = parsePssDocument("* { bold: true; }", "runtime.pss");
  REQUIRE(parsed);

  for (unsigned form = 0; form != 5; ++form) {
    auto owner = ui::scene(ui::label("ready"));

    if (form == 0) {
      owner.style(COMPACT);
    } else if (form == 1) {
      ui::style(owner, COMPACT);
    } else if (form == 2) {
      owner | ui::style(COMPACT);
    } else if (form == 3) {
      owner.style(*parsed);
    } else {
      owner.style(std::string("* { bold: true; }"));
    }

    REQUIRE(owner.prepareChecked());
    auto managed = owner.managed();
    REQUIRE(managed);
    auto sources = managed->get().styleSources();
    REQUIRE(sources);
    REQUIRE(sources->size() == 3);
    REQUIRE(sources->back().document);
    CHECK(sources->back().tier == StyleTier::APPLICATION);
    CHECK(sources->back().document->sheet.rules[0].declarations.bold == true);
  }
}

TEST_CASE("array literals retain embedded NUL while sized views retain all bytes") {
  constexpr char BAD[] = "* { bold: true; }\0* { italic: true; }";

  for (bool method : {false, true}) {
    auto owner = ui::scene(ui::label("ready"));

    if (method) {
      owner.style(BAD);
    } else {
      owner | ui::style(BAD);
    }

    auto prepared = owner.prepareChecked();
    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().pss);
    CHECK(prepared.error().pss->code == PssErrorCode::NUL_BYTE);
    CHECK(prepared.error().pss->location.offset == 17);
  }

  const char *pointer = "* { bold: true; }";
  auto owner = ui::scene(ui::label("ready"));

  owner.style(pointer);
  REQUIRE(owner.prepareChecked());
}

TEST_CASE("mutable document is validated before copy and never retained through a mutable alias") {
  auto parsed = parsePssDocument("* { bold: true; }", "owned.pss");
  REQUIRE(parsed);
  const auto stage = ui::style(*parsed);

  parsed->sheet.rules[0].declarations.bold = false;
  parsed->sourceName = "mutated.pss";

  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  REQUIRE(owner.prepareChecked());
  auto sources = owner.managed()->get().styleSources();
  REQUIRE(sources);
  REQUIRE(sources->back().document);
  CHECK(sources->back().document->sourceName == "owned.pss");
  CHECK(sources->back().document->sheet.rules[0].declarations.bold == true);

  parsed->declarations.resize(20000);
  parsed->origins[0].effectiveDeclaration[0] = parsed->declarations.size() + 1;
  fault::largest = 0;
  fault::measure = true;
  const auto forged = ui::style(*parsed);
  fault::measure = false;
  const auto largest = fault::largest;

  CHECK(largest < 20000 * sizeof(PssDeclarationOrigin));
  auto invalid = ui::scene(ui::label("invalid"));

  invalid | forged;
  auto admitted = invalid.prepareChecked();
  REQUIRE_FALSE(admitted);
  REQUIRE(admitted.error().pss);
  CHECK(admitted.error().pss->code == PssErrorCode::INVALID_VALUE);
  REQUIRE(admitted.error().styleSource);
  CHECK(*admitted.error().styleSource == "mutated.pss");
}

TEST_CASE("document scalar occurrence and origin forgery are exact typed admission failures") {
  for (unsigned forgery = 0; forgery != 4; ++forgery) {
    auto document = parsePssDocument("* { bold: true; }", "forged.pss");
    REQUIRE(document);

    if (forgery == 0) {
      document->originKind = static_cast<PssOriginKind>(99);
    } else if (forgery == 1) {
      document->declarations[0].property = static_cast<PssProperty>(99);
    } else if (forgery == 2) {
      document->declarations[0].ruleIndex = 99;
    } else {
      document->declarations[0].value.width = 99;
    }

    auto owner = ui::scene(ui::label("ready"));

    owner.style(*document);
    auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().pss);
    CHECK(result.error().pss->code == PssErrorCode::INVALID_VALUE);
    CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  }
}

TEST_CASE("independent stages compose while explicit names upsert in the selected tier") {
  const auto first = ui::style("* { bold: true; }");
  const auto second = ui::style("* { italic: true; }");
  const ui::StyleOptions user{.tier = StyleTier::USER, .name = "override", .sourceName = "user.pss"};
  auto owner = ui::scene(ui::label("ready"));

  owner | first | second;
  owner.style("* { underline: true; }", user);
  REQUIRE(owner.prepareChecked());
  auto managed = owner.managed();
  REQUIRE(managed);
  auto before = managed->get().styleSources();
  REQUIRE(before);
  REQUIRE(before->size() == 5);
  CHECK(before->back().tier == StyleTier::USER);

  owner.style("* { underline: false; }", user);
  auto after = managed->get().styleSources();
  REQUIRE(after);
  REQUIRE(after->size() == 5);
  REQUIRE(after->back().document);
  CHECK(after->back().document->sheet.rules[0].declarations.underline == false);
  CHECK(managed->get().validateStyleLayer(before->back().handle));
}

TEST_CASE("unnamed generated names cannot overwrite a low-level foreign layer") {
  const auto seed = ui::style("* { bold: true; }");
  auto owner = ui::scene(ui::label("ready"));

  owner | seed;
  REQUIRE(owner.prepareChecked());
  auto managed = owner.managed();
  REQUIRE(managed);
  auto before = managed->get().styleSources();
  REQUIRE(before);
  const auto &seedName = before->back().name;
  const auto split = seedName.find_last_of('_');
  const auto next = std::stoull(seedName.substr(split + 1)) + 1;
  const auto foreignName = seedName.substr(0, split + 1) + std::to_string(next);
  auto foreign = managed->get().addLayer(StyleTier::APPLICATION, foreignName, {});
  REQUIRE(foreign);
  bool intact{};

  owner.onError([&](const ManagedDiagnostic &diagnostic) {
    auto sources = managed->get().styleSources();

    intact = !sources && treeCause(sources.error(), TreeError::WOULD_BLOCK) &&
             treeCause(diagnostic.error, TreeError::INVALID_ARGUMENT) && diagnostic.error.pss.has_value();
  });
  owner | ui::style("* { italic: true; }");

  REQUIRE_FALSE(owner.checked());
  CHECK(intact);
  CHECK(treeCause(owner.checked().error(), TreeError::INVALID_ARGUMENT));
}

TEST_CASE("explicit reserved namespace names are rejected without overwriting") {
  auto owner = ui::scene(ui::label("ready"));
  const ui::StyleOptions options{.name = "__portals_ui_style_foreign"};

  owner.style("* { bold: true; }", options);
  auto prepared = owner.prepareChecked();
  REQUIRE_FALSE(prepared);
  CHECK(treeCause(prepared.error(), TreeError::INVALID_ARGUMENT));
}

TEST_CASE("reused removed stage checks the original stale handle and never resurrects") {
  const auto stage = ui::style("* { bold: true; }");
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  REQUIRE(owner.prepareChecked());
  auto managed = owner.managed();
  REQUIRE(managed);
  auto sources = managed->get().styleSources();
  REQUIRE(sources);
  REQUIRE(managed->get().removeLayer(sources->back().handle));
  bool absent{};

  owner.onError([&](const ManagedDiagnostic &diagnostic) {
    auto current = managed->get().styleSources();

    absent = !current && treeCause(current.error(), TreeError::WOULD_BLOCK) &&
             std::holds_alternative<StyleError>(diagnostic.error.cause);
  });
  owner | stage;

  REQUIRE_FALSE(owner.checked());
  CHECK(absent);
}

TEST_CASE("equal and metadata-only replacements request no defaults recomputation or redraw") {
  example::ReplayTerminal terminal({{20, "\x1b[21~", {}}}, 80, {20, 4});
  ui::SceneOptions sceneOptions;
  int defaults{}, changes{};
  sceneOptions.clock = terminal.clock();
  const ui::StyleOptions first{.name = "stable", .sourceName = "first.pss"};
  const ui::StyleOptions second{.name = "stable", .sourceName = "second.pss"};
  const auto stage = ui::style("* { bold: true; }", first);
  auto owner = ui::scene(ui::custom<Defaults>(std::ref(defaults)), sceneOptions);

  owner | stage;
  owner.onKey(ui::key(InputKey::F10), [&] {
    const auto before = defaults;

    owner | stage;
    owner.style("* { bold: true; }", second);

    CHECK(defaults == before);
    auto sources = owner.managed()->get().styleSources();
    REQUIRE(sources);
    CHECK(sources->back().document->sourceName == "second.pss");
    ++changes;
  });
  auto result = owner.run(terminal);
  REQUIRE(result);
  CHECK(changes == 1);
  CHECK(result->frames == 1);
  CHECK(terminal.closed);
}

TEST_CASE("low-level equal replacement clears provenance and stage reapply restores it without recomputation") {
  int defaults{};
  auto owner = ui::scene(ui::custom<Defaults>(std::ref(defaults)));
  const auto stage = ui::style("* { bold: true; }");

  owner | stage;
  REQUIRE(owner.prepareChecked());
  auto managed = owner.managed();
  REQUIRE(managed);
  auto sources = managed->get().styleSources();
  REQUIRE(sources);
  const auto before = defaults;

  REQUIRE(managed->get().replaceLayer(sources->back().handle, sources->back().document->sheet));
  auto replaced = managed->get().styleSources();
  REQUIRE(replaced);
  CHECK_FALSE(replaced->back().document);

  owner | stage;
  auto restored = managed->get().styleSources();
  REQUIRE(restored);
  CHECK(restored->back().document);
  CHECK(defaults == before);
}

TEST_CASE("temporary owner chain and stored generic stages preserve one owning facade") {
  auto report = ui::reportOnError([](const ui::SceneReport &) {
  });
  auto style = ui::style(std::string("* { bold: true; }"));
  auto owner = ui::scene(ui::label("ready")) | style | report;

  REQUIRE(owner.prepareChecked());
  auto observed = owner.ref();
  auto moved = std::move(owner) | ui::prepare();

  CHECK(observed.quit());
  CHECK(moved.checked());
  auto empty = owner.ref().quit();
  REQUIRE_FALSE(empty);
  CHECK(treeCause(empty.error(), TreeError::CLOSED));
}

TEST_CASE("deferred Unicode file source owns its path and reads strict BOM UTF8 bytes") {
  TempFile source("\xef\xbb\xbf* { bold: true; }");
  const auto stage = ui::style(ui::file(std::filesystem::path(source.path)));
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  REQUIRE(owner.prepareChecked());
  auto sources = owner.managed()->get().styleSources();
  REQUIRE(sources);
  REQUIRE(sources->back().document);
  CHECK(sources->back().document->sheet.rules[0].declarations.bold == true);
  CHECK(sources->back().document->origins[0].selector.offset == 3);
  CHECK(sources->back().document->sourceName.find("\xd1\x81") != std::string::npos);
}

TEST_CASE("deferred file errors own source identity and original operation category") {
  TempFile source("* { bold: true; }");
  const auto stage = ui::style(ui::file(source.path));
  std::filesystem::remove(source.path);
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  auto result = owner.prepareChecked();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().file);
  REQUIRE(result.error().styleSource);
  CHECK(result.error().file->operation == PssFileOperation::OPEN);
  CHECK(&result.error().file->cause.category() == &std::generic_category());
  CHECK(result.error().file->cause.value() != 0);
  CHECK(result.error().styleSource->find("portals-ui-style-") != std::string::npos);
}

TEST_CASE("file input limit and strict invalid bytes are separate typed failures") {
  for (unsigned variant = 0; variant != 3; ++variant) {
    const std::string bytes = variant == 0   ? std::string(MAX_PSS_BYTES + 1, ' ')
                              : variant == 1 ? std::string("* {}\0tail", 10)
                                             : std::string("\xff", 1);
    TempFile source(bytes);
    auto owner = ui::scene(ui::label("ready"));

    owner.style(ui::file(source.path));
    auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);

    if (variant == 0) {
      REQUIRE(result.error().file);
      CHECK(result.error().file->operation == PssFileOperation::INPUT_LIMIT);
      CHECK(result.error().file->cause == std::errc::file_too_large);
    } else {
      REQUIRE(result.error().pss);
      CHECK(result.error().pss->code == (variant == 1 ? PssErrorCode::NUL_BYTE : PssErrorCode::INVALID_UTF8));
    }
  }
}

TEST_CASE("parse failure preserves owned position source and registration origin in collector") {
  const auto origin = std::source_location::current();
  const auto stage = ui::style("\n* { unknown: true; }", {.sourceName = "broken.pss"}, origin);
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  auto result = owner.prepareChecked();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().pss);
  CHECK(result.error().pss->code == PssErrorCode::UNKNOWN_PROPERTY);
  CHECK(result.error().pss->location.line == 2);
  CHECK(result.error().pss->location.column == 5);
  REQUIRE(result.error().styleSource);
  CHECK(*result.error().styleSource == "broken.pss");
  auto diagnostics = owner.diagnostics();
  REQUIRE(diagnostics);
  REQUIRE(diagnostics->errors.size() == 1);
  CHECK(diagnostics->errors[0].origin.line() == origin.line());
}

TEST_CASE("weak quit remains usable from compact key and button callbacks across facade moves") {
  for (bool key : {false, true}) {
    example::ReplayTerminal terminal({{10, key ? "\x1b[21~" : "\r", {}}}, 70, {20, 4});
    ui::SceneOptions options;
    options.clock = terminal.clock();
    ui::Ref<Button> button;
    auto owner = ui::scene(ui::button("Quit").reference(button), options);
    auto quit = ui::quit(owner.ref());

    if (key) {
      owner.onKey(ui::key(InputKey::F10), quit);
    } else {
      owner.configure([button, quit](ManagedScene &) mutable {
        return button.onClick(quit);
      });
    }

    auto moved = std::move(owner);
    CHECK(moved.runApp(terminal) == 0);
    CHECK(terminal.closed);
  }
}

TEST_CASE("weak quit notification reentry is recoverable and reports its exact RUN cause") {
  auto owner = ui::scene(ui::label("ready"));
  auto action = ui::quit(owner.ref());
  int notifications{};

  owner.onError([&](const ManagedDiagnostic &) {
    ++notifications;
    action();
  });
  REQUIRE(owner.prepareChecked());
  REQUIRE(owner.managed()->get().reportError(ManagedError{ManagedPhase::INPUT, {}, TreeError::INVALID_ARGUMENT}));
  CHECK(owner.checked());
  auto diagnostics = owner.diagnostics();
  REQUIRE(diagnostics);
  REQUIRE(diagnostics->errors.size() == 2);
  CHECK(diagnostics->errors.back().error.phase == ManagedPhase::RUN);
  CHECK(treeCause(diagnostics->errors.back().error, TreeError::WOULD_BLOCK));
  CHECK(notifications == 1);
}

TEST_CASE("foreign thread weak and live operations reject before raw UI access") {
  const auto stage = ui::liveTheme(LIVE);
  const auto live = stage.ref();
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  REQUIRE(owner.prepareChecked());
  const auto weak = owner.ref();
  bool quitWrong{}, liveWrong{};

  std::thread worker([&] {
    const auto quit = weak.quit();
    const auto value = live.get<"bold">();

    quitWrong = !quit && treeCause(quit.error(), TreeError::WRONG_THREAD);
    liveWrong = !value && treeCause(value.error(), TreeError::WRONG_THREAD);
  });
  worker.join();

  CHECK(quitWrong);
  CHECK(liveWrong);
  REQUIRE(owner.diagnostics());
  CHECK(owner.diagnostics()->errors.empty());
}

TEST_CASE("live stage binds only one scene and cleanup prevents rebinding permanently") {
  const auto stage = ui::liveTheme(LIVE);
  const auto copy = stage;
  auto first = ui::scene(ui::label("first"));

  first | stage | copy;
  REQUIRE(first.prepareChecked());
  REQUIRE(stage.get<"bold">());
  auto second = ui::scene(ui::label("second"));

  second | copy;
  auto denied = second.prepareChecked();
  REQUIRE_FALSE(denied);
  CHECK(treeCause(denied.error(), TreeError::INVALID_ARGUMENT));
  CHECK(stage.get<"bold">());
  REQUIRE(first.close());
  auto third = ui::scene(ui::label("third"));

  third | stage;
  auto closed = third.prepareChecked();
  REQUIRE_FALSE(closed);
  CHECK(treeCause(closed.error(), TreeError::CLOSED));
}

TEST_CASE("scene owns actual live binding after an external temporary stage disappears") {
  auto owner = ui::scene(ui::label("ready"));
  const auto live = [&] {
    const auto temporary = ui::liveTheme(LIVE);
    auto weak = temporary.ref();

    owner | temporary;

    return weak;
  }();

  REQUIRE(owner.prepareChecked());
  REQUIRE(live.set<"bold">(true));
  CHECK(*live.get<"bold">());
  REQUIRE(owner.close());
  CHECK_FALSE(live.get<"bold">());
}

TEST_CASE("live invalid update and stale equal update are recoverable exact typed collector errors") {
  const auto stage = ui::liveTheme(WIDTH);
  auto owner = ui::scene(ui::label("ready"));

  owner.style(COMPACT);
  owner | stage;
  REQUIRE(owner.prepareChecked());
  auto invalid = stage.set<"width">(std::int32_t{-1});
  REQUIRE_FALSE(invalid);
  CHECK(std::holds_alternative<StyleError>(invalid.error().cause));
  CHECK(*stage.get<"width">() == 2);
  CHECK(owner.checked());
  REQUIRE(owner.managed()->get().theme({}));
  auto stale = stage.set<"width">(std::int32_t{2});
  REQUIRE_FALSE(stale);
  CHECK(std::holds_alternative<StyleError>(stale.error().cause));
  CHECK(owner.checked());
  auto diagnostics = owner.diagnostics();
  REQUIRE(diagnostics);
  CHECK(diagnostics->errors.size() == 2);
}

TEST_CASE("live controller and weak stop are revoked before managed callback captures retire") {
  const auto stage = ui::liveTheme(LIVE);
  auto owner = ui::scene(ui::label("ready"));
  const auto live = stage.ref();
  const auto weak = owner.ref();
  bool retired{}, liveClosed{}, weakClosed{};

  struct Capture {
    decltype(live) theme;
    ui::SceneRef scene;
    bool &retired, &liveClosed, &weakClosed;

    ~Capture() {
      retired = true;
      const auto value = theme.get<"bold">();
      const auto stopped = scene.quit();

      liveClosed = !value && treeCause(value.error(), TreeError::CLOSED);
      weakClosed = !stopped && treeCause(stopped.error(), TreeError::CLOSED);
    }
  };

  auto capture = std::make_shared<Capture>(live, weak, retired, liveClosed, weakClosed);

  owner | stage;
  owner.onKey(ui::character(U'a'), [capture] {
  });
  capture.reset();
  REQUIRE(owner.prepareChecked());
  REQUIRE(owner.close());
  CHECK(retired);
  CHECK(liveClosed);
  CHECK(weakClosed);
}

TEST_CASE("failed preallocated live attachment leaves the stage unbound for a fresh scene") {
  const auto stage = ui::liveTheme(LIVE);
  auto failed = ui::scene(ui::label("failed"));

  fault::remaining = 0;
  failed | stage;
  fault::remaining = -1;

  REQUIRE_FALSE(failed.checked());
  CHECK(treeCause(failed.checked().error(), TreeError::NO_MEMORY));
  auto retry = ui::scene(ui::label("retry"));

  retry | stage;
  REQUIRE(retry.prepareChecked());
  CHECK(stage.get<"bold">());
}

TEST_CASE("facade style allocation error retains preowned source without allocating a replacement label") {
  const auto stage = ui::style("* { bold: true; }", {.sourceName = "allocation.pss"});
  auto owner = ui::scene(ui::label("ready"));

  fault::remaining = 0;
  owner | stage;
  fault::remaining = -1;

  auto failure = owner.checked();
  REQUIRE_FALSE(failure);
  CHECK(treeCause(failure.error(), TreeError::NO_MEMORY));
  REQUIRE(failure.error().pss);
  CHECK(failure.error().pss->code == PssErrorCode::ALLOCATION);
  REQUIRE(failure.error().styleSource);
  CHECK(*failure.error().styleSource == "allocation.pss");
}

TEST_CASE("stationary factory setup uses native-equivalent runner cleanup with a replay transport") {
  example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 70, {20, 4});
  ui::RunAppOptions options;
  int factories{}, setups{};
  options.scene.clock = terminal.clock();
  options.terminal = &terminal;
  const auto result = ui::runApp(
    [&] {
      ++factories;

      return ui::label("ready");
    },
    [&](ui::Scene &owner) {
      ++setups;
      owner.style(COMPACT).onKey(ui::key(InputKey::F10), ui::quit(owner.ref()));
    },
    options);

  CHECK(result == 0);
  CHECK(factories == 1);
  CHECK(setups == 1);
  CHECK(terminal.closed);
}

TEST_CASE("factory exact managed failure and setup throws reach the configured final sink once") {
  for (unsigned failure = 0; failure != 4; ++failure) {
    ui::RunAppOptions options;
    int reports{}, setups{};
    options.report = [&](const ui::SceneReport &report) {
      ++reports;
      REQUIRE(report.failure);
      CHECK(
        treeCause(*report.failure, failure == 1 || failure == 3 ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED));
    };

    auto factory = [&]() -> std::expected<ui::Description<Label>, ManagedError> {
      if (failure == 0) {
        return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::CALLBACK_FAILED});
      }

      if (failure == 1) {
        throw std::bad_alloc();
      }

      return ui::label("ready");
    };
    const auto result = ui::runApp(
      factory,
      [&](ui::Scene &) {
        ++setups;

        if (failure == 2) {
          throw 42;
        }

        throw std::bad_alloc();
      },
      options);

    CHECK(result == 1);
    CHECK(reports == 1);
    CHECK(setups == (failure < 2 ? 0 : 1));
  }
}

TEST_CASE("exact setup failure is preserved and a throwing final sink never escapes") {
  ui::RunAppOptions options;
  int reports{};
  options.report = [&](const ui::SceneReport &report) {
    ++reports;
    REQUIRE(report.failure);
    REQUIRE(report.failure->pss);
    CHECK(report.failure->pss->code == PssErrorCode::INVALID_WIDTH);
    throw 42;
  };
  ManagedError failure{ManagedPhase::STYLE, {}, TreeError::INVALID_ARGUMENT};
  failure.pss = PssError{.code = PssErrorCode::INVALID_WIDTH, .message = "fixed"};

  CHECK(ui::runApp(
          [] {
            return ui::label("ready");
          },
          [&](ui::Scene &) -> std::expected<void, ManagedError> {
            return std::unexpected(failure);
          },
          options) == 1);
  CHECK(reports == 1);
}

TEST_CASE("style allocation sweep retains its preowned identity through every facade allocation boundary") {
  const auto stage = ui::style("* { bold: true; }", {.sourceName = "fault-sweep.pss"});
  unsigned failures{}, successes{};

  constexpr std::ptrdiff_t MAX_BOUNDARY_ALLOCATION_PROBES = 256;

  for (std::ptrdiff_t allocation = 0; allocation != MAX_BOUNDARY_ALLOCATION_PROBES; ++allocation) {
    auto owner = ui::scene(ui::label("ready"));

    REQUIRE(owner.prepareChecked());
    fault::remaining = allocation;
    owner | stage;
    fault::remaining = -1;
    auto result = owner.checked();

    if (!result) {
      ++failures;
      INFO(allocation);
      CHECK(treeCause(result.error(), TreeError::NO_MEMORY));
      CHECK(result.error().phase == ManagedPhase::STYLE);
      REQUIRE(result.error().styleSource);
      CHECK(*result.error().styleSource == "fault-sweep.pss");
    } else {
      ++successes;

      break;
    }
  }

  CHECK(failures > 0);
  CHECK(successes > 0);
}

TEST_CASE("live candidate allocation runs inside facade admission and rejects close reentry") {
  const auto stage = ui::liveTheme(LIVE);
  auto owner = ui::scene(ui::label("ready"));

  REQUIRE(owner.prepareChecked());
  fault::scene = &owner;
  fault::closeBlocked = false;
  fault::hook = [] {
    const auto result = fault::scene->close();

    fault::closeBlocked = !result && treeCause(result.error(), TreeError::WOULD_BLOCK);
  };
  owner | stage;
  fault::hook = nullptr;
  fault::scene = nullptr;

  CHECK(fault::closeBlocked);
  REQUIRE_FALSE(owner.checked());
  CHECK(treeCause(owner.checked().error(), TreeError::WOULD_BLOCK));
  CHECK_FALSE(stage.get<"bold">());
}

TEST_CASE("prepared factory description exposes scene construction allocation failure to the preowned sink") {
  auto description = ui::label("ready");
  ui::RunAppOptions options;
  int reports{};
  bool returned{};

  options.report = [&](const ui::SceneReport &report) {
    fault::remaining = -1;
    ++reports;
    REQUIRE(report.failure);
    CHECK(report.failure->phase == ManagedPhase::MOUNT);
    CHECK(treeCause(*report.failure, TreeError::NO_MEMORY));
  };
  const auto result = ui::runApp(
    [&] {
      returned = true;
      fault::remaining = 0;

      return std::move(description);
    },
    options);
  fault::remaining = -1;

  CHECK(returned);
  CHECK(result == 1);
  CHECK(reports == 1);
}

TEST_CASE("live parameter reentry preserves exact recoverable error and the outer committed value") {
  const auto stage = ui::liveTheme(WIDTH);
  const auto live = stage.ref();
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  REQUIRE(owner.prepareChecked());
  bool blocked{};

  fault::reentry = [&] {
    auto result = live.set<"width">(std::int32_t{2});

    blocked = !result && treeCause(result.error(), TreeError::WOULD_BLOCK);
  };
  fault::hook = [] {
    fault::reentry();
  };

  auto result = live.set<"width">(std::int32_t{3});
  fault::hook = nullptr;
  fault::reentry = {};

  REQUIRE(result);
  CHECK(*result);
  CHECK(blocked);
  CHECK(*live.get<"width">() == 3);
  CHECK(owner.checked());
  auto diagnostics = owner.diagnostics();
  REQUIRE(diagnostics);
  REQUIRE(diagnostics->errors.size() == 1);
  CHECK(treeCause(diagnostics->errors[0].error, TreeError::WOULD_BLOCK));
}

TEST_CASE("compact live callback errors are collected without stopping a successful runner") {
  example::ReplayTerminal terminal({{10, "\x1b[21~", {}}}, 70, {20, 4});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  const auto stage = ui::liveTheme(WIDTH);
  const auto live = stage.ref();
  auto owner = ui::scene(ui::label("ready"), options);
  int reports{}, updates{};

  owner | stage;
  owner.onKey(ui::key(InputKey::F10), [live, &updates] {
    (void)live.set<"width">(std::int32_t{-1});
    ++updates;
  });
  owner.reportOnError([&](const ui::SceneReport &report) {
    ++reports;
    CHECK_FALSE(report.failure);
    CHECK(report.retainedErrors == 1);
  });

  CHECK(owner.runApp(terminal) == 0);
  CHECK(updates == 1);
  CHECK(reports == 1);
  CHECK(terminal.closed);
}

TEST_CASE("directory source preserves native OPEN or POSIX READ error category") {
  TempFile source("");
  REQUIRE(std::filesystem::remove(source.path));
  REQUIRE(std::filesystem::create_directory(source.path));
  auto owner = ui::scene(ui::label("ready"));

  owner.style(ui::file(source.path));
  auto result = owner.prepareChecked();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().file);
#ifdef _WIN32
  CHECK(result.error().file->operation == PssFileOperation::OPEN);
#else
  CHECK(result.error().file->operation == PssFileOperation::READ);
#endif
  CHECK(&result.error().file->cause.category() == &std::generic_category());
  CHECK(result.error().file->cause.value() != 0);
}

TEST_CASE("stage rvalue transfers preserve reusable owning identities") {
  auto original = ui::style("* { bold: true; }");
  auto transferred = std::move(original);
  auto owner = ui::scene(ui::label("ready"));

  owner | original | transferred;
  REQUIRE(owner.prepareChecked());
  auto sources = owner.managed()->get().styleSources();
  REQUIRE(sources);
  CHECK(sources->size() == 3);

  auto live = ui::liveTheme(LIVE);
  auto other = std::move(live);

  owner | live | other;
  CHECK(owner.checked());
  CHECK(live.get<"bold">());
  CHECK(other.get<"bold">());
}

TEST_CASE("error stage labels remain bounded valid UTF8 for oversized and invalid source identities") {
  for (unsigned variant = 0; variant != 3; ++variant) {
    const auto source = variant == 0   ? std::string(MAX_PSS_SOURCE_NAME_BYTES - 2, 'a') + "\xf0\x9f\x98\x80"
                        : variant == 1 ? std::string("bad-\xff", 5)
                                       : std::string("bad-\0tail", 9);
    const auto stage = ui::style("* { bold: true; }", {.sourceName = source});
    auto owner = ui::scene(ui::label("ready"));

    owner | stage;
    auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().pss);
    CHECK(result.error().pss->code == (variant == 0   ? PssErrorCode::SOURCE_NAME_LIMIT
                                       : variant == 1 ? PssErrorCode::INVALID_UTF8
                                                      : PssErrorCode::NUL_BYTE));
    REQUIRE(result.error().styleSource);
    CHECK(result.error().styleSource->size() <= MAX_PSS_SOURCE_NAME_BYTES);
    CHECK(parsePssDocument("", *result.error().styleSource));

    if (variant == 0) {
      CHECK(result.error().styleSource->size() == MAX_PSS_SOURCE_NAME_BYTES - 2);
    } else {
      CHECK(result.error().styleSource->empty());
    }
  }
}

TEST_CASE("temporary named replacements release retired style documents before scene cleanup") {
  constexpr std::size_t LARGE_SELECTOR_BYTES = 16 * 1024;
  constexpr unsigned REPLACEMENTS = 32;
  const std::string bytes = "." + std::string(LARGE_SELECTOR_BYTES, 'a') + " { bold: true; }";
  auto document = parsePssDocument(bytes, "retention.pss");
  auto owner = ui::scene(ui::label("ready"));
  const ui::StyleOptions options{.name = "same"};

  REQUIRE(document);
  REQUIRE(owner.prepareChecked());

  fault::RetentionScope tracking(LARGE_SELECTOR_BYTES);

  owner.style(ui::style(*document, options));
  REQUIRE(owner.checked());

  const auto firstPayloadBytes = fault::retainedBytes;

  REQUIRE(firstPayloadBytes >= LARGE_SELECTOR_BYTES);

  for (unsigned replacement = 0; replacement != REPLACEMENTS; ++replacement) {
    owner.style(ui::style(*document, options));
    REQUIRE(owner.checked());
  }

  const auto finalPayloadBytes = fault::retainedBytes;

  INFO("first payload bytes = ", firstPayloadBytes, ", final payload bytes = ", finalPayloadBytes);
  CHECK_FALSE(fault::overflow);
  CHECK(finalPayloadBytes == firstPayloadBytes);

  auto managed = owner.managed();
  auto sources = managed->get().styleSources();

  REQUIRE(sources);
  CHECK(sources->size() == 3);
}

TEST_CASE("removed styles retain payload only while a caller owns the historical stage") {
  constexpr std::size_t LARGE_SELECTOR_BYTES = 16 * 1024;
  const std::string bytes = "." + std::string(LARGE_SELECTOR_BYTES, 'a') + " { bold: true; }";
  auto document = parsePssDocument(bytes, "removed.pss");
  auto owner = ui::scene(ui::label("ready"));

  REQUIRE(document);
  REQUIRE(owner.prepareChecked());

  fault::RetentionScope tracking(LARGE_SELECTOR_BYTES);
  std::optional<ui::StyleStage> retained(ui::style(*document));
  const auto callerPayloadBytes = fault::retainedBytes;

  REQUIRE(callerPayloadBytes >= LARGE_SELECTOR_BYTES);
  owner.style(*retained);
  REQUIRE(owner.checked());

  auto managed = owner.managed();
  StyleLayerHandle handle;

  {
    auto sources = managed->get().styleSources();

    REQUIRE(sources);
    handle = sources->back().handle;
  }

  REQUIRE(managed->get().removeLayer(handle));
  CHECK(fault::retainedBytes == callerPayloadBytes);
  retained.reset();
  CHECK_FALSE(fault::overflow);
  CHECK(fault::retainedBytes == 0);
  CHECK(owner.checked());
}

TEST_CASE("temporary removed stages keep scene registry allocation bounded") {
  constexpr unsigned REMOVAL_CYCLES = 256;
  constexpr std::size_t REGISTRY_ALLOCATION_THRESHOLD = 1024;
  auto owner = ui::scene(ui::label("ready"));

  REQUIRE(owner.prepareChecked());

  auto managed = owner.managed();

  REQUIRE(managed);

  // Warm up layer storage before measuring repeated admission and removal.
  owner.style(ui::style("* { bold: true; }"));

  {
    auto sources = managed->get().styleSources();

    REQUIRE(sources);
    REQUIRE(managed->get().removeLayer(sources->back().handle));
  }

  fault::RetentionScope tracking(REGISTRY_ALLOCATION_THRESHOLD);

  for (unsigned cycle = 0; cycle != REMOVAL_CYCLES; ++cycle) {
    owner.style(ui::style("* { bold: true; }"));
    REQUIRE(owner.checked());

    auto sources = managed->get().styleSources();

    REQUIRE(sources);
    REQUIRE(sources->size() == 3);
    REQUIRE(managed->get().removeLayer(sources->back().handle));
  }

  INFO(fault::retainedBytes);
  CHECK_FALSE(fault::overflow);
  CHECK(fault::retainedBytes == 0);
}

TEST_CASE("retained named stage reuses layer order and remains stale after removal") {
  const auto stage = ui::style("* { bold: true; }", {.name = "same"});
  auto owner = ui::scene(ui::label("ready"));

  owner | stage;
  REQUIRE(owner.prepareChecked());

  auto managed = owner.managed();

  owner.style(ui::style("* { underline: true; }", {.name = "trailing"}));

  auto before = managed->get().styleSources();

  REQUIRE(before);

  const auto handle = (*before)[2].handle;

  owner.style(ui::style("* { italic: true; }", {.name = "same"}));
  owner | stage;
  REQUIRE(owner.checked());

  auto after = managed->get().styleSources();

  REQUIRE(after);
  REQUIRE(after->size() == 4);
  CHECK((*after)[2].name == "same");
  CHECK(after->back().name == "trailing");
  CHECK(managed->get().validateStyleLayer(handle));
  REQUIRE((*after)[2].document);
  CHECK((*after)[2].document->sheet.rules[0].declarations.bold == true);
  REQUIRE(managed->get().removeLayer(handle));

  SUBCASE("removed named stage cannot reinstall its layer") {
  }

  SUBCASE("fresh same-name layer cannot revive the historical stage handle") {
    owner.style(ui::style("* { italic: true; }", {.name = "same"}));
    REQUIRE(owner.checked());
  }

  owner | stage;

  REQUIRE_FALSE(owner.checked());
  CHECK(std::holds_alternative<StyleError>(owner.checked().error().cause));
}

TEST_CASE("scoped layout admission keeps metadata and layer atomic under allocation failures") {
  unsigned failures{}, successes{};

  for (std::ptrdiff_t limit = 0; limit != 128; ++limit) {
    ui::Ref<Column> group;
    auto owner = ui::scene(ui::vbox(group, ui::label("child")));
    auto document = parsePssDocument("* { bold: true; }", "scoped-oom.pss");

    REQUIRE(document);
    REQUIRE(owner.prepareChecked());

    document->sheet.scope = group.node().id();

    auto managed = owner.managed();

    REQUIRE(managed);

    fault::remaining = limit;

    auto installed = managed->get().styleDocument(StyleTier::APPLICATION, "scoped", *document);

    fault::remaining = -1;

    if (installed) {
      ++successes;
    } else {
      ++failures;
      CHECK(treeCause(installed.error(), TreeError::NO_MEMORY));
    }

    bool hasStyle{};
    bool bold{};

    owner.configure([&](ManagedScene &, Application &application) {
      REQUIRE(application.tree().read(group.node(), [&](const Widget &widget) {
        hasStyle = static_cast<bool>(widget.computedStyle());
        bold = widget.computedStyle() && widget.computedStyle()->normal.bold;
      }));
    });

    CHECK(hasStyle == installed.has_value());
    CHECK(bold == installed.has_value());
    REQUIRE(owner.checked());
  }

  CHECK(failures > 0);
  CHECK(successes > 0);
}

namespace fault {
thread_local unsigned lookupCountdown{};

void lookupAllocationHook() {
  if (--lookupCountdown) {
    hook = lookupAllocationHook;
  } else {
    reentry();
  }
}
} // namespace fault

TEST_CASE("lookup queue rejects allocator reentry before vector growth and restores its caller") {
  unsigned blocked{}, entered{};

  for (unsigned allocation = 1; allocation != 16; ++allocation) {
    auto owner = ui::scene(ui::button("A").id("a"));

    fault::reentry = [&] {
      ++entered;

      auto nested = owner.get<Button>("a");
      auto read = nested.text().get();

      if (!read && treeCause(read.error(), TreeError::WOULD_BLOCK)) {
        ++blocked;
      }
    };
    fault::lookupCountdown = allocation;
    fault::hook = fault::lookupAllocationHook;

    auto reference = owner.get<Button>("a");

    fault::hook = nullptr;
    fault::reentry = {};

    REQUIRE(owner.checked());
    REQUIRE(owner.prepareChecked());
    auto text = reference.text().get();

    REQUIRE(text);
    CHECK(*text == "A");
  }

  CHECK(entered > 0);
  CHECK(blocked > 0);
}

TEST_CASE("reusing an equal exact-node style requests no defaults work or idle redraw") {
  example::ReplayTerminal terminal({{20, "\x1b[21~", {}}}, 80, {20, 4});
  ui::SceneOptions options;
  ui::Ref<Defaults> reference;
  int defaults{}, reuses{};

  options.clock = terminal.clock();

  auto owner = ui::scene(ui::custom<Defaults>(std::ref(defaults)).reference(reference), options);
  const auto scoped = ui::style(reference, "* { bold: true; }");

  owner | scoped;
  owner.onKey(key::F10, [&] {
    const auto before = defaults;

    owner | scoped;

    CHECK(defaults == before);
    ++reuses;
  });

  auto result = owner.run(terminal);

  REQUIRE(result);
  CHECK(reuses == 1);
  CHECK(result->frames == 1);
  CHECK(terminal.closed);
}

static_assert(std::is_copy_constructible_v<ui::detail::LookupSource>);
static_assert(!std::is_nothrow_move_constructible_v<ui::detail::LookupSource>);

TEST_CASE("named lookup carriers copy under allocation failure without noexcept termination") {
  unsigned caught{}, rejected{}, completed{};
  const std::string id(80, 'a');

  for (const unsigned mode : {0u, 1u, 2u}) {
    for (std::ptrdiff_t limit = 0; limit != 64; ++limit) {
      auto owner = ui::scene(ui::button("A").id(id));
      auto original = owner.get(id);
      ui::Ref<Button> observed;
      bool obtained{};

      try {
        fault::remaining = limit;

        if (mode == 0) {
          observed = owner.get<Button>(id);
        } else if (mode == 1) {
          auto dynamic = owner.get(id);

          fault::remaining = -1;
          observed = dynamic;
        } else {
          observed = original;
        }

        fault::remaining = -1;
        obtained = true;
      } catch (const std::bad_alloc &) {
        fault::remaining = -1;
        ++caught;
      }

      auto status = owner.checked();

      if (!status) {
        ++rejected;
        CHECK(treeCause(status.error(), TreeError::NO_MEMORY));
      } else {
        REQUIRE(owner.prepareChecked());

        if (obtained) {
          auto text = observed.text().get();

          REQUIRE(text);
          CHECK(*text == "A");
          ++completed;
        }
      }
    }
  }

  CHECK(caught > 0);
  CHECK(rejected > 0);
  CHECK(completed > 0);
}

TEST_CASE("upper runner reports named lookup allocation failures once after cleanup before terminal") {
  struct ResetFault {
    ~ResetFault() {
      fault::remaining = -1;
    }
  };

  unsigned failures{}, successes{};
  const std::string id(80, 'a');

  for (std::ptrdiff_t limit = 0; limit != 64; ++limit) {
    example::ReplayTerminal terminal({}, 5, {20, 4});
    ui::RunAppOptions options;
    std::optional<ManagedError> failure;
    unsigned reports{}, cleanup{};

    options.terminal = &terminal;
    options.scene.clock = terminal.clock();
    options.report = [&](const ui::SceneReport &report) {
      ++reports;
      failure = report.failure;
      CHECK(cleanup == 1);
    };

    const auto result = ui::runApp(
      [&] {
        return ui::button("A").id(id);
      },
      [&](ui::Scene &owner) {
        owner.onStop([&] {
          ++cleanup;
        });

        ResetFault reset;

        fault::remaining = limit;

        auto reference = owner.get<Button>(id);
      },
      options);

    REQUIRE(cleanup == 1);

    if (result) {
      ++failures;
      REQUIRE(failure);
      CHECK(treeCause(*failure, TreeError::NO_MEMORY));
      CHECK(failure->phase == ManagedPhase::BIND);
      CHECK(reports == 1);
      CHECK(terminal.output.empty());
      CHECK_FALSE(terminal.closed);
    } else {
      ++successes;
      CHECK_FALSE(failure);
      CHECK(reports == 0);
      CHECK(terminal.closed);
    }
  }

  CHECK(failures > 0);
  CHECK(successes > 0);
}

TEST_CASE("frame PSS string admission reaches native preparation rendering and terminal cleanup") {
  constexpr char SOURCE[] = R"pss(ScrollArea {
    frame-line-foreground:#010203; frame-line-background:#040506;
    frame-line-bold:true; frame-line-italic:false; frame-line-underline:true;
    frame-title-foreground:#070809; frame-title-background:#0a0b0c;
    frame-title-bold:false; frame-title-italic:true; frame-title-underline:false;
    frame-ends-foreground:#0d0e0f; frame-ends-background:#101112;
    frame-ends-bold:true; frame-ends-italic:true; frame-ends-underline:false;
  })pss";
  example::ReplayTerminal terminal({{20, "\x1b[21~", {}}}, 80, {20, 6});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<ScrollArea> area;
  auto owner = ui::scene(ui::vbox(ui::scrollArea(ui::label("body"))
                                    .reference(area)
                                    .framed("ascii", "Title")
                                    .titleDecoration("square")
                                    .width(16)
                                    .height(4)),
                         options);
  owner | ui::style(SOURCE);
  REQUIRE(owner.prepareChecked());
  const auto sources = owner.managed()->get().styleSources();
  REQUIRE(sources);
  REQUIRE(sources->back().document);
  CHECK(sources->back().document->declarations.size() == 15);
  bool inspected{};
  owner.onKey(key::F10, [&] {
    auto &managed = owner.managed()->get();
    auto &app = ui::detail::SceneAccess::application(managed);
    REQUIRE(app.tree().read(area.node(), [&](const ScrollArea &widget) {
      REQUIRE(widget.computedStyle());
      const auto &parts = widget.computedStyle()->frame()[0];
      REQUIRE(parts.line);
      REQUIRE(parts.title);
      REQUIRE(parts.ends);
      CHECK(parts.line->foreground == RgbColor{1, 2, 3});
      CHECK(parts.line->background == RgbColor{4, 5, 6});
      CHECK(parts.title->foreground == RgbColor{7, 8, 9});
      CHECK(parts.title->background == RgbColor{10, 11, 12});
      CHECK(parts.ends->foreground == RgbColor{13, 14, 15});
      CHECK(parts.ends->background == RgbColor{16, 17, 18});
      CHECK_FALSE(parts.title->bold);
      CHECK_FALSE(parts.ends->underline);
      inspected = true;
    }));
    CHECK(owner.ref().quit());
  });
  const auto result = owner.run(terminal);
  REQUIRE(result);
  CHECK(inspected);
  CHECK(result->frames == 1);
  CHECK(terminal.closed);
}

TEST_CASE("frame document and compact sentinel properties share managed admission") {
  constexpr auto COMPACT_FRAME = sc::compactSheet(std::array{
    sc::flatRule(sc::type<"ScrollArea">(), sc::frameTitleBackground(sc::rgb(1, 2, 3)), sc::frameEndsUnderline(false))});
  const auto document = parsePssDocument("ScrollArea{frame-title-background:#010203;frame-ends-underline:false;}");
  REQUIRE(document);

  for (const bool compact : {false, true}) {
    auto owner = ui::scene(ui::scrollArea(ui::label("body")).framed("ascii", "Title"));

    if (compact) {
      owner | ui::style(COMPACT_FRAME);
    } else {
      owner | ui::style(*document);
    }

    REQUIRE(owner.prepareChecked());
    const auto sources = owner.managed()->get().styleSources();
    REQUIRE(sources);
    REQUIRE(sources->back().document);
    CHECK(sources->back().document->sheet == document->sheet);
    REQUIRE(owner.close());
  }
}

TEST_CASE("frame property range and noncanonical RGB boolean payloads remain typed admission errors") {
  for (unsigned forgery = 0; forgery < 3; ++forgery) {
    auto document = parsePssDocument("ScrollArea{frame-title-background:#000000;frame-ends-underline:false;}");
    REQUIRE(document);

    if (forgery == 0) {
      document->declarations[0].property = static_cast<PssProperty>(PSS_PROPERTY_COUNT);
    } else if (forgery == 1) {
      document->declarations[0].value.boolean = true;
    } else {
      document->declarations[1].value.color = {1, 2, 3};
    }

    auto owner = ui::scene(ui::label("body"));
    owner | ui::style(*document);
    const auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().pss);
    CHECK(result.error().pss->code == PssErrorCode::INVALID_VALUE);
    CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  }
}

TEST_CASE("scrollbar PSS all parts reach real preparation run rendering and terminal cleanup") {
  constexpr char SOURCE[] = R"pss(ScrollArea {
    scrollbar-track-foreground:#010203; scrollbar-track-background:#040506;
    scrollbar-track-bold:true; scrollbar-track-italic:false; scrollbar-track-underline:true;
    scrollbar-thumb-foreground:#070809; scrollbar-thumb-background:#0a0b0c;
    scrollbar-thumb-bold:false; scrollbar-thumb-italic:true; scrollbar-thumb-underline:false;
    scrollbar-thumb-start-foreground:#313233; scrollbar-thumb-body-italic:false;
    scrollbar-thumb-end-background:#343536; scrollbar-thumb-single-bold:true;
    scrollbar-ends-foreground:#0d0e0f; scrollbar-ends-background:#101112;
    scrollbar-ends-bold:true; scrollbar-ends-italic:true; scrollbar-ends-underline:false;
    scrollbar-arrows-foreground:#232425; scrollbar-arrows-background:#262728;
    scrollbar-arrows-bold:false; scrollbar-arrows-italic:true; scrollbar-arrows-underline:true;
    scrollbar-corner-foreground:#131415; scrollbar-corner-background:#161718;
    scrollbar-corner-bold:false; scrollbar-corner-italic:false; scrollbar-corner-underline:true;
  } ScrollArea:disabled {scrollbar-thumb-foreground:#202122;scrollbar-thumb-single-foreground:#373839;}
    ScrollArea:focused {scrollbar-track-bold:false;scrollbar-thumb-start-bold:false;})pss";
  example::ReplayTerminal terminal({{20, "\x1b[21~", {}}}, 80, {20, 6});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<ScrollArea> area;
  auto owner = ui::scene(ui::vbox(ui::scrollArea(ui::label("body").width(40).height(20))
                                    .reference(area)
                                    .framed("double", "Title")
                                    .scrollOnFrame()
                                    .arrows()
                                    .scrollbar("frame")
                                    .width(16)
                                    .height(4)),
                         options);
  owner | ui::style(SOURCE);
  REQUIRE(owner.prepareChecked());
  const auto sources = owner.managed()->get().styleSources();
  REQUIRE(sources);
  REQUIRE(sources->back().document);
  CHECK(sources->back().document->declarations.size() == 33);
  bool inspected{};
  owner.onKey(key::F10, [&] {
    auto &managed = owner.managed()->get();
    auto &app = ui::detail::SceneAccess::application(managed);
    REQUIRE(app.tree().read(area.node(), [&](const ScrollArea &widget) {
      REQUIRE(widget.computedStyle());
      const auto &parts = widget.computedStyle()->scrollbar()[0];
      REQUIRE(parts.track);
      REQUIRE(parts.thumb);
      REQUIRE(parts.ends);
      REQUIRE(parts.corner);
      REQUIRE(parts.arrows);
      CHECK(parts.arrows->foreground == RgbColor{35, 36, 37});
      CHECK(parts.arrows->background == RgbColor{38, 39, 40});
      CHECK(parts.track->foreground == RgbColor{1, 2, 3});
      CHECK(parts.track->background == RgbColor{4, 5, 6});
      CHECK(parts.thumb->foreground == RgbColor{7, 8, 9});
      CHECK(parts.thumb->background == RgbColor{10, 11, 12});
      CHECK(parts.ends->foreground == RgbColor{13, 14, 15});
      CHECK(parts.ends->background == RgbColor{16, 17, 18});
      CHECK(parts.corner->foreground == RgbColor{19, 20, 21});
      CHECK(parts.corner->background == RgbColor{22, 23, 24});
      CHECK_FALSE(parts.thumb->bold);
      CHECK_FALSE(parts.ends->underline);
      CHECK_FALSE(widget.computedStyle()->scrollbar()[1].track->bold);
      CHECK(widget.computedStyle()->scrollbar()[2].thumb->foreground == RgbColor{32, 33, 34});
      REQUIRE(widget.computedStyle()->scrollbarThumbSegments());
      const auto &segments = *widget.computedStyle()->scrollbarThumbSegments();
      CHECK(segments[0].start.foreground == RgbColor{49, 50, 51});
      CHECK_FALSE(segments[0].body.italic);
      CHECK(segments[0].end.background == RgbColor{52, 53, 54});
      CHECK(segments[0].single.bold);
      CHECK_FALSE(segments[1].start.bold);
      CHECK(segments[2].body.foreground == RgbColor{32, 33, 34});
      CHECK(segments[2].single.foreground == RgbColor{55, 56, 57});
      inspected = true;
    }));
    CHECK(owner.ref().quit());
  });
  const auto result = owner.run(terminal);
  REQUIRE(result);
  CHECK(inspected);
  CHECK(result->frames == 1);
  CHECK(terminal.closed);
  CHECK(terminal.output.find("38;2;1;2;3") != std::string::npos);
}

TEST_CASE("scrollbar document compact and typed sheets retain the appended sentinels") {
  constexpr auto SCROLLBAR_COMPACT = sc::compactSheet(std::array{sc::flatRule(
    sc::type<"ScrollArea">(), sc::scrollbarTrackForeground(sc::rgb(1, 2, 3)), sc::scrollbarCornerUnderline(false),
    sc::scrollbarArrowsForeground(sc::rgb(3, 4, 5)), sc::scrollbarArrowsItalic(true))});
  constexpr auto TYPED = sc::sheet(
    sc::parameters(), sc::rule(sc::type<"ScrollArea">(), sc::scrollbarTrackForeground(sc::rgb(1, 2, 3)),
                               sc::scrollbarCornerUnderline(false), sc::scrollbarArrowsForeground(sc::rgb(3, 4, 5)),
                               sc::scrollbarArrowsItalic(true)));
  const auto document = parsePssDocument("ScrollArea{scrollbar-track-foreground:#010203;scrollbar-corner-underline:"
                                         "false;scrollbar-arrows-foreground:#030405;scrollbar-arrows-italic:true;}");
  REQUIRE(document);
  CHECK(sc::materialize(TYPED) == document->sheet);
  for (unsigned mode : {0u, 1u}) {
    auto owner = ui::scene(ui::scrollArea(ui::label("body")).scrollbar("ascii"));
    if (mode == 0) {
      owner | ui::style(SCROLLBAR_COMPACT);
    } else {
      owner | ui::style(*document);
    }
    REQUIRE(owner.prepareChecked());
    const auto sources = owner.managed()->get().styleSources();
    REQUIRE(sources);
    REQUIRE(sources->back().document);
    CHECK(sources->back().document->sheet == document->sheet);
    REQUIRE(owner.close());
  }
  for (unsigned forgery : {0u, 1u, 2u}) {
    auto forged = *document;
    if (forgery == 0)
      forged.declarations[0].property = static_cast<PssProperty>(PSS_PROPERTY_COUNT);
    else if (forgery == 1)
      forged.declarations[0].value.boolean = true;
    else
      forged.declarations[1].value.color = {1, 2, 3};
    auto owner = ui::scene(ui::label("body"));
    owner | ui::style(forged);
    const auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  }
}

TEST_CASE("scrollbar pattern PSS runtime static typed and compact documents share enum scalars") {
  namespace sc = portals::style::ct;
  constexpr auto LITERAL =
    sc::pss("ScrollBar{scrollbar-track-pattern:minimal;scrollbar-thumb-pattern:block;scrollbar-arrow-pattern:ascii;}");
  constexpr auto TYPED =
    sc::sheet(sc::parameters(), sc::rule(sc::type<"ScrollBar">(), sc::scrollbarTrackPattern(ScrollbarPattern::MINIMAL),
                                         sc::scrollbarThumbPattern(ScrollbarThumbPattern::BLOCK),
                                         sc::scrollbarArrowPattern(ScrollbarArrowPattern::ASCII)));
  constexpr auto PART_COMPACT = sc::compactSheet(std::array{sc::flatRule(
    sc::type<"ScrollBar">(), sc::scrollbarTrackPattern(ScrollbarPattern::MINIMAL),
    sc::scrollbarThumbPattern(ScrollbarThumbPattern::BLOCK), sc::scrollbarArrowPattern(ScrollbarArrowPattern::ASCII))});
  const auto parsed = parsePssDocument(
    "ScrollBar{scrollbar-track-pattern:minimal;scrollbar-thumb-pattern:block;scrollbar-arrow-pattern:ascii;}");
  REQUIRE(parsed);
  CHECK(sc::materialize(TYPED) == parsed->sheet);

  for (unsigned mode : {0u, 1u, 2u}) {
    auto owner = ui::scene(ui::scrollBar().scrollbar("ascii"));

    if (mode == 0) {
      owner | ui::style(LITERAL);
    } else if (mode == 1) {
      owner | ui::style(PART_COMPACT);
    } else {
      owner | ui::style(*parsed);
    }

    REQUIRE(owner.prepareChecked());
    const auto sources = owner.managed()->get().styleSources();
    REQUIRE(sources);
    CHECK(sources->back().document->sheet == parsed->sheet);
    REQUIRE(owner.close());
  }

  for (const auto source : {"*{scrollbar-track-pattern:block;}", "*{scrollbar-thumb-pattern:rounded;}",
                            "*{scrollbar-arrow-pattern:frame;}"}) {
    const auto result = parsePssDocument(source);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == PssErrorCode::INVALID_VALUE);
  }

  for (unsigned forgery : {0u, 1u, 2u}) {
    auto forged = *parsed;

    if (forgery == 0) {
      forged.declarations[0].value.pattern = 99;
    } else if (forgery == 1) {
      forged.declarations[0].value.boolean = true;
    } else {
      forged.declarations[1].value.width = 1;
    }

    auto owner = ui::scene(ui::label("body"));
    owner | ui::style(forged);
    const auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  }
}

TEST_CASE("runtime literal PSS documents retain ownership and reject forged glyph metadata") {
  const auto parsed = parsePssDocument(
    "ScrollBar{scrollbar-vertical-track-glyph:'.';scrollbar-vertical-thumb-single-glyph:'Q';}", "literal.pss");
  REQUIRE(parsed);
  auto owner = ui::scene(ui::scrollBar());
  owner | ui::style(*parsed);
  REQUIRE(owner.prepareChecked());
  const auto sources = owner.managed()->get().styleSources();
  REQUIRE(sources);
  REQUIRE(sources->back().document);
  CHECK(sources->back().document->sheet == parsed->sheet);
  REQUIRE(owner.close());
  for (unsigned forgery : {0u, 1u, 2u}) {
    auto forged = *parsed;
    if (forgery == 0) {
      forged.declarations[0].glyph.reset();
    } else if (forgery == 1) {
      forged.declarations[0].value.boolean = true;
    } else {
      auto leaves = std::make_shared<detail::ScrollbarGlyphRefs>(*forged.sheet.rules[0].scrollbarGlyphs);
      (*leaves)[10] = (*leaves)[19];
      forged.sheet.rules[0].scrollbarGlyphs = std::move(leaves);
    }
    auto invalid = ui::scene(ui::scrollBar()) | ui::style(forged);
    const auto result = invalid.prepareChecked();
    REQUIRE_FALSE(result);
    CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  }
}

TEST_CASE("literal width preparation keeps source identity in the managed style error") {
  auto owner = ui::scene(ui::scrollBar()) |
               ui::style("ScrollBar{scrollbar-vertical-track-glyph:'界';}", {.sourceName = "wide-literal.pss"});
  const auto result = owner.prepareChecked();
  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  REQUIRE(result.error().styleSource);
  CHECK(*result.error().styleSource == "wide-literal.pss");
  const auto *failure = std::get_if<StyleError>(&result.error().cause);
  REQUIRE(failure);
  CHECK(failure->propertyName() == "scrollbar-vertical-track-glyph");
  CHECK(failure->layerId != 0);
}

TEST_CASE("managed literal errors share full source ownership and keep nothrow copies after document destruction") {
  static_assert(std::is_trivially_copy_constructible_v<StyleError>);
  static_assert(std::is_nothrow_copy_constructible_v<ManagedError>);
  const std::string expected = "owned-" + std::string(512, 'a') + "Ж.pss";
  ManagedError retained;
  std::weak_ptr<const std::string> sourceOwner;
  {
    const auto document = parsePssDocument("ScrollBar{scrollbar-vertical-track-glyph:'界';}", expected);
    REQUIRE(document);
    auto owner = ui::scene(ui::scrollBar()) | ui::style(*document);
    const auto rejected = owner.prepareChecked();
    REQUIRE_FALSE(rejected);
    retained = rejected.error();
    sourceOwner = retained.styleSource;
  }
  REQUIRE_FALSE(sourceOwner.expired());
  {
    fault::remaining = 0;
    const ManagedError copied(retained);
    fault::remaining = -1;
    retained.styleSource.reset();
    REQUIRE(copied.styleSource);
    CHECK(*copied.styleSource == expected);
    CHECK_FALSE(sourceOwner.expired());
    const auto *failure = std::get_if<StyleError>(&copied.cause);
    REQUIRE(failure);
    CHECK(failure->propertyName() == "scrollbar-vertical-track-glyph");
    CHECK(failure->line == 1);
    CHECK(failure->layerId != 0);
  }
  CHECK(sourceOwner.expired());
}
