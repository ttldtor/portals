// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/pss_parser.hpp>

namespace portals {
namespace {
class RuntimeSink {
public:
  RuntimeSink(std::string_view source, bool metadata) : source_(source), metadata_(metadata) {
  }

  std::expected<void, PssError> sourceName(std::string_view name) {
    auto admitted = charge(name.size(), {});

    if (!admitted) {
      return admitted;
    }

    document.sourceName.assign(name);

    return {};
  }

  std::expected<void, PssError> beginRule(PssLocation location) {
    auto admitted = charge(sizeof(StyleRule) + sizeof(PssRuleOrigin), location);

    if (!admitted) {
      return admitted;
    }

    current_ = StyleRule{};
    origin_ = PssRuleOrigin{};
    origin_.selector = location;
    origin_.declarationBegin = document.declarations.size();

    return {};
  }

  std::expected<void, PssError> type(detail::PssSpan span, PssLocation location) {
    return name(span, location, current_.selector.type);
  }

  std::expected<void, PssError> id(detail::PssSpan span, PssLocation location) {
    return name(span, location, current_.selector.id);
  }

  std::expected<void, PssError> className(detail::PssSpan span, PssLocation location) {
    auto admitted = charge(sizeof(std::string) + span.size, location);

    if (!admitted) {
      return admitted;
    }

    current_.selector.classes.emplace_back(source_.substr(span.offset, span.size));

    return {};
  }

  std::expected<void, PssError> state(StyleState value) {
    current_.selector.state = value;

    return {};
  }

  std::expected<void, PssError> declaration(PssProperty property, detail::PssValue value, PssLocation location) {
    if (metadata_) {
      auto admitted = charge(sizeof(PssDeclarationOrigin), location);

      if (!admitted) {
        return admitted;
      }

      origin_.effectiveDeclaration[static_cast<std::size_t>(property)] = document.declarations.size();
      ++origin_.declarationCount;
      document.declarations.push_back({document.sheet.rules.size(), property, location, value});
    }

    detail::assignPssDeclaration(property, value, current_.declarations);

    return {};
  }

  std::expected<void, PssError> endRule() {
    document.sheet.rules.push_back(current_);

    if (metadata_) {
      document.origins.push_back(origin_);
    }

    return {};
  }

  PssDocument document;

private:
  std::expected<void, PssError> charge(std::size_t bytes, PssLocation location) {
    if (!metadata_) {
      return {};
    }

    if (bytes > MAX_PSS_DOCUMENT_BYTES - ownedBytes_) {
      return std::unexpected(
        detail::pssError(PssErrorCode::DOCUMENT_LIMIT, location, "PSS document exceeds owning payload budget"));
    }

    ownedBytes_ += bytes;

    return {};
  }

  std::expected<void, PssError> name(detail::PssSpan span, PssLocation location, std::string &destination) {
    auto admitted = charge(span.size, location);

    if (!admitted) {
      return admitted;
    }

    destination.assign(source_.substr(span.offset, span.size));

    return {};
  }

  std::string_view source_;
  bool metadata_;
  std::size_t ownedBytes_{};
  StyleRule current_;
  PssRuleOrigin origin_;
};

PssError allocationError() {
  auto error = detail::pssError(PssErrorCode::ALLOCATION, {}, "PSS allocation failed");

  error.cause = TreeError::NO_MEMORY;

  return error;
}
} // namespace

std::expected<StyleSheet, StyleError> parsePss(std::string_view source) {
  if (source.size() > MAX_PSS_BYTES) {
    return std::unexpected(StyleError{TreeError::INVALID_ARGUMENT, 1, 1, "PSS source exceeds byte budget"});
  }

  try {
    RuntimeSink sink(source, false);
    auto parsed = detail::PssParser(source, sink, detail::PssPolicy::LEGACY).parse();

    if (!parsed) {
      const auto &error = parsed.error();

      return std::unexpected(StyleError{error.cause, error.location.line, error.location.column, error.message});
    }

    return sink.document.sheet;
  } catch (const std::bad_alloc &) {
    return std::unexpected(StyleError{TreeError::NO_MEMORY, 1, 1, "PSS allocation failed"});
  }
}

std::expected<PssDocument, PssError> parsePssDocument(std::string_view source, std::string_view sourceName) {
  if (source.size() > MAX_PSS_BYTES) {
    return std::unexpected(detail::pssError(PssErrorCode::INPUT_LIMIT, {}, "PSS source exceeds byte budget"));
  }

  if (sourceName.size() > MAX_PSS_SOURCE_NAME_BYTES) {
    return std::unexpected(
      detail::pssError(PssErrorCode::SOURCE_NAME_LIMIT, {}, "PSS source identity exceeds byte budget"));
  }

  auto validName = detail::validatePssUtf8(sourceName);

  if (!validName) {
    return std::unexpected(validName.error());
  }

  try {
    RuntimeSink sink(source, true);
    auto admitted = sink.sourceName(sourceName);

    if (!admitted) {
      return std::unexpected(admitted.error());
    }

    auto parsed = detail::PssParser(source, sink, detail::PssPolicy::STRICT_UTF8).parse();

    if (!parsed) {
      return std::unexpected(parsed.error());
    }

    return sink.document;
  } catch (const std::bad_alloc &) {
    return std::unexpected(allocationError());
  }
}
} // namespace portals
