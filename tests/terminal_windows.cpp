// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/diff.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr DWORD childDeadlineMs = 20000;
constexpr DWORD writerObserveMs = 500;
constexpr DWORD maximumDrainPauseMs = 1500;
constexpr DWORD pressureBytes = 1024 * 1024;
constexpr DWORD watchdogExit = 75;
constexpr COORD frameViewport{240, 160};
constexpr std::size_t frameOutputLimit = 2 * 1024 * 1024;
constexpr std::size_t frameMinimumPressureBytes = 1024 * 1024;
constexpr portals::CellBufferLimits frameBufferLimits{240, 160, 38400, 16, 64, 8};
constexpr portals::FrameWriterLimits frameWriterLimits{frameOutputLimit, 1024 * 1024};
constexpr std::size_t maximumPressureFrames = 16;
constexpr DWORD minimumFrameWaitMs = 500;
constexpr DWORD maximumFrameCancellationMs = 1200;
constexpr DWORD slowFramePreparationMs = 1000;
constexpr DWORD pressureObservePollMs = 1;

struct Handle {
  HANDLE value = nullptr;

  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() {
    reset();
  }

  void reset(HANDLE h = nullptr) {
    if (value && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }

    value = h;
  }
};

void require(bool result, const char *operation) {
  if (!result) {
    throw std::runtime_error(std::string(operation) + " error=" + std::to_string(GetLastError()));
  }
}

enum Stage : DWORD {
  inputReady = 1,
  inputDone,
  pressureReady,
  cancelResult,
  pressureDone,
  childFailure,
  writerComplete,
  consoleClassification,
  resizeReady = 10,
  idleResizeReady = 12,
  pipeClassification,
  frameCommitted,
  framePoisoned,
  frameJoined,
  frameRecovered,
  framePressureObserved
};
enum Flag : DWORD {
  blocked = 1,
  writeSucceeded = 2,
  hiddenWindow = 4,
  hostUnicode = 8,
  hostCtrlC = 16,
  viewportResized = 32,
  hostResizeRecord = 64,
  restored = 128
};

struct Message {
  DWORD stage;
  DWORD error;
  DWORD flags;
  DWORD elapsedMs;
  std::uint64_t amount;
  char detail[128];
};

void report(HANDLE resultPipe, Stage stage, DWORD error = 0, DWORD flags = 0, DWORD elapsed = 0,
            std::uint64_t amount = 0, const char *detail = "") {
  Message message{stage, error, flags, elapsed, amount, {}};
  std::snprintf(message.detail, sizeof(message.detail), "%s", detail);
  DWORD written = 0;
  require(WriteFile(resultPipe, &message, sizeof(message), &written, nullptr) && written == sizeof(message), "report");
}

INPUT_RECORD key(wchar_t character, BOOL down = TRUE) {
  INPUT_RECORD record{};
  record.EventType = KEY_EVENT;
  record.Event.KeyEvent.bKeyDown = down;
  record.Event.KeyEvent.wRepeatCount = 1;
  record.Event.KeyEvent.uChar.UnicodeChar = character;

  return record;
}

using namespace std::chrono_literals;
using namespace portals;

struct NativeConsole {
  Handle input{CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr)};
  Handle output{CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  DWORD inputMode{}, outputMode{};
  UINT inputPage{}, outputPage{};

  NativeConsole() {
    require(GetConsoleMode(input.value, &inputMode) && GetConsoleMode(output.value, &outputMode), "snapshot modes");
    inputPage = GetConsoleCP();
    outputPage = GetConsoleOutputCP();
  }
  NativeTerminalHandles handles() const {
    return {reinterpret_cast<std::intptr_t>(input.value), reinterpret_cast<std::intptr_t>(output.value)};
  }
  void checkRestored() const {
    DWORD in = 0, out = 0;
    require(GetConsoleMode(input.value, &in) && GetConsoleMode(output.value, &out) && in == inputMode &&
              out == outputMode && GetConsoleCP() == inputPage && GetConsoleOutputCP() == outputPage,
            "exact modes/pages restoration");
  }
  void inject(const std::vector<INPUT_RECORD> &records) const {
    DWORD count = 0;
    DWORD currentMode = 0;
    require(GetConsoleMode(input.value, &currentMode), "snapshot injection mode");
    require(SetConsoleMode(input.value, currentMode & ~ENABLE_VIRTUAL_TERMINAL_INPUT), "raw-record injection mode");
    require(WriteConsoleInputW(input.value, records.data(), static_cast<DWORD>(records.size()), &count) &&
              count == records.size(),
            "inject native records");
    require(SetConsoleMode(input.value, currentMode), "restore VT mode before session read");
  }
};

std::vector<InputEvent> collect(TerminalSession &session, std::size_t desired) {
  std::vector<InputEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + 2s;

  while (events.size() < desired && std::chrono::steady_clock::now() < deadline) {
    auto batch = session.read(deadline);
    require(batch.status != TerminalStatus::error, "semantic read");
    require(batch.events.size() <= 2 && batch.bytes.empty(), "bounded events-only batch");

    for (auto &event : batch.events) {
      events.push_back(std::move(event));
    }
  }

  if (events.size() != desired) {
    std::string detail = "semantic count " + std::to_string(events.size()) + "/" + std::to_string(desired);

    for (const auto &event : events) {
      detail += " type=" + std::to_string(event.index());

      if (const auto *text = std::get_if<TextInputEvent>(&event)) {
        detail += ":" + std::to_string(text->scalar);
      }
    }
    throw std::runtime_error(detail);
  }

  return events;
}

DWORD inputCase(HANDLE resultPipe) {
  NativeConsole native;
  require(!IsWindowVisible(GetConsoleWindow()), "headless console");
  TerminalOptions options;
  options.max_input_events = 2;
  options.windows_input.max_paste_bytes = 64;
  auto opened = TerminalSession::open(native.handles(), options);
  require(opened.has_value(), "native TerminalSession open");
  TerminalSession session = std::move(*opened);
  require(!opened->size() && opened->close().has_value(), "moved source closed");
  require(FlushConsoleInputBuffer(native.input.value), "clear fixture input");
  DWORD mode = 0;
  require(GetConsoleMode(native.input.value, &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_INPUT) &&
            !(mode & (ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)),
          "VT input and Ctrl+C key policy");
  auto up = key(0);
  up.Event.KeyEvent.wVirtualKeyCode = VK_UP;
  auto mouse = INPUT_RECORD{};
  mouse.EventType = MOUSE_EVENT;
  mouse.Event.MouseEvent.dwMousePosition = {4, 7};
  mouse.Event.MouseEvent.dwButtonState = FROM_LEFT_1ST_BUTTON_PRESSED;
  auto repeat = key(L'r');
  repeat.Event.KeyEvent.wRepeatCount = 3;
  auto ctrlSpace = key(0);
  ctrlSpace.Event.KeyEvent.wVirtualKeyCode = VK_SPACE;
  ctrlSpace.Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED;
  native.inject(
    {key(L'\x0416'), key(L'\x0416', FALSE), key(L'\xd83d'), key(L'\xde00'), up, mouse, key(3), repeat, ctrlSpace});
  const auto events = collect(session, 9);
  require(std::get<TextInputEvent>(events[0]).scalar == U'\u0416' &&
            std::get<TextInputEvent>(events[1]).scalar == U'\U0001f600',
          "Unicode surrogate composition");
  require(std::get<KeyEvent>(events[2]).key == InputKey::up, "native arrow order");
  require(std::get<MouseInputEvent>(events[3]) == MouseInputEvent{4, 7, MouseButton::left, MouseAction::press, {}},
          "native mouse order/payload");
  require(std::get<KeyEvent>(events[4]).scalar == U'C' && std::get<KeyEvent>(events[4]).modifiers.control,
          "Ctrl+C semantic key");

  for (std::size_t index = 5; index < 8; ++index) {
    require(std::get<TextInputEvent>(events[index]).scalar == U'r', "bounded repeat preservation");
  }
  require(std::get<KeyEvent>(events[8]).modifiers.control, "Ctrl+Space NUL preserved");
  auto nativeAlt = key(L'x');
  nativeAlt.Event.KeyEvent.dwControlKeyState = LEFT_ALT_PRESSED | SHIFT_PRESSED;
  nativeAlt.Event.KeyEvent.wRepeatCount = 3;
  auto altGr = key(L'\x20ac');
  altGr.Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED;
  native.inject({key(27), key(L'x'), nativeAlt, altGr});
  const auto modified = collect(session, 5);
  require(std::get<TextInputEvent>(modified[0]).modifiers.alt, "VT ESC Alt survives native modifier merge");

  for (std::size_t index = 1; index < 4; ++index) {
    const auto &text = std::get<TextInputEvent>(modified[index]);
    require(text.modifiers.alt && text.modifiers.shift, "native repeated modifiers preserved");
  }
  require(std::get<TextInputEvent>(modified[4]).modifiers == InputModifiers{}, "AltGr character is text");
  mouse.Event.MouseEvent.dwButtonState = RIGHTMOST_BUTTON_PRESSED;
  native.inject({mouse});
  const auto switched = collect(session, 2);
  require(std::get<MouseInputEvent>(switched[0]).button == MouseButton::left &&
            std::get<MouseInputEvent>(switched[0]).action == MouseAction::release &&
            std::get<MouseInputEvent>(switched[1]).button == MouseButton::right &&
            std::get<MouseInputEvent>(switched[1]).action == MouseAction::press,
          "all changed mouse buttons retained");
  auto highRepeat = key(L'\xd83d');
  auto lowRepeat = key(L'\xde00');
  highRepeat.Event.KeyEvent.wRepeatCount = lowRepeat.Event.KeyEvent.wRepeatCount = 3;
  native.inject({highRepeat, lowRepeat});

  for (const auto &event : collect(session, 3)) {
    require(std::get<TextInputEvent>(event).scalar == U'\U0001f600', "surrogate repeated pair");
  }
  auto many = key(L'm');
  many.Event.KeyEvent.wRepeatCount = MAXWORD;
  native.inject({many});
  auto bounded = session.read(std::chrono::steady_clock::now() + 500ms);
  require(bounded.events.size() <= options.max_input_events, "maximum repeat count remains bounded");
  report(resultPipe, resizeReady);
  bool repeatResize = false;
  const auto repeatDeadline = std::chrono::steady_clock::now() + 1s;
  while (!repeatResize && std::chrono::steady_clock::now() < repeatDeadline) {
    auto batch = session.read(repeatDeadline);
    require(batch.events.size() <= options.max_input_events, "repeat and resize stay bounded");
    repeatResize = batch.resized == CellSize{95, 31};
    Sleep(1);
  }
  require(repeatResize, "resize observed while repeat still pending");
  // Close discards the remaining repeat; it never expands a 65535-item queue.
  require(session.close().has_value(), "close pending repeat");
  native.checkRestored();
  opened = TerminalSession::open(native.handles(), options);
  require(opened.has_value(), "reopen after pending input");
  session = std::move(*opened);
  require(FlushConsoleInputBuffer(native.input.value), "clear discarded fixture input");
  std::vector<INPUT_RECORD> paste;

  for (const char ch : std::string("\x1b[A\x1b[200~a\x03\x1b[A\x1b[201~z")) {
    paste.push_back(key(ch));
  }
  native.inject(paste);
  const auto decoded = collect(session, 3);
  require(std::get<KeyEvent>(decoded[0]).key == InputKey::up, "VT arrow decoder interop");
  require(std::get<PasteInputEvent>(decoded[1]).bytes == "a\x03\x1b[A" &&
            std::get<TextInputEvent>(decoded[2]).scalar == U'z',
          "paste controls remain payload and ordered");
  INPUT_RECORD ignored{};
  ignored.EventType = FOCUS_EVENT;
  native.inject({ignored, key(L'q', FALSE)});
  auto idle = session.read(std::chrono::steady_clock::now() + 30ms);
  require(idle.status == TerminalStatus::deadline, "nontext-only records do not block read");
  native.inject({ignored});
  report(resultPipe, idleResizeReady);
  const auto idleResizeStart = std::chrono::steady_clock::now();
  const auto idleResize = session.read(idleResizeStart + 1s);
  require(idleResize.resized == CellSize{97, 33} && std::chrono::steady_clock::now() - idleResizeStart < 700ms,
          "delayed resize wakes long read after ignored record");
  std::stop_source stopped;
  stopped.request_stop();
  require(session.read(std::chrono::steady_clock::now() - 1ms, stopped.get_token()).status == TerminalStatus::stopped,
          "input stop precedes deadline");
  report(resultPipe, inputReady);
  bool unicode = false, control = false, resized = false;
  const auto hostDeadline = std::chrono::steady_clock::now() + 3s;

  while ((!unicode || !control || !resized) && std::chrono::steady_clock::now() < hostDeadline) {
    auto batch = session.read(hostDeadline);
    require(batch.status != TerminalStatus::error, "host input read");
    resized |= batch.resized == CellSize{91, 37};

    for (const auto &event : batch.events) {
      if (const auto *text = std::get_if<TextInputEvent>(&event)) {
        unicode |= text->scalar == U'\u0416';
      }

      if (const auto *pressed = std::get_if<KeyEvent>(&event)) {
        control |= pressed->scalar == U'C';
      }
    }
  }
  require(unicode && control && resized, "actual ConPTY UTF8/CtrlC/viewport resize");
  require(session.submit("native VT output\r\n").has_value(), "submit output");
  const auto preStopped = session.flush(std::chrono::steady_clock::now() + 1s, stopped.get_token());
  require(preStopped.status == TerminalStatus::stopped && !preStopped.error && preStopped.pending == 18,
          "unissued stopped output remains retryable");
  const auto preExpired = session.flush(std::chrono::steady_clock::now() - 1ms);
  require(preExpired.status == TerminalStatus::deadline && !preExpired.error && preExpired.pending == 18,
          "unissued expired output remains retryable");
  const auto output = session.flush(std::chrono::steady_clock::now() + 1s);
  require(output.status == TerminalStatus::progress && output.completed == 18 && output.pending == 0,
          "actual output completion");
  require(session.close().has_value() && session.close().has_value(), "explicit close idempotence");
  native.checkRestored();
  try {
    auto exceptional = TerminalSession::open(native.handles());
    require(exceptional.has_value(), "exception path open");
    throw std::runtime_error("caught fixture error");
  } catch (const std::runtime_error &) {
  }
  native.checkRestored();
  TerminalOptions invalid;
  invalid.max_input_events = 0;
  require(!TerminalSession::open(native.handles(), invalid), "invalid options rejected");
  native.checkRestored();
  Handle readOnly(
    CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  auto rollback = TerminalSession::open({native.handles().input, reinterpret_cast<std::intptr_t>(readOnly.value)});
  require(!rollback && rollback.error().kind == TerminalErrorKind::system, "output mode error surfaced");
  native.checkRestored();
  auto wrongRole = TerminalSession::open({native.handles().input, native.handles().input});
  require(!wrongRole && wrongRole.error().kind == TerminalErrorKind::system, "input handle is not output buffer");
  native.checkRestored();
  HANDLE read = nullptr, write = nullptr;
  require(CreatePipe(&read, &write, nullptr, 4096), "redirected pipe");
  Handle pipeRead(read), pipeWrite(write);

  for (const auto handles : {NativeTerminalHandles{reinterpret_cast<std::intptr_t>(read), native.handles().output},
                             NativeTerminalHandles{native.handles().input, reinterpret_cast<std::intptr_t>(write)}}) {
    auto rejected = TerminalSession::open(handles);
    require(!rejected && rejected.error().kind == TerminalErrorKind::unsupported, "redirected endpoint rejected");
    native.checkRestored();
  }
  report(resultPipe, inputDone, 0, restored);

  return 0;
}

DWORD pressureCase(HANDLE resultPipe, const wchar_t *kind) {
  NativeConsole native;
  auto opened = TerminalSession::open(native.handles());
  require(opened.has_value(), "pressure open");
  std::string bytes(pressureBytes, 'x');

  for (std::size_t index = 79; index < bytes.size(); index += 80) {
    bytes[index] = '\n';
  }
  require(opened->submit(std::move(bytes)).has_value(), "pressure submit");
  const bool resume = wcscmp(kind, L"resume") == 0;
  const bool stop = wcscmp(kind, L"stop") == 0;
  std::stop_source source;
  std::jthread stopper;

  if (stop) {
    stopper = std::jthread([&source] {
      Sleep(writerObserveMs);
      source.request_stop();
    });
  }
  const ULONGLONG start = GetTickCount64();
  report(resultPipe, pressureReady);
  const auto written =
    opened->flush(std::chrono::steady_clock::now() + (resume || stop ? 4s : 500ms), source.get_token());
  const DWORD elapsed = static_cast<DWORD>(GetTickCount64() - start);
  require(written.status == (resume ? TerminalStatus::progress
                             : stop ? TerminalStatus::stopped
                                    : TerminalStatus::deadline),
          "pressure completion reason");

  if (resume) {
    require(written.completed == pressureBytes && written.pending == 0 && elapsed >= 500,
            "paused output completes after host resume");
  } else {
    require(elapsed < 1200 && written.pending > 0 && written.error &&
              written.error->kind == TerminalErrorKind::output_uncertain,
            "flush returns before host resume");
    auto rejected = opened->submit("must not replay");
    require(!rejected && rejected.error().kind == TerminalErrorKind::output_uncertain,
            "cancelled output cannot replay suffix");
    require(opened->flush(std::chrono::steady_clock::now() + 100ms).status == TerminalStatus::error,
            "cancelled flush cannot retry");
  }
  report(resultPipe, writerComplete, 0, blocked, elapsed, written.completed);
  require(opened->close().has_value(), "pressure join and restore");
  native.checkRestored();
  report(resultPipe, pressureDone, 0, blocked | restored, static_cast<DWORD>(GetTickCount64() - start),
         written.completed);

  return 0;
}

bool isFrameCase(const wchar_t *kind) {
  return wcscmp(kind, L"frame-resume") == 0 || wcscmp(kind, L"frame-cancel") == 0 || wcscmp(kind, L"frame-stop") == 0;
}

CellBuffer makeFrame(bool recovery, bool alternate = false) {
  auto frame = CellBuffer::from({frameViewport.X, frameViewport.Y}, {}, frameBufferLimits);
  require(frame.has_value(), "bounded native frame allocation");
  const auto ascii = GraphemeText::from(alternate ? "B" : "A");
  const CellStyle first{{RgbColor{255, 127, 64}}, {RgbColor{5, 41, 90}}, true, true, true};
  const CellStyle second{{RgbColor{3, 127, 9}}, {RgbColor{84, 193, 211}}, false, true, true};
  require(ascii.has_value(), "native frame text");

  if (!recovery) {
    for (int y = 0; y < frameViewport.Y; ++y) {
      for (int x = 0; x < frameViewport.X; ++x) {
        require(frame->put({x, y}, *ascii, 0, x % 2 ? first : second).has_value(), "native pressure cell");
      }
    }
  } else {
    const auto mark = GraphemeText::from("R");
    const auto omega = GraphemeText::from("\xce\xa9");
    const auto wide = GraphemeText::from("\xe4\xb8\xad");
    require(mark && omega && wide, "native recovery text");
    require(frame->put({0, 0}, *mark, 0, first).has_value() && frame->put({1, 0}, *omega, 0).has_value() &&
              frame->put({2, 0}, *wide, 0).has_value() &&
              frame->put({frameViewport.X - 2, frameViewport.Y - 1}, *wide, 0, second).has_value(),
            "native recovery cells");
  }

  return std::move(*frame);
}

CHAR_INFO nativeCell(HANDLE output, SHORT x, SHORT y) {
  CHAR_INFO cell{};
  SMALL_RECT area{x, y, x, y};
  require(ReadConsoleOutputW(output, &cell, {1, 1}, {0, 0}, &area), "native frame cell observation");
  require(area.Left == x && area.Right == x && area.Top == y && area.Bottom == y, "native cell inside viewport");

  return cell;
}

void checkNativeFrame(HANDLE output, bool recovery, bool alternate = false) {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  require(GetConsoleScreenBufferInfo(output, &info), "native frame cursor observation");
  require(info.dwCursorPosition.X == 0 && info.dwCursorPosition.Y == 0 && info.srWindow.Left == 0 &&
            info.srWindow.Top == 0,
          "completed frame cursor at home");

  if (!recovery) {
    const wchar_t expected = alternate ? L'B' : L'A';
    require(nativeCell(output, 0, 0).Char.UnicodeChar == expected &&
              nativeCell(output, frameViewport.X - 1, frameViewport.Y - 1).Char.UnicodeChar == expected,
            "native full frame reaches lower-right without scroll");
  } else {
    const auto lowerLead = nativeCell(output, frameViewport.X - 2, frameViewport.Y - 1);
    const auto lowerTail = nativeCell(output, frameViewport.X - 1, frameViewport.Y - 1);
    require(nativeCell(output, 0, 0).Char.UnicodeChar == L'R' &&
              nativeCell(output, 1, 0).Char.UnicodeChar == L'\x03a9' &&
              nativeCell(output, 2, 0).Char.UnicodeChar == L'\x4e2d' && lowerLead.Char.UnicodeChar == L'\x4e2d' &&
              (lowerLead.Attributes & COMMON_LVB_LEADING_BYTE) && (lowerTail.Attributes & COMMON_LVB_TRAILING_BYTE),
            "native recovered BMP and complete CJK width at lower-right");
    require(nativeCell(output, frameViewport.X - 1, 0).Char.UnicodeChar == L' ' &&
              nativeCell(output, frameViewport.X / 2, frameViewport.Y / 2).Char.UnicodeChar == L' ',
            "native recovery removes stale pressure cells");
  }
}

DWORD frameCase(HANDLE resultPipe, const wchar_t *kind) {
  NativeConsole native;
  require(!IsWindowVisible(GetConsoleWindow()), "headless native frame console");
  TerminalOptions options;
  options.max_output_bytes = frameOutputLimit;
  auto session = TerminalSession::open(native.handles(), options);
  require(session.has_value() && session->size() == CellSize{frameViewport.X, frameViewport.Y},
          "native frame viewport matches buffer");
  TerminalFrameOutput output(*session);
  FrameWriter writer(output, frameWriterLimits);
  auto desired = makeFrame(false);
  const auto encoded = encode_diff(nullptr, desired, frameOutputLimit);
  require(encoded && encoded->size() >= frameMinimumPressureBytes, "native frame creates bounded pressure batch");
  auto expectedBytes = encoded->size();
  require(writer.submit(std::move(desired)).has_value() && session->pending_bytes() == 0 && writer.pending() &&
            !writer.committed_frame(),
          "frame submission does no native I/O or belief commit");
  std::stop_source preStopped;
  preStopped.request_stop();
  const auto stoppedBeforeIo = writer.flush(std::chrono::steady_clock::now() + 1s, preStopped.get_token());
  require(stoppedBeforeIo.status == TerminalStatus::stopped && !stoppedBeforeIo.error &&
            stoppedBeforeIo.completed == 0 && stoppedBeforeIo.pending == expectedBytes && writer.pending() &&
            !writer.poisoned() && !writer.committed_frame(),
          "frame stop before I/O preserves retryable batch and unknown belief");
  const auto expiredBeforeIo = writer.flush(std::chrono::steady_clock::now() - 1ms);
  require(expiredBeforeIo.status == TerminalStatus::deadline && !expiredBeforeIo.error &&
            expiredBeforeIo.pending == expectedBytes && !writer.poisoned(),
          "frame deadline before I/O preserves retryable batch");
  auto blockedFrame = makeFrame(true);
  const auto rejectedPending = writer.submit(std::move(blockedFrame));
  require(!rejectedPending && rejectedPending.error() == FrameError::pending &&
            blockedFrame.size() == CellSize{frameViewport.X, frameViewport.Y},
          "native pending frame cannot be replaced");

  const bool resume = wcscmp(kind, L"frame-resume") == 0;
  const bool stop = wcscmp(kind, L"frame-stop") == 0;
  std::stop_source source;
  std::atomic<ULONGLONG> activeFlushStarted{0};
  std::atomic<bool> observerReady{false};
  std::atomic<bool> resumePressureObserved{false};
  std::exception_ptr observerError;
  std::jthread pressureObserver;

  if (resume || stop) {
    // Start the observer before ConPTY pressure can delay new thread startup.
    pressureObserver = std::jthread([&](std::stop_token observerStop) {
      observerReady.store(true);

      try {
        while (!observerStop.stop_requested()) {
          const auto flushStarted = activeFlushStarted.load();

          if (flushStarted != 0 && GetTickCount64() - flushStarted >= minimumFrameWaitMs &&
              activeFlushStarted.load() == flushStarted) {
            if (resume) {
              resumePressureObserved.store(true);
              report(resultPipe, framePressureObserved, 0, blocked,
                     static_cast<DWORD>(GetTickCount64() - flushStarted));
            } else {
              source.request_stop();
            }

            return;
          }

          Sleep(pressureObservePollMs);
        }
      } catch (...) {
        observerError = std::current_exception();
      }
    });

    while (!observerReady.load()) {
      Sleep(pressureObservePollMs);
    }
  }

  const ULONGLONG start = GetTickCount64();
  report(resultPipe, pressureReady, 0, hiddenWindow, 0, expectedBytes);

  // ConPTY can accept one complete viewport before its renderer blocks on the
  // host pipe. Bound the warmup series and require actual pending uncertainty,
  // rather than treating a large encoded byte count as proof of backpressure.
  const auto writeBudget = resume || stop ? 4s : 500ms;
  TerminalWrite written;
  DWORD elapsed = 0;
  DWORD flushWaitMs = 0;
  bool alternate = false;
  std::size_t completedFrames = 0;

  for (std::size_t attempt = 0; attempt < maximumPressureFrames; ++attempt) {
    if (attempt == 1) {
      // Preparation must not consume a later flush's cancellation budget.
      Sleep(slowFramePreparationMs);
    }

    const ULONGLONG flushStarted = GetTickCount64();

    if (resume || stop) {
      activeFlushStarted.store(flushStarted);
    }

    // Each flush owns its wait budget; frame preparation is outside it.
    const auto writeDeadline = std::chrono::steady_clock::now() + writeBudget;

    written = writer.flush(writeDeadline, source.get_token());
    activeFlushStarted.store(0);
    flushWaitMs = static_cast<DWORD>(GetTickCount64() - flushStarted);
    elapsed = static_cast<DWORD>(GetTickCount64() - start);
    char completion[128]{};
    std::snprintf(completion, sizeof(completion),
                  "status=%d pending=%llu poisoned=%d committed=%d attempt=%llu flush_ms=%lu",
                  static_cast<int>(written.status), static_cast<unsigned long long>(written.pending), writer.poisoned(),
                  writer.committed_frame() != nullptr, static_cast<unsigned long long>(attempt + 1), flushWaitMs);
    report(resultPipe, writerComplete, written.error ? static_cast<DWORD>(written.error->native_code) : 0, 0, elapsed,
           written.completed, completion);

    if (written.status != TerminalStatus::progress) {
      break;
    }

    ++completedFrames;
    require(written.completed == expectedBytes && written.pending == 0 && writer.committed_frame(),
            "native warmup frame commits complete bytes only");

    if (resume && resumePressureObserved.load() && flushWaitMs >= minimumFrameWaitMs) {
      break;
    }

    require(attempt + 1 < maximumPressureFrames, "native fixture must establish bounded pressure");
    alternate = !alternate;
    auto next = makeFrame(false, alternate);
    const auto nextEncoded = encode_diff(writer.committed_frame(), next, frameOutputLimit);
    require(nextEncoded && nextEncoded->size() >= frameMinimumPressureBytes, "native alternating pressure delta");
    expectedBytes = nextEncoded->size();
    require(writer.submit(std::move(next)).has_value(), "native pressure series keeps only one pending frame");
  }

  if (pressureObserver.joinable()) {
    pressureObserver.request_stop();
    pressureObserver.join();
  }

  if (observerError) {
    std::rethrow_exception(observerError);
  }

  if (resume) {
    require(resumePressureObserved.load() && written.status == TerminalStatus::progress && !written.error &&
              written.completed == expectedBytes && written.pending == 0 && flushWaitMs >= minimumFrameWaitMs &&
              !writer.pending() && !writer.poisoned() && writer.committed_frame(),
            "native pressure resume commits only completed frame");
    checkNativeFrame(native.output.value, false, alternate);
    report(resultPipe, frameCommitted, 0, blocked, elapsed, written.completed);
    require(writer.submit(makeFrame(false, alternate)).has_value() && !writer.pending() &&
              session->pending_bytes() == 0,
            "unchanged native frame creates no terminal batch");
    require(writer.flush(std::chrono::steady_clock::now() + 1s).completed == 0,
            "unchanged native frame writes no bytes");
  } else {
    require(written.status == TerminalStatus::error && written.error &&
              written.error->kind == TerminalErrorKind::output_uncertain && written.pending > 0 &&
              flushWaitMs < maximumFrameCancellationMs && writer.poisoned() && !writer.pending() &&
              !writer.committed_frame() && completedFrames > 0,
            "native pressure cancellation poisons frame belief before host resume");
    const auto rejectedPoisoned = writer.submit(std::move(blockedFrame));
    require(!rejectedPoisoned && rejectedPoisoned.error() == FrameError::poisoned,
            "native uncertain writer rejects later frames");
    const auto pendingNative = session->pending_bytes();
    const auto refusedRetry = writer.flush(std::chrono::steady_clock::now() + 100ms);
    require(refusedRetry.status == TerminalStatus::error && refusedRetry.error &&
              refusedRetry.error->kind == TerminalErrorKind::closed && session->pending_bytes() == pendingNative,
            "poisoned writer rejects retry without forwarding to uncertain session");
    report(resultPipe, framePoisoned, 0, blocked, elapsed, written.completed);
  }

  require(session->close().has_value(), "frame transport close joins old writer before recovery");
  native.checkRestored();
  report(resultPipe, frameJoined, 0, blocked | restored, static_cast<DWORD>(GetTickCount64() - start));

  if (!resume) {
    auto freshSession = TerminalSession::open(native.handles(), options);
    require(freshSession.has_value(), "frame recovery opens fresh session after join");
    TerminalFrameOutput freshOutput(*freshSession);
    FrameWriter recovered(freshOutput, frameWriterLimits);
    require(recovered.submit(makeFrame(true)).has_value() && !recovered.committed_frame(),
            "new writer begins full CAN recovery without inherited belief");
    const auto completed = recovered.flush(std::chrono::steady_clock::now() + 4s);
    require(completed.status == TerminalStatus::progress && !completed.error && completed.completed > 0 &&
              completed.pending == 0 && recovered.committed_frame() && !recovered.pending(),
            "fresh native full repaint completes and commits");
    checkNativeFrame(native.output.value, true);
    require(freshSession->close().has_value(), "recovered frame closes normally");
    native.checkRestored();
    report(resultPipe, frameRecovered, 0, restored, static_cast<DWORD>(GetTickCount64() - start), completed.completed);
  }

  report(resultPipe, pressureDone, 0, blocked | restored, static_cast<DWORD>(GetTickCount64() - start),
         written.completed);

  return 0;
}

int nativeSmoke(const wchar_t *reportPath) {
  Handle outputReport(
    CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (outputReport.value == INVALID_HANDLE_VALUE) {
    return 2;
  }
  int status = 0;
  std::string detail;
  try {
    NativeConsole native;
    auto session = TerminalSession::open(native.handles());
    require(session.has_value(), "native smoke open");
    require(session->size().has_value(), "native smoke viewport");
    const std::string sample = "Portals native session: \xd0\x96 \xf0\x9f\x98\x80\r\n";
    require(session->submit(sample).has_value(), "native smoke submit");
    const auto written = session->flush(std::chrono::steady_clock::now() + 1s);
    require(written.status == TerminalStatus::progress && written.completed == sample.size(), "native smoke output");
    require(session->close().has_value(), "native smoke close");
    native.checkRestored();
    {
      auto exceptional = TerminalSession::open(native.handles());
      require(exceptional.has_value(), "native smoke exception open");
      try {
        TerminalSession owner = std::move(*exceptional);
        throw std::runtime_error("intentional caught fixture exception");
      } catch (const std::runtime_error &) {
      }
    }
    native.checkRestored();
    detail = "PASS: native public TerminalSession Unicode output, normal close, caught-error unwind; "
             "exact input/output modes and both code pages restored. No input consumed or injected.\n";
  } catch (const std::exception &error) {
    status = 1;
    detail = std::string("FAIL: ") + error.what() + "\n";
  }
  DWORD written = 0;
  if (!WriteFile(outputReport.value, detail.data(), static_cast<DWORD>(detail.size()), &written, nullptr) ||
      written != detail.size()) {
    return 2;
  }
  return status;
}
// Only fixture-owned pipe bytes are polled. The thread never blocks in ReadFile.
void drain(HANDLE output, std::atomic<bool> &enabled, std::atomic<bool> &stop, std::atomic<std::uint64_t> &total) {
  char buffer[16384];

  while (!stop.load()) {
    DWORD available = 0;

    if (enabled.load()) {
      if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) {
        break;
      }

      if (available != 0) {
        DWORD count = 0;
        const DWORD requested = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));

        if (!ReadFile(output, buffer, requested, &count, nullptr)) {
          break;
        }

        total.fetch_add(count);

        continue;
      }
    }

    Sleep(1);
  }
}

struct Session {
  Handle inputRead, inputWrite, outputRead, outputWrite, resultRead, resultWrite;
  Handle process, processThread, job;
  HPCON console = nullptr;
  std::atomic<bool> enabled{true};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> drained{0};
  std::thread reader;

  ~Session() {
    enabled.store(true);

    if (process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
      TerminateProcess(process.value, watchdogExit);
      const DWORD waited = WaitForSingleObject(process.value, 2000);
      DWORD code = STILL_ACTIVE;
      GetExitCodeProcess(process.value, &code);
      std::printf("cleanup child_pid=%lu waited=%lu child_exit=%lu\n", GetProcessId(process.value), waited, code);
      std::fflush(stdout);
    }

    inputWrite.reset();

    if (console) {
      ClosePseudoConsole(console);
    }

    stop.store(true);

    if (reader.joinable()) {
      reader.join();
    }
  }
};

void launch(Session &session, const wchar_t *kind) {
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  require(CreatePipe(&read, &write, nullptr, 4096), "input pipe");
  session.inputRead.reset(read);
  session.inputWrite.reset(write);
  require(CreatePipe(&read, &write, nullptr, 4096), "output pipe");
  session.outputRead.reset(read);
  session.outputWrite.reset(write);
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  require(CreatePipe(&read, &write, &security, 4096), "result pipe");
  session.resultRead.reset(read);
  session.resultWrite.reset(write);
  require(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0), "result inheritance");
  const HRESULT created = CreatePseudoConsole(isFrameCase(kind) ? frameViewport : COORD{80, 25},
                                              session.inputRead.value, session.outputWrite.value, 0, &session.console);
  require(SUCCEEDED(created), "CreatePseudoConsole");
  session.enabled.store(wcscmp(kind, L"input") == 0);
  session.reader = std::thread(drain, session.outputRead.value, std::ref(session.enabled), std::ref(session.stop),
                               std::ref(session.drained));
  session.job.reset(CreateJobObjectW(nullptr, nullptr));
  require(session.job.value != nullptr, "CreateJobObjectW");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
  limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  require(SetInformationJobObject(session.job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)),
          "job kill-on-close");
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  auto attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  require(InitializeProcThreadAttributeList(attributes, 2, 0, &bytes), "attributes");
  struct AttributeCleanup {
    PPROC_THREAD_ATTRIBUTE_LIST value;
    ~AttributeCleanup() {
      DeleteProcThreadAttributeList(value);
    }
  } cleanup{attributes};
  require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, session.console,
                                    sizeof(session.console), nullptr, nullptr),
          "pseudoconsole attribute");
  require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &session.resultWrite.value,
                                    sizeof(HANDLE), nullptr, nullptr),
          "result handle attribute");
  wchar_t path[32768];
  require(GetModuleFileNameW(nullptr, path, 32768) != 0, "self executable path");
  std::wstring command = L"\"" + std::wstring(path) + L"\" --child " + kind + L" " +
                         std::to_wstring(reinterpret_cast<std::uintptr_t>(session.resultWrite.value));
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  require(CreateProcessW(path, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                         nullptr, nullptr, &startup.StartupInfo, &process),
          "CreateProcessW child");
  session.process.reset(process.hProcess);
  session.processThread.reset(process.hThread);
  require(AssignProcessToJobObject(session.job.value, session.process.value), "AssignProcessToJobObject");
  require(ResumeThread(session.processThread.value) != static_cast<DWORD>(-1), "ResumeThread");
  std::printf("launched case=%ls host_pid=%lu child_pid=%lu\n", kind, GetCurrentProcessId(), process.dwProcessId);
  std::fflush(stdout);
  session.inputRead.reset();
  session.outputWrite.reset();
  session.resultWrite.reset();
}

bool hostCase(const wchar_t *kind) {
  Session session;
  launch(session, kind);
  const bool frameResume = wcscmp(kind, L"frame-resume") == 0;
  const bool frameCancellation = wcscmp(kind, L"frame-cancel") == 0 || wcscmp(kind, L"frame-stop") == 0;
  const ULONGLONG deadline = GetTickCount64() + (wcscmp(kind, L"hang") == 0 ? 500 : childDeadlineMs);
  ULONGLONG pressureStarted = 0;
  ULONGLONG idleResizeStarted = 0;
  bool finalSeen = false;
  bool pressureBlocked = false;
  bool framePressureSeen = false;
  DWORD childExit = STILL_ACTIVE;

  while (GetTickCount64() < deadline) {
    DWORD available = 0;

    while (PeekNamedPipe(session.resultRead.value, nullptr, 0, nullptr, &available, nullptr) &&
           available >= sizeof(Message)) {
      Message message{};
      DWORD count = 0;
      require(ReadFile(session.resultRead.value, &message, sizeof(message), &count, nullptr) &&
                count == sizeof(message),
              "host result read");
      std::printf("case=%ls stage=%lu error=%lu flags=%lu elapsed_ms=%lu amount=%llu detail=%s\n", kind, message.stage,
                  message.error, message.flags, message.elapsedMs, static_cast<unsigned long long>(message.amount),
                  message.detail);
      std::fflush(stdout);

      if (message.stage == resizeReady) {
        require(SUCCEEDED(ResizePseudoConsole(session.console, {95, 31})), "resize without text input");
      }
      if (message.stage == idleResizeReady) {
        idleResizeStarted = GetTickCount64();
      }
      if (message.stage == inputReady) {
        const char unicodeAndCtrlC[] = "\xd0\x96\x03";
        DWORD written = 0;
        require(WriteFile(session.inputWrite.value, unicodeAndCtrlC, 3, &written, nullptr) && written == 3,
                "host VT input write");
        require(SUCCEEDED(ResizePseudoConsole(session.console, {91, 37})), "ResizePseudoConsole");
      }

      if (message.stage == pressureReady && !isFrameCase(kind)) {
        pressureStarted = GetTickCount64();
      }

      if (message.stage == framePoisoned) {
        require(frameCancellation && (message.flags & blocked) != 0 && !session.enabled.load(),
                "host observes cancelled frame before resuming");
        session.enabled.store(true);
      }

      if (message.stage == framePressureObserved) {
        require(frameResume && message.elapsedMs >= minimumFrameWaitMs && (message.flags & blocked) != 0 &&
                  !session.enabled.load(),
                "host observes incomplete frame before resuming");
        framePressureSeen = true;
        session.enabled.store(true);
      }

      if (message.stage == inputDone || message.stage == pressureDone) {
        finalSeen = true;
        pressureBlocked = (message.flags & blocked) != 0;
      }
    }

    if (idleResizeStarted != 0 && GetTickCount64() - idleResizeStarted >= 100) {
      require(SUCCEEDED(ResizePseudoConsole(session.console, {97, 33})), "delayed idle host resize");
      idleResizeStarted = 0;
    }
    if (pressureStarted != 0) {
      const DWORD pause = wcscmp(kind, L"resume") == 0 ? 700 : maximumDrainPauseMs;

      if (GetTickCount64() - pressureStarted >= pause) {
        session.enabled.store(true);
      }
    }

    if (WaitForSingleObject(session.process.value, 0) == WAIT_OBJECT_0) {
      require(GetExitCodeProcess(session.process.value, &childExit), "child exit");

      if (finalSeen || childExit != 0) {
        break;
      }
    }

    Sleep(5);
  }

  session.enabled.store(true);
  std::printf("case=%ls child_exit=%lu final_seen=%d pressure_blocked=%d drained=%llu\n", kind, childExit, finalSeen,
              pressureBlocked, static_cast<unsigned long long>(session.drained.load()));

  return childExit == 0 && finalSeen && (!frameResume || framePressureSeen);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 3 && wcscmp(argv[1], L"--native-smoke") == 0) {
    return nativeSmoke(argv[2]);
  }
  if (argc == 4 && wcscmp(argv[1], L"--child") == 0) {
    const HANDLE result = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));

    try {
      if (wcscmp(argv[2], L"hang") == 0) {
        Sleep(30000);

        return 0;
      }

      if (isFrameCase(argv[2])) {
        return frameCase(result, argv[2]);
      }

      return wcscmp(argv[2], L"input") == 0 ? inputCase(result) : pressureCase(result, argv[2]);
    } catch (const std::exception &error) {
      report(result, childFailure, GetLastError(), 0, 0, 0, error.what());

      return 1;
    }
  }

  try {
    if (argc == 2 && wcscmp(argv[1], L"--child-watchdog") == 0) {
      return hostCase(L"hang") ? 0 : watchdogExit;
    }

    if (argc == 2 && wcscmp(argv[1], L"--hold-host") == 0) {
      Session session;
      launch(session, L"hang");
      Sleep(30000);

      return 0;
    }

    const bool input = hostCase(L"input");
    const bool cancel = hostCase(L"cancel");
    const bool resume = hostCase(L"resume");
    const bool stopped = hostCase(L"stop");
    const bool frameResume = hostCase(L"frame-resume");
    const bool frameCancel = hostCase(L"frame-cancel");
    const bool frameStop = hostCase(L"frame-stop");
    std::printf("summary input=%d cancel=%d resume=%d stopped=%d frame_resume=%d frame_cancel=%d frame_stop=%d\n",
                input, cancel, resume, stopped, frameResume, frameCancel, frameStop);

    return input && cancel && resume && stopped && frameResume && frameCancel && frameStop ? 0 : 1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "host failure: %s\n", error.what());

    return 2;
  }
}
