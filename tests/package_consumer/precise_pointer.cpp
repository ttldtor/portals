// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/application.hpp>

int main() {
  using namespace portals;
  const MouseInputEvent legacy{1, 2, MouseButton::LEFT, MouseAction::PRESS, {}};
  const InputStep step{0, {}, InputStatus::NEED_INPUT};
  const InputIssue legacyIssue{InputIssueKind::INVALID_SEQUENCE, 0, 1};

  if (legacy.precision || step.protocol || legacyIssue.invalidates_pointer_metrics) {
    return 1;
  }

  InputOptions options;

  options.terminal_reports = true;
  auto decoder = InputDecoder::create(options);

  if (!decoder) {
    return 2;
  }

  decoder->setPointerContext({InputCoordinateMode::PIXEL, 80, 24, 800, 480, 11});
  const auto pointer = decoder->next("\x1b[<0;106;51M", 0);

  if (!pointer || !pointer->event) {
    return 3;
  }

  const auto *mouse = std::get_if<MouseInputEvent>(&*pointer->event);

  if (!mouse || mouse->column != 10 || mouse->row != 2 || !mouse->precision || mouse->precision->generation != 11) {
    return 4;
  }

  const auto report = decoder->next("\x1b[48;24;80;0;0t", 0);

  if (!report || report->status != InputStatus::PROTOCOL || !report->protocol || report->event ||
      std::get<InputSizeReport>(*report->protocol).text_width_px != 0) {
    return 5;
  }

  // Invalid timeout rejects before acquiring native handles; exercise the new exported overload without terminal I/O.
  const auto native =
    openNativeApplicationTerminal({}, {}, std::chrono::milliseconds{0}, false, NativeScreenMode::MAIN_SCREEN, false,
                                  NativePointerOptions{NativePointerPolicy::AUTO, NativeInputOrder::FIFO});

  return !native && native.error().kind == TerminalErrorKind::INVALID_OPTIONS ? 0 : 6;
}
