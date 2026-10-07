// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "test_terminal.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <stdexcept>

namespace test_support {
namespace {
using namespace portals;
[[noreturn]] void invalid(std::string_view message) {
  throw std::runtime_error("trace: " + std::string(message));
}
std::uint64_t number(std::string_view token) {
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (token.empty() || error != std::errc{} || end != token.data() + token.size()) {
    invalid("invalid unsigned integer");
  }

  return value;
}
int hex_digit(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }

  invalid("invalid lowercase hex payload");
}
std::string unhex(std::string_view text) {
  if (text == "-") {
    return {};
  }
  if (text.empty() || text.size() % 2 != 0) {
    invalid("odd or missing hex payload");
  }
  std::string bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    bytes.push_back(static_cast<char>(hex_digit(text[i]) * 16 + hex_digit(text[i + 1])));
  }

  return bytes;
}
std::string hex(std::string_view bytes) {
  if (bytes.empty()) {
    return "-";
  }
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(bytes.size() * 2);
  for (unsigned char ch : bytes) {
    text.push_back(digits[ch >> 4]);
    text.push_back(digits[ch & 15]);
  }

  return text;
}
void validate_viewport(portals::CellSize size) {
  if (VtOracle::validate_size(size.width, size.height) > TraceLimits::cells) {
    invalid("viewport limit exceeded");
  }
}
void validate(std::span<const TraceRecord> trace) {
  if (trace.size() > TraceLimits::records) {
    invalid("record limit exceeded");
  }
  FakeClock clock;
  std::size_t payload = 0;
  std::size_t snapshots = 1;
  for (const auto &record : trace) {
    clock.advance_to(record.time);
    const bool byte_record = record.kind == TraceKind::input || record.kind == TraceKind::output;
    if ((!byte_record && !record.bytes.empty()) || (record.kind != TraceKind::resize && record.size != CellSize{}) ||
        (record.kind != TraceKind::fail_after && record.count != 0)) {
      invalid("unexpected record payload");
    }
    if (record.bytes.size() > TraceLimits::bytes - payload) {
      invalid("payload limit exceeded");
    }
    payload += record.bytes.size();
    switch (record.kind) {
    case TraceKind::resize:
      validate_viewport(record.size);
      break;
    case TraceKind::fail_after:
      if (record.count > TraceLimits::bytes) {
        invalid("failure prefix limit exceeded");
      }
      break;
    case TraceKind::snapshot:
      if (++snapshots > TraceLimits::snapshots) {
        invalid("snapshot limit exceeded");
      }
      break;
    case TraceKind::input:
    case TraceKind::output:
    case TraceKind::tick:
    case TraceKind::finish:
      break;
    default:
      invalid("unknown record kind");
    }
  }
}
void append(std::vector<InputEvent> &events, InputStep &step) {
  if (step.event) {
    if (events.size() == TraceLimits::events) {
      invalid("event limit exceeded");
    }
    events.push_back(std::move(*step.event));
  }
  if (step.status == InputStatus::failed) {
    invalid("input decoder failed");
  }
}
} // namespace

portals::InputTime FakeClock::now() const noexcept {
  return now_;
}
portals::TerminalDeadline FakeClock::deadline() const noexcept {
  return portals::TerminalDeadline{
    std::chrono::duration_cast<portals::TerminalDeadline::duration>(std::chrono::milliseconds{now_})};
}
void FakeClock::advance_to(portals::InputTime time) {
  if (time < now_ || time > max_time) {
    invalid("backward or unrepresentable fake time");
  }
  now_ = time;
}

Trace parse_trace(std::string_view text) {
  if (text.size() > TraceLimits::bytes) {
    invalid("file limit exceeded");
  }
  std::istringstream stream{std::string(text)};
  std::string line;
  if (!std::getline(stream, line) || (line != "PORTALS-TRACE 1" && line != "PORTALS-TRACE 1\r")) {
    invalid("line 1: unsupported version or missing PORTALS-TRACE header");
  }
  Trace trace;
  std::size_t line_number = 1;
  while (std::getline(stream, line)) {
    ++line_number;
    try {
      if (trace.size() == TraceLimits::records) {
        invalid("record limit exceeded");
      }
      std::istringstream fields(line);
      std::string time, kind, first, second, extra;
      if (!(fields >> time >> kind)) {
        invalid("missing time or operation");
      }
      TraceRecord record;
      record.time = number(time);
      if (kind == "INPUT" || kind == "OUTPUT") {
        fields >> first;
        record.kind = kind == "INPUT" ? TraceKind::input : TraceKind::output;
        record.bytes = unhex(first);
      } else if (kind == "RESIZE") {
        fields >> first >> second;
        const auto width = number(first);
        const auto height = number(second);
        if (width > VtOracle::max_dimension || height > VtOracle::max_dimension) {
          invalid("viewport limit exceeded");
        }
        record.kind = TraceKind::resize;
        record.size = {static_cast<int>(width), static_cast<int>(height)};
      } else if (kind == "FAIL_AFTER") {
        fields >> first;
        const auto count = number(first);
        if (count > TraceLimits::bytes) {
          invalid("failure prefix limit exceeded");
        }
        record.kind = TraceKind::fail_after;
        record.count = static_cast<std::size_t>(count);
      } else if (kind == "TICK") {
        record.kind = TraceKind::tick;
      } else if (kind == "FINISH") {
        record.kind = TraceKind::finish;
      } else if (kind == "SNAPSHOT") {
        record.kind = TraceKind::snapshot;
      } else {
        invalid("unknown operation");
      }
      if (fields >> extra) {
        invalid("unexpected trailing field");
      }
      // Validate the new record and chronological boundary before retaining it.
      validate(std::span<const TraceRecord>(&record, 1));
      if (!trace.empty() && record.time < trace.back().time) {
        invalid("backward fake time");
      }
      trace.push_back(std::move(record));
    } catch (const std::runtime_error &error) {
      invalid("line " + std::to_string(line_number) + ": " + error.what());
    }
  }
  validate(trace);

  return trace;
}

std::string record_trace(std::span<const TraceRecord> trace) {
  validate(trace);
  std::string text = "PORTALS-TRACE 1\n";
  for (const auto &record : trace) {
    std::string line = std::to_string(record.time) + " ";
    switch (record.kind) {
    case TraceKind::input:
      line += "INPUT " + hex(record.bytes);
      break;
    case TraceKind::output:
      line += "OUTPUT " + hex(record.bytes);
      break;
    case TraceKind::resize:
      line += "RESIZE " + std::to_string(record.size.width) + " " + std::to_string(record.size.height);
      break;
    case TraceKind::fail_after:
      line += "FAIL_AFTER " + std::to_string(record.count);
      break;
    case TraceKind::tick:
      line += "TICK";
      break;
    case TraceKind::finish:
      line += "FINISH";
      break;
    case TraceKind::snapshot:
      line += "SNAPSHOT";
      break;
    }
    line += '\n';
    if (line.size() > TraceLimits::bytes - text.size()) {
      invalid("file limit exceeded");
    }
    text += line;
  }

  return text;
}

TestTerminal::TestTerminal(FakeClock &clock, portals::CellSize size, std::size_t output_chunk)
    : clock_(clock), oracle_(0, 0), decoder_(std::move(*portals::InputDecoder::create())), output_chunk_(output_chunk) {
  if (output_chunk == 0) {
    invalid("output chunk must be positive");
  }

  resize(size);
}
void TestTerminal::resize(portals::CellSize size) {
  validate_viewport(size);
  oracle_.resize(size.width, size.height);
}
std::vector<portals::InputEvent> TestTerminal::input(std::string_view bytes) {
  if (bytes.size() > TraceLimits::bytes) {
    invalid("input fragment limit exceeded");
  }
  std::vector<portals::InputEvent> events;
  // Each byte may emit an event and one deferred issue. Empty pulls drain at most two.
  const auto budget = bytes.size() * 2 + 4;
  std::size_t offset = 0;
  for (std::size_t calls = 0; calls < budget; ++calls) {
    auto step = decoder_.next(bytes.substr(offset), clock_.now());
    if (!step || step->consumed > bytes.size() - offset) {
      invalid("input decoder rejected feed");
    }
    offset += step->consumed;
    append(events, *step);
    if (step->status == portals::InputStatus::need_input && offset == bytes.size()) {
      return events;
    }
  }

  invalid("input work budget exceeded");
}
std::vector<portals::InputEvent> TestTerminal::tick() {
  return input({});
}
std::vector<portals::InputEvent> TestTerminal::finish() {
  std::vector<portals::InputEvent> events;
  for (unsigned call = 0; call < 4; ++call) {
    auto step = decoder_.finish(clock_.now());
    if (!step) {
      invalid("input decoder rejected EOF");
    }
    append(events, *step);
    if (step->status == portals::InputStatus::finished) {
      return events;
    }
  }

  invalid("EOF work budget exceeded");
}
VtOracle::Snapshot TestTerminal::snapshot() const {
  return oracle_.snapshot();
}
void TestTerminal::fail_after(std::size_t accepted) {
  if (accepted > TraceLimits::bytes) {
    invalid("failure prefix limit exceeded");
  }
  fail_after_ = accepted;
}
std::expected<void, portals::TerminalError> TestTerminal::submit(std::string bytes) {
  using namespace portals;
  if (failed_) {
    return std::unexpected(TerminalError{TerminalErrorKind::output_uncertain});
  }
  if (!pending_.empty()) {
    return std::unexpected(TerminalError{TerminalErrorKind::output_pending});
  }
  if (bytes.size() > TraceLimits::bytes) {
    return std::unexpected(TerminalError{TerminalErrorKind::output_limit});
  }
  pending_ = std::move(bytes);

  return {};
}
portals::TerminalWrite TestTerminal::flush(portals::TerminalDeadline deadline, std::stop_token stop) {
  using namespace portals;
  if (failed_) {
    return {TerminalStatus::error, 0, pending_.size(), TerminalError{TerminalErrorKind::output_uncertain}};
  }
  if (stop.stop_requested() || deadline <= clock_.deadline()) {
    return {stop.stop_requested() ? TerminalStatus::stopped : TerminalStatus::deadline, 0, pending_.size(), {}};
  }
  const auto count = std::min(pending_.size(), fail_after_.value_or(pending_.size()));
  for (std::size_t offset = 0; offset < count;) {
    const auto fragment = std::min(output_chunk_, count - offset);
    oracle_.feed(std::string_view(pending_).substr(offset, fragment));
    offset += fragment;
  }
  pending_.erase(0, count);
  if (fail_after_) {
    failed_ = true;
    fail_after_.reset();

    return {TerminalStatus::error, count, pending_.size(), TerminalError{TerminalErrorKind::system, 1}};
  }

  return {TerminalStatus::progress, count, pending_.size(), {}};
}

ReplayResult replay(std::span<const TraceRecord> trace, std::size_t input_chunk, std::size_t output_chunk) {
  validate(trace);
  if (input_chunk == 0 || output_chunk == 0) {
    invalid("chunk sizes must be positive");
  }
  FakeClock clock;
  TestTerminal terminal(clock, {1, 1}, output_chunk);
  ReplayResult result;
  auto collect = [&](std::vector<portals::InputEvent> events) {
    if (events.size() > TraceLimits::events - result.events.size()) {
      invalid("event limit exceeded");
    }
    for (auto &event : events) {
      result.events.push_back({clock.now(), std::move(event)});
    }
  };
  for (const auto &record : trace) {
    clock.advance_to(record.time);
    switch (record.kind) {
    case TraceKind::input:
      if (record.bytes.empty()) {
        collect(terminal.input({}));
      }
      for (std::size_t offset = 0; offset < record.bytes.size();) {
        const auto count = std::min(input_chunk, record.bytes.size() - offset);
        collect(terminal.input(std::string_view(record.bytes).substr(offset, count)));
        offset += count;
      }
      break;
    case TraceKind::resize:
      terminal.resize(record.size);
      if (result.events.size() == TraceLimits::events) {
        invalid("event limit exceeded");
      }
      result.events.push_back({clock.now(), record.size});
      break;
    case TraceKind::output: {
      // One transport transaction: fragment only the independent interpreter feed.
      if (!terminal.submit(record.bytes)) {
        invalid("output submission rejected");
      }
      const auto write = terminal.flush(portals::TerminalDeadline::max());
      result.writes.push_back({write.status, write.completed, write.pending, write.error.has_value()});
      break;
    }
    case TraceKind::fail_after:
      terminal.fail_after(record.count);
      break;
    case TraceKind::tick:
      collect(terminal.tick());
      break;
    case TraceKind::finish:
      collect(terminal.finish());
      break;
    case TraceKind::snapshot:
      result.snapshots.push_back(terminal.snapshot());
      break;
    }
  }
  result.snapshots.push_back(terminal.snapshot());

  return result;
}
} // namespace test_support
