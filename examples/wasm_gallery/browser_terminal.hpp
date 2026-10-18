// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/application.hpp>

#include <functional>
#include <string_view>

namespace wasm_gallery {

/** @brief Bounded single-UI-thread transport for a browser host, with no waits or implicit output.
 * All calls, including host delivery and destruction, require the same thread and must not reenter.
 * Input and output own their bytes. No JavaScript or WASM heap pointer is retained by this adapter.
 * Allocation failures may throw. The first host/sink failure is retained until close; failed output is never retried.
 */
class BrowserTerminal final : public portals::ApplicationReadyTerminal {
public:
  /** @brief Synchronous nonblocking byte acceptance function.
   * @param bytes Borrowed unsent suffix, valid only during this invocation. Copy every accepted byte before returning.
   * @return Accepted prefix length, zero for backpressure, or an original transport failure.
   * Must return promptly without waiting for display completion and must not reenter this transport.
   * An exception or a count larger than bytes.size() makes output uncertain and permanently fails the transport.
   */
  using Sink = std::function<std::expected<std::size_t, portals::TerminalError>(std::string_view bytes)>;

  /** @brief Validate and create an exclusive transport without performing host I/O.
   * @param size Initial nonnegative cell viewport; zero dimensions are supported.
   * @param sink Owning callback; captured borrowed state must survive close. An empty callback is invalid.
   * @param options Positive max_input_bytes and max_output_bytes queue limits; native-only fields are unused.
   * @return Owner, or INVALID_OPTIONS for an invalid viewport, callback or relevant limit.
   */
  static std::expected<std::unique_ptr<BrowserTerminal>, portals::TerminalError>
  create(portals::CellSize size, Sink sink, portals::TerminalOptions options = {});
  ~BrowserTerminal() override = default;                        ///< Release storage without emitting terminal commands.
  BrowserTerminal(const BrowserTerminal &) = delete;            ///< Exclusive input/output state cannot be copied.
  BrowserTerminal &operator=(const BrowserTerminal &) = delete; ///< Exclusive state cannot be assigned.

  /** @brief Copy host bytes into the bounded input queue without decoding or running the application.
   * @param bytes Borrowed fragment, including incomplete UTF-8 or escape sequences; empty is allowed.
   * @return Success, CLOSED, retained first failure, or a sticky INPUT_LIMIT failure without partial admission.
   */
  std::expected<void, portals::TerminalError> pushInput(std::string_view bytes);
  /** @brief Coalesce host resize notifications without running the application.
   * @param size New nonnegative viewport; zero dimensions are supported.
   * @return Success, CLOSED, retained failure, or sticky INVALID_OPTIONS for negative dimensions.
   */
  std::expected<void, portals::TerminalError> resize(portals::CellSize size);
  /** @brief Retain the first asynchronous host error for the next ready operation.
   * @param error Owning original bridge error; never replaces an earlier error.
   * @return The retained failure, or CLOSED after close; always an unexpected result.
   */
  std::expected<void, portals::TerminalError> recordFailure(portals::TerminalError error);
  /** @param stop Cancellation signal; stopped polls do not consume input or resize.
   * @return One owning batch, DEADLINE when empty, STOPPED, or the retained failure.
   */
  portals::TerminalRead poll(std::stop_token stop = {}) override;
  /** @return Latest viewport, CLOSED or retained failure; does not consume resize notifications. */
  std::expected<portals::CellSize, portals::TerminalError> size() const override;
  /** @param bytes Complete owning transaction; no sink invocation occurs here.
   * @return Success, CLOSED, retained failure, OUTPUT_PENDING, or OUTPUT_LIMIT; rejected batches are not retained.
   */
  std::expected<void, portals::TerminalError> submit(std::string bytes) override;
  /** @param stop Cancellation signal; a stopped flush preserves the unsent suffix.
   * @return One sink invocation's exact acceptance; DEADLINE while pending, PROGRESS when complete, or failure.
   */
  portals::TerminalWrite flushReady(std::stop_token stop = {}) override;
  /** @return Unaccepted bytes still owned, including a failed transaction; zero after close. */
  std::size_t pendingBytes() const noexcept;
  /** @brief Discard queues and close once, without emitting clear/restoration bytes or calling the sink.
   * @return Success, including repeated close; the last host picture remains visible.
   */
  std::expected<void, portals::TerminalError> close() noexcept override;

private:
  BrowserTerminal(portals::CellSize size, Sink sink, portals::TerminalOptions options);
  std::optional<portals::TerminalError> operationError() const noexcept;

  portals::CellSize size_;
  Sink sink_;
  std::size_t maxInputBytes_;
  std::size_t maxOutputBytes_;
  std::string input_;
  std::string output_;
  std::size_t outputOffset_{};
  std::optional<portals::CellSize> resized_;
  std::optional<portals::TerminalError> failure_;
  bool closed_{};
};
} // namespace wasm_gallery
