// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/application.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <limits>
#include <string_view>
#include <utility>

namespace portals {
namespace {
constexpr std::string_view SAVE_CURSOR = "\x1b"
                                         "7";
constexpr std::string_view ENTER_SCREEN = "\x1b[?1049h";
#ifdef _WIN32
constexpr std::string_view SAVE_VISIBILITY = "";
constexpr std::string_view LEAVE_SCREEN = "\x1b[0m\x1b[?1049l";
#else
constexpr std::string_view SAVE_VISIBILITY = "\x1b[?25s";
constexpr std::string_view LEAVE_SCREEN = "\x1b[0m\x1b[?1049l\x1b[?25r";
#endif
constexpr std::string_view CANCEL_SEQUENCE = "\x18";
constexpr std::string_view SAVE_TRACKING = "\x1b[?9;1000;1001;1002;1003s";
constexpr std::string_view RESTORE_TRACKING = "\x1b[?9;1000;1001;1002;1003r";
constexpr std::string_view ENABLE_MOUSE = "\x1b[?1005;1015;1016l\x1b[?1000;1006h";
constexpr std::string_view ENABLE_BUTTON_MOTION = "\x1b[?1005;1015;1016l\x1b[?1002;1006h";
constexpr std::array ENCODING_MODES{1005, 1006, 1015, 1016};
constexpr std::array TRACKING_MODES{9, 1000, 1001, 1002, 1003};
constexpr std::array MOTION_QUERY_MODES{1005, 1006, 1015, 1016, 1002};
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

// Correlation is process-local and never wraps. A declared FIFO host must reply to
// each unknown private mode with state zero after all earlier complete input packets.
std::optional<std::uint32_t> nextBarrierMode() noexcept {
  constexpr std::uint64_t FIRST_MODE = 1000000000;
  static std::atomic<std::uint64_t> next{FIRST_MODE};
  auto value = next.load(std::memory_order_relaxed);

  while (value <= std::numeric_limits<std::uint32_t>::max()) {
    if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
      return static_cast<std::uint32_t>(value);
    }
  }

  return {};
}

std::string modeQuery(std::uint32_t mode) {
  std::string result{"\x1b[?"};

  result += std::to_string(mode);
  result += "$p";

  return std::string(result);
}

std::string verificationQueries(std::uint32_t nonce) {
  std::string result{""};

  for (const auto mode : TRACKING_MODES) {
    result += modeQuery(static_cast<std::uint32_t>(mode));
  }

  result += modeQuery(1006);
  result += modeQuery(1016);
  result += modeQuery(nonce);

  return std::string(result);
}

constexpr std::string_view QUIESCE_MOUSE = "\x1b[?9;1000;1001;1002;1003l";
constexpr std::string_view ENABLE_PIXEL_MOUSE = "\x1b[?1005;1006;1015l\x1b[?1016h\x1b[?1000h";
constexpr std::string_view ENABLE_PIXEL_MOTION = "\x1b[?1005;1006;1015l\x1b[?1016h\x1b[?1002h";

} // namespace

namespace detail {
class NativeTerminal final : public ApplicationTerminal {
public:
  NativeTerminal(TerminalSession session, TerminalOptions options, std::chrono::milliseconds timeout, bool mouse,
                 NativeScreenMode screen, bool buttonMotion, NativePointerOptions pointer)
      : session_(std::move(session)), inputLimit_(options.max_input_bytes), outputLimit_(options.max_output_bytes),
        timeout_(timeout), pointer_(pointer), mouseReporting_(mouse), buttonMotion_(mouse && buttonMotion),
        screen_(screen) {
    InputOptions filterOptions;

    filterOptions.terminal_reports = true;
    startupDecoder_ = InputDecoder::create(filterOptions);

    if (screen_ == NativeScreenMode::ALTERNATE_SCREEN) {
      screenCleanup_ = std::string(CANCEL_SEQUENCE) + std::string(LEAVE_SCREEN);
      screenSave_ = std::string(SAVE_VISIBILITY) + std::string(SAVE_CURSOR);
      captureCleanup_ = std::string(CANCEL_SEQUENCE);
    }
  }

  ~NativeTerminal() noexcept override {
    (void)close();
  }

  PointerCaptureCapabilities pointerCaptureCapabilities() const noexcept override {
    return started_ && !closed_ && buttonMotion_ ? PointerCaptureCapabilities{true, true, true}
                                                 : PointerCaptureCapabilities{};
  }

  std::expected<void, TerminalError> start() {
    if (screen_ == NativeScreenMode::ALTERNATE_SCREEN) {
      auto saved = session_.saveCursorVisibility();

      if (!saved) {
        return saved;
      }
    }

    auto mouse = startMouse();

    if (mouse && startupInput_.bytes.size() + startupAfter_.bytes.size() > inputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
    }

    if (!mouse || screen_ == NativeScreenMode::MAIN_SCREEN) {
      started_ = mouse.has_value();

      return mouse;
    }

    const auto deadline = deadlineAfter(timeout_);

    if (!screenSave_.empty()) {
      // Confirm cursor/rendition capture separately: reset 1049 may restore this cache even on the main screen.
      captureStarted_ = true;

      auto captured = send(screenSave_, deadline);

      if (!captured) {
        return captured;
      }
    }

    // Arm rollback before submission: a failed write may already have entered the alternate screen.
    screenRestoreNeeded_ = true;

    auto entered = send(ENTER_SCREEN, deadline);

    started_ = entered.has_value();

    return entered;
  }

  std::expected<void, TerminalError> startMouse() {
#ifndef _WIN32
    if (!mouseReporting_) {
      return {};
    }

    const auto deadline = deadlineAfter(timeout_);
    auto queried = buttonMotion_ ? queryModes(deadline, MOTION_QUERY_MODES) : queryModes(deadline, ENCODING_MODES);

    if (!queried) {
      return queried;
    }

    if (pointer_.policy == NativePointerPolicy::AUTO && pointer_.ordering == NativeInputOrder::FIFO && initialPixel_) {
      // The retained prefix predates our cell setup, so its pixel dimensions and
      // generation are unknown. Reject before encoding/tracking mutation.
      return std::unexpected(
        TerminalError{TerminalErrorKind::INPUT_PROTOCOL, 0, 0, TerminalProtocolIssue::INITIAL_PIXEL_CONTEXT_UNKNOWN});
    }

    // xterm 390 restores encoding with an 8-bit Boolean cast (DoRM), truncating
    // the saved numeric encoding. Use queried values; tracking uses integer DoRM0.
    restoreBytes_ += RESTORE_TRACKING;

    // Reject before changing modes if the mandatory cleanup batch cannot fit.
    if (CANCEL_SEQUENCE.size() + restoreBytes_.size() + screenCleanup_.size() > outputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
    }

    mouseCleanup_ = std::string(CANCEL_SEQUENCE) + restoreBytes_;
    combinedCleanup_ = mouseCleanup_ + (screenCleanup_.empty() ? std::string{} : screenCleanup_.substr(1));

    auto saved = send(SAVE_TRACKING, deadline);

    if (!saved) {
      return saved;
    }

    // Arm cleanup before allocating submission storage or partially changing modes.
    restoreNeeded_ = true;

    auto enabled = send(buttonMotion_ ? ENABLE_BUTTON_MOTION : ENABLE_MOUSE, deadline);

    if (!enabled) {
      return enabled;
    }

    return startPrecision(deadline);
#else
    return {};
#endif
  }

  bool terminalReports() const noexcept override {
    return preciseSession_;
  }

  std::expected<void, TerminalError> protocol(const InputProtocolEvent &report) override {
    const auto *mode = std::get_if<InputModeReport>(&report);

    if (mode && transition_ == Transition::WAIT_QUIESCE && barrier_ && mode->mode == *barrier_ && mode->state == 0) {
      // Only request output here. States are collected after this fresh boundary
      // and only after the verification queries have been fully written.
      auto output = verificationQueries(*verificationBarrier_);

      protocolOutput_.swap(output);
      resetVerification();
      transition_ = Transition::VERIFY;
    } else if (mode && transition_ == Transition::WAIT_VERIFY) {
      recordVerification(*mode);

      if (verificationBarrier_ && mode->mode == *verificationBarrier_ && mode->state == 0) {
        if (!quiescenceConfirmed(true)) {
          return std::unexpected(TerminalError{TerminalErrorKind::INPUT_PROTOCOL, EIO});
        }

        std::string output{buttonMotion_ ? ENABLE_BUTTON_MOTION : ENABLE_MOUSE};

        if (resizeOwned_) {
          output += "\x1b[?2048l";
        }

        protocolOutput_.swap(output);
        transition_ = Transition::ENABLE_CELL;
      }
    }

    return checkTransition();
  }

  std::expected<void, TerminalError> pointerMetrics(bool usable) override {
    if (!usable && pixel_ && transition_ == Transition::NONE) {
      const auto nonce = nextBarrierMode();
      const auto verification = nextBarrierMode();

      if (!nonce || !verification) {
        return std::unexpected(TerminalError{TerminalErrorKind::INPUT_PROTOCOL, EOVERFLOW});
      }

      std::string output{QUIESCE_MOUSE};

      output += modeQuery(*nonce);

      if (output.size() > outputLimit_) {
        return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
      }

      protocolOutput_.swap(output);
      barrier_ = nonce;
      verificationBarrier_ = verification;
      transitionDeadline_ = deadlineAfter(timeout_);
      transition_ = Transition::QUIESCE;
    }

    return checkTransition();
  }

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (closed_) {
      return {TerminalStatus::FAILED, {}, {}, {}, TerminalError{TerminalErrorKind::CLOSED}};
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      if (auto checked = checkTransition(); !checked) {
        return {TerminalStatus::FAILED, {}, {}, {}, checked.error()};
      }

      return {TerminalStatus::DEADLINE, {}, {}, {}, {}};
    }

    if (stop.stop_requested()) {
      return {TerminalStatus::STOPPED, {}, {}, {}, {}};
    }

    if (auto pumped = pumpProtocol(deadline, stop); !pumped) {
      return {TerminalStatus::FAILED, {}, {}, {}, pumped.error()};
    }

    if (!startupInput_.bytes.empty() || startupInput_.resized) {
      return std::exchange(startupInput_, {});
    }

    if (startupAfter_.coordinate_mode || !startupAfter_.bytes.empty() || startupAfter_.resized) {
      return std::exchange(startupAfter_, {});
    }

    if (coordinateNotice_) {
      TerminalRead result;

      result.coordinate_mode = std::exchange(coordinateNotice_, {});

      return result;
    }

    auto result =
      session_.read(transition_ == Transition::NONE ? deadline : std::min(deadline, transitionDeadline_), stop);

    if (auto checked = checkTransition(); !checked) {
      return {TerminalStatus::FAILED, {}, {}, {}, checked.error()};
    }

    return result;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return session_.size();
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    if (closed_) {
      return std::unexpected(TerminalError{TerminalErrorKind::CLOSED});
    }

    if (deferredFrame_ || framePending_) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_PENDING});
    }

    if (bytes.size() > outputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
    }

    if (transition_ != Transition::NONE || controlWriting_) {
      deferredFrame_.emplace(std::move(bytes));

      return {};
    }

    auto accepted = session_.submit(std::move(bytes));

    framePending_ = accepted.has_value();

    return accepted;
  }

  TerminalWrite flush(TerminalDeadline deadline, std::stop_token stop) override {
    if (closed_ && !closing_) {
      return {TerminalStatus::FAILED, 0, 0, TerminalError{TerminalErrorKind::CLOSED}};
    }

    TerminalWrite result;

    if (!framePending_ && stop.stop_requested()) {
      return {TerminalStatus::STOPPED, 0, deferredFrame_ ? deferredFrame_->size() : 0, {}};
    }

    if (framePending_) {
      result = session_.flush(deadline, stop);
      framePending_ = result.pending != 0;

      if (result.error || framePending_ || result.status != TerminalStatus::PROGRESS) {
        return result;
      }
    }

    if (auto pumped = pumpProtocol(deadline, stop); !pumped) {
      return {TerminalStatus::FAILED, result.completed, deferredFrame_ ? deferredFrame_->size() : 0, pumped.error()};
    }

    if (!stop.stop_requested() && transition_ == Transition::NONE && !controlWriting_ && deferredFrame_) {
      auto accepted = session_.submit(std::move(*deferredFrame_));

      if (!accepted) {
        return {TerminalStatus::FAILED, result.completed, deferredFrame_->size(), accepted.error()};
      }

      deferredFrame_.reset();
      framePending_ = true;
      auto frame = session_.flush(deadline, stop);

      frame.completed += result.completed;
      framePending_ = frame.pending != 0;

      return frame;
    }

    result.pending = deferredFrame_ ? deferredFrame_->size() : 0;
    result.status = stop.stop_requested() ? TerminalStatus::STOPPED
                    : result.pending      ? TerminalStatus::DEADLINE
                                          : TerminalStatus::PROGRESS;

    return result;
  }

  std::expected<void, TerminalError> close() noexcept override {
    if (closed_) {
      return {};
    }

    closed_ = closing_ = true;
    std::optional<TerminalError> failure;

    const auto deadline = deadlineAfter(timeout_);
    std::string cleanup;

    if (restoreNeeded_ || screenRestoreNeeded_ || captureStarted_) {
      if (started_) {
        try {
          auto progress = flush(deadline, {});
          auto drained = progress.error ? std::expected<void, TerminalError>(std::unexpected(*progress.error))
                                        : std::expected<void, TerminalError>{};

          if (!progress.error && (progress.pending != 0 || framePending_ || controlWriting_)) {
            drained = std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, ETIMEDOUT});
          }

          if (!drained) {
            failure = drained.error();
          }
        } catch (...) {
          failure = TerminalError{TerminalErrorKind::SYSTEM, EIO};
        }
      }

      // Never complete a failed startup sequence while trying to roll it back.

      if (restoreNeeded_ && screenRestoreNeeded_) {
        cleanup = std::move(combinedCleanup_);
      } else if (restoreNeeded_) {
        cleanup = std::move(mouseCleanup_);
      } else if (screenRestoreNeeded_) {
        cleanup = std::move(screenCleanup_);
      } else {
        cleanup = std::move(captureCleanup_);
      }
    }

    if (!cleanup.empty()) {
      // Joining uncertain native I/O may outlive the drain deadline. Start the
      // mandatory restoration budget only after no worker borrows the old batch.
      auto joined = session_.joinOutputWriter();

      if (!joined) {
        if (failure) {
          failure->restoration_code = joined.error().native_code;
        } else {
          failure = joined.error();
        }
      }
    }

    auto closed =
      cleanup.empty() ? session_.close() : session_.closeWithOutput(std::move(cleanup), deadlineAfter(timeout_));

    if (!closed) {
      if (failure) {
        failure->restoration_code = closed.error().native_code;
      } else {
        failure = closed.error();
      }
    }

    closing_ = false;

    if (failure) {
      return std::unexpected(*failure);
    }

    return {};
  }

private:
  template <std::size_t N>
  std::expected<void, TerminalError> queryModes(TerminalDeadline deadline, const std::array<int, N> &modes) {
    std::string queries;

    for (const int mode : modes) {
      queries += "\x1b[?" + std::to_string(mode) + "$p";
    }

    auto sent = send(queries, deadline);

    if (!sent) {
      return sent;
    }

    std::array<int, N> states;

    states.fill(-1);

    while (!std::ranges::all_of(states, [](int state) {
      return state >= 0;
    })) {
      auto input = session_.read(deadline);

      if (input.error) {
        return std::unexpected(*input.error);
      }

      if (input.status != TerminalStatus::PROGRESS) {
        return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, ETIMEDOUT});
      }

      auto accepted = collectStartup(input, [&](const InputProtocolEvent &report) {
        if (const auto *mode = std::get_if<InputModeReport>(&report)) {
          for (std::size_t index = 0; index < modes.size(); ++index) {
            if (mode->mode == static_cast<std::uint32_t>(modes[index])) {
              states[index] = static_cast<int>(mode->state);

              return true;
            }
          }
        }

        return false;
      });

      if (!accepted) {
        return accepted;
      }
    }

    if (startupInput_.bytes.size() > inputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
    }

    initialPixel_ = states[3] == MODE_SET;
    pixelSupported_ = (states[3] == MODE_SET || states[3] == MODE_RESET) && states[1] != MODE_PERMANENTLY_SET;

    const auto sgr = states[1];

    if ((sgr != MODE_SET && sgr != MODE_RESET && sgr != MODE_PERMANENTLY_SET) || states[0] == MODE_PERMANENTLY_SET ||
        states[2] == MODE_PERMANENTLY_SET || states[3] == MODE_PERMANENTLY_SET) {
      return std::unexpected(TerminalError{TerminalErrorKind::UNSUPPORTED});
    }

    if constexpr (N == MOTION_QUERY_MODES.size()) {
      const auto motion = states.back();
      if (motion != MODE_SET && motion != MODE_RESET && motion != MODE_PERMANENTLY_SET) {
        return std::unexpected(TerminalError{TerminalErrorKind::UNSUPPORTED});
      }
    }

    // Encoding restoration uses only its original four queried values. Tracking,
    // including 1002, belongs exclusively to the existing XTSAVE/XTRESTORE batch.
    for (std::size_t index = 0; index < ENCODING_MODES.size(); ++index) {
      if (states[index] == MODE_SET || states[index] == MODE_RESET) {
        restoreBytes_ += "\x1b[?" + std::to_string(ENCODING_MODES[index]) + "l";
      }
    }

    for (std::size_t index = 0; index < ENCODING_MODES.size(); ++index) {
      if (states[index] == MODE_SET) {
        restoreBytes_ += "\x1b[?" + std::to_string(ENCODING_MODES[index]) + "h";
      }
    }

    return {};
  }

  enum class Transition {
    NONE,
    QUIESCE,
    WAIT_QUIESCE,
    VERIFY,
    WAIT_VERIFY,
    ENABLE_CELL
  };

  void resetVerification() noexcept {
    trackingStates_.fill(-1);
    sgrState_ = pixelState_ = -1;
  }

  bool recordVerification(const InputModeReport &report) noexcept {
    for (std::size_t index = 0; index < TRACKING_MODES.size(); ++index) {
      if (report.mode == static_cast<std::uint32_t>(TRACKING_MODES[index])) {
        trackingStates_[index] = static_cast<int>(report.state);

        return true;
      }
    }

    if (report.mode == 1006) {
      sgrState_ = static_cast<int>(report.state);

      return true;
    }

    if (report.mode == 1016) {
      pixelState_ = static_cast<int>(report.state);

      return true;
    }

    return false;
  }

  bool quiescenceConfirmed(bool pixels) const noexcept {
    const auto selected = buttonMotion_ ? 1002 : 1000;

    for (std::size_t index = 0; index < TRACKING_MODES.size(); ++index) {
      const auto state = trackingStates_[index];

      if ((state != 0 && state != MODE_RESET && state != 4) ||
          (TRACKING_MODES[index] == selected && state != MODE_RESET)) {
        return false;
      }
    }

    // Both encodings must remain mutable and consistent with the old stream.
    return sgrState_ == (pixels ? MODE_RESET : MODE_SET) && pixelState_ == (pixels ? MODE_SET : MODE_RESET);
  }

  std::expected<void, TerminalError> checkTransition() const {
    if (transition_ != Transition::NONE && std::chrono::steady_clock::now() >= transitionDeadline_) {
      return std::unexpected(TerminalError{TerminalErrorKind::INPUT_PROTOCOL, ETIMEDOUT});
    }

    return {};
  }

  std::expected<void, TerminalError> pumpProtocol(TerminalDeadline deadline, std::stop_token stop) {
    if (auto checked = checkTransition(); !checked) {
      return checked;
    }

    if (stop.stop_requested() || framePending_ || transition_ == Transition::NONE ||
        transition_ == Transition::WAIT_QUIESCE || transition_ == Transition::WAIT_VERIFY) {
      return {};
    }

    if (!controlWriting_) {
      auto accepted = session_.submit(std::move(protocolOutput_));

      if (!accepted) {
        return accepted;
      }

      controlWriting_ = true;
    }

    const auto result = session_.flush(std::min(deadline, transitionDeadline_), stop);

    if (result.error) {
      return std::unexpected(*result.error);
    }

    if (result.pending == 0 && result.status == TerminalStatus::PROGRESS) {
      controlWriting_ = false;

      if (transition_ == Transition::QUIESCE) {
        transition_ = Transition::WAIT_QUIESCE;
      } else if (transition_ == Transition::VERIFY) {
        transition_ = Transition::WAIT_VERIFY;
      } else {
        transition_ = Transition::NONE;
        pixel_ = false;
        coordinateNotice_ = InputCoordinateMode::CELL;
      }
    }

    return checkTransition();
  }

  template <class Report>
  std::expected<void, TerminalError> collectStartup(const TerminalRead &input, Report &&report) {
    if (input.resized) {
      startupInput_.resized = input.resized;
    }

    for (std::size_t offset = 0; offset < input.bytes.size();) {
      auto &target = precisionBoundary_ ? startupAfter_.bytes : startupInput_.bytes;
      const auto step = startupDecoder_->next(std::string_view(input.bytes).substr(offset, 1), 0);

      if (!step) {
        return std::unexpected(TerminalError{TerminalErrorKind::INPUT_DECODE});
      }

      if (step->consumed) {
        target += input.bytes[offset++];
      }

      if (step->event) {
        if (const auto *issue = std::get_if<InputIssue>(&*step->event); issue && issue->invalidates_pointer_metrics) {
          startupSize_.reset();
        }
      }

      if (step->protocol) {
        if (const auto *size = std::get_if<InputSizeReport>(&*step->protocol)) {
          startupSize_ = *size;
        }

        if (report(*step->protocol)) {
          // Only a report emitted by the paste-aware parser can be removed. Its
          // numeric grammar contains no ESC, so the last ESC begins this exact report.
          const auto begin = target.rfind('\x1b');

          if (begin != std::string::npos) {
            target.erase(begin);
          }
        }
      }

      if (startupInput_.bytes.size() + startupAfter_.bytes.size() > inputLimit_ + 64) {
        return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
      }
    }

    return {};
  }

  template <class Report> std::expected<bool, TerminalError> waitOptional(TerminalDeadline deadline, Report &&report) {
    while (std::chrono::steady_clock::now() < deadline) {
      auto input = session_.read(deadline);

      if (input.error) {
        return std::unexpected(*input.error);
      }

      if (input.status == TerminalStatus::DEADLINE) {
        return false;
      }

      if (input.status != TerminalStatus::PROGRESS) {
        return std::unexpected(TerminalError{TerminalErrorKind::INPUT_PROTOCOL, EIO});
      }

      bool done{};
      auto accepted = collectStartup(input, [&](const InputProtocolEvent &value) {
        const auto matched = report(value);

        done = done || (matched && *matched);

        return matched.has_value() && std::holds_alternative<InputModeReport>(value);
      });

      if (!accepted) {
        return std::unexpected(accepted.error());
      }

      if (done) {
        return true;
      }
    }

    return false;
  }

  void refreshCleanup() {
    mouseCleanup_ = std::string(CANCEL_SEQUENCE) + restoreBytes_;
    combinedCleanup_ = mouseCleanup_ + (screenCleanup_.empty() ? std::string{} : screenCleanup_.substr(1));
  }

  bool usableStartupMetrics() const {
    const auto viewport = session_.size();

    return startupSize_ && startupSize_->text_width_px && startupSize_->text_height_px && viewport &&
           startupSize_->columns == static_cast<std::uint32_t>(viewport->width) &&
           startupSize_->rows == static_cast<std::uint32_t>(viewport->height);
  }

  std::expected<void, TerminalError> startPrecision(TerminalDeadline setupDeadline) {
#ifndef _WIN32
    if (pointer_.policy != NativePointerPolicy::AUTO || pointer_.ordering != NativeInputOrder::FIFO ||
        !pixelSupported_) {
      return {};
    }

    if (verificationQueries(std::numeric_limits<std::uint32_t>::max()).size() > outputLimit_) {
      return {};
    }

    const auto now = std::chrono::steady_clock::now();
    const auto remaining = setupDeadline - now;
    const auto budget =
      std::min(std::chrono::duration_cast<TerminalDeadline::duration>(pointer_.probeTimeout), remaining / 4);

    if (budget <= TerminalDeadline::duration::zero()) {
      return {};
    }

    const auto optionalDeadline = now + budget;
    preciseSession_ = true;
    auto sent = send(modeQuery(2048), optionalDeadline);

    if (!sent) {
      return sent;
    }

    std::optional<std::uint32_t> resizeState;
    auto queried = waitOptional(optionalDeadline, [&](const InputProtocolEvent &report) -> std::optional<bool> {
      if (const auto *mode = std::get_if<InputModeReport>(&report); mode && mode->mode == 2048) {
        resizeState = mode->state;

        return true;
      }

      return std::nullopt;
    });

    if (!queried) {
      return std::unexpected(queried.error());
    }

    if (!*queried || !resizeState || (*resizeState != 1 && *resizeState != 2 && *resizeState != 3)) {
      return {};
    }

    // Complete bounded cleanup storage before any optional mode mutation.
    if (*resizeState == 2) {
      restoreBytes_ = "\x1b[?2048l" + restoreBytes_;

      if (CANCEL_SEQUENCE.size() + restoreBytes_.size() + screenCleanup_.size() > outputLimit_) {
        return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
      }

      refreshCleanup();
      resizeOwned_ = true;
    }

    sent = send("\x1b[?2048h", optionalDeadline);

    if (!sent) {
      return sent;
    }

    auto sized = waitOptional(optionalDeadline, [](const InputProtocolEvent &report) -> std::optional<bool> {
      return std::holds_alternative<InputSizeReport>(report) ? std::optional<bool>{true} : std::nullopt;
    });

    if (!sized) {
      return std::unexpected(sized.error());
    }

    const bool usable = *sized && usableStartupMetrics();

    if (!usable) {
      return resizeOwned_ ? send("\x1b[?2048l", setupDeadline) : std::expected<void, TerminalError>{};
    }

    const auto first = nextBarrierMode();
    const auto second = nextBarrierMode();

    if (!first || !second) {
      return resizeOwned_ ? send("\x1b[?2048l", setupDeadline) : std::expected<void, TerminalError>{};
    }

    std::string quiesce{QUIESCE_MOUSE};

    quiesce += modeQuery(*first);
    sent = send(quiesce, optionalDeadline);

    if (!sent) {
      return sent;
    }

    auto drained = waitOptional(optionalDeadline, [&](const InputProtocolEvent &report) -> std::optional<bool> {
      const auto *mode = std::get_if<InputModeReport>(&report);

      return mode && mode->mode == *first && mode->state == 0 ? std::optional<bool>{true} : std::nullopt;
    });

    if (!drained) {
      return std::unexpected(drained.error());
    }

    bool confirmed{};

    if (*drained) {
      // The first nonce drains stale state replies. Only queries issued after it
      // may establish quiescence, and the second fresh nonce closes that window.
      resetVerification();
      sent = send(verificationQueries(*second), optionalDeadline);

      if (!sent) {
        return sent;
      }

      bool completed{};
      auto checked = waitOptional(optionalDeadline, [&](const InputProtocolEvent &report) -> std::optional<bool> {
        const auto *mode = std::get_if<InputModeReport>(&report);

        if (!mode || completed) {
          return std::nullopt;
        }

        if (recordVerification(*mode)) {
          return false;
        }

        if (mode->mode == *second && mode->state == 0) {
          completed = true;
          confirmed = quiescenceConfirmed(false);
          precisionBoundary_ = confirmed;

          return true;
        }

        return std::nullopt;
      });

      if (!checked) {
        return std::unexpected(checked.error());
      }

      confirmed = confirmed && *checked;
    }

    // A resize/invalid report can arrive during either barrier. Revalidate the
    // latest candidate before changing wire units; optional loss still means cells.
    confirmed = confirmed && usableStartupMetrics();

    if (!confirmed) {
      return send(std::string(buttonMotion_ ? ENABLE_BUTTON_MOTION : ENABLE_MOUSE) +
                    (resizeOwned_ ? "\x1b[?2048l" : ""),
                  setupDeadline);
    }

    sent = send(buttonMotion_ ? ENABLE_PIXEL_MOTION : ENABLE_PIXEL_MOUSE, setupDeadline);

    if (!sent) {
      return sent;
    }

    pixel_ = preciseSession_ = true;
    startupAfter_.coordinate_mode = InputCoordinateMode::PIXEL;

    if (startupInput_.bytes.size() + startupAfter_.bytes.size() > inputLimit_) {
      return std::unexpected(TerminalError{TerminalErrorKind::INPUT_LIMIT});
    }
#else
    (void)setupDeadline;
#endif
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
  // Explicit fallible empty constructors avoid MSVC Debug container-proxy
  // allocation through implicit noexcept default construction during acquisition.
  TerminalRead startupInput_{TerminalStatus::PROGRESS, std::string(""), std::vector<InputEvent>(0), {}, {}, {}};
  TerminalRead startupAfter_{TerminalStatus::PROGRESS, std::string(""), std::vector<InputEvent>(0), {}, {}, {}};
  std::expected<InputDecoder, InputError> startupDecoder_{std::unexpected(InputError::INVALID_OPTIONS)};
  NativePointerOptions pointer_;
  std::optional<InputSizeReport> startupSize_;
  std::optional<std::uint32_t> barrier_, verificationBarrier_;
  std::array<int, TRACKING_MODES.size()> trackingStates_{};
  int sgrState_{-1}, pixelState_{-1};
  std::optional<InputCoordinateMode> coordinateNotice_;
  std::optional<std::string> deferredFrame_;
  std::string protocolOutput_{""};
  TerminalDeadline transitionDeadline_{};
  Transition transition_{Transition::NONE};
  bool initialPixel_{}, pixelSupported_{}, resizeOwned_{}, preciseSession_{}, pixel_{}, precisionBoundary_{},
    controlWriting_{}, framePending_{};
  std::string restoreBytes_, mouseCleanup_, screenCleanup_, combinedCleanup_, screenSave_, captureCleanup_;
  bool mouseReporting_{}, buttonMotion_{}, restoreNeeded_{}, captureStarted_{}, screenRestoreNeeded_{}, started_{},
    closed_{}, closing_{};
  NativeScreenMode screen_;
};
} // namespace detail

std::expected<std::unique_ptr<ApplicationTerminal>, TerminalError>
openNativeApplicationTerminal(NativeTerminalHandles handles, TerminalOptions options,
                              std::chrono::milliseconds modeTimeout, bool mouseReporting, NativeScreenMode screen) {
  return openNativeApplicationTerminal(handles, options, modeTimeout, mouseReporting, screen, false);
}

std::expected<std::unique_ptr<ApplicationTerminal>, TerminalError>
openNativeApplicationTerminal(NativeTerminalHandles handles, TerminalOptions options,
                              std::chrono::milliseconds modeTimeout, bool mouseReporting, NativeScreenMode screen,
                              bool buttonMotion) {
  return openNativeApplicationTerminal(handles, options, modeTimeout, mouseReporting, screen, buttonMotion,
                                       NativePointerOptions{NativePointerPolicy::OFF});
}

std::expected<std::unique_ptr<ApplicationTerminal>, TerminalError>
openNativeApplicationTerminal(NativeTerminalHandles handles, TerminalOptions options,
                              std::chrono::milliseconds modeTimeout, bool mouseReporting, NativeScreenMode screen,
                              bool buttonMotion, NativePointerOptions pointer) {
  if (modeTimeout.count() <= 0 || modeTimeout > MAX_MODE_TIMEOUT ||
      options.max_input_bytes > std::string{}.max_size() - 64 || pointer.probeTimeout.count() <= 0 ||
      pointer.probeTimeout > MAX_MODE_TIMEOUT ||
      (pointer.policy != NativePointerPolicy::AUTO && pointer.policy != NativePointerPolicy::OFF) ||
      (pointer.ordering != NativeInputOrder::UNCONFIRMED && pointer.ordering != NativeInputOrder::FIFO)) {
    return std::unexpected(TerminalError{TerminalErrorKind::INVALID_OPTIONS});
  }

  if (screen != NativeScreenMode::MAIN_SCREEN && screen != NativeScreenMode::ALTERNATE_SCREEN) {
    return std::unexpected(TerminalError{TerminalErrorKind::INVALID_OPTIONS});
  }

  if (screen == NativeScreenMode::ALTERNATE_SCREEN &&
      (CANCEL_SEQUENCE.size() + LEAVE_SCREEN.size() > options.max_output_bytes ||
       SAVE_VISIBILITY.size() + SAVE_CURSOR.size() > options.max_output_bytes ||
       ENTER_SCREEN.size() > options.max_output_bytes)) {
    return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT});
  }

  auto session = TerminalSession::open(handles, options);

  if (!session) {
    return std::unexpected(session.error());
  }

  auto terminal = std::make_unique<detail::NativeTerminal>(std::move(*session), options, modeTimeout, mouseReporting,
                                                           screen, buttonMotion, pointer);
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
