// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/text_transform.hpp>

#include <utf8proc.h>

namespace portals {
namespace detail {
struct TextSegmentsData {
  explicit TextSegmentsData(const GraphemeText &value) : input(value) {
  }

  GraphemeText input;
  std::vector<std::pair<std::size_t, std::size_t>> ranges = std::vector<std::pair<std::size_t, std::size_t>>(0);
};

std::optional<TextError> textSplitLimit(const GraphemeText &input, const TextLimits &limits) noexcept {
  if (input.bytes().size() > limits.max_input_bytes) {
    return TextError{TextErrorKind::INPUT_LIMIT, 0};
  }
  std::size_t count{}, cells{};
  for (const auto &cluster : input.graphemes()) {
    if (cluster.byte_end - cluster.byte_begin > limits.max_cluster_bytes) {
      return TextError{TextErrorKind::CLUSTER_LIMIT, cluster.byte_begin};
    }
    if (++count > limits.max_graphemes) {
      return TextError{TextErrorKind::GRAPHEME_LIMIT, cluster.byte_begin};
    }
    cells += cluster.cells;
    if (cells > limits.max_cells) {
      return TextError{TextErrorKind::CELL_LIMIT, cluster.byte_begin};
    }
  }
  return {};
}
} // namespace detail

namespace {
bool whitespace(char32_t scalar) noexcept {
  return (scalar >= 0x09 && scalar <= 0x0d) || scalar == 0x20 || scalar == 0x85 || scalar == 0xa0 || scalar == 0x1680 ||
         (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202f ||
         scalar == 0x205f || scalar == 0x3000;
}

bool delimiter(std::string_view cluster) noexcept {
  while (!cluster.empty()) {
    utf8proc_int32_t scalar{};
    const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(cluster.data()),
                                        static_cast<utf8proc_ssize_t>(cluster.size()), &scalar);

    if (count <= 0 || !whitespace(static_cast<char32_t>(scalar))) {
      return false;
    }

    cluster.remove_prefix(static_cast<std::size_t>(count));
  }

  return true;
}
} // namespace

std::string_view TextSegment::bytes() const noexcept {
  return owner_->input.bytes().substr(first_, last_ - first_);
}

WidthPolicy TextSegment::policy() const noexcept {
  return owner_->input.policy();
}

GraphemeText TextSegment::text() const {
  TextLimits limits;
  limits.max_input_bytes = bytes().size();
  limits.max_cluster_bytes = bytes().size();
  limits.max_graphemes = owner_->input.graphemes().size();
  limits.max_cells = static_cast<std::size_t>(owner_->input.measure());

  return GraphemeText::from(bytes(), policy(), limits).value();
}

bool TextSegment::operator==(const TextSegment &other) const noexcept {
  return bytes() == other.bytes() && policy() == other.policy();
}

std::size_t TextSegments::size() const noexcept {
  return owner_->ranges.size();
}

std::expected<TextSegment, TreeError> TextSegments::at(std::size_t index) const {
  if (index >= size()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto [first, last] = owner_->ranges[index];

  return TextSegment(owner_, first, last);
}

bool TextSegments::operator==(const TextSegments &other) const noexcept {
  if (size() != other.size() || owner_->input.policy() != other.owner_->input.policy()) {
    return false;
  }

  for (std::size_t i = 0; i < size(); ++i) {
    const auto [first, last] = owner_->ranges[i];
    const auto [otherFirst, otherLast] = other.owner_->ranges[i];

    if (owner_->input.bytes().substr(first, last - first) !=
        other.owner_->input.bytes().substr(otherFirst, otherLast - otherFirst)) {
      return false;
    }
  }

  return true;
}

std::expected<TextSegments, TreeError> splitText(const GraphemeText &input, bool splitWords, TextSplitOptions options) {
  if (!options.maxTokens) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (detail::textSplitLimit(input, options.limits)) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  try {
    auto data = std::make_shared<detail::TextSegmentsData>(input);
    std::optional<std::size_t> begin;
    const auto append = [&](std::size_t first, std::size_t last) -> bool {
      if (data->ranges.size() >= options.maxTokens) {
        return false;
      }

      data->ranges.emplace_back(first, last);

      return true;
    };

    for (const auto &cluster : input.graphemes()) {
      const bool separated =
        splitWords && delimiter(input.bytes().substr(cluster.byte_begin, cluster.byte_end - cluster.byte_begin));

      if (!splitWords) {
        if (!append(cluster.byte_begin, cluster.byte_end)) {
          return std::unexpected(TreeError::NODE_LIMIT);
        }
      } else if (separated) {
        if (begin && !append(*begin, cluster.byte_begin)) {
          return std::unexpected(TreeError::NODE_LIMIT);
        }

        begin.reset();
      } else if (!begin) {
        begin = cluster.byte_begin;
      }
    }

    if (begin && !append(*begin, input.bytes().size())) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    return TextSegments(std::move(data));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TreeError::NODE_LIMIT);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<TextSegments, TreeError> words(const GraphemeText &input, TextSplitOptions options) {
  return splitText(input, true, options);
}

std::expected<TextSegments, TreeError> graphemes(const GraphemeText &input, TextSplitOptions options) {
  return splitText(input, false, options);
}
} // namespace portals
