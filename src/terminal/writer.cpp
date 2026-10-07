// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/diff.hpp>
#include <utility>

namespace portals {
TerminalFrameOutput::TerminalFrameOutput(TerminalSession &session) noexcept : session_(session) {
}

std::expected<void, TerminalError> TerminalFrameOutput::submit(std::string bytes) {
  return session_.submit(std::move(bytes));
}

TerminalWrite TerminalFrameOutput::flush(TerminalDeadline deadline, std::stop_token stop) {
  return session_.flush(deadline, stop);
}

FrameWriter::FrameWriter(FrameOutput &output, FrameWriterLimits limits) noexcept : output_(output), limits_(limits) {
}

std::expected<void, FrameError> FrameWriter::submit(CellBuffer &&frame) {
  if (poisoned_) {
    return std::unexpected(FrameError::poisoned);
  }
  if (pending_) {
    return std::unexpected(FrameError::pending);
  }
  if (frame.memory().total_bytes > limits_.max_frame_storage_bytes) {
    return std::unexpected(FrameError::frame_limit);
  }

  auto encoded = encode_diff(committed_frame(), frame, limits_.max_output_bytes);

  if (!encoded) {
    return std::unexpected(encoded.error() == DiffError::output_limit ? FrameError::output_limit
                                                                      : FrameError::allocation_failure);
  }

  if (encoded->empty()) {
    committed_.emplace(std::move(frame));

    return {};
  }

  bytes_ = std::move(*encoded);
  remaining_ = bytes_.size();
  pending_.emplace(std::move(frame));

  return {};
}

void FrameWriter::poison() noexcept {
  poisoned_ = true;
  committed_.reset();
  pending_.reset();
  bytes_.clear();
  remaining_ = 0;
}

TerminalWrite FrameWriter::flush(TerminalDeadline deadline, std::stop_token stop) {
  const auto failure = [&](TerminalError error) {
    poison();

    return TerminalWrite{TerminalStatus::error, 0, 0, error};
  };

  if (poisoned_) {
    return failure({TerminalErrorKind::closed});
  }

  if (!pending_) {
    return {};
  }

  try {
    if (!submitted_) {
      auto accepted = output_.submit(std::move(bytes_));

      if (!accepted) {
        return failure(accepted.error());
      }

      submitted_ = true;
    }

    auto result = output_.flush(deadline, stop);

    if (result.status == TerminalStatus::error || result.error) {
      result.status = TerminalStatus::error;
      if (!result.error) {
        result.error = TerminalError{TerminalErrorKind::system};
      }

      poison();

      return result;
    }

    if (result.completed > remaining_ || result.pending != remaining_ - result.completed ||
        result.status == TerminalStatus::eof || (result.status == TerminalStatus::progress && result.pending != 0)) {
      return failure({TerminalErrorKind::system});
    }

    remaining_ = result.pending;

    if (result.status == TerminalStatus::progress && remaining_ == 0) {
      committed_ = std::move(pending_);
      pending_.reset();
      submitted_ = false;
    }

    return result;
  } catch (...) {
    poison();
    throw;
  }
}

const CellBuffer *FrameWriter::committed_frame() const noexcept {
  return committed_ ? &*committed_ : nullptr;
}

bool FrameWriter::pending() const noexcept {
  return pending_.has_value();
}

bool FrameWriter::poisoned() const noexcept {
  return poisoned_;
}
} // namespace portals
