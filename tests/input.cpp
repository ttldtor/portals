// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/input.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace portals;
int failures = 0;
std::size_t partition_runs = 0;

void check(bool value, const char *message) {
  if (!value) {
    if (failures < 30) {
      std::cerr << message << '\n';
    }

    ++failures;
  }
}

InputEvent text(char32_t scalar, bool alt = false) {
  return TextInputEvent{scalar, {false, alt, false, false}};
}

InputEvent key(InputKey identity, InputModifiers mods = {}) {
  return KeyEvent{identity, 0, mods};
}

struct Run {
  std::vector<InputEvent> events;
  std::size_t consumed{};
  InputStatus status{};
  bool operator==(const Run &) const = default;
};

bool feed(InputDecoder &decoder, std::string_view bytes, InputTime now, Run &run) {
  std::size_t offset = 0;
  std::size_t pulls = 0;

  do {
    if (++pulls > bytes.size() * 3 + 20) {
      check(false, "bounded pull progress");

      return false;
    }

    auto step = decoder.next(bytes.substr(offset), now);
    check(step.has_value(), "valid feed schedule accepted");

    if (!step) {
      return false;
    }

    check(step->consumed <= bytes.size() - offset, "exact consumed prefix in range");
    offset += step->consumed;
    run.consumed += step->consumed;
    run.status = step->status;

    if (step->event) {
      if (const auto *issue = std::get_if<InputIssue>(&*step->event)) {
        check(issue->begin <= issue->end && issue->end <= run.consumed, "issue range covers consumed bytes only");
      }

      run.events.push_back(std::move(*step->event));
    }

    if (step->status == InputStatus::failed) {
      return false;
    }

    if (step->status == InputStatus::need_input) {
      check(offset == bytes.size(), "need_input consumes complete available suffix");

      return true;
    }
  } while (true);
}

void finish(InputDecoder &decoder, InputTime now, Run &run) {
  for (unsigned tries = 0; tries < 5; ++tries) {
    auto step = decoder.finish(now);
    check(step.has_value(), "finish accepts monotonic clock");

    if (!step) {
      return;
    }

    check(step->consumed == 0, "finish consumes no bytes");
    run.status = step->status;

    if (step->event) {
      run.events.push_back(std::move(*step->event));
    }

    if (step->status == InputStatus::finished || step->status == InputStatus::failed) {
      return;
    }
  }

  check(false, "finish drains bounded pending events");
}

Run decode(std::string_view bytes, const std::vector<std::size_t> &ends = {}, InputOptions options = {},
           bool probes = false) {
  auto decoder = *InputDecoder::create(options);
  Run result;

  if (probes) {
    check(decoder.expect_replies({true, true, 42, 0, 100}).has_value(), "probe window armed");
  }

  std::size_t start = 0;

  for (auto end : ends) {
    if (!feed(decoder, bytes.substr(start, end - start), 0, result)) {
      return result;
    }

    start = end;
  }

  if (feed(decoder, bytes.substr(start), 0, result)) {
    finish(decoder, 0, result);
  }

  check(result.consumed == bytes.size(), "all input accounted exactly once");

  return result;
}

void golden(std::string_view bytes, std::vector<InputEvent> expected, InputOptions options = {}, bool probes = false) {
  const auto actual = decode(bytes, {}, options, probes);
  check(actual.events == expected, "semantic golden event sequence");

  if (actual.events != expected && failures < 30) {
    std::cerr << "  fixture bytes=" << bytes.size() << " actual=" << actual.events.size()
              << " expected=" << expected.size() << '\n';
  }
}

void semantic_fixtures() {
  for (unsigned byte = 0; byte < 32; ++byte) {
    const std::string bytes(1, static_cast<char>(byte));

    if (byte == 8 || byte == 9 || byte == 13 || byte == 27) {
      const auto identity = byte == 8    ? InputKey::backspace
                            : byte == 9  ? InputKey::tab
                            : byte == 13 ? InputKey::enter
                                         : InputKey::escape;
      golden(bytes, {key(identity)});
    } else {
      golden(bytes, {KeyEvent{InputKey::character, static_cast<char32_t>(byte + 0x40), {false, false, true, false}}});
    }
  }

  golden("a\r\t\x7f\b",
         {text(U'a'), key(InputKey::enter), key(InputKey::tab), key(InputKey::backspace), key(InputKey::backspace)});
  golden(std::string("\0\x01\x1c\n", 4), {KeyEvent{InputKey::character, U'@', {false, false, true, false}},
                                          KeyEvent{InputKey::character, U'A', {false, false, true, false}},
                                          KeyEvent{InputKey::character, U'\\', {false, false, true, false}},
                                          KeyEvent{InputKey::character, U'J', {false, false, true, false}}});
  golden("\xc2\xa2\xe7\x95\x8c\xf0\x9f\x98\x80", {text(U'¢'), text(U'界'), text(U'😀')});
  golden("e\xcc\x81", {text(U'e'), text(U'\u0301')});
  golden("\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xf4\x8f\xbf\xbf",
         {text(0x80), text(0x7ff), text(0x800), text(0xd7ff), text(0xe000), text(0x10ffff)});
  golden("\x1b"
         "x\x1b\xc3\xa9",
         {text(U'x', true), text(U'é', true)});
  golden("\x1b\x1b", {key(InputKey::escape), key(InputKey::escape)});
  golden("\x1b[A\x1bOB\x1b[1;6C\x1b[24~\x1b[Z",
         {key(InputKey::up), key(InputKey::down), key(InputKey::right, {true, false, true, false}), key(InputKey::f12),
          key(InputKey::tab, {true, false, false, false})});

  const std::pair<std::string, InputKey> keys[] = {
    {"A", InputKey::up},  {"B", InputKey::down}, {"C", InputKey::right}, {"D", InputKey::left}, {"H", InputKey::home},
    {"F", InputKey::end}, {"P", InputKey::f1},   {"Q", InputKey::f2},    {"R", InputKey::f3},   {"S", InputKey::f4}};

  for (const auto &[suffix, identity] : keys) {
    golden("\x1b[" + suffix, {key(identity)});
    golden("\x1bO" + suffix, {key(identity)});

    for (unsigned mod = 1; mod <= 8; ++mod) {
      golden("\x1b[1;" + std::to_string(mod) + suffix,
             {key(identity, {(mod & 1) == 0, ((mod - 1) & 2) != 0, ((mod - 1) & 4) != 0, false})});
    }
  }

  const std::pair<unsigned, InputKey> tilde[] = {
    {1, InputKey::home},    {2, InputKey::insert},    {3, InputKey::delete_key}, {4, InputKey::end},
    {5, InputKey::page_up}, {6, InputKey::page_down}, {7, InputKey::home},       {8, InputKey::end},
    {11, InputKey::f1},     {12, InputKey::f2},       {13, InputKey::f3},        {14, InputKey::f4},
    {15, InputKey::f5},     {17, InputKey::f6},       {18, InputKey::f7},        {19, InputKey::f8},
    {20, InputKey::f9},     {21, InputKey::f10},      {23, InputKey::f11},       {24, InputKey::f12}};

  for (auto [number, identity] : tilde) {
    golden("\x1b[" + std::to_string(number) + ";3~", {key(identity, {false, true, false, false})});
  }

  golden("\x80", {InputIssue{InputIssueKind::invalid_utf8, 0, 1}});
  golden("\xe2\x82", {InputIssue{InputIssueKind::truncated, 0, 2}});
  golden("\xe2\x1b[A", {InputIssue{InputIssueKind::invalid_utf8, 0, 1}, key(InputKey::up)});
  golden("\xed\xa0\x80\xf4\x90\x80\x80\xc0\xaf",
         {InputIssue{InputIssueKind::invalid_utf8, 0, 3}, InputIssue{InputIssueKind::invalid_utf8, 3, 7},
          InputIssue{InputIssueKind::invalid_utf8, 7, 8}, InputIssue{InputIssueKind::invalid_utf8, 8, 9}});
  golden("\x1b[1;;2A", {InputIssue{InputIssueKind::invalid_sequence, 0, 7}});
  golden("\x1b[1;A", {InputIssue{InputIssueKind::invalid_sequence, 0, 5}});
  golden("\x1b[1:2A", {InputIssue{InputIssueKind::invalid_sequence, 0, 6}});
  golden("\x1b[1 q", {InputIssue{InputIssueKind::unknown_sequence, 0, 5}});
  golden("\x1b[1 2Aq", {InputIssue{InputIssueKind::invalid_sequence, 0, 5}, text(U'q')});
  golden("\x1b[\x01"
         "a",
         {InputIssue{InputIssueKind::invalid_sequence, 0, 3}, text(U'a')});
  golden("\x1b[4294967296~", {InputIssue{InputIssueKind::numeric_overflow, 0, 13}});
  golden("\x1b[00000000001~", {InputIssue{InputIssueKind::parameter_limit, 0, 14}});
  golden("\x1b[1;9A", {InputIssue{InputIssueKind::unknown_sequence, 0, 6}});
  golden("\x9b"
         "A",
         {InputIssue{InputIssueKind::invalid_utf8, 0, 1}, text(U'A')});
}

void mouse_and_probes() {
  golden("\x1b[<0;1;2M\x1b[<0;1;2m\x1b[<35;9;8M\x1b[<92;3;4M",
         {MouseInputEvent{0, 1, MouseButton::left, MouseAction::press, {}},
          MouseInputEvent{0, 1, MouseButton::left, MouseAction::release, {}},
          MouseInputEvent{8, 7, MouseButton::no_button, MouseAction::move, {}},
          MouseInputEvent{2, 3, MouseButton::wheel_up, MouseAction::scroll, {true, true, true, false}}});

  for (unsigned code = 64; code < 68; ++code) {
    golden("\x1b[<" + std::to_string(code) + ";1;1M",
           {MouseInputEvent{0,
                            0,
                            static_cast<MouseButton>(static_cast<unsigned>(MouseButton::wheel_up) + code - 64),
                            MouseAction::scroll,
                            {}}});
  }

  for (const std::string seq : {"\x1b[<0;0;1M", "\x1b[<128;1;1M", "\x1b[<64;1;1m", "\x1b[<96;1;1M", "\x1b[<3;1;1M",
                                "\x1b[<0;2147483649;1M", "\x1b[<1;1M"}) {
    golden(seq, {InputIssue{InputIssueKind::invalid_sequence, 0, seq.size()}});
  }

  golden("\x1b[<2;2147483648;1M",
         {MouseInputEvent{std::numeric_limits<std::int32_t>::max(), 0, MouseButton::right, MouseAction::press, {}}});
  golden("\x1b[?1;2c\x1b[4;5R",
         {InputProbeCandidate{InputProbeKind::primary_device_attributes, 42, {1, 2}, 2},
          InputProbeCandidate{InputProbeKind::cursor_position, 42, {4, 5}, 2}},
         {}, true);
  golden("\x1b[?4294967295c", {InputProbeCandidate{InputProbeKind::primary_device_attributes, 42, {4294967295U}, 1}},
         {}, true);
  golden("\x1b[1;2R", {key(InputKey::f3, {true, false, false, false})});
  golden("\x1b[1;2R\x1b[1;2R",
         {InputProbeCandidate{InputProbeKind::cursor_position, 42, {1, 2}, 2},
          key(InputKey::f3, {true, false, false, false})},
         {}, true);
  golden("\x1b[?1c\x1b[?1c",
         {InputProbeCandidate{InputProbeKind::primary_device_attributes, 42, {1}, 1},
          InputIssue{InputIssueKind::unknown_sequence, 5, 10}},
         {}, true);
  golden("\x1b[0;1R", {InputIssue{InputIssueKind::unknown_sequence, 0, 6}}, {}, true);
  InputOptions disabled;
  disabled.sgr_mouse = false;
  disabled.bracketed_paste = false;
  golden("\x1b[<0;1;1M", {InputIssue{InputIssueKind::unknown_sequence, 0, 9}}, disabled);
  golden("\x1b[200~", {InputIssue{InputIssueKind::unknown_sequence, 0, 6}}, disabled);
}

void paste_and_limits() {
  const std::string begin = "\x1b[200~";
  const std::string end = "\x1b[201~";
  golden(begin + end, {PasteInputEvent{}});
  std::string hostile = "\x1b[A\x1b[<0;1;1M\x1b[?1c\x1b]52;hello\a\x1b[200~\xff";
  hostile.append("\0\r\t\n", 4);
  golden(begin + hostile + end + "x", {PasteInputEvent{hostile}, text(U'x')});
  golden(begin + "\x1b[20\x1b\x1b[201x" + end, {PasteInputEvent{"\x1b[20\x1b\x1b[201x"}});
  // The legacy delimiter is not authenticated: a forged end really closes paste.
  golden(begin + "a" + end + "\x1b[A", {PasteInputEvent{"a"}, key(InputKey::up)});
  InputOptions small;
  small.max_paste_bytes = 3;
  golden(begin + "abc" + end, {PasteInputEvent{"abc"}}, small);
  golden(begin + "abcd\x1b[A" + end + "z", {InputIssue{InputIssueKind::paste_limit, 0, 10}, text(U'z')}, small);
  golden(begin + "ab\x1b[20x\x1b[A" + end + "z", {InputIssue{InputIssueKind::paste_limit, 0, 13}, text(U'z')}, small);
  small.max_paste_bytes = 0;
  golden(begin + end, {PasteInputEvent{}}, small);
  golden(begin + "a" + end, {InputIssue{InputIssueKind::paste_limit, 0, 7}}, small);
  golden(begin + "abc", {InputIssue{InputIssueKind::truncated, 0, 9}});

  for (const std::string intro : {"\x1b]", "\x1bP", "\x1bX", "\x1b^", "\x1b_"}) {
    golden(intro + "\x1b[A\x1b[200~hidden\x1b\\x", {InputIssue{InputIssueKind::unknown_sequence, 0, 2}, text(U'x')});
  }

  golden("\x1b]hidden\ax", {InputIssue{InputIssueKind::unknown_sequence, 0, 2}, text(U'x')});
  golden("\x1bPhidden\ax\x1b\\z", {InputIssue{InputIssueKind::unknown_sequence, 0, 2}, text(U'z')});
  small = {};
  small.max_sequence_bytes = 6;
  golden("\x1b[11~", {key(InputKey::f1)}, small);
  golden("\x1b[1;2A", {key(InputKey::up, {true, false, false, false})}, small);
  golden("\x1b[01;2Aq", {InputIssue{InputIssueKind::sequence_limit, 0, 7}, text(U'q')}, small);
  golden("\x1b[00001;2Aq", {InputIssue{InputIssueKind::sequence_limit, 0, 7}, text(U'q')}, small);
  small.max_parameters = 1;
  golden("\x1b[1;2A", {InputIssue{InputIssueKind::parameter_limit, 0, 6}}, small);
  small.max_digits = 1;
  golden("\x1b[11~", {InputIssue{InputIssueKind::parameter_limit, 0, 5}}, small);

  for (std::size_t length : {63, 64, 65}) {
    const auto sequence = "\x1b[" + std::string(length - 3, ' ') + "q";
    golden(sequence,
           {InputIssue{length == 65 ? InputIssueKind::sequence_limit : InputIssueKind::unknown_sequence, 0, length}});
  }

  std::string parameters = "\x1b[?0";

  for (unsigned i = 1; i < 16; ++i) {
    parameters += ";0";
  }

  golden(parameters + "c", {InputProbeCandidate{InputProbeKind::primary_device_attributes, 42, {}, 16}}, {}, true);
  golden(parameters + ";0c", {InputIssue{InputIssueKind::parameter_limit, 0, parameters.size() + 3}}, {}, true);
}

void timing() {
  auto decoder = *InputDecoder::create();
  Run run;
  feed(decoder, "\x1b", 10, run);
  check(decoder.deadline() == 40, "ESC absolute deadline exposed");
  feed(decoder, "", 39, run);
  check(run.events.empty(), "ESC does not expire early");
  feed(decoder, "x", 40, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::escape), text(U'x')}, "deadline equality expires first");
  auto backwards = decoder.next("bad", 39);
  check(!backwards && backwards.error() == InputError::backward_time, "backward time rejected without input");
  check(!decoder.deadline(), "ground has no timer");
  decoder.reset();
  run = {};
  feed(decoder, "\x1b\xc3", 0, run);
  feed(decoder, "\xa9", 29, run);
  check(run.events == std::vector<InputEvent>{text(U'é', true)}, "Alt scalar within ESC window");
  decoder.reset();
  run = {};
  feed(decoder, "\x1b\xc3", 0, run);
  feed(decoder, "\xa9", 30, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::escape), InputIssue{InputIssueKind::truncated, 1, 2},
                                              InputIssue{InputIssueKind::invalid_utf8, 2, 3}},
        "Alt timeout drains Escape and truncated tail before suffix");
  decoder.reset();
  run = {};
  feed(decoder, "\x1b\xc3", 0, run);
  finish(decoder, 0, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::escape), InputIssue{InputIssueKind::truncated, 1, 2}},
        "EOF Alt tail drains Escape and truncated scalar");
  auto closed = decoder.next("x", 0);
  check(!closed && closed.error() == InputError::closed, "EOF closes feed");
  auto again = decoder.finish(0);
  check(again && again->status == InputStatus::finished && !again->event, "EOF idempotent after drain");

  for (const std::string prefix : {"\x1b[12", "\x1bO", "\x1b]abc", "\x1b[200~abc"}) {
    decoder.reset();
    run = {};
    feed(decoder, prefix, 0, run);
    const auto deadline = *decoder.deadline();
    feed(decoder, "", deadline - 1, run);
    const auto step = decoder.next("\x1b[A", deadline);
    check(step && step->consumed == 0 && step->status == InputStatus::failed && step->event &&
            std::get<InputIssue>(*step->event).kind == InputIssueKind::timeout,
          "unfinished control deadline fails before suffix consumption");
    check(!decoder.next("x", deadline), "failed stream requires explicit reset");
    decoder.reset();
    run = {};
    feed(decoder, "x", 0, run);
    check(run.events == std::vector<InputEvent>{text(U'x')}, "reset creates independent session");
  }

  decoder.reset();
  run = {};
  feed(decoder, "\x1b[", 0, run);
  feed(decoder, "1", 999, run);
  check(decoder.deadline() == 1000, "CSI bytes never extend deadline");
  decoder.reset();
  run = {};
  check(decoder.expect_replies({true, true, 8, 10, 20}).has_value(), "explicit probe window");
  feed(decoder, "\x1b[1;2R", 20, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::f3, {true, false, false, false})},
        "late CPR uses key table");
  decoder.reset();
  run = {};
  check(decoder.expect_replies({true, true, 8, 10, 20}).has_value(), "future probe window armed");
  feed(decoder, "\x1b[", 9, run);
  feed(decoder, "1;2R", 10, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::f3, {true, false, false, false})},
        "reply start must also belong to expected window");
  decoder.reset();
  run = {};
  feed(decoder, "\x1b[", 0, run);
  check(!decoder.expect_replies({true, true, 1, 0, 100}), "window cannot change inside candidate");
  decoder.reset();
  run = {};
  const auto max_time = std::numeric_limits<InputTime>::max();
  feed(decoder, "\x1b", max_time - 1, run);
  check(decoder.deadline() == max_time, "deadline addition saturates without wrap");
  feed(decoder, "", max_time, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::escape)}, "saturated deadline expires at maximum time");
  decoder.reset();
  run = {};
  feed(decoder,
       "\x1b"
       "x",
       max_time, run);
  check(run.events == std::vector<InputEvent>{key(InputKey::escape), text(U'x')},
        "new saturated deadline expires within a single feed");
  InputOptions shorter;
  shorter.sequence_timeout = 10;
  decoder = *InputDecoder::create(shorter);
  run = {};
  feed(decoder, "\x1b", 0, run);
  const auto step = decoder.next("[A", 20);
  check(step && step->consumed == 1 && step->status == InputStatus::failed && step->event &&
          std::get<InputIssue>(*step->event).kind == InputIssueKind::timeout,
        "shorter sequence interval expires immediately on introducer transition");
  shorter = {};
  shorter.max_paste_bytes = 1;
  decoder = *InputDecoder::create(shorter);
  run = {};
  feed(decoder, "\x1b[200~ab", 0, run);
  feed(decoder, "\x1b[A", 29999, run);
  check(decoder.deadline() == 30000 && run.events.size() == 1, "oversize paste discard keeps original deadline");
  const auto expired_paste = decoder.next("\x1b[201~q", 30000);
  check(expired_paste && expired_paste->status == InputStatus::failed && expired_paste->consumed == 0,
        "discarded paste cannot recover after deadline through input suffix");

  const auto scheduled = [](bool bytewise) {
    auto scheduled_decoder = *InputDecoder::create();
    Run events;
    const std::pair<std::string_view, InputTime> batches[] = {
      {"\x1b\xc3", 0}, {"\xa9\x1b[", 29}, {"1;2A\x1b", 30}, {"\xc3\xa9", 60}};

    for (auto [bytes, time] : batches) {
      if (bytewise) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
          feed(scheduled_decoder, bytes.substr(i, 1), time, events);
        }
      } else {
        feed(scheduled_decoder, bytes, time, events);
      }
    }

    finish(scheduled_decoder, 60, events);

    return events;
  };
  check(scheduled(false) == scheduled(true), "same timed arrival batches preserve chunk invariance across deadlines");
}

std::uint32_t random_next(std::uint32_t &seed) {
  seed = seed * 1664525U + 1013904223U;

  return seed;
}

void partitions(std::string_view bytes, InputOptions options = {}, bool probes = false) {
  const auto expected = decode(bytes, {}, options, probes);

  if (bytes.size() <= 12 && !bytes.empty()) {
    const auto possibilities = std::size_t{1} << (bytes.size() - 1);

    for (std::size_t mask = 0; mask < possibilities; ++mask) {
      std::vector<std::size_t> ends;

      for (std::size_t i = 1; i < bytes.size(); ++i) {
        if (mask & (std::size_t{1} << (i - 1))) {
          ends.push_back(i);
        }
      }

      check(decode(bytes, ends, options, probes) == expected,
            "all short nonempty partitions preserve events and accounting");
      ++partition_runs;
    }
  } else {
    for (std::size_t split = 1; split < bytes.size(); ++split) {
      check(decode(bytes, {split}, options, probes) == expected, "every long single split preserves events");
      ++partition_runs;
    }
  }

  for (std::size_t chunk : {1, 2, 3, 5, 8, 17}) {
    std::vector<std::size_t> ends;

    for (std::size_t end = chunk; end < bytes.size(); end += chunk) {
      ends.push_back(end);
    }

    check(decode(bytes, ends, options, probes) == expected, "fixed chunks preserve events");
    ++partition_runs;
  }

  for (std::size_t budget : {1, 2, 7}) {
    auto bounded = options;
    bounded.work_bytes = budget;
    check(decode(bytes, {}, bounded, probes) == expected, "one-event output and small work budgets preserve semantics");
  }
}

void partition_and_fuzz() {
  for (const std::string bytes :
       {"a\r\t", "\x1b\x1b", "\x1b\xc3\xa9", "\xe2\x1b[A", "\x1b[1;2A", "\x1b[<0;1;1M", "\x1b[?1;2c", "\x1b[1;2R",
        "\x1b[1;;A", "\x1b[200~\x1b[201~", "\x1b]a\x1b\\x", "\xf4\x90\x80\x80", "\x1b[1 2Aq",
        "\x1b[200~x\x1b[20\x1b\x1b[201~z", "\x1b[200~\x1b[?1c\x1b[<0;1;1M\x1b]52;bad\a\x1b[201~"}) {
    partitions(bytes);
    partitions(bytes, {}, true);
  }

  InputOptions small;
  small.max_paste_bytes = 3;
  small.max_sequence_bytes = 6;
  partitions("\x1b[200~abcdef\x1b[A\x1b[201~q", small);
  partitions("\x1b[000000000000Aq", small);
  partitions("\x1b]" + std::string(512, 'x') + "\x1b\\q", small);
  golden("\x1b]" + std::string(1024 * 1024, 'x') + "\x1b\\q",
         {InputIssue{InputIssueKind::unknown_sequence, 0, 2}, text(U'q')}, small);
  golden("\x1b[200~" + std::string(1024 * 1024, 'x') + "\x1b[201~q",
         {InputIssue{InputIssueKind::paste_limit, 0, 10}, text(U'q')}, small);
  std::uint32_t seed = 0x7a113;

  for (unsigned iteration = 0; iteration < 600; ++iteration) {
    std::string bytes;
    const auto length = random_next(seed) % 96;

    for (unsigned i = 0; i < length; ++i) {
      bytes += static_cast<char>(random_next(seed) >> 24);
    }

    if (iteration % 3 == 0) {
      bytes = "\x1b[200~" + bytes + "\x1b[201~q";
    } else if (iteration % 3 == 1) {
      bytes = "\x1b[" + bytes + "Aq";
    }

    const auto expected = decode(bytes, {}, small, true);
    std::vector<std::size_t> ends;

    for (std::size_t end = 1; end < bytes.size(); ++end) {
      if (random_next(seed) % 3 == 0) {
        ends.push_back(end);
      }
    }

    check(decode(bytes, ends, small, true) == expected, "seeded malformed stream has chunk-invariant recovery");
    ++partition_runs;
  }
}

void options_and_ownership() {
  InputOptions options;
  options.work_bytes = 0;
  check(!InputDecoder::create(options), "zero work budget rejected");
  options = {};
  options.max_sequence_bytes = 65;
  check(!InputDecoder::create(options), "scratch cap validated");
  options = {};
  options.max_parameters = 17;
  check(!InputDecoder::create(options), "parameter storage cap validated");
  options = {};
  options.escape_timeout = 0;
  check(!InputDecoder::create(options), "zero ESC interval rejected");
  auto decoder = *InputDecoder::create();
  std::string source = "\x1b[200~abc\x1b[201~";
  auto result = decoder.next(source, 0);
  source.assign(source.size(), 'x');
  decoder.reset();
  check(result && result->event && std::get<PasteInputEvent>(*result->event).bytes == "abc",
        "event owns payload independent of source and reset");
  check(!decoder.expect_replies({true, true, 0, 1, 1}), "empty probe window rejected");
  KeyEvent native{InputKey::character, U'é', {true, false, false, true}, KeyAction::release};
  check(native.action == KeyAction::release && native.modifiers.super,
        "event values can represent native backend data");
}
} // namespace

int main() {
  semantic_fixtures();
  mouse_and_probes();
  paste_and_limits();
  timing();
  partition_and_fuzz();
  options_and_ownership();
  std::cout << "Input partition/fuzz runs: " << partition_runs << "; failures: " << failures << '\n';

  return failures == 0 ? 0 : 1;
}
