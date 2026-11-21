// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/render.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace portals {
namespace {
bool consume(std::size_t amount, std::size_t cap, std::size_t &used) noexcept {
  if (amount > cap - used) {
    return false;
  }

  used += amount;

  return true;
}

CellRect fillArea(const FillCommand &command, CellRect surface) noexcept {
  return command.area.intersection(command.clip).intersection(surface);
}

// Resolve index-only ranges before any allocation or grapheme access.
bool textRange(const TextCommand &command, std::size_t &count) noexcept {
  const auto size = command.text.graphemes().size();
  if (command.firstGrapheme > size) {
    return false;
  }
  const auto available = size - command.firstGrapheme;
  count = command.graphemeCount == std::numeric_limits<std::size_t>::max() ? available : command.graphemeCount;

  return count <= available;
}

// Validate the entire scene before allocation or painting. Offscreen text still
// participates in policy/byte/work limits; clipping is not a way around resource caps.
std::expected<void, RenderError> preflight(std::span<const RenderCommand> commands, CellRect surface,
                                           WidthPolicy policy, const RenderLimits &limits) noexcept {
  if (commands.size() > limits.max_commands) {
    return std::unexpected(RenderError{RenderErrorKind::COMMAND_LIMIT});
  }

  std::size_t text_bytes = 0;
  std::size_t work = 0;

  for (const auto &command : commands) {
    if (const auto *run = std::get_if<TextCommand>(&command)) {
      if (run->text.policy() != policy) {
        return std::unexpected(RenderError{RenderErrorKind::POLICY_MISMATCH});
      }

      std::size_t count{};
      if (!textRange(*run, count)) {
        return std::unexpected(RenderError{RenderErrorKind::INVALID_TEXT_RANGE});
      }
      const auto selected = run->text.graphemes().subspan(run->firstGrapheme, count);
      const auto bytes = selected.empty() ? 0 : selected.back().byte_end - selected.front().byte_begin;

      if (!consume(bytes, limits.max_text_bytes, text_bytes)) {
        return std::unexpected(RenderError{RenderErrorKind::TEXT_BYTE_LIMIT});
      }

      if (!consume(count, limits.max_work, work)) {
        return std::unexpected(RenderError{RenderErrorKind::WORK_LIMIT});
      }
    } else {
      const auto area = fillArea(std::get<FillCommand>(command), surface);
      const auto width = static_cast<std::size_t>(area.size().width);
      const auto height = static_cast<std::size_t>(area.size().height);

      if (height != 0 && width > (limits.max_work - work) / height) {
        return std::unexpected(RenderError{RenderErrorKind::WORK_LIMIT});
      }

      work += width * height;
    }
  }

  return {};
}

std::int32_t zOrder(const RenderCommand &command) noexcept {
  return std::visit(
    [](const auto &value) {
      return value.z;
    },
    command);
}

std::expected<void, CellBufferError> paintText(CellBuffer &buffer, const TextCommand &command,
                                               CellRect surface) noexcept {
  const auto clip = command.clip.intersection(surface);

  if (clip.empty() || command.origin.y < clip.origin().y || command.origin.y >= clip.bottom()) {
    return {};
  }

  std::int64_t x = command.origin.x;
  const auto graphemes = command.text.graphemes();

  const auto count = command.graphemeCount == std::numeric_limits<std::size_t>::max()
                       ? graphemes.size() - command.firstGrapheme
                       : command.graphemeCount;
  const auto last = command.firstGrapheme + count;

  for (auto i = command.firstGrapheme; i < last; ++i) {
    if (x >= clip.right()) {
      break;
    }

    const auto end = x + graphemes[i].cells;

    // Clip whole grapheme footprints. Emitting just a wide lead or continuation would
    // disagree with CellBuffer ownership and with the terminal's cursor advancement.
    if (x >= clip.origin().x && end <= clip.right()) {
      const auto written = buffer.put({static_cast<std::int32_t>(x), command.origin.y}, command.text, i, command.style);

      if (!written) {
        return written;
      }
    }

    x = end;
  }

  return {};
}

std::expected<void, CellBufferError> paintFill(CellBuffer &buffer, const FillCommand &command, CellRect surface,
                                               const GraphemeText &space) noexcept {
  const auto area = fillArea(command, surface);

  if (area.empty()) {
    return {};
  }

  for (auto y = area.origin().y; y < area.bottom(); ++y) {
    for (auto x = area.origin().x; x < area.right(); ++x) {
      const auto written = buffer.put({x, y}, space, 0, command.style);

      if (!written) {
        return written;
      }
    }
  }

  return {};
}
} // namespace

std::expected<CellBuffer, RenderError> renderFullFrame(CellSize size, std::span<const RenderCommand> commands,
                                                       WidthPolicy policy, RenderLimits limits) {
  const auto surface = *CellRect::from({0, 0}, size.normalized());
  const auto checked = preflight(commands, surface, policy, limits);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  auto buffer = CellBuffer::from(size, policy, limits.buffer);

  if (!buffer) {
    return std::unexpected(RenderError{RenderErrorKind::BUFFER_ERROR, buffer.error()});
  }

  try {
    std::vector<std::size_t> order(commands.size());

    std::iota(order.begin(), order.end(), std::size_t{0});
    // Stable ordering makes equal-z overlap follow the original command order.
    // Only indices move; commands and borrowed text keep their original lifetime.
    std::ranges::stable_sort(order, [commands](std::size_t left, std::size_t right) {
      return zOrder(commands[left]) < zOrder(commands[right]);
    });

    const auto space = GraphemeText::from(" ", policy);

    for (const auto index : order) {
      const auto &command = commands[index];
      const auto painted = std::holds_alternative<TextCommand>(command)
                             ? paintText(*buffer, std::get<TextCommand>(command), surface)
                             : paintFill(*buffer, std::get<FillCommand>(command), surface, *space);

      if (!painted) {
        return std::unexpected(RenderError{RenderErrorKind::BUFFER_ERROR, painted.error()});
      }
    }

    return std::move(*buffer);
  } catch (const std::bad_alloc &) {
    return std::unexpected(RenderError{RenderErrorKind::ALLOCATION_FAILURE});
  } catch (const std::length_error &) {
    return std::unexpected(RenderError{RenderErrorKind::ALLOCATION_FAILURE});
  }
}
} // namespace portals
