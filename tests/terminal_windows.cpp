// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/terminal.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
  pipeClassification
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
  const HRESULT created =
    CreatePseudoConsole({80, 25}, session.inputRead.value, session.outputWrite.value, 0, &session.console);
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
  const ULONGLONG deadline = GetTickCount64() + (wcscmp(kind, L"hang") == 0 ? 500 : childDeadlineMs);
  ULONGLONG pressureStarted = 0;
  ULONGLONG idleResizeStarted = 0;
  bool finalSeen = false;
  bool pressureBlocked = false;
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

      if (message.stage == pressureReady) {
        pressureStarted = GetTickCount64();
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

  return childExit == 0 && finalSeen;
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
    std::printf("summary input=%d cancel=%d resume=%d\n", input, cancel, resume);

    return input && cancel && resume && stopped ? 0 : 1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "host failure: %s\n", error.what());

    return 2;
  }
}
