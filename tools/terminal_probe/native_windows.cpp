// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "native.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>

namespace portals::probe {
namespace {
constexpr std::string_view requests = "\x1b[0c\x1b[6n";
constexpr DWORD event_chunk_size = 32;
constexpr std::size_t event_summary_limit = 1024;

std::string error(std::string_view operation) {
  return std::string(operation) + ": Win32 error " + std::to_string(GetLastError());
}

std::string transport(HANDLE handle, bool console) {
  if (console) {
    return "Win32 console API (may be ConPTY; host identity unknown)";
  }

  switch (GetFileType(handle)) {
  case FILE_TYPE_PIPE:
    return "redirected pipe";
  case FILE_TYPE_DISK:
    return "redirected file";
  case FILE_TYPE_CHAR:
    return "character device without console mode";
  default:
    return "unknown/invalid handle";
  }
}

class ModeGuard {
public:
  ModeGuard(HANDLE input, HANDLE output) : input_(input), output_(output) {
  }

  bool acquire() {
    if (!GetConsoleMode(input_, &input_mode_) || !GetConsoleMode(output_, &output_mode_)) {
      return false;
    }

    const DWORD input =
      (input_mode_ | ENABLE_EXTENDED_FLAGS) & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT |
                                                ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);

    if (!SetConsoleMode(input_, input)) {
      return false;
    }

    input_changed_ = true;

    if (!SetConsoleMode(output_, output_mode_ | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return false;
    }

    output_changed_ = true;

    return true;
  }

  bool restore() {
    bool restored = true;

    if (output_changed_) {
      output_changed_ = SetConsoleMode(output_, output_mode_) == FALSE;
      restored &= !output_changed_;
    }

    if (input_changed_) {
      input_changed_ = SetConsoleMode(input_, input_mode_) == FALSE;
      restored &= !input_changed_;
    }

    return restored;
  }

  ~ModeGuard() {
    restore();
  }

private:
  HANDLE input_;
  HANDLE output_;
  DWORD input_mode_ = 0;
  DWORD output_mode_ = 0;
  bool input_changed_ = false;
  bool output_changed_ = false;
};

void record_event(RunResult &result, const INPUT_RECORD &record) {
  ++result.non_byte_events;

  if (result.event_summary.size() >= event_summary_limit) {
    return;
  }

  std::string description = " type=" + std::to_string(record.EventType);

  if (record.EventType == KEY_EVENT) {
    description += " key_down=" + std::to_string(record.Event.KeyEvent.bKeyDown != FALSE) +
                   " utf16=" + std::to_string(static_cast<unsigned>(record.Event.KeyEvent.uChar.UnicodeChar)) +
                   " repeat=" + std::to_string(record.Event.KeyEvent.wRepeatCount);
  }

  result.event_summary += description.substr(0, event_summary_limit - result.event_summary.size());
}

RunResult collect(HANDLE input, HANDLE output, QueryCollector &collector, QueryCollector::Time deadline,
                  std::size_t event_limit) {
  RunResult result;

  if (std::chrono::steady_clock::now() >= deadline) {
    result.stop = "deadline during mode setup";

    return result;
  }

  DWORD written = 0;

  if (!WriteFile(output, requests.data(), static_cast<DWORD>(requests.size()), &written, nullptr) ||
      written != requests.size()) {
    result.stop = "I/O error";
    result.error = error("write query");

    return result;
  }

  std::size_t events_consumed = 0;

  while (collector.active(std::chrono::steady_clock::now()) && events_consumed < event_limit) {
    const auto remaining = deadline - std::chrono::steady_clock::now();

    if (remaining <= QueryCollector::Time::duration::zero()) {
      break;
    }

    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    const DWORD wait = WaitForSingleObject(input, static_cast<DWORD>(milliseconds));

    if (wait == WAIT_TIMEOUT) {
      break;
    }

    if (wait != WAIT_OBJECT_0) {
      result.stop = "I/O error";
      result.error = error("wait console input");

      return result;
    }

    DWORD available = 0;

    if (!GetNumberOfConsoleInputEvents(input, &available)) {
      result.stop = "I/O error";
      result.error = error("count console input records");

      return result;
    }

    if (available == 0) {
      continue;
    }

    const auto received_at = std::chrono::steady_clock::now();

    if (!collector.active(received_at)) {
      break;
    }

    // Only this standalone owner reads the console. Read at most available
    // records, rather than a blocking byte read after a non-text event signals.
    INPUT_RECORD records[event_chunk_size];
    const DWORD capacity = std::min({available, event_chunk_size, static_cast<DWORD>(event_limit - events_consumed)});
    DWORD count = 0;

    if (!ReadConsoleInputW(input, records, capacity, &count)) {
      result.stop = "I/O error";
      result.error = error("read console input records");

      return result;
    }

    events_consumed += count;

    for (DWORD index = 0; index < count; ++index) {
      const auto &record = records[index];

      if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE ||
          record.Event.KeyEvent.uChar.UnicodeChar == 0 || record.Event.KeyEvent.uChar.UnicodeChar > 0x7f) {
        record_event(result, record);

        continue;
      }

      const char byte = static_cast<char>(record.Event.KeyEvent.uChar.UnicodeChar);
      const auto repeats =
        std::min<std::size_t>(record.Event.KeyEvent.wRepeatCount, event_limit - collector.received());

      for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        collector.feed(std::string_view(&byte, 1), received_at);
      }

      if (repeats < record.Event.KeyEvent.wRepeatCount) {
        auto remainder = record;
        remainder.Event.KeyEvent.wRepeatCount = static_cast<WORD>(record.Event.KeyEvent.wRepeatCount - repeats);
        record_event(result, remainder);
      }
    }
  }

  if (collector.limited()) {
    result.stop = "byte limit";
  } else if (collector.device_attributes() && collector.cursor_position()) {
    result.stop = "replies";
  } else if (events_consumed == event_limit) {
    result.stop = "native input event limit";
  } else {
    result.stop = "deadline";
  }

  return result;
}
} // namespace

NativeFacts inspect_native() {
  NativeFacts facts;
  facts.fields.emplace_back("os.name", "Windows");
  using VersionFunction = LONG(WINAPI *)(OSVERSIONINFOW *);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto version_function =
    module == nullptr ? nullptr : reinterpret_cast<VersionFunction>(GetProcAddress(module, "RtlGetVersion"));
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);

  if (version_function != nullptr && version_function(&version) == 0) {
    facts.fields.emplace_back("os.version", std::to_string(version.dwMajorVersion) + "." +
                                              std::to_string(version.dwMinorVersion) + "." +
                                              std::to_string(version.dwBuildNumber) + " (RtlGetVersion)");
  } else {
    facts.fields.emplace_back("os.version", "unknown (RtlGetVersion unavailable)");
  }

  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  facts.fields.emplace_back("os.architecture",
                            std::to_string(system.wProcessorArchitecture) + " (Win32 architecture enum)");
  const auto input = GetStdHandle(STD_INPUT_HANDLE);
  const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD input_mode = 0;
  DWORD output_mode = 0;
  const bool input_console = GetConsoleMode(input, &input_mode) != FALSE;
  const bool output_console = GetConsoleMode(output, &output_mode) != FALSE;
  facts.query_eligible = input_console && output_console;
  facts.fields.emplace_back("transport.stdin", transport(input, input_console));
  facts.fields.emplace_back("transport.stdout", transport(output, output_console));
  facts.fields.emplace_back("transport.underlying", "unknown (console API does not identify ConPTY/frontend)");
  facts.fields.emplace_back("mode.stdin", input_console ? std::to_string(input_mode) : "unknown");
  facts.fields.emplace_back("mode.stdout", output_console ? std::to_string(output_mode) : "unknown");
  facts.fields.emplace_back("mode.stdin.code_page", input_console ? std::to_string(GetConsoleCP()) : "unknown");
  facts.fields.emplace_back("mode.stdout.code_page", output_console ? std::to_string(GetConsoleOutputCP()) : "unknown");
  CONSOLE_SCREEN_BUFFER_INFO size{};

  if (output_console && GetConsoleScreenBufferInfo(output, &size)) {
    facts.fields.emplace_back("size.columns", std::to_string(size.srWindow.Right - size.srWindow.Left + 1));
    facts.fields.emplace_back("size.rows", std::to_string(size.srWindow.Bottom - size.srWindow.Top + 1));
    facts.fields.emplace_back("size.buffer_columns", std::to_string(size.dwSize.X));
    facts.fields.emplace_back("size.buffer_rows", std::to_string(size.dwSize.Y));
  } else {
    facts.fields.emplace_back("size.columns", "unknown");
    facts.fields.emplace_back("size.rows", "unknown");
  }

  return facts;
}

RunResult run_native_queries(QueryCollector &collector, QueryCollector::Time deadline, std::size_t event_limit) {
  const auto input = GetStdHandle(STD_INPUT_HANDLE);
  const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD input_mode = 0;
  DWORD output_mode = 0;

  if (!GetConsoleMode(input, &input_mode) || !GetConsoleMode(output, &output_mode)) {
    return {"skipped: redirected handles", "", 0, ""};
  }

  ModeGuard modes(input, output);
  RunResult result;

  if (!modes.acquire()) {
    result = {"mode setup error", error("temporary console modes"), 0, ""};
  } else {
    result = collect(input, output, collector, deadline, event_limit);
  }

  if (!modes.restore()) {
    result.error += "; " + error("restore console modes");
    result.stop = "restore error";
  }

  return result;
}
} // namespace portals::probe
