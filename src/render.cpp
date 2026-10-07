// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/render.hpp>

#include <algorithm>
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

CellRect fill_area(const FillCommand &command, CellRect surface) noexcept {
  return command.area.intersection(command.clip).intersection(surface);
}

std::expected<void, RenderError> preflight(std::span<const RenderCommand> commands, CellRect surface,
                                           WidthPolicy policy, const RenderLimits &limits) noexcept {
  if (commands.size() > limits.max_commands) {
    return std::unexpected(RenderError{RenderErrorKind::command_limit});
  }

  std::size_t text_bytes = 0;
  std::size_t work = 0;

  for (const auto &command : commands) {
    if (const auto *run = std::get_if<TextCommand>(&command)) {
      if (run->text.policy() != policy) {
        return std::unexpected(RenderError{RenderErrorKind::policy_mismatch});
      }

      if (!consume(run->text.bytes().size(), limits.max_text_bytes, text_bytes)) {
        return std::unexpected(RenderError{RenderErrorKind::text_byte_limit});
      }

      if (!consume(run->text.graphemes().size(), limits.max_work, work)) {
        return std::unexpected(RenderError{RenderErrorKind::work_limit});
      }
    } else {
      const auto area = fill_area(std::get<FillCommand>(command), surface);
      const auto width = static_cast<std::size_t>(area.size().width);
      const auto height = static_cast<std::size_t>(area.size().height);

      if (height != 0 && width > (limits.max_work - work) / height) {
        return std::unexpected(RenderError{RenderErrorKind::work_limit});
      }

      work += width * height;
    }
  }

  return {};
}

std::int32_t z_order(const RenderCommand &command) noexcept {
  return std::visit(
    [](const auto &value) {
      return value.z;
    },
    command);
}

std::expected<void, CellBufferError> paint_text(CellBuffer &buffer, const TextCommand &command,
                                                CellRect surface) noexcept {
  const auto clip = command.clip.intersection(surface);

  if (clip.empty() || command.origin.y < clip.origin().y || command.origin.y >= clip.bottom()) {
    return {};
  }

  std::int64_t x = command.origin.x;
  const auto graphemes = command.text.graphemes();

  for (std::size_t i = 0; i < graphemes.size(); ++i) {
    const auto end = x + graphemes[i].cells;

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

std::expected<void, CellBufferError> paint_fill(CellBuffer &buffer, const FillCommand &command, CellRect surface,
                                                const GraphemeText &space) noexcept {
  const auto area = fill_area(command, surface);

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

std::expected<CellBuffer, RenderError> render_full_frame(CellSize size, std::span<const RenderCommand> commands,
                                                         WidthPolicy policy, RenderLimits limits) {
  const auto surface = *CellRect::from({0, 0}, size.normalized());
  const auto checked = preflight(commands, surface, policy, limits);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  auto buffer = CellBuffer::from(size, policy, limits.buffer);

  if (!buffer) {
    return std::unexpected(RenderError{RenderErrorKind::buffer_error, buffer.error()});
  }

  try {
    std::vector<std::size_t> order(commands.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [commands](std::size_t left, std::size_t right) {
      return z_order(commands[left]) < z_order(commands[right]);
    });
    const auto space = GraphemeText::from(" ", policy);

    for (const auto index : order) {
      const auto &command = commands[index];
      const auto painted = std::holds_alternative<TextCommand>(command)
                             ? paint_text(*buffer, std::get<TextCommand>(command), surface)
                             : paint_fill(*buffer, std::get<FillCommand>(command), surface, *space);

      if (!painted) {
        return std::unexpected(RenderError{RenderErrorKind::buffer_error, painted.error()});
      }
    }

    return std::move(*buffer);
  } catch (const std::bad_alloc &) {
    return std::unexpected(RenderError{RenderErrorKind::allocation_failure});
  } catch (const std::length_error &) {
    return std::unexpected(RenderError{RenderErrorKind::allocation_failure});
  }
}
} // namespace portals
