// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_app.hpp>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
extern char **environ;
#endif

namespace portals::ui::detail {
std::expected<LaunchAction, CommandLineError> commandLinePreflight(int argc, const char *const *argv,
                                                                   CommandLineParser &parser) noexcept {
  std::expected<LaunchAction, CommandLineError> action;

  try {
    const auto initialized = app::initialize(argc, argv);

    if (!initialized) {
      const auto code = initialized.error() == app::ProcessContextError::NO_MEMORY
                          ? CommandLineErrorCode::NO_MEMORY
                          : CommandLineErrorCode::PROCESS_CONTEXT;

      action = std::unexpected(CommandLineError{code, {}, initialized.error()});
    } else if (parser) {
      action = parser(*app::args());
    }
  } catch (const std::bad_alloc &) {
    action = std::unexpected(CommandLineError{CommandLineErrorCode::NO_MEMORY, {}});
  } catch (...) {
    action = std::unexpected(CommandLineError{CommandLineErrorCode::CALLBACK_FAILED, {}});
  }

  // Captures retire while the caller still excludes lifecycle reentry.
  parser = nullptr;

  return action;
}
} // namespace portals::ui::detail

namespace portals::ui::app {
struct Arguments::Data {
  // Explicit count construction keeps Debug iterator-proxy allocation inside the checked boundary.
  std::vector<std::string> values = std::vector<std::string>(0);
  std::vector<const char *> pointers = std::vector<const char *>(0);
};

struct Environment::Data {
  std::vector<Entry> entries = std::vector<Entry>(0);
#ifdef _WIN32
  // Aligned with entries before publication; lookups convert only their query, never stored names.
  std::vector<std::wstring> nativeNames = std::vector<std::wstring>(0);
#endif
};

namespace {
struct ProcessState {
  std::mutex mutex;
  std::shared_ptr<const Arguments::Data> arguments;
  std::optional<Environment> environment;
};

ProcessState &processState() {
  static ProcessState state;

  return state;
}

#ifdef _WIN32
void toWide(std::string_view value, std::wstring &result) {
  if (value.empty()) {
    result.clear();

    return;
  }

  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("Environment name is too long");
  }

  const auto size =
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);

  if (!size) {
    throw std::invalid_argument("Environment name is not UTF-8");
  }

  result.resize(static_cast<std::size_t>(size));

  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                           size)) {
    throw std::invalid_argument("Environment name conversion failed");
  }
}

void fromWide(std::wstring_view value, std::string &result) {
  if (value.empty()) {
    result.clear();

    return;
  }

  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("Environment entry is too long");
  }

  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);

  if (!size) {
    throw std::invalid_argument("Environment entry is not Unicode");
  }

  result.resize(static_cast<std::size_t>(size));

  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                           size, nullptr, nullptr)) {
    throw std::invalid_argument("Environment entry conversion failed");
  }
}
#endif

std::expected<Environment, ProcessContextError> captureEnvironment() {
  try {
    std::vector<Environment::Entry> entries(0);

#ifdef _WIN32
    struct EnvironmentBlock {
      wchar_t *value{GetEnvironmentStringsW()};

      ~EnvironmentBlock() {
        if (value)
          FreeEnvironmentStringsW(value);
      }
    } block;

    if (!block.value) {
      return std::unexpected(ProcessContextError::CAPTURE_FAILED);
    }

    std::size_t entryCount{};

    for (auto *entry = block.value; *entry; entry += std::wcslen(entry) + 1) {
      ++entryCount;
    }

    // Reserve before constructing strings so vector growth cannot move Debug string proxies.
    entries.reserve(entryCount);

    for (auto *entry = block.value; *entry; entry += std::wcslen(entry) + 1) {
      const std::wstring_view text(entry);
      // Win32 also exposes hidden drive entries such as '=C:=C:\\'; preserve their complete names.
      const auto separator = text.find(L'=', text.starts_with(L'=') ? 1 : 0);

      if (separator != std::wstring_view::npos) {
        entries.emplace_back(std::string_view{}, std::string_view{});
        auto &captured = entries.back();

        fromWide(text.substr(0, separator), captured.first);
        fromWide(text.substr(separator + 1), captured.second);
      }
    }
#else
    std::size_t entryCount{};

    for (auto **entry = environ; entry && *entry; ++entry) {
      ++entryCount;
    }

    entries.reserve(entryCount);

    for (auto **entry = environ; entry && *entry; ++entry) {
      const std::string_view text(*entry);
      const auto separator = text.find('=');

      if (separator != std::string_view::npos) {
        entries.emplace_back(text.substr(0, separator), text.substr(separator + 1));
      }
    }
#endif

    return Environment::fromEntries(entries);
  } catch (const std::bad_alloc &) {
    return std::unexpected(ProcessContextError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(ProcessContextError::CAPTURE_FAILED);
  }
}

std::expected<void, ProcessContextError> initializeImpl(int count, const char *const *values,
                                                        const Environment *injected) {
  if (count < 0 || (count && !values)) {
    return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
  }

  for (int index = 0; index < count; ++index) {
    if (!values[index]) {
      return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
    }
  }

  auto &state = processState();
  std::lock_guard lock(state.mutex);

  if (state.arguments) {
    if (injected && *state.environment != *injected) {
      return std::unexpected(ProcessContextError::ALREADY_INITIALIZED);
    }

    if (state.arguments->values.size() != static_cast<std::size_t>(count)) {
      return std::unexpected(ProcessContextError::ALREADY_INITIALIZED);
    }

    for (int index = 0; index < count; ++index) {
      if (state.arguments->values[static_cast<std::size_t>(index)] != values[index]) {
        return std::unexpected(ProcessContextError::ALREADY_INITIALIZED);
      }
    }

    // Identical argv never refreshes an already published environment snapshot.
    return {};
  }

  try {
    auto arguments = std::make_shared<Arguments::Data>();

    arguments->values.reserve(static_cast<std::size_t>(count));
    arguments->pointers.reserve(static_cast<std::size_t>(count) + 1);

    for (int index = 0; index < count; ++index) {
      arguments->values.emplace_back(values[index]);
    }

    for (const auto &value : arguments->values) {
      arguments->pointers.push_back(value.c_str());
    }

    arguments->pointers.push_back(nullptr);

    auto environment = injected ? std::expected<Environment, ProcessContextError>(*injected) : captureEnvironment();

    if (!environment) {
      return std::unexpected(environment.error());
    }

    state.environment = std::move(*environment);
    state.arguments = std::move(arguments);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ProcessContextError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(ProcessContextError::CAPTURE_FAILED);
  }
}
} // namespace

std::expected<Arguments, ProcessContextError> Arguments::capture(int count, const char *const *values) {
  if (count < 0 || (count && !values)) {
    return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
  }

  try {
    auto data = std::make_shared<Data>();

    data->values.reserve(static_cast<std::size_t>(count));
    data->pointers.reserve(static_cast<std::size_t>(count) + 1);

    for (int index = 0; index < count; ++index) {
      if (!values[index]) {
        return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
      }

      data->values.emplace_back(values[index]);
    }

    for (const auto &value : data->values) {
      data->pointers.push_back(value.c_str());
    }

    data->pointers.push_back(nullptr);

    return Arguments(std::move(data));
  } catch (const std::bad_alloc &) {
    return std::unexpected(ProcessContextError::NO_MEMORY);
  }
}

int Arguments::argc() const noexcept {
  return static_cast<int>(data_->values.size());
}

const char *const *Arguments::argv() const noexcept {
  return data_->pointers.data();
}

std::size_t Arguments::size() const noexcept {
  return data_->values.size();
}

std::string_view Arguments::operator[](std::size_t index) const {
  return data_->values.at(index);
}

std::span<const std::string>::iterator Arguments::begin() const noexcept {
  return std::span(data_->values).begin();
}

std::span<const std::string>::iterator Arguments::end() const noexcept {
  return std::span(data_->values).end();
}

std::expected<Environment, ProcessContextError> Environment::fromEntries(std::span<const Entry> entries) {
  try {
    auto data = std::make_shared<Data>();

    data->entries.reserve(entries.size());
#ifdef _WIN32
    data->nativeNames.reserve(entries.size());
    std::wstring validatedValue(L"", 0);
#endif

    for (const auto &[name, value] : entries) {
      if (name.empty() || name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
        return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
      }

#ifdef _WIN32
      data->nativeNames.emplace_back(L"", 0);

      toWide(name, data->nativeNames.back());
      toWide(value, validatedValue);
#endif
      data->entries.emplace_back(name, value);
    }

    return Environment(std::move(data));
  } catch (const std::bad_alloc &) {
    return std::unexpected(ProcessContextError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
  }
}

std::expected<std::optional<std::string_view>, ProcessContextError>
Environment::get(std::string_view name) const noexcept {
  if (name.empty() || name.find('\0') != std::string_view::npos) {
    return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
  }

  try {
#ifdef _WIN32
    std::wstring wideName(L"", 0);

    toWide(name, wideName);
#endif

    for (std::size_t index = 0; index < data_->entries.size(); ++index) {
      const auto &entry = data_->entries[index];

#ifdef _WIN32
      const auto &wideKey = data_->nativeNames[index];
      const auto comparison = CompareStringOrdinal(wideName.data(), static_cast<int>(wideName.size()), wideKey.data(),
                                                   static_cast<int>(wideKey.size()), TRUE);

      if (!comparison) {
        return std::unexpected(ProcessContextError::CAPTURE_FAILED);
      }

      const bool matches = comparison == CSTR_EQUAL;
#else
      const bool matches = entry.first == name;
#endif

      if (matches) {
        return std::string_view(entry.second);
      }
    }

    return std::optional<std::string_view>{};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ProcessContextError::NO_MEMORY);
  } catch (const std::invalid_argument &) {
    return std::unexpected(ProcessContextError::INVALID_ARGUMENT);
  } catch (...) {
    return std::unexpected(ProcessContextError::CAPTURE_FAILED);
  }
}

std::size_t Environment::size() const noexcept {
  return data_->entries.size();
}

bool Environment::operator==(const Environment &other) const noexcept {
  return data_->entries == other.data_->entries;
}

std::expected<void, ProcessContextError> initialize(int count, const char *const *values) {
  return initializeImpl(count, values, nullptr);
}

std::expected<void, ProcessContextError> initialize(int count, const char *const *values,
                                                    const Environment &environment) {
  return initializeImpl(count, values, &environment);
}

std::expected<Arguments, ProcessContextError> args() {
  auto &state = processState();
  std::lock_guard lock(state.mutex);

  if (!state.arguments) {
    return std::unexpected(ProcessContextError::UNINITIALIZED);
  }

  return Arguments(state.arguments);
}

std::expected<Environment, ProcessContextError> environment() {
  auto &state = processState();
  std::lock_guard lock(state.mutex);

  if (!state.environment) {
    return std::unexpected(ProcessContextError::UNINITIALIZED);
  }

  return *state.environment;
}
} // namespace portals::ui::app
