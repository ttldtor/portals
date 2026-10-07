// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/terminal.hpp>

#include "posix_io.hpp"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <limits>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace portals {
namespace {
TerminalError system_error(int code = errno) {
  return {TerminalErrorKind::system, code, 0};
}

TerminalError closed_error() {
  return {TerminalErrorKind::closed, 0, 0};
}

struct DescriptorState {
  int descriptor{-1};
  int flags{};
  termios modes{};
  bool flags_changed{};
  bool modes_changed{};

  int restore() noexcept {
    int error = 0;

    // Every restoration is attempted, even if another one already failed.
    if (flags_changed && fcntl(descriptor, F_SETFL, flags) < 0) {
      error = errno;
    }

    if (modes_changed && tcsetattr(descriptor, TCSANOW, &modes) < 0 && error == 0) {
      error = errno;
    }

    if (descriptor >= 0 && ::close(descriptor) < 0 && error == 0) {
      error = errno;
    }

    descriptor = -1;
    flags_changed = false;
    modes_changed = false;

    return error;
  }
};

std::expected<CellSize, TerminalError> dimensions(int descriptor) noexcept {
  winsize size{};

  if (ioctl(descriptor, TIOCGWINSZ, &size) < 0) {
    return std::unexpected(system_error());
  }

  return CellSize{size.ws_col, size.ws_row};
}

bool snapshot(DescriptorState &state, int descriptor) {
  state.descriptor = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);

  if (state.descriptor < 0 || tcgetattr(state.descriptor, &state.modes) < 0) {
    return false;
  }

  state.flags = fcntl(state.descriptor, F_GETFL);

  return state.flags >= 0;
}

bool configure(DescriptorState &state, termios modes) {
  // Mark before changing: even a reported native failure may warrant rollback.
  state.modes_changed = true;

  if (tcsetattr(state.descriptor, TCSANOW, &modes) < 0) {
    return false;
  }

  state.flags_changed = true;

  return fcntl(state.descriptor, F_SETFL, state.flags | O_NONBLOCK) == 0;
}

bool valid_options(NativeTerminalHandles handles, TerminalOptions options) {
  return handles.input >= 0 && handles.input <= INT_MAX && handles.output >= 0 && handles.output <= INT_MAX &&
         options.max_input_bytes > 0 && options.max_input_events > 0 && options.max_output_bytes > 0 &&
         options.max_input_bytes <= static_cast<std::size_t>(SSIZE_MAX) &&
         options.max_output_bytes <= static_cast<std::size_t>(SSIZE_MAX) && options.wait_cadence.count() > 0 &&
         options.wait_cadence.count() <= INT_MAX;
}
} // namespace

struct TerminalSession::Impl {
  DescriptorState input;
  DescriptorState output;
  TerminalOptions options;
  CellSize last_size;
  std::string pending;
  std::size_t offset{};

  int restore() noexcept {
    const int input_error = input.restore();
    const int output_error = output.restore();

    return input_error != 0 ? input_error : output_error;
  }

  ~Impl() {
    restore();
  }
};

TerminalSession::TerminalSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {
}
TerminalSession::TerminalSession(TerminalSession &&) noexcept = default;
TerminalSession &TerminalSession::operator=(TerminalSession &&) noexcept = default;
TerminalSession::~TerminalSession() noexcept = default;

std::expected<TerminalSession, TerminalError> TerminalSession::open(NativeTerminalHandles handles,
                                                                    TerminalOptions options) {
  if (!valid_options(handles, options)) {
    return std::unexpected(TerminalError{TerminalErrorKind::invalid_options, 0, 0});
  }

  const auto input = static_cast<int>(handles.input);
  const auto output = static_cast<int>(handles.output);

  if (isatty(input) != 1 || isatty(output) != 1) {
    return std::unexpected(TerminalError{TerminalErrorKind::unsupported, errno, 0});
  }

  auto impl = std::make_unique<Impl>();
  impl->options = options;
  const auto fail = [&impl]() -> std::expected<TerminalSession, TerminalError> {
    auto error = system_error();
    error.restoration_code = impl->restore();

    return std::unexpected(error);
  };

  // Both snapshots precede all mutations: input/output often alias the same TTY.
  if (!snapshot(impl->input, input) || !snapshot(impl->output, output)) {
    return fail();
  }

  auto raw = impl->input.modes;
  raw.c_iflag &= static_cast<tcflag_t>(~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON));
  raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
  raw.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB));
  raw.c_cflag |= CS8;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  auto output_modes = impl->output.modes;
  output_modes.c_oflag &= static_cast<tcflag_t>(~OPOST);

  // Configure output first; a shared terminal must end in the full input raw mode.
  // Restore reverses the actual mutations below, independently for both aliases.
  if (!configure(impl->output, output_modes) || !configure(impl->input, raw)) {
    return fail();
  }

  auto size = dimensions(impl->output.descriptor);

  if (!size) {
    errno = size.error().native_code;

    return fail();
  }

  impl->last_size = *size;

  return TerminalSession(std::move(impl));
}

std::expected<CellSize, TerminalError> TerminalSession::size() const noexcept {
  if (!impl_) {
    return std::unexpected(closed_error());
  }

  return dimensions(impl_->output.descriptor);
}

TerminalRead TerminalSession::read(TerminalDeadline deadline, std::stop_token stop) {
  if (!impl_) {
    return {TerminalStatus::error, {}, {}, {}, closed_error()};
  }

  const auto &io = detail::native_posix_io();

  for (;;) {
    if (stop.stop_requested() || io.now() >= deadline) {
      return {stop.stop_requested() ? TerminalStatus::stopped : TerminalStatus::deadline, {}, {}, {}, {}};
    }

    auto current_size = size();

    if (!current_size) {
      // A disconnected PTY may reject TIOCGWINSZ while its input still has a
      // final readable prefix or EOF. Do not let geometry hide that transport state.
      pollfd hung_up{impl_->input.descriptor, POLLIN, 0};
      const int ready = io.poll(&hung_up, 1, 0);

      if (ready > 0 && (hung_up.revents & POLLHUP) != 0) {
        return detail::read_posix(impl_->input.descriptor, impl_->options.max_input_bytes, deadline, stop, io);
      }

      return {TerminalStatus::error, {}, {}, {}, current_size.error()};
    }

    if (*current_size != impl_->last_size) {
      impl_->last_size = *current_size;

      return {TerminalStatus::progress, {}, {}, *current_size, {}};
    }

    pollfd descriptor{impl_->input.descriptor, POLLIN, 0};
    const int waited = io.poll(&descriptor, 1, detail::poll_timeout(deadline, impl_->options.wait_cadence, io.now()));

    if (waited < 0) {
      if (errno == EINTR) {
        continue;
      }

      return {TerminalStatus::error, {}, {}, {}, system_error()};
    }

    if (waited == 0) {
      continue;
    }

    if ((descriptor.revents & POLLNVAL) != 0) {
      return {TerminalStatus::error, {}, {}, {}, system_error(EBADF)};
    }

    // POLLIN and POLLHUP may coexist: deliver buffered bytes before EOF.
    auto result = detail::read_posix(impl_->input.descriptor, impl_->options.max_input_bytes, deadline, stop, io);

    if (result.error && (result.error->native_code == EAGAIN || result.error->native_code == EWOULDBLOCK)) {
      continue;
    }

    return result;
  }
}

std::expected<void, TerminalError> TerminalSession::submit(std::string bytes) {
  if (!impl_) {
    return std::unexpected(closed_error());
  }

  if (pending_bytes() != 0) {
    return std::unexpected(TerminalError{TerminalErrorKind::output_pending, 0, 0});
  }

  if (bytes.size() > impl_->options.max_output_bytes) {
    return std::unexpected(TerminalError{TerminalErrorKind::output_limit, 0, 0});
  }

  // Do not retain an arbitrarily large reserved capacity from the caller.
  std::string compact(bytes.data(), bytes.size());
  impl_->pending.swap(compact);
  impl_->offset = 0;

  return {};
}

TerminalWrite TerminalSession::flush(TerminalDeadline deadline, std::stop_token stop) {
  if (!impl_) {
    return {TerminalStatus::error, 0, 0, closed_error()};
  }

  auto result = detail::flush_posix(impl_->output.descriptor, impl_->pending, impl_->offset, deadline, stop,
                                    impl_->options.wait_cadence, detail::native_posix_io());

  if (result.pending == 0) {
    impl_->pending.clear();
    impl_->offset = 0;
  }

  return result;
}

std::size_t TerminalSession::pending_bytes() const noexcept {
  return impl_ ? impl_->pending.size() - impl_->offset : 0;
}

std::expected<void, TerminalError> TerminalSession::close() noexcept {
  if (!impl_) {
    return {};
  }

  const int error = impl_->restore();
  impl_.reset();

  if (error != 0) {
    return std::unexpected(system_error(error));
  }

  return {};
}

} // namespace portals
