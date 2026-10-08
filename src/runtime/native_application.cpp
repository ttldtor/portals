// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/application.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string_view>
#include <utility>

namespace portals {
namespace {
constexpr std::string_view SAVE_TRACKING = "\x1b[?9;1000;1001;1002;1003s";
constexpr std::string_view RESTORE_TRACKING = "\x1b[?9;1000;1001;1002;1003r";
constexpr std::string_view ENABLE_MOUSE = "\x1b[?1005;1015;1016l\x1b[?1000;1006h";
constexpr std::array ENCODING_MODES{1005, 1006, 1015, 1016};
constexpr std::size_t MAX_QUERY_REPLY_BYTES = 16;
constexpr int MODE_SET = 1;
constexpr int MODE_RESET = 2;
constexpr int MODE_PERMANENTLY_SET = 3;

constexpr auto MAX_MODE_TIMEOUT =
  std::chrono::duration_cast<std::chrono::milliseconds>(TerminalDeadline::duration::max());

TerminalDeadline deadlineAfter(std::chrono::milliseconds timeout) {
  const auto now = std::chrono::steady_clock::now();
  const auto interval = std::chrono::duration_cast<TerminalDeadline::duration>(timeout);

  if (now > TerminalDeadline{} && TerminalDeadline::max().time_since_epoch() - now.time_since_epoch() < interval) {
    return TerminalDeadline::max();
  }

  return now + interval;
}

// Remove only complete replies for our four queries. Preserve unrelated input and
// an unfinished reply suffix, including bytes arriving before or between replies.
std::size_t collectReplies(std::string &bytes, std::array<int, ENCODING_MODES.size()> &states) {
  std::size_t cursor = 0;

  while ((cursor = bytes.find('\x1b', cursor)) != std::string::npos) {
    const std::string_view tail(bytes.data() + cursor, bytes.size() - cursor);
    bool consumed = false;

    for (std::size_t index = 0; index < ENCODING_MODES.size(); ++index) {
      const auto prefix = "\x1b[?" + std::to_string(ENCODING_MODES[index]) + ";";
      const auto length = prefix.size() + 3;

      if (tail.starts_with(prefix) && tail.size() >= length && tail[prefix.size()] >= '0' &&
          tail[prefix.size()] <= '4' && tail.substr(prefix.size() + 1, 2) == "$y") {
        states[index] = tail[prefix.size()] - '0';
        bytes.erase(cursor, length);
        consumed = true;

        break;
      }

      if (tail.size() < length) {
        for (char state = '0'; state <= '4'; ++state) {
          const auto reply = prefix + state + "$y";

          if (std::string_view(reply).starts_with(tail)) {
            return tail.size();
          }
        }
      }
    }

    if (!consumed) {
      ++cursor;
    }
  }

  return 0;
}

class NativeTerminal final : public ApplicationTerminal {
public:
  NativeTerminal(TerminalSession session, TerminalOptions options, std::chrono::milliseconds timeout, bool mouse)
      : session_(std::move(session)), inputLimit_(options.max_input_bytes), outputLimit_(options.max_output_bytes),
        timeout_(timeout), mouseReporting_(mouse) {
  }

  ~NativeTerminal() noexcept override {
    (void)close();
  }

  std::expected<void, TerminalError> start() {
#ifndef _WIN32
    if (!mouseReporting_) {
      return {};
    }

    const auto deadline = deadlineAfter(timeout_);
    auto queried = queryEncodings(deadline);

    if (!queried) {
      return queried;
    }

    // xterm 390 restores encoding with an 8-bit Boolean cast (DoRM), truncating
    // the saved numeric encoding. Use queried values; tracking uses integer DoRM0.
    restoreBytes_ += RESTORE_TRACKING;

    // Reject before changing modes if the mandatory cleanup batch cannot fit.
    if (restoreBytes_.size() > outputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
    }

    auto saved = send(SAVE_TRACKING, deadline);

    if (!saved) {
      return saved;
    }

    // Arm cleanup before allocating submission storage or partially changing modes.
    restoreNeeded_ = true;

    return send(ENABLE_MOUSE, deadline);
#else
    return {};
#endif
  }

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (!closed_ && (!startupInput_.bytes.empty() || startupInput_.resized)) {
      if (stop.stop_requested()) {
        return {TerminalStatus::STOPPED, {}, {}, {}, {}};
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        return {TerminalStatus::DEADLINE, {}, {}, {}, {}};
      }

      return std::exchange(startupInput_, {});
    }

    return session_.read(deadline, stop);
  }

  std::expected<CellSize, TerminalError> size() const override {
    return session_.size();
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    return session_.submit(std::move(bytes));
  }

  TerminalWrite flush(TerminalDeadline deadline, std::stop_token stop) override {
    return session_.flush(deadline, stop);
  }

  std::expected<void, TerminalError> close() noexcept override {
    if (closed_) {
      return {};
    }

    closed_ = true;
    std::optional<TerminalError> failure;

#ifndef _WIN32
    if (mouseReporting_) {
      try {
        const auto deadline = deadlineAfter(timeout_);
        auto drained = finishOutput(deadline);

        if (!drained) {
          failure = drained.error();
        } else if (restoreNeeded_) {
          auto restored = send(restoreBytes_, deadline);

          if (!restored) {
            failure = restored.error();
          }
        }
      } catch (const std::bad_alloc &) {
        failure = TerminalError{TerminalErrorKind::SYSTEM, ENOMEM};
      } catch (...) {
        failure = TerminalError{TerminalErrorKind::SYSTEM, EIO};
      }
    }
#endif

    auto closed = session_.close();

    if (!closed) {
      if (failure) {
        failure->restoration_code = closed.error().native_code;
      } else {
        failure = closed.error();
      }
    }

    if (failure) {
      return std::unexpected(*failure);
    }

    return {};
  }

private:
  std::expected<void, TerminalError> queryEncodings(TerminalDeadline deadline) {
    std::string queries;

    for (const int mode : ENCODING_MODES) {
      queries += "\x1b[?" + std::to_string(mode) + "$p";
    }

    auto sent = send(queries, deadline);

    if (!sent) {
      return sent;
    }

    std::array<int, ENCODING_MODES.size()> states;

    states.fill(-1);

    for (;;) {
      auto input = session_.read(deadline);

      if (input.error) {
        return std::unexpected(*input.error);
      }

      if (input.status != TerminalStatus::PROGRESS) {
        return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, ETIMEDOUT});
      }

      if (input.resized) {
        startupInput_.resized = input.resized;
      }

      std::size_t offset = 0;

      while (offset < input.bytes.size()) {
        const auto room = inputLimit_ + MAX_QUERY_REPLY_BYTES - startupInput_.bytes.size();
        const auto amount = std::min(room, input.bytes.size() - offset);

        if (amount == 0) {
          return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
        }

        startupInput_.bytes.append(input.bytes, offset, amount);
        offset += amount;

        const auto suffix = collectReplies(startupInput_.bytes, states);

        if (startupInput_.bytes.size() - suffix > inputLimit_) {
          return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
        }
      }

      if (std::ranges::all_of(states, [](int state) {
            return state >= 0;
          })) {
        break;
      }
    }

    if (startupInput_.bytes.size() > inputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
    }

    const auto sgr = states[1];

    if ((sgr != MODE_SET && sgr != MODE_RESET && sgr != MODE_PERMANENTLY_SET) || states[0] == MODE_PERMANENTLY_SET ||
        states[2] == MODE_PERMANENTLY_SET || states[3] == MODE_PERMANENTLY_SET) {
      return std::unexpected(TerminalError{TerminalErrorKind::UNSUPPORTED});
    }

    for (std::size_t index = 0; index < states.size(); ++index) {
      if (states[index] == MODE_SET || states[index] == MODE_RESET) {
        restoreBytes_ += "\x1b[?" + std::to_string(ENCODING_MODES[index]) + "l";
      }
    }

    for (std::size_t index = 0; index < states.size(); ++index) {
      if (states[index] == MODE_SET) {
        restoreBytes_ += "\x1b[?" + std::to_string(ENCODING_MODES[index]) + "h";
      }
    }

    return {};
  }

  std::expected<void, TerminalError> finishOutput(TerminalDeadline deadline) {
    const auto result = session_.flush(deadline);

    if (result.error) {
      return std::unexpected(*result.error);
    }

    if (result.status != TerminalStatus::PROGRESS || result.pending != 0) {
      return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, ETIMEDOUT});
    }

    return {};
  }

  std::expected<void, TerminalError> send(std::string_view bytes, TerminalDeadline deadline) {
    auto accepted = session_.submit(std::string(bytes));

    if (!accepted) {
      return accepted;
    }

    return finishOutput(deadline);
  }

  TerminalSession session_;
  std::size_t inputLimit_;
  std::size_t outputLimit_;
  std::chrono::milliseconds timeout_;
  TerminalRead startupInput_;
  std::string restoreBytes_;
  bool mouseReporting_{}, restoreNeeded_{}, closed_{};
};
} // namespace

std::expected<std::unique_ptr<ApplicationTerminal>, TerminalError>
openNativeApplicationTerminal(NativeTerminalHandles handles, TerminalOptions options,
                              std::chrono::milliseconds modeTimeout, bool mouseReporting) {
  if (modeTimeout.count() <= 0 || modeTimeout > MAX_MODE_TIMEOUT ||
      options.max_input_bytes > std::string{}.max_size() - MAX_QUERY_REPLY_BYTES) {
    return std::unexpected(TerminalError{TerminalErrorKind::INVALID_OPTIONS});
  }

  auto session = TerminalSession::open(handles, options);

  if (!session) {
    return std::unexpected(session.error());
  }

  auto terminal = std::make_unique<NativeTerminal>(std::move(*session), options, modeTimeout, mouseReporting);
  auto started = terminal->start();

  if (!started) {
    auto error = started.error();
    auto cleanup = terminal->close();

    if (!cleanup) {
      error.restoration_code = cleanup.error().native_code;
    }

    return std::unexpected(error);
  }

  return terminal;
}
} // namespace portals
