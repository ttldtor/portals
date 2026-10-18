// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../examples/wasm_gallery/browser_terminal.hpp"

#include <iostream>
#include <source_location>
#include <stdexcept>
#include <vector>

namespace {
using namespace portals;
using wasm_gallery::BrowserTerminal;

void check(bool value, std::source_location where = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("browser terminal check line " + std::to_string(where.line()));
  }
}

void errorIs(const TerminalError &error, TerminalErrorKind kind, int native = 0) {
  check(error.kind == kind && error.native_code == native);
}

void inputAndResize() {
  auto made = BrowserTerminal::create({80, 24}, [](std::string_view bytes) {
    return std::expected<std::size_t, TerminalError>{bytes.size()};
  });
  check(made.has_value());
  auto &terminal = **made;
  check(terminal.poll().status == TerminalStatus::DEADLINE);
  check(terminal.size()->width == 80);
  check(terminal.resize({90, 25}).has_value());
  check(terminal.resize({100, 30}).has_value());

  std::string first{"\xe2\x82", 2};
  check(terminal.pushInput(first).has_value());
  first[0] = 'x';
  auto read = terminal.poll();
  check(read.status == TerminalStatus::PROGRESS && read.bytes == std::string("\xe2\x82", 2));
  check(read.resized && read.resized->width == 100 && read.resized->height == 30);
  check(terminal.pushInput(std::string_view{"\xac", 1}).has_value());
  check(terminal.poll().bytes == std::string("\xac", 1));
  check(read.bytes == std::string("\xe2\x82", 2));
  check(terminal.resize({100, 30}).has_value());
  check(terminal.pushInput("").has_value());
  check(terminal.poll().status == TerminalStatus::DEADLINE);
}

void partialOutput() {
  std::vector<std::string> offers;
  std::string copied;
  std::vector<std::size_t> receipts{2, 0, 1, 3};
  auto made = BrowserTerminal::create({80, 24}, [&](std::string_view bytes) {
    offers.emplace_back(bytes);
    const auto count = receipts.at(offers.size() - 1);
    copied.append(bytes.substr(0, count));
    return std::expected<std::size_t, TerminalError>{count};
  });
  auto &terminal = **made;
  const std::string bytes{"A\xe2\x82\xac"
                          "BC",
                          6};

  check(terminal.submit(bytes).has_value());
  check(offers.empty() && terminal.pendingBytes() == 6);
  auto rejected = terminal.submit("other");
  check(!rejected);
  errorIs(rejected.error(), TerminalErrorKind::OUTPUT_PENDING);

  auto write = terminal.flushReady();
  check(write.status == TerminalStatus::DEADLINE && write.completed == 2 && write.pending == 4);
  write = terminal.flushReady();
  check(write.status == TerminalStatus::DEADLINE && write.completed == 0 && write.pending == 4);
  write = terminal.flushReady();
  check(write.completed == 1 && write.pending == 3);
  write = terminal.flushReady();
  check(write.status == TerminalStatus::PROGRESS && write.completed == 3 && write.pending == 0);
  check((offers == std::vector<std::string>{bytes, bytes.substr(2), bytes.substr(2), bytes.substr(3)}));
  check(copied == bytes);
  check(terminal.flushReady().completed == 0 && offers.size() == 4);
  check(terminal.submit("").has_value());
  check(terminal.flushReady().status == TerminalStatus::PROGRESS && offers.size() == 4);
}

void boundsAndFailures() {
  TerminalOptions options;
  options.max_input_bytes = 3;
  options.max_output_bytes = 4;
  auto sink = [](std::string_view bytes) {
    return std::expected<std::size_t, TerminalError>{bytes.size()};
  };
  auto made = BrowserTerminal::create({0, 0}, sink, options);
  auto &terminal = **made;
  auto result = terminal.submit("12345");
  check(!result);
  errorIs(result.error(), TerminalErrorKind::OUTPUT_LIMIT);
  check(terminal.pendingBytes() == 0 && terminal.submit("1234").has_value());
  check(terminal.pushInput("12").has_value());
  result = terminal.pushInput("34");
  check(!result);
  errorIs(result.error(), TerminalErrorKind::INPUT_LIMIT);
  auto read = terminal.poll();
  check(read.status == TerminalStatus::FAILED && read.error && read.bytes.empty());
  errorIs(*read.error, TerminalErrorKind::INPUT_LIMIT);
  check(!terminal.size());
  check(!terminal.recordFailure({TerminalErrorKind::SYSTEM, 42}));
  errorIs(*terminal.flushReady().error, TerminalErrorKind::INPUT_LIMIT);
  check(terminal.close().has_value() && terminal.close().has_value());
  check(terminal.pendingBytes() == 0);
  result = terminal.pushInput("late");
  check(!result);
  errorIs(result.error(), TerminalErrorKind::CLOSED);
  check(!terminal.resize({1, 1}) && !terminal.submit("late"));
  errorIs(*terminal.poll().error, TerminalErrorKind::CLOSED);
  errorIs(*terminal.flushReady().error, TerminalErrorKind::CLOSED);
  check(!terminal.recordFailure({TerminalErrorKind::SYSTEM, 43}));
  errorIs(*terminal.poll().error, TerminalErrorKind::CLOSED);

  options.max_input_bytes = 0;
  check(!BrowserTerminal::create({1, 1}, sink, options));
  options.max_input_bytes = 3;
  options.max_output_bytes = 0;
  check(!BrowserTerminal::create({1, 1}, sink, options));
  check(!BrowserTerminal::create({-1, 1}, sink));
  check(!BrowserTerminal::create({1, 1}, {}));
}

void stopAndSinkErrors() {
  int calls{};
  auto made = BrowserTerminal::create({1, 1}, [&](std::string_view) -> std::expected<std::size_t, TerminalError> {
    ++calls;
    return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 71, 72});
  });
  auto &terminal = **made;
  check(terminal.submit("abc").has_value() && terminal.pushInput("x").has_value());
  check(terminal.resize({0, 0}).has_value());
  std::stop_source stop;
  stop.request_stop();
  check(terminal.poll(stop.get_token()).status == TerminalStatus::STOPPED);
  auto write = terminal.flushReady(stop.get_token());
  check(write.status == TerminalStatus::STOPPED && write.completed == 0 && write.pending == 3 && calls == 0);
  const auto ready = terminal.poll();

  check(ready.bytes == "x" && ready.resized && ready.resized->width == 0 && ready.resized->height == 0);
  write = terminal.flushReady();
  check(write.status == TerminalStatus::FAILED && write.completed == 0 && write.pending == 3 && calls == 1);
  errorIs(*write.error, TerminalErrorKind::SYSTEM, 71);
  check(write.error->restoration_code == 72);
  errorIs(*terminal.flushReady().error, TerminalErrorKind::SYSTEM, 71);
  check(calls == 1);
  errorIs(terminal.submit("new").error(), TerminalErrorKind::SYSTEM, 71);
  check(terminal.close().has_value() && terminal.close().has_value() && calls == 1);

  made = BrowserTerminal::create({1, 1}, [&](std::string_view bytes) {
    ++calls;
    return std::expected<std::size_t, TerminalError>{bytes.size() + 1};
  });
  check((*made)->submit("x").has_value());
  write = (*made)->flushReady();
  check(write.status == TerminalStatus::FAILED && write.completed == 0 && write.pending == 1);
  errorIs(*write.error, TerminalErrorKind::OUTPUT_UNCERTAIN);
  check((*made)->flushReady().status == TerminalStatus::FAILED && calls == 2);

  made = BrowserTerminal::create({1, 1}, [](std::string_view) -> std::expected<std::size_t, TerminalError> {
    throw std::runtime_error("sink");
  });
  check((*made)->submit("x").has_value());
  errorIs(*(*made)->flushReady().error, TerminalErrorKind::OUTPUT_UNCERTAIN);
}
} // namespace

int main() {
  try {
    inputAndResize();
    partialOutput();
    boundsAndFailures();
    stopAndSinkErrors();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
