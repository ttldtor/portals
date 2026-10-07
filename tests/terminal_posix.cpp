// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "posix_io.hpp"
#include <portals/terminal.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

#ifdef PORTALS_TEST_WRAP_TERMIOS
int termios_calls = 0;
int fail_termios_call = 0;
int second_fail_termios_call = 0;
extern "C" int __real_tcsetattr(int, int, const termios *);
extern "C" int __wrap_tcsetattr(int fd, int action, const termios *modes) {
  ++termios_calls;

  if (termios_calls == fail_termios_call || termios_calls == second_fail_termios_call) {
    errno = EIO;
    return -1;
  }
  return __real_tcsetattr(fd, action, modes);
}
#endif

namespace {
using namespace portals;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

class Descriptor {
public:
  explicit Descriptor(int value = -1) : value_(value) {
  }
  ~Descriptor() {
    reset();
  }
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  int get() const {
    return value_;
  }
  void reset() {
    if (value_ >= 0) {
      ::close(value_);
      value_ = -1;
    }
  }

private:
  int value_;
};

class Pty {
public:
  Pty() : master(posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK)), slave(open_slave()) {
    if (slave.get() < 0 || tcgetattr(slave.get(), &before) != 0) {
      throw std::runtime_error("PTY allocation/snapshot");
    }
    flags = fcntl(slave.get(), F_GETFL);
    winsize initial{25, 80, 0, 0};
    if (ioctl(slave.get(), TIOCSWINSZ, &initial) != 0) {
      throw std::runtime_error("PTY initial size");
    }
  }
  NativeTerminalHandles handles() const {
    return {slave.get(), slave.get()};
  }
  bool restored() const {
    termios after{};
    if (tcgetattr(slave.get(), &after) != 0) {
      return false;
    }
    return before.c_iflag == after.c_iflag && before.c_oflag == after.c_oflag && before.c_cflag == after.c_cflag &&
           before.c_lflag == after.c_lflag && cfgetispeed(&before) == cfgetispeed(&after) &&
           cfgetospeed(&before) == cfgetospeed(&after) &&
           std::equal(std::begin(before.c_cc), std::end(before.c_cc), std::begin(after.c_cc)) &&
           flags == fcntl(slave.get(), F_GETFL);
  }
  Descriptor master;
  Descriptor slave;
  termios before{};
  int flags{};

private:
  int open_slave() {
    if (master.get() < 0 || grantpt(master.get()) != 0 || unlockpt(master.get()) != 0) {
      return -1;
    }
    const char *name = ptsname(master.get());
    return name ? ::open(name, O_RDWR | O_NOCTTY) : -1;
  }
};

void sessions() {
  Pty pty;
  TerminalOptions options;
  options.max_input_bytes = 2;
  options.max_output_bytes = 8;
  options.wait_cadence = 2ms;
  {
    auto opened = TerminalSession::open(pty.handles(), options);
    check(opened.has_value(), "open shared input/output descriptor");
    if (!opened) {
      return;
    }
    auto session = std::move(*opened);
    check(!opened->size() && opened->read(Clock::now()).error->kind == TerminalErrorKind::closed,
          "moved-from session closed");
    termios raw{};
    tcgetattr(pty.slave.get(), &raw);
    check((raw.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)) == 0 && (raw.c_oflag & OPOST) == 0 &&
            (fcntl(pty.slave.get(), F_GETFL) & O_NONBLOCK) != 0,
          "raw and nonblocking modes");
    check(session.size().has_value() && *session.size() == CellSize{80, 25}, "initial cell size");
    check(::write(pty.master.get(), "abc", 3) == 3, "inject raw bytes");
    auto first = session.read(Clock::now() + 1s);
    auto second = session.read(Clock::now() + 1s);
    check(first.bytes == "ab" && second.bytes == "c" && first.events.empty(), "bounded owning raw input");
    winsize resized{31, 91, 0, 0};
    check(ioctl(pty.master.get(), TIOCSWINSZ, &resized) == 0, "resize PTY");
    auto resize = session.read(Clock::now() + 1s);
    check(resize.resized == CellSize{91, 31} && resize.status == TerminalStatus::progress, "owner resize event");
    const auto start = Clock::now();
    check(session.read(start + 5ms).status == TerminalStatus::deadline, "read deadline");
    check(Clock::now() - start < 500ms, "bounded empty-input wait");
    std::stop_source stopped;
    stopped.request_stop();
    check(session.read(Clock::now() + 1s, stopped.get_token()).status == TerminalStatus::stopped, "read cancellation");
    check(!session.submit("123456789") && session.pending_bytes() == 0, "bounded output rejection");
    check(session.submit("hello\n").has_value(), "submit owned output");
    check(!session.submit("x") && session.pending_bytes() == 6, "one pending batch only");
    auto timeout = session.flush(Clock::now());
    check(timeout.status == TerminalStatus::deadline && timeout.completed == 0 && timeout.pending == 6,
          "deadline preserves output");
    auto stop = session.flush(Clock::now() + 1s, stopped.get_token());
    check(stop.status == TerminalStatus::stopped && stop.pending == 6, "stop preserves output");
    auto written = session.flush(Clock::now() + 1s);
    check(written.status == TerminalStatus::progress && written.completed == 6 && written.pending == 0,
          "honest completed-byte count");
    char output[16]{};
    check(::read(pty.master.get(), output, sizeof(output)) == 6 && std::string_view(output, 6) == "hello\n",
          "no output postprocessing");
    check(session.submit("discard").has_value(), "pending output before close");
    check(session.close().has_value() && session.close().has_value(), "explicit close idempotent");
    check(::read(pty.master.get(), output, sizeof(output)) == -1 && errno == EAGAIN,
          "close discards pending output without flushing");
    check(!session.submit("x") && !session.size(), "closed calls rejected");
    check(first.bytes == "ab", "earlier owning input survives close");
  }
  check(pty.restored(), "all original modes/flags restored after shared-fd close");
  try {
    auto session = TerminalSession::open(pty.handles());
    if (!session) {
      throw std::runtime_error("open");
    }
    throw std::runtime_error("caught application error");
  } catch (const std::runtime_error &) {
  }
  check(pty.restored(), "RAII restores after caught exception");

  Pty other;
  {
    auto first = TerminalSession::open(pty.handles());
    auto second = TerminalSession::open(other.handles());
    check(first.has_value() && second.has_value(), "two independent terminals");
    if (first && second) {
      *first = std::move(*second);
      check(pty.restored(), "move assignment restores old owner");
    }
  }
  check(other.restored(), "move assignment owns replacement");

  TerminalOptions invalid;
  invalid.wait_cadence = 0ms;
  check(!TerminalSession::open(pty.handles(), invalid), "invalid cadence rejected before mutation");
  check(pty.restored(), "invalid open leaves modes alone");

#ifdef PORTALS_TEST_WRAP_TERMIOS
  termios_calls = 0;
  fail_termios_call = 2;
  auto failed = TerminalSession::open(pty.handles());
  fail_termios_call = 0;
  check(!failed && failed.error().native_code == EIO && failed.error().restoration_code == 0,
        "partial acquisition exposes original error");
  check(pty.restored() && termios_calls == 4, "partial acquisition restores both independent states");

  termios_calls = 0;
  fail_termios_call = 2;
  second_fail_termios_call = 3;
  failed = TerminalSession::open(pty.handles());
  fail_termios_call = 0;
  second_fail_termios_call = 0;
  check(!failed && failed.error().native_code == EIO && failed.error().restoration_code == EIO,
        "open distinguishes acquisition and rollback errors");
  check(pty.restored() && termios_calls == 4, "rollback failure still attempts the other shared state");

  auto session = TerminalSession::open({pty.slave.get(), other.slave.get()});
  termios_calls = 0;
  fail_termios_call = 1;
  auto closed = session->close();
  fail_termios_call = 0;
  check(!closed && closed.error().native_code == EIO, "explicit close reports restore error");
  check(termios_calls == 2 && other.restored() && fcntl(pty.slave.get(), F_GETFL) == pty.flags,
        "restore failure still restores other terminal and both descriptor flags");
  tcsetattr(pty.slave.get(), TCSANOW, &pty.before);
#endif
}

struct Script {
  TerminalDeadline current{};
  std::string written;
  int writes{};
  int polls{};
  int reads{};
  std::vector<int> timeouts;
} script;
TerminalDeadline scripted_now() {
  return script.current;
}
int scripted_poll(pollfd *, nfds_t, int timeout) {
  script.timeouts.push_back(timeout);
  ++script.polls;
  script.current += 3ms;
  errno = EINTR;
  return -1;
}
ssize_t scripted_write(int, const void *bytes, std::size_t count) {
  ++script.writes;
  if (script.writes == 1) {
    const auto size = std::min<std::size_t>(2, count);
    script.written.append(static_cast<const char *>(bytes), size);
    return static_cast<ssize_t>(size);
  }
  errno = script.writes == 2 ? EINTR : EAGAIN;
  return -1;
}
ssize_t success_write(int, const void *bytes, std::size_t count) {
  script.written.append(static_cast<const char *>(bytes), count);
  return static_cast<ssize_t>(count);
}
ssize_t scripted_read(int, void *bytes, std::size_t count) {
  if (++script.reads < 3) {
    script.current += 1ms;
    errno = EINTR;
    return -1;
  }
  if (count) {
    static_cast<char *>(bytes)[0] = 'x';
  }
  return count ? 1 : 0;
}
void interrupted_io() {
  script = {};
  detail::PosixIo io{scripted_poll, scripted_read, scripted_write, scripted_now};
  std::string bytes = "abcdef";
  std::size_t offset = 0;
  auto result = detail::flush_posix(-1, bytes, offset, TerminalDeadline{} + 8ms, {}, 20ms, io);
  check(result.status == TerminalStatus::deadline && result.completed == 2 && result.pending == 4 && offset == 2,
        "partial write/EINTR/EAGAIN preserves exact suffix");
  check(script.timeouts == std::vector<int>({8, 5, 2}), "EINTR recomputes absolute deadline");
  io.write = success_write;
  auto resumed = detail::flush_posix(-1, bytes, offset, script.current + 1s, {}, 20ms, io);
  check(resumed.completed == 4 && resumed.pending == 0 && script.written == bytes,
        "resumed output contains no duplicated or missing bytes");
  script = {};
  auto input = detail::read_posix(-1, 8, TerminalDeadline{} + 8ms, {}, io);
  check(input.bytes == "x" && script.reads == 3, "read EINTR retries without discarding data");
  script = {};
  input = detail::read_posix(-1, 8, TerminalDeadline{} + 1ms, {}, io);
  check(input.status == TerminalStatus::deadline && input.bytes.empty() && script.reads == 1,
        "read EINTR observes expired absolute deadline");
  check(detail::poll_timeout(TerminalDeadline::max(), 10ms, TerminalDeadline::min()) == 10,
        "extreme absolute deadline has no arithmetic overflow");
}

void terminal_eof() {
  Pty pty;
  auto session = TerminalSession::open(pty.handles());
  check(session.has_value(), "open for terminal hangup");

  if (!session) {
    return;
  }

  pty.master.reset();
  check(session->read(Clock::now() + 1s).status == TerminalStatus::eof,
        "terminal hangup reports EOF even when winsize query fails");
  auto closed = session->close();
  check(!closed && closed.error().kind == TerminalErrorKind::system,
        "disconnected terminal explicitly reports unavailable restoration");
}

void pipes() {
  int fds[2];
  if (pipe(fds) != 0) {
    throw std::runtime_error("pipe");
  }
  Descriptor reader(fds[0]);
  Descriptor writer(fds[1]);
  fcntl(reader.get(), F_SETFL, O_NONBLOCK);
  fcntl(writer.get(), F_SETFL, O_NONBLOCK);
  check(!TerminalSession::open({reader.get(), writer.get()}), "public session rejects redirected pipes");
  const auto &io = detail::native_posix_io();
  std::string bytes(1024 * 1024, 'q');
  std::size_t offset = 0;
  auto partial = detail::flush_posix(writer.get(), bytes, offset, Clock::now() + 5ms, {}, 1ms, io);
  check(partial.status == TerminalStatus::deadline && partial.completed > 0 && partial.pending > 0,
        "real nonblocking pipe short write and backpressure");
  std::string received;
  char buffer[8192];
  for (;;) {
    const auto count = ::read(reader.get(), buffer, sizeof(buffer));
    if (count <= 0) {
      break;
    }
    received.append(buffer, static_cast<std::size_t>(count));
  }
  check(received == bytes.substr(0, offset), "real completed prefix equals pipe bytes");
  check(::write(writer.get(), "tail", 4) == 4, "buffer before hangup");
  writer.reset();
  pollfd ready{reader.get(), POLLIN, 0};
  check(::poll(&ready, 1, 0) == 1 && (ready.revents & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP),
        "fixture exposes simultaneous readable and hangup");
  auto tail = detail::read_posix(reader.get(), 16, Clock::now() + 1s, {}, io);
  auto eof = detail::read_posix(reader.get(), 16, Clock::now() + 1s, {}, io);
  check(tail.bytes == "tail" && eof.status == TerminalStatus::eof, "drain bytes before EOF");

  int broken[2];
  if (pipe(broken) != 0) {
    throw std::runtime_error("broken pipe");
  }
  Descriptor broken_writer(broken[1]);
  ::close(broken[0]);
  std::signal(SIGPIPE, SIG_DFL);
  sigset_t before{}, after{};
  pthread_sigmask(SIG_SETMASK, nullptr, &before);
  offset = 0;
  auto closed = detail::flush_posix(broken_writer.get(), "x", offset, Clock::now() + 1s, {}, 1ms, io);
  pthread_sigmask(SIG_SETMASK, nullptr, &after);
  check(closed.status == TerminalStatus::error && closed.error->native_code == EPIPE && closed.completed == 0,
        "closed pipe returns EPIPE and child survives default SIGPIPE");
  check(sigismember(&before, SIGPIPE) == sigismember(&after, SIGPIPE), "writer restores thread mask");
  struct sigaction disposition{};
  sigaction(SIGPIPE, nullptr, &disposition);
  check(disposition.sa_handler == SIG_DFL, "writer leaves process SIGPIPE disposition unchanged");

  sigset_t blocked{};
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &blocked, &before);
  raise(SIGPIPE);
  check(detail::write_without_sigpipe(broken_writer.get(), "x", 1) == -1 && errno == EPIPE,
        "write with already pending SIGPIPE");
  sigpending(&after);
  check(sigismember(&after, SIGPIPE) == 1, "preexisting pending signal remains owned by caller");
  timespec zero{};
  sigtimedwait(&blocked, nullptr, &zero);
  pthread_sigmask(SIG_SETMASK, &before, nullptr);
}

int run() {
  sessions();
  interrupted_io();
  terminal_eof();
  pipes();
  std::cout << "Terminal POSIX failures: " << failures << '\n';
  return failures == 0 ? 0 : 1;
}
} // namespace

int main() {
  // Only the fixture child owns PTYs, signals and descriptors. Its watchdog also
  // bounds a broken test when run directly without CTest's outer timeout.
  const pid_t child = fork();
  if (child == 0) {
    alarm(10);
    int result = 2;
    try {
      result = run();
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
    }
    std::cout.flush();
    std::cerr.flush();
    _exit(result);
  }
  if (child < 0) {
    return 2;
  }
  int status{};
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return 2;
    }
  }
  if (!WIFEXITED(status)) {
    std::cerr << "Fixture child terminated by signal " << WTERMSIG(status) << '\n';
    return 2;
  }
  return WEXITSTATUS(status);
}
