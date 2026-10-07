// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/diff.hpp>

#include <charconv>
#include <iterator>
#include <new>
#include <stdexcept>

namespace portals {
namespace {
// Count before allocating: oversized frames never build an oversized temporary.
class Encoding {
public:
  explicit Encoding(std::size_t limit, std::string *bytes = nullptr) : limit_(limit), bytes_(bytes) {
  }
  void append(std::string_view value) {
    if (value.size() > limit_ - size_) {
      throw DiffError::output_limit;
    }

    size_ += value.size();

    if (bytes_) {
      bytes_->append(value);
    }
  }
  void number(std::uint32_t value) {
    char digits[10];
    const auto result = std::to_chars(std::begin(digits), std::end(digits), value);
    append({digits, static_cast<std::size_t>(result.ptr - digits)});
  }
  std::size_t size() const noexcept {
    return size_;
  }

private:
  std::size_t limit_;
  std::size_t size_{};
  std::string *bytes_;
};

// Intern IDs belong to different buffers and cannot establish semantic equality.
// Compare resolved glyph/style values and the continuation role instead.
bool equal_cell(const CellView &left, const CellView &right) noexcept {
  return left.glyph == right.glyph && left.style == right.style && left.cell.columns == right.cell.columns &&
         (left.cell.kind == CellKind::continuation) == (right.cell.kind == CellKind::continuation);
}

bool equal_row(const CellBuffer &previous, const CellBuffer &next, std::int32_t y) noexcept {
  for (std::int32_t x = 0; x < next.size().width; ++x) {
    if (!equal_cell(*previous.at({x, y}), *next.at({x, y}))) {
      return false;
    }
  }

  return true;
}

void color(Encoding &out, const RgbColor &rgb, std::string_view selector) {
  out.append(selector);
  out.number(rgb.red);
  out.append(";");
  out.number(rgb.green);
  out.append(";");
  out.number(rgb.blue);
}

void style(Encoding &out, const CellStyle &value) {
  out.append("\x1b[0");

  if (value.bold) {
    out.append(";1");
  }
  if (value.italic) {
    out.append(";3");
  }
  if (value.underline) {
    out.append(";4");
  }
  if (value.foreground) {
    color(out, *value.foreground, ";38;2;");
  }
  if (value.background) {
    color(out, *value.background, ";48;2;");
  }

  out.append("m");
}

void encode(Encoding &out, const CellBuffer *previous, const CellBuffer &next) {
  const bool full = !previous || previous->size() != next.size() || previous->policy() != next.policy();
  bool started = false;
  CellStyle current;

  // A full transaction establishes a parser boundary for our CSI/UTF-8 grammar.
  if (full) {
    out.append("\x18");
  }

  const auto begin = [&] {
    out.append("\x1b[0m\x1b[?6l\x1b[r\x1b[4l\x1b[?7l");
    started = true;

    if (full) {
      out.append("\x1b[2J");
    }
  };

  if (full) {
    begin();
  }

  // A changed row is emitted completely, with wrapping disabled, so overwriting a
  // wide glyph also clears its previous footprint without splitting grapheme bytes.
  for (std::int32_t y = 0; y < next.size().height; ++y) {
    if (!full && equal_row(*previous, next, y)) {
      continue;
    }

    if (!started) {
      begin();
    }

    out.append("\x1b[");
    out.number(static_cast<std::uint32_t>(y) + 1);
    out.append(";1H");

    for (std::int32_t x = 0; x < next.size().width; ++x) {
      const auto cell = *next.at({x, y});

      if (cell.cell.kind == CellKind::continuation) {
        continue;
      }

      if (cell.style != current) {
        style(out, cell.style);
        current = cell.style;
      }

      out.append(cell.glyph);
    }
  }

  if (started) {
    out.append("\x1b[0m\x1b[1;1H\x1b[?7h");
  }
}
} // namespace

std::expected<std::string, DiffError> encode_diff(const CellBuffer *previous, const CellBuffer &next,
                                                  std::size_t max_bytes) {
  try {
    Encoding measure(max_bytes);
    encode(measure, previous, next);
    // MSVC Debug allocates a proxy even for empty strings. The pointer constructor
    // can propagate that failure; the default constructor is noexcept.
    std::string bytes("");
    bytes.reserve(measure.size());
    Encoding output(max_bytes, &bytes);
    encode(output, previous, next);

    return bytes;
  } catch (DiffError error) {
    return std::unexpected(error);
  } catch (const std::bad_alloc &) {
    return std::unexpected(DiffError::allocation_failure);
  } catch (const std::length_error &) {
    return std::unexpected(DiffError::allocation_failure);
  }
}
} // namespace portals
