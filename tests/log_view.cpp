// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/log_view_state.hpp"
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <thread>

namespace allocation_test {
thread_local bool armed{};
thread_local std::size_t count{};
thread_local std::ptrdiff_t remaining{-1};
thread_local void (*hook)(){};
thread_local std::size_t hookBytes{};
} // namespace allocation_test

void *operator new(std::size_t bytes) {
  if (allocation_test::armed) {
    ++allocation_test::count;

    if (allocation_test::remaining == 0) {
      allocation_test::armed = false;

      throw std::bad_alloc();
    }

    if (allocation_test::remaining > 0) {
      --allocation_test::remaining;
    }
  }

  if (allocation_test::hook && (!allocation_test::hookBytes || bytes == allocation_test::hookBytes)) {
    std::exchange(allocation_test::hook, nullptr)();
  }

  if (auto *memory = std::malloc(bytes ? bytes : 1)) {
    return memory;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t bytes) {
  return ::operator new(bytes);
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

void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(bytes);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept {
  return ::operator new(bytes, std::nothrow);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

namespace {
using namespace portals;
int failures{};
std::size_t checks{};

void check(bool value, const char *message) {
  ++checks;

  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  if (!result) {
    std::cerr << "unexpected checked failure at " << origin.line() << '\n';
    std::exit(2);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

std::size_t prepareManaged(ManagedScene &scene, Application &app, CellSize size) {
  take(scene.prepare(app, size));

  for (std::size_t pass = 1; pass <= ApplicationOptions{}.maxPreparationPasses; ++pass) {
    std::optional<LayoutSnapshot> snapshot;
    const auto rectangle = take(CellRect::from({}, size));

    take(app.tree().frame([&] {
      take(layoutTree(app.tree(), app.root(), rectangle, rectangle));
      snapshot.emplace(take(LayoutSnapshot::capture(app.tree(), app.root(), LayoutSnapshotProfile::GEOMETRY)));
    }));
    take(app.tree().consumeFrameRequest());
    take(scene.prepareViews(app, *snapshot));

    if (take(app.tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
      take(scene.completedLayout(app));

      std::cout << "prepared " << size.width << 'x' << size.height << " passes=" << pass << '\n';

      return pass;
    }
  }

  check(false, "default three-pass preparation must converge");

  return 4;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  Node<LogView> log;
  std::shared_ptr<ScrollController> controller;

  explicit Fixture(LogViewConfig config = {}, std::optional<SizePolicy> policy = {}, WidthPolicy width = {}) {
    const auto column = take(scene->emplace<Column>(scene->root()));

    log = policy ? take(scene->emplace<LogView>(column, width, config, ControlStyle{}, *policy))
                 : take(scene->emplace<LogView>(column, width, config));
    take(app->tree().read(log, [&](const LogView &widget) {
      controller = detail::ScrollWidgetAccess::state(widget).controller;
    }));
  }

  std::size_t prepare(CellSize size) {
    return prepareManaged(*scene, *app, size);
  }

  template <class Read> void read(Read function) {
    take(app->tree().read(log, [&](const LogView &widget) {
      function(*detail::LogViewAccess::state(widget).data);
    }));
  }
};

struct PaintSink : PaintContext {
  std::vector<RenderCommand> commands = std::vector<RenderCommand>(std::initializer_list<RenderCommand>{});

  PaintSink() {
    commands.reserve(2048);
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    if (values.size() > commands.capacity() - commands.size()) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    for (const auto &value : values) {
      commands.push_back(value);
    }

    return {};
  }
};

void convergenceAndAnchor() {
  Fixture fixture({.limits = {.maxLines = 6, .maxBytes = 128, .maxBatchBytes = 128, .maxRecordBytes = 64}});

  take(appendLogBatch(fixture.app->tree(), fixture.log, "abcdefghij\nsecond\nthird\nfourth\nfifth\nsixth"));
  check(fixture.prepare({8, 6}) <= 3, "initial wrap metrics settle within default budget");
  check(take(fixture.controller->snapshot()).vertical.offset.raster > 0, "initial following reaches tail");
  take(fixture.scene->setScrollOffset(fixture.log, ScrollAxis::VERTICAL, {1, 0.25}));
  fixture.prepare({8, 6});
  fixture.read([&](const auto &data) {
    check(!data.status.followTail && data.anchor.record == 1 && data.anchor.byte == 6,
          "manual reading anchor stores source record and grapheme byte");
  });

  check(fixture.prepare({6, 6}) <= 3, "resize rewrap settles within default budget");
  check(take(fixture.controller->snapshot()).vertical.offset.raster == 1,
        "rewrap selects row containing source anchor");
  check(take(fixture.controller->snapshot()).vertical.offset.fraction == 0.25,
        "preparation preserves deliberate fractional scrolling");
  take(appendLogBatch(fixture.app->tree(), fixture.log, "seventh\neighth"));
  fixture.prepare({6, 6});
  fixture.read([&](const auto &data) {
    check(data.status.droppedLines == 2 && data.status.newWhilePaused == 2, "drop and paused-new counts are visible");
    check(data.anchor.record == 3 && data.anchor.byte == 0, "evicted anchor selects first surviving record");
  });

  take(fixture.scene->setScrollOffset(fixture.log, ScrollAxis::VERTICAL, {1, 0}));
  fixture.prepare({6, 6});
  check(take(logStatus(fixture.app->tree(), fixture.log)).newWhilePaused == 2,
        "further paused navigation does not erase accepted-new count");

  const auto anchorBeforeZero = [&] {
    std::uint64_t id{};
    fixture.read([&](const auto &data) {
      id = data.anchor.record;
    });

    return id;
  }();

  fixture.prepare({0, 0});
  fixture.prepare({6, 6});
  fixture.read([&](const auto &data) {
    check(!data.status.followTail && data.anchor.record == anchorBeforeZero, "zero reopening preserves logical anchor");
  });
  take(fixture.app->tree().setVisible(fixture.log, false));
  fixture.prepare({6, 6});
  take(appendLogBatch(fixture.app->tree(), fixture.log, "ninth"));
  take(fixture.app->tree().setVisible(fixture.log, true));
  fixture.prepare({6, 6});
  take(followLogTail(fixture.app->tree(), fixture.log));
  fixture.prepare({6, 6});
  check(take(logStatus(fixture.app->tree(), fixture.log)).followTail, "explicit return resumes following");
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE, "settled log schedules no idle frame");
}

void admissionAndUnicode() {
  Fixture fixture({.limits = {.maxLines = 3, .maxBytes = 12, .maxBatchBytes = 64, .maxRecordBytes = 12}});
  const auto batch = take(appendLogBatch(fixture.app->tree(), fixture.log, "a\r\n\r\nb\rc\n"));

  check(batch.acceptedLines == 4 && batch.droppedLines == 1 && batch.retainedLines == 3 && batch.retainedBytes == 2,
        "CRLF empty records and final separator follow whole-batch contract");
  fixture.read([&](const auto &data) {
    check(data.records[0]->text.bytes().empty() && data.records[1]->text.bytes() == "b" &&
            data.records[2]->text.bytes() == "c",
          "drop-oldest preserves internal empty record");
  });

  const auto before = take(logStatus(fixture.app->tree(), fixture.log));
  const auto invalid = appendLogBatch(fixture.app->tree(), fixture.log, "good\n\xff");

  check(!invalid && std::get<TextError>(invalid.error()) == TextError{TextErrorKind::INVALID_UTF8, 5},
        "invalid late record preserves exact batch byte diagnostic");
  check(take(logStatus(fixture.app->tree(), fixture.log)).droppedLines == before.droppedLines,
        "failed batch preserves counters");
  fixture.read([&](const auto &data) {
    check(data.nextID == 5, "failed batch preserves next ID");
  });
  check(take(appendLogBatch(fixture.app->tree(), fixture.log, "")).acceptedLines == 0, "empty batch is no-op");
  take(clearLog(fixture.app->tree(), fixture.log));
  fixture.read([&](const auto &data) {
    check(data.nextID == 5 && data.records.empty(), "clear never reuses IDs");
  });

  Fixture unicode;

  take(appendLogBatch(unicode.app->tree(), unicode.log, "界a\ncafe\xcc\x81\n👩‍💻\n\x01"));
  unicode.prepare({3, 6});
  unicode.read([&](const auto &data) {
    check(data.rows.size() == 8, "one-cell viewport consumes wide combining and ZWJ clusters once");
    const auto bytes = [&](const auto &row) {
      return data.records[row.record]->text.bytes().substr(row.byteBegin, row.byteEnd - row.byteBegin);
    };

    check(bytes(data.rows.front()) == "界" && bytes(data.rows[6]) == "👩‍💻",
          "wrap endpoints preserve complete original graphemes");
    check(data.paint->lines.back().graphemes().front().display == TextDisplay::UNSUPPORTED,
          "controls retain placeholder disposition");
  });

  constexpr WidthPolicy WIDTH{AmbiguousWidth::WIDE, EmojiWidth::ONE, 7};
  Fixture customWidth({}, {}, WIDTH);

  take(appendLogBatch(customWidth.app->tree(), customWidth.log, "·👩‍💻e\xcc\x81\x01"));
  customWidth.prepare({4, 8});
  customWidth.read([&](const auto &data) {
    check(data.rows.size() == 3 && data.paint->lines.size() == 3,
          "custom ambiguous and emoji widths drive the full wrap index");

    for (std::size_t index = 0; index < data.paint->lines.size(); ++index) {
      const auto &row = data.rows[index];
      const auto &line = data.paint->lines[index];
      const auto source = data.records[row.record]->text.graphemes();

      check(line.policy() == WIDTH, "visible cache retains the complete caller width policy");

      for (const auto &cluster : line.graphemes()) {
        const auto original = std::ranges::find(source, row.byteBegin + cluster.byte_begin, &Grapheme::byte_begin);

        check(original != source.end() && original->byte_end == row.byteBegin + cluster.byte_end &&
                original->cells == cluster.cells && original->display == cluster.display,
              "visible resegmentation preserves original whole-grapheme endpoints and display metadata");
      }
    }
  });
}

void guardsAndBounds() {
  auto rawApp = take(Application::create());
  const auto raw = take(rawApp->tree().emplace<LogView>(rawApp->root()));

  take(initializeScrollArea(rawApp->tree(), Node<ScrollArea>(raw)));

  const auto unmanaged = appendLogBatch(rawApp->tree(), raw, "not initialized");

  check(!unmanaged && std::get<TreeError>(unmanaged.error()) == TreeError::INVALID_ARGUMENT,
        "raw tree emplacement and base lease do not silently create a managed log");

  Fixture fixture({.readOnly = true});
  const auto parent = take(fixture.app->tree().inspect(fixture.log)).parent;
  const auto beforeChildren = take(fixture.app->tree().inspect(parent)).children.size();
  const auto invalidCaps =
    fixture.scene->emplace<LogView>(parent, WidthPolicy{}, LogViewConfig{.limits = {.maxLines = 0}});
  FrameOptions missingBottom;

  missingBottom.sides.bottom = false;

  const auto invalidFrame = fixture.scene->emplace<LogView>(parent, WidthPolicy{}, LogViewConfig{}, ControlStyle{},
                                                            DEFAULT_LOG_VIEW_POLICY, std::move(missingBottom));

  check(!invalidCaps && std::get<TreeError>(invalidCaps.error().cause) == TreeError::INVALID_ARGUMENT &&
          !invalidFrame && std::get<TreeError>(invalidFrame.error().cause) == TreeError::INVALID_ARGUMENT &&
          take(fixture.app->tree().inspect(parent)).children.size() == beforeChildren,
        "native managed emplacement rejects invalid caps or missing footer side and rolls back attempted nodes");

  take(fixture.app->tree().setEnabled(fixture.log, false));
  take(appendLogBatch(fixture.app->tree(), fixture.log, "programmatic"));
  take(clearLog(fixture.app->tree(), fixture.log));
  take(followLogTail(fixture.app->tree(), fixture.log));

  std::optional<LogError> wrongThread;
  std::thread worker([&] {
    const auto result = appendLogBatch(fixture.app->tree(), fixture.log, "x");

    if (!result) {
      wrongThread = result.error();
    }
  });

  worker.join();
  check(wrongThread && std::get<TreeError>(*wrongThread) == TreeError::WRONG_THREAD,
        "thread admission precedes mutable log read");
  take(fixture.app->tree().frame([&] {
    const auto result = appendLogBatch(fixture.app->tree(), fixture.log, "x");

    check(!result && std::get<TreeError>(result.error()) == TreeError::WOULD_BLOCK,
          "frame rejects append before touching state");
  }));
  take(fixture.app->tree().edit(fixture.log, [&](LogView &widget) {
    auto &data = *detail::LogViewAccess::state(widget).data;

    data.nextID = std::numeric_limits<std::uint64_t>::max();
  }));
  const auto exhausted = appendLogBatch(fixture.app->tree(), fixture.log, "last\nreused");

  check(!exhausted && std::get<TreeError>(exhausted.error()) == TreeError::IDENTITY_EXHAUSTED,
        "ID exhaustion rejects entire batch without reuse");
  fixture.read([&](const auto &data) {
    check(data.records.empty() && data.nextID == std::numeric_limits<std::uint64_t>::max(),
          "ID failure preserves prior state");
  });
  take(appendLogBatch(fixture.app->tree(), fixture.log, "last"));
  take(clearLog(fixture.app->tree(), fixture.log));

  const auto exhaustedAfterClear = appendLogBatch(fixture.app->tree(), fixture.log, "reused");

  check(!exhaustedAfterClear && std::get<TreeError>(exhaustedAfterClear.error()) == TreeError::IDENTITY_EXHAUSTED,
        "clear preserves exhausted ID history");
  take(fixture.scene->remove(fixture.log));

  const auto retired = appendLogBatch(fixture.app->tree(), fixture.log, "late");

  check(!retired && std::get<TreeError>(retired.error()) == TreeError::STALE_NODE, "retired log rejects late append");
  check(!validateLogLimits({.maxLines = 8193}), "hard record ceiling is rejected");
  check(!validateLogLimits({.maxBytes = 8, .maxRecordBytes = 9}), "inconsistent byte cap is rejected");
}

void structuralBoundsAndSizing() {
  Fixture fixture({.limits = {.maxLines = 3, .maxBytes = 8, .maxBatchBytes = 64, .maxRecordBytes = 8}});
  const auto blanks = take(appendLogBatch(fixture.app->tree(), fixture.log, std::string(32, '\n')));

  check(blanks.acceptedLines == 32 && blanks.droppedLines == 29 && blanks.retainedLines == 3 &&
          blanks.retainedBytes == 0,
        "separator-heavy batch admits all records and keeps bounded latest empty records");
  fixture.prepare({12, 6});
  take(fixture.app->tree().read(fixture.log, [&](const LogView &widget) {
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);

    check(widget.sizePolicy().height.mode == SizeMode::FLEX && scroll.geometry.viewport.height > 0,
          "default native log receives positive flexible viewport");
  }));
  take(appendLogBatch(fixture.app->tree(), fixture.log, "ééé"));

  const auto eviction = take(appendLogBatch(fixture.app->tree(), fixture.log, "abc"));

  check(eviction.retainedLines == 1 && eviction.retainedBytes == 3 && eviction.droppedLines == 3,
        "byte cap evicts empty older records as well as payload records");
  fixture.prepare({12, 6});
  fixture.read([&](const auto &data) {
    std::size_t rowBytes{};

    for (const auto &line : data.paint->lines) {
      rowBytes += line.bytes().size();
    }

    check(data.records.capacity() <= 3 && data.rows.capacity() <= 11 && rowBytes <= 8 && data.paint->lines.size() <= 4,
          "retained index and visible owning cache respect structural bounds");
  });

  const auto tooLargeRecord = appendLogBatch(fixture.app->tree(), fixture.log, "123456789");
  const auto tooLargeBatch = appendLogBatch(fixture.app->tree(), fixture.log, std::string(65, '\n'));

  check(!tooLargeRecord && std::get<TextError>(tooLargeRecord.error()) == TextError{TextErrorKind::INPUT_LIMIT, 0} &&
          !tooLargeBatch && std::get<TextError>(tooLargeBatch.error()) == TextError{TextErrorKind::INPUT_LIMIT, 0},
        "record and batch limits reject without truncation");

  Fixture explicitPolicy({}, SizePolicy{});

  take(explicitPolicy.app->tree().read(explicitPolicy.log, [&](const LogView &widget) {
    check(widget.sizePolicy().height.mode == SizeMode::AUTOMATIC, "explicit intrinsic SizePolicy remains literal");
  }));

  ui::Ref<LogView> fixed;
  const auto mounted = take(ui::mount(*fixture.scene, ui::logView(fixed, "fixed").height(5)));

  take(fixture.app->tree().read(mounted, [&](const LogView &widget) {
    const auto height = widget.sizePolicy().height;

    check(height.mode == SizeMode::FIXED && height.preferred == 5,
          "explicit fluent fixed height overrides native default");
  }));

  Fixture saturated({.limits = {.maxLines = 1}, .followTail = false});

  take(saturated.app->tree().edit(saturated.log, [&](LogView &widget) {
    auto &status = detail::LogViewAccess::state(widget).data->status;

    status.droppedLines = std::numeric_limits<std::uint64_t>::max() - 1;
    status.newWhilePaused = std::numeric_limits<std::uint64_t>::max() - 1;
  }));
  take(appendLogBatch(saturated.app->tree(), saturated.log, "one\ntwo\nthree"));

  const auto status = take(logStatus(saturated.app->tree(), saturated.log));

  check(status.droppedSaturated && status.newWhilePausedSaturated &&
          status.droppedLines == std::numeric_limits<std::uint64_t>::max() &&
          status.newWhilePaused == std::numeric_limits<std::uint64_t>::max(),
        "cumulative counts saturate with explicit flags rather than wrap");
}

void narrowViewportStorage() {
  Fixture fixture;
  std::string batch(LogLimits{}.maxRecordBytes, 'x');

  batch += '\n';

  for (std::size_t record = 0; record < LogLimits{}.maxBytes / LogLimits{}.maxRecordBytes; ++record) {
    take(appendLogBatch(fixture.app->tree(), fixture.log, batch));
  }

  fixture.prepare({3, 12});
  fixture.read([&](const auto &data) {
    check(data.rows.size() == LogLimits{}.maxBytes && data.paint->lines.size() == 10,
          "one-cell default-cap document keeps full numeric index and only viewport-sized owning cache");
    check(data.paint->first + data.paint->lines.size() == data.rows.size(),
          "following prepares the actual final viewport before painting");
  });
  take(fixture.scene->setScrollOffset(fixture.log, ScrollAxis::VERTICAL, {128, 0}));
  check(fixture.prepare({3, 12}) <= 2, "scrolling refreshes the visible cache without a wrap/layout cycle");
  fixture.read([&](const auto &data) {
    check(data.paint->first == 128 && data.paint->lines.size() == 10,
          "manual navigation prepares only its new viewport");
  });
}

void paintAndFooterInput() {
  Fixture fixture({.followTail = false});

  take(appendLogBatch(fixture.app->tree(), fixture.log, "one\ntwo\nthree\nfour\nfive\nsix\n"));
  fixture.prepare({80, 6});

  PaintSink sink;

  take(fixture.app->tree().frame([&] {
    allocation_test::count = 0;
    allocation_test::armed = true;

    const auto painted = fixture.app->tree().paint(fixture.log, sink);

    allocation_test::armed = false;
    take(painted);
    check(allocation_test::count == 0, "prepared native paint allocates no storage");

    const auto rendered = take(renderFullFrame({80, 6}, sink.commands));

    check(take(rendered.at({1, 1})).glyph == "o" && take(rendered.at({1, 5})).glyph == "R",
          "actual renderer shows first paused record and visible return-to-tail footer");
  }));

  const auto clicked =
    take(fixture.app->router().dispatch(MouseInputEvent{2, 5, MouseButton::LEFT, MouseAction::PRESS, {}}));

  check(clicked.status == DispatchStatus::HANDLED && take(logStatus(fixture.app->tree(), fixture.log)).followTail,
        "visible footer action uses existing input route to resume following");
  fixture.prepare({80, 6});

  Fixture readOnly({.followTail = false, .readOnly = true});

  take(appendLogBatch(readOnly.app->tree(), readOnly.log, "one\ntwo\nthree\nfour\nfive\nsix"));
  readOnly.prepare({80, 6});
  take(readOnly.app->router().dispatch(MouseInputEvent{2, 5, MouseButton::LEFT, MouseAction::PRESS, {}}));
  check(!take(logStatus(readOnly.app->tree(), readOnly.log)).followTail, "readOnly rejects footer input");
  take(readOnly.app->tree().setEnabled(readOnly.log, false));
  take(readOnly.app->router().dispatch(KeyEvent{InputKey::END}));
  check(!take(logStatus(readOnly.app->tree(), readOnly.log)).followTail, "disabled subtree preserves paused state");

  take(fixture.app->tree().setVisible(fixture.log, false));
  fixture.prepare({80, 6});
  sink.commands.clear();
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(fixture.log, sink));
  }));
  check(sink.commands.empty(), "hidden log skips paint");
}

void nativeStyleAndFrame() {
  Fixture fixture;

  const auto originalColumn = take(fixture.app->tree().inspect(fixture.log)).parent;

  take(fixture.scene->remove(originalColumn));

  constexpr RgbColor NORMAL{12, 34, 56};
  constexpr RgbColor DISABLED{78, 90, 123};
  ControlStyle style;

  style.normal.foreground = NORMAL;
  style.focused = style.normal;
  style.disabled.foreground = DISABLED;
  style.disabled.italic = true;
  fixture.log = take(
    ui::mount(*fixture.scene, ui::logView("body", {.followTail = false}, style).framed("double", "Events").height(6)));
  fixture.prepare({80, 6});

  const auto render = [&](auto inspect) {
    PaintSink sink;

    take(fixture.app->tree().frame([&] {
      take(fixture.app->tree().paint(fixture.log, sink));

      const auto buffer = take(renderFullFrame({80, 6}, sink.commands));

      inspect(buffer);
    }));
  };

  render([&](const CellBuffer &buffer) {
    check(take(buffer.at({1, 1})).style.foreground == NORMAL && take(buffer.at({0, 0})).style.foreground == NORMAL &&
            take(buffer.at({1, 5})).style.foreground == NORMAL,
          "manual style reaches native log body, frame and footer");

    bool title{};

    for (std::int32_t column = 1; column < 79; ++column) {
      title |= take(buffer.at({column, 0})).glyph == "E";
    }

    check(take(buffer.at({0, 0})).glyph == "╔" && title, "actual native advanced double frame retains owning title");
  });

  StyleSheet sheet;

  sheet.rules.push_back({.selector = {.type = "LogView"}, .declarations = {.bold = true}});
  take(fixture.scene->theme(sheet));
  fixture.prepare({80, 6});
  render([&](const CellBuffer &buffer) {
    check(take(buffer.at({1, 1})).style.bold && take(buffer.at({0, 0})).style.bold &&
            take(buffer.at({1, 5})).style.bold,
          "natural LogView selector reaches body, frame and footer");
  });
  take(fixture.app->tree().setEnabled(fixture.log, false));
  fixture.prepare({80, 6});
  render([&](const CellBuffer &buffer) {
    check(take(buffer.at({1, 1})).style.foreground == DISABLED &&
            take(buffer.at({0, 0})).style.foreground == DISABLED &&
            take(buffer.at({1, 5})).style.foreground == DISABLED && take(buffer.at({1, 1})).style.italic,
          "disabled native appearance does not retain normal body or footer defaults");
  });
}

void facadeAndLoadedDefaults() {
  ui::Ref<LogView> native;
  ui::ControlContext controls;

  controls.defaults("LogView",
                    {.maxLines = 2, .maxBytes = 32, .maxBatchBytes = 32, .maxRecordBytes = 16, .followTail = false});

  auto owner = ui::scene(ui::logView(native, "first\nsecond\nthird"), {.controls = controls});

  take(owner.prepareChecked());
  check(native.status()->retainedLines == 2 && native.status()->droppedLines == 1 && !native.status()->followTail,
        "native scoped defaults overlay bounded config without extra runtime");

  const auto before = take(owner.diagnostics()).errors.size();
  const auto checked = native.appendBatch("\xff");

  check(!checked && take(owner.diagnostics()).errors.size() == before, "checked append does not auto-report");
  check(!native.append("\xff") && take(owner.diagnostics()).errors.size() == before + 1,
        "short append reports original failure exactly once");
  take(owner.close());

  const auto late = native.appendBatch("late");

  check(!late && std::get<TreeError>(late.error().cause) == TreeError::CLOSED, "late Ref observes closed owner");

  constexpr std::string_view SOURCE = R"(
    defaults { LogView { maxLines=2 maxBytes=32 maxBatchBytes=32 maxRecordBytes=16 followTail=false } }
    scene { VBox { LogView#log { text="one\ntwo\nthree\n" readOnly=true } } }
  )";
  auto loaded = ui::scene(SOURCE, {.format = ui::SceneResourceFormat::CONFIGURATION});
  auto log = loaded.get<LogView>("log");
  ManagedScene *loadedManaged{};
  Application *loadedApp{};

  loaded.configure([&](ManagedScene &scene, Application &app) {
    loadedManaged = &scene;
    loadedApp = &app;
  });

  take(loaded.prepareChecked());
  check(log.status()->retainedLines == 2 && log.status()->droppedLines == 1 && !log.status()->followTail,
        "builtin loaded log accepts exact caps and initial follow default without registration");
  prepareManaged(*loadedManaged, *loadedApp, {80, 8});
  take(loadedApp->tree().read(log.node(), [&](const LogView &widget) {
    check(widget.sizePolicy().height.mode == SizeMode::FLEX &&
            detail::ScrollWidgetAccess::state(widget).geometry.viewport.height == 6,
          "loaded default log receives a positive flexible viewport");
  }));

  StyleSheet loadedTheme;

  loadedTheme.rules.push_back({.selector = {.type = "LogView"}, .declarations = {.bold = true}});
  take(loadedManaged->theme(loadedTheme));
  prepareManaged(*loadedManaged, *loadedApp, {80, 8});

  PaintSink loadedPaint;

  take(loadedApp->tree().frame([&] {
    take(loadedApp->tree().paint(log.node(), loadedPaint));

    const auto buffer = take(renderFullFrame({80, 8}, loadedPaint.commands));

    check(take(buffer.at({1, 1})).style.bold && take(buffer.at({1, 7})).style.bold,
          "loaded builtin retains its natural LogView style identity for body and footer");
  }));
  check(log.append("four\n") && log.tail() && log.clear(), "loaded readOnly permits programmatic commands");

  Fixture rollback;
  const auto beforeChildren = take(rollback.app->tree().inspect(rollback.scene->root())).children.size();
  const auto rejected = rollback.scene->logView(rollback.scene->root(), "good\n\xff");

  check(!rejected && rejected.error().text && rejected.error().text->kind == TextErrorKind::INVALID_UTF8,
        "initial admission retains typed error after partial mount");
  check(take(rollback.app->tree().inspect(rollback.scene->root())).children.size() == beforeChildren,
        "failed initial batch removes only attempted child");
}

void positionDuringAppendStaging() {
  constexpr std::size_t MAX_LINES = 6;
  Fixture fixture({.limits = {.maxLines = MAX_LINES}});

  take(appendLogBatch(fixture.app->tree(), fixture.log, "A\nB\nC\nD\nE\nF"));
  fixture.prepare({8, 4});
  check(take(fixture.controller->snapshot()).vertical.offset.raster == 4, "reentry fixture starts following F");

  static Fixture *active{};
  static bool positioned{};

  active = &fixture;
  positioned = false;
  // Candidate reference-array staging occurs after navigation was sampled, on both supported libraries.
  allocation_test::hookBytes = MAX_LINES * sizeof(std::shared_ptr<const detail::LogRecord>);
  allocation_test::hook = +[] {
    positioned = bool(active->scene->setScrollOffset(active->log, ScrollAxis::VERTICAL, {1, 0}));
  };

  const auto appended = appendLogBatch(fixture.app->tree(), fixture.log, "G");

  allocation_test::hook = nullptr;
  allocation_test::hookBytes = 0;
  active = nullptr;
  check(positioned, "public POSITION is accepted during fallible append staging");
  check(!appended && std::get<TreeError>(appended.error()) == TreeError::WOULD_BLOCK,
        "changed navigation rejects the stale append candidate before publication");
  fixture.read([&](const auto &data) {
    check(data.records.front()->id == 1 && data.records.back()->id == 6 && data.nextID == 7 &&
            data.status.droppedLines == 0,
          "stale candidate rejection preserves original records IDs and counters");
  });
  fixture.prepare({8, 4});
  fixture.read([&](const auto &data) {
    std::cout << "staging position accepted=" << positioned << " outer append accepted=" << bool(appended)
              << " reading anchor=" << data.anchor.record << '\n';
    check(!data.status.followTail && data.anchor.record == 2, "accepted nested POSITION still selects B, not C");
  });
}

void publicationFaultBudget() {
  constexpr std::string_view BATCH = "second\nthird\nfourth";
  Fixture measured;

  take(appendLogBatch(measured.app->tree(), measured.log, "first"));
  measured.prepare({12, 6});
  allocation_test::count = 0;
  allocation_test::remaining = -1;
  allocation_test::armed = true;

  const auto successful = appendLogBatch(measured.app->tree(), measured.log, BATCH);

  allocation_test::armed = false;
  take(successful);

  const auto allocations = allocation_test::count;

  check(allocations > 2, "publication workload observes its own fallible staging allocations");
  std::cout << "append staging allocations=" << allocations << '\n';

  // Three boundaries cover candidate creation, prepared content and the final prepublication allocation.
  // This is one local publication budget, not a sweep of the renderer, tree or existing Unicode engine.
  for (const auto budget : {std::size_t{0}, allocations / 2, allocations - 1}) {
    Fixture fixture;

    take(appendLogBatch(fixture.app->tree(), fixture.log, "first"));
    fixture.prepare({12, 6});

    allocation_test::count = 0;
    allocation_test::remaining = static_cast<std::ptrdiff_t>(budget);
    allocation_test::armed = true;

    const auto rejected = appendLogBatch(fixture.app->tree(), fixture.log, BATCH);

    allocation_test::armed = false;
    allocation_test::remaining = -1;
    check(!rejected && std::get<TreeError>(rejected.error()) == TreeError::NO_MEMORY,
          "own staging allocation failure maps to original NO_MEMORY");
    fixture.read([&](const auto &data) {
      check(data.records.size() == 1 && data.records.front()->id == 1 && data.nextID == 2 && data.rows.size() == 1 &&
              data.status.droppedLines == 0,
            "failed publication preserves records, row index, IDs and counters");
    });
    std::cout << "rejected staging budget=" << budget << '\n';
  }

  Fixture reentry;
  static Fixture *active{};
  static bool blocked{};

  active = &reentry;
  allocation_test::hook = +[] {
    const auto nested = appendLogBatch(active->app->tree(), active->log, "nested");

    blocked = !nested && std::get<TreeError>(nested.error()) == TreeError::WOULD_BLOCK;
  };
  take(appendLogBatch(reentry.app->tree(), reentry.log, BATCH));
  check(blocked, "allocation reentry cannot mutate in-flight log admission");

  allocation_test::hook = +[] {
    take(active->scene->remove(active->log));
  };

  const auto retired = appendLogBatch(reentry.app->tree(), reentry.log, "retire during staging");

  check(!retired && std::get<TreeError>(retired.error()) == TreeError::STALE_NODE,
        "postallocation liveness rejects publication to a retired owner");
  active = nullptr;
}
} // namespace

int main() {
  convergenceAndAnchor();
  admissionAndUnicode();
  guardsAndBounds();
  structuralBoundsAndSizing();
  narrowViewportStorage();
  paintAndFooterInput();
  nativeStyleAndFrame();
  facadeAndLoadedDefaults();
  positionDuringAppendStaging();
  publicationFaultBudget();

  std::cout << "checks=" << checks << " failures=" << failures << '\n';

  return failures ? 1 : 0;
}
