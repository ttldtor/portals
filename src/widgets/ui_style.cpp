// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

#include <portals/detail/pss_parser.hpp>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <limits>

namespace portals::ui {
namespace {
constexpr std::size_t FILE_READ_CHUNK_BYTES = 4096;
constexpr std::string_view STAGE_NAMESPACE = "__portals_ui_style_";
constexpr std::size_t STAGE_NAME_BUFFER_BYTES =
  STAGE_NAMESPACE.size() + std::numeric_limits<std::uint64_t>::digits10 + 2;
std::atomic<std::uint64_t> nextStage{1};

struct FileCloser {
  void operator()(std::FILE *file) const noexcept {
    (void)std::fclose(file);
  }
};

std::string_view safeSourceLabel(std::string_view source) {
  auto size = std::min(source.size(), MAX_PSS_SOURCE_NAME_BYTES);

  // A budget boundary may cut through a UTF-8 scalar. Exclude its leading byte and continuation tail.
  if (size < source.size()) {
    constexpr unsigned char CONTINUATION_MASK = 0xc0;
    constexpr unsigned char CONTINUATION_MARKER = 0x80;

    while (size && (static_cast<unsigned char>(source[size]) & CONTINUATION_MASK) == CONTINUATION_MARKER) {
      --size;
    }
  }

  const auto prefix = source.substr(0, size);

  // Invalid UTF-8 and NUL are represented by the original fixed PSS error, never raw diagnostic label bytes.
  return portals::detail::validatePssUtf8(prefix) ? prefix : std::string_view{};
}

ManagedError pssFailure(PssError failure, const std::shared_ptr<const std::string> &source) {
  ManagedError result{ManagedPhase::STYLE,
                      {},
                      StyleError{failure.cause, failure.location.line, failure.location.column, failure.message},
                      {},
                      {}};

  result.pss = failure;
  result.styleSource = source;

  return result;
}

ManagedError fileFailure(PssFileOperation operation, std::error_code cause,
                         const std::shared_ptr<const std::string> &source) {
  ManagedError result{ManagedPhase::STYLE, {}, TreeError::INVALID_ARGUMENT, {}, {}};

  result.file = PssFileError{operation, cause};
  result.styleSource = source;

  return result;
}

std::expected<PssDocument, ManagedError> readDocument(const std::filesystem::path &path,
                                                      const std::shared_ptr<const std::string> &source) {
  std::FILE *raw{};
#ifdef _WIN32
  const int opened = _wfopen_s(&raw, path.c_str(), L"rb");
#else
  errno = 0;
  raw = std::fopen(path.c_str(), "rb");
  const int opened = raw ? 0 : errno;
#endif

  if (!raw) {
    return std::unexpected(fileFailure(PssFileOperation::OPEN, {opened, std::generic_category()}, source));
  }

  const std::unique_ptr<std::FILE, FileCloser> input(raw);
  std::string bytes;
  std::array<char, FILE_READ_CHUNK_BYTES> chunk{};

  for (;;) {
    errno = 0;

    const auto count = std::fread(chunk.data(), 1, chunk.size(), input.get());
    const int readCause = errno;

    if (std::ferror(input.get())) {
      return std::unexpected(fileFailure(PssFileOperation::READ, {readCause, std::generic_category()}, source));
    }

    if (count > MAX_PSS_BYTES - bytes.size()) {
      return std::unexpected(
        fileFailure(PssFileOperation::INPUT_LIMIT, std::make_error_code(std::errc::file_too_large), source));
    }

    bytes.append(chunk.data(), count);

    if (count < chunk.size()) {
      break;
    }
  }

  auto parsed = parsePssDocument(bytes, *source);

  if (!parsed) {
    return std::unexpected(pssFailure(parsed.error(), source));
  }

  // Checked MSVC container moves may allocate inside noexcept. Copy the owning result under caller's boundary.
  return std::expected<PssDocument, ManagedError>(std::in_place, *parsed);
}
} // namespace

struct StyleStage::Impl {
  const StyleOptions options;
  const std::string name;
  const std::shared_ptr<const std::string> source;
  const std::shared_ptr<const PssDocument> document;
  const std::shared_ptr<const std::filesystem::path> path;
  const std::optional<PssError> error;

  Impl(const StyleOptions &policy, std::string_view label, std::shared_ptr<const PssDocument> snapshot,
       std::shared_ptr<const std::filesystem::path> file, std::optional<PssError> failure)
      : options{policy.tier, policy.name, std::string(0, '\0'), policy.layers}, name(makeName(policy)),
        source(std::make_shared<const std::string>(safeSourceLabel(label))), document(std::move(snapshot)),
        path(std::move(file)), error(failure) {
  }

  static std::string makeName(const StyleOptions &options) {
    if (options.name) {
      return *options.name;
    }

    auto identity = nextStage.load(std::memory_order_relaxed);

    for (;;) {
      if (!identity || identity == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("UI style stage identity exhausted");
      }

      if (nextStage.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed)) {
        break;
      }
    }

    std::array<char, STAGE_NAME_BUFFER_BYTES> bytes{};
    const auto size =
      std::snprintf(bytes.data(), bytes.size(), "__portals_ui_style_%llu", static_cast<unsigned long long>(identity));

    return std::string(bytes.data(), static_cast<std::size_t>(size));
  }
};

StyleStage StyleStage::bytes(std::string_view bytes, const StyleOptions &options, std::source_location origin) {
  auto parsed = parsePssDocument(bytes, options.sourceName);

  return parsed ? document(*parsed, options, origin) : failure(parsed.error(), options, origin);
}

StyleStage StyleStage::document(const PssDocument &document, const StyleOptions &options, std::source_location origin) {
  // The public carrier is mutable. Shape validation must precede copying any indexed metadata or payload.
  auto valid = portals::detail::validateStyleDocument(document);
  const std::string_view label =
    options.sourceName.empty() ? std::string_view(document.sourceName) : std::string_view(options.sourceName);

  if (!valid) {
    return StyleStage(std::make_shared<const Impl>(options, label, nullptr, nullptr, valid.error()), origin);
  }

  if (!options.sourceName.empty()) {
    auto sourceValid = portals::detail::validatePssUtf8(options.sourceName);

    if (!sourceValid) {
      return failure(sourceValid.error(), options, origin);
    }

    if (options.sourceName.size() > MAX_PSS_SOURCE_NAME_BYTES) {
      return failure(
        portals::detail::pssError(PssErrorCode::SOURCE_NAME_LIMIT, {}, "PSS source identity exceeds budget"), options,
        origin);
    }
  }

  auto owned = std::make_shared<PssDocument>(document);

  if (!options.sourceName.empty()) {
    owned->sourceName = options.sourceName;
  }

  valid = portals::detail::validateStyleDocument(*owned);

  return StyleStage(std::make_shared<const Impl>(options, label, valid ? owned : nullptr, nullptr,
                                                 valid ? std::nullopt : std::optional(valid.error())),
                    origin);
}

StyleStage StyleStage::source(const FileSource &source, const StyleOptions &options, std::source_location origin) {
  const auto utf8Path = source.path.u8string();
  const std::string_view label = options.sourceName.empty()
                                   ? std::string_view(reinterpret_cast<const char *>(utf8Path.data()), utf8Path.size())
                                   : std::string_view(options.sourceName);
  auto valid = portals::detail::validatePssUtf8(label);
  std::optional<PssError> failure;

  if (source.path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
    failure = portals::detail::pssError(PssErrorCode::NUL_BYTE, {}, "PSS file path contains NUL");
  } else if (!valid) {
    failure = valid.error();
  } else if (label.size() > MAX_PSS_SOURCE_NAME_BYTES) {
    failure = portals::detail::pssError(PssErrorCode::SOURCE_NAME_LIMIT, {}, "PSS source identity exceeds budget");
  }

  return StyleStage(std::make_shared<const Impl>(options, label, nullptr,
                                                 std::make_shared<const std::filesystem::path>(source.path), failure),
                    origin);
}

StyleStage StyleStage::failure(PssError failure, const StyleOptions &options, std::source_location origin) {
  return StyleStage(std::make_shared<const Impl>(options, options.sourceName, nullptr, nullptr, failure), origin);
}

StyleStage StyleStage::scoped(std::shared_ptr<detail::RefTarget> target) const {
  StyleStage result(std::make_shared<const Impl>(payload_->options, *payload_->source, payload_->document,
                                                 payload_->path, payload_->error),
                    origin_);

  result.scoped_ = true;
  result.target_ = std::move(target);

  return result;
}

ManagedError StyleStage::boundaryFailure(TreeError cause) const {
  if (cause == TreeError::NO_MEMORY) {
    auto failure = portals::detail::pssError(PssErrorCode::ALLOCATION, {}, "UI style allocation failed");

    failure.cause = cause;

    return pssFailure(failure, payload_->source);
  }

  ManagedError failure{ManagedPhase::STYLE, {}, cause, {}, {}};

  failure.styleSource = payload_->source;

  return failure;
}

std::expected<StyleLayerHandle, ManagedError> StyleStage::apply(ManagedScene &scene,
                                                                const StyleLayerHandle *existing) const {
  std::optional<NodeId> scope;

  if (scoped_) {
    auto checkedScope = detail::NamedAccess::scope(scene, target_);

    if (!checkedScope) {
      auto failure = checkedScope.error();
      failure.styleSource = payload_->source;

      return std::unexpected(failure);
    }

    scope = *checkedScope;
  }

  if (existing) {
    auto checked = scene.validateStyleLayer(*existing);

    if (!checked) {
      auto failure = checked.error();

      failure.styleSource = payload_->source;

      return std::unexpected(failure);
    }
  }

  if (payload_->options.name && payload_->name.starts_with(STAGE_NAMESPACE)) {
    return std::unexpected(
      pssFailure(portals::detail::pssError(PssErrorCode::INVALID_VALUE, {},
                                           "Explicit UI style name uses the reserved stage namespace"),
                 payload_->source));
  }

  if (payload_->error) {
    return std::unexpected(pssFailure(*payload_->error, payload_->source));
  }

  try {
    // A low-level caller can occupy any text name. New unnamed stages must never upsert that foreign layer.
    if (!existing && !payload_->options.name) {
      auto sources = scene.styleSources();

      if (!sources) {
        auto failure = sources.error();

        failure.styleSource = payload_->source;

        return std::unexpected(failure);
      }

      for (const auto &source : *sources) {
        if (source.tier == payload_->options.tier && source.name == payload_->name) {
          return std::unexpected(
            pssFailure(portals::detail::pssError(PssErrorCode::INVALID_VALUE, {},
                                                 "Generated UI style name collides with an existing layer"),
                       payload_->source));
        }
      }
    }

    auto install = [&](const PssDocument &document) {
      std::optional<PssDocument> scopedDocument;

      if (scope) {
        scopedDocument.emplace(document);
        scopedDocument->sheet.scope = scope;
      }

      auto result = scene.styleDocument(payload_->options.tier, payload_->name,
                                        scopedDocument ? *scopedDocument : document, payload_->options.layers);

      if (!result) {
        result.error().styleSource = payload_->source;
      }

      return result;
    };

    if (payload_->document) {
      return install(*payload_->document);
    }

    auto parsed = readDocument(*payload_->path, payload_->source);

    return parsed ? install(*parsed) : std::unexpected(parsed.error());
  } catch (const std::bad_alloc &) {
    auto failure = portals::detail::pssError(PssErrorCode::ALLOCATION, {}, "UI style allocation failed");

    failure.cause = TreeError::NO_MEMORY;

    return std::unexpected(pssFailure(failure, payload_->source));
  } catch (...) {
    ManagedError failure{ManagedPhase::STYLE, {}, TreeError::CALLBACK_FAILED, {}, {}};

    failure.styleSource = payload_->source;

    return std::unexpected(failure);
  }
}
} // namespace portals::ui
