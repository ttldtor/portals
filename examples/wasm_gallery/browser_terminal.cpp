// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "browser_terminal.hpp"

#include <utility>

namespace wasm_gallery {
using namespace portals;

std::expected<std::unique_ptr<BrowserTerminal>, TerminalError>
BrowserTerminal::create(CellSize size, Sink sink, TerminalOptions options) {
  if (size.width < 0 || size.height < 0 || !sink || options.max_input_bytes == 0 || options.max_output_bytes == 0) {
    return std::unexpected(TerminalError{TerminalErrorKind::INVALID_OPTIONS});
  }

  return std::unique_ptr<BrowserTerminal>(new BrowserTerminal(size, std::move(sink), options));
}

BrowserTerminal::BrowserTerminal(CellSize size, Sink sink, TerminalOptions options)
    : size_(size), sink_(std::move(sink)), maxInputBytes_(options.max_input_bytes),
      maxOutputBytes_(options.max_output_bytes) {
}

std::optional<TerminalError> BrowserTerminal::operationError() const noexcept {
  if (closed_) {
    return TerminalError{TerminalErrorKind::CLOSED};
  }

  return failure_;
}

std::expected<void, TerminalError> BrowserTerminal::pushInput(std::string_view bytes) {
  if (const auto error = operationError()) {
    return std::unexpected(*error);
  }

  if (bytes.size() > maxInputBytes_ - input_.size()) {
    return recordFailure({TerminalErrorKind::INPUT_LIMIT});
  }

  input_.append(bytes);

  return {};
}

std::expected<void, TerminalError> BrowserTerminal::resize(CellSize size) {
  if (const auto error = operationError()) {
    return std::unexpected(*error);
  }

  if (size.width < 0 || size.height < 0) {
    return recordFailure({TerminalErrorKind::INVALID_OPTIONS});
  }

  if (size != size_) {
    size_ = size;
    resized_ = size;
  }

  return {};
}

std::expected<void, TerminalError> BrowserTerminal::recordFailure(TerminalError error) {
  if (const auto current = operationError()) {
    return std::unexpected(*current);
  }

  failure_ = error;

  return std::unexpected(error);
}

TerminalRead BrowserTerminal::poll(std::stop_token stop) {
  if (const auto error = operationError()) {
    return {TerminalStatus::FAILED, {}, {}, {}, *error};
  }

  if (stop.stop_requested()) {
    return {TerminalStatus::STOPPED};
  }

  if (input_.empty() && !resized_) {
    return {TerminalStatus::DEADLINE};
  }

  TerminalRead result;

  result.bytes.swap(input_);
  result.resized = std::exchange(resized_, std::nullopt);

  return result;
}

std::expected<CellSize, TerminalError> BrowserTerminal::size() const {
  if (const auto error = operationError()) {
    return std::unexpected(*error);
  }

  return size_;
}

std::expected<void, TerminalError> BrowserTerminal::submit(std::string bytes) {
  if (const auto error = operationError()) {
    return std::unexpected(*error);
  }

  if (pendingBytes() != 0) {
    return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_PENDING});
  }

  if (bytes.size() > maxOutputBytes_) {
    return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
  }

  output_ = std::move(bytes);
  outputOffset_ = 0;

  return {};
}

TerminalWrite BrowserTerminal::flushReady(std::stop_token stop) {
  if (const auto error = operationError()) {
    return {TerminalStatus::FAILED, 0, pendingBytes(), *error};
  }

  if (stop.stop_requested()) {
    return {TerminalStatus::STOPPED, 0, pendingBytes()};
  }

  if (pendingBytes() == 0) {
    return {};
  }

  const auto suffix = std::string_view(output_).substr(outputOffset_);
  std::expected<std::size_t, TerminalError> accepted;

  try {
    accepted = sink_(suffix);
  } catch (...) {
    accepted = std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_UNCERTAIN});
  }

  if (!accepted || *accepted > suffix.size()) {
    const auto error = accepted ? TerminalError{TerminalErrorKind::OUTPUT_UNCERTAIN} : accepted.error();

    failure_ = error;

    return {TerminalStatus::FAILED, 0, pendingBytes(), error};
  }

  outputOffset_ += *accepted;

  const auto remaining = pendingBytes();

  if (remaining == 0) {
    output_.clear();
    outputOffset_ = 0;
  }

  return {remaining == 0 ? TerminalStatus::PROGRESS : TerminalStatus::DEADLINE, *accepted, remaining};
}

std::size_t BrowserTerminal::pendingBytes() const noexcept {
  return output_.size() - outputOffset_;
}

std::expected<void, TerminalError> BrowserTerminal::close() noexcept {
  closed_ = true;
  input_.clear();
  output_.clear();
  outputOffset_ = 0;
  resized_.reset();
  failure_.reset();

  return {};
}
} // namespace wasm_gallery
