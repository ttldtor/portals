// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace portals::ui {

/** @brief Borrowed typed options; the lvalue must outlive the synchronous lazy scene run.
 * @tparam T Caller-owned option type. Access never grants mutation.
 */
template <class T> class OptionsRef {
public:
  /** @param value Borrowed lvalue; it must outlive this reference and the lazy run. */
  explicit OptionsRef(T &value) noexcept : value_(&value) {
  }

  /** @param value Temporary options, deliberately rejected to prevent a dangling borrow. */
  OptionsRef(T &&value) = delete;

  /** @return Read-only caller-owned options, without transferring ownership. */
  const T &get() const noexcept {
    return *value_;
  }

private:
  T *value_;
};

/** @tparam T Caller-owned option type, deduced only from an lvalue.
 * @param value Caller-owned lvalue, retained by reference without copying.
 * @return Borrowed typed read-only view; temporaries are not accepted.
 */
template <class T> OptionsRef<T> opts(T &value) noexcept {
  return OptionsRef<T>(value);
}

/** @tparam T Temporary option type, deliberately rejected to prevent dangling borrowed options.
 * @param value Temporary options whose lifetime cannot cover a lazy scene.
 */
template <class T> void opts(const T &&value) = delete;

/** @brief Read-only process snapshots, independent of a current Scene or Application. */
namespace app {

/** @brief Process capture failures are separate from widget and parser failures. */
enum class ProcessContextError {
  UNINITIALIZED,       ///< No process snapshot has been successfully published.
  INVALID_ARGUMENT,    ///< Invalid argument array, environment name or platform encoding.
  ALREADY_INITIALIZED, ///< Initialization conflicts with the previously published snapshot.
  NO_MEMORY,           ///< Allocation failed while capturing or querying an owning snapshot.
  CAPTURE_FAILED       ///< Process capture or snapshot query failed without changing stored state.
};

/** @brief Immutable owning argument snapshot, including argv[0]; copies share immutable storage. */
class Arguments {
public:
  struct Data; ///< Opaque immutable storage; no implementation borrow is exposed.
  /** @brief Copy standalone arguments without registering or modifying the process context.
   * @param argc Nonnegative argument count, including the program name when present.
   * @param argv Array of argc non-null C strings; nullptr is allowed for argc zero.
   * @return Owning copy, or INVALID_ARGUMENT/NO_MEMORY. Bytes are preserved as supplied by main().
   */
  static std::expected<Arguments, ProcessContextError> capture(int argc, const char *const *argv);
  /** @return Copied argument count, including argv[0] when supplied. */
  int argc() const noexcept;
  /** @return Read-only null-terminated pointer array, retained by this snapshot. */
  const char *const *argv() const noexcept;
  /** @return Number of copied arguments, including the program name. */
  std::size_t size() const noexcept;
  /** @param index Zero-based argument index.
   * @return Borrowed argument bytes, retained by this snapshot.
   * @throws std::out_of_range When index is not less than size().
   */
  std::string_view operator[](std::size_t index) const;
  /** @return Immutable range beginning, retained by this snapshot. */
  std::span<const std::string>::iterator begin() const noexcept;
  /** @return Immutable range end, retained by this snapshot. */
  std::span<const std::string>::iterator end() const noexcept;

private:
  explicit Arguments(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {
  }

  std::shared_ptr<const Data> data_;
  friend std::expected<Arguments, ProcessContextError> args();
};

/** @brief Immutable owning environment snapshot; missing and empty values remain distinct.
 * Windows names compare without case using Unicode ordinal rules; names/values are UTF-8.
 * POSIX names and values preserve native bytes, including non-UTF-8 values, and compare exactly.
 * No accessor mutates or refreshes the process environment.
 */
class Environment {
public:
  struct Data;                                       ///< Opaque immutable storage; no process-owned pointers escape.
  using Entry = std::pair<std::string, std::string>; ///< Owning name/value pair for synthetic snapshots.
  /** @param entries Synthetic native-policy entries; no process environment is modified.
   * @return Owning copy, or INVALID_ARGUMENT/NO_MEMORY. Names must be nonempty; embedded NUL is rejected.
   */
  static std::expected<Environment, ProcessContextError> fromEntries(std::span<const Entry> entries);
  /** @param name Nonempty variable name without NUL, in the platform's documented encoding.
   * @return Checked borrowed value retained by this snapshot, or successful nullopt if missing.
   * Empty values remain successful engaged views of zero length. Duplicate synthetic names resolve to the first entry.
   * Windows Unicode conversion allocation failure is NO_MEMORY; invalid query encoding/name is INVALID_ARGUMENT.
   */
  std::expected<std::optional<std::string_view>, ProcessContextError> get(std::string_view name) const noexcept;
  /** @return Number of captured entries, without consulting the live process. */
  std::size_t size() const noexcept;
  /** @param other Immutable snapshot to compare.
   * @return Exact ordered name/value byte equality, without reading the live environment.
   */
  bool operator==(const Environment &other) const noexcept;

private:
  explicit Environment(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {
  }

  std::shared_ptr<const Data> data_;
  friend std::expected<Environment, ProcessContextError> environment();
};

/** @brief Capture argv and the environment once, before any concurrent foreign environment mutation.
 * Repeating identical argv is safe and does not refresh the environment; contradictory argv is rejected.
 * The internal mutex synchronizes Portals initialization only, never foreign setenv/putenv/Win32 calls.
 * Call before creating threads that can mutate the environment. Native capture uses Win32 Unicode on Windows.
 * @param argc Nonnegative argument count, including the program name when present.
 * @param argv Borrowed array of argc non-null strings, copied before publication; nullptr is allowed for zero.
 * @return Success, or exact argument/capture/allocation failure; no partial context is published.
 */
std::expected<void, ProcessContextError> initialize(int argc, const char *const *argv);
/** @brief Initialize using an explicit immutable environment, without reading the live environment.
 * @param argc Nonnegative count of arguments, including the program name when present.
 * @param argv Argument array copied at initialization, using the same contract as the native overload.
 * @param environment Immutable synthetic snapshot, retained by ownership. Contradictory reinitialization is rejected.
 * @return Success, or exact argument/allocation/initialization failure; existing snapshots are never refreshed.
 */
std::expected<void, ProcessContextError> initialize(int argc, const char *const *argv, const Environment &environment);
/** @return Owning snapshot, or typed UNINITIALIZED before successful initialize/runApp(argc, argv). */
std::expected<Arguments, ProcessContextError> args();
/** @return Owning snapshot with the same initialization contract as args(). */
std::expected<Environment, ProcessContextError> environment();

/** @return Alias for environment(). */
inline std::expected<Environment, ProcessContextError> env() {
  return environment();
}
} // namespace app

/** @brief Immutable owning launch text with allocation-free empty, copy and move operations.
 * Nonempty construction copies bytes into shared immutable storage and may throw std::bad_alloc.
 * Default construction retains no string object, including on MSVC Debug.
 * Views/pointers remain valid while any copy retaining the same storage remains alive.
 */
class LaunchText {
public:
  /** @brief Create empty text without allocating. */
  LaunchText() noexcept = default;

  /** @param bytes Bytes borrowed only during this owning copy; empty text allocates nothing.
   * @throws std::bad_alloc If nonempty storage cannot be allocated.
   */
  LaunchText(std::string_view bytes) {
    if (!bytes.empty()) {
      bytes_ = std::make_shared<const std::string>(bytes.data(), bytes.size());
    }
  }

  /** @param bytes Borrowed string, including rvalues; bytes are copied without invoking a string move.
   * @throws std::bad_alloc If nonempty storage cannot be allocated.
   */
  LaunchText(const std::string &bytes) : LaunchText(std::string_view(bytes)) {
  }

  /** @param bytes NUL-terminated text borrowed only during copying; nullptr denotes empty text.
   * @throws std::bad_alloc If nonempty storage cannot be allocated.
   */
  LaunchText(const char *bytes) : LaunchText(bytes ? std::string_view(bytes) : std::string_view{}) {
  }

  /** @param other Immutable text whose ownership is shared without allocation. */
  LaunchText(const LaunchText &other) noexcept = default;
  /** @param other Text whose storage is transferred; the source becomes empty. */
  LaunchText(LaunchText &&other) noexcept = default;
  /** @param other Immutable text whose ownership replaces this value without allocation.
   * @return This owning text after sharing the storage.
   */
  LaunchText &operator=(const LaunchText &other) noexcept = default;
  /** @param other Text whose storage is transferred; the source becomes empty.
   * @return This owning text after transferring the storage.
   */
  LaunchText &operator=(LaunchText &&other) noexcept = default;

  /** @return Read-only NUL-terminated bytes; empty text returns a static empty buffer. */
  const char *data() const noexcept {
    return bytes_ ? bytes_->data() : "";
  }

  /** @return Number of owned bytes, excluding the trailing NUL. */
  std::size_t size() const noexcept {
    return bytes_ ? bytes_->size() : 0;
  }

  /** @return True when the owning text has no bytes. */
  bool empty() const noexcept {
    return size() == 0;
  }

  /** @return Borrowed read-only view, retained by this value or another copy sharing its storage. */
  std::string_view view() const noexcept {
    return {data(), size()};
  }

  /** @param text Borrowed bytes to find.
   * @param position First byte position to inspect.
   * @return Matching byte position, or std::string_view::npos; no allocation occurs.
   */
  std::size_t find(std::string_view text, std::size_t position = 0) const noexcept {
    return view().find(text, position);
  }

  /** @param left Owning immutable text.
   * @param right Owning immutable text to compare by byte value.
   * @return True for equal bytes, independently of storage identity.
   */
  friend bool operator==(const LaunchText &left, const LaunchText &right) noexcept {
    return left.view() == right.view();
  }

  /** @param left Owning immutable text.
   * @param right Borrowed bytes to compare.
   * @return True for equal bytes, without allocating.
   */
  friend bool operator==(const LaunchText &left, std::string_view right) noexcept {
    return left.view() == right;
  }

  /** @param left Owning immutable text.
   * @param right Borrowed string to compare.
   * @return True for equal bytes, without constructing another owning value.
   */
  friend bool operator==(const LaunchText &left, const std::string &right) noexcept {
    return left.view() == right;
  }

  /** @param left Owning immutable text.
   * @param right NUL-terminated bytes to compare; nullptr denotes empty text.
   * @return True for equal bytes, without allocating.
   */
  friend bool operator==(const LaunchText &left, const char *right) noexcept {
    return left.view() == (right ? std::string_view(right) : std::string_view{});
  }

  /** @param stream Borrowed output stream.
   * @param text Immutable owning text to write.
   * @return The same output stream after writing the complete byte view.
   */
  friend std::ostream &operator<<(std::ostream &stream, const LaunchText &text) {
    return stream << text.view();
  }

private:
  std::shared_ptr<const std::string> bytes_;
};

/** @brief Parser failures do not masquerade as widget errors. */
enum class CommandLineErrorCode {
  INVALID_ARGUMENT, ///< The parser rejected an option, value or command-line structure.
  CALLBACK_FAILED,  ///< A parser callback threw an exception other than allocation failure.
  NO_MEMORY,        ///< Allocation failed at the command-line launch boundary.
  PROCESS_CONTEXT   ///< Process snapshot initialization failed; inspect the exact context cause.
};

/** @brief Owning parser diagnostic, safe after the parser's captures retire. */
struct CommandLineError {
  CommandLineErrorCode code{}; ///< Original parser or boundary failure category.
  LaunchText message; ///< Immutable owning diagnostic; empty fallback and ownership transfers allocate nothing.
  std::optional<app::ProcessContextError> processContext{}; ///< Exact initialization cause, when applicable.
};
/** @brief Continue into the existing runtime, or finish successfully without opening a terminal. */
enum class LaunchDisposition {
  CONTINUE, ///< Proceed to application creation and the existing scene lifecycle.
  FINISH    ///< Print the owning output and finish successfully before terminal acquisition.
};

/** @brief Successful preflight, optionally printing help/version before any application is created. */
struct LaunchAction {
  LaunchDisposition disposition{LaunchDisposition::CONTINUE}; ///< Successful preflight decision.
  LaunchText output; ///< Immutable owning help/version text, printed to stdout only for FINISH.
};

/** @brief Owning lazy parser; borrowed bound options must outlive its synchronous invocation. */
using CommandLineParser =
  std::move_only_function<std::expected<LaunchAction, CommandLineError>(const app::Arguments &)>;
} // namespace portals::ui
